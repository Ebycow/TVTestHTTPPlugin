#pragma once

// Requires TVTestPlugin.h to be included before this header
// (TVTest::EpgEventInfo, TVTest::EpgEventQueryInfo etc. are defined there)

#include "Types.h"
#include "JsonHelpers.h"

#include <sstream>

// =============================================================================
// EPG ヘルパー
// =============================================================================

// SYSTEMTIME (JST=UTC+9 固定) → "YYYY-MM-DDTHH:MM:SS+09:00"
static std::string SystemTimeToIso8601(const SYSTEMTIME &st)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d+09:00",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 現在のローカル時刻 → "YYYY-MM-DDTHH:MM:SS"
static std::string NowLocalIso8601()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// SYSTEMTIME に秒を加算する (JST 日時の終了時刻計算用)
static SYSTEMTIME AddSeconds(const SYSTEMTIME &st, DWORD seconds)
{
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULONGLONG ull = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ull += (ULONGLONG)seconds * 10000000ULL; // 秒 → 100ns 単位
    ft.dwHighDateTime = (DWORD)(ull >> 32);
    ft.dwLowDateTime  = (DWORD)(ull & 0xFFFFFFFF);
    SYSTEMTIME result = {};
    FileTimeToSystemTime(&ft, &result);
    return result;
}

// EpgQuery + EpgEventInfo* から JSON オブジェクト文字列を組み立てる
// pEvent が nullptr の場合は status:"unavailable"
static std::string BuildEpgSingleJson(const EpgQuery &q, TVTest::EpgEventInfo *pEvent)
{
    std::ostringstream j;
    j << "{"
      << "\"networkId\":"         << q.networkId << ","
      << "\"transportStreamId\":" << q.tsId      << ","
      << "\"serviceId\":"         << q.serviceId << ","
      << "\"fetchedAt\":\""       << NowLocalIso8601() << "\",";

    if (pEvent) {
        SYSTEMTIME endSt = AddSeconds(pEvent->StartTime, pEvent->Duration);
        j << "\"status\":\"available\","
          << "\"program\":{"
          << "\"eventId\":"     << pEvent->EventID << ","
          << "\"name\":\""      << (pEvent->pszEventName ? JsonStr(std::wstring(pEvent->pszEventName)) : "") << "\","
          << "\"text\":\""      << (pEvent->pszEventText ? JsonStr(std::wstring(pEvent->pszEventText)) : "") << "\","
          << "\"startTime\":\"" << SystemTimeToIso8601(pEvent->StartTime) << "\","
          << "\"endTime\":\""   << SystemTimeToIso8601(endSt) << "\","
          << "\"duration\":"    << pEvent->Duration
          << "}}";
    } else {
        j << "\"status\":\"unavailable\",\"program\":null}";
    }
    return j.str();
}

static HWND FindTTRecWindowInCurrentProcess()
{
    static constexpr LPCTSTR TTREC_WINDOW_CLASS = TEXT("TVTest TTRec");
    HWND hwnd = nullptr;
    while ((hwnd = ::FindWindowEx(nullptr, hwnd, TTREC_WINDOW_CLASS, nullptr)) != nullptr) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid == ::GetCurrentProcessId()) return hwnd;
    }
    return nullptr;
}

// JSON オブジェクト文字列と channelList から EpgQuery を解決する。
// 解決できた場合 true を返す。
static bool ResolveEpgQuery(const std::string &obj,
                             const std::vector<ChannelEntry> &channelList,
                             EpgQuery &q)
{
    int sp        = ParseIntField(obj, "space");
    int ch        = ParseIntField(obj, "channel");
    int networkId = ParseIntField(obj, "networkId");
    int serviceId = ParseIntField(obj, "serviceId");
    int tsId      = ParseIntField(obj, "transportStreamId");
    if (networkId == INT_MIN) networkId = ParseIntField(obj, "onid");
    if (serviceId == INT_MIN) serviceId = ParseIntField(obj, "sid");
    if (tsId == INT_MIN) tsId = ParseIntField(obj, "tsid");

    if (sp != INT_MIN && ch != INT_MIN) {
        // space + channel → channelList から ID を補完
        for (const auto &e : channelList) {
            if (e.space == sp && e.channel == ch) {
                q.networkId = static_cast<WORD>(e.networkID);
                q.tsId      = static_cast<WORD>(e.tsID);
                q.serviceId = static_cast<WORD>(e.serviceID);
                return true;
            }
        }
        return false; // 見つからない
    }

    if (networkId != INT_MIN && serviceId != INT_MIN) {
        q.networkId = static_cast<WORD>(networkId);
        q.serviceId = static_cast<WORD>(serviceId);
        if (tsId != INT_MIN) {
            q.tsId = static_cast<WORD>(tsId);
        } else {
            // channelList から tsId を補完 (見つからなくても続行)
            for (const auto &e : channelList) {
                if (e.networkID == networkId && e.serviceID == serviceId) {
                    q.tsId = static_cast<WORD>(e.tsID);
                    break;
                }
            }
        }
        return true;
    }

    return false;
}

// JSON 配列文字列を解析して EpgQuery のベクタを返す
static std::vector<EpgQuery> ParseEpgQueryArray(const std::string &json,
                                                 const std::vector<ChannelEntry> &channelList)
{
    std::vector<EpgQuery> result;
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                std::string obj = json.substr(start, i - start + 1);
                EpgQuery q;
                if (ResolveEpgQuery(obj, channelList, q))
                    result.push_back(q);
                start = std::string::npos;
            }
        }
    }
    return result;
}
