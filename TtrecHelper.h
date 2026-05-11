#pragma once

#include "JsonHelpers.h"

#include <windows.h>
#include <string>

// =============================================================================
// TTRec 予約ファイルパーサー
// =============================================================================

// Parses UTF-16 LE content of TTRec's _Reserves.txt and returns a JSON array string.
static std::string BuildTtrecReservesJson(const std::wstring &content)
{
    auto popField = [](std::wstring &s) -> std::wstring {
        size_t tab = s.find(L'\t');
        if (tab == std::wstring::npos) {
            std::wstring r;
            std::swap(r, s);
            return r;
        }
        std::wstring r = s.substr(0, tab);
        s.erase(0, tab + 1);
        return r;
    };

    using json = nlohmann::json;
    json arr = json::array();
    size_t pos = 0;

    for (;;) {
        size_t nl = content.find(L'\n', pos);
        std::wstring line = (nl == std::wstring::npos)
            ? content.substr(pos)
            : content.substr(pos, nl - pos);
        if (nl == std::wstring::npos) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            pos = content.size() + 1;
        } else {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            pos = nl + 1;
        }
        if (line.empty()) {
            if (pos > content.size()) break;
            continue;
        }

        std::wstring rest = line;
        int networkID  = (int)wcstol(popField(rest).c_str(), nullptr, 16);
        int tsID       = (int)wcstol(popField(rest).c_str(), nullptr, 16);
        int serviceID  = (int)wcstol(popField(rest).c_str(), nullptr, 16);
        int eventID    = (int)wcstol(popField(rest).c_str(), nullptr, 16);
        std::string startTime = WStrToUtf8(popField(rest));

        // "hh:mm:ss[!][#][$]"
        std::wstring durField = popField(rest);
        int durSec = 0;
        bool followPF = false, isEnabled = true, followFixed = false;
        if (durField.size() >= 8) {
            durSec = (int)wcstol(durField.substr(0, 2).c_str(), nullptr, 10) * 3600
                   + (int)wcstol(durField.substr(3, 2).c_str(), nullptr, 10) * 60
                   + (int)wcstol(durField.substr(6, 2).c_str(), nullptr, 10);
            for (size_t k = 8; k < durField.size(); ++k) {
                if      (durField[k] == L'!') followPF    = true;
                else if (durField[k] == L'#') isEnabled   = false;
                else if (durField[k] == L'$') followFixed = true;
            }
        }

        // 番組名 (先頭が PREFIX_EPGORIGIN=0x11 なら除去)
        std::wstring nameW = popField(rest);
        if (!nameW.empty() && nameW[0] == L'\x11') nameW = nameW.substr(1);

        int startMargin = (int)wcstol(popField(rest).c_str(), nullptr, 10);
        int endMargin   = (int)wcstol(popField(rest).c_str(), nullptr, 10);
        int priorityRel = (int)wcstol(popField(rest).c_str(), nullptr, 10);
        int onStopped   = (int)wcstol(popField(rest).c_str(), nullptr, 10);
        std::wstring saveDirW  = popField(rest);
        std::wstring saveNameW = popField(rest);
        int startTrim = (int)wcstol(popField(rest).c_str(), nullptr, 10);
        int endTrim   = (int)wcstol(popField(rest).c_str(), nullptr, 10);

        bool isViewOnly = (priorityRel < 0);
        const char *followMode = followFixed ? "fixed" : (followPF ? "following" : "default");

        arr.push_back({
            {"networkId",         networkID},
            {"transportStreamId", tsID},
            {"serviceId",         serviceID},
            {"eventId",           eventID},
            {"startTime",         startTime},
            {"duration",          durSec},
            {"eventName",         WStrToUtf8(nameW)},
            {"isEnabled",         isEnabled},
            {"isViewOnly",        isViewOnly},
            {"followMode",        followMode},
            {"recOption", {
                {"startMargin", startMargin},
                {"endMargin",   endMargin},
                {"priority",    priorityRel},
                {"onStopped",   onStopped},
                {"saveDir",     WStrToUtf8(saveDirW  == L"*" ? L"" : saveDirW)},
                {"saveName",    WStrToUtf8(saveNameW == L"*" ? L"" : saveNameW)},
                {"startTrim",   startTrim},
                {"endTrim",     endTrim}
            }}
        });

        if (pos > content.size()) break;
    }

    return arr.dump();
}

static bool LoadTtrecReservesJson(HWND hwndTTRec, std::string &json)
{
    HINSTANCE hinstTTRec = reinterpret_cast<HINSTANCE>(
        ::GetWindowLongPtr(hwndTTRec, GWLP_HINSTANCE));
    wchar_t dllPath[MAX_PATH] = {};
    if (!::GetModuleFileNameW(hinstTTRec, dllPath, MAX_PATH)) {
        return false;
    }

    wchar_t reservePath[MAX_PATH + 16] = {};
    ::wcscpy_s(reservePath, dllPath);
    wchar_t *dot = ::wcsrchr(reservePath, L'.');
    if (dot) *dot = L'\0';
    ::wcscat_s(reservePath, L"_Reserves.txt");

    HANDLE hFile = ::CreateFileW(reservePath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        json = "[]";
        return true;
    }

    DWORD fileBytes = ::GetFileSize(hFile, nullptr);
    std::wstring content;
    if (fileBytes != INVALID_FILE_SIZE && fileBytes >= sizeof(wchar_t) * 2) {
        wchar_t bom = 0;
        DWORD rd = 0;
        if (::ReadFile(hFile, &bom, sizeof(wchar_t), &rd, nullptr) && bom == L'\xFEFF') {
            DWORD wchars = (fileBytes / sizeof(wchar_t)) - 1;
            content.resize(wchars);
            ::ReadFile(hFile, &content[0], wchars * sizeof(wchar_t), &rd, nullptr);
            content.resize(rd / sizeof(wchar_t));
        }
    }
    ::CloseHandle(hFile);

    json = BuildTtrecReservesJson(content);
    return true;
}
