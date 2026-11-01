// Tool operations for Debugger. Each public method validates arguments on the caller
// thread and does the DbgEng work on the engine thread via Invoke().
#include <algorithm>
#include <cstring>
#include <regex>
#include <set>
#include <sstream>

#include "args.h"
#include "engine.h"

#include <tlhelp32.h>

#ifndef STACK_FRAME_TYPE_INLINE
#define STACK_FRAME_TYPE_INLINE 0x02
#endif

using namespace args;

namespace {

constexpr int kDefaultWaitMs = 30000;

bool IsSourceLine(const std::string& loc, std::string* file, ULONG* line) {
    static const std::regex re(
        R"(^\s*(.+?\.(c|cc|cpp|cxx|c\+\+|h|hh|hpp|hxx|inl|ipp|ixx|cppm|tpp|asm))\s*[:@](\d+)\s*$)",
        std::regex::icase);
    std::smatch m;
    if (!std::regex_match(loc, m, re)) return false;
    *file = m[1].str();
    *line = (ULONG)std::stoul(m[3].str());
    return true;
}

bool IsRawAddress(const std::string& loc) {
    static const std::regex re(R"(^\s*(0x[0-9a-fA-F`]+|[0-9a-fA-F]{8}`?[0-9a-fA-F]{8})\s*$)");
    return std::regex_match(loc, re);
}

const std::set<std::string>& SystemModules() {
    static const std::set<std::string> s = {
        "ntdll", "kernelbase", "kernel32", "ucrtbase", "ucrtbased", "msvcp140", "msvcp140d",
        "vcruntime140", "vcruntime140d", "vcruntime140_1", "vcruntime140_1d", "user32", "win32u",
        "combase", "rpcrt4", "sechost", "advapi32", "gdi32", "gdi32full", "ws2_32", "mswsock",
        "dxgi", "d3d11", "d3d12", "dbghelp", "msvcrt", "shell32", "ole32", "cfgmgr32"};
    return s;
}

std::string ModuleOf(const std::string& sym) {
    size_t bang = sym.find('!');
    return bang == std::string::npos ? "" : ToLower(sym.substr(0, bang));
}

std::string FormatDebugValue(IDebugControl7* control, const DEBUG_VALUE& v) {
    char buf[256];
    switch (v.Type) {
        case DEBUG_VALUE_INT8: snprintf(buf, sizeof(buf), "0x%02x", v.I8); return buf;
        case DEBUG_VALUE_INT16: snprintf(buf, sizeof(buf), "0x%04x", v.I16); return buf;
        case DEBUG_VALUE_INT32: snprintf(buf, sizeof(buf), "0x%08lx", v.I32); return buf;
        case DEBUG_VALUE_INT64: snprintf(buf, sizeof(buf), "0x%016llx", v.I64); return buf;
        case DEBUG_VALUE_FLOAT32: snprintf(buf, sizeof(buf), "%.9g", v.F32); return buf;
        case DEBUG_VALUE_FLOAT64: snprintf(buf, sizeof(buf), "%.17g", v.F64); return buf;
        case DEBUG_VALUE_FLOAT80: {
            DEBUG_VALUE out = {};
            if (SUCCEEDED(control->CoerceValue(const_cast<DEBUG_VALUE*>(&v), DEBUG_VALUE_FLOAT64, &out))) {
                snprintf(buf, sizeof(buf), "%.17g", out.F64);
                return buf;
            }
            return "?";
        }
        case DEBUG_VALUE_VECTOR64:
        case DEBUG_VALUE_VECTOR128: {
            int n = v.Type == DEBUG_VALUE_VECTOR64 ? 8 : 16;
            std::string s = "0x";
            for (int i = n - 1; i >= 0; --i) {
                snprintf(buf, sizeof(buf), "%02x", v.VI8[i]);
                s += buf;
            }
            if (n == 16) {
                snprintf(buf, sizeof(buf), " (f32: %g %g %g %g | f64: %g %g)", v.VF32[0],
                         v.VF32[1], v.VF32[2], v.VF32[3], v.VF64[0],
                         v.VF64[1]);
                s += buf;
            }
            return s;
        }
        default: return "?";
    }
}

std::wstring BuildEnvBlock(const json& env) {
    // Start from our environment, override/extend with the provided map (null removes).
    std::map<std::wstring, std::wstring, std::less<>> vars;
    std::vector<std::wstring> order;
    wchar_t* block = GetEnvironmentStringsW();
    for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
        std::wstring kv = p;
        size_t eq = kv.find(L'=', 1);
        if (eq == std::wstring::npos) continue;
        vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    FreeEnvironmentStringsW(block);
    auto find_ci = [&](const std::wstring& k) {
        for (auto it = vars.begin(); it != vars.end(); ++it)
            if (_wcsicmp(it->first.c_str(), k.c_str()) == 0) return it;
        return vars.end();
    };
    for (auto& [k, v] : env.items()) {
        std::wstring wk = Widen(k);
        auto it = find_ci(wk);
        if (it != vars.end()) vars.erase(it);
        if (!v.is_null()) vars[wk] = Widen(v.is_string() ? v.get<std::string>() : v.dump());
    }
    std::wstring out;
    for (auto& [k, v] : vars) {
        out += k + L"=" + v;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

std::string DefaultSymbolPath() {
    char env[8192];
    if (GetEnvironmentVariableA("DEBUGMCP_SYMBOL_PATH", env, sizeof(env))) return env;
    if (GetEnvironmentVariableA("_NT_SYMBOL_PATH", env, sizeof(env))) return env;
    std::string cache = DataDir() + "\\symcache";
    return "cache*" + cache + ";srv*" + cache + "*https://msdl.microsoft.com/download/symbols";
}

std::string DirOf(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? "" : path.substr(0, s);
}

}  // namespace

// ======================= helpers needing members =======================

void Debugger::SetSymbolAndSourcePaths(const json& a) {
    std::string sp = GetStr(a, "symbol_path");
    if (sp.empty()) sp = DefaultSymbolPath();
    std::string prog = GetStr(a, "program");
    if (!prog.empty()) {
        std::string d = DirOf(prog);
        if (!d.empty()) sp = d + ";" + sp;
    }
    symbols_->SetSymbolPathWide(Widen(sp).c_str());
    std::string src = GetStr(a, "source_path");
    if (!src.empty()) symbols_->SetSourcePathWide(Widen(src).c_str());
}

void Debugger::LoadNatvisList(const json& a) {
    auto it = a.find("natvis");
    if (it == a.end() || it->is_null()) return;
    auto add = [&](const std::string& in) {
        std::string f = FullPath(in);
        if (std::find(natvis_files_.begin(), natvis_files_.end(), f) == natvis_files_.end())
            natvis_files_.push_back(f);
    };
    if (it->is_string())
        add(it->get<std::string>());
    else if (it->is_array())
        for (auto& f : *it)
            if (f.is_string()) add(f.get<std::string>());
}

void Debugger::RemoveEngineBreakpoint(BreakpointSpec& b) {
    if (b.engine_id == DEBUG_ANY_ID) return;
    // engine_id holds the first id; extra ids for multi-location bps live in engine_to_bp_.
    std::vector<ULONG> ids;
    for (auto& [eid, id] : engine_to_bp_)
        if (id == b.id) ids.push_back(eid);
    for (ULONG eid : ids) {
        IDebugBreakpoint2* bp = nullptr;
        if (session_ && SUCCEEDED(control_->GetBreakpointById2(eid, &bp))) control_->RemoveBreakpoint2(bp);
        engine_to_bp_.erase(eid);
    }
    b.engine_id = DEBUG_ANY_ID;
}

static void ClassifyLocation(BreakpointSpec& b) {
    b.src_file.clear();
    b.module_hint.clear();
    b.src_line = 0;
    b.is_function = false;
    if (b.kind != "code") return;
    std::string file;
    ULONG line = 0;
    if (IsSourceLine(b.location, &file, &line)) {
        size_t bang = file.find('!');
        if (bang != std::string::npos && file.find_first_of("\\/") > bang) {
            b.module_hint = NormalizeModuleName(Trim(file.substr(0, bang)));
            file = file.substr(bang + 1);
        }
        b.src_file = Trim(file);
        std::replace(b.src_file.begin(), b.src_file.end(), '/', '\\');
        b.src_line = line;
    } else if (!IsRawAddress(b.location) && b.location.find_first_of("+@`") == std::string::npos) {
        b.is_function = true;
    }
}

std::vector<std::pair<std::string, ULONG>> Debugger::UserModules() {
    // (module name, symbol type) for every loaded module that is not part of Windows.
    std::vector<std::pair<std::string, ULONG>> out;
    ULONG loaded = 0, unloaded = 0;
    symbols_->GetNumberModules(&loaded, &unloaded);
    std::vector<DEBUG_MODULE_PARAMETERS> params(loaded);
    if (!loaded || FAILED(symbols_->GetModuleParameters(loaded, nullptr, 0, params.data()))) return out;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::string win = ToLower(Narrow(windir));
    for (ULONG i = 0; i < loaded; ++i) {
        wchar_t name[512] = {}, image[1024] = {};
        symbols_->GetModuleNameStringWide(DEBUG_MODNAME_MODULE, i, 0, name, 512, nullptr);
        symbols_->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, i, 0, image, 1024, nullptr);
        if (ToLower(Narrow(image)).rfind(win, 0) == 0) continue;
        out.push_back({Narrow(name), params[i].SymbolType});
    }
    return out;
}

void Debugger::LoadUserModuleSymbols(const std::string& source_path_hint) {
    // Load PDBs for non-system modules whose symbols are still deferred so that source-line and
    // unqualified-name lookups can see them. With a source path hint, only modules whose name
    // matches a directory of the path are loaded (UE: ...\Runtime\Core\Private\X.cpp ->
    // UnrealEditor_Core). VS loads every PDB eagerly; we avoid that unless we have to.
    std::vector<std::string> comps;
    if (!source_path_hint.empty()) {
        std::string cur;
        for (char c : source_path_hint) {
            if (c == '\\' || c == '/') {
                if (!cur.empty()) comps.push_back(ToLower(NormalizeModuleName(cur)));
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (comps.empty()) return;
    }
    for (auto& [name, symtype] : UserModules()) {
        if (symtype != DEBUG_SYMTYPE_DEFERRED) continue;
        if (!comps.empty()) {
            std::string n = ToLower(name);
            bool match = false;
            for (auto& c : comps) {
                if (c.size() < 3) continue;
                if (n == c || (n.size() > c.size() && n.compare(n.size() - c.size(), c.size(), c) == 0 &&
                               n[n.size() - c.size() - 1] == '_'))
                    match = true;
            }
            if (!match) continue;
        }
        Exec("ld " + name);
    }
}

std::vector<uint64_t> Debugger::FindFunctionAddresses(const std::string& location) {
    std::string loc = NormalizeModuleQualifiers(Trim(location));
    size_t bang = loc.find('!');
    std::string want = bang == std::string::npos ? loc : loc.substr(bang + 1);
    bool wildcard = want.find_first_of("*?") != std::string::npos;
    std::vector<std::string> modules;
    if (bang != std::string::npos) {
        modules.push_back(loc.substr(0, bang));
        Exec("ld " + modules[0]);
    } else {
        LoadUserModuleSymbols();
        for (auto& [name, symtype] : UserModules())
            if (symtype == DEBUG_SYMTYPE_PDB || symtype == DEBUG_SYMTYPE_DIA) modules.push_back(name);
    }
    std::vector<uint64_t> out;
    std::set<uint64_t> seen;
    std::vector<std::string> hit_modules;
    // Pass 1 (fast): symbol match; keep only real code (has line info), which drops import thunks.
    for (auto& mod : modules) {
        ULONG64 handle = 0;
        if (FAILED(symbols_->StartSymbolMatchWide(Widen(mod + "!" + want).c_str(), &handle))) continue;
        bool hit = false;
        for (;;) {
            wchar_t name[2048];
            ULONG64 off = 0;
            if (symbols_->GetNextSymbolMatchWide(handle, name, 2048, nullptr, &off) != S_OK) break;
            std::string n = Narrow(name);
            size_t nb = n.find('!');
            std::string bare = nb == std::string::npos ? n : n.substr(nb + 1);
            if (!wildcard && bare != want) continue;
            std::string f;
            ULONG l = 0;
            if (!LineAt(off, 0, &f, &l)) continue;
            hit = true;
            if (seen.insert(off).second) out.push_back(off);
            if (out.size() >= 64) break;
        }
        symbols_->EndSymbolMatch(handle);
        if (hit) hit_modules.push_back(mod);
    }
    // Pass 2 (slower): inline copies, only in modules that define the function.
    for (size_t m = 0; m < hit_modules.size() && m < 16 && out.size() < 64; ++m) {
        for (auto& line : SplitLines(Exec("x /v " + hit_modules[m] + "!" + want))) {
            std::istringstream ls(line);
            std::string scope, kind, addr_s, size_s;
            ls >> scope >> kind >> addr_s >> size_s;
            if (kind != "inline") continue;
            std::string rest;
            std::getline(ls, rest);
            rest = Trim(rest);
            std::string name = rest.substr(0, std::min(rest.find(" ("), rest.find(" =")));
            size_t nb = name.find('!');
            std::string bare = nb == std::string::npos ? name : name.substr(nb + 1);
            if (!wildcard && bare != want) continue;
            addr_s.erase(std::remove(addr_s.begin(), addr_s.end(), '`'), addr_s.end());
            uint64_t off = 0;
            try {
                off = std::stoull(addr_s, nullptr, 16);
            } catch (...) {
                continue;
            }
            inline_sites_.insert(off);
            if (seen.insert(off).second) out.push_back(off);
            if (out.size() >= 64) break;
        }
    }
    return out;
}

bool Debugger::BindSourceLine(BreakpointSpec& b, bool allow_symbol_load) {
    auto query = [&](std::vector<DEBUG_SYMBOL_SOURCE_ENTRY>* out) {
        // GetSourceEntriesByLine needs the path as recorded in the PDB; GetOffsetByLine accepts a
        // partial path, so use it to discover the full path first.
        std::wstring wf = Widen(b.src_file);
        ULONG64 off = 0;
        if (FAILED(symbols_->GetOffsetByLineWide(b.src_line, wf.c_str(), &off))) return false;
        std::string full;
        ULONG l = 0;
        if (LineAt(off, 0, &full, &l)) wf = Widen(full);
        ULONG n = 0;
        if (FAILED(symbols_->GetSourceEntriesByLineWide(b.src_line, wf.c_str(), DEBUG_GSEL_ALLOW_HIGHER,
                                                        nullptr, 0, &n)) || n == 0) {
            DEBUG_SYMBOL_SOURCE_ENTRY e = {};
            e.Offset = off;
            e.StartLine = l;
            out->assign(1, e);
            return true;
        }
        out->resize(n);
        symbols_->GetSourceEntriesByLineWide(b.src_line, wf.c_str(), DEBUG_GSEL_ALLOW_HIGHER, out->data(), n, &n);
        out->resize(n);
        return n > 0;
    };
    std::vector<DEBUG_SYMBOL_SOURCE_ENTRY> entries;
    bool found = query(&entries);
    if (!found && allow_symbol_load) {
        if (!b.module_hint.empty()) {
            Exec("ld " + b.module_hint);
        } else {
            LoadUserModuleSymbols(b.src_file);  // modules named like a directory of the path
            found = query(&entries);
            if (!found) LoadUserModuleSymbols();  // everything (slow on huge apps)
        }
        if (!found) found = query(&entries);
    }
    if (!found) return false;
    // Only keep the nearest line with code (DEBUG_GSEL_ALLOW_HIGHER may return later lines).
    ULONG best = ~0u;
    for (auto& e : entries) best = std::min(best, e.StartLine);
    std::set<uint64_t> seen;
    for (auto& e : entries) {
        if (e.StartLine != best || !seen.insert(e.Offset).second) continue;
        if (!b.module_hint.empty() && !IEquals(ModuleNameAt(e.Offset), b.module_hint)) continue;
        if (seen.size() > 64) break;
        IDebugBreakpoint2* bp = nullptr;
        if (FAILED(control_->AddBreakpoint2(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp))) continue;
        bp->SetOffset(e.Offset);
        if (b.enabled) bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
        ULONG eid = 0;
        bp->GetId(&eid);
        if (b.engine_id == DEBUG_ANY_ID) b.engine_id = eid;
        engine_to_bp_[eid] = b.id;
    }
    // If the function containing the line was inlined elsewhere, also break at each inline site
    // (approximate: the start of the inlined body; exact line mapping inside inlined code needs
    // inline-context line tables, which DbgEng does not expose for breakpoints).
    std::set<std::string> fns;
    for (uint64_t off : seen) {
        std::string fn = SymbolAt(off, 0);
        if (!fn.empty() && !inline_sites_.count(off)) fns.insert(fn);
        if (fns.size() >= 8) break;
    }
    for (auto& fn : fns) {
        {
            for (uint64_t site : FindFunctionAddresses(fn)) {
                if (!inline_sites_.count(site) || seen.count(site)) continue;
                IDebugBreakpoint2* bp = nullptr;
                if (FAILED(control_->AddBreakpoint2(DEBUG_BREAKPOINT_CODE, DEBUG_ANY_ID, &bp))) continue;
                bp->SetOffset(site);
                if (b.enabled) bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
                ULONG eid = 0;
                bp->GetId(&eid);
                engine_to_bp_[eid] = b.id;
            }
        }
    }
    return b.engine_id != DEBUG_ANY_ID;
}

bool Debugger::ApplyBreakpoint(BreakpointSpec& b) {
    b.last_error.clear();
    RemoveEngineBreakpoint(b);
    ClassifyLocation(b);
    b.relocated = false;
    auto add_bp = [&](ULONG type, IDebugBreakpoint2** out) {
        HRESULT hr = control_->AddBreakpoint2(type, DEBUG_ANY_ID, out);
        if (FAILED(hr)) throw ToolError("AddBreakpoint failed: " + HResultText(hr));
    };
    auto finish = [&](IDebugBreakpoint2* bp) {
        if (b.enabled) bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
        ULONG eid = 0;
        bp->GetId(&eid);
        if (b.engine_id == DEBUG_ANY_ID) b.engine_id = eid;
        engine_to_bp_[eid] = b.id;
    };
    try {
        if (b.kind == "data") {
            uint64_t addr = EvalAddress(b.location);
            ULONG access = b.data_access == "read"        ? DEBUG_BREAK_READ
                           : b.data_access == "readwrite" ? DEBUG_BREAK_READ | DEBUG_BREAK_WRITE
                           : b.data_access == "execute"   ? DEBUG_BREAK_EXECUTE
                                                          : DEBUG_BREAK_WRITE;
            if (b.data_access == "readwrite") access = DEBUG_BREAK_READ;  // x64 "read" traps r/w
            IDebugBreakpoint2* bp = nullptr;
            add_bp(DEBUG_BREAKPOINT_DATA, &bp);
            HRESULT hr = bp->SetDataParameters((ULONG)b.data_size, access);
            if (SUCCEEDED(hr)) hr = bp->SetOffset(addr);
            if (FAILED(hr)) {
                control_->RemoveBreakpoint2(bp);
                throw ToolError("invalid data breakpoint (size must be 1/2/4/8 and aligned): " +
                                HResultText(hr));
            }
            finish(bp);
            return true;
        }
        if (!b.src_file.empty()) {
            // Source lines are bound by us (all instantiations); unbound ones stay pending and are
            // retried on every module load (see OnLoadModuleEvent).
            BindSourceLine(b, true);
            return true;
        }
        if (b.is_function) {
            // Bind every match (overloads, inlined copies, wildcard patterns) like VS / 'bm /a'.
            auto addrs = FindFunctionAddresses(b.location);
            if (!addrs.empty()) {
                for (uint64_t addr : addrs) {
                    uint64_t disp = 1;
                    if (!b.internal && !SymbolAt(addr, 0, &disp).empty() && disp == 0) addr = SkipPrologue(addr);
                    IDebugBreakpoint2* bp = nullptr;
                    add_bp(DEBUG_BREAKPOINT_CODE, &bp);
                    bp->SetOffset(addr);
                    finish(bp);
                }
                b.relocated = true;
                return true;
            }
        }
        IDebugBreakpoint2* bp = nullptr;
        add_bp(DEBUG_BREAKPOINT_CODE, &bp);
        HRESULT hr;
        if (IsRawAddress(b.location))
            hr = bp->SetOffset(EvalAddress(b.location));
        else
            hr = bp->SetOffsetExpressionWide(Widen(ToOffsetExpression(b.location)).c_str());
        if (FAILED(hr)) {
            control_->RemoveBreakpoint2(bp);
            throw ToolError("cannot set breakpoint at '" + b.location + "': " + HResultText(hr));
        }
        finish(bp);
        if (b.is_function) {
            ULONG flags = 0;
            bp->GetFlags(&flags);
            if (flags & DEBUG_BREAKPOINT_DEFERRED) {
                // Deferred symbols never resolve for modules that are already loaded; load them.
                size_t bang = b.location.find('!');
                if (bang != std::string::npos)
                    Exec("ld " + NormalizeModuleName(Trim(b.location.substr(0, bang))));
                else
                    LoadUserModuleSymbols();
                bp->SetOffsetExpressionWide(Widen(ToOffsetExpression(b.location)).c_str());
            }
        }
        if (b.is_function && !b.internal) {
            ULONG flags = 0;
            ULONG64 off = 0;
            bp->GetFlags(&flags);
            uint64_t disp = 1;
            if (!(flags & DEBUG_BREAKPOINT_DEFERRED) && SUCCEEDED(bp->GetOffset(&off)) &&
                !SymbolAt(off, 0, &disp).empty() && disp == 0) {
                uint64_t moved = SkipPrologue(off);
                if (moved != off) bp->SetOffset(moved);
                b.relocated = true;
            }
        }
        return true;
    } catch (const ToolError& e) {
        b.last_error = e.what();
        return false;
    }
}

void Debugger::ApplyAllBreakpoints() {
    for (auto& [id, b] : bps_)
        if (!b.internal) ApplyBreakpoint(b);
}

json Debugger::BreakpointJson(BreakpointSpec& b) {
    json j = {{"id", b.id}, {"location", b.location}, {"kind", b.kind}, {"enabled", b.enabled}};
    if (!b.condition.empty()) j["condition"] = b.condition;
    if (!b.hit_condition.empty()) j["hit_condition"] = b.hit_condition;
    if (!b.log_message.empty()) {
        j["log_message"] = b.log_message;
        j["log_and_break"] = b.log_and_break;
    }
    if (b.one_shot) j["one_shot"] = true;
    if (b.kind == "data") {
        j["data_size"] = b.data_size;
        j["data_access"] = b.data_access;
    }
    j["hits"] = b.hits;
    if (!b.last_error.empty()) j["error"] = b.last_error;
    if (!session_) {
        j["status"] = "pending (no session; applied on launch/attach)";
        return j;
    }
    json locs = json::array();
    bool deferred = false;
    for (auto& [eid, id] : engine_to_bp_) {
        if (id != b.id) continue;
        IDebugBreakpoint2* bp = nullptr;
        if (FAILED(control_->GetBreakpointById2(eid, &bp))) continue;
        ULONG flags = 0;
        bp->GetFlags(&flags);
        ULONG64 off = 0;
        if ((flags & DEBUG_BREAKPOINT_DEFERRED) || FAILED(bp->GetOffset(&off))) {
            deferred = true;
            continue;
        }
        json l = {{"address", Hex(off)}};
        if (inline_sites_.count(off)) l["inline_site"] = "start of an inlined copy (approximate for source lines)";
        std::string fn = SymbolAt(off, 0);
        if (!fn.empty()) l["function"] = fn;
        std::string file;
        ULONG line;
        if (LineAt(off, 0, &file, &line)) l["at"] = file + ":" + std::to_string(line);
        locs.push_back(l);
    }
    if (!locs.empty()) {
        j["status"] = "bound";
        j["locations"] = locs;
    } else if (deferred || (!b.src_file.empty() && b.last_error.empty())) {
        j["status"] = "pending (module/symbol not loaded yet; binds automatically on load)";
    } else {
        j["status"] = b.last_error.empty() ? "unbound" : "error";
    }
    return j;
}

// ======================= Session tools =======================

json Debugger::Launch(const json& a) {
    std::string program = RequireStr(a, "program");
    std::string stop_at = ToLower(GetStr(a, "stop_at", "main"));
    if (stop_at != "main" && stop_at != "entry" && stop_at != "none")
        throw ToolError("stop_at must be main, entry or none");
    int timeout = GetInt(a, "timeout_ms", kDefaultWaitMs);
    uint64_t seq = 0;
    json pre = Invoke([&]() -> json {
        if (session_)
            throw ToolError("a debug session is already active (pid " + std::to_string(pid_) +
                            "); call terminate or detach first, or use restart");
        std::string cmd = QuoteArg(program);
        auto ait = a.find("args");
        if (ait != a.end() && ait->is_array()) {
            for (auto& x : *ait) cmd += " " + QuoteArg(x.is_string() ? x.get<std::string>() : x.dump());
        } else if (ait != a.end() && ait->is_string() && !ait->get<std::string>().empty()) {
            cmd += " " + ait->get<std::string>();
        }
        std::string cwd = GetStr(a, "cwd", DirOf(program));
        bool new_console = GetBool(a, "new_console", false);
        SetSymbolAndSourcePaths(a);
        LoadNatvisList(a);

        std::wstring env;
        auto eit = a.find("env");
        bool has_env = eit != a.end() && eit->is_object() && !eit->empty();
        if (has_env) env = BuildEnvBlock(*eit);

        DEBUG_CREATE_PROCESS_OPTIONS opt = {};
        opt.CreateFlags = DEBUG_ONLY_THIS_PROCESS | (new_console ? CREATE_NEW_CONSOLE : 0) |
                          (has_env ? CREATE_UNICODE_ENVIRONMENT : 0);
        opt.EngCreateFlags = new_console ? 0 : DEBUG_ECREATE_PROCESS_INHERIT_HANDLES;
        std::wstring wcmd = Widen(cmd);
        std::wstring wcwd = Widen(cwd);
        out_cursor_ = debuggee_out_->NextSeq();
        trace_cursor_ = trace_log_.NextSeq();
        pending_exit_ = false;
        HRESULT hr = client_->CreateProcess2Wide(0, wcmd.data(), &opt, sizeof(opt),
                                                 wcwd.empty() ? nullptr : wcwd.c_str(),
                                                 has_env ? env.c_str() : nullptr);
        if (FAILED(hr))
            throw ToolError("failed to start '" + cmd + "': " + HResultText(hr));
        hr = control_->WaitForEvent(0, 60000);
        if (hr != S_OK || pending_exit_) {
            client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
            throw ToolError("process did not reach its initial breakpoint: " + HResultText(hr));
        }
        BeginSession(true, false);
        last_launch_args_ = a;
        target_desc_ = "launched " + cmd;
        ApplyAllBreakpoints();

        {
            std::lock_guard<std::mutex> lk(state_m_);
            seq = stop_seq_;
        }
        auto record_stop = [&](const std::string& reason, const std::string& note) {
            std::lock_guard<std::mutex> lk(state_m_);
            last_stop_ = StopInfo{};
            last_stop_.reason = reason;
            last_stop_.note = note;
            ++stop_seq_;
            stop_cv_.notify_all();
        };
        if (stop_at == "entry") {
            record_stop("entry", "stopped at the loader's initial breakpoint (before CRT init and main)");
        } else if (stop_at == "main") {
            uint64_t addr = 0;
            std::string found;
            for (const char* name : {"main", "wmain", "WinMain", "wWinMain"}) {
                ULONG64 off = 0;
                std::string full = exe_module_ + "!" + name;
                if (SUCCEEDED(symbols_->GetOffsetByNameWide(Widen(full).c_str(), &off)) && off) {
                    addr = off;
                    found = full;
                    break;
                }
            }
            if (addr) addr = SkipPrologue(addr);
            if (!addr) {
                record_stop("entry",
                            "could not find main/wmain/WinMain/wWinMain in " + exe_module_ +
                                " (no symbols?); stopped at the initial breakpoint instead");
            } else {
                BreakpointSpec t;
                t.id = next_bp_id_++;
                t.location = Hex(addr);
                t.internal = true;
                t.internal_tag = "main";
                if (!ApplyBreakpoint(t)) throw ToolError("failed to set entry breakpoint: " + t.last_error);
                bps_[t.id] = t;
                ResumeTarget(DEBUG_STATUS_GO, "continue");
            }
        } else {
            ResumeTarget(DEBUG_STATUS_GO, "continue");
        }
        return nullptr;
    });
    json r = WaitForStop(seq, timeout, GetInt(a, "frames", 8));
    r["pid"] = pid_.load();
    return r;
}

json Debugger::Attach(const json& a) {
    int timeout = GetInt(a, "timeout_ms", kDefaultWaitMs);
    uint32_t pid = (uint32_t)GetInt(a, "pid", 0);
    std::string name = GetStr(a, "process_name");
    if (!pid && name.empty()) throw ToolError("provide pid or process_name (see list_processes)");
    if (!pid) {
        std::vector<std::pair<uint32_t, std::string>> matches;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = {sizeof(pe)};
        std::string want = ToLower(name);
        if (!EndsWithI(want, ".exe")) want += ".exe";
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            std::string exe = Narrow(pe.szExeFile);
            if (ToLower(exe) == want) matches.push_back({pe.th32ProcessID, exe});
        }
        CloseHandle(snap);
        if (matches.empty()) throw ToolError("no running process named " + name);
        if (matches.size() > 1) {
            std::string list;
            for (auto& m : matches) list += " " + std::to_string(m.first);
            throw ToolError("multiple processes named " + name + "; pass pid. PIDs:" + list);
        }
        pid = matches[0].first;
    }
    bool brk = GetBool(a, "break", true);
    uint64_t seq = 0;
    Invoke([&]() -> json {
        if (session_) throw ToolError("a debug session is already active; detach or terminate first");
        SetSymbolAndSourcePaths(a);
        LoadNatvisList(a);
        out_cursor_ = debuggee_out_->NextSeq();
        trace_cursor_ = trace_log_.NextSeq();
        pending_exit_ = false;
        HRESULT hr = client_->AttachProcess(0, pid, DEBUG_ATTACH_DEFAULT);
        if (FAILED(hr))
            throw ToolError("attach to pid " + std::to_string(pid) + " failed: " + HResultText(hr) +
                            " (already being debugged? needs elevation?)");
        hr = control_->WaitForEvent(0, 60000);
        if (hr != S_OK) {
            client_->EndSession(DEBUG_END_ACTIVE_DETACH);
            throw ToolError("attach did not complete: " + HResultText(hr));
        }
        BeginSession(false, false);
        target_desc_ = "attached to pid " + std::to_string(pid);
        ApplyAllBreakpoints();
        {
            std::lock_guard<std::mutex> lk(state_m_);
            seq = stop_seq_;
        }
        if (brk) {
            ULONG ids[1], sys[1];
            if (SUCCEEDED(sysobj_->GetThreadIdsByIndex(0, 1, ids, sys))) sysobj_->SetCurrentThreadId(ids[0]);
            std::lock_guard<std::mutex> lk(state_m_);
            last_stop_ = StopInfo{};
            last_stop_.reason = "attach";
            last_stop_.note = "attached and broke in; showing the process's first thread";
            ++stop_seq_;
            stop_cv_.notify_all();
        } else {
            ResumeTarget(DEBUG_STATUS_GO, "continue");
        }
        return nullptr;
    });
    json r = WaitForStop(seq, brk ? timeout : 0, GetInt(a, "frames", 8));
    r["pid"] = pid;
    return r;
}

json Debugger::OpenDump(const json& a) {
    std::string path = RequireStr(a, "path");
    return Invoke([&]() -> json {
        if (session_) throw ToolError("a debug session is already active; detach or terminate first");
        SetSymbolAndSourcePaths(a);
        LoadNatvisList(a);
        HRESULT hr = client_->OpenDumpFileWide(Widen(path).c_str(), 0);
        if (FAILED(hr)) throw ToolError("cannot open dump: " + HResultText(hr));
        hr = control_->WaitForEvent(0, 300000);
        if (hr != S_OK) {
            client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
            throw ToolError("dump load failed: " + HResultText(hr));
        }
        BeginSession(false, true);
        target_desc_ = "dump " + path;
        StopInfo si;
        si.reason = "dump";
        ULONG type = 0, pid = 0, tid = 0;
        DEBUG_LAST_EVENT_INFO_EXCEPTION ex = {};
        if (SUCCEEDED(control_->GetLastEventInformation(&type, &pid, &tid, &ex, sizeof(ex), nullptr,
                                                        nullptr, 0, nullptr)) &&
            type == DEBUG_EVENT_EXCEPTION) {
            si.exception = DecodeException(ex.ExceptionRecord, ex.FirstChance != 0);
            HRESULT ehr;
            Exec(".ecxr", &ehr);
            si.note = SUCCEEDED(ehr) ? "context set to the exception record (.ecxr)" : "";
        }
        {
            std::lock_guard<std::mutex> lk(state_m_);
            last_stop_ = si;
            ++stop_seq_;
        }
        return BuildStopReport(GetInt(a, "frames", 16));
    });
}

json Debugger::Detach(const json&) {
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) {
            suppress_stop_ = true;
            BreakIn();
            control_->WaitForEvent(0, 5000);
            suppress_stop_ = false;
        }
        CleanupTempBreakpoints();
        HRESULT hr = client_->EndSession(is_dump_ ? DEBUG_END_ACTIVE_TERMINATE : DEBUG_END_ACTIVE_DETACH);
        {
            std::lock_guard<std::mutex> lk(state_m_);
            last_stop_ = StopInfo{};
            last_stop_.reason = "detached";
        }
        EndSessionState("detached");
        json j = {{"state", "no_session"}, {"detached", SUCCEEDED(hr)}};
        if (FAILED(hr)) j["error"] = HResultText(hr);
        return j;
    });
}

json Debugger::Terminate(const json&) {
    return Invoke([&]() -> json {
        RequireSession();
        HRESULT hr = client_->EndSession(DEBUG_END_ACTIVE_TERMINATE);
        {
            std::lock_guard<std::mutex> lk(state_m_);
            last_stop_ = StopInfo{};
            last_stop_.reason = "terminated";
        }
        EndSessionState("terminated");
        json j = {{"state", "no_session"}, {"terminated", SUCCEEDED(hr)}};
        if (FAILED(hr)) j["error"] = HResultText(hr);
        AttachNewOutput(j);
        return j;
    });
}

json Debugger::Restart(const json& a) {
    json launch = Invoke([&]() -> json { return last_launch_args_; });
    if (launch.is_null()) throw ToolError("nothing to restart (no previous launch)");
    for (auto& [k, v] : a.items()) launch[k] = v;
    if (session_) Terminate(json::object());
    return Launch(launch);
}

json Debugger::Status(const json& a) {
    return Invoke([&]() -> json {
        json j;
        if (!session_) {
            j = BuildStopReport(1);
        } else if (running_) {
            j = RunningReport();
        } else {
            j = BuildStopReport(GetInt(a, "frames", 5));
        }
        j["engine"] = engine_info_;
        if (session_) {
            j["target"] = target_desc_;
            if (stale_traps_) j["stale_breakpoint_traps_resumed"] = stale_traps_;
            j["pid"] = pid_.load();
            if (is_dump_) j["dump"] = true;
        }
        int n = 0;
        for (auto& [id, b] : bps_)
            if (!b.internal) ++n;
        j["breakpoints"] = n;
        return j;
    });
}

// ======================= Run control =======================

json Debugger::Continue(const json& a) {
    uint64_t seq = 0;
    Invoke([&]() -> json {
        RequireStopped();
        RequireLive();
        {
            std::lock_guard<std::mutex> lk(state_m_);
            seq = stop_seq_;
        }
        ResumeTarget(DEBUG_STATUS_GO, "continue");
        return nullptr;
    });
    return WaitForStop(seq, GetInt(a, "timeout_ms", kDefaultWaitMs), GetInt(a, "frames", 8));
}

json Debugger::Pause(const json& a) {
    // Runs on the caller thread: the engine thread may be busy inside WaitForEvent.
    if (!session_) throw ToolError("no active debug session (use launch, attach or open_dump first)");
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(state_m_);
        seq = stop_seq_;
    }
    if (!running_) {
        json r = Invoke([&]() -> json { return BuildStopReport(GetInt(a, "frames", 8)); });
        r["note"] = "target was already stopped";
        return r;
    }
    pause_requested_ = true;
    BreakIn();
    return WaitForStop(seq, GetInt(a, "timeout_ms", 10000), GetInt(a, "frames", 8));
}

json Debugger::Wait(const json& a) {
    uint64_t seq = 0;
    bool running = false;
    Invoke([&]() -> json {
        std::lock_guard<std::mutex> lk(state_m_);
        seq = stop_seq_;
        running = running_;
        return nullptr;
    });
    if (!running) return Invoke([&]() -> json { return BuildStopReport(GetInt(a, "frames", 8)); });
    return WaitForStop(seq, GetInt(a, "timeout_ms", kDefaultWaitMs), GetInt(a, "frames", 8));
}

json Debugger::Step(const json& a) {
    std::string kind = ToLower(GetStr(a, "kind", "over"));
    int count = std::max(1, GetInt(a, "count", 1));
    int timeout = GetInt(a, "timeout_ms", kDefaultWaitMs);
    int frames = GetInt(a, "frames", 5);
    json report;
    for (int i = 0; i < count; ++i) {
        uint64_t seq = 0;
        Invoke([&]() -> json {
            RequireStopped();
            RequireLive();
            {
                std::lock_guard<std::mutex> lk(state_m_);
                seq = stop_seq_;
            }
            if (kind != "over" && kind != "into" && kind != "out" && kind != "over_instruction" &&
                kind != "into_instruction")
                throw ToolError("kind must be over, into, out, over_instruction or into_instruction");
            bool instr = kind.find("instruction") != std::string::npos;
            // Steps are relative to the top frame (as in VS), not a frame picked with select_frame.
            ResetScopeToTop();
            if (instr) {
                control_->SetCodeLevel(DEBUG_LEVEL_ASSEMBLY);
                ResumeTarget(kind == "over_instruction" ? DEBUG_STATUS_STEP_OVER : DEBUG_STATUS_STEP_INTO, "step");
                return nullptr;
            }
            StepPos p = CurPos();
            ULONG tid = 0;
            sysobj_->GetCurrentThreadId(&tid);
            step_ = StepCtl{};
            step_.kind = kind;
            step_.tid = tid;
            step_.start = p;
            DiagLog("step %s from %s:%lu inline_depth=%d cfa=%llx ret=%llx", kind.c_str(), p.func.c_str(),
                    p.line, p.depth, (unsigned long long)p.cfa, (unsigned long long)p.ret);
            if (p.valid && (p.depth > 0 || !p.has_line)) {
                // In an inline frame (or code without line info): drive it instruction by
                // instruction (see StepCtl in engine.h).
                step_.active = step_.custom = true;
                control_->SetCodeLevel(DEBUG_LEVEL_ASSEMBLY);
                ResumeTarget(kind == "into" ? DEBUG_STATUS_STEP_INTO : DEBUG_STATUS_STEP_OVER, "step");
            } else if (kind == "out") {
                if (!p.valid || !p.ret) throw ToolError("cannot step out of the thread's outermost frame");
                step_.active = true;
                SetGuard(p.ret, p.cfa, tid);
                ResumeTarget(DEBUG_STATUS_GO, "step");
            } else {
                step_.active = true;
                if (p.valid) SetGuard(p.ret, p.cfa, tid);  // a native step never outruns its frame
                control_->SetCodeLevel(DEBUG_LEVEL_SOURCE);
                ResumeTarget(kind == "into" ? DEBUG_STATUS_STEP_INTO : DEBUG_STATUS_STEP_OVER, "step");
            }
            return nullptr;
        });
        report = WaitForStop(seq, timeout, frames);
        if (report.value("state", "") != "stopped" || report.value("reason", "") != "step") break;
    }
    return report;
}

json Debugger::RunTo(const json& a) {
    std::string loc = RequireStr(a, "location");
    uint64_t seq = 0;
    Invoke([&]() -> json {
        RequireStopped();
        RequireLive();
        BreakpointSpec t;
        t.id = next_bp_id_++;
        t.location = loc;
        t.internal = true;
        t.internal_tag = "run_to";
        if (!ApplyBreakpoint(t)) throw ToolError(t.last_error);
        bps_[t.id] = t;
        {
            std::lock_guard<std::mutex> lk(state_m_);
            seq = stop_seq_;
        }
        ResumeTarget(DEBUG_STATUS_GO, "continue");
        return nullptr;
    });
    return WaitForStop(seq, GetInt(a, "timeout_ms", kDefaultWaitMs), GetInt(a, "frames", 8));
}

// ======================= Breakpoints =======================

json Debugger::SetBreakpoint(const json& a) {
    BreakpointSpec b;
    b.location = RequireStr(a, "location");
    b.kind = ToLower(GetStr(a, "kind", "code"));
    if (b.kind != "code" && b.kind != "data") throw ToolError("kind must be code or data");
    b.condition = GetStr(a, "condition");
    b.hit_condition = GetStr(a, "hit_condition");
    b.log_message = GetStr(a, "log_message");
    b.log_and_break = GetBool(a, "log_and_break", false);
    b.one_shot = GetBool(a, "one_shot", false);
    b.enabled = GetBool(a, "enabled", true);
    b.data_size = GetInt(a, "data_size", 4);
    b.data_access = ToLower(GetStr(a, "data_access", "write"));
    return Invoke([&]() -> json {
        if (b.kind == "data" && !session_)
            throw ToolError("data breakpoints need a live session (the address must be known)");
        b.id = next_bp_id_++;
        if (session_) {
            WithTargetStopped([&]() -> json {
                ApplyBreakpoint(b);
                return nullptr;
            });
        }
        if (!b.last_error.empty() && b.kind == "data") throw ToolError(b.last_error);
        auto& stored = bps_[b.id] = b;
        return BreakpointJson(stored);
    });
}

json Debugger::RemoveBreakpoint(const json& a) {
    bool all = GetBool(a, "all", false);
    int id = GetInt(a, "id", 0);
    if (!all && !id) throw ToolError("provide id or all=true");
    return Invoke([&]() -> json {
        int removed = 0;
        auto body = [&]() -> json {
            for (auto it = bps_.begin(); it != bps_.end();) {
                if (!it->second.internal && (all || it->first == id)) {
                    RemoveEngineBreakpoint(it->second);
                    it = bps_.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            return nullptr;
        };
        if (session_) WithTargetStopped(body);
        else body();
        if (!removed && !all) throw ToolError("no breakpoint with id " + std::to_string(id));
        return json{{"removed", removed}};
    });
}

json Debugger::ListBreakpoints(const json&) {
    return Invoke([&]() -> json {
        json arr = json::array();
        for (auto& [id, b] : bps_)
            if (!b.internal) arr.push_back(BreakpointJson(b));
        return json{{"breakpoints", arr}};
    });
}

json Debugger::EnableBreakpoint(const json& a) {
    int id = GetInt(a, "id", 0);
    bool en = GetBool(a, "enabled", true);
    return Invoke([&]() -> json {
        auto it = bps_.find(id);
        if (it == bps_.end() || it->second.internal) throw ToolError("no breakpoint with id " + std::to_string(id));
        BreakpointSpec& b = it->second;
        b.enabled = en;
        if (session_) {
            WithTargetStopped([&]() -> json {
                for (auto& [eid, bid] : engine_to_bp_) {
                    if (bid != id) continue;
                    IDebugBreakpoint2* bp = nullptr;
                    if (SUCCEEDED(control_->GetBreakpointById2(eid, &bp))) {
                        if (en) bp->AddFlags(DEBUG_BREAKPOINT_ENABLED);
                        else bp->RemoveFlags(DEBUG_BREAKPOINT_ENABLED);
                    }
                }
                return nullptr;
            });
        }
        return BreakpointJson(b);
    });
}

// ======================= Threads / stacks =======================

json Debugger::Threads(const json& a) {
    bool with_loc = GetBool(a, "locations", true);
    return Invoke([&]() -> json {
        RequireStopped();
        ULONG n = 0;
        sysobj_->GetNumberThreads(&n);
        std::vector<ULONG> ids(n), sys(n);
        if (n) sysobj_->GetThreadIdsByIndex(0, n, ids.data(), sys.data());
        ULONG cur = 0;
        sysobj_->GetCurrentThreadId(&cur);
        ULONG event_tid = 0;
        sysobj_->GetEventThread(&event_tid);
        json arr = json::array();
        for (ULONG i = 0; i < n; ++i) {
            json t = {{"tid", sys[i]}};
            if (ids[i] == cur) t["current"] = true;
            std::string name = ThreadName(sys[i]);
            if (!name.empty()) t["name"] = name;
            if (with_loc) {
                sysobj_->SetCurrentThreadId(ids[i]);
                auto st = CurrentStack(24);
                std::string top, user;
                for (size_t k = 0; k < st.size(); ++k) {
                    std::string fn = SymbolAt(st[k].InstructionOffset, st[k].InlineFrameContext);
                    if (k == 0) top = fn;
                    if (user.empty() && !fn.empty() && !SystemModules().count(ModuleOf(fn))) {
                        user = fn;
                        std::string file;
                        ULONG line;
                        bool adjust = IsReturnAddressFrame(st, k);
                        if (LineAt(adjust ? st[k].InstructionOffset - 1 : st[k].InstructionOffset,
                                   st[k].InlineFrameContext, &file, &line)) {
                            size_t s = file.find_last_of("\\/");
                            user += "  " + file.substr(s == std::string::npos ? 0 : s + 1) + ":" +
                                    std::to_string(line);
                        }
                    }
                }
                t["top"] = top;
                if (!user.empty() && user.rfind(top, 0) != 0) t["user_frame"] = user;
            }
            arr.push_back(t);
        }
        sysobj_->SetCurrentThreadId(cur);
        return json{{"count", n}, {"threads", arr}};
    });
}

json Debugger::SelectThread(const json& a) {
    ULONG tid = (ULONG)GetInt(a, "tid", 0);
    return Invoke([&]() -> json {
        RequireStopped();
        SelectThreadSys(tid);
        auto st = CurrentStack(1);
        json j = {{"tid", tid}};
        std::string n = ThreadName(tid);
        if (!n.empty()) j["name"] = n;
        if (!st.empty()) j["location"] = FrameJson(st[0], false, true);
        return j;
    });
}

json Debugger::Stack(const json& a) {
    int max_frames = GetInt(a, "max_frames", 64);
    int start = GetInt(a, "start_frame", 0);
    bool all = GetBool(a, "all_threads", false);
    bool verbose = GetBool(a, "verbose", false);
    return Invoke([&]() -> json {
        RequireStopped();
        auto frame_line = [&](const std::vector<DEBUG_STACK_FRAME_EX>& st, size_t i, bool with_lines) {
            const auto& f = st[i];
            std::string fn = SymbolAt(f.InstructionOffset, f.InlineFrameContext);
            std::string s = fn.empty() ? Hex(f.InstructionOffset) : fn;
            if (with_lines) {
                std::string file;
                ULONG line;
                bool adjust = IsReturnAddressFrame(st, i);
                if (LineAt(adjust ? f.InstructionOffset - 1 : f.InstructionOffset, f.InlineFrameContext,
                           &file, &line))
                    s += "  " + file + ":" + std::to_string(line);
                if (IsInlineFrame(f)) s += "  [inline]";
            }
            return s;
        };
        if (!all) {
            ULONG prev = 0;
            sysobj_->GetCurrentThreadId(&prev);
            if (a.contains("tid") && !a["tid"].is_null()) SelectThreadSys((ULONG)GetInt(a, "tid", 0));
            ULONG tid = 0;
            sysobj_->GetCurrentThreadSystemId(&tid);
            auto st = CurrentStack((ULONG)(start + max_frames + 1));
            json frames = json::array();
            for (size_t i = (size_t)start; i < st.size() && (int)(i - start) < max_frames; ++i) {
                if (verbose) {
                    bool adjust = IsReturnAddressFrame(st, i);
                    json fj = FrameJson(st[i], adjust, false);
                    fj["sp"] = Hex(st[i].StackOffset);
                    fj["return"] = Hex(st[i].ReturnOffset);
                    frames.push_back(fj);
                } else {
                    frames.push_back("#" + std::to_string(st[i].FrameNumber) + " " + frame_line(st, i, true));
                }
            }
            json j = {{"tid", tid}, {"frames", frames}};
            if ((int)st.size() > start + max_frames) j["more"] = "use start_frame to page";
            if (a.contains("tid") && !GetBool(a, "select", false)) sysobj_->SetCurrentThreadId(prev);
            return j;
        }
        // All threads, grouped by identical call stacks (like VS Parallel Stacks).
        ULONG n = 0, cur = 0;
        sysobj_->GetNumberThreads(&n);
        sysobj_->GetCurrentThreadId(&cur);
        std::vector<ULONG> ids(n), sys(n);
        if (n) sysobj_->GetThreadIdsByIndex(0, n, ids.data(), sys.data());
        std::map<std::string, std::pair<std::vector<std::string>, json>> groups;
        std::vector<std::string> order;
        for (ULONG i = 0; i < n; ++i) {
            sysobj_->SetCurrentThreadId(ids[i]);
            auto st = CurrentStack((ULONG)max_frames);
            std::string key;
            std::vector<std::string> lines;
            for (size_t k = 0; k < st.size(); ++k) {
                std::string fl = frame_line(st, k, true);
                key += SymbolAt(st[k].InstructionOffset, st[k].InlineFrameContext) + "|";
                lines.push_back(fl);
            }
            auto& g = groups[key];
            if (g.first.empty()) {
                g.first = lines;
                g.second = json::array();
                order.push_back(key);
            }
            json tj = sys[i];
            std::string name = ThreadName(sys[i]);
            if (!name.empty()) tj = std::to_string(sys[i]) + " (" + name + ")";
            g.second.push_back(tj);
        }
        sysobj_->SetCurrentThreadId(cur);
        std::sort(order.begin(), order.end(), [&](const std::string& x, const std::string& y) {
            return groups[x].second.size() > groups[y].second.size();
        });
        json out = json::array();
        for (auto& k : order) out.push_back({{"threads", groups[k].second}, {"frames", groups[k].first}});
        return json{{"thread_count", n}, {"unique_stacks", out.size()}, {"groups", out}};
    });
}

json Debugger::SelectFrame(const json& a) {
    ULONG frame = (ULONG)GetInt(a, "frame", 0);
    return Invoke([&]() -> json {
        RequireStopped();
        if (a.contains("tid") && !a["tid"].is_null()) SelectThreadSys((ULONG)GetInt(a, "tid", 0));
        SelectFrameNum(frame);
        auto st = CurrentStack(frame + 1);
        for (size_t i = 0; i < st.size(); ++i)
            if (st[i].FrameNumber == frame) {
                bool adjust = IsReturnAddressFrame(st, i);
                return FrameJson(st[i], adjust, true);
            }
        return json{{"frame", frame}};
    });
}

// ======================= Data inspection =======================

json Debugger::Locals(const json& a) {
    int depth = std::clamp(GetInt(a, "depth", 1), 0, 6);
    size_t max_chars = (size_t)GetInt(a, "max_chars", 20000);
    return Invoke([&]() -> json {
        RequireStopped();
        ApplyThreadFrameArgs(a);
        std::string d = std::to_string(depth);
        HRESULT hr1, hr2;
        std::string params = Exec("dx -r" + d + " @$curframe.Parameters", &hr1);
        std::string locals = Exec("dx -r" + d + " @$curframe.LocalVariables", &hr2);
        auto strip_header = [](const std::string& s) {
            auto lines = SplitLines(s);
            std::string out;
            for (size_t i = 1; i < lines.size(); ++i) {
                std::string l = lines[i];
                if (l.rfind("    ", 0) == 0) l = l.substr(4);
                out += l + "\n";
            }
            return out;
        };
        json j;
        if (SUCCEEDED(hr1)) j["parameters"] = Clip(strip_header(params), max_chars / 2);
        if (SUCCEEDED(hr2)) j["locals"] = Clip(strip_header(locals), max_chars / 2);
        if (FAILED(hr1) && FAILED(hr2)) {
            // Fallback (no private symbols / data model unavailable).
            std::string dv = Exec("dv /t /V");
            j["locals"] = Clip(dv, max_chars);
        }
        j["hint"] = "Values use natvis. Use evaluate with depth for more detail, e.g. evaluate `obj,[10]` or `ptr->member`.";
        return j;
    });
}

json Debugger::Evaluate(const json& a) {
    std::string expr = NormalizeModuleQualifiers(RequireStr(a, "expression"));
    int depth = std::clamp(GetInt(a, "depth", 1), 0, 10);
    size_t max_chars = (size_t)GetInt(a, "max_chars", 20000);
    return Invoke([&]() -> json {
        RequireStopped();
        ApplyThreadFrameArgs(a);
        HRESULT hr;
        std::string out = Exec("dx -r" + std::to_string(depth) + " " + expr, &hr);
        std::string t = Trim(out);
        auto bad = [](const std::string& o) {
            return o.find("unqualified symbol") != std::string::npos || o.find("Couldn't resolve") != std::string::npos ||
                   Trim(o).rfind("Error", 0) == 0;
        };
        if (SUCCEEDED(hr) && !bad(t)) {
            json j = {{"result", Clip(t, max_chars)}};
            std::string first = SplitLines(t).empty() ? "" : SplitLines(t)[0];
            if (first.find("[Type: FName") != std::string::npos || first.find("[Type: const FName") != std::string::npos) {
                std::string v = ShortValue(expr);
                if (!v.empty() && v[0] == '"') j["value"] = v;
            }
            return j;
        }
        if (t.find("unqualified symbol") != std::string::npos) t = "symbol not found";
        // Fall back to the classic C++ evaluator, then MASM.
        HRESULT hr2;
        std::string out2 = Exec("?? " + expr, &hr2);
        if (SUCCEEDED(hr2) && Trim(out2).find("Couldn't resolve") == std::string::npos &&
            out2.find("unqualified symbol") == std::string::npos && Trim(out2).rfind("Error", 0) != 0)
            return json{{"result", Clip(Trim(out2), max_chars)}, {"evaluator", "c++ (??)"}};
        uint64_t v = 0;
        std::string err;
        if (TryEvalNumber(expr, false, &v, &err))
            return json{{"result", Hex(v) + " (" + std::to_string((long long)v) + ")"},
                        {"evaluator", "masm"}};
        std::string msg = Trim(t);
        if (msg.rfind("Error:", 0) == 0) msg = Trim(msg.substr(6));
        throw ToolError("cannot evaluate '" + expr + "' in the current frame: " + Clip(msg, 500) +
                        " (check the frame with stack/select_frame; globals may need module!name)");
    });
}

json Debugger::SetVariable(const json& a) {
    std::string expr = RequireStr(a, "expression");
    std::string value = RequireStr(a, "value");
    return Invoke([&]() -> json {
        RequireStopped();
        ApplyThreadFrameArgs(a);
        HRESULT hr;
        std::string out = Exec("dx " + expr + " = " + value, &hr);
        if (FAILED(hr) || Trim(out).rfind("Error", 0) == 0) {
            std::string out2 = Exec("?? " + expr + " = " + value, &hr);
            if (FAILED(hr) || Trim(out2).rfind("Error", 0) == 0 ||
                out2.find("Couldn't resolve") != std::string::npos)
                throw ToolError("assignment failed: " + Trim(out) + " / " + Trim(out2));
        }
        return json{{"expression", expr}, {"new_value", ShortValue(expr)}};
    });
}

json Debugger::Watch(const json& a) {
    std::string action = ToLower(GetStr(a, "action", "list"));
    std::string expr = GetStr(a, "expression");
    return Invoke([&]() -> json {
        if (action == "add") {
            if (expr.empty()) throw ToolError("expression required");
            if (std::find(watches_.begin(), watches_.end(), expr) == watches_.end()) watches_.push_back(expr);
        } else if (action == "remove") {
            watches_.erase(std::remove(watches_.begin(), watches_.end(), expr), watches_.end());
        } else if (action == "clear") {
            watches_.clear();
        } else if (action != "list") {
            throw ToolError("action must be add, remove, clear or list");
        }
        json w = json::object();
        for (auto& e : watches_)
            w[e] = (session_ && !running_) ? ShortValue(e) : std::string("<not stopped>");
        return json{{"watches", w}};
    });
}

json Debugger::Registers(const json& a) {
    return Invoke([&]() -> json {
        RequireStopped();
        ApplyThreadFrameArgs(a);
        std::vector<std::string> names;
        std::string set = ToLower(GetStr(a, "set", "general"));
        auto nit = a.find("names");
        if (nit != a.end() && nit->is_array() && !nit->empty()) {
            for (auto& n : *nit) names.push_back(ToLower(n.get<std::string>()));
        } else if (set == "general") {
            names = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8",  "r9", "r10",
                     "r11", "r12", "r13", "r14", "r15", "rip", "efl", "cs",  "ss",  "ds", "es",
                     "fs",  "gs"};
        } else {
            ULONG n = 0;
            registers_->GetNumberRegisters(&n);
            for (ULONG i = 0; i < n; ++i) {
                wchar_t nb[64];
                DEBUG_REGISTER_DESCRIPTION desc = {};
                if (FAILED(registers_->GetDescriptionWide(i, nb, 64, nullptr, &desc))) continue;
                if (desc.Flags & DEBUG_REGISTER_SUB_REGISTER) continue;
                std::string nm = Narrow(nb);
                bool vec = nm.rfind("xmm", 0) == 0 || nm.rfind("ymm", 0) == 0 || nm.rfind("zmm", 0) == 0;
                if (set == "vector" && !vec) continue;
                names.push_back(nm);
            }
        }
        json regs = json::object();
        for (auto& nm : names) {
            ULONG idx = 0;
            if (FAILED(registers_->GetIndexByNameWide(Widen(nm).c_str(), &idx))) {
                regs[nm] = "<unknown register>";
                continue;
            }
            DEBUG_VALUE v = {};
            if (FAILED(registers_->GetValues2(DEBUG_REGSRC_FRAME, 1, &idx, 0, &v)) &&
                FAILED(registers_->GetValue(idx, &v))) {
                regs[nm] = "<unavailable>";
                continue;
            }
            regs[nm] = FormatDebugValue(control_, v);
        }
        if (regs.contains("efl")) {
            // Decode common flags.
            uint64_t f = std::stoull(regs["efl"].get<std::string>(), nullptr, 16);
            std::string flags;
            const std::pair<int, const char*> bits[] = {{0, "CF"}, {2, "PF"}, {6, "ZF"}, {7, "SF"},
                                                        {8, "TF"}, {9, "IF"}, {10, "DF"}, {11, "OF"}};
            for (auto& [bit, name] : bits)
                if (f & (1ull << bit)) flags += std::string(flags.empty() ? "" : " ") + name;
            regs["efl_flags"] = flags;
        }
        return json{{"registers", regs}};
    });
}

json Debugger::SetRegister(const json& a) {
    std::string name = ToLower(RequireStr(a, "name"));
    std::string value = RequireStr(a, "value");
    return Invoke([&]() -> json {
        RequireStopped();
        RequireLive();
        ApplyThreadFrameArgs(a);
        ULONG idx = 0;
        if (FAILED(registers_->GetIndexByNameWide(Widen(name).c_str(), &idx)))
            throw ToolError("unknown register " + name);
        DEBUG_VALUE v = {};
        v.Type = DEBUG_VALUE_INT64;
        v.I64 = EvalAddress(value);
        HRESULT hr = registers_->SetValue(idx, &v);
        if (FAILED(hr)) throw ToolError("SetValue failed: " + HResultText(hr));
        DEBUG_VALUE nv = {};
        registers_->GetValue(idx, &nv);
        return json{{"name", name}, {"value", FormatDebugValue(control_, nv)}};
    });
}

json Debugger::ReadMemory(const json& a) {
    std::string addr_expr = RequireStr(a, "address");
    int size = std::clamp(GetInt(a, "size", 128), 1, 1 << 20);
    std::string fmt = ToLower(GetStr(a, "format", "hex"));
    size_t max_chars = (size_t)GetInt(a, "max_chars", 30000);
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) throw ToolError("target is running; pause first");
        ApplyThreadFrameArgs(a);
        uint64_t addr = EvalAddress(addr_expr);
        std::vector<uint8_t> buf(size);
        std::vector<bool> ok(size, false);
        // Read page by page so partially-readable ranges still return data.
        for (int off = 0; off < size;) {
            uint64_t cur = addr + off;
            int chunk = (int)std::min<uint64_t>(size - off, 0x1000 - (cur & 0xFFF));
            ULONG got = 0;
            if (SUCCEEDED(data_->ReadVirtual(cur, buf.data() + off, chunk, &got)))
                for (ULONG i = 0; i < got; ++i) ok[off + i] = true;
            off += chunk;
        }
        std::string out;
        char tmp[128];
        if (fmt == "hex" || fmt == "bytes") {
            for (int row = 0; row < size; row += 16) {
                snprintf(tmp, sizeof(tmp), "%016llx  ", (unsigned long long)(addr + row));
                out += tmp;
                std::string ascii;
                for (int i = 0; i < 16; ++i) {
                    int k = row + i;
                    if (k >= size) {
                        out += "   ";
                        continue;
                    }
                    if (ok[k]) {
                        snprintf(tmp, sizeof(tmp), "%02x ", buf[k]);
                        out += tmp;
                        ascii += (buf[k] >= 32 && buf[k] < 127) ? (char)buf[k] : '.';
                    } else {
                        out += "?? ";
                        ascii += '?';
                    }
                    if (i == 7) out += " ";
                }
                out += " " + ascii + "\n";
            }
        } else if (fmt == "ascii" || fmt == "utf8") {
            std::string s;
            for (int i = 0; i < size && ok[i] && buf[i]; ++i) s += (char)buf[i];
            out = s;
        } else if (fmt == "utf16") {
            std::wstring s;
            for (int i = 0; i + 1 < size && ok[i] && ok[i + 1]; i += 2) {
                wchar_t c = (wchar_t)(buf[i] | (buf[i + 1] << 8));
                if (!c) break;
                s += c;
            }
            out = Narrow(s);
        } else {
            int width = 0;
            if (fmt == "u8" || fmt == "i8") width = 1;
            else if (fmt == "u16" || fmt == "i16") width = 2;
            else if (fmt == "u32" || fmt == "i32" || fmt == "f32") width = 4;
            else if (fmt == "u64" || fmt == "i64" || fmt == "f64" || fmt == "pointers") width = 8;
            else throw ToolError("format must be hex, ascii, utf16, u8..u64, i8..i64, f32, f64 or pointers");
            int per_row = fmt == "pointers" ? 1 : std::max(1, 16 / width);
            for (int off = 0; off + width <= size; off += width) {
                if ((off / width) % per_row == 0) {
                    if (off) out += "\n";
                    snprintf(tmp, sizeof(tmp), "%016llx  ", (unsigned long long)(addr + off));
                    out += tmp;
                }
                bool readable = true;
                for (int i = 0; i < width; ++i) readable = readable && ok[off + i];
                if (!readable) {
                    out += "?? ";
                    continue;
                }
                uint64_t raw = 0;
                memcpy(&raw, buf.data() + off, width);
                if (fmt == "f32") {
                    float f;
                    memcpy(&f, &raw, 4);
                    snprintf(tmp, sizeof(tmp), "%.9g ", f);
                } else if (fmt == "f64") {
                    double d;
                    memcpy(&d, &raw, 8);
                    snprintf(tmp, sizeof(tmp), "%.17g ", d);
                } else if (fmt[0] == 'i') {
                    int64_t s = width == 1 ? (int8_t)raw : width == 2 ? (int16_t)raw
                                : width == 4 ? (int32_t)raw : (int64_t)raw;
                    snprintf(tmp, sizeof(tmp), "%lld ", (long long)s);
                } else if (fmt == "pointers") {
                    std::string sym = SymbolAt(raw, 0);
                    snprintf(tmp, sizeof(tmp), "%016llx", (unsigned long long)raw);
                    out += tmp;
                    if (!sym.empty()) out += "  " + sym;
                    tmp[0] = 0;
                } else {
                    snprintf(tmp, sizeof(tmp), "0x%0*llx ", width * 2, (unsigned long long)raw);
                }
                out += tmp;
            }
            out += "\n";
        }
        size_t unreadable = std::count(ok.begin(), ok.end(), false);
        json j = {{"address", Hex(addr)}, {"size", size}, {"data", Clip(out, max_chars)}};
        if (unreadable) j["unreadable_bytes"] = unreadable;
        return j;
    });
}

json Debugger::WriteMemory(const json& a) {
    std::string addr_expr = RequireStr(a, "address");
    std::string hex = GetStr(a, "hex");
    std::string value = GetStr(a, "value");
    std::string type = ToLower(GetStr(a, "type", "u32"));
    return Invoke([&]() -> json {
        RequireStopped();
        RequireLive();
        uint64_t addr = EvalAddress(addr_expr);
        std::vector<uint8_t> bytes;
        if (!hex.empty()) {
            std::string h;
            for (char c : hex)
                if (isxdigit((unsigned char)c)) h += c;
            if (h.size() % 2) throw ToolError("hex must have an even number of digits");
            for (size_t i = 0; i < h.size(); i += 2) bytes.push_back((uint8_t)std::stoul(h.substr(i, 2), nullptr, 16));
        } else if (!value.empty()) {
            if (type == "ascii") {
                bytes.assign(value.begin(), value.end());
                bytes.push_back(0);
            } else if (type == "utf16") {
                std::wstring w = Widen(value);
                bytes.resize((w.size() + 1) * 2);
                memcpy(bytes.data(), w.c_str(), bytes.size());
            } else if (type == "f32") {
                float f = std::stof(value);
                bytes.resize(4);
                memcpy(bytes.data(), &f, 4);
            } else if (type == "f64") {
                double d = std::stod(value);
                bytes.resize(8);
                memcpy(bytes.data(), &d, 8);
            } else {
                int width = (type == "u8" || type == "i8") ? 1 : (type == "u16" || type == "i16") ? 2
                            : (type == "u64" || type == "i64") ? 8 : 4;
                long long v = std::stoll(value, nullptr, 0);
                bytes.resize(width);
                memcpy(bytes.data(), &v, width);
            }
        } else {
            throw ToolError("provide hex or value");
        }
        ULONG written = 0;
        HRESULT hr = data_->WriteVirtual(addr, bytes.data(), (ULONG)bytes.size(), &written);
        if (FAILED(hr)) throw ToolError("write failed: " + HResultText(hr));
        return json{{"address", Hex(addr)}, {"bytes_written", written}};
    });
}

json Debugger::Disassemble(const json& a) {
    int count = std::clamp(GetInt(a, "count", 24), 1, 2000);
    int before = std::clamp(GetInt(a, "before", 0), 0, 200);
    bool whole = GetBool(a, "function", false);
    std::string addr_expr = GetStr(a, "address");
    size_t max_chars = (size_t)GetInt(a, "max_chars", 30000);
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) throw ToolError("target is running; pause first");
        ApplyThreadFrameArgs(a);
        ULONG64 ip = 0;
        registers_->GetInstructionOffset(&ip);
        uint64_t addr = addr_expr.empty() ? EvalAddress("@$scopeip") : ResolveLocation(addr_expr);
        if (whole) {
            std::string out = ExecChecked("uf " + Hex(addr));
            return json{{"disassembly", Clip(out, max_chars)}};
        }
        if (before) {
            ULONG64 near_off = addr;
            if (SUCCEEDED(control_->GetNearInstruction(addr, -before, &near_off))) addr = near_off;
        }
        std::string out, last_line;
        ULONG64 off = addr;
        {
            uint64_t d0 = 0;
            std::string s0 = SymbolAt(addr, 0, &d0);
            if (!s0.empty() && d0) out += s0 + "+" + Hex(d0) + ":\n";
        }
        for (int i = 0; i < count + before; ++i) {
            ULONG64 disp = 0;
            wchar_t sym[1024];
            if (SUCCEEDED(symbols_->GetNameByOffsetWide(off, sym, 1024, nullptr, &disp)) && disp == 0)
                out += Narrow(sym) + ":\n";
            std::string file;
            ULONG line;
            if (LineAt(off, 0, &file, &line)) {
                std::string key = file + ":" + std::to_string(line);
                if (key != last_line) {
                    std::string snip = SourceSnippet(file, line, 0);
                    size_t s = file.find_last_of("\\/");
                    out += "    ; " + file.substr(s == std::string::npos ? 0 : s + 1) + ":" +
                           std::to_string(line) + (snip.empty() ? "" : "  " + Trim(snip.substr(8))) + "\n";
                    last_line = key;
                }
            }
            wchar_t buf[1024];
            ULONG dsize = 0;
            ULONG64 end = 0;
            HRESULT hr = control_->DisassembleWide(off, DEBUG_DISASM_EFFECTIVE_ADDRESS, buf, 1024, &dsize, &end);
            if (FAILED(hr)) {
                out += Hex(off) + " ??\n";
                break;
            }
            out += (off == ip ? "=> " : "   ") + TrimRight(Narrow(buf)) + "\n";
            if (end <= off) break;
            off = end;
        }
        return json{{"disassembly", Clip(out, max_chars)}};
    });
}

// ======================= Symbols / modules =======================

json Debugger::LookupSymbol(const json& a) {
    std::string pattern = NormalizeModuleQualifiers(RequireStr(a, "pattern"));
    int limit = std::clamp(GetInt(a, "limit", 50), 1, 5000);
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) throw ToolError("target is running; pause first");
        json arr = json::array();
        ULONG64 handle = 0;
        HRESULT hr = symbols_->StartSymbolMatchWide(Widen(pattern).c_str(), &handle);
        bool more = false;
        if (SUCCEEDED(hr)) {
            for (;;) {
                wchar_t name[2048];
                ULONG64 off = 0;
                if (symbols_->GetNextSymbolMatchWide(handle, name, 2048, nullptr, &off) != S_OK) break;
                if ((int)arr.size() >= limit) {
                    more = true;
                    break;
                }
                json e = {{"name", Narrow(name)}, {"address", Hex(off)}};
                std::string file;
                ULONG line;
                if (LineAt(off, 0, &file, &line)) e["at"] = file + ":" + std::to_string(line);
                arr.push_back(e);
            }
            symbols_->EndSymbolMatch(handle);
        }
        if (arr.empty() && pattern.find_first_of("*?") == std::string::npos) {
            ULONG64 off = 0;
            if (SUCCEEDED(symbols_->GetOffsetByNameWide(Widen(pattern).c_str(), &off)))
                arr.push_back({{"name", pattern}, {"address", Hex(off)}});
        }
        json j = {{"matches", arr}};
        if (more) j["more"] = "limit reached; narrow the pattern (use module!prefix*)";
        if (arr.empty())
            j["hint"] = "Use module!pattern with wildcards, e.g. 'MyGame!*Player*'. Unqualified patterns search every module and can be slow on large apps.";
        return j;
    });
}

json Debugger::LookupAddress(const json& a) {
    std::string expr = RequireStr(a, "address");
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) throw ToolError("target is running; pause first");
        uint64_t addr = EvalAddress(expr);
        json j = {{"address", Hex(addr)}};
        uint64_t disp = 0;
        std::string sym = SymbolAt(addr, 0, &disp);
        if (!sym.empty()) j["symbol"] = sym + (disp ? "+" + Hex(disp) : "");
        std::string mod = ModuleNameAt(addr);
        if (!mod.empty()) j["module"] = mod;
        std::string file;
        ULONG line;
        if (LineAt(addr, 0, &file, &line)) j["at"] = file + ":" + std::to_string(line);
        MEMORY_BASIC_INFORMATION64 mbi = {};
        if (SUCCEEDED(data_->QueryVirtual(addr, &mbi))) {
            std::string state = mbi.State == MEM_COMMIT ? "commit" : mbi.State == MEM_RESERVE ? "reserve" : "free";
            std::string type = mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped"
                               : mbi.Type == MEM_PRIVATE ? "private" : "";
            char prot[32];
            snprintf(prot, sizeof(prot), "0x%lx", mbi.Protect);
            j["region"] = {{"base", Hex(mbi.BaseAddress)}, {"size", Hex(mbi.RegionSize)},
                           {"state", state}, {"type", type}, {"protect", prot}};
        }
        return j;
    });
}

json Debugger::Modules(const json& a) {
    std::string filter = ToLower(GetStr(a, "filter"));
    int limit = std::clamp(GetInt(a, "limit", 300), 1, 10000);
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) throw ToolError("target is running; pause first");
        ULONG loaded = 0, unloaded = 0;
        symbols_->GetNumberModules(&loaded, &unloaded);
        std::vector<DEBUG_MODULE_PARAMETERS> params(loaded);
        if (loaded) symbols_->GetModuleParameters(loaded, nullptr, 0, params.data());
        json arr = json::array();
        int matched = 0;
        for (ULONG i = 0; i < loaded; ++i) {
            wchar_t name[512] = {}, image[1024] = {}, symf[1024] = {};
            symbols_->GetModuleNameStringWide(DEBUG_MODNAME_MODULE, i, 0, name, 512, nullptr);
            symbols_->GetModuleNameStringWide(DEBUG_MODNAME_IMAGE, i, 0, image, 1024, nullptr);
            std::string n = Narrow(name), img = Narrow(image);
            if (!filter.empty() && ToLower(n).find(filter) == std::string::npos &&
                ToLower(img).find(filter) == std::string::npos)
                continue;
            if (++matched > limit) continue;
            const char* st = "none";
            switch (params[i].SymbolType) {
                case DEBUG_SYMTYPE_PDB: st = "pdb"; break;
                case DEBUG_SYMTYPE_DEFERRED: st = "deferred (loads on demand)"; break;
                case DEBUG_SYMTYPE_EXPORT: st = "export-only (no pdb)"; break;
                case DEBUG_SYMTYPE_COFF: st = "coff"; break;
                case DEBUG_SYMTYPE_CODEVIEW: st = "codeview"; break;
                case DEBUG_SYMTYPE_SYM: st = "sym"; break;
                case DEBUG_SYMTYPE_DIA: st = "pdb (dia)"; break;
            }
            json m = {{"name", n}, {"base", Hex(params[i].Base)}, {"size", Hex(params[i].Size)},
                      {"image", img}, {"symbols", st}};
            if (params[i].SymbolType == DEBUG_SYMTYPE_PDB || params[i].SymbolType == DEBUG_SYMTYPE_DIA) {
                if (SUCCEEDED(symbols_->GetModuleNameStringWide(DEBUG_MODNAME_SYMBOL_FILE, i, 0, symf, 1024, nullptr)))
                    m["pdb"] = Narrow(symf);
            }
            arr.push_back(m);
        }
        json j = {{"loaded", loaded}, {"matched", matched}, {"modules", arr}};
        if (matched > limit) j["more"] = "raise limit or use filter";
        return j;
    });
}

json Debugger::LoadSymbols(const json& a) {
    std::string module = RequireStr(a, "module");
    if (module != "*") module = NormalizeModuleName(module);
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) throw ToolError("target is running; pause first");
        if (a.contains("symbol_path")) {
            std::string sp = GetStr(a, "symbol_path");
            if (GetBool(a, "append", true)) symbols_->AppendSymbolPathWide(Widen(sp).c_str());
            else symbols_->SetSymbolPathWide(Widen(sp).c_str());
            Exec(".reload");
        }
        std::string out;
        if (module == "*")
            out = Exec(".reload /f");
        else
            out = Exec("ld " + module);
        std::string lm = Exec("lm m " + module);
        wchar_t sp[8192] = {};
        symbols_->GetSymbolPathWide(sp, 8192, nullptr);
        return json{{"output", Clip(Trim(out + "\n" + lm), 8000)}, {"symbol_path", Narrow(sp)}};
    });
}

json Debugger::TypeInfo(const json& a) {
    std::string type = NormalizeModuleQualifiers(RequireStr(a, "type"));
    std::string addr = GetStr(a, "address");
    int depth = std::clamp(GetInt(a, "depth", 0), 0, 5);
    size_t max_chars = (size_t)GetInt(a, "max_chars", 20000);
    return Invoke([&]() -> json {
        RequireSession();
        if (running_) throw ToolError("target is running; pause first");
        std::string cmd = "dt";
        if (depth) cmd += " -r" + std::to_string(depth);
        cmd += " " + type;
        if (!addr.empty()) cmd += " " + Hex(EvalAddress(addr));
        HRESULT hr;
        std::string out = Exec(cmd, &hr);
        if (FAILED(hr) || Trim(out).empty()) {
            // Maybe a pattern: list matching types.
            out = Exec("dt " + type + "*", &hr);
        }
        return json{{"result", Clip(Trim(out), max_chars)}};
    });
}

json Debugger::ExceptionSettings(const json& a) {
    std::string exc = GetStr(a, "exception");
    std::string mode = ToLower(GetStr(a, "mode"));
    return Invoke([&]() -> json {
        if (!exc.empty()) {
            std::string cmd = mode == "first" || mode == "break" ? "sxe"
                              : mode == "second"                  ? "sxd"
                              : mode == "output"                  ? "sxn"
                              : mode == "ignore"                  ? "sxi"
                                                                  : "";
            if (cmd.empty()) throw ToolError("mode must be first, second, output or ignore");
            std::string code = exc;
            std::string l = ToLower(exc);
            if (l == "cpp" || l == "c++") code = "eh";
            ExecChecked(cmd + " " + code);
        }
        std::string out = Exec("sx");
        // Keep the interesting part only.
        std::string summary;
        for (auto& line : SplitLines(out)) {
            std::string t = Trim(line);
            if (t.empty()) continue;
            if (t.find("av ") == 0 || t.find("eh ") == 0 || t.find("sov") == 0 || t.find("dz ") == 0 ||
                t.find("ii ") == 0 || t.find("bpe") == 0 || t.find("asrt") == 0 || t.find("gp ") == 0 ||
                t.find("ud ") == 0 || t.find("ibp") == 0 || t.find("ld ") == 0 || t.find("cpr") == 0 ||
                t.find("vcpp") == 0 || t.find("Other") != std::string::npos || t.find("0x") == 0 ||
                t.find(" - ") != std::string::npos && t.find("break") != std::string::npos)
                summary += t + "\n";
        }
        return json{{"settings", summary.empty() ? out : summary},
                    {"legend", "first=break on first chance (when thrown), second=break only if unhandled, output=log only, ignore. Aliases: av, eh/cpp (C++), sov, dz, ii, asrt, or a hex code."}};
    });
}

json Debugger::Output(const json& a) {
    std::string source = ToLower(GetStr(a, "source", "debuggee"));
    long long since = GetInt64(a, "since", 0);
    int tail = std::clamp(GetInt(a, "tail", 200), 1, 20000);
    LineLog* log = source == "engine" ? &engine_log_ : source == "logpoints" ? &trace_log_ : debuggee_out_;
    if (source == "debuggee") debuggee_out_->Flush();
    uint64_t next = 0;
    bool trunc = false;
    std::vector<std::string> lines = since > 0 ? log->Since((uint64_t)since, (size_t)tail, &next, &trunc)
                                               : log->Tail((size_t)tail, &next);
    std::string s;
    for (auto& l : lines) s += l + "\n";
    json j = {{"source", source}, {"lines", lines.size()}, {"text", Clip(s, (size_t)GetInt(a, "max_chars", 40000))}, {"next_since", next}};
    if (trunc) j["truncated"] = true;
    return j;
}

json Debugger::Command(const json& a) {
    std::string cmd = RequireStr(a, "command");
    size_t max_chars = (size_t)GetInt(a, "max_chars", 30000);
    int timeout = GetInt(a, "timeout_ms", kDefaultWaitMs);
    uint64_t seq = 0;
    bool resumed = false;
    json r = Invoke([&]() -> json {
        if (running_) throw ToolError("target is running; pause first");
        {
            std::lock_guard<std::mutex> lk(state_m_);
            seq = stop_seq_;
        }
        HRESULT hr;
        std::string out = Exec(cmd, &hr);
        if (session_) {
            ULONG status = 0;
            control_->GetExecutionStatus(&status);
            if (status != DEBUG_STATUS_BREAK && status != DEBUG_STATUS_NO_DEBUGGEE) {
                // The command resumed the target (g, p, t, gu, ...).
                resume_kind_ = (status == DEBUG_STATUS_GO) ? "continue" : "step";
                pending_bp_hit_ = 0;
                running_ = true;
                resumed = true;
                task_cv_.notify_all();
            }
        }
        json j = {{"output", Clip(out, max_chars)}};
        if (FAILED(hr)) j["error"] = HResultText(hr);
        return j;
    });
    if (resumed) r["stop"] = WaitForStop(seq, timeout, GetInt(a, "frames", 5));
    return r;
}

json Debugger::SetInstructionPointer(const json& a) {
    std::string loc = RequireStr(a, "location");
    return Invoke([&]() -> json {
        RequireStopped();
        RequireLive();
        uint64_t addr = ResolveLocation(loc);
        ULONG64 ip = 0;
        registers_->GetInstructionOffset(&ip);
        std::string from_fn = SymbolAt(ip, 0), to_fn = SymbolAt(addr, 0);
        auto strip = [](std::string s) {
            size_t p = s.find('+');
            return p == std::string::npos ? s : s.substr(0, p);
        };
        if (strip(from_fn) != strip(to_fn) && !GetBool(a, "force", false))
            throw ToolError("target " + Hex(addr) + " (" + to_fn + ") is outside the current function (" +
                            from_fn + "); pass force=true to do it anyway");
        ULONG idx = 0;
        registers_->GetIndexByNameWide(L"rip", &idx);
        DEBUG_VALUE v = {};
        v.Type = DEBUG_VALUE_INT64;
        v.I64 = addr;
        HRESULT hr = registers_->SetValue(idx, &v);
        if (FAILED(hr)) throw ToolError("failed to set rip: " + HResultText(hr));
        auto st = CurrentStack(1);
        return st.empty() ? json{{"rip", Hex(addr)}} : FrameJson(st[0], false, true);
    });
}

json Debugger::WriteDump(const json& a) {
    std::string path = RequireStr(a, "path");
    bool full = GetBool(a, "full", true);
    return Invoke([&]() -> json {
        RequireStopped();
        std::string out = ExecChecked(std::string(".dump /o ") + (full ? "/ma " : "/mhi ") + QuoteArg(path));
        return json{{"path", path}, {"output", Trim(out)}};
    });
}

json Debugger::LoadNatvis(const json& a) {
    std::string path = FullPath(RequireStr(a, "path"));
    if (GetFileAttributesW(Widen(path).c_str()) == INVALID_FILE_ATTRIBUTES) throw ToolError("file not found: " + path);
    return Invoke([&]() -> json {
        if (std::find(natvis_files_.begin(), natvis_files_.end(), path) == natvis_files_.end())
            natvis_files_.push_back(path);
        std::string out;
        if (session_ && !running_) out = Exec(".nvload " + QuoteArg(path));
        return json{{"loaded", natvis_files_}, {"output", Trim(out)},
                    {"note", session_ ? "applied" : "will be applied when a session starts"}};
    });
}

json Debugger::Source(const json& a) {
    std::string file = GetStr(a, "file");
    int line = GetInt(a, "line", 0);
    int context = std::clamp(GetInt(a, "context", 15), 0, 500);
    return Invoke([&]() -> json {
        if (file.empty()) {
            RequireStopped();
            ApplyThreadFrameArgs(a);
            uint64_t ip = EvalAddress("@$scopeip");
            ULONG l = 0;
            if (!LineAt(ip, 0, &file, &l)) throw ToolError("no source line information for the current frame");
            line = (int)l;
        }
        std::string snip = SourceSnippet(file, (ULONG)std::max(line, 1), context);
        if (snip.empty())
            throw ToolError("source file not found: " + file + " (set source_path on launch/attach to remap)");
        return json{{"file", ResolveSourceFile(file)}, {"line", line}, {"text", snip}};
    });
}

json Debugger::ListProcesses(const json& a) {
    std::string filter = ToLower(GetStr(a, "filter"));
    json arr = json::array();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::string exe = Narrow(pe.szExeFile);
        if (!filter.empty() && ToLower(exe).find(filter) == std::string::npos) continue;
        arr.push_back({{"pid", pe.th32ProcessID}, {"name", exe}, {"parent", pe.th32ParentProcessID}});
        if (arr.size() >= 500) break;
    }
    CloseHandle(snap);
    return json{{"processes", arr}};
}
