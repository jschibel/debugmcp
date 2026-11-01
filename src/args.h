#pragma once
// Small helpers for reading tool arguments.
#include <string>

#include "engine.h"

namespace args {

inline int GetInt(const json& a, const char* k, int def) {
    auto it = a.find(k);
    if (it == a.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<int>();
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>(), nullptr, 0);
        } catch (...) {
        }
    }
    throw ToolError(std::string("argument '") + k + "' must be an integer");
}

inline long long GetInt64(const json& a, const char* k, long long def) {
    auto it = a.find(k);
    if (it == a.end() || it->is_null()) return def;
    if (it->is_number()) return it->get<long long>();
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>(), nullptr, 0);
        } catch (...) {
        }
    }
    throw ToolError(std::string("argument '") + k + "' must be an integer");
}

inline bool GetBool(const json& a, const char* k, bool def) {
    auto it = a.find(k);
    if (it == a.end() || it->is_null()) return def;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return IEquals(it->get<std::string>(), "true");
    if (it->is_number()) return it->get<int>() != 0;
    return def;
}

inline std::string GetStr(const json& a, const char* k, const std::string& def = "") {
    auto it = a.find(k);
    if (it == a.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

inline std::string RequireStr(const json& a, const char* k) {
    std::string v = GetStr(a, k);
    if (v.empty()) throw ToolError(std::string("missing required argument '") + k + "'");
    return v;
}

inline std::string QuoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            out.push_back(c);
            continue;
        }
        if (c == '"') {
            out.append(backslashes + 1, '\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes, '\\');
    out.push_back('"');
    return out;
}

}  // namespace args
