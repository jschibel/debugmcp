#pragma once
// DbgEng-backed debugger session manager.
//
// Threading model: DbgEng interfaces are owned by a single dedicated engine thread.
// Every public method marshals work onto that thread via Invoke(). While the target is
// running the engine thread sits in WaitForEvent() with a short timeout and services the
// task queue in between, so state queries never block on a running target.
// Pause (SetInterrupt) is the only call made from a foreign thread, which DbgEng allows.

#include <windows.h>
#include <dbgeng.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "util.h"

// INLINE_FRAME_CONTEXT is {BYTE FrameId; BYTE FrameType; WORD FrameSignature}; FrameType may carry
// STACK_FRAME_TYPE_RA (0x80) on top of STACK_FRAME_TYPE_INLINE (0x02).
inline bool IsInlineFrame(const DEBUG_STACK_FRAME_EX& f) {
    return ((f.InlineFrameContext >> 8) & 0x7F) == 0x02;
}
// Frame i's IP is a return address (look up its line at IP-1) iff a physical frame lies below it.
inline bool IsReturnAddressFrame(const std::vector<DEBUG_STACK_FRAME_EX>& st, size_t i) {
    for (size_t j = 0; j < i && j < st.size(); ++j)
        if (!IsInlineFrame(st[j])) return true;
    return false;
}

struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct BreakpointSpec {
    int id = 0;
    std::string location;       // user-facing location string
    std::string kind = "code";  // code | data
    std::string condition;      // C++ expression; break only when non-zero
    std::string hit_condition;  // "N", "==N", ">=N", "%N"
    std::string log_message;    // logpoint/tracepoint message; {expr} interpolation
    bool log_and_break = false; // logpoint that also breaks
    bool one_shot = false;
    bool enabled = true;
    int data_size = 4;
    std::string data_access = "write";  // write | read | readwrite | execute
    bool internal = false;              // run_to / stop-at-main temporaries
    std::string internal_tag;
    // Parsed location (set by ClassifyLocation).
    std::string src_file;     // for file:line locations
    ULONG src_line = 0;
    std::string module_hint;  // "mod" from "mod!file.cpp:N"
    bool is_function = false; // symbolic function location (gets prologue relocation)
    // Per-session runtime state.
    bool relocated = false;
    ULONG engine_id = DEBUG_ANY_ID;
    uint64_t hits = 0;
    std::string last_error;
};

struct StopInfo {
    std::string reason;  // breakpoint | step | exception | entry | main | pause | attach |
                         // run_to | dump | exited | session_ended | ...
    int breakpoint_id = 0;
    std::string breakpoint_location;
    json exception;      // null unless reason == exception
    uint32_t exit_code = 0;
    std::string note;
};

class Debugger {
public:
    explicit Debugger(LineLog* debuggee_out);
    ~Debugger();

    // Loads DbgEng and starts the engine thread.
    bool Start(std::string* error);
    // Terminates/detaches any session and stops the engine thread.
    void Shutdown();

    std::string EngineInfo() const { return engine_info_; }

    // Tool entry points. All return JSON or throw ToolError.
    json Launch(const json& a);
    json Attach(const json& a);
    json OpenDump(const json& a);
    json Detach(const json& a);
    json Terminate(const json& a);
    json Restart(const json& a);
    json Status(const json& a);
    json Continue(const json& a);
    json Pause(const json& a);
    json Wait(const json& a);
    json Step(const json& a);
    json RunTo(const json& a);
    json SetBreakpoint(const json& a);
    json RemoveBreakpoint(const json& a);
    json ListBreakpoints(const json& a);
    json EnableBreakpoint(const json& a);
    json Threads(const json& a);
    json SelectThread(const json& a);
    json Stack(const json& a);
    json SelectFrame(const json& a);
    json Locals(const json& a);
    json Evaluate(const json& a);
    json SetVariable(const json& a);
    json Watch(const json& a);
    json Registers(const json& a);
    json SetRegister(const json& a);
    json ReadMemory(const json& a);
    json WriteMemory(const json& a);
    json Disassemble(const json& a);
    json LookupSymbol(const json& a);
    json LookupAddress(const json& a);
    json Modules(const json& a);
    json LoadSymbols(const json& a);
    json TypeInfo(const json& a);
    json ExceptionSettings(const json& a);
    json Output(const json& a);
    json Command(const json& a);
    json SetInstructionPointer(const json& a);
    json WriteDump(const json& a);
    json LoadNatvis(const json& a);
    json Source(const json& a);
    json ListProcesses(const json& a);

private:
    // ---- engine thread plumbing ----
    void EngineThreadMain();
    json Invoke(std::function<json()> fn);
    void DrainTasks();

    // ---- state helpers (engine thread) ----
    void RequireSession();
    void RequireStopped();
    void RequireLive();
    void ResumeTarget(ULONG status, const char* kind);   // GO / STEP_*
    void OnWaitReturned(HRESULT hr);
    void EndSessionState(const std::string& reason);
    void BeginSession(bool launched, bool dump);
    void ApplyAllBreakpoints();
    bool BindSourceLine(BreakpointSpec& b, bool allow_symbol_load);
    void LoadUserModuleSymbols(const std::string& source_path_hint = "");
    std::vector<std::pair<std::string, ULONG>> UserModules();
    std::vector<uint64_t> FindFunctionAddresses(const std::string& location);
    uint64_t SkipPrologue(uint64_t fn_start);
    void BreakIn();
    ULONG OnLoadModuleEvent(const std::wstring& module_name, const std::wstring& image_name);
    bool ApplyBreakpoint(BreakpointSpec& b);
    void RemoveEngineBreakpoint(BreakpointSpec& b);
    json BreakpointJson(BreakpointSpec& b);
    void CleanupTempBreakpoints();
    json WithTargetStopped(const std::function<json()>& fn);  // break in transparently if running

    // Waits (on the caller thread) for the stop sequence to change, then builds a report.
    json WaitForStop(uint64_t seq_before, int timeout_ms, int report_frames);
    json BuildStopReport(int frames);  // engine thread
    json RunningReport();              // engine thread or caller thread (uses atomics)
    void AttachNewOutput(json& j);

    // ---- DbgEng helpers (engine thread) ----
    std::string Exec(const std::string& cmd, HRESULT* hr = nullptr);
    std::string ExecChecked(const std::string& cmd);
    uint64_t EvalAddress(const std::string& expr);
    bool TryEvalNumber(const std::string& expr, bool cplusplus, uint64_t* out, std::string* err);
    uint64_t ResolveLocation(const std::string& loc);  // file:line or expression
    std::string ToOffsetExpression(const std::string& loc);
    std::string ShortValue(const std::string& expr);   // one-line value via dx
    std::string FormatLogMessage(const std::string& msg, BreakpointSpec& b);
    json FrameJson(const DEBUG_STACK_FRAME_EX& f, bool use_return_adjust, bool with_source_text);
    std::vector<DEBUG_STACK_FRAME_EX> CurrentStack(ULONG max_frames);
    std::string SymbolAt(uint64_t addr, ULONG inline_ctx, uint64_t* disp = nullptr);
    bool LineAt(uint64_t addr, ULONG inline_ctx, std::string* file, ULONG* line);
    std::string SourceSnippet(const std::string& file, ULONG line, int context);
    std::string ResolveSourceFile(const std::string& file);
    std::string ModuleNameAt(uint64_t addr);
    std::string ThreadName(ULONG sys_tid);
    void SelectThreadSys(ULONG sys_tid);
    void SelectFrameNum(ULONG frame);
    void ResetScopeToTop();
    void ApplyThreadFrameArgs(const json& a);
    json DecodeException(const EXCEPTION_RECORD64& rec, bool first_chance);
    std::string ExeModuleName();
    void SetSymbolAndSourcePaths(const json& a);
    void LoadNatvisList(const json& a);
    void EnsureNatvisLoaded();
    std::string PatchNatvis(const std::string& path);
    std::string DecodeFName(const std::string& obj);
    std::string ShortValueRaw(const std::string& expr);

    friend class EventCallbacks;
    friend class OutputCallbacks;
    ULONG OnBreakpointEvent(IDebugBreakpoint2* bp);
    ULONG OnExceptionEvent(EXCEPTION_RECORD64* rec, ULONG first_chance);
    ULONG OnExitProcessEvent(ULONG exit_code);
    void OnOutput(ULONG mask, const wchar_t* text);

    LineLog* debuggee_out_;
    LineLog engine_log_{5000};
    LineLog trace_log_{20000};  // logpoint output
    std::string engine_info_;

    // DbgEng interfaces (engine thread only).
    HMODULE dbgeng_ = nullptr;
    IDebugClient7* client_ = nullptr;
    IDebugControl7* control_ = nullptr;
    IDebugSymbols5* symbols_ = nullptr;
    IDebugRegisters2* registers_ = nullptr;
    IDebugDataSpaces4* data_ = nullptr;
    IDebugSystemObjects4* sysobj_ = nullptr;
    class EventCallbacks* event_cb_ = nullptr;
    class OutputCallbacks* output_cb_ = nullptr;

    // Engine thread & task queue.
    std::thread thread_;
    std::mutex task_m_;
    std::condition_variable task_cv_;
    std::deque<std::function<void()>> tasks_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> ready_{false};
    std::string start_error_;
    std::mutex start_m_;
    std::condition_variable start_cv_;

    // Session state (written on engine thread; read under state_m_ from others).
    std::mutex state_m_;
    std::condition_variable stop_cv_;
    uint64_t stop_seq_ = 0;
    std::atomic<bool> session_{false};
    std::atomic<bool> running_{false};
    bool is_dump_ = false;
    bool launched_ = false;
    bool suppress_stop_ = false;  // internal break-in in progress
    std::string resume_kind_;     // what caused the last resume: continue|step|pause|...
    StopInfo last_stop_;
    std::atomic<uint32_t> pid_{0};
    std::string target_desc_;
    json last_launch_args_;
    std::string exe_module_;

    // Source-level stepping (engine thread). DbgEng's own source steps and `gu` misbehave in
    // inline frames: the return breakpoints they plant aren't checked against thread or stack
    // depth, so a step can run free and stop in an unrelated later call of the same code. Steps
    // from inline frames are therefore driven here instruction by instruction, steps out of
    // physical frames use a stack-checked return breakpoint, and native steps get that same
    // breakpoint as a guard so they can never run past their own function's return.
    struct StepPos {
        bool valid = false;
        int depth = 0;           // inline frames above the first physical frame
        ULONG64 cfa = 0;         // physical frame's CFA (caller's SP): stable for the call
        ULONG64 ret = 0;         // physical frame's return address
        std::string func, file;
        ULONG line = 0;
        bool has_line = false;
    };
    struct StepCtl {
        bool active = false;
        bool custom = false;     // instruction-driven (else: native step or guard-only)
        bool guard_out = false;  // running to the guard to leave a function without source
        std::string kind;        // into | over | out
        ULONG tid = 0;           // engine thread id
        StepPos start;
        uint64_t instrs = 0;
        int noline_run = 0;
    };
    StepCtl step_;
    uint64_t stale_traps_ = 0;  // stale int3 traps resumed transparently (OnExceptionEvent)
    ULONG guard_eid_ = DEBUG_ANY_ID;
    ULONG64 guard_cfa_ = 0;
    StepPos CurPos();
    void SetGuard(ULONG64 ret, ULONG64 cfa, ULONG tid);
    void ClearGuard();
    bool StepContinue(std::string* note);  // true: re-issued, keep running
    void StepIssue(ULONG status);
    bool ResumeAfterStaleTrap(ULONG64 addr);

    // Pending event details captured by callbacks.
    int pending_bp_hit_ = 0;
    std::string pending_bp_note_;
    bool pending_exit_ = false;
    uint32_t pending_exit_code_ = 0;

    // Capture state for Exec().
    int capture_depth_ = 0;
    std::string capture_;

    // Breakpoints (engine thread).
    std::map<int, BreakpointSpec> bps_;
    std::unordered_map<ULONG, int> engine_to_bp_;
    int next_bp_id_ = 1;
    std::vector<std::string> watches_;
    std::vector<std::string> natvis_files_;
    std::set<uint64_t> inline_sites_;
    std::map<std::string, std::string> patched_natvis_;
    std::string fname_module_;

    // Output cursors for "new output since last report".
    std::mutex cursor_m_;
    std::atomic<bool> pause_requested_{false};
    bool natvis_checked_ = false;
    uint64_t out_cursor_ = 0;
    uint64_t trace_cursor_ = 0;

    std::map<std::string, std::vector<std::string>> source_cache_;
};
