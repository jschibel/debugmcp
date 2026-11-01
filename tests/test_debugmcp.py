"""End-to-end tests: drives bin/debugmcp.exe over MCP against the targets in tests/targets/bin.

Run all:     python tests/test_debugmcp.py
Run some:    python tests/test_debugmcp.py logpoint data_breakpoint

The server and the C++ test targets are (re)built automatically when missing or out of date.
"""
import os
import re
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_client import Client, ensure_server_built, ensure_targets_built  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TBIN = os.path.join(HERE, "targets", "bin")
BASIC = os.path.join(TBIN, "basic.exe")
BASIC_OPT = os.path.join(TBIN, "basic_opt.exe")
THREADS = os.path.join(TBIN, "threads.exe")
BASIC_SRC = os.path.join(HERE, "targets", "basic.cpp")
PLUGIN_SRC = os.path.join(HERE, "targets", "plugin.cpp")


def line_of(path, needle):
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f, 1):
            if needle in line:
                return i
    raise KeyError(needle)


L_MAIN_FIRST = line_of(BASIC_SRC, "LINE_MAIN_FIRST")
L_WATCH_WRITE = line_of(BASIC_SRC, "LINE_WATCH_WRITE")
L_LOOP_CALL = line_of(BASIC_SRC, "LINE_LOOP_CALL")
L_CRASH = line_of(BASIC_SRC, "LINE_CRASH")
L_FACT_BASE = line_of(BASIC_SRC, "LINE_FACTORIAL_BASE")
L_DIST_RET = line_of(BASIC_SRC, "LINE_DISTANCE_RETURN")
L_WAIT_LOOP = line_of(BASIC_SRC, "Sleep(100);")
L_PLUGIN = line_of(PLUGIN_SRC, "LINE_PLUGIN_HELPER")
L_UPDATE_FIRST = line_of(BASIC_SRC, "p.health -= tick;")


class Failure(Exception):
    pass


def check(cond, msg, ctx=None):
    if not cond:
        raise Failure(msg + ("\n--- context ---\n" + str(ctx) if ctx is not None else ""))


def loc(r):
    return r.get("location", {})


# ---------------------------------------------------------------- tests

def test_launch_stops_at_main(c):
    r = c.call("launch", {"program": BASIC})
    check(r["state"] == "stopped" and r["reason"] == "main", "expected stop at main", r)
    check(loc(r)["function"] == "basic!main", "wrong function", r)
    check(loc(r)["line"] == L_MAIN_FIRST, f"expected first statement line {L_MAIN_FIRST}", r)
    check("source" in loc(r) and ">" in loc(r)["source"], "missing source excerpt", r)
    l = c.call("locals")
    check("argc" in l["parameters"] and ": 1 " in l["parameters"], "argc should be 1", l)
    check("hero" in l["locals"], "locals should list hero", l)
    s = c.call("status")
    check(s["state"] == "stopped" and s["pid"], "status", s)


def test_stop_at_entry_then_continue_to_exit(c):
    r = c.call("launch", {"program": BASIC, "stop_at": "entry"})
    check(r["reason"] == "entry", "expected entry", r)
    check("ntdll" in loc(r).get("function", "").lower(), "entry should be in ntdll", r)
    r = c.call("continue")
    check(r["state"] == "exited" and r["exit_code"] == 0, "expected clean exit", r)
    check("factorial=120" in r.get("new_output", ""), "stdout should be captured", r)
    check("OutputDebugString" in r.get("new_output", ""), "OutputDebugString should be captured", r)
    o = c.call("output", {"tail": 50})
    check("tick 4 health" in o["text"], "output tool should return stdout", o)


def test_source_breakpoint_locals_evaluate(c):
    b = c.call("set_breakpoint", {"location": f"basic.cpp:{L_WATCH_WRITE}"})
    check(b["status"].startswith("pending"), "bp before launch should be pending", b)
    r = c.call("launch", {"program": BASIC, "stop_at": "none"})
    check(r["reason"] == "breakpoint" and r["breakpoint"]["id"] == b["id"], "bp should hit", r)
    check(loc(r)["line"] == L_WATCH_WRITE and loc(r)["function"] == "basic!UpdatePlayer", "location", r)
    e = c.call("evaluate", {"expression": "p.health"})
    check(": 100" in e["result"], "health should be 100 on first hit (tick 0)", e)
    e = c.call("evaluate", {"expression": "p.inventory", "depth": 2})
    check("[0]" in e["result"] and "size=0x3" in e["result"].replace(" ", "").replace("size=3", "size=0x3") or "[2]" in e["result"], "vector natvis", e)
    e = c.call("evaluate", {"expression": "p.target->name"})
    check("Villain" in e["result"], "pointer deref + std::string natvis", e)
    e = c.call("evaluate", {"expression": "p.position.x * 2"})
    check("result" in e, "arithmetic", e)
    # Parent frame locals.
    l = c.call("locals", {"frame": 1})
    check("tick" in l["locals"] and "scores" in l["locals"], "main locals via frame 1", l)
    lb = c.call("list_breakpoints")
    check(lb["breakpoints"][0]["hits"] == 1 and lb["breakpoints"][0]["status"] == "bound", "bp list", lb)


def test_step_into_over_out(c):
    c.call("launch", {"program": BASIC})
    r = c.call("run_to", {"location": f"basic.cpp:{L_LOOP_CALL}"})
    check(r["reason"] == "run_to" and loc(r)["line"] == L_LOOP_CALL, "run_to", r)
    r = c.call("step", {"kind": "into"})
    check(r["reason"] == "step" and "UpdatePlayer" in loc(r)["function"], "step into UpdatePlayer", r)
    r = c.call("step", {"kind": "over"})
    check(r["reason"] == "step" and "UpdatePlayer" in loc(r)["function"], "step over stays in fn", r)
    line1 = loc(r)["line"]
    r = c.call("step", {"kind": "over", "count": 2})
    check(loc(r)["line"] > line1, "step count advanced lines", r)
    r = c.call("step", {"kind": "out"})
    check(r["reason"] == "step" and loc(r)["function"].startswith("basic!main"), "step out to main", r)
    r = c.call("step", {"kind": "over_instruction"})
    check(r["reason"] == "step", "instruction step", r)
    # A frame picked with select_frame must not become the stepping scope: steps stay relative to
    # the top frame (a stale scope used to make source steps run free until the next breakpoint).
    r = c.call("run_to", {"location": f"basic.cpp:{L_LOOP_CALL}"})
    r = c.call("step", {"kind": "into"})
    check("UpdatePlayer" in loc(r)["function"], "into UpdatePlayer again", r)
    c.call("select_frame", {"frame": 1})
    r = c.call("step", {"kind": "over"})
    check(r["reason"] == "step" and "UpdatePlayer" in loc(r)["function"], "step after select_frame stays at top", r)
    d = c.call("disassemble", {"count": 2})
    check("=>" in d["disassembly"], "disassembly after a step starts at the IP", d)


def test_conditional_and_hitcount(c):
    c.call("set_breakpoint", {"location": f"basic.cpp:{L_WATCH_WRITE}", "condition": "tick == 3"})
    r = c.call("launch", {"program": BASIC, "stop_at": "none"})
    check(r["reason"] == "breakpoint", "cond bp hit", r)
    e = c.call("evaluate", {"expression": "tick"})
    check(": 3 " in e["result"], "condition tick==3", e)
    c.call("terminate")
    c.call("remove_breakpoint", {"all": True})
    c.call("set_breakpoint", {"location": "UpdatePlayer", "hit_condition": "3"})
    r = c.call("launch", {"program": BASIC, "stop_at": "none"})
    check(r["reason"] == "breakpoint" and r["breakpoint"]["hits"] == 3, "hit count 3", r)
    e = c.call("evaluate", {"expression": "tick"})
    check(": 2 " in e["result"], "3rd hit should have tick 2", e)
    # Conditional on std::string via data-model fallback.
    c.call("terminate")
    c.call("remove_breakpoint", {"all": True})
    c.call("set_breakpoint", {"location": "Distance2", "condition": "a.x > 8"})
    r = c.call("launch", {"program": BASIC, "stop_at": "none"})
    check(r["reason"] == "breakpoint", "float condition", r)


def test_logpoint(c):
    c.call("set_breakpoint", {"location": f"basic.cpp:{L_WATCH_WRITE}",
                              "log_message": "tick={tick} hp={p.health} name={p.name} in $FUNCTION hit $HITS"})
    r = c.call("launch", {"program": BASIC, "stop_at": "none"})
    check(r["state"] == "exited", "logpoints must not stop", r)
    lp = r.get("logpoints", "")
    lines = [l for l in lp.splitlines() if l.startswith("[bp")]
    check(len(lines) == 5, "5 logpoint lines expected", r)
    check("tick=4" in lines[-1] and "hp=90" in lines[-1] and '"Hero"' in lines[-1], "values interpolated", lines)
    check("UpdatePlayer" in lines[0] and "hit 1" in lines[0], "$FUNCTION/$HITS", lines)
    o = c.call("output", {"source": "logpoints"})
    check("tick=0" in o["text"], "logpoints via output tool", o)


def test_function_breakpoint_recursion(c):
    c.call("set_breakpoint", {"location": "basic!Factorial", "condition": "n == 1"})
    r = c.call("launch", {"program": BASIC, "stop_at": "none"})
    check(r["reason"] == "breakpoint", "Factorial bp", r)
    st = c.call("stack", {"max_frames": 10})
    n = sum(1 for f in st["frames"] if "Factorial" in f)
    check(n == 5, "5 recursive Factorial frames", st)
    e = c.call("evaluate", {"expression": "n", "frame": 4})
    check(": 5 " in e["result"], "n in frame 4 should be 5", e)


def test_data_breakpoint(c):
    c.call("launch", {"program": BASIC})
    b = c.call("set_breakpoint", {"location": "&g_watchme", "kind": "data", "data_size": 4})
    check(b["status"] == "bound", "data bp bound", b)
    r = c.call("continue")
    check(r["reason"] == "breakpoint" and "UpdatePlayer" in loc(r)["function"], "data bp hit in writer", r)
    check(loc(r)["line"] in (L_WATCH_WRITE, L_WATCH_WRITE + 1), "data bp line", r)
    r = c.call("continue")
    e = c.call("evaluate", {"expression": "g_watchme"})
    check(": 10 " in e["result"], "second write tick*10 == 10", e)


def test_access_violation(c):
    r = c.call("launch", {"program": BASIC, "args": ["crash"], "stop_at": "none"})
    check(r["reason"] == "exception", "AV expected", r)
    ex = r["exception"]
    check(ex["code"] == "0xc0000005" and ex["first_chance"], "AV code", ex)
    check("write" in ex.get("description", "") and "0x0" in ex["description"], "AV description", ex)
    check(loc(r)["line"] == L_CRASH and "Crash" in loc(r)["function"], "AV location", r)
    # Dump it and analyse post-mortem.
    dump = os.path.join(tempfile.gettempdir(), "debugmcp_test_crash.dmp")
    if os.path.exists(dump):
        os.remove(dump)
    w = c.call("write_dump", {"path": dump})
    check(os.path.exists(dump), "dump written", w)
    c.call("terminate")
    r = c.call("open_dump", {"path": dump})
    check(r["state"] == "stopped" and r["reason"] == "dump", "dump opened", r)
    check(r.get("exception", {}).get("code") == "0xc0000005", "dump exception", r)
    check("Crash" in loc(r).get("function", ""), "dump stack at Crash", r)
    e = c.call("evaluate", {"expression": "p", "frame": 0})
    check("0x0" in e["result"] or "00000000" in e["result"], "p is null in dump", e)
    c.call("detach")


def test_cpp_exception(c):
    s = c.call("exception_settings", {"exception": "cpp", "mode": "first"})
    r = c.call("launch", {"program": BASIC, "args": ["throw"], "stop_at": "none"})
    check(r["reason"] == "exception", "C++ throw should stop", r)
    ex = r["exception"]
    check(ex["code"] == "0xe06d7363", "C++ code", ex)
    check(ex.get("cpp_type") == "std::runtime_error", "decoded thrown type", ex)
    check(ex.get("what") == "something bad happened", "what() message", ex)
    check(any("ThrowIt" in f for f in r["stack"]), "thrown from ThrowIt", r)
    c.call("exception_settings", {"exception": "cpp", "mode": "second"})


def test_pending_breakpoint_in_late_loaded_dll(c):
    b = c.call("set_breakpoint", {"location": f"plugin.cpp:{L_PLUGIN}"})
    r = c.call("launch", {"program": BASIC, "args": ["dll"], "stop_at": "main"})
    lb = c.call("list_breakpoints")
    check(lb["breakpoints"][0]["status"].startswith("pending"), "pending before DLL load", lb)
    r = c.call("continue")
    check(r["reason"] == "breakpoint" and "Helper" in loc(r)["function"], "bp in DLL hit", r)
    e = c.call("evaluate", {"expression": "doubled"})
    check(": 14 " in e["result"], "doubled == 14", e)
    m = c.call("modules", {"filter": "plugin"})
    check(m["modules"] and m["modules"][0]["symbols"].startswith("pdb"), "plugin pdb loaded", m)
    r = c.call("continue")
    check(r["state"] == "exited" and "plugin=15" in r.get("new_output", ""), "plugin result", r)


def test_threads_pause_and_deadlock(c):
    r = c.call("launch", {"program": THREADS, "stop_at": "none", "timeout_ms": 1500})
    check(r["state"] == "running", "threads target keeps running", r)
    r = c.call("pause")
    check(r["state"] == "stopped" and r["reason"] == "pause", "pause", r)
    t = c.call("threads")
    names = {th.get("name") for th in t["threads"]}
    check({"Worker0", "Worker3", "DeadlockA", "DeadlockB"} <= names, "thread names", t)
    s = c.call("stack", {"all_threads": True, "max_frames": 30})
    check(s["unique_stacks"] < s["thread_count"], "workers should group", s)
    dl = [g for g in s["groups"] if any("Deadlock" in str(x) for x in g["threads"])]
    check(dl and any("mutex" in f.lower() or "lock" in f.lower() for g in dl for f in g["frames"]),
          "deadlock threads blocked in mutex lock", dl)
    ta = next(th for th in t["threads"] if th.get("name") == "DeadlockA")
    sel = c.call("select_thread", {"tid": ta["tid"]})
    st = c.call("stack", {"max_frames": 40})
    check(any("DeadlockA" in f for f in st["frames"]), "DeadlockA stack", st)
    r = c.call("continue", {"timeout_ms": 500})
    check(r["state"] == "running", "continue times out while running", r)
    r = c.call("wait", {"timeout_ms": 300})
    check(r["state"] == "running", "wait times out", r)
    r = c.call("pause")
    check(r["reason"] == "pause", "second pause", r)


def test_stepping_with_contending_threads(c):
    # Five threads run Shared()/Leaf() nonstop. Stepping one of them plants temporary breakpoints
    # the others keep hitting; traps queued after a breakpoint is removed must be resumed
    # transparently, and steps must stay on the stepping thread.
    c.call("launch", {"program": THREADS, "args": ["hot"]})
    for i in range(120):
        r = c.call("run_to", {"location": "Shared"})
        check(r["reason"] == "run_to", f"run_to Shared #{i}", r)
        tid = r["thread"]["tid"]
        for kind, fn in (("over", "Shared"), ("into", "Leaf"), ("out", "Shared"), ("over", "Shared")):
            r = c.call("step", {"kind": kind, "timeout_ms": 10000})
            check(r["state"] == "stopped" and r["reason"] == "step", f"step {kind} #{i} stays a step", r)
            check(r["thread"]["tid"] == tid, f"step {kind} #{i} stays on the stepping thread", r)
            check(fn in loc(r)["function"], f"step {kind} #{i} lands in {fn}", r)
    s = c.call("status")
    print(f"    stale traps resumed: {s.get('stale_breakpoint_traps_resumed', 0)}")


def test_breakpoint_while_running(c):
    r = c.call("launch", {"program": BASIC, "args": ["wait"], "stop_at": "none", "timeout_ms": 800})
    check(r["state"] == "running", "running", r)
    b = c.call("set_breakpoint", {"location": f"basic.cpp:{L_WAIT_LOOP}"})
    check(b["status"] == "bound", "bound while running", b)
    r = c.call("wait", {"timeout_ms": 5000})
    check(r["reason"] == "breakpoint" and loc(r)["line"] == L_WAIT_LOOP, "hit after set while running", r)
    c.call("enable_breakpoint", {"id": b["id"], "enabled": False})
    r = c.call("continue", {"timeout_ms": 700})
    check(r["state"] == "running", "disabled bp does not hit", r)
    c.call("enable_breakpoint", {"id": b["id"], "enabled": True})
    r = c.call("wait", {"timeout_ms": 5000})
    check(r["reason"] == "breakpoint", "re-enabled bp hits (enabled while running)", r)


def test_registers_memory_disasm_symbols(c):
    c.call("set_breakpoint", {"location": f"basic.cpp:{L_WATCH_WRITE}", "hit_condition": "2"})
    c.call("launch", {"program": BASIC, "stop_at": "none"})
    regs = c.call("registers")["registers"]
    check(regs["rip"].startswith("0x") and "rsp" in regs, "general regs", regs)
    v = c.call("registers", {"names": ["xmm0", "eax"]})["registers"]
    check("xmm0" in v and "f32" in v["xmm0"], "vector reg", v)
    m = c.call("read_memory", {"address": "&g_counter", "size": 4, "format": "u32"})
    check(re.search(r"0x0+2\b", m["data"]), "g_counter == 2 at 2nd hit (incremented on the line before)", m)
    m = c.call("read_memory", {"address": "@rsp", "size": 64, "format": "pointers"})
    check("basic!" in m["data"], "stack has return address symbols", m)
    m = c.call("read_memory", {"address": "p.name._Mypair._Myval2._Bx._Buf", "size": 4, "format": "ascii"})
    check(m["data"].startswith("Hero"), "string bytes", m)
    m = c.call("read_memory", {"address": "0", "size": 16})
    check(m.get("unreadable_bytes") == 16, "null page unreadable", m)
    d = c.call("disassemble", {"count": 8, "before": 4})
    check("=>" in d["disassembly"] and "UpdatePlayer" in d["disassembly"], "disasm marks ip", d)
    d = c.call("disassemble", {"address": "basic!Factorial", "function": True})
    check("ret" in d["disassembly"], "whole-function disasm", d)
    s = c.call("lookup_symbol", {"pattern": "basic!*Player*"})
    names = [x["name"] for x in s["matches"]]
    check(any("UpdatePlayer" in n for n in names), "symbol search", s)
    a = c.call("lookup_address", {"address": "basic!main+0x10"})
    check(a["symbol"].startswith("basic!main") and "basic.cpp" in a.get("at", ""), "address lookup", a)
    a = c.call("lookup_address", {"address": "@rsp"})
    check(a["region"]["type"] == "private", "stack region", a)
    t = c.call("type_info", {"type": "basic!Player"})
    check("health" in t["result"] and "+0x" in t["result"], "type layout", t)
    mods = c.call("modules", {"filter": "basic"})
    check(mods["modules"][0]["symbols"].startswith("pdb"), "basic pdb", mods)
    raw = c.call("command", {"command": "k 3"})
    check("UpdatePlayer" in raw["output"], "raw command", raw)
    src = c.call("source", {"context": 2})
    check(">" in src["text"], "source tool", src)


def test_modify_state(c):
    c.call("set_breakpoint", {"location": f"basic.cpp:{L_UPDATE_FIRST}", "one_shot": True})
    c.call("launch", {"program": BASIC, "stop_at": "none"})
    r = c.call("set_variable", {"expression": "p.health", "value": "999"})
    check(r["new_value"] == "999", "set var", r)
    w = c.call("write_memory", {"address": "&g_counter", "value": "41", "type": "u32"})
    e = c.call("evaluate", {"expression": "g_counter"})
    check(": 41 " in e["result"], "write_memory", e)
    c.call("watch", {"action": "add", "expression": "g_counter"})
    c.call("watch", {"action": "add", "expression": "p.health"})
    r = c.call("step", {"kind": "over", "count": 3})
    check(r["watches"]["g_counter"] == "42" and r["watches"]["p.health"] == "999", "watches + edits applied", r)
    # Set next statement back to the first line and re-run the decrement.
    r = c.call("set_instruction_pointer", {"location": f"basic.cpp:{L_UPDATE_FIRST}"})
    check(r["line"] == L_UPDATE_FIRST, "set ip", r)
    r = c.call("step", {"kind": "over"})
    check(r["watches"]["p.health"] == "999", "tick 0 decrement keeps 999", r)
    c.call("watch", {"action": "clear"})
    reg = c.call("set_register", {"name": "rax", "value": "0x1234"})
    check(reg["value"].endswith("1234"), "set register", reg)
    lb = c.call("list_breakpoints")
    check(lb["breakpoints"] == [], "one-shot removed", lb)


def test_attach_detach(c):
    p = subprocess.Popen([BASIC, "wait"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.7)
        lp = c.call("list_processes", {"filter": "basic"})
        check(any(x["pid"] == p.pid for x in lp["processes"]), "list_processes", lp)
        r = c.call("attach", {"pid": p.pid})
        check(r["state"] == "stopped" and r["reason"] == "attach", "attach", r)
        st = c.call("stack", {"max_frames": 30})
        check(any("main" in f for f in st["frames"]), "main thread selected", st)
        c.call("set_breakpoint", {"location": f"basic.cpp:{L_WAIT_LOOP}"})
        r = c.call("continue")
        check(r["reason"] == "breakpoint", "bp after attach", r)
        d = c.call("detach")
        check(d["detached"], "detach", d)
        time.sleep(0.3)
        check(p.poll() is None, "process still alive after detach")
        c.call("remove_breakpoint", {"all": True})
        r = c.call("attach", {"process_name": "basic.exe", "break": False})
        check(r["state"] == "running", "attach without break", r)
        r = c.call("pause")
        check(r["reason"] == "pause", "pause after attach", r)
        c.call("detach")
    finally:
        p.kill()


def test_restart_and_terminate(c):
    c.call("set_breakpoint", {"location": "basic!Distance2"})
    r = c.call("launch", {"program": BASIC, "stop_at": "none"})
    check(r["reason"] == "breakpoint", "first run", r)
    r = c.call("restart")
    check(r["reason"] == "breakpoint" and "Distance2" in loc(r)["function"], "restart keeps bps", r)
    t = c.call("terminate")
    check(t["state"] == "no_session", "terminate", t)
    s = c.call("status")
    check(s["state"] in ("no_session", "exited"), "status after terminate", s)
    e = c.call("locals", expect_error=True)
    check("no active debug session" in e, "error without session", e)


def test_optimized_build_inline_frames(c):
    c.call("set_breakpoint", {"location": f"basic.cpp:{L_DIST_RET}"})
    r = c.call("launch", {"program": BASIC_OPT, "stop_at": "none"})
    check(r["state"] == "stopped", "opt build stops at inlined line", r)
    st = c.call("stack", {"max_frames": 10, "verbose": True})
    check(isinstance(st["frames"], list) and st["frames"], "verbose stack", st)
    lb = c.call("list_breakpoints")
    check(lb["breakpoints"][0]["status"] == "bound", "bound in optimized build", lb)
    # Stepping out of / over inline frames must stay in this invocation (DbgEng's native inline
    # step-out used to run free to the next execution of the same code).
    check(st["frames"][0].get("inline") and not st["frames"][1].get("inline"), "inlined Distance2 frame flagged", st)
    r = c.call("step", {"kind": "out"})
    check(r["reason"] == "step" and "Distance2" not in loc(r).get("function", ""), "step out of inline frame", r)
    for i in range(3):
        r = c.call("step", {"kind": "over"})
        check(r["reason"] == "step", f"inline-aware step over #{i}", r)


def test_concurrent_pause_and_logpoint_storm(c):
    # A logpoint in a hot loop keeps DbgEng busy inside WaitForEvent; timeouts and pause must
    # still respond promptly, and pause must be able to interrupt a blocked continue.
    c.call("set_breakpoint", {"location": f"basic.cpp:{L_WAIT_LOOP}", "log_message": "i={i}"})
    r = c.call("launch", {"program": BASIC, "args": ["wait"], "stop_at": "none", "timeout_ms": 1000})
    check(r["state"] == "running", "running with logpoints", r)
    start = time.time()
    rid = c.call_async("wait", {"timeout_ms": 60000})
    time.sleep(1.0)
    p = c.call("pause", {"timeout_ms": 5000})
    check(p["state"] == "stopped" and p["reason"] == "pause", "pause during blocked wait", p)
    w = c.result_of(rid)
    check(w["state"] == "stopped", "blocked wait returns on pause", w)
    check(time.time() - start < 10, "pause latency")
    r = c.call("continue", {"timeout_ms": 1500})
    check(r["state"] == "running", "timeout honoured during logpoint storm", r)
    check("i=" in r.get("logpoints", ""), "logpoint lines while running", r)
    p = c.call("pause")
    e = c.call("evaluate", {"expression": "definitely_not_a_variable"}, expect_error=True)
    check("cannot evaluate" in e and "***" not in e, "clean evaluate error", e)


def test_mcp_protocol(c):
    tools = c.request("tools/list")["tools"]
    names = {t["name"] for t in tools}
    for n in ["launch", "attach", "continue", "step", "set_breakpoint", "evaluate", "registers",
              "read_memory", "disassemble", "lookup_symbol", "stack", "threads", "command"]:
        check(n in names, f"tool {n} listed", names)
    for t in tools:
        check(t["inputSchema"]["type"] == "object", "schema", t)
    check(c.request("ping") == {}, "ping")
    e = c.call("nonexistent_tool", expect_error=True)
    check("unknown tool" in e, "unknown tool error", e)


TESTS = [v for k, v in list(globals().items()) if k.startswith("test_")]


def main():
    ensure_server_built()
    ensure_targets_built()
    want = sys.argv[1:]
    tests = [t for t in TESTS if not want or any(w in t.__name__ for w in want)]
    failed = []
    for t in tests:
        c = Client()
        start = time.time()
        try:
            t(c)
            print(f"PASS {t.__name__} ({time.time() - start:.1f}s)", flush=True)
        except Exception as e:
            failed.append(t.__name__)
            print(f"FAIL {t.__name__} ({time.time() - start:.1f}s)\n{e}", flush=True)
            if not isinstance(e, Failure):
                traceback.print_exc()
            if c.stderr_lines:
                print("  stderr:", "\n  ".join(c.stderr_lines[-10:]))
        finally:
            c.close()
    print(f"\n{len(tests) - len(failed)}/{len(tests)} passed")
    if failed:
        print("failed:", ", ".join(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
