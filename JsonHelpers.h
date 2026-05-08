#pragma once

#include <windows.h>
#include <string>
#include <sstream>

// =============================================================================
// JSON ヘルパー
// =============================================================================

static std::string WStrToUtf8(const std::wstring &ws)
{
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static bool ParseIso8601Local(const std::string &value, SYSTEMTIME &st)
{
    if (value.size() < 19) return false;

    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u}) {
        if (!is_digit(value[i])) return false;
    }
    if (value[4] != '-' || value[7] != '-' || (value[10] != 'T' && value[10] != ' ') ||
        value[13] != ':' || value[16] != ':') {
        return false;
    }

    st = {};
    st.wYear   = static_cast<WORD>(std::stoi(value.substr(0, 4)));
    st.wMonth  = static_cast<WORD>(std::stoi(value.substr(5, 2)));
    st.wDay    = static_cast<WORD>(std::stoi(value.substr(8, 2)));
    st.wHour   = static_cast<WORD>(std::stoi(value.substr(11, 2)));
    st.wMinute = static_cast<WORD>(std::stoi(value.substr(14, 2)));
    st.wSecond = static_cast<WORD>(std::stoi(value.substr(17, 2)));

    FILETIME ft = {};
    return SystemTimeToFileTime(&st, &ft) != FALSE;
}

// wstring → UTF-8 std::string (WStrToUtf8 の別名)
static inline std::string WStrToStr(const std::wstring &ws) { return WStrToUtf8(ws); }

// UTF-8 std::string → wstring
static std::wstring StrToWStr(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), n);
    return ws;
}

// JSON ボディから文字列フィールドを取得。キーが無ければ空文字列を返す
static std::string ParseStrField(const std::string &json, const char *key)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) { ++pos; }
        val += json[pos++];
    }
    return val;
}

static std::string JsonStr(const std::wstring &ws)
{
    std::string src = WStrToUtf8(ws);
    std::string r;
    r.reserve(src.size() + 4);
    for (unsigned char c : src) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20)  {}        // 制御文字は除去
        else                r += static_cast<char>(c);
    }
    return r;
}

// JSON ボディから整数フィールドを取得。キーが無ければ INT_MIN を返す
static int ParseIntField(const std::string &json, const char *key)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return INT_MIN;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    if (pos >= json.size()) return INT_MIN;
    if (json[pos] == '"' || json[pos] == 't' || json[pos] == 'f') return INT_MIN;
    int sign = 1;
    if (json[pos] == '-') { sign = -1; ++pos; }
    if (pos >= json.size() || !isdigit(static_cast<unsigned char>(json[pos]))) return INT_MIN;
    int val = 0;
    while (pos < json.size() && isdigit(static_cast<unsigned char>(json[pos])))
        val = val * 10 + (json[pos++] - '0');
    return val * sign;
}

// JSON ボディから bool フィールドを取得。見つかれば true を返す
static bool ParseBoolField(const std::string &json, const char *key, bool &out)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) ++pos;
    if (pos + 4 <= json.size() && json.substr(pos, 4) == "true")  { out = true;  return true; }
    if (pos + 5 <= json.size() && json.substr(pos, 5) == "false") { out = false; return true; }
    return false;
}
