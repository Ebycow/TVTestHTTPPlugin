/**
 * TVTestHTTPPlugin.cpp
 * TVTest HTTP API Plugin v1.0
 *
 * TVTest を HTTP REST API 経由で外部から制御するプラグイン。
 * デフォルトポート: 40152
 *
 * スレッド安全設計:
 *   - TVTest API は必ずメインスレッドから呼ぶ (SDK 制約)
 *   - GET: キャッシュから読む (mutex 保護, HTTP スレッドから OK)
 *   - POST/EPG取得: キューに積み → 50ms タイマーでメインスレッドが処理 → イベント通知
 */

// WIN32_LEAN_AND_MEAN は CMakeLists.txt で定義済み
// NOMINMAX は定義しない: TVTestPlugin.h が windows.h の min/max マクロを必要とするため
#include <windows.h>

// `interface` マクロ (TVTestPlugin.h の FilterGraphInfo で使用)
#include <objbase.h>

// TVTest Plugin SDK
// ・Shift-JIS コメント警告 C4828 を抑制
// ・httplib.h より先にインクルード必須
#pragma warning(push)
#pragma warning(disable: 4828)
#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"
#pragma warning(pop)

// cpp-httplib (TVTestPlugin.h の後にインクルード)
#include "httplib.h"

#include <string>
#include <thread>
#include <mutex>
#include <queue>
#include <memory>
#include <vector>

#include "resource.h"

#include "Constants.h"
#include "Types.h"
#include "JsonHelpers.h"
#include "ApiJson.h"
#include "EpgHelper.h"
#include "HttpStateRoutes.h"
#include "HttpPostRoutes.h"
#include "RequestProcessor.h"
#include "IpFilter.h"
#include "Settings.h"
#include "SettingsStore.h"
#include "TtrecHelper.h"

// =============================================================================
// プラグイン本体
// =============================================================================

static class CTVTestHTTPPlugin *s_pPlugin = nullptr;

class CTVTestHTTPPlugin : public TVTest::CTVTestPlugin
{
    httplib::Server   m_httpServer;
    std::thread       m_httpThread;
    bool              m_serverStarted = false;
    bool              m_routesConfigured = false;

    mutable std::mutex m_stateMutex;
    TVTestState        m_state;

    std::mutex                                    m_queueMutex;
    std::queue<std::shared_ptr<WriteRequest>>     m_requestQueue;

    int m_programRefreshTick = 0;

    mutable std::mutex       m_settingsMutex;
    PluginSettings           m_settings;
    std::vector<CidrBlock>   m_allowBlocks;
    std::vector<CidrBlock>   m_denyBlocks;

public:
    CTVTestHTTPPlugin()  { s_pPlugin = this; }
    ~CTVTestHTTPPlugin() { s_pPlugin = nullptr; StopServer(); }

    // -------------------------------------------------------------------------
    // TVTest::CTVTestPlugin 実装
    // -------------------------------------------------------------------------

    bool GetPluginInfo(TVTest::PluginInfo *pInfo) override
    {
        pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
        pInfo->Flags          = TVTest::PLUGIN_FLAG_ENABLEDEFAULT
                              | TVTest::PLUGIN_FLAG_HASSETTINGS;
        pInfo->pszPluginName  = L"TVTest HTTP API";
        pInfo->pszCopyright   = L"(c) 2026";
        pInfo->pszDescription = L"HTTP REST API で TVTest を外部から制御します";
        return true;
    }

    bool Initialize() override
    {
        LoadSettings();
        UpdateCidrBlocks();
        m_pApp->SetEventCallback(EventCallback, this);
        SetTimer(m_pPluginParam->hwndApp, TIMER_ID, TIMER_MS, TimerProc);
        if (m_pApp->IsPluginEnabled()) {
            RefreshAll();
            StartServer();
        }
        return true;
    }

    bool Finalize() override
    {
        KillTimer(m_pPluginParam->hwndApp, TIMER_ID);
        StopServer();
        return true;
    }

private:
    // -------------------------------------------------------------------------
    // TVTest イベントハンドラ (メインスレッド)
    // -------------------------------------------------------------------------

    static LRESULT CALLBACK EventCallback(UINT event, LPARAM p1, LPARAM p2, void *data)
    {
        auto *self = static_cast<CTVTestHTTPPlugin *>(data);
        switch (event) {
        case TVTest::EVENT_PLUGINENABLE:
            if (p1) { self->RefreshAll(); self->StartServer(); }
            else      self->StopServer();
            return TRUE;

        case TVTest::EVENT_PLUGINSETTINGS:
            return self->OnPluginSettings(reinterpret_cast<HWND>(p1)) ? TRUE : FALSE;

        case TVTest::EVENT_CHANNELCHANGE:
            {
                bool needList = false;
                {
                    std::lock_guard<std::mutex> lk(self->m_stateMutex);
                    needList = self->m_state.channelList.empty();
                }
                if (needList) self->RefreshChannelList();
            }
            self->RefreshChannel();
            self->RefreshProgram();
            break;

        case TVTest::EVENT_SERVICECHANGE:
        case TVTest::EVENT_SERVICEUPDATE:
            self->RefreshChannel();
            self->RefreshProgram();
            break;

        case TVTest::EVENT_VOLUMECHANGE:
            {
                std::lock_guard<std::mutex> lk(self->m_stateMutex);
                self->m_state.volume = static_cast<int>(p1);
                self->m_state.mute   = p2 != 0;
            }
            break;

        case TVTest::EVENT_RECORDSTATUSCHANGE:
            {
                std::lock_guard<std::mutex> lk(self->m_stateMutex);
                self->m_state.recordStatus = static_cast<int>(p1);
            }
            break;

        case TVTest::EVENT_STARTRECORD:
            {
                std::lock_guard<std::mutex> lk(self->m_stateMutex);
                self->m_state.recordStatus = TVTest::RECORD_STATUS_RECORDING;
            }
            break;

        case TVTest::EVENT_DRIVERCHANGE:
            self->RefreshChannelList();
            self->RefreshChannel();
            break;
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // タイマー (メインスレッド) → 書き込みキューをドレイン
    // -------------------------------------------------------------------------

    static VOID CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD)
    {
        if (s_pPlugin) s_pPlugin->DrainQueue();
    }

    void DrainQueue()
    {
        for (;;) {
            std::shared_ptr<WriteRequest> req;
            {
                std::lock_guard<std::mutex> lk(m_queueMutex);
                if (m_requestQueue.empty()) break;
                req = m_requestQueue.front();
                m_requestQueue.pop();
            }
            ProcessRequest(*req);
        }

        if (++m_programRefreshTick >= PROGRAM_REFRESH_INTERVAL) {
            m_programRefreshTick = 0;
            RefreshProgram();
        }
    }

    void ProcessRequest(WriteRequest &req)
    {
        RequestProcessContext ctx;
        ctx.app                = m_pApp;
        ctx.stateMutex         = &m_stateMutex;
        ctx.state              = &m_state;
        ctx.refreshChannel     = [this] { RefreshChannel(); };
        ctx.refreshChannelList = [this] { RefreshChannelList(); };
        ProcessWriteRequest(req, ctx);
    }

    // HTTP スレッドからリクエストを投げてメインスレッドの処理を待つ
    void Dispatch(std::shared_ptr<WriteRequest> req)
    {
        if (!req->hDone.valid()) {
            req->success      = false;
            req->responseJson = R"({"error":"内部エラー: イベントハンドルの作成に失敗しました"})";
            return;
        }
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            m_requestQueue.push(req);
        }
        if (WaitForSingleObject(req->hDone, DISPATCH_TIMEOUT_MS) != WAIT_OBJECT_0) {
            req->success      = false;
            req->responseJson = R"({"error":"タイムアウト"})";
        }
    }

    // -------------------------------------------------------------------------
    // 状態更新 (すべてメインスレッドから呼ぶこと)
    // -------------------------------------------------------------------------

    void RefreshAll()
    {
        RefreshChannelList();
        RefreshChannel();
        RefreshVolume();
        RefreshRecord();
        RefreshProgram();
    }

    void RefreshChannelList()
    {
        std::vector<ChannelEntry> list;
        int numSpaces = 0;
        m_pApp->GetTuningSpace(&numSpaces);

        for (int sp = 0; sp < numSpaces; ++sp) {
            for (int ch = 0; ; ++ch) {
                TVTest::ChannelInfo info = {};
                info.Size = sizeof(info);
                if (!m_pApp->GetChannelInfo(sp, ch, &info)) break;
                if (info.Flags & TVTest::CHANNEL_FLAG_DISABLED) continue;

                ChannelEntry e;
                e.space            = sp;
                e.channel          = ch;
                e.remoteControlKey = info.RemoteControlKeyID;
                e.serviceID        = info.ServiceID;
                e.networkID        = info.NetworkID;
                e.tsID             = info.TransportStreamID;
                e.name             = info.szChannelName;
                e.networkName      = info.szNetworkName;
                list.push_back(std::move(e));
            }
        }

        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_state.channelList = std::move(list);
    }

    void RefreshChannel()
    {
        TVTest::ChannelInfo info = {};
        info.Size = sizeof(info);
        std::lock_guard<std::mutex> lk(m_stateMutex);
        if (m_pApp->GetCurrentChannelInfo(&info)) {
            m_state.hasChannel       = true;
            m_state.space            = info.Space;
            m_state.channel          = info.Channel;
            m_state.remoteControlKey = info.RemoteControlKeyID;
            m_state.serviceID        = info.ServiceID;
            m_state.networkID        = info.NetworkID;
            m_state.tsID             = info.TransportStreamID;
            m_state.channelName      = info.szChannelName;
            m_state.networkName      = info.szNetworkName;
        } else {
            m_state.hasChannel = false;
        }
    }

    void RefreshVolume()
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_state.volume = m_pApp->GetVolume();
        m_state.mute   = m_pApp->GetMute() != FALSE;
    }

    void RefreshRecord()
    {
        TVTest::RecordStatusInfo info = {};
        info.Size = sizeof(info);
        std::lock_guard<std::mutex> lk(m_stateMutex);
        m_state.recordStatus = m_pApp->GetRecordStatus(&info)
                               ? info.Status
                               : TVTest::RECORD_STATUS_NOTRECORDING;
    }

    void RefreshProgram()
    {
        WCHAR nameB[256] = {}, textB[512] = {};
        TVTest::ProgramInfo prog = {};
        prog.Size         = sizeof(prog);
        prog.pszEventName = nameB;
        prog.MaxEventName = _countof(nameB);
        prog.pszEventText = textB;
        prog.MaxEventText = _countof(textB);

        std::lock_guard<std::mutex> lk(m_stateMutex);
        if (m_pApp->GetCurrentProgramInfo(&prog)) {
            m_state.hasProgramInfo = true;
            m_state.programName    = nameB;
            m_state.programText    = textB;
        } else {
            m_state.hasProgramInfo = false;
            m_state.programName.clear();
            m_state.programText.clear();
        }
    }

    TVTestState SnapState() const
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        return m_state;
    }

    // -------------------------------------------------------------------------
    // HTTP サーバー
    // -------------------------------------------------------------------------

    void StartServer()
    {
        if (m_serverStarted) return;
        if (!m_routesConfigured) {
            SetupRoutes();
            m_routesConfigured = true;
        }
        int port = m_settings.port;

        if (!m_httpServer.bind_to_port("0.0.0.0", port)) {
            m_httpServer.stop();
            wchar_t msg[128];
            ::swprintf_s(msg, L"TVTestHTTPPlugin: ポート %d へのバインドに失敗しました。他のプロセスが使用中の可能性があります。", port);
            m_pApp->AddLog(msg, TVTest::LOG_TYPE_ERROR);
            return;
        }

        m_serverStarted = true;
        m_httpThread = std::thread([this] {
            m_httpServer.listen_after_bind();
        });
    }

    void StopServer()
    {
        if (!m_serverStarted) return;
        m_httpServer.stop();
        if (m_httpThread.joinable()) m_httpThread.join();
        m_serverStarted = false;
    }

    void SetupRoutes()
    {
        m_httpServer.set_pre_routing_handler(
            [this](const httplib::Request &req, httplib::Response &res)
                -> httplib::Server::HandlerResponse
            {
                if (!IsIpAllowed(req.remote_addr)) {
                    TVTestHTTP::SendJson(res, R"({"error":"Forbidden"})", 403);
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            });

        m_httpServer.Options(".*", [](const httplib::Request &, httplib::Response &res) {
            TVTestHTTP::SetCorsHeaders(res);
        });

        TVTestHTTP::RegisterStateRoutes(
            m_httpServer,
            [this] { return SnapState(); },
            TVTest::RECORD_STATUS_RECORDING);

        TVTestHTTP::DynamicRouteContext dynCtx;
        dynCtx.app       = m_pApp;
        dynCtx.snapState = [this] { return SnapState(); };
        dynCtx.dispatch  = [this](std::shared_ptr<WriteRequest> req) { Dispatch(req); };
        TVTestHTTP::RegisterDynamicRoutes(m_httpServer, dynCtx);
    }

    // -------------------------------------------------------------------------
    // 設定の読み書き (メインスレッドから呼ぶこと)
    // -------------------------------------------------------------------------

    void LoadSettings()  { m_settings = TVTestHTTP::LoadPluginSettings(); }
    void SaveSettings() const { TVTestHTTP::SavePluginSettings(m_settings); }

    void UpdateCidrBlocks()
    {
        auto newAllow = ParseCidrList(m_settings.allowList);
        auto newDeny  = ParseCidrList(m_settings.denyList);
        std::lock_guard<std::mutex> lk(m_settingsMutex);
        m_allowBlocks = std::move(newAllow);
        m_denyBlocks  = std::move(newDeny);
    }

    bool IsIpAllowed(const std::string &ip) const
    {
        std::vector<CidrBlock> allows, denies;
        {
            std::lock_guard<std::mutex> lk(m_settingsMutex);
            allows = m_allowBlocks;
            denies = m_denyBlocks;
        }
        if (!denies.empty() && IpMatchesList(ip, denies)) return false;
        if (!allows.empty() && !IpMatchesList(ip, allows)) return false;
        return true;
    }

    // -------------------------------------------------------------------------
    // 設定ダイアログ (メインスレッドから呼ぶこと)
    // -------------------------------------------------------------------------

    bool OnPluginSettings(HWND hwndOwner)
    {
        SettingsDlgData data;
        data.settings = m_settings;

        INT_PTR result = DialogBoxParamW(
            g_hinstDLL,
            MAKEINTRESOURCEW(IDD_SETTINGS),
            hwndOwner,
            SettingsDlgProc,
            reinterpret_cast<LPARAM>(&data));

        if (result != IDOK) return false;

        bool portChanged = (data.settings.port != m_settings.port);
        m_settings = data.settings;
        SaveSettings();
        UpdateCidrBlocks();

        if (portChanged && m_serverStarted) {
            StopServer();
            StartServer();
        }
        return true;
    }
};

// =============================================================================
// プラグインクラスファクトリ
// =============================================================================
TVTest::CTVTestPlugin *CreatePluginClass() { return new CTVTestHTTPPlugin; }
