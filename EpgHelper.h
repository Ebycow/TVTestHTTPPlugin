#pragma once

// Requires TVTestPlugin.h to be included before this header
// (TVTest::EpgEventInfo, TVTest::EpgEventQueryInfo etc. are defined there)

#include "Types.h"
#include "JsonHelpers.h"

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
    ull += (ULONGLONG)seconds * 10000000ULL;
    ft.dwHighDateTime = (DWORD)(ull >> 32);
    ft.dwLowDateTime  = (DWORD)(ull & 0xFFFFFFFF);
    SYSTEMTIME result = {};
    FileTimeToSystemTime(&ft, &result);
    return result;
}

// EpgQuery + EpgEventInfo* から nlohmann::json オブジェクトを組み立てる
static nlohmann::json BuildEpgEventJson(const EpgQuery &q, TVTest::EpgEventInfo *pEvent)
{
    using json = nlohmann::json;
    json j;
    j["networkId"]         = q.networkId;
    j["transportStreamId"] = q.tsId;
    j["serviceId"]         = q.serviceId;
    j["fetchedAt"]         = NowLocalIso8601();

    if (pEvent) {
        SYSTEMTIME endSt = AddSeconds(pEvent->StartTime, pEvent->Duration);
        j["status"]  = "available";
        j["program"] = {
            {"eventId",   pEvent->EventID},
            {"name",      pEvent->pszEventName ? WStrToUtf8(pEvent->pszEventName) : ""},
            {"text",      pEvent->pszEventText ? WStrToUtf8(pEvent->pszEventText) : ""},
            {"startTime", SystemTimeToIso8601(pEvent->StartTime)},
            {"endTime",   SystemTimeToIso8601(endSt)},
            {"duration",  pEvent->Duration}
        };
    } else {
        j["status"]  = "unavailable";
        j["program"] = nullptr;
    }
    return j;
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

// nlohmann::json オブジェクトから EpgQuery を解決する
static bool ResolveEpgQuery(const nlohmann::json &j,
                             const std::vector<ChannelEntry> &channelList,
                             EpgQuery &q)
{
    auto intVal = [&](const char *key) -> int {
        auto it = j.find(key);
        if (it == j.end() || !it->is_number_integer()) return -1;
        return it->get<int>();
    };

    int sp = intVal("space"), ch = intVal("channel");
    if (sp >= 0 && ch >= 0) {
        for (const auto &e : channelList) {
            if (e.space == sp && e.channel == ch) {
                q.networkId = static_cast<WORD>(e.networkID);
                q.tsId      = static_cast<WORD>(e.tsID);
                q.serviceId = static_cast<WORD>(e.serviceID);
                return true;
            }
        }
        return false;
    }

    int networkId = intVal("networkId");
    if (networkId < 0) networkId = intVal("onid");
    int serviceId = intVal("serviceId");
    if (serviceId < 0) serviceId = intVal("sid");
    int tsId = intVal("transportStreamId");
    if (tsId < 0) tsId = intVal("tsid");

    if (networkId >= 0 && serviceId >= 0) {
        q.networkId = static_cast<WORD>(networkId);
        q.serviceId = static_cast<WORD>(serviceId);
        if (tsId >= 0) {
            q.tsId = static_cast<WORD>(tsId);
        } else {
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
static std::vector<EpgQuery> ParseEpgQueryArray(const std::string &body,
                                                 const std::vector<ChannelEntry> &channelList)
{
    std::vector<EpgQuery> result;
    auto arr = nlohmann::json::parse(body, nullptr, false);
    if (!arr.is_array()) return result;
    for (const auto &item : arr) {
        if (!item.is_object()) continue;
        EpgQuery q;
        if (ResolveEpgQuery(item, channelList, q))
            result.push_back(q);
    }
    return result;
}
