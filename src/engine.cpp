#include "engine.h"
#include "args.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>

#ifndef STACK_FRAME_TYPE_INLINE
#define STACK_FRAME_TYPE_INLINE 0x02
#endif

using namespace args;

namespace {

constexpr int kDefaultTimeoutMs = 30000;
constexpr int kMaxTimeoutMs = 600000;
constexpr ULONG kStackLimit = 1024;

int ClampTimeout(int t) {
    if (t < 0) return 0;
    return std::min(t, kMaxTimeoutMs);
}

const char* ExceptionName(uint32_t code) {
    switch (code) {
        case 0xC0000005: return "Access violation";
        case 0xC000001D: return "Illegal instruction";
        case 0xC0000094: return "Integer divide by zero";
        case 0xC0000095: return "Integer overflow";
        case 0xC00000FD: return "Stack overflow";
        case 0xC0000096: return "Privileged instruction";
        case 0xC0000006: return "In-page I/O error";
        case 0xC000008E: return "Float divide by zero";
        case 0xC0000409: return "Stack buffer overrun / fail-fast (__fastfail)";
        case 0xC0000374: return "Heap corruption";
        case 0xC0000602: return "Unknown/fail-fast exception";
        case 0xE06D7363: return "C++ exception";
        case 0x80000003: return "Breakpoint (int 3 / __debugbreak)";
        case 0x80000004: return "Single step";
        case 0x40010006: return "OutputDebugString";
        case 0x406D1388: return "SetThreadName";
        case 0xC0000420: return "Assertion failure";
        default: return "Exception";
    }
}

// ".?AVruntime_error@std@@" -> "std::runtime_error" (best effort; falls back to raw).
std::string DemangleTypeName(const std::string& raw) {
    std::string s = raw;
    if (s.rfind(".?A", 0) != 0 || s.size() < 5) return raw;
    char kind = s[3];  // V=class U=struct W4=enum
    size_t start = 4;
    if (kind == 'W') start = 5;
    std::string body = s.substr(start);
    if (body.size() >= 2 && body.compare(body.size() - 2, 2, "@@") == 0)
        body = body.substr(0, body.size() - 2);
    if (body.find('?') != std::string::npos || body.find('$') != std::string::npos)
        return raw;  // templates etc. -- keep raw rather than guessing
    std::vector<std::string> parts;
    std::string cur;
    for (char c : body) {
        if (c == '@') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += "::";
        out += *it;
    }
    return out.empty() ? raw : out;
}

bool LooksLikeSourceLine(const std::string& loc, std::string* file, ULONG* line) {
    static const std::regex re(R"(^\s*(.+?\.(c|cc|cpp|cxx|c\+\+|h|hh|hpp|hxx|inl|ipp|ixx|cppm|tpp|asm))\s*[:@](\d+)\s*$)",
                               std::regex::icase);
    std::smatch m;
    if (!std::regex_match(loc, m, re)) return false;
    *file = m[1].str();
    *line = (ULONG)std::stoul(m[3].str());
    return true;
}

bool LooksLikeAddress(const std::string& loc) {
    static const std::regex re(R"(^\s*(0x)?[0-9a-fA-F`]{6,}\s*$)");
    return std::regex_match(loc, re) && (loc.find("0x") != std::string::npos ||
                                          std::any_of(loc.begin(), loc.end(), ::isdigit));
}

}  // namespace

// ======================= Callbacks =======================

class EventCallbacks : public DebugBaseEventCallbacksWide {
public:
    explicit EventCallbacks(Debugger* d) : d_(d) {}
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(GetInterestMask)(PULONG mask) override {
        *mask = DEBUG_EVENT_BREAKPOINT | DEBUG_EVENT_EXCEPTION | DEBUG_EVENT_EXIT_PROCESS |
                DEBUG_EVENT_LOAD_MODULE;
        return S_OK;
    }
    STDMETHOD(Breakpoint)(PDEBUG_BREAKPOINT2 bp) override { return d_->OnBreakpointEvent(bp); }
    STDMETHOD(Exception)(PEXCEPTION_RECORD64 rec, ULONG first) override {
        return d_->OnExceptionEvent(rec, first);
    }
    STDMETHOD(ExitProcess)(ULONG code) override { return d_->OnExitProcessEvent(code); }
    STDMETHOD(LoadModule)(ULONG64, ULONG64, ULONG, PCWSTR module_name, PCWSTR image_name, ULONG,
                          ULONG) override {
        return d_->OnLoadModuleEvent(module_name ? module_name : L"", image_name ? image_name : L"");
    }

private:
    Debugger* d_;
};

class OutputCallbacks : public IDebugOutputCallbacksWide {
public:
    explicit OutputCallbacks(Debugger* d) : d_(d) {}
    STDMETHOD(QueryInterface)(REFIID iid, PVOID* out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDebugOutputCallbacksWide)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(Output)(ULONG mask, PCWSTR text) override {
        d_->OnOutput(mask, text);
        return S_OK;
    }

private:
    Debugger* d_;
};

// ======================= Lifetime =======================

Debugger::Debugger(LineLog* debuggee_out) : debuggee_out_(debuggee_out) {}

Debugger::~Debugger() { Shutdown(); }

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    return p.substr(0, p.find_last_of(L"\\/"));
}

static HMODULE LoadDbgEng(std::string* where) {
    std::vector<std::wstring> dirs;
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"DEBUGMCP_DBGENG_DIR", env, MAX_PATH)) dirs.push_back(env);
    std::wstring exe = ExeDir();
    dirs.push_back(exe + L"\\dbgeng");
    dirs.push_back(exe + L"\\..\\runtime\\dbgeng");
    dirs.push_back(exe + L"\\..\\..\\runtime\\dbgeng");
    for (auto& d : dirs) {
        std::wstring path = d + L"\\dbgeng.dll";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        // Make dbghelp/dbgmodel/symsrv resolve from the same directory.
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h) {
            wchar_t full[MAX_PATH];
            GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
            *where = Narrow(full);
            return h;
        }
    }
    HMODULE h = LoadLibraryExW(L"dbgeng.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) *where = "System32\\dbgeng.dll (fallback: run scripts\\setup-runtime.ps1 for the full WinDbg engine)";
    return h;
}

bool Debugger::Start(std::string* error) {
    thread_ = std::thread([this] { EngineThreadMain(); });
    std::unique_lock<std::mutex> lk(start_m_);
    start_cv_.wait(lk, [&] { return ready_.load() || !start_error_.empty(); });
    if (!start_error_.empty()) {
        *error = start_error_;
        lk.unlock();
        quit_ = true;
        task_cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void Debugger::Shutdown() {
    if (!thread_.joinable()) return;
    if (ready_) {
        try {
            Invoke([this]() -> json {
                if (session_) {
                    if (launched_)
                        client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
                    else if (!is_dump_) {
                        if (running_) {
                            BreakIn();
                            control_->WaitForEvent(0, 5000);
                        }
                        client_->EndSession(DEBUG_END_ACTIVE_DETACH);
                    } else
                        client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
                    session_ = false;
                    running_ = false;
                }
                return nullptr;
            });
        } catch (...) {
        }
    }
    quit_ = true;
    task_cv_.notify_all();
    thread_.join();
}

void Debugger::EngineThreadMain() {
    auto fail = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lk(start_m_);
        start_error_ = msg;
        start_cv_.notify_all();
    };
    std::string where;
    dbgeng_ = LoadDbgEng(&where);
    if (!dbgeng_) return fail("could not load dbgeng.dll");
    using DebugCreateFn = HRESULT(STDAPICALLTYPE*)(REFIID, PVOID*);
    auto create = (DebugCreateFn)GetProcAddress(dbgeng_, "DebugCreate");
    if (!create) return fail("dbgeng.dll has no DebugCreate export");
    HRESULT hr = create(__uuidof(IDebugClient7), (void**)&client_);
    if (FAILED(hr)) return fail("DebugCreate failed: " + HResultText(hr));
    if (FAILED(client_->QueryInterface(__uuidof(IDebugControl7), (void**)&control_)) ||
        FAILED(client_->QueryInterface(__uuidof(IDebugSymbols5), (void**)&symbols_)) ||
        FAILED(client_->QueryInterface(__uuidof(IDebugRegisters2), (void**)&registers_)) ||
        FAILED(client_->QueryInterface(__uuidof(IDebugDataSpaces4), (void**)&data_)) ||
        FAILED(client_->QueryInterface(__uuidof(IDebugSystemObjects4), (void**)&sysobj_)))
        return fail("dbgeng.dll is too old (missing IDebugControl7/IDebugSymbols5)");

    event_cb_ = new EventCallbacks(this);
    output_cb_ = new OutputCallbacks(this);
    client_->SetEventCallbacksWide(event_cb_);
    client_->SetOutputCallbacksWide(output_cb_);
    client_->SetOutputMask(DEBUG_OUTPUT_NORMAL | DEBUG_OUTPUT_ERROR | DEBUG_OUTPUT_WARNING |
                           DEBUG_OUTPUT_DEBUGGEE);
    control_->AddEngineOptions(DEBUG_ENGOPT_INITIAL_BREAK);
    control_->SetExpressionSyntax(DEBUG_EXPR_MASM);
    // SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS
    symbols_->AddSymbolOptions(0x2 | 0x10 | 0x200 | 0x80000);
    // Note: SYMOPT_NO_UNQUALIFIED_LOADS stays on -- unqualified lookups scan every module's PDB
    // (tens of seconds on Unreal). Natvis files needing globals are patched instead (PatchNatvis).

    char ver[64] = {};
    {
        DWORD dummy;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(dbgeng_, path, MAX_PATH);
        DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
        std::vector<char> vi(sz);
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT len = 0;
        if (sz && GetFileVersionInfoW(path, 0, sz, vi.data()) &&
            VerQueryValueW(vi.data(), L"\\", (void**)&ffi, &len) && ffi)
            snprintf(ver, sizeof(ver), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS),
                     LOWORD(ffi->dwFileVersionMS), HIWORD(ffi->dwFileVersionLS),
                     LOWORD(ffi->dwFileVersionLS));
    }
    engine_info_ = "DbgEng " + std::string(ver) + " from " + where;
    DiagLog("%s", engine_info_.c_str());
    {
        std::lock_guard<std::mutex> lk(start_m_);
        ready_ = true;
        start_cv_.notify_all();
    }

    while (!quit_) {
        if (running_) {
            HRESULT whr = control_->WaitForEvent(0, 50);
            if (whr != S_FALSE) OnWaitReturned(whr);
            DrainTasks();
        } else {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(task_m_);
                task_cv_.wait(lk, [&] { return quit_ || running_ || !tasks_.empty(); });
                if (quit_) break;
                if (tasks_.empty()) continue;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    // Drain any stragglers so callers are not left waiting forever.
    DrainTasks();
    client_->SetEventCallbacksWide(nullptr);
    client_->SetOutputCallbacksWide(nullptr);
    if (sysobj_) sysobj_->Release();
    if (data_) data_->Release();
    if (registers_) registers_->Release();
    if (symbols_) symbols_->Release();
    if (control_) control_->Release();
    if (client_) client_->Release();
}

void Debugger::DrainTasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lk(task_m_);
            if (tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

json Debugger::Invoke(std::function<json()> fn) {
    if (std::this_thread::get_id() == thread_.get_id()) return fn();
    auto done = std::make_shared<std::promise<void>>();
    json result;
    std::string error;
    bool is_tool_error = false;
    {
        std::lock_guard<std::mutex> lk(task_m_);
        if (quit_) throw ToolError("debugger engine is shutting down");
        tasks_.push_back([&, done] {
            try {
                result = fn();
            } catch (const ToolError& e) {
                error = e.what();
                is_tool_error = true;
            } catch (const std::exception& e) {
                error = std::string("internal error: ") + e.what();
            } catch (...) {
                error = "internal error";
            }
            done->set_value();
        });
    }
    task_cv_.notify_all();
    done->get_future().wait();
    if (!error.empty()) throw ToolError(error);
    return result;
}

// ======================= Callbacks → state =======================

void Debugger::OnOutput(ULONG mask, const wchar_t* text) {
    std::string s = Narrow(text ? text : L"");
    if (mask & DEBUG_OUTPUT_DEBUGGEE) {
        debuggee_out_->Append(s);
    } else if (capture_depth_ > 0) {
        capture_ += s;
    } else {
        engine_log_.Append(s);
    }
}

ULONG Debugger::OnBreakpointEvent(IDebugBreakpoint2* bp) {
    ULONG eid = 0;
    if (FAILED(bp->GetId(&eid))) return DEBUG_STATUS_NO_CHANGE;
    if (eid == guard_eid_) {
        // Step guard: only the return of the frame being stepped counts, not a recursive or
        // later call that reaches the same return address.
        ULONG64 sp = 0;
        registers_->GetStackOffset(&sp);
        return sp >= guard_cfa_ ? DEBUG_STATUS_BREAK : DEBUG_STATUS_IGNORE_EVENT;
    }
    auto it = engine_to_bp_.find(eid);
    if (it == engine_to_bp_.end()) return DEBUG_STATUS_NO_CHANGE;
    auto bit = bps_.find(it->second);
    if (bit == bps_.end()) return DEBUG_STATUS_NO_CHANGE;
    BreakpointSpec& b = bit->second;

    if (b.is_function && !b.relocated) {
        // First hit at a function's entry: move the breakpoint past the prologue (VS semantics, so
        // parameters are readable) and let execution reach the new address.
        b.relocated = true;
        ULONG64 off = 0;
        uint64_t disp = 1;
        if (SUCCEEDED(bp->GetOffset(&off)) && !SymbolAt(off, 0, &disp).empty() && disp == 0) {
            uint64_t moved = SkipPrologue(off);
            if (moved != off && SUCCEEDED(bp->SetOffset(moved))) return DEBUG_STATUS_IGNORE_EVENT;
        }
    }

    if (!b.condition.empty()) {
        uint64_t v = 0;
        std::string err;
        bool ok = TryEvalNumber(b.condition, true, &v, &err);
        if (!ok) {
            // Fall back to the data model evaluator (handles natvis'd types, std::string ==, ...).
            std::string r = ShortValue("(bool)(" + b.condition + ")");
            if (r == "true" || r == "false") {
                ok = true;
                v = r == "true";
            }
        }
        if (!ok) {
            b.last_error = "condition failed to evaluate: " + err;
            pending_bp_hit_ = b.id;
            pending_bp_note_ = b.last_error + " (stopped so you can fix it)";
            return DEBUG_STATUS_BREAK;
        }
        if (v == 0) return DEBUG_STATUS_IGNORE_EVENT;
    }
    b.hits++;
    if (!b.hit_condition.empty()) {
        std::string hc = b.hit_condition;
        hc.erase(std::remove(hc.begin(), hc.end(), ' '), hc.end());
        bool pass = false;
        try {
            if (hc.rfind(">=", 0) == 0)
                pass = b.hits >= std::stoull(hc.substr(2));
            else if (hc.rfind("==", 0) == 0)
                pass = b.hits == std::stoull(hc.substr(2));
            else if (hc.rfind("%", 0) == 0) {
                uint64_t m = std::stoull(hc.substr(1));
                pass = m && b.hits % m == 0;
            } else
                pass = b.hits == std::stoull(hc);
        } catch (...) {
            pass = true;
        }
        if (!pass) return DEBUG_STATUS_IGNORE_EVENT;
    }
    if (!b.log_message.empty()) {
        EnsureNatvisLoaded();
        std::string msg = FormatLogMessage(b.log_message, b);
        trace_log_.Append("[bp " + std::to_string(b.id) + "] " + msg + "\n");
        if (!b.log_and_break) return DEBUG_STATUS_IGNORE_EVENT;
    }
    pending_bp_hit_ = b.id;
    return DEBUG_STATUS_BREAK;
}

ULONG Debugger::OnExceptionEvent(EXCEPTION_RECORD64* rec, ULONG first_chance) {
    // Stale breakpoint trap: another thread executed a temporary int3 (a step's return
    // breakpoint, a step guard, a removed breakpoint) while the event that removed it was being
    // handled. Its trap arrives after the int3 is gone. RIP already points at the trap address,
    // which now holds the original instruction, so resume as if nothing happened (as VS does).
    // Real int3s (__debugbreak, DbgBreakPoint used by pause) still read as 0xCC.
    if (rec && first_chance && rec->ExceptionCode == 0x80000003 && !pause_requested_) {
        uint8_t b = 0;
        ULONG got = 0;
        if (SUCCEEDED(data_->ReadVirtual(rec->ExceptionAddress, &b, 1, &got)) && got == 1 && b != 0xCC) {
            ULONG64 ip = 0;
            registers_->GetInstructionOffset(&ip);
            ULONG rip_idx = 0;
            if (ip == rec->ExceptionAddress + 1 && SUCCEEDED(registers_->GetIndexByName("rip", &rip_idx))) {
                DEBUG_VALUE v = {};
                v.Type = DEBUG_VALUE_INT64;
                v.I64 = rec->ExceptionAddress;
                if (SUCCEEDED(registers_->SetValue(rip_idx, &v))) ip = rec->ExceptionAddress;
            }
            if (ip == rec->ExceptionAddress) {
                ++stale_traps_;
                return DEBUG_STATUS_IGNORE_EVENT;
            }
            DiagLog("int3 trap at %llx (byte %02x) not resumed: ip=%llx", (unsigned long long)rec->ExceptionAddress,
                    b, (unsigned long long)ip);
        }
    }
    return DEBUG_STATUS_NO_CHANGE;  // event filters (sxe/sxd) decide
}

ULONG Debugger::OnExitProcessEvent(ULONG exit_code) {
    pending_exit_ = true;
    pending_exit_code_ = exit_code;
    return DEBUG_STATUS_BREAK;
}

// ======================= Session state machine =======================

void Debugger::RequireSession() {
    if (!session_)
        throw ToolError("no active debug session (use launch, attach or open_dump first)");
}

void Debugger::RequireStopped() {
    RequireSession();
    if (running_)
        throw ToolError(
            "target is running; call pause (or wait) first. Breakpoints can be set while running.");
}

void Debugger::RequireLive() {
    RequireSession();
    if (is_dump_) throw ToolError("not supported for dump files (target is not live)");
}

void Debugger::ResumeTarget(ULONG status, const char* kind) {
    HRESULT hr = control_->SetExecutionStatus(status);
    if (FAILED(hr)) throw ToolError("failed to resume target: " + HResultText(hr));
    resume_kind_ = kind;
    pending_bp_hit_ = 0;
    pending_bp_note_.clear();
    running_ = true;
    task_cv_.notify_all();
}

void Debugger::OnWaitReturned(HRESULT hr) {
    if (suppress_stop_) return;
    StopInfo si;
    if (hr != S_OK) {
        // No more targets (process exited without an exit event, or session died).
        si.reason = pending_exit_ ? "exited" : "session_ended";
        si.exit_code = pending_exit_code_;
        si.note = "WaitForEvent: " + HResultText(hr);
        last_stop_ = si;
        EndSessionState(si.reason);
        return;
    }
    ULONG status = 0;
    control_->GetExecutionStatus(&status);
    if (pending_exit_) {
        si.reason = "exited";
        si.exit_code = pending_exit_code_;
        last_stop_ = si;
        client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
        EndSessionState("exited");
        return;
    }
    if (status != DEBUG_STATUS_BREAK) {
        if (status == DEBUG_STATUS_NO_DEBUGGEE) {
            si.reason = "session_ended";
            last_stop_ = si;
            EndSessionState("session_ended");
        }
        return;  // still running (e.g. GO after an ignored event)
    }
    running_ = false;

    ULONG type = 0, pid = 0, tid = 0;
    union {
        DEBUG_LAST_EVENT_INFO_BREAKPOINT bp;
        DEBUG_LAST_EVENT_INFO_EXCEPTION ex;
        DEBUG_LAST_EVENT_INFO_EXIT_PROCESS ep;
        char raw[512];
    } extra = {};
    ULONG used = 0;
    control_->GetLastEventInformation(&type, &pid, &tid, &extra, sizeof(extra), &used, nullptr, 0,
                                      nullptr);

    if (type == DEBUG_EVENT_EXCEPTION && !pending_bp_hit_ && extra.ex.FirstChance &&
        extra.ex.ExceptionRecord.ExceptionCode == 0x80000003 && !pause_requested_ &&
        resume_kind_ != "pause" && ResumeAfterStaleTrap(extra.ex.ExceptionRecord.ExceptionAddress))
        return;

    if (pending_bp_hit_) {
        si.reason = "breakpoint";
        si.breakpoint_id = pending_bp_hit_;
        si.note = pending_bp_note_;
        auto it = bps_.find(pending_bp_hit_);
        if (it != bps_.end()) {
            si.breakpoint_location = it->second.location;
            if (it->second.internal) {
                si.reason = it->second.internal_tag;  // "main" or "run_to"
                si.breakpoint_id = 0;
            }
        }
    } else if (type == DEBUG_EVENT_BREAKPOINT) {
        // Engine-internal breakpoint (step-out, etc.) or a raw-command breakpoint.
        si.reason = (resume_kind_ == "step") ? "step" : "breakpoint";
    } else if (type == DEBUG_EVENT_EXCEPTION) {
        uint32_t code = extra.ex.ExceptionRecord.ExceptionCode;
        if ((resume_kind_ == "pause" || pause_requested_) && code == 0x80000003) {
            si.reason = "pause";
        } else if (code == 0x80000004 && resume_kind_ == "step") {
            si.reason = "step";
        } else {
            si.reason = "exception";
            si.exception = DecodeException(extra.ex.ExceptionRecord, extra.ex.FirstChance != 0);
        }
    } else if (resume_kind_ == "step") {
        si.reason = "step";
    } else if (resume_kind_ == "pause" || pause_requested_) {
        si.reason = "pause";
    } else {
        si.reason = "break";
    }

    if (step_.active) {
        std::string note;
        if (si.reason == "step") {
            try {
                if (StepContinue(&note)) return;  // still stepping
            } catch (const std::exception& e) {
                note = e.what();
            }
        }
        if (!note.empty()) si.note += (si.note.empty() ? "" : "; ") + note;
        ClearGuard();
        step_ = StepCtl{};
        control_->SetCodeLevel(DEBUG_LEVEL_SOURCE);
    }

    if (si.reason == "pause") {
        // Break-in happens on an injected thread; select the main thread like VS does.
        ULONG ids[1], sys[1];
        if (SUCCEEDED(sysobj_->GetThreadIdsByIndex(0, 1, ids, sys))) sysobj_->SetCurrentThreadId(ids[0]);
    }
    // Every stop starts at the top frame. DbgEng otherwise keeps an explicitly selected scope
    // (select_frame, or the inline frame `gu` leaves selected) across events, and source-level
    // stepping then runs relative to that possibly-dead frame and can run free.
    ResetScopeToTop();

    // One-shot user breakpoints are removed after their first (breaking) hit.
    if (si.reason == "breakpoint") {
        auto it = bps_.find(si.breakpoint_id);
        if (it != bps_.end() && it->second.one_shot) {
            RemoveEngineBreakpoint(it->second);
            bps_.erase(it);
            si.note += (si.note.empty() ? "" : "; ") + std::string("one-shot breakpoint removed");
        }
    }
    CleanupTempBreakpoints();
    resume_kind_.clear();
    pause_requested_ = false;
    {
        std::lock_guard<std::mutex> lk(state_m_);
        last_stop_ = si;
        ++stop_seq_;
    }
    stop_cv_.notify_all();
}

void Debugger::EndSessionState(const std::string& reason) {
    running_ = false;
    session_ = false;
    is_dump_ = false;
    pid_ = 0;
    pending_exit_ = false;
    for (auto& [id, b] : bps_) {
        b.engine_id = DEBUG_ANY_ID;
    }
    engine_to_bp_.clear();
    step_ = StepCtl{};
    guard_eid_ = DEBUG_ANY_ID;
    inline_sites_.clear();
    patched_natvis_.clear();
    fname_module_.clear();
    // Drop internal temporaries.
    for (auto it = bps_.begin(); it != bps_.end();) {
        if (it->second.internal)
            it = bps_.erase(it);
        else
            ++it;
    }
    debuggee_out_->Flush();
    {
        std::lock_guard<std::mutex> lk(state_m_);
        ++stop_seq_;
    }
    stop_cv_.notify_all();
    DiagLog("session ended: %s", reason.c_str());
}

void Debugger::BeginSession(bool launched, bool dump) {
    session_ = true;
    launched_ = launched;
    is_dump_ = dump;
    pending_exit_ = false;
    pending_exit_code_ = 0;
    pending_bp_hit_ = 0;
    stale_traps_ = 0;
    ULONG sys = 0;
    if (SUCCEEDED(sysobj_->GetCurrentProcessSystemId(&sys))) pid_ = sys;
    for (auto& [id, b] : bps_) b.hits = 0;
    exe_module_ = ExeModuleName();
    // Make C++ the default for dx/?? and keep MASM for addresses.
    Exec(".lines -e");
    Exec(".prefer_dml 0");
    // Continue break-instruction exceptions as handled, so a stale breakpoint trap (see
    // OnExceptionEvent) can be resumed transparently and a __debugbreak() can be continued past.
    if (!dump) Exec("sxe -h bpe");
    // Auto-load Unreal Engine's visualizers when the target lives in a UE tree
    // (<root>\Engine\Binaries\... or a project's Binaries next to an engine).
    {
        wchar_t exe[MAX_PATH * 2] = {};
        DWORD exe_len = MAX_PATH * 2;
        HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid_);
        BOOL got_exe = ph && QueryFullProcessImageNameW(ph, 0, exe, &exe_len);
        if (ph) CloseHandle(ph);
        if (!got_exe) got_exe = SUCCEEDED(sysobj_->GetCurrentProcessExecutableNameWide(exe, MAX_PATH * 2, nullptr));
        if (got_exe) {
            std::wstring dir = exe;
            for (int i = 0; i < 8; ++i) {
                size_t s = dir.find_last_of(L"\\/");
                if (s == std::wstring::npos) break;
                dir = dir.substr(0, s);
                std::wstring cand = dir + L"\\Engine\\Extras\\VisualStudioDebugging\\Unreal.natvis";
                if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    std::string c = Narrow(cand);
                    if (std::find(natvis_files_.begin(), natvis_files_.end(), c) == natvis_files_.end())
                        natvis_files_.push_back(c);
                    break;
                }
            }
        }
    }
}

void Debugger::EnsureNatvisLoaded() {
    // DbgEng drops visualizers loaded at the initial breakpoint when the process finishes
    // initializing, so (re)load any of ours that are missing whenever we stop.
    if (natvis_files_.empty()) return;
    std::string list = ToLower(Exec(".nvlist"));
    for (auto& orig : natvis_files_) {
        std::string f = PatchNatvis(orig);
        if (list.find(ToLower(f)) != std::string::npos) continue;
        std::string out = Exec(".nvload " + QuoteArg(f));
        if (out.find("Successfully") == std::string::npos) DiagLog("nvload %s: %s", f.c_str(), Trim(out).c_str());
    }
    natvis_checked_ = true;
}

void Debugger::CleanupTempBreakpoints() {
    for (auto it = bps_.begin(); it != bps_.end();) {
        if (it->second.internal) {
            RemoveEngineBreakpoint(it->second);
            it = bps_.erase(it);
        } else {
            ++it;
        }
    }
}

json Debugger::WithTargetStopped(const std::function<json()>& fn) {
    if (!running_) return fn();
    // Transparently break in, do the work, and resume -- like VS editing breakpoints at runtime.
    suppress_stop_ = true;
    BreakIn();
    HRESULT hr = control_->WaitForEvent(0, 10000);
    suppress_stop_ = false;
    if (hr != S_OK) {
        OnWaitReturned(hr);
        throw ToolError("could not break into the target to apply the change: " + HResultText(hr));
    }
    ULONG type = 0, pid = 0, tid = 0;
    DEBUG_LAST_EVENT_INFO_EXCEPTION ex = {};
    control_->GetLastEventInformation(&type, &pid, &tid, &ex, sizeof(ex), nullptr, nullptr, 0,
                                      nullptr);
    bool our_break = type == DEBUG_EVENT_EXCEPTION && ex.ExceptionRecord.ExceptionCode == 0x80000003 &&
                     !pending_bp_hit_ && !pending_exit_;
    if (!our_break) {
        // A real event raced with our break-in; surface it as a normal stop.
        running_ = true;
        OnWaitReturned(hr);
        return fn();
    }
    json r;
    try {
        r = fn();
    } catch (...) {
        control_->SetExecutionStatus(DEBUG_STATUS_GO);
        running_ = true;
        throw;
    }
    control_->SetExecutionStatus(DEBUG_STATUS_GO);
    running_ = true;
    return r;
}

json Debugger::WaitForStop(uint64_t seq_before, int timeout_ms, int report_frames) {
    bool stopped;
    {
        std::unique_lock<std::mutex> lk(state_m_);
        stopped = stop_cv_.wait_for(lk, std::chrono::milliseconds(ClampTimeout(timeout_ms)),
                                    [&] { return stop_seq_ != seq_before; });
    }
    // A target that is still running can be reported without the engine thread (which may be
    // busy inside WaitForEvent processing a stream of logpoint hits).
    if (!stopped && running_) return RunningReport();
    return Invoke([&]() -> json {
        if (running_) return RunningReport();
        return BuildStopReport(report_frames);
    });
}

json Debugger::RunningReport() {
    json j;
    j["state"] = "running";
    j["pid"] = pid_.load();
    j["note"] =
        "Target still running (timeout). Call wait to keep waiting, or pause to break in.";
    AttachNewOutput(j);
    return j;
}

void Debugger::AttachNewOutput(json& j) {
    std::lock_guard<std::mutex> lk(cursor_m_);
    bool trunc = false;
    auto out = debuggee_out_->Since(out_cursor_, 60, &out_cursor_, &trunc);
    if (!out.empty()) {
        std::string s;
        for (auto& l : out) s += l + "\n";
        j["new_output"] = Clip(s, 6000);
        if (trunc) j["new_output_truncated"] = true;
    }
    auto tr = trace_log_.Since(trace_cursor_, 100, &trace_cursor_, &trunc);
    if (!tr.empty()) {
        std::string s;
        for (auto& l : tr) s += l + "\n";
        j["logpoints"] = Clip(s, 8000);
        if (trunc) j["logpoints_truncated"] = "older logpoint lines omitted; use output source=logpoints";
    }
}

json Debugger::BuildStopReport(int frames) {
    json j;
    StopInfo si;
    {
        std::lock_guard<std::mutex> lk(state_m_);
        si = last_stop_;
    }
    if (!session_) {
        j["state"] = si.reason == "exited" ? "exited" : "no_session";
        if (si.reason == "exited") j["exit_code"] = si.exit_code;
        if (!si.note.empty()) j["note"] = si.note;
        AttachNewOutput(j);
        return j;
    }
    if (running_) return RunningReport();
    EnsureNatvisLoaded();
    j["state"] = "stopped";
    j["reason"] = si.reason;
    if (si.breakpoint_id) {
        j["breakpoint"] = {{"id", si.breakpoint_id}, {"location", si.breakpoint_location}};
        auto it = bps_.find(si.breakpoint_id);
        if (it != bps_.end()) j["breakpoint"]["hits"] = it->second.hits;
    }
    if (!si.exception.is_null()) j["exception"] = si.exception;
    if (!si.note.empty()) j["note"] = si.note;

    ULONG sys_tid = 0;
    sysobj_->GetCurrentThreadSystemId(&sys_tid);
    json th = {{"tid", sys_tid}};
    std::string tn = ThreadName(sys_tid);
    if (!tn.empty()) th["name"] = tn;
    j["thread"] = th;

    auto stack = CurrentStack((ULONG)std::max(frames, 1));
    if (!stack.empty()) {
        j["location"] = FrameJson(stack[0], false, true);
        if (frames > 1) {
            json st = json::array();
            for (size_t i = 0; i < stack.size(); ++i) {
                bool adjust = IsReturnAddressFrame(stack, i);
                json fj = FrameJson(stack[i], adjust, false);
                std::string line = "#" + std::to_string(stack[i].FrameNumber) + " " +
                                   fj.value("function", Hex(stack[i].InstructionOffset));
                if (fj.contains("file"))
                    line += "  " + fj["file"].get<std::string>() + ":" +
                            std::to_string(fj["line"].get<int>());
                st.push_back(line);
            }
            j["stack"] = st;
        }
    }
    if (!watches_.empty()) {
        json w = json::object();
        for (auto& e : watches_) w[e] = ShortValue(e);
        j["watches"] = w;
    }
    AttachNewOutput(j);
    return j;
}

// ======================= Exec / eval helpers =======================

std::string Debugger::Exec(const std::string& cmd, HRESULT* phr) {
    ++capture_depth_;
    std::string saved;
    saved.swap(capture_);
    HRESULT hr = control_->ExecuteWide(DEBUG_OUTCTL_THIS_CLIENT | DEBUG_OUTCTL_NOT_LOGGED,
                                       Widen(cmd).c_str(),
                                       DEBUG_EXECUTE_NOT_LOGGED | DEBUG_EXECUTE_NO_REPEAT);
    std::string out;
    out.swap(capture_);
    capture_.swap(saved);
    --capture_depth_;
    if (phr) *phr = hr;
    // Lazy symbol loads triggered by the command print loader chatter into its output
    // ("*** WARNING: Unable to verify timestamp for X.dll" for every UE module, etc.).
    if (out.find("*** ") != std::string::npos) {
        std::string kept;
        size_t pos = 0;
        while (pos < out.size()) {
            size_t nl = out.find('\n', pos);
            size_t end = nl == std::string::npos ? out.size() : nl + 1;
            std::string_view ln(out.data() + pos, end - pos);
            bool noise = ln.rfind("*** WARNING: Unable to verify timestamp", 0) == 0 ||
                         ln.rfind("*** WARNING: Unable to verify checksum", 0) == 0;
            if (!noise) kept.append(ln);
            pos = end;
        }
        out.swap(kept);
    }
    return out;
}

std::string Debugger::ExecChecked(const std::string& cmd) {
    HRESULT hr;
    std::string out = Exec(cmd, &hr);
    if (FAILED(hr)) {
        std::string msg = Trim(out);
        throw ToolError("'" + cmd + "' failed: " + (msg.empty() ? HResultText(hr) : msg));
    }
    return out;
}

bool Debugger::TryEvalNumber(const std::string& expr, bool cplusplus, uint64_t* out,
                             std::string* err) {
    ULONG old = DEBUG_EXPR_MASM;
    control_->GetExpressionSyntax(&old);
    control_->SetExpressionSyntax(cplusplus ? DEBUG_EXPR_CPLUSPLUS : DEBUG_EXPR_MASM);
    DEBUG_VALUE v = {};
    ULONG rem = 0;
    ++capture_depth_;
    std::string saved;
    saved.swap(capture_);
    HRESULT hr = control_->EvaluateWide(Widen(expr).c_str(), DEBUG_VALUE_INT64, &v, &rem);
    std::string msg;
    msg.swap(capture_);
    capture_.swap(saved);
    --capture_depth_;
    control_->SetExpressionSyntax(old);
    if (FAILED(hr)) {
        if (err) *err = Trim(msg).empty() ? HResultText(hr) : Trim(msg);
        return false;
    }
    *out = v.I64;
    return true;
}

uint64_t Debugger::EvalAddress(const std::string& expr_in) {
    std::string expr = NormalizeModuleQualifiers(expr_in);
    uint64_t v = 0;
    std::string e1, e2;
    if (TryEvalNumber(expr, false, &v, &e1)) return v;
    if (TryEvalNumber(expr, true, &v, &e2)) return v;
    // Data model fallback: pointer values, or the address of arrays/objects.
    for (const std::string& dxe : {expr, "&(" + expr + ")"}) {
        std::string val = ShortValue(dxe);
        size_t p = val.find("0x");
        if (p == 0) {
            try {
                return std::stoull(val.substr(2, val.find_first_not_of("0123456789abcdefABCDEF`", 2) - 2), nullptr, 16);
            } catch (...) {
            }
        }
    }
    throw ToolError("cannot evaluate '" + expr + "' to an address: " + e1);
}

std::string Debugger::ToOffsetExpression(const std::string& loc_in) {
    std::string loc = NormalizeModuleQualifiers(loc_in);
    std::string file;
    ULONG line;
    if (LooksLikeSourceLine(loc, &file, &line)) {
        // Source-line breakpoints must use the backtick syntax.
        return "`" + file + ":" + std::to_string(line) + "`";
    }
    std::string s = Trim(loc);
    // Names containing template brackets/spaces/operators need the @!"..." form.
    if (s.find_first_of("<> ,()") != std::string::npos && s.rfind("@!", 0) != 0 &&
        s.find('+') == std::string::npos && s.find('"') == std::string::npos)
        return "@!\"" + s + "\"";
    return s;
}

uint64_t Debugger::ResolveLocation(const std::string& loc) {
    std::string file;
    ULONG line;
    if (LooksLikeSourceLine(loc, &file, &line)) {
        ULONG64 off = 0;
        HRESULT hr = symbols_->GetOffsetByLineWide(line, Widen(file).c_str(), &off);
        if (FAILED(hr))
            throw ToolError("no code found for " + loc +
                            " (is the module loaded and are its symbols available?)");
        return off;
    }
    return EvalAddress(ToOffsetExpression(loc));
}

std::string Debugger::ShortValue(const std::string& expr) {
    HRESULT hr;
    std::string out = Exec("dx -r0 " + expr, &hr);
    auto lines = SplitLines(out);
    if (FAILED(hr) || lines.empty()) {
        std::string m = Trim(out);
        return "<error: " + (m.empty() ? HResultText(hr) : m) + ">";
    }
    std::string first = lines[0];
    if (first.rfind("Error:", 0) == 0) return "<" + Trim(first) + ">";
    {
        // Unreal FName: decode natively (DbgEng cannot evaluate UE's FName natvis).
        size_t t = first.rfind("[Type: ");
        if (t != std::string::npos) {
            std::string ty = first.substr(t + 7);
            if (!ty.empty() && ty.back() == ']') ty.pop_back();
            ty = Trim(ty);
            if (ty.rfind("const ", 0) == 0) ty = ty.substr(6);
            if (ty == "FName" || ty == "FName *" || ty == "FName &") {
                std::string obj = ty == "FName *" ? "(*(" + expr + "))" : "(" + expr + ")";
                std::string d = DecodeFName(obj);
                if (!d.empty()) return "\"" + d + "\"";
            }
        }
    }
    // Format: "<expr>  : <value> [Type: T]"  or  "<expr>  [Type: T]" for aggregates.
    size_t colon = first.find(" : ");
    std::string val;
    if (colon != std::string::npos) {
        val = first.substr(colon + 3);
        size_t t = val.rfind(" [Type: ");
        if (t != std::string::npos) val = val.substr(0, t);
        return Trim(val);
    }
    // Aggregate: summarise first-level children.
    out = Exec("dx -r1 " + expr, &hr);
    lines = SplitLines(out);
    std::string agg = "{";
    int n = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string l = Trim(lines[i]);
        if (l.rfind("[+0x", 0) == 0) {
            size_t e = l.find(']');
            if (e != std::string::npos) l = Trim(l.substr(e + 1));
        }
        size_t c = l.find(" : ");
        std::string name = c == std::string::npos ? l : Trim(l.substr(0, c));
        std::string v = c == std::string::npos ? "" : l.substr(c + 3);
        size_t t = v.rfind(" [Type: ");
        if (t != std::string::npos) v = v.substr(0, t);
        size_t nt = name.find(" [Type: ");
        if (nt != std::string::npos) name = Trim(name.substr(0, nt));
        if (n++) agg += ", ";
        agg += v.empty() ? name : name + "=" + Trim(v);
        if (agg.size() > 300) {
            agg += ", ...";
            break;
        }
    }
    agg += "}";
    if (n == 0) {
        size_t t = first.find("[Type: ");
        return t == std::string::npos ? Trim(first) : "{" + first.substr(t) ;
    }
    return agg;
}

std::string Debugger::FormatLogMessage(const std::string& msg, BreakpointSpec& b) {
    std::string out;
    for (size_t i = 0; i < msg.size();) {
        char c = msg[i];
        if (c == '{') {
            size_t depth = 1, j = i + 1;
            while (j < msg.size() && depth) {
                if (msg[j] == '{') depth++;
                else if (msg[j] == '}') depth--;
                if (depth) ++j;
            }
            if (j >= msg.size()) {
                out += msg.substr(i);
                break;
            }
            out += ShortValue(msg.substr(i + 1, j - i - 1));
            i = j + 1;
            continue;
        }
        if (c == '$') {
            auto starts = [&](const char* tok) { return msg.compare(i, strlen(tok), tok) == 0; };
            if (starts("$FUNCTION")) {
                ULONG64 ip = 0;
                registers_->GetInstructionOffset(&ip);
                out += SymbolAt(ip, 0);
                i += 9;
                continue;
            }
            if (starts("$CALLER")) {
                auto st = CurrentStack(2);
                out += st.size() > 1 ? SymbolAt(st[1].InstructionOffset, st[1].InlineFrameContext)
                                     : "?";
                i += 7;
                continue;
            }
            if (starts("$CALLSTACK")) {
                auto st = CurrentStack(8);
                for (size_t k = 0; k < st.size(); ++k)
                    out += (k ? " <- " : "") + SymbolAt(st[k].InstructionOffset, st[k].InlineFrameContext);
                i += 10;
                continue;
            }
            if (starts("$TID")) {
                ULONG t = 0;
                sysobj_->GetCurrentThreadSystemId(&t);
                out += std::to_string(t);
                i += 4;
                continue;
            }
            if (starts("$HITS")) {
                out += std::to_string(b.hits);
                i += 5;
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

std::string Debugger::SymbolAt(uint64_t addr, ULONG inline_ctx, uint64_t* disp) {
    wchar_t buf[2048];
    ULONG sz = 0;
    ULONG64 d = 0;
    HRESULT hr = inline_ctx
                     ? symbols_->GetNameByInlineContextWide(addr, inline_ctx, buf, 2048, &sz, &d)
                     : symbols_->GetNameByOffsetWide(addr, buf, 2048, &sz, &d);
    if (FAILED(hr)) return "";
    if (disp) *disp = d;
    return Narrow(buf);
}

bool Debugger::LineAt(uint64_t addr, ULONG inline_ctx, std::string* file, ULONG* line) {
    wchar_t buf[MAX_PATH * 2];
    ULONG sz = 0;
    ULONG64 d = 0;
    HRESULT hr = inline_ctx ? symbols_->GetLineByInlineContextWide(addr, inline_ctx, line, buf,
                                                                   MAX_PATH * 2, &sz, &d)
                            : symbols_->GetLineByOffsetWide(addr, line, buf, MAX_PATH * 2, &sz, &d);
    if (FAILED(hr)) return false;
    *file = Narrow(buf);
    return true;
}

std::string Debugger::ModuleNameAt(uint64_t addr) {
    ULONG idx = 0;
    ULONG64 base = 0;
    if (FAILED(symbols_->GetModuleByOffset(addr, 0, &idx, &base))) return "";
    wchar_t buf[512];
    if (FAILED(symbols_->GetModuleNameStringWide(DEBUG_MODNAME_MODULE, idx, base, buf, 512, nullptr)))
        return "";
    return Narrow(buf);
}

std::string Debugger::ResolveSourceFile(const std::string& file) {
    if (GetFileAttributesW(Widen(file).c_str()) != INVALID_FILE_ATTRIBUTES) return file;
    wchar_t buf[MAX_PATH * 2];
    ULONG found = 0, sz = 0;
    if (SUCCEEDED(symbols_->FindSourceFileWide(0, Widen(file).c_str(),
                                               DEBUG_FIND_SOURCE_BEST_MATCH, &found, buf,
                                               MAX_PATH * 2, &sz)))
        return Narrow(buf);
    return "";
}

std::string Debugger::SourceSnippet(const std::string& file, ULONG line, int context) {
    std::string path = ResolveSourceFile(file);
    if (path.empty()) return "";
    auto it = source_cache_.find(path);
    if (it == source_cache_.end()) {
        std::ifstream in(Widen(path), std::ios::binary);
        if (!in) return "";
        std::stringstream ss;
        ss << in.rdbuf();
        if (source_cache_.size() > 200) source_cache_.clear();
        it = source_cache_.emplace(path, SplitLines(ss.str())).first;
    }
    auto& lines = it->second;
    std::string out;
    int lo = std::max(1, (int)line - context), hi = std::min((int)lines.size(), (int)line + context);
    for (int i = lo; i <= hi; ++i) {
        char pre[24];
        snprintf(pre, sizeof(pre), "%s%5d  ", i == (int)line ? ">" : " ", i);
        out += pre + lines[i - 1] + "\n";
    }
    return out;
}

std::vector<DEBUG_STACK_FRAME_EX> Debugger::CurrentStack(ULONG max_frames) {
    max_frames = std::min(max_frames, kStackLimit);
    std::vector<DEBUG_STACK_FRAME_EX> frames(max_frames);
    ULONG filled = 0;
    if (FAILED(control_->GetStackTraceEx(0, 0, 0, frames.data(), max_frames, &filled))) filled = 0;
    frames.resize(filled);
    return frames;
}

json Debugger::FrameJson(const DEBUG_STACK_FRAME_EX& f, bool adjust, bool with_source) {
    json j;
    j["frame"] = f.FrameNumber;
    uint64_t disp = 0;
    std::string fn = SymbolAt(f.InstructionOffset, f.InlineFrameContext, &disp);
    if (!fn.empty()) j["function"] = fn + (disp && !f.InlineFrameContext ? "+" + Hex(disp) : "");
    j["address"] = Hex(f.InstructionOffset);
    if (IsInlineFrame(f)) j["inline"] = true;
    std::string file;
    ULONG line = 0;
    uint64_t lookup = adjust ? f.InstructionOffset - 1 : f.InstructionOffset;
    if (LineAt(lookup, f.InlineFrameContext, &file, &line)) {
        j["file"] = file;
        j["line"] = line;
        if (with_source) {
            std::string snip = SourceSnippet(file, line, 3);
            if (!snip.empty()) j["source"] = snip;
        }
    } else if (fn.empty()) {
        std::string m = ModuleNameAt(f.InstructionOffset);
        if (!m.empty()) j["module"] = m;
    }
    return j;
}

std::string Debugger::ThreadName(ULONG sys_tid) {
    if (is_dump_ || !sys_tid) return "";
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, sys_tid);
    if (!h) return "";
    std::string name;
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
        name = Narrow(desc);
        LocalFree(desc);
    }
    CloseHandle(h);
    return name;
}

void Debugger::SelectThreadSys(ULONG sys_tid) {
    ULONG id = 0;
    if (FAILED(sysobj_->GetThreadIdBySystemId(sys_tid, &id)))
        throw ToolError("no thread with tid " + std::to_string(sys_tid) + " (see threads)");
    if (FAILED(sysobj_->SetCurrentThreadId(id))) throw ToolError("failed to select thread");
}

Debugger::StepPos Debugger::CurPos() {
    StepPos p;
    auto st = CurrentStack(48);
    size_t d = 0;
    while (d < st.size() && IsInlineFrame(st[d])) ++d;
    if (st.empty() || d >= st.size()) return p;
    p.valid = true;
    p.depth = (int)d;
    p.ret = st[d].ReturnOffset;
    size_t k = d + 1;
    while (k < st.size() && IsInlineFrame(st[k])) ++k;
    // The caller's Child-SP is this function's CFA (SP right after it returns): unlike RSP it
    // doesn't move during the prologue/epilogue.
    p.cfa = k < st.size() ? st[k].StackOffset : st[d].StackOffset + 8;
    p.func = SymbolAt(st[0].InstructionOffset, st[0].InlineFrameContext);
    if (LineAt(st[0].InstructionOffset, st[0].InlineFrameContext, &p.file, &p.line))
        p.has_line = p.line != 0 && p.line != 0xFEEFEE && p.line != 0xF00F00;
    return p;
}

void Debugger::SetGuard(ULONG64 ret, ULONG64 cfa, ULONG tid) {
    ClearGuard();
    if (!ret) return;
    IDebugBreakpoint2* bp = nullptr;
    if (FAILED(control_->AddBreakpoint2(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp)) || !bp) return;
    bp->SetOffset(ret);
    bp->SetMatchThreadId(tid);
    bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
    bp->GetId(&guard_eid_);
    guard_cfa_ = cfa;
}

void Debugger::ClearGuard() {
    if (guard_eid_ == DEBUG_ANY_ID) return;
    IDebugBreakpoint2* bp = nullptr;
    if (SUCCEEDED(control_->GetBreakpointById2(guard_eid_, &bp)) && bp) control_->RemoveBreakpoint2(bp);
    guard_eid_ = DEBUG_ANY_ID;
}

bool Debugger::ResumeAfterStaleTrap(ULONG64 addr) {
    // Called at a break for a first-chance int3 exception. Breakpoints are out of memory now, so
    // the byte at the trap address shows whether a real int3 lives there. If not, another thread
    // hit a temporary breakpoint (step return/guard) that was removed while its trap was queued:
    // put RIP back on the original instruction and resume what was in progress.
    uint8_t b = 0;
    ULONG got = 0;
    if (FAILED(data_->ReadVirtual(addr, &b, 1, &got)) || got != 1 || b == 0xCC) return false;
    ULONG64 ip = 0;
    registers_->GetInstructionOffset(&ip);
    if (ip == addr + 1) {
        ULONG rip_idx = 0;
        DEBUG_VALUE v = {};
        v.Type = DEBUG_VALUE_INT64;
        v.I64 = addr;
        if (SUCCEEDED(registers_->GetIndexByName("rip", &rip_idx)) && SUCCEEDED(registers_->SetValue(rip_idx, &v)))
            ip = addr;
    }
    if (ip != addr) return false;
    try {
        if (step_.active) {
            sysobj_->SetCurrentThreadId(step_.tid);
            if (!step_.custom && step_.kind != "out") {
                // The interrupted native step can't be resumed as such; finish it instruction
                // by instruction from where the stepping thread is (same start position).
                step_.custom = true;
                control_->SetCodeLevel(DEBUG_LEVEL_ASSEMBLY);
            }
            ULONG st = (step_.guard_out || (!step_.custom && step_.kind == "out")) ? DEBUG_STATUS_GO
                       : step_.kind == "into"                                    ? DEBUG_STATUS_STEP_INTO
                                                                                  : DEBUG_STATUS_STEP_OVER;
            StepIssue(st);
        } else if (resume_kind_ == "step") {
            return false;  // a user instruction step: just report the stop
        } else {
            std::string kind = resume_kind_.empty() ? "continue" : resume_kind_;
            ResumeTarget(DEBUG_STATUS_GO, kind == "continue" ? "continue" : kind.c_str());
        }
    } catch (const std::exception& e) {
        DiagLog("could not resume after stale breakpoint trap at %llx: %s", (unsigned long long)addr, e.what());
        return false;
    }
    ++stale_traps_;
    return true;
}

void Debugger::StepIssue(ULONG status) {
    HRESULT hr = control_->SetExecutionStatus(status);
    if (FAILED(hr)) throw ToolError("failed to resume target: " + HResultText(hr));
    resume_kind_ = "step";
    running_ = true;
}

bool Debugger::StepContinue(std::string* note) {
    if (!step_.custom) {
        // A native step-into can stop on an incremental-linking/import thunk or other code
        // without source; finish it instruction by instruction.
        if (step_.kind != "into" || step_.guard_out) return false;
        StepPos p = CurPos();
        if (!p.valid || p.has_line) return false;
        ClearGuard();
        step_.custom = true;
        control_->SetCodeLevel(DEBUG_LEVEL_ASSEMBLY);
    }
    if (step_.guard_out) {
        ClearGuard();
        step_.guard_out = false;
        step_.noline_run = 0;
    }
    constexpr uint64_t kMaxInstr = 200000;
    if (++step_.instrs > kMaxInstr) {
        *note = "step gave up after " + std::to_string(kMaxInstr) + " instructions";
        return false;
    }
    ULONG tid = 0;
    sysobj_->GetCurrentThreadId(&tid);
    if (tid != step_.tid) return false;  // another thread's event; surface it
    StepPos p = CurPos();
    const StepPos& s = step_.start;
    if (!p.valid) return false;
    static const bool trace = GetEnvironmentVariableA("DEBUGMCP_TRACE_STEPS", nullptr, 0) > 0;
    if (trace)
        DiagLog("  step@%s:%lu depth=%d cfa=%llx", p.func.c_str(), p.line, p.depth, (unsigned long long)p.cfa);
    bool deeper = p.cfa < s.cfa || (p.cfa == s.cfa && p.depth > s.depth);
    bool shallower = p.cfa > s.cfa || (p.cfa == s.cfa && p.depth < s.depth);
    ULONG over = DEBUG_STATUS_STEP_OVER, into = DEBUG_STATUS_STEP_INTO;
    if (step_.kind == "out") {
        if (shallower) return false;
        StepIssue(over);
        return true;
    }
    if (shallower) return false;
    if (!deeper) {
        bool moved = p.file != s.file || p.line != s.line;
        // A sibling inline call at the same inline depth is "inside" the line for a step over.
        // (At depth 0 a function change at the same CFA is a tail jump/thunk target: stop.)
        bool sibling_inline = p.depth > 0 && p.func != s.func;
        if (p.has_line && (moved || p.func != s.func) && (step_.kind == "into" || !sibling_inline))
            return false;
        StepIssue(step_.kind == "into" ? into : over);
        return true;
    }
    if (step_.kind == "into") {
        if (p.has_line) return false;  // entered a call or an inline function with source
        // No source here (system code, thunk): allow a few instructions for import thunks to
        // reach their target, then run to this function's return and keep stepping there.
        if (p.cfa < s.cfa && ++step_.noline_run > 8 && p.ret) {
            SetGuard(p.ret, p.cfa, tid);
            step_.guard_out = true;
            StepIssue(DEBUG_STATUS_GO);
            return true;
        }
        StepIssue(into);
        return true;
    }
    StepIssue(over);
    return true;
}

void Debugger::ResetScopeToTop() {
    if (symbols_) symbols_->ResetScope();
    HRESULT hr;
    Exec(".frame 0n0", &hr);
}

void Debugger::SelectFrameNum(ULONG frame) {
    HRESULT hr;
    std::string out = Exec(".frame 0n" + std::to_string(frame), &hr);
    if (FAILED(hr)) throw ToolError("cannot select frame " + std::to_string(frame) + ": " + Trim(out));
}

void Debugger::ApplyThreadFrameArgs(const json& a) {
    if (a.contains("tid") && !a["tid"].is_null()) SelectThreadSys((ULONG)GetInt(a, "tid", 0));
    if (a.contains("frame") && !a["frame"].is_null()) SelectFrameNum((ULONG)GetInt(a, "frame", 0));
}

std::string Debugger::ExeModuleName() {
    wchar_t buf[MAX_PATH * 2];
    if (FAILED(sysobj_->GetCurrentProcessExecutableNameWide(buf, MAX_PATH * 2, nullptr))) return "";
    std::wstring p = buf;
    size_t s = p.find_last_of(L"\\/");
    if (s != std::wstring::npos) p = p.substr(s + 1);
    size_t dot = p.find_last_of(L'.');
    if (dot != std::wstring::npos) p = p.substr(0, dot);
    return NormalizeModuleName(Narrow(p));
}

json Debugger::DecodeException(const EXCEPTION_RECORD64& rec, bool first_chance) {
    json j;
    j["code"] = Hex(rec.ExceptionCode);
    j["name"] = ExceptionName(rec.ExceptionCode);
    j["address"] = Hex(rec.ExceptionAddress);
    j["first_chance"] = first_chance;
    std::string sym = SymbolAt(rec.ExceptionAddress, 0);
    if (!sym.empty()) j["at"] = sym;
    if (rec.ExceptionCode == 0xC0000005 && rec.NumberParameters >= 2) {
        const char* op = rec.ExceptionInformation[0] == 0 ? "read"
                         : rec.ExceptionInformation[0] == 1 ? "write"
                         : rec.ExceptionInformation[0] == 8 ? "execute (DEP)" : "access";
        j["description"] = std::string("Attempt to ") + op + " address " + Hex(rec.ExceptionInformation[1]);
    }
    if (rec.ExceptionCode == 0xE06D7363 && rec.NumberParameters >= 3) {
        // MSVC C++ EH: [0]=magic [1]=object [2]=ThrowInfo [3]=image base (x64)
        uint64_t obj = rec.ExceptionInformation[1];
        uint64_t throw_info = rec.ExceptionInformation[2];
        uint64_t base = rec.NumberParameters >= 4 ? rec.ExceptionInformation[3] : 0;
        j["object"] = Hex(obj);
        auto rd32 = [&](uint64_t addr, uint32_t* v) {
            ULONG got = 0;
            return SUCCEEDED(data_->ReadVirtual(addr, v, 4, &got)) && got == 4;
        };
        json types = json::array();
        uint32_t cta_rva = 0;
        if (throw_info && rd32(throw_info + 12, &cta_rva) && cta_rva) {
            uint64_t cta = base + cta_rva;
            uint32_t n = 0;
            if (rd32(cta, &n)) {
                for (uint32_t i = 0; i < std::min(n, 16u); ++i) {
                    uint32_t ct_rva = 0, td_rva = 0;
                    if (!rd32(cta + 4 + 4 * i, &ct_rva)) break;
                    if (!rd32(base + ct_rva + 4, &td_rva)) break;
                    char name[512] = {};
                    ULONG got = 0;
                    data_->ReadVirtual(base + td_rva + 16, name, sizeof(name) - 1, &got);
                    name[sizeof(name) - 1] = 0;
                    types.push_back(DemangleTypeName(name));
                }
            }
        }
        if (!types.empty()) {
            j["cpp_type"] = types[0];
            if (types.size() > 1) j["cpp_catchable_as"] = types;
            bool is_std = false;
            for (auto& t : types)
                if (t == "std::exception") is_std = true;
            if (is_std && obj) {
                // MSVC std::exception layout: vptr, __std_exception_data{ const char* _What; bool _DoFree; }
                uint64_t what = 0;
                ULONG got = 0;
                if (SUCCEEDED(data_->ReadVirtual(obj + 8, &what, 8, &got)) && what) {
                    char buf[1024] = {};
                    data_->ReadVirtual(what, buf, sizeof(buf) - 1, &got);
                    j["what"] = std::string(buf);
                }
            }
            j["inspect"] = "evaluate `(" + types[0].get<std::string>() + "*)" + Hex(obj) + "`";
        }
    }
    return j;
}

// ======================= Break-in / prologue / module load =======================

void Debugger::BreakIn() {
    // SetInterrupt is unreliable while we poll WaitForEvent with a timeout; inject a breakpoint
    // thread instead (exactly what "Break All" does).
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid_);
    if (!h) throw ToolError("cannot open target process to break in: " + HResultText(HRESULT_FROM_WIN32(GetLastError())));
    BOOL ok = DebugBreakProcess(h);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) throw ToolError("DebugBreakProcess failed: " + HResultText(HRESULT_FROM_WIN32(err)));
}

uint64_t Debugger::SkipPrologue(uint64_t addr) {
    // Returns the first instruction of the first source line after the function's opening line.
    std::string f0;
    ULONG l0 = 0;
    if (!LineAt(addr, 0, &f0, &l0)) return addr;
    ULONG64 off = addr;
    for (int i = 0; i < 256; ++i) {
        ULONG64 next = 0;
        if (FAILED(control_->GetNearInstruction(off, 1, &next)) || next <= off) break;
        off = next;
        uint64_t fdisp = 0;
        SymbolAt(off, 0, &fdisp);
        if (off - addr != fdisp) break;  // left the function
        wchar_t fb[MAX_PATH * 2];
        ULONG ln = 0;
        ULONG64 disp = 1;
        if (SUCCEEDED(symbols_->GetLineByOffsetWide(off, &ln, fb, MAX_PATH * 2, nullptr, &disp)) &&
            disp == 0 && ln != l0)
            return off;
    }
    return addr;
}

ULONG Debugger::OnLoadModuleEvent(const std::wstring& module_name, const std::wstring& image_name) {
    // Bind pending source-line breakpoints whose file lives in the newly loaded module.
    std::string mod = Narrow(module_name);
    bool any = false;
    std::vector<IDebugBreakpoint2*> deferred_fns;
    for (auto& [id, b] : bps_) {
        if (b.kind == "code" && !b.src_file.empty() && b.engine_id == DEBUG_ANY_ID && b.enabled &&
            (b.module_hint.empty() || IEquals(b.module_hint, mod)))
            any = true;
        if (b.is_function && b.engine_id != DEBUG_ANY_ID && b.location.find('!') == std::string::npos) {
            IDebugBreakpoint2* bp = nullptr;
            ULONG flags = 0;
            if (SUCCEEDED(control_->GetBreakpointById2(b.engine_id, &bp)) && SUCCEEDED(bp->GetFlags(&flags)) &&
                (flags & DEBUG_BREAKPOINT_DEFERRED)) {
                deferred_fns.push_back(bp);
                any = true;
            }
        }
    }
    if (!any) return DEBUG_STATUS_NO_CHANGE;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::string img = ToLower(Narrow(image_name)), win = ToLower(Narrow(windir));
    if (!img.empty() && img.rfind(win, 0) == 0) return DEBUG_STATUS_NO_CHANGE;  // system DLL
    Exec("ld " + mod);
    // Re-evaluate unqualified deferred function breakpoints now that this module has symbols.
    for (auto* bp : deferred_fns) {
        wchar_t expr[2048];
        if (SUCCEEDED(bp->GetOffsetExpressionWide(expr, 2048, nullptr))) bp->SetOffsetExpressionWide(expr);
    }
    for (auto& [id, b] : bps_)
        if (b.kind == "code" && !b.src_file.empty() && b.engine_id == DEBUG_ANY_ID && b.enabled &&
            (b.module_hint.empty() || IEquals(b.module_hint, mod)))
            BindSourceLine(b, false);
    return DEBUG_STATUS_NO_CHANGE;
}

// Unreal.natvis (and similar) reference engine globals such as GNameBlocksDebug unqualified.
// DbgEng only resolves unqualified symbols by scanning every PDB, which we keep disabled, so we
// write a copy with those globals module-qualified (e.g. UnrealEditor_Core!GNameBlocksDebug).
std::string Debugger::PatchNatvis(const std::string& path) {
    auto cached = patched_natvis_.find(path);
    if (cached != patched_natvis_.end()) return cached->second;
    std::ifstream in(Widen(path), std::ios::binary);
    if (!in) return path;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    static const char* kGlobals[] = {"GNameBlocksDebug", "FNameDebugVisualizer", "GDebuggingState",
                                     "GFixedTagStoresForDebugVisualizers", "GObjectHandlePackageDebug",
                                     "GObjectArrayForDebugVisualizers", "GComplexObjectPathDebug"};
    bool any_present = false;
    for (auto* g : kGlobals) any_present |= text.find(g) != std::string::npos;
    if (!any_present) return path;
    // Candidate defining modules: names containing "core" (Core/CoreUObject first), then the exe
    // (monolithic builds).
    std::vector<std::string> cands;
    for (auto& [name, symtype] : UserModules())
        if (ToLower(name).find("core") != std::string::npos) cands.push_back(name);
    auto rank = [](const std::string& n) {
        std::string l = ToLower(n);
        auto ends = [&](const char* suf) {
            size_t k = strlen(suf);
            return l.size() >= k && l.compare(l.size() - k, k, suf) == 0;
        };
        return ends("_core") || l == "core" ? 0 : ends("_coreuobject") || l == "coreuobject" ? 1 : 2;
    };
    std::stable_sort(cands.begin(), cands.end(), [&](auto& x, auto& y) { return rank(x) < rank(y); });
    if (!exe_module_.empty()) cands.push_back(exe_module_);
    int replaced = 0;
    bool unresolved = false;
    std::string name_owner;
    for (auto* g : kGlobals) {
        if (text.find(g) == std::string::npos) continue;
        std::string owner;
        if (std::string(g) == "FNameDebugVisualizer") {
            owner = name_owner;  // a type: lives with GNameBlocksDebug
        } else {
            for (auto& m : cands) {
                ULONG64 off = 0;
                if (SUCCEEDED(symbols_->GetOffsetByNameWide(Widen(m + "!" + g).c_str(), &off)) && off) {
                    owner = m;
                    break;
                }
            }
            if (std::string(g) == "GNameBlocksDebug") name_owner = fname_module_ = owner;
        }
        if (owner.empty()) {
            unresolved = true;
            continue;
        }
        std::string out;
        size_t glen = strlen(g);
        for (size_t i = 0; i < text.size();) {
            if (text.compare(i, glen, g) == 0) {
                char prev = i ? text[i - 1] : ' ';
                char next = i + glen < text.size() ? text[i + glen] : ' ';
                char prev2 = i > 1 ? text[i - 2] : ' ';
                bool qualified = prev == '!' && (isalnum((unsigned char)prev2) || prev2 == '_');  // mod!name
                bool ident_prev = isalnum((unsigned char)prev) || prev == '_' || qualified || prev == ':' ||
                                  prev == '.' || prev == '>';
                bool ident_next = isalnum((unsigned char)next) || next == '_';
                if (!ident_prev && !ident_next) {
                    out += owner + "!" + g;
                    i += glen;
                    ++replaced;
                    continue;
                }
            }
            out.push_back(text[i++]);
        }
        text.swap(out);
    }
    if (!replaced) {
        if (!unresolved) patched_natvis_[path] = path;  // nothing to do; retry later if unresolved
        return path;
    }
    char local[MAX_PATH];
    std::string dir = "C:\\Temp";
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local))) dir = std::string(local) + "\\debugmcp";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\natvis";
    CreateDirectoryA(dir.c_str(), nullptr);
    size_t slash = path.find_last_of("\\/");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string outp = dir + "\\" + std::to_string(std::hash<std::string>{}(path) % 100000) + "_" + base;
    std::ofstream o(Widen(outp), std::ios::binary);
    o << text;
    o.close();
    if (!unresolved) patched_natvis_[path] = outp;
    Exec(".nvunload " + QuoteArg(path));
    DiagLog("patched natvis %s -> %s (%d references qualified)", path.c_str(), outp.c_str(), replaced);
    return outp;
}

// Decodes an Unreal FName (expression of type FName) using the engine's debug name tables.
std::string Debugger::DecodeFName(const std::string& obj) {
    if (fname_module_.empty()) {
        for (auto& [name, symtype] : UserModules()) {
            std::string l = ToLower(name);
            if (l != "core" && !(l.size() > 5 && l.compare(l.size() - 5, 5, "_core") == 0)) continue;
            ULONG64 off = 0;
            if (SUCCEEDED(symbols_->GetOffsetByNameWide(Widen(name + "!GNameBlocksDebug").c_str(), &off)) && off) {
                fname_module_ = name;
                break;
            }
        }
        if (fname_module_.empty() && !exe_module_.empty()) {
            ULONG64 off = 0;
            if (SUCCEEDED(symbols_->GetOffsetByNameWide(Widen(exe_module_ + "!GNameBlocksDebug").c_str(), &off)) && off)
                fname_module_ = exe_module_;
        }
        if (fname_module_.empty()) return "";
    }
    const std::string& m = fname_module_;
    auto num = [&](const std::string& e, uint64_t* v) {
        std::string r = ShortValueRaw(e);
        if (r.empty() || r[0] == '<') return false;
        try {
            *v = std::stoull(r, nullptr, 0);
            return true;
        } catch (...) {
            return false;
        }
    };
    uint64_t id = 0;
    if (!num(obj + ".DisplayIndex.Value", &id) && !num(obj + ".ComparisonIndex.Value", &id)) return "";
    auto entry = [&](uint64_t i) {
        return "((FNameEntry*)&" + m + "!GNameBlocksDebug[" + std::to_string(i) + " >> " + m +
               "!FNameDebugVisualizer::OffsetBits][" + m + "!FNameDebugVisualizer::EntryStride * (" +
               std::to_string(i) + " & " + m + "!FNameDebugVisualizer::OffsetMask)])";
    };
    std::string suffix;
    uint64_t number = 0;
    if (num(obj + ".Number", &number) && number) suffix = "_" + std::to_string(number - 1);
    for (int depth = 0; depth < 2; ++depth) {
        std::string e = entry(id);
        uint64_t len = 0, wide = 0;
        if (!num(e + "->Header.Len", &len)) return "";
        if (len == 0) {  // numbered name: points at the base entry + number
            uint64_t base = 0, n2 = 0;
            if (!num("*(unsigned int*)" + e + "->NumberedName.Id", &base)) return "";
            if (num("*(unsigned int*)" + e + "->NumberedName.Number", &n2) && n2) suffix = "_" + std::to_string(n2 - 1);
            id = base;
            continue;
        }
        if (len > 1024) return "";
        num(e + "->Header.bIsWide", &wide);
        std::string out = Exec("dx -r0 " + e + (wide ? "->WideName,[" : "->AnsiName,[") + std::to_string(len) + (wide ? "]su" : "]s"));
        size_t q1 = out.find(" : \"");
        size_t q2 = out.rfind("\" [Type:");
        if (q1 == std::string::npos || q2 == std::string::npos || q2 < q1 + 4) return "";
        return out.substr(q1 + 4, q2 - q1 - 4) + suffix;
    }
    return "";
}

// Like ShortValue but without type-specific decoding (avoids recursion).
std::string Debugger::ShortValueRaw(const std::string& expr) {
    HRESULT hr;
    std::string out = Exec("dx -r0 " + expr, &hr);
    auto lines = SplitLines(out);
    if (FAILED(hr) || lines.empty() || lines[0].rfind("Error", 0) == 0) return "<error>";
    size_t colon = lines[0].find(" : ");
    if (colon == std::string::npos) return "<aggregate>";
    std::string v = lines[0].substr(colon + 3);
    size_t t = v.rfind(" [Type: ");
    if (t != std::string::npos) v = v.substr(0, t);
    return Trim(v);
}
