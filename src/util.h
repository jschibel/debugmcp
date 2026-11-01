#pragma once
#include <windows.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/json.hpp"

using json = nlohmann::json;

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::string Hex(uint64_t v);
std::string TrimRight(std::string s);
std::string Trim(std::string s);
std::string ToLower(std::string s);
bool IEquals(const std::string& a, const std::string& b);
bool EndsWithI(const std::string& s, const std::string& suffix);
std::vector<std::string> SplitLines(const std::string& s);
// Truncate to roughly max_chars, appending a note about how much was cut.
std::string Clip(const std::string& s, size_t max_chars);
std::string HResultText(long hr);
// DbgEng module names replace characters like '-' with '_' (UnrealEditor-Core -> UnrealEditor_Core).
// Rewrites "UnrealEditor-Core!Foo" qualifiers inside an expression accordingly.
std::string NormalizeModuleQualifiers(const std::string& expr);
std::string NormalizeModuleName(std::string name);
// Absolute path with backslashes.
std::string FullPath(const std::string& p);

// Private diagnostics log (our real stderr; the process-level stderr is
// redirected so that debuggee output can be captured).
void DiagLog(const char* fmt, ...);
void SetDiagHandle(HANDLE h);

// Thread-safe line-oriented ring buffer with monotonically increasing sequence numbers.
class LineLog {
public:
    explicit LineLog(size_t max_lines = 20000) : max_lines_(max_lines) {}
    void Append(const std::string& text);  // may contain partial lines
    void Flush();                           // flush a pending partial line
    // Returns lines with seq >= since (since == 0 -> tail). Sets next_seq.
    std::vector<std::string> Since(uint64_t since, size_t max_lines, uint64_t* next_seq,
                                   bool* truncated);
    std::vector<std::string> Tail(size_t n, uint64_t* next_seq);
    uint64_t NextSeq();
    void Clear();

private:
    void PushLine(std::string line);
    std::mutex m_;
    std::deque<std::string> lines_;
    uint64_t first_seq_ = 1;  // seq of lines_.front()
    std::string partial_;
    size_t max_lines_;
};
