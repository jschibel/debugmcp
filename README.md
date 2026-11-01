# debugmcp

An MCP server that gives an AI agent a real native debugger for Windows C++ (MSVC/PDB): launch
under the debugger and stop at `main`, attach, open crash dumps, set breakpoints (source, function,
conditional, hit-count, data/hardware), use **logpoints instead of printf/UE_LOG**, step, inspect
locals/expressions with natvis, read registers and memory, disassemble, and resolve symbols. It is
built for large codebases such as Unreal Engine.

## Engine choice: DbgEng, not vsdbg

VS Code's C++ debugging (`cppvsdbg`) speaks the Debug Adapter Protocol to **vsdbg**, which ships in
the C/C++ extension. Its license only permits use *with Visual Studio / VS Code* and forbids working
around technical limitations (vsdbg authenticates its client with a VS Code-only handshake). A
standalone MCP server driving vsdbg would violate those terms, so debugmcp uses **DbgEng**, the
engine behind WinDbg/cdb. DbgEng is Microsoft's documented, supported debugging API and has full
MSVC/PDB, natvis, symbol-server and minidump support. The MCP tools follow VS concepts and naming
(Continue, Step Over/Into/Out, Run To Cursor, Set Next Statement, Break All, Exception Settings,
Parallel Stacks, ...).

## Setup

Requirements: Windows x64, Visual Studio 2022+ with the C++ toolset, WinDbg (installed from winget
by the setup script if missing), Python 3 for the tests.

```bat
powershell -ExecutionPolicy Bypass -File scripts\setup-runtime.ps1   :: copy DbgEng from WinDbg into runtime\dbgeng
build.cmd                                                             :: -> bin\debugmcp.exe   (build.cmd debug for /Od)
```

Re-run `setup-runtime.ps1` after updating WinDbg. The server looks for `dbgeng.dll` in
`%DEBUGMCP_DBGENG_DIR%`, `<exe>\dbgeng`, then `<exe>\..\runtime\dbgeng`. It falls back to
System32's copy, which is reduced: no natvis Visualizers and no symsrv.

### Register with Claude Code

This repo's `.mcp.json` registers it for sessions opened here. To use it from any project (e.g.
your game or engine tree):

```bat
claude mcp add --scope user debugmcp -- D:\debugmcp\bin\debugmcp.exe
```

Environment knobs: `DEBUGMCP_SYMBOL_PATH` (else `_NT_SYMBOL_PATH`, else the program dir plus the
Microsoft symbol server cached in `%LOCALAPPDATA%\debugmcp\symcache`), `DEBUGMCP_DBGENG_DIR`.

## Tools

| Area | Tools |
| --- | --- |
| Session | `launch` (stop_at main/entry/none, args, cwd, env, natvis), `attach` (pid/name), `open_dump`, `detach`, `terminate`, `restart`, `status`, `list_processes` |
| Run control | `continue`, `pause`, `wait`, `step` (over/into/out/instruction), `run_to`, `set_instruction_pointer` |
| Breakpoints | `set_breakpoint` (file:line, function, address, condition, hit_condition, log_message, one_shot, kind=data), `remove_breakpoint`, `enable_breakpoint`, `list_breakpoints`, `exception_settings` |
| Inspection | `stack` (incl. all_threads grouped like Parallel Stacks), `threads`, `select_thread`, `select_frame`, `locals`, `evaluate`, `set_variable`, `watch`, `source` |
| Machine level | `registers`, `set_register`, `read_memory` (hex/typed/pointers), `write_memory`, `disassemble` |
| Symbols | `lookup_symbol`, `lookup_address`, `modules`, `load_symbols`, `type_info`, `load_natvis` |
| Misc | `output` (debuggee stdout/stderr + OutputDebugString, logpoints, engine log), `write_dump`, `command` (raw WinDbg command escape hatch) |

Every run-control tool blocks until the target stops (or `timeout_ms`). It returns a **stop
report**: reason, thread, function/file/line with a source excerpt, a short stack, watch values,
new program output and logpoint lines. A timed-out call returns `state: running`; then call
`wait` or `pause`. Requests run concurrently, so `pause` can interrupt a blocked `continue`.

### Breakpoint locations

* `file.cpp:123`, `D:\full\path\file.cpp:123`, or `Module!file.cpp:123`. Binds to every code
  location for the line (templates, multiple modules), moving to the next line with code if
  needed. If the file isn't loaded yet, the breakpoint stays *pending* and binds when a DLL
  containing the file loads.
* `Function`, `Class::Method`, `Module!Class::Method`, wildcards (`MyGame!*Tick*`). Binds every
  overload and inlined copy, past the prologue so parameters are valid. Module names may use
  hyphens (`UnrealEditor-Core!...` is mapped to DbgEng's `UnrealEditor_Core`).
* `0x7ff6...` addresses; for `kind=data`, any address expression (`&g_x`, `&obj->field`).

### Stepping

`step` follows VS semantics in optimized code too. Step into enters inlined functions, step over
skips calls and sibling inline calls on the same line, and step out leaves the current inline
frame or function. Steps always act on the top frame of the stepping thread, whatever
`select_frame` picked. They never run past the function they started in and never stop in
another invocation of the same code (recursion, other threads). Hitting a breakpoint on another
thread interrupts a step, as in VS. In a big multithreaded process, other threads constantly
run into the temporary breakpoints a step plants; their late traps are resumed transparently
(`status` reports `stale_breakpoint_traps_resumed`).

**Logpoints:** `log_message: "hp={p.health} name={p.name} from $CALLER"` evaluates `{expr}` with
natvis at every hit and doesn't stop the program. Tokens: `$FUNCTION $CALLER $CALLSTACK $TID
$HITS`.

### Unreal Engine notes

* `Engine\Extras\VisualStudioDebugging\Unreal.natvis` is found and loaded automatically for
  executables inside a UE tree. Its unqualified engine globals (`GNameBlocksDebug`, ...) are
  rewritten module-qualified in a cached copy, because DbgEng won't resolve unqualified globals
  without scanning every PDB.
* DbgEng can't evaluate UE's `FName` visualizer expressions, so `FName` values are decoded natively
  in `evaluate` (`value` field), logpoints and watches. `FString`/`TArray` use natvis.
* For speed on huge PDB sets, prefer module-qualified locations (`UnrealEditor-Engine!AActor::Tick`)
  or full source paths. Paths are matched to modules by directory name
  (`...\Runtime\Core\...` → `UnrealEditor-Core`). An unqualified function name loads every
  non-system PDB once per session (~15 s for a 300-module editor; VS pays this cost at startup).
* Measured on a 286-module UnrealEditor-Cmd session: launch to WinMain 0.3 s; module-qualified
  breakpoint 0.2 s; full-path source breakpoint 0.2-0.4 s; `threads` (57 threads) 0.6-5 s;
  `pause` 0.04 s.

## Limitations

* x64 user-mode targets only. Child processes aren't followed (attach to them separately).
* Breakpoints on *source lines inside inlined functions* in optimized builds bind to the
  out-of-line copy and to the start of each inlined copy. That's approximate: DbgEng exposes no
  inline line tables for breakpoints. Function breakpoints on inlined functions are exact.
* Not supported: Edit and Continue, Just My Code stepping, or function evaluation that calls into
  the target.
* Conditions use the C++ expression evaluator first, then fall back to the data model. Hot-path
  conditional breakpoints and logpoints slow the target, as they do in VS.

## Tests

```bat
tests\targets\build.cmd               :: build test programs (Debug + /O2)
python tests\test_debugmcp.py         :: 21 end-to-end scenarios over MCP (or pass name filters)
python tests\test_unreal.py           :: optional; needs DEBUGMCP_UE_ROOT pointing at a UE build with PDBs
python tests\mcp_client.py launch "{\"program\":\"...\"}" locals "{}"   :: ad-hoc tool calls
```

## License

debugmcp is released into the public domain under [the Unlicense](LICENSE).

Exception: `third_party/json.hpp` ([nlohmann/json](https://github.com/nlohmann/json)) remains
under its own MIT license, stated in the file header. DbgEng is not part of this repository.
`scripts\setup-runtime.ps1` copies it from your own WinDbg installation, under Microsoft's terms.
