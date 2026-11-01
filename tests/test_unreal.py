"""Optional large-scale integration test against a local Unreal Engine build (with PDBs).

    set DEBUGMCP_UE_ROOT=D:\\UE\\Astrid        (directory containing Engine\\)
    python tests/test_unreal.py

Launches UnrealEditor-Cmd.exe with a nonexistent commandlet (engine init, then exit; no project
files are modified) and checks: stop at WinMain, module-qualified/hyphenated breakpoints, source
breakpoints by full path, natvis (FString) + native FName decoding in logpoints, pause latency,
and thread naming at scale.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_client import Client  # noqa: E402

ROOT = os.environ.get("DEBUGMCP_UE_ROOT", r"D:\UE\Astrid")
EXE = os.path.join(ROOT, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe")
ARGS = ["-run=DebugMcpNoSuchCommandlet", "-nullrhi", "-unattended", "-nosplash", "-nopause"]


def check(cond, msg, ctx=None):
    if not cond:
        raise AssertionError(msg + ("\n" + str(ctx)[:3000] if ctx is not None else ""))


def timed(c, tool, args=None, limit=None):
    t = time.time()
    r = c.call(tool, args or {}, timeout=900)
    dt = time.time() - t
    print(f"  {tool:16s} {dt:6.2f}s")
    if limit:
        check(dt < limit, f"{tool} took {dt:.1f}s (limit {limit}s)")
    return r


def main():
    if not os.path.exists(EXE):
        print(f"SKIP: {EXE} not found (set DEBUGMCP_UE_ROOT)")
        return
    c = Client()
    try:
        r = timed(c, "launch", {"program": EXE, "args": ARGS, "frames": 1}, limit=30)
        check(r["reason"] == "main" and "WinMain" in r["location"]["function"], "stop at WinMain", r)

        b = timed(c, "set_breakpoint", {"location": "UnrealEditor-Core!FFileHelper::LoadFileToString"}, limit=30)
        check(b["status"] == "bound" and len(b["locations"]) >= 2, "overloads bound", b)
        r = timed(c, "continue", {"frames": 3}, limit=60)
        check(r["reason"] == "breakpoint", "LoadFileToString hit", r)
        loc = c.call("locals", {"depth": 1})
        check("Filename" in loc.get("parameters", "") or "Reader" in loc.get("parameters", ""), "params", loc)
        c.call("remove_breakpoint", {"all": True})

        src = os.path.join(ROOT, "Engine", "Source", "Runtime", "Core", "Private", "Modules", "ModuleManager.cpp")
        b = timed(c, "set_breakpoint", {"location": "UnrealEditor-Core!FModuleManager::LoadModuleWithFailureReason",
                                        "log_message": "load {InModuleName}"}, limit=30)
        r = timed(c, "continue", {"timeout_ms": 3000, "frames": 1}, limit=10)
        lp = r.get("logpoints", "")
        named = [l for l in lp.splitlines() if l.count('"') >= 2]
        check(len(named) >= 3, "FName values decoded in logpoints", lp[:2000])
        p = timed(c, "pause", {}, limit=10)
        check(p["reason"] == "pause", "pause", p)
        c.call("remove_breakpoint", {"all": True})
        if os.path.exists(src):
            b = timed(c, "set_breakpoint", {"location": src + ":927"}, limit=60)
            check(b["status"] == "bound", "full-path source breakpoint", b)
        t = timed(c, "threads", {}, limit=60)
        named = [x for x in t["threads"] if x.get("name")]
        check(t["count"] >= 10 and len(named) >= 5, "many named threads", t)
        s = timed(c, "stack", {"all_threads": True, "max_frames": 16}, limit=60)
        check(s["unique_stacks"] <= s["thread_count"], "grouped stacks", s)
        timed(c, "terminate", {}, limit=30)
        print("PASS test_unreal")
    finally:
        c.close()


if __name__ == "__main__":
    main()
