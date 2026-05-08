#pragma once

#include <windows.h>
#include "Constants.h"
#include "Types.h"
#include "resource.h"

// g_hinstDLL は TVTestPlugin.h の TVTEST_PLUGIN_CLASS_IMPLEMENT マクロが定義する
extern HINSTANCE g_hinstDLL;

// =============================================================================
// 設定ダイアログ
// =============================================================================

struct SettingsDlgData {
    PluginSettings settings;
};

static INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_INITDIALOG: {
        SetWindowLongPtr(hwnd, DWLP_USER, lParam);
        auto *data = reinterpret_cast<SettingsDlgData *>(lParam);

        // ポート
        wchar_t portStr[16] = {};
        _itow_s(data->settings.port, portStr, 10);
        SetDlgItemTextW(hwnd, IDC_PORT, portStr);

        // 許可リスト: カンマ → 改行に変換して表示
        std::wstring allow = data->settings.allowList;
        for (auto &c : allow) if (c == L',') c = L'\n';
        SetDlgItemTextW(hwnd, IDC_ALLOW, allow.c_str());

        // 拒否リスト
        std::wstring deny = data->settings.denyList;
        for (auto &c : deny) if (c == L',') c = L'\n';
        SetDlgItemTextW(hwnd, IDC_DENY, deny.c_str());

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDOK: {
            auto *data = reinterpret_cast<SettingsDlgData *>(
                GetWindowLongPtr(hwnd, DWLP_USER));

            // ポート検証
            wchar_t portStr[16] = {};
            GetDlgItemTextW(hwnd, IDC_PORT, portStr, _countof(portStr));
            int port = _wtoi(portStr);
            if (port < 1024 || port > 65535) {
                MessageBoxW(hwnd,
                    L"ポート番号は 1024〜65535 の範囲で入力してください",
                    L"入力エラー", MB_ICONWARNING | MB_OK);
                SetFocus(GetDlgItem(hwnd, IDC_PORT));
                return TRUE;
            }
            data->settings.port = port;

            // 許可リスト: \r\n → カンマに変換して保存
            auto ReadList = [&](int ctrlId) -> std::wstring {
                wchar_t buf[4096] = {};
                GetDlgItemTextW(hwnd, ctrlId, buf, _countof(buf));
                std::wstring result;
                for (wchar_t c : std::wstring(buf)) {
                    if (c == L'\r') continue;
                    if (c == L'\n') {
                        if (!result.empty() && result.back() != L',') result += L',';
                    } else {
                        result += c;
                    }
                }
                // 末尾のカンマを除去
                while (!result.empty() && result.back() == L',') result.pop_back();
                return result;
            };

            data->settings.allowList = ReadList(IDC_ALLOW);
            data->settings.denyList  = ReadList(IDC_DENY);

            EndDialog(hwnd, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
