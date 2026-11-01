#include "util.h"

#include <cstdarg>
#include <cstdio>

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Hex(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return buf;
}

std::string TrimRight(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}

std::string Trim(std::string s) {
    s = TrimRight(std::move(s));
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return s.substr(i);
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool IEquals(const std::string& a, const std::string& b) { return ToLower(a) == ToLower(b); }

bool EndsWithI(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return IEquals(s.substr(s.size() - suffix.size()), suffix);
}

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Clip(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    // Avoid cutting a UTF-8 sequence in half.
    size_t cut = max_chars;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "\n... [truncated " + std::to_string(s.size() - cut) +
           " chars; narrow the request or raise max_chars]";
}

std::string HResultText(long hr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "HRESULT 0x%08lx", (unsigned long)hr);
    std::string out = buf;
    wchar_t* msg = nullptr;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, (DWORD)hr, 0, (LPWSTR)&msg, 0, nullptr) &&
        msg) {
        out += " (" + Trim(Narrow(msg)) + ")";
        LocalFree(msg);
    }
    return out;
}

static HANDLE g_diag = INVALID_HANDLE_VALUE;
static std::mutex g_diag_m;

void SetDiagHandle(HANDLE h) { g_diag = h; }

void DiagLog(const char* fmt, ...) {
    if (g_diag == INVALID_HANDLE_VALUE || g_diag == nullptr) return;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    std::lock_guard<std::mutex> lk(g_diag_m);
    DWORD w;
    WriteFile(g_diag, "[debugmcp] ", 11, &w, nullptr);
    WriteFile(g_diag, buf, (DWORD)n, &w, nullptr);
}

// ---------------- LineLog ----------------

void LineLog::PushLine(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines_.push_back(std::move(line));
    while (lines_.size() > max_lines_) {
        lines_.pop_front();
        ++first_seq_;
    }
}

void LineLog::Append(const std::string& text) {
    std::lock_guard<std::mutex> lk(m_);
    for (char c : text) {
        if (c == '\n') {
            PushLine(std::move(partial_));
            partial_.clear();
        } else {
            partial_.push_back(c);
            if (partial_.size() > 64 * 1024) {  // pathological line, force a break
                PushLine(std::move(partial_));
                partial_.clear();
            }
        }
    }
}

void LineLog::Flush() {
    std::lock_guard<std::mutex> lk(m_);
    if (!partial_.empty()) {
        PushLine(std::move(partial_));
        partial_.clear();
    }
}

uint64_t LineLog::NextSeq() {
    std::lock_guard<std::mutex> lk(m_);
    return first_seq_ + lines_.size();
}

std::vector<std::string> LineLog::Since(uint64_t since, size_t max_lines, uint64_t* next_seq,
                                        bool* truncated) {
    std::lock_guard<std::mutex> lk(m_);
    uint64_t end = first_seq_ + lines_.size();
    if (truncated) *truncated = false;
    if (since < first_seq_) {
        if (truncated && since != 0) *truncated = true;
        since = first_seq_;
    }
    std::vector<std::string> out;
    uint64_t start = since;
    if (end - start > max_lines) {
        // Keep the most recent lines.
        start = end - max_lines;
        if (truncated) *truncated = true;
    }
    for (uint64_t s = start; s < end; ++s) out.push_back(lines_[(size_t)(s - first_seq_)]);
    if (next_seq) *next_seq = end;
    return out;
}

std::vector<std::string> LineLog::Tail(size_t n, uint64_t* next_seq) {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    size_t start = lines_.size() > n ? lines_.size() - n : 0;
    for (size_t i = start; i < lines_.size(); ++i) out.push_back(lines_[i]);
    if (next_seq) *next_seq = first_seq_ + lines_.size();
    return out;
}

void LineLog::Clear() {
    std::lock_guard<std::mutex> lk(m_);
    first_seq_ += lines_.size();
    lines_.clear();
    partial_.clear();
}

std::string NormalizeModuleName(std::string name) {
    for (auto& c : name)
        if (!isalnum((unsigned char)c) && c != '_') c = '_';
    return name;
}

std::string NormalizeModuleQualifiers(const std::string& expr) {
    std::string out = expr;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] != '!' || (i + 1 < out.size() && out[i + 1] == '=')) continue;
        size_t j = i;
        while (j > 0 && (isalnum((unsigned char)out[j - 1]) || out[j - 1] == '_' || out[j - 1] == '-' || out[j - 1] == '.'))
            --j;
        for (size_t k = j; k < i; ++k)
            if (out[k] == '-' || out[k] == '.') out[k] = '_';
    }
    return out;
}

std::string FullPath(const std::string& p) {
    std::wstring w = Widen(p);
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH * 4, buf, nullptr);
    if (!n || n >= MAX_PATH * 4) return p;
    return Narrow(buf);
}
