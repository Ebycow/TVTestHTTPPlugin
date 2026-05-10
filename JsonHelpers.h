#pragma once

#include <windows.h>
#include <string>

#include <nlohmann/json.hpp>

// =============================================================================
// 文字列変換ヘルパー
// =============================================================================

static std::string WStrToUtf8(const std::wstring &ws)
{
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
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

// ISO8601 文字列 (YYYY-MM-DDTHH:MM:SS) を SYSTEMTIME にパース
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
