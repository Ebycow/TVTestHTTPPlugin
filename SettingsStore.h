#pragma once

#include "Constants.h"
#include "Types.h"

#include <windows.h>
#include <string>

extern HINSTANCE g_hinstDLL;

namespace TVTestHTTP {

static std::wstring GetIniFilePath()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(g_hinstDLL, path, MAX_PATH);
    wchar_t *dot = wcsrchr(path, L'.');
    if (dot) {
        wcscpy_s(dot, MAX_PATH - (dot - path), L".ini");
    } else {
        wcscat_s(path, MAX_PATH, L".ini");
    }
    return path;
}

static PluginSettings LoadPluginSettings()
{
    PluginSettings settings;
    std::wstring ini = GetIniFilePath();
    const wchar_t *sec = L"Settings";

    int port = static_cast<int>(
        GetPrivateProfileIntW(sec, L"Port", HTTP_PORT_DEFAULT, ini.c_str()));
    if (port < 1024 || port > 65535) port = HTTP_PORT_DEFAULT;
    settings.port = port;

    wchar_t buf[4096] = {};
    GetPrivateProfileStringW(sec, L"AllowList", L"", buf, _countof(buf), ini.c_str());
    settings.allowList = buf;

    GetPrivateProfileStringW(sec, L"DenyList", L"", buf, _countof(buf), ini.c_str());
    settings.denyList = buf;

    return settings;
}

static void SavePluginSettings(const PluginSettings &settings)
{
    std::wstring ini = GetIniFilePath();
    const wchar_t *sec = L"Settings";

    wchar_t portStr[16] = {};
    _itow_s(settings.port, portStr, 10);
    WritePrivateProfileStringW(sec, L"Port",      portStr,                    ini.c_str());
    WritePrivateProfileStringW(sec, L"AllowList", settings.allowList.c_str(), ini.c_str());
    WritePrivateProfileStringW(sec, L"DenyList",  settings.denyList.c_str(),  ini.c_str());
}

} // namespace TVTestHTTP
