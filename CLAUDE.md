# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`debugmcp` is a native C++ MCP server (stdio, newline-delimited JSON-RPC) that exposes Microsoft
**DbgEng** (the WinDbg/cdb engine) as debugger tools for MSVC C++ targets, with Unreal Engine as
the primary large-scale target. It deliberately does *not* use vsdbg, because vsdbg's license
restricts it to VS/VS Code; don't reintroduce vsdbg or its handshake. See README.md for the user
docs and tool list.

## Commands

```bat
build.cmd                     :: build bin\debugmcp.exe (Release /MT); "build.cmd debug" for /Od /Zi.
                              :: First build also runs scripts\setup-runtime.ps1 (DbgEng -> runtime\dbgeng)
tests\targets\build.cmd       :: build the C++ test targets into tests\targets\bin
python tests\test_debugmcp.py                 :: full end-to-end suite (~20 s); rebuilds server/targets if stale
python tests\test_debugmcp.py logpoint data   :: run tests whose names contain these substrings
python tests\test_unreal.py                   :: optional UE integration; skipped unless DEBUGMCP_UE_ROOT is set
python tests\mcp_client.py <tool> '<json>' [<tool> '<json>' ...]   :: ad-hoc calls against bin\debugmcp.exe
```

Both build scripts find Visual Studio via `scripts\vcvars.cmd` (vswhere). From Git Bash, run them
from the repo root as `cmd //c build.cmd`. A bare `cmd.exe /c build.cmd` hangs because MSYS
converts `/c` to a path. Nothing in the repo may hard-code machine paths: per-user data goes
through `DataDir()` (`%LOCALAPPDATA%\debugmcp`, else temp), and `.mcp.json` uses a repo-relative
path. CI (`.github/workflows/ci.yml`) builds and runs the suite on a clean Windows runner. Tests locate source lines through `LINE_*` marker comments in
`tests/targets/basic.cpp`, so keep those markers when editing targets. The server writes
diagnostics to its real stderr, prefixed `[debugmcp]`; `Client.stderr_lines` collects them.

## Architecture

- `src/main.cpp`: MCP protocol, the tool table (names, descriptions, JSON schemas → `Debugger`
  methods), and stdio plumbing. At startup it duplicates the MCP stdin/stdout to private handles
  and points the process std handles at a pipe. A launched debuggee inherits that pipe, which is
  how its stdout is captured without corrupting the MCP channel. **Never write to
  stdout/printf** in the server. Each request runs on its own thread, so `pause` can interrupt a
  blocked `continue`/`wait`.
- `src/engine.h/.cpp`: the `Debugger` core. **All DbgEng calls happen on one engine thread.**
  Public methods marshal work there via `Invoke()`. While the target runs, the engine thread loops
  `WaitForEvent(50ms)` and drains the task queue in between. Stops bump `stop_seq_` under
  `state_m_`; callers block in `WaitForStop()` on `stop_cv_` and then build a stop report.
  Anything that must work while DbgEng is busy inside `WaitForEvent` (e.g. a logpoint storm keeps
  it there) must not need the engine thread: `Pause` (DebugBreakProcess via `BreakIn()`) and
  timed-out `RunningReport()` run on the caller thread.
  Event callbacks (`OnBreakpointEvent`, `OnLoadModuleEvent`, ...) run on the engine thread inside
  `WaitForEvent`. Conditions, hit counts and logpoints are implemented there by returning
  `DEBUG_STATUS_IGNORE_EVENT`.
  `WithTargetStopped()` transparently breaks in, applies a change, and resumes. That's how
  breakpoints can be edited while running.
- `src/engine_ops.cpp`: one method per tool, plus breakpoint binding. Breakpoint binding is the
  subtle part, because DbgEng's own deferred breakpoints don't re-resolve for already-loaded
  modules whose symbols are deferred:
  - `file:line` → `BindSourceLine`: `GetOffsetByLine` to learn the PDB's full path, then
    `GetSourceEntriesByLine` for all instances, then inline sites of the containing function.
    Unbound lines stay pending; `OnLoadModuleEvent` retries them per newly loaded module.
  - Function names → `FindFunctionAddresses`: a fast `StartSymbolMatch` pass that keeps only
    addresses with line info (this drops import thunks), then `x /v` for inline sites in matching
    modules only. Non-inline addresses get `SkipPrologue`.
  - Symbol loading is lazy. `LoadUserModuleSymbols(path)` loads only modules whose name matches a
    source-path directory before falling back to all non-system PDBs. Keep it that way:
    Unreal has ~800 PDBs (Engine.pdb is 575 MB).
- Stepping (`Step` in engine_ops.cpp; `StepPos`/`StepCtl`/`StepContinue` in engine.cpp): DbgEng's
  own source steps and `gu` are unreliable in inline frames. Their return breakpoints aren't
  checked against thread or stack depth, so a step can run free into a later, unrelated call of
  the same code. So:
  - From an inline frame (or code without lines), the step is driven instruction by
    instruction. `StepContinue` runs in `OnWaitReturned` after each instruction step and compares
    (CFA of the physical frame, inline depth, function, line) to the start until done.
  - Step out of a physical frame is a guard breakpoint at the return address, matched to the
    thread and to `rsp >= CFA` (checked in `OnBreakpointEvent`).
  - Native steps from physical frames get the same guard, so they can't outrun their function.
  - `ResetScopeToTop()` runs at every stop and before every step.
  Inline frames are detected with `IsInlineFrame()`: FrameType is the *second* byte of
  `InlineFrameContext`. `DEBUGMCP_TRACE_STEPS=1` logs every controller step to stderr.
- Stale breakpoint traps: other threads hit a step's temporary int3s. A trap still queued when
  the breakpoint is removed arrives as a break-instruction exception at an address that no
  longer holds 0xCC. `ResumeAfterStaleTrap` (called from `OnWaitReturned`, after DbgEng has taken
  breakpoints out of memory; during the event callback they still read 0xCC) resumes the
  interrupted step or continue. UE hits this constantly. `test_stepping_with_contending_threads`
  covers it.
- Values: `evaluate`/`locals`/logpoints use `dx` (data model, natvis) via `Exec()` (captured
  output), with fallbacks to `??` and MASM. `ShortValue()` produces one-line values; it decodes
  Unreal `FName` natively (`DecodeFName`) because DbgEng can't run UE's FName natvis.
  `EnsureNatvisLoaded()` runs at every stop, since DbgEng drops visualizers loaded at the initial
  breakpoint. `PatchNatvis()` writes a copy of Unreal.natvis with engine globals module-qualified.
- DbgEng naming quirks the code compensates for: module names replace `-` with `_`
  (`NormalizeModuleQualifiers`/`NormalizeModuleName`). `.nvload` needs absolute backslash paths
  (`FullPath`). `SYMOPT_NO_UNQUALIFIED_LOADS` stays on on purpose: unqualified lookups scan
  every PDB.
- `src/args.h` holds argument helpers; `src/util.*` holds string utilities and `LineLog`, a
  thread-safe ring buffer with sequence cursors used for debuggee output, logpoints and engine
  messages.

When adding a tool: implement `json Debugger::X(const json&)` (validate args on the caller
thread, do DbgEng work inside `Invoke`), declare it in `engine.h`, register it in `BuildTools()`
in `main.cpp` with a schema, and add an end-to-end scenario to `tests/test_debugmcp.py`.
