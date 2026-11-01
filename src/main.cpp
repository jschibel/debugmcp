// debugmcp: an MCP (Model Context Protocol) server exposing a native Windows C++ debugger
// (Microsoft DbgEng, the engine behind WinDbg/cdb) over stdio JSON-RPC.
#include <windows.h>
#include <io.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "engine.h"
#include "util.h"

#ifndef DEBUGMCP_VERSION
#define DEBUGMCP_VERSION "0.1.0"
#endif

namespace {

HANDLE g_in = INVALID_HANDLE_VALUE;   // MCP input (private dup of original stdin)
HANDLE g_out = INVALID_HANDLE_VALUE;  // MCP output (private dup of original stdout)
std::mutex g_out_m;
LineLog g_debuggee_out(50000);
Debugger* g_dbg = nullptr;

void Send(const json& msg) {
    std::string s = msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    std::lock_guard<std::mutex> lk(g_out_m);
    DWORD w = 0;
    const char* p = s.data();
    size_t left = s.size();
    while (left) {
        if (!WriteFile(g_out, p, (DWORD)std::min<size_t>(left, 1 << 20), &w, nullptr)) return;
        p += w;
        left -= w;
    }
}

// Redirect the process-level std handles so a launched debuggee (which inherits handles)
// writes into a pipe we capture, never into the MCP channel.
void SetupStdio() {
    HANDLE self = GetCurrentProcess();
    DuplicateHandle(self, GetStdHandle(STD_INPUT_HANDLE), self, &g_in, 0, FALSE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(self, GetStdHandle(STD_OUTPUT_HANDLE), self, &g_out, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HANDLE diag = INVALID_HANDLE_VALUE;
    DuplicateHandle(self, GetStdHandle(STD_ERROR_HANDLE), self, &diag, 0, FALSE, DUPLICATE_SAME_ACCESS);
    SetDiagHandle(diag);
    // Make sure the originals are not inherited by the debuggee.
    for (DWORD id : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = GetStdHandle(id);
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    }

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        SetStdHandle(STD_OUTPUT_HANDLE, wr);
        SetStdHandle(STD_ERROR_HANDLE, wr);
        std::thread([rd] {
            char buf[8192];
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) g_debuggee_out.Append(std::string(buf, n));
        }).detach();
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul != INVALID_HANDLE_VALUE) SetStdHandle(STD_INPUT_HANDLE, nul);
    // Keep CRT-level stdout/stderr (if anything uses them) away from the MCP channel too.
    int fd = _open_osfhandle((intptr_t)wr, 0);
    if (fd >= 0) {
        _dup2(fd, 1);
        _dup2(fd, 2);
    }
}

// ---------------- tool registry ----------------

struct Tool {
    std::string name;
    std::string description;
    json schema;
    std::function<json(const json&)> fn;
    bool read_only = false;
};

json Obj(std::initializer_list<std::pair<const std::string, json>> props, std::vector<std::string> required = {}) {
    json s = {{"type", "object"}, {"properties", json(props)}};
    if (!required.empty()) s["required"] = required;
    return s;
}
json Str(const std::string& d) { return {{"type", "string"}, {"description", d}}; }
json Int(const std::string& d) { return {{"type", "integer"}, {"description", d}}; }
json Bool(const std::string& d) { return {{"type", "boolean"}, {"description", d}}; }
json Enum(const std::string& d, std::vector<std::string> vals) {
    return {{"type", "string"}, {"description", d}, {"enum", vals}};
}
json StrArr(const std::string& d) { return {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", d}}; }

const char* kStopReportDoc =
    " Returns a stop report: state (stopped|running|exited|no_session), reason (breakpoint|step|exception|entry|main|pause|run_to|...), "
    "thread, location (function, file, line, source excerpt), short stack, watch values, new debuggee output and logpoint lines.";

std::vector<Tool> BuildTools(Debugger& d) {
    using std::placeholders::_1;
    auto B = [&](json (Debugger::*m)(const json&)) { return [&d, m](const json& a) { return (d.*m)(a); }; };
    json tid = Int("Thread id (system TID as shown by threads). Defaults to the current thread.");
    json frame = Int("Stack frame number (from stack). Defaults to the current frame.");
    json timeout = Int("How long to wait for the target to stop, in ms (default 30000). On timeout returns state=running; call wait or pause.");
    json frames = Int("Number of stack frames to include in the stop report (default 8).");
    json max_chars = Int("Truncate the textual result to this many characters.");
    std::vector<Tool> t = {
        {"launch",
         "Start a program under the debugger (like F5/F11 in Visual Studio). By default runs to the first line of main/wmain/WinMain "
         "(stop_at=main); stop_at=entry stops at the loader breakpoint before CRT init; stop_at=none runs freely until a breakpoint/exception. "
         "Breakpoints you set before launch are applied automatically. Debuggee stdout/stderr and OutputDebugString are captured (see output)." +
             std::string(kStopReportDoc),
         Obj({{"program", Str("Path to the .exe")},
              {"args", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Command line arguments (array; each is quoted). A single string is passed verbatim."}}},
              {"cwd", Str("Working directory (default: the program's directory)")},
              {"env", {{"type", "object"}, {"description", "Environment overrides merged into the current environment (null value removes a variable)"}}},
              {"stop_at", Enum("Where to stop first (default main)", {"main", "entry", "none"})},
              {"new_console", Bool("Give the program its own console window (output then is NOT captured). Default false.")},
              {"symbol_path", Str("Symbol path (default: program dir + _NT_SYMBOL_PATH or the Microsoft symbol server with a local cache)")},
              {"source_path", Str("Source search path, for remapping sources built on another machine")},
              {"natvis", StrArr("Extra .natvis files to load (e.g. Unreal's Engine/Extras/VisualStudioDebugging/Unreal.natvis)")},
              {"timeout_ms", timeout},
              {"frames", frames}},
             {"program"}),
         B(&Debugger::Launch)},
        {"attach",
         "Attach to a running process by pid or exe name and break in (break=false keeps it running)." + std::string(kStopReportDoc),
         Obj({{"pid", Int("Process id")},
              {"process_name", Str("Executable name, e.g. UnrealEditor.exe (must be unique)")},
              {"break", Bool("Break in immediately (default true)")},
              {"symbol_path", Str("Symbol path override")},
              {"source_path", Str("Source search path")},
              {"natvis", StrArr("Extra .natvis files")},
              {"timeout_ms", timeout},
              {"frames", frames}}),
         B(&Debugger::Attach)},
        {"open_dump",
         "Open a crash dump (.dmp) for post-mortem debugging. Context is set to the exception record if there is one.",
         Obj({{"path", Str("Path to the .dmp file")},
              {"symbol_path", Str("Symbol path override")},
              {"source_path", Str("Source search path")},
              {"natvis", StrArr("Extra .natvis files")},
              {"frames", frames}},
             {"path"}),
         B(&Debugger::OpenDump)},
        {"detach", "Detach from the target and leave it running.", Obj({}), B(&Debugger::Detach)},
        {"terminate", "Kill the debuggee and end the session (Shift+F5).", Obj({}), B(&Debugger::Terminate)},
        {"restart", "Terminate and relaunch with the previous launch arguments (optionally overriding some). Breakpoints are kept.",
         Obj({{"stop_at", Enum("Where to stop first", {"main", "entry", "none"})},
              {"args", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Override arguments"}}},
              {"timeout_ms", timeout}}),
         B(&Debugger::Restart)},
        {"status", "Current session state; if stopped, the full stop report (location, stack, watches, new output).",
         Obj({{"frames", frames}}), B(&Debugger::Status), true},
        {"continue", "Resume execution (F5) and wait until the next stop (breakpoint, exception, exit) or timeout." + std::string(kStopReportDoc),
         Obj({{"timeout_ms", timeout}, {"frames", frames}}), B(&Debugger::Continue)},
        {"pause", "Break into a running target (Break All). The main thread is selected.", Obj({{"timeout_ms", timeout}, {"frames", frames}}),
         B(&Debugger::Pause)},
        {"wait", "Keep waiting for a running target to stop (after a continue/step timed out).", Obj({{"timeout_ms", timeout}, {"frames", frames}}),
         B(&Debugger::Wait)},
        {"step",
         "Step the current thread: over (F10, source line), into (F11), out (Shift+F11), or over_instruction / into_instruction for machine-level stepping. "
         "count repeats the step; stops early if something else (breakpoint/exception) happens.",
         Obj({{"kind", Enum("Step kind (default over)", {"over", "into", "out", "over_instruction", "into_instruction"})},
              {"count", Int("Number of steps (default 1)")},
              {"timeout_ms", timeout},
              {"frames", frames}}),
         B(&Debugger::Step)},
        {"run_to", "Run to a location (Run To Cursor) using a temporary breakpoint. Location formats as in set_breakpoint.",
         Obj({{"location", Str("file.cpp:line, function name, or address")}, {"timeout_ms", timeout}, {"frames", frames}}, {"location"}),
         B(&Debugger::RunTo)},
        {"set_breakpoint",
         "Set a breakpoint. Works before launch, while stopped, or while running. Locations: 'file.cpp:123' (path optional, binds to every "
         "instantiation), 'Function', 'Class::Method', 'module!Function', or an address '0x7ff6...'. Unresolved locations stay pending and bind "
         "when the module loads (e.g. DLLs/plugins). Supports conditions (C++ expression), hit counts, one-shot, and LOGPOINTS: set log_message to "
         "log instead of stopping -- this replaces adding printf/UE_LOG: '{expr}' is evaluated (natvis-aware), plus $FUNCTION $CALLER $CALLSTACK $TID $HITS. "
         "kind=data sets a hardware data breakpoint (watchpoint) on an address expression, e.g. location='&g_counter' or '&obj->field'.",
         Obj({{"location", Str("Where to break (see description). For kind=data: an address expression.")},
              {"condition", Str("Break only when this C++ expression is non-zero, e.g. 'i == 42 && ptr != 0'")},
              {"hit_condition", Str("Hit count filter: 'N' or '==N' (Nth hit), '>=N', '%N' (every Nth)")},
              {"log_message", Str("Logpoint message, e.g. 'tick={tick} hp={p.health} at $FUNCTION'. Does not stop unless log_and_break.")},
              {"log_and_break", Bool("For logpoints: also stop (default false)")},
              {"one_shot", Bool("Delete after first hit")},
              {"enabled", Bool("Default true")},
              {"kind", Enum("code (default) or data (hardware watchpoint, max 4)", {"code", "data"})},
              {"data_size", Int("Data breakpoint size: 1, 2, 4 or 8 (default 4)")},
              {"data_access", Enum("Data breakpoint trigger (default write)", {"write", "read", "readwrite", "execute"})}},
             {"location"}),
         B(&Debugger::SetBreakpoint)},
        {"remove_breakpoint", "Delete a breakpoint by id, or all=true.", Obj({{"id", Int("Breakpoint id")}, {"all", Bool("Remove all")}}),
         B(&Debugger::RemoveBreakpoint)},
        {"enable_breakpoint", "Enable or disable a breakpoint.", Obj({{"id", Int("Breakpoint id")}, {"enabled", Bool("true to enable")}}, {"id", "enabled"}),
         B(&Debugger::EnableBreakpoint)},
        {"list_breakpoints", "List breakpoints with their bound addresses/source lines, hit counts and status (bound/pending/error).", Obj({}),
         B(&Debugger::ListBreakpoints), true},
        {"threads", "List threads with names (SetThreadDescription), the top frame and the first non-system 'user' frame.",
         Obj({{"locations", Bool("Include per-thread locations (default true; set false for very many threads)")}}), B(&Debugger::Threads), true},
        {"select_thread", "Make a thread current for subsequent stack/locals/evaluate/step.", Obj({{"tid", tid}}, {"tid"}), B(&Debugger::SelectThread)},
        {"stack",
         "Call stack of the current (or given) thread, including inline frames. all_threads=true groups every thread by identical stacks "
         "(like Parallel Stacks) -- ideal for spotting deadlocks and hangs.",
         Obj({{"tid", tid},
              {"max_frames", Int("Frames per thread (default 64)")},
              {"start_frame", Int("First frame to show (paging)")},
              {"all_threads", Bool("Show all threads grouped by unique stack")},
              {"verbose", Bool("Return per-frame objects with addresses, SP and return address")},
              {"select", Bool("With tid: also make that thread current")}}),
         B(&Debugger::Stack), true},
        {"select_frame", "Select a stack frame (sets scope for locals/evaluate/registers) and show its source.", Obj({{"frame", frame}, {"tid", tid}}, {"frame"}),
         B(&Debugger::SelectFrame)},
        {"locals", "Parameters and local variables of a frame (natvis-formatted, like the Locals window).",
         Obj({{"frame", frame}, {"tid", tid}, {"depth", Int("Expansion depth for structs/containers (default 1)")}, {"max_chars", max_chars}}),
         B(&Debugger::Locals), true},
        {"evaluate",
         "Evaluate a C++ expression in the current frame (Watch/Immediate window) via the debugger data model: members, pointers, casts, "
         "array/index, natvis visualizers for STL/Unreal types, format specifiers (',x' hex, ',[N]' to view N elements of a pointer, ',s', ',d'), "
         "and LINQ-style queries (e.g. 'vec.Where(x => x > 3)'). Falls back to the classic C++ (??) and MASM evaluators.",
         Obj({{"expression", Str("Expression, e.g. 'player->inventory', '*this', 'arr,[16]', '(MyType*)0x1234', 'sizeof(Foo)'")},
              {"depth", Int("Expansion depth for children (default 1)")},
              {"frame", frame},
              {"tid", tid},
              {"max_chars", max_chars}},
             {"expression"}),
         B(&Debugger::Evaluate), true},
        {"set_variable", "Assign a new value to a variable/field/register-backed local (Edit Value).",
         Obj({{"expression", Str("Lvalue, e.g. 'p.health' or 'g_counter'")}, {"value", Str("New value (C++ expression)")}, {"frame", frame}, {"tid", tid}},
             {"expression", "value"}),
         B(&Debugger::SetVariable)},
        {"watch", "Manage watch expressions; their values are included in every stop report.",
         Obj({{"action", Enum("add/remove/clear/list (default list)", {"add", "remove", "clear", "list"})}, {"expression", Str("Watch expression")}}),
         B(&Debugger::Watch)},
        {"registers", "Read CPU registers for the current thread/frame (frame>0 shows the unwound context).",
         Obj({{"names", StrArr("Specific registers, e.g. [\"rax\",\"xmm0\",\"rip\"]")},
              {"set", Enum("Register set when names not given (default general)", {"general", "vector", "all"})},
              {"frame", frame},
              {"tid", tid}}),
         B(&Debugger::Registers), true},
        {"set_register", "Set a register value (e.g. rax, rip, rcx).", Obj({{"name", Str("Register")}, {"value", Str("Value expression")}, {"tid", tid}}, {"name", "value"}),
         B(&Debugger::SetRegister)},
        {"read_memory",
         "Read target memory. Address may be any expression (0x..., @rsp, &var, module!symbol, ptr->field). format: hex (dump with ASCII), "
         "u8/u16/u32/u64, i8..i64, f32, f64, ascii, utf16, pointers (each qword annotated with its symbol -- great for raw stacks/vtables).",
         Obj({{"address", Str("Address expression")},
              {"size", Int("Bytes to read (default 128, max 1 MiB)")},
              {"format", Enum("Output format (default hex)", {"hex", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "ascii", "utf16", "pointers"})},
              {"frame", frame},
              {"max_chars", max_chars}},
             {"address"}),
         B(&Debugger::ReadMemory), true},
        {"write_memory", "Write target memory from hex bytes or a typed value.",
         Obj({{"address", Str("Address expression")},
              {"hex", Str("Bytes as hex, e.g. '90 90 cc'")},
              {"value", Str("Typed value (use with type)")},
              {"type", Enum("Type for value (default u32)", {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32", "f64", "ascii", "utf16"})}},
             {"address"}),
         B(&Debugger::WriteMemory)},
        {"disassemble", "Disassemble around the current instruction (default) or an address/symbol/file:line, interleaved with source lines. function=true disassembles the whole function.",
         Obj({{"address", Str("Address, symbol or file:line (default: current frame's instruction)")},
              {"count", Int("Instructions to show (default 24)")},
              {"before", Int("Also show N instructions before the address")},
              {"function", Bool("Disassemble the whole containing function")},
              {"frame", frame},
              {"tid", tid},
              {"max_chars", max_chars}}),
         B(&Debugger::Disassemble), true},
        {"lookup_symbol", "Find symbols (functions, globals) by pattern with wildcards and return addresses and source locations. Prefer module-qualified patterns on large apps.",
         Obj({{"pattern", Str("e.g. 'MyGame!*Player*Tick*', 'basic!g_*', 'kernel32!CreateFileW'")}, {"limit", Int("Max results (default 50)")}}, {"pattern"}),
         B(&Debugger::LookupSymbol), true},
        {"lookup_address", "Resolve an address to symbol+offset, module, source line and memory region info (heap/stack/image).",
         Obj({{"address", Str("Address expression")}}, {"address"}), B(&Debugger::LookupAddress), true},
        {"modules", "List loaded modules with base/size and symbol status (pdb, deferred, export-only).",
         Obj({{"filter", Str("Substring filter on module/image name")}, {"limit", Int("Max rows (default 300)")}}), B(&Debugger::Modules), true},
        {"load_symbols", "Force-load symbols for a module ('*' reloads all), optionally extending the symbol path. Shows the resulting symbol status.",
         Obj({{"module", Str("Module name, e.g. 'UnrealEditor-Engine' or '*'")}, {"symbol_path", Str("Path to add")}, {"append", Bool("Append (default) vs replace")}}, {"module"}),
         B(&Debugger::LoadSymbols)},
        {"type_info", "Show a type's layout (fields, offsets, sizes) like 'dt'; optionally overlay it on an address. Supports wildcard to list matching types.",
         Obj({{"type", Str("Type name, e.g. 'basic!Player' or 'MyGame!FMyStruct'")}, {"address", Str("Optional address to interpret as this type")}, {"depth", Int("Recurse into nested structs")}, {"max_chars", max_chars}},
             {"type"}),
         B(&Debugger::TypeInfo), true},
        {"exception_settings",
         "View or change when exceptions stop the debugger (Exception Settings). Default: access violations break on first chance, C++ exceptions only when unhandled. "
         "Set exception='cpp' mode='first' to break where a C++ exception is thrown.",
         Obj({{"exception", Str("av, cpp/eh, sov, dz, asrt, or hex code like 0xC0000005")}, {"mode", Enum("Behavior", {"first", "second", "output", "ignore"})}}),
         B(&Debugger::ExceptionSettings)},
        {"output", "Read captured output: debuggee (stdout/stderr + OutputDebugString), logpoints (tracepoint messages), or engine (debugger messages, symbol load errors).",
         Obj({{"source", Enum("Which stream (default debuggee)", {"debuggee", "logpoints", "engine"})},
              {"since", Int("Return lines after this cursor (use next_since from a previous call)")},
              {"tail", Int("Max lines (default 200)")},
              {"max_chars", max_chars}}),
         B(&Debugger::Output), true},
        {"command",
         "Run a raw WinDbg/cdb command for anything not covered by other tools (e.g. '!heap -p -a <addr>', 'lm', '!locks', 'dx -g ...', '.frame /r 2', "
         "'!analyze -v', 'x mod!*', 'bl'). Execution commands (g, p, t, gu) are tracked and wait for the next stop.",
         Obj({{"command", Str("Debugger command")}, {"timeout_ms", timeout}, {"max_chars", max_chars}}, {"command"}),
         B(&Debugger::Command)},
        {"set_instruction_pointer", "Set Next Statement: move the instruction pointer to a file:line/address in the current function (force=true to allow leaving it).",
         Obj({{"location", Str("file.cpp:line or address")}, {"force", Bool("Allow jumping outside the current function")}}, {"location"}),
         B(&Debugger::SetInstructionPointer)},
        {"write_dump", "Write a minidump of the target (full memory by default) for later analysis.",
         Obj({{"path", Str("Output .dmp path")}, {"full", Bool("Full memory dump (default true)")}}, {"path"}), B(&Debugger::WriteDump)},
        {"load_natvis", "Load a .natvis visualizer file (applies now and to future sessions).", Obj({{"path", Str(".natvis path")}}, {"path"}),
         B(&Debugger::LoadNatvis)},
        {"source", "Show source around the current frame's line, or around file:line.",
         Obj({{"file", Str("Source file (default: current frame)")}, {"line", Int("Line number")}, {"context", Int("Lines of context (default 15)")}, {"frame", frame}, {"tid", tid}}),
         B(&Debugger::Source), true},
        {"list_processes", "List running processes (pid, name, parent) to pick an attach target.", Obj({{"filter", Str("Substring filter")}}),
         B(&Debugger::ListProcesses), true},
    };
    return t;
}

std::map<std::string, Tool> g_tools;
std::vector<std::string> g_tool_order;

json ToolsList() {
    json arr = json::array();
    for (auto& name : g_tool_order) {
        auto& t = g_tools[name];
        json j = {{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}};
        if (t.read_only) j["annotations"] = {{"readOnlyHint", true}};
        arr.push_back(j);
    }
    return arr;
}

json CallTool(const json& params) {
    std::string name = params.value("name", "");
    json a = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
    auto it = g_tools.find(name);
    if (it == g_tools.end()) {
        return {{"content", {{{"type", "text"}, {"text", "unknown tool: " + name}}}}, {"isError", true}};
    }
    try {
        json r = it->second.fn(a);
        std::string text = r.is_string() ? r.get<std::string>() : r.dump(1, ' ', false, json::error_handler_t::replace);
        return {{"content", {{{"type", "text"}, {"text", text}}}}};
    } catch (const std::exception& e) {
        return {{"content", {{{"type", "text"}, {"text", std::string("error: ") + e.what()}}}}, {"isError", true}};
    }
}

void Handle(const json& msg) {
    if (!msg.is_object()) return;
    std::string method = msg.value("method", "");
    bool is_request = msg.contains("id");
    json id = is_request ? msg["id"] : json();
    auto reply = [&](const json& result) {
        if (is_request) Send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    };
    auto error = [&](int code, const std::string& m) {
        if (is_request) Send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", m}}}});
    };
    if (method == "initialize") {
        std::string ver = "2025-06-18";
        if (msg.contains("params") && msg["params"].contains("protocolVersion"))
            ver = msg["params"]["protocolVersion"].get<std::string>();
        reply({{"protocolVersion", ver},
               {"capabilities", {{"tools", {{"listChanged", false}}}}},
               {"serverInfo", {{"name", "debugmcp"}, {"version", DEBUGMCP_VERSION}}},
               {"instructions",
                "Native C++ debugger for Windows/MSVC (DbgEng). Typical loop: set_breakpoint (or logpoints with log_message instead of adding prints) -> "
                "launch (stops at main) -> continue -> inspect with locals/evaluate/stack -> step/continue. Every run-control tool returns a stop "
                "report with location, source, stack and new output, so you rarely need status. Use stack all_threads=true for hangs/deadlocks, "
                "exception_settings cpp first to catch throws, kind=data breakpoints to find who writes a variable. Engine: " +
                    (g_dbg ? g_dbg->EngineInfo() : std::string())}});
    } else if (method == "tools/list") {
        reply({{"tools", ToolsList()}});
    } else if (method == "tools/call") {
        reply(CallTool(msg.value("params", json::object())));
    } else if (method == "ping") {
        reply(json::object());
    } else if (method == "resources/list") {
        reply({{"resources", json::array()}});
    } else if (method == "prompts/list") {
        reply({{"prompts", json::array()}});
    } else if (method.rfind("notifications/", 0) == 0) {
        // ignore
    } else {
        error(-32601, "method not found: " + method);
    }
}

}  // namespace

int main(int argc, char** argv) {
    SetupStdio();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    Debugger dbg(&g_debuggee_out);
    std::string err;
    if (!dbg.Start(&err)) {
        DiagLog("failed to start debugger engine: %s", err.c_str());
        // Still serve MCP so the client sees a useful error.
    }
    g_dbg = &dbg;
    for (auto& t : BuildTools(dbg)) {
        g_tool_order.push_back(t.name);
        g_tools[t.name] = t;
    }
    if (!err.empty()) {
        for (auto& [name, t] : g_tools)
            t.fn = [err](const json&) -> json { throw ToolError("debugger engine unavailable: " + err); };
    }
    if (argc > 1 && std::string(argv[1]) == "--self-test") {
        DiagLog("%s", dbg.EngineInfo().c_str());
        return err.empty() ? 0 : 1;
    }

    std::atomic<int> inflight{0};
    std::string buf;
    char chunk[65536];
    DWORD n = 0;
    while (ReadFile(g_in, chunk, sizeof(chunk), &n, nullptr) && n) {
        buf.append(chunk, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (Trim(line).empty()) continue;
            json msg;
            try {
                msg = json::parse(line);
            } catch (const std::exception& e) {
                Send({{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", std::string("parse error: ") + e.what()}}}});
                continue;
            }
            // Requests run concurrently so that e.g. pause can interrupt a long continue/wait.
            ++inflight;
            std::thread([msg, &inflight] {
                Handle(msg);
                --inflight;
            }).detach();
        }
    }
    // stdin closed: client went away. Give in-flight requests a moment, then clean up the target.
    for (int i = 0; i < 50 && inflight > 0; ++i) Sleep(20);
    dbg.Shutdown();
    return 0;
}
