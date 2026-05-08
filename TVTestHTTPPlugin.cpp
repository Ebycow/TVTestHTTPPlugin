/**
 * TVTestHTTPPlugin.cpp
 * TVTest HTTP API Plugin v1.0
 *
 * TVTest を HTTP REST API 経由で外部から制御するプラグイン。
 * デフォルトポート: 40152
 *
 * エンドポイント:
 *   GET  /api/status                現在の状態（チャンネル・音量・録画・番組）
 *   GET  /api/channels              チャンネル一覧
 *   POST /api/channel               チャンネル変更
 *   GET  /api/volume                音量取得
 *   POST /api/volume                音量・ミュート設定
 *   GET  /api/program               現在番組情報
 *   GET  /api/program/channel       任意チャンネルの現在番組取得
 *   POST /api/program/channels      複数チャンネルの現在番組一括取得
 *   GET  /api/record/status         録画状態
 *   POST /api/record/start          録画開始
 *   POST /api/record/stop           録画停止
 *   GET  /api/ttrec/reserves         TTRec の予約一覧取得
 *   POST /api/ttrec/reserve/default TTRec のデフォルト設定で予約追加
 *
 * GET /api/program/channel クエリパラメータ (いずれか):
 *   ?space=0&channel=5
 *   ?networkId=32736&serviceId=1024
 *   ?networkId=32736&serviceId=1024&transportStreamId=32736
 *
 * POST /api/program/channels ボディ (JSON配列):
 *   [{"space":0,"channel":5}, {"networkId":32736,"serviceId":1024}]
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
// objbase.h が basetyps.h 経由で `interface` を struct として定義する
#include <objbase.h>

// TVTest Plugin SDK
// ・Shift-JIS コメント警告 C4828 を抑制
// ・httplib.h より先にインクルード必須
//   (httplib.h が NOMINMAX を定義して windows.h の min/max マクロを消す前に
//    TVTestPlugin.h を通しておく必要があるため)
#pragma warning(push)
#pragma warning(disable: 4828)
#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"
#pragma warning(pop)

// cpp-httplib (TVTestPlugin.h の後にインクルード)
#include "httplib.h"

#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <memory>
#include <vector>

#include "resource.h"

#include "Constants.h"
#include "Types.h"
#include "JsonHelpers.h"
#include "EpgHelper.h"
#include "IpFilter.h"
#include "Settings.h"
#include "TtrecHelper.h"

// =============================================================================
// プラグイン本体
// =============================================================================

// タイマーコールバックから this を取得するための静的ポインタ
// (プラグインは TVTest に 1 インスタンスのみ)
static class CTVTestHTTPPlugin *s_pPlugin = nullptr;

class CTVTestHTTPPlugin : public TVTest::CTVTestPlugin
{
    // HTTP サーバー
    httplib::Server   m_httpServer;
    std::thread       m_httpThread;
    bool              m_serverStarted = false;

    // 状態キャッシュ
    mutable std::mutex m_stateMutex;
    TVTestState        m_state;

    // 書き込みリクエストキュー
    std::mutex                                    m_queueMutex;
    std::queue<std::shared_ptr<WriteRequest>>     m_requestQueue;

    // 番組情報定期更新カウンター (50ms × 40 = 2秒ごとに RefreshProgram)
    int m_programRefreshTick = 0;

    // 設定 (メインスレッドから読み書き; HTTP スレッドからは m_settingsMutex で保護)
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
        // 起動時に既に有効状態の場合、EVENT_PLUGINENABLE が来ないのでここで起動する
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
            if (p1) {
                self->RefreshAll();
                self->StartServer();
            } else {
                self->StopServer();
            }
            return TRUE;

        case TVTest::EVENT_PLUGINSETTINGS:
            return self->OnPluginSettings(reinterpret_cast<HWND>(p1)) ? TRUE : FALSE;

        case TVTest::EVENT_CHANNELCHANGE:
            // チャンネルリストが未取得の場合（起動時に空だった）はここで取得する
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

        // 2秒ごとに番組情報を更新 (EPG受信後の反映に対応)
        if (++m_programRefreshTick >= PROGRAM_REFRESH_INTERVAL) {
            m_programRefreshTick = 0;
            RefreshProgram();
        }
    }

    void ProcessRequest(WriteRequest &req)
    {
        switch (req.type) {

        case WriteRequest::Type::SET_CHANNEL_RCK: {
            bool found = false;
            std::vector<ChannelEntry> list;
            {
                std::lock_guard<std::mutex> lk(m_stateMutex);
                list = m_state.channelList;
            }
            for (const auto &e : list) {
                if (e.remoteControlKey == req.remoteControlKey) {
                    req.success = m_pApp->SetChannel(e.space, e.channel) != FALSE;
                    found = true;
                    break;
                }
            }
            if (!found) {
                req.success      = false;
                req.responseJson = R"({"error":"指定リモコンキーのチャンネルが見つかりません"})";
            } else if (req.success) {
                req.responseJson = R"({"success":true})";
                RefreshChannel();
            } else {
                req.responseJson = R"({"error":"チャンネル変更に失敗しました"})";
            }
            break;
        }

        case WriteRequest::Type::SET_CHANNEL_SPACE:
            req.success = m_pApp->SetChannel(req.space, req.channel) != FALSE;
            if (req.success) {
                req.responseJson = R"({"success":true})";
                RefreshChannel();
            } else {
                req.responseJson = R"({"error":"チャンネル変更に失敗しました"})";
            }
            break;

        case WriteRequest::Type::SET_VOLUME:
            req.success      = m_pApp->SetVolume(req.volume) != FALSE;
            req.responseJson = req.success ? R"({"success":true})"
                                           : R"({"error":"音量設定に失敗しました"})";
            break;

        case WriteRequest::Type::SET_MUTE:
            req.success      = m_pApp->SetMute(req.mute) != FALSE;
            req.responseJson = req.success ? R"({"success":true})"
                                           : R"({"error":"ミュート設定に失敗しました"})";
            break;

        case WriteRequest::Type::START_RECORD: {
            TVTest::RecordInfo ri = {};
            ri.Size          = sizeof(ri);
            ri.Mask          = 0;
            ri.StartTimeSpec = TVTest::RECORD_START_NOTSPECIFIED;
            ri.StopTimeSpec  = TVTest::RECORD_STOP_NOTSPECIFIED;
            req.success      = m_pApp->StartRecord(&ri) != FALSE;
            req.responseJson = req.success ? R"({"success":true})"
                                           : R"({"error":"録画開始に失敗しました"})";
            break;
        }

        case WriteRequest::Type::SET_DRIVER:
            req.success = m_pApp->SetDriverName(req.driverName.c_str());
            if (req.success) {
                RefreshChannelList();
                // チャンネル指定があれば続けてチューニング
                if (req.hasChannel) {
                    if (req.remoteControlKey != 0) {
                        // remoteControlKey で検索
                        bool found = false;
                        std::vector<ChannelEntry> list;
                        {
                            std::lock_guard<std::mutex> lk(m_stateMutex);
                            list = m_state.channelList;
                        }
                        for (const auto &e : list) {
                            if (e.remoteControlKey == req.remoteControlKey) {
                                m_pApp->SetChannel(e.space, e.channel);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            req.responseJson = R"({"error":"ドライバ切り替え成功。ただし指定リモコンキーが見つかりません"})";
                            RefreshChannel();
                            break;
                        }
                    } else {
                        m_pApp->SetChannel(req.space, req.channel);
                    }
                }
                RefreshChannel();
                req.responseJson = R"({"success":true})";
            } else {
                req.responseJson = R"({"error":"BonDriver の切り替えに失敗しました"})";
            }
            break;

        case WriteRequest::Type::STOP_RECORD:
            req.success      = m_pApp->StopRecord() != FALSE;
            req.responseJson = req.success ? R"({"success":true})"
                                           : R"({"error":"録画停止に失敗しました"})";
            break;

        case WriteRequest::Type::GET_EPG_EVENT: {
            FILETIME ft = {};
            if ([&]{ for (const auto &q : req.epgQueries) if (!q.hasEventId) return true; return false; }())
                GetSystemTimeAsFileTime(&ft);

            const auto &queries = req.epgQueries;
            if (queries.size() == 1) {
                // 単体 → JSON オブジェクト
                const auto &q = queries[0];
                TVTest::EpgEventQueryInfo qi = {};
                qi.NetworkID         = q.networkId;
                qi.TransportStreamID = q.tsId;
                qi.ServiceID         = q.serviceId;
                qi.Flags             = 0;
                if (q.hasEventId) {
                    qi.Type    = TVTest::EPG_EVENT_QUERY_EVENTID;
                    qi.EventID = q.eventId;
                } else {
                    qi.Type  = TVTest::EPG_EVENT_QUERY_TIME;
                    qi.Time  = ft;
                }
                TVTest::EpgEventInfo *pEvent = m_pApp->GetEpgEventInfo(&qi);
                req.epgResultJson = BuildEpgSingleJson(q, pEvent);
                if (pEvent) m_pApp->FreeEpgEventInfo(pEvent);
            } else {
                // 複数 → JSON 配列
                std::ostringstream j;
                j << "[";
                bool first = true;
                for (const auto &q : queries) {
                    if (!first) j << ",";
                    first = false;
                    TVTest::EpgEventQueryInfo qi = {};
                    qi.NetworkID         = q.networkId;
                    qi.TransportStreamID = q.tsId;
                    qi.ServiceID         = q.serviceId;
                    qi.Flags             = 0;
                    if (q.hasEventId) {
                        qi.Type    = TVTest::EPG_EVENT_QUERY_EVENTID;
                        qi.EventID = q.eventId;
                    } else {
                        qi.Type  = TVTest::EPG_EVENT_QUERY_TIME;
                        qi.Time  = ft;
                    }
                    TVTest::EpgEventInfo *pEvent = m_pApp->GetEpgEventInfo(&qi);
                    j << BuildEpgSingleJson(q, pEvent);
                    if (pEvent) m_pApp->FreeEpgEventInfo(pEvent);
                }
                j << "]";
                req.epgResultJson = j.str();
            }
            req.success = true;
            break;
        }

        case WriteRequest::Type::TTREC_RESERVE_DEFAULT: {
            // TVTest の EPG に対象イベントが存在するか確認する
            {
                TVTest::EpgEventQueryInfo qi = {};
                qi.NetworkID         = req.epgQuery.networkId;
                qi.TransportStreamID = req.epgQuery.tsId;
                qi.ServiceID         = req.epgQuery.serviceId;
                qi.Type              = TVTest::EPG_EVENT_QUERY_EVENTID;
                qi.EventID           = req.eventId;
                qi.Flags             = 0;
                TVTest::EpgEventInfo *pEvent = m_pApp->GetEpgEventInfo(&qi);
                if (!pEvent) {
                    req.responseJson =
                        R"({"error":"指定された番組の EPG 情報が見つかりません。TVTest の番組表が取得されているか確認してください"})";
                    break;
                }
                m_pApp->FreeEpgEventInfo(pEvent);
            }

            HWND hwndTTRec = FindTTRecWindowInCurrentProcess();
            if (!hwndTTRec) {
                req.responseJson =
                    R"({"error":"TTRec が見つかりません。TVTest に TTRec プラグインを導入して有効化してください"})";
                break;
            }

            LRESULT msgVer = ::SendMessage(hwndTTRec, WM_TTREC_GET_MSGVER, 0, 0);
            if (msgVer != TTREC_CURRENT_MSGVER) {
                req.responseJson =
                    R"({"error":"TTRec のメッセージ互換バージョンが一致しません"})";
                break;
            }

            TVTest::ProgramGuideCommandParam param = {};
            param.ID = TTREC_COMMAND_RESERVE_DEFAULT;
            param.Action = TVTest::PROGRAMGUIDE_COMMAND_ACTION_MOUSE;
            param.Program.NetworkID = req.epgQuery.networkId;
            param.Program.TransportStreamID = req.epgQuery.tsId;
            param.Program.ServiceID = req.epgQuery.serviceId;
            param.Program.EventID = req.eventId;
            param.Program.StartTime = req.startTime;
            param.Program.Duration = req.duration;

            LRESULT rv = ::SendMessage(
                hwndTTRec,
                WM_TTREC_EVENT_PROGRAMGUIDE_COMMAND,
                TTREC_COMMAND_RESERVE_DEFAULT,
                reinterpret_cast<LPARAM>(&param));

            req.success = rv != FALSE;
            req.responseJson = req.success
                ? R"({"success":true,"mode":"default","note":"TTRec のデフォルト予約設定に従って追加しました。TTRec 側のデフォルトが「見るだけ」の場合は見るだけ予約になります"})"
                : R"({"error":"TTRec への予約追加に失敗しました。TTRec が有効で、対象番組の EPG 情報が利用可能か確認してください"})";
            break;
        }
        }

        SetEvent(req.hDone);
    }

    // HTTP スレッドからリクエストを投げてメインスレッドの処理を待つ
    void Dispatch(std::shared_ptr<WriteRequest> req)
    {
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            m_requestQueue.push(req);
        }
        // 最大 3 秒待機 (タイマー 50ms なので通常は即座)
        if (WaitForSingleObject(req->hDone, 3000) != WAIT_OBJECT_0) {
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
        SetupRoutes();
        m_serverStarted = true;
        int port = m_settings.port;  // ← 設定されたポートを使用
        m_httpThread = std::thread([this, port] {
            m_httpServer.listen("0.0.0.0", port);
        });
    }

    void StopServer()
    {
        if (!m_serverStarted) return;
        m_httpServer.stop();
        if (m_httpThread.joinable()) m_httpThread.join();
        m_serverStarted = false;
    }

    static void Cors(httplib::Response &res)
    {
        res.set_header("Access-Control-Allow-Origin",  "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
    }

    static void Json(httplib::Response &res, const std::string &body, int status = 200)
    {
        Cors(res);
        res.status = status;
        res.set_content(body, "application/json; charset=utf-8");
    }

    void SetupRoutes()
    {
        // IP フィルター: 全リクエストに先立って許可/拒否を確認
        m_httpServer.set_pre_routing_handler(
            [this](const httplib::Request &req, httplib::Response &res)
                -> httplib::Server::HandlerResponse
            {
                if (!IsIpAllowed(req.remote_addr)) {
                    Json(res, R"({"error":"Forbidden"})", 403);
                    return httplib::Server::HandlerResponse::Handled;
                }
                return httplib::Server::HandlerResponse::Unhandled;
            });

        // CORS プリフライト
        m_httpServer.Options(".*", [](const httplib::Request &, httplib::Response &res) {
            Cors(res);
        });

        // ------------------------------------------------------------------
        // POST /api/ttrec/reserve/default
        // Body:
        // {
        //   "onid":32736,
        //   "tsid":32736,
        //   "sid":1024,
        //   "eid":12345,
        //   "startTime":"2026-03-26T02:00:00",
        //   "duration":1800
        // }
        // EDCB と共通の onid/tsid/sid/eid 形式を優先して受け付ける。
        // networkId/serviceId/transportStreamId/eventId も後方互換で受け付ける。
        // TTRec のデフォルト予約設定に従って追加する。
        // ------------------------------------------------------------------
        m_httpServer.Post("/api/ttrec/reserve/default", [this](const httplib::Request &req, httplib::Response &res) {
            auto state = SnapState();
            EpgQuery q = {};
            if (!ResolveEpgQuery(req.body, state.channelList, q)) {
                Json(res, R"({"error":"onid+sid(+tsid) または networkId+serviceId(+transportStreamId) または space+channel が必要です"})", 400);
                return;
            }

            int eventId = ParseIntField(req.body, "eid");
            if (eventId == INT_MIN) eventId = ParseIntField(req.body, "eventId");
            int duration = ParseIntField(req.body, "duration");
            std::string startTimeText = ParseStrField(req.body, "startTime");
            if (eventId == INT_MIN || duration == INT_MIN || startTimeText.empty()) {
                Json(res, R"({"error":"eid(eventId)・startTime・duration が必要です"})", 400);
                return;
            }
            if (eventId < 0 || eventId > 0xFFFF || duration < 0) {
                Json(res, R"({"error":"eventId または duration の値が不正です"})", 400);
                return;
            }

            SYSTEMTIME startTime = {};
            if (!ParseIso8601Local(startTimeText, startTime)) {
                Json(res, R"({"error":"startTime は YYYY-MM-DDTHH:MM:SS 形式で指定してください"})", 400);
                return;
            }

            auto wreq = std::make_shared<WriteRequest>();
            wreq->type = WriteRequest::Type::TTREC_RESERVE_DEFAULT;
            wreq->epgQuery = q;
            wreq->eventId = static_cast<WORD>(eventId);
            wreq->startTime = startTime;
            wreq->duration = static_cast<DWORD>(duration);
            Dispatch(wreq);
            Json(res, wreq->responseJson, wreq->success ? 200 : 500);
        });

        // ------------------------------------------------------------------
        // GET /api/ttrec/reserves
        // TTRec の予約一覧を JSON 配列で返す。
        // TTRec の HINSTANCE から _Reserves.txt のパスを解決して直接読み込む。
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/ttrec/reserves", [](const httplib::Request &, httplib::Response &res) {
            HWND hwndTTRec = FindTTRecWindowInCurrentProcess();
            if (!hwndTTRec) {
                Json(res, R"({"error":"TTRec が見つかりません。TVTest に TTRec プラグインを導入して有効化してください"})", 500);
                return;
            }

            // TTRec の HINSTANCE から DLL パスを取得し _Reserves.txt のパスを構築
            HINSTANCE hinstTTRec = reinterpret_cast<HINSTANCE>(
                ::GetWindowLongPtr(hwndTTRec, GWLP_HINSTANCE));
            wchar_t dllPath[MAX_PATH] = {};
            if (!::GetModuleFileNameW(hinstTTRec, dllPath, MAX_PATH)) {
                Json(res, R"({"error":"TTRec のパスを取得できません"})", 500);
                return;
            }
            wchar_t reservePath[MAX_PATH + 16] = {};
            ::wcscpy_s(reservePath, dllPath);
            wchar_t *dot = ::wcsrchr(reservePath, L'.');
            if (dot) *dot = L'\0';
            ::wcscat_s(reservePath, L"_Reserves.txt");

            // BOM 付き UTF-16 LE ファイルを読み込む
            HANDLE hFile = ::CreateFileW(reservePath, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) {
                Json(res, R"([])", 200);  // 予約ファイルなし = 予約なし
                return;
            }
            DWORD fileBytes = ::GetFileSize(hFile, nullptr);
            std::wstring content;
            if (fileBytes >= sizeof(wchar_t) * 2) {
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

            Json(res, BuildTtrecReservesJson(content), 200);
        });

        // ------------------------------------------------------------------
        // GET /api/status
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/status", [this](const httplib::Request &, httplib::Response &res) {
            auto s = SnapState();
            std::ostringstream j;
            j << "{";

            // channel
            if (s.hasChannel) {
                j << "\"channel\":{"
                  << "\"space\":"            << s.space            << ","
                  << "\"channel\":"          << s.channel          << ","
                  << "\"remoteControlKey\":" << s.remoteControlKey << ","
                  << "\"serviceId\":"        << s.serviceID        << ","
                  << "\"networkId\":"        << s.networkID        << ","
                  << "\"transportStreamId\":" << s.tsID            << ","
                  << "\"name\":\""           << JsonStr(s.channelName)  << "\","
                  << "\"networkName\":\""    << JsonStr(s.networkName)  << "\""
                  << "},";
            } else {
                j << "\"channel\":null,";
            }

            j << "\"volume\":"       << s.volume                              << ","
              << "\"mute\":"         << (s.mute ? "true" : "false")           << ","
              << "\"recordStatus\":" << s.recordStatus                        << ","
              << "\"recording\":"    << (s.recordStatus == TVTest::RECORD_STATUS_RECORDING ? "true" : "false");

            if (s.hasProgramInfo) {
                j << ",\"program\":{"
                  << "\"name\":\""  << JsonStr(s.programName) << "\","
                  << "\"text\":\""  << JsonStr(s.programText) << "\""
                  << "}";
            } else {
                j << ",\"program\":null";
            }
            j << "}";
            Json(res, j.str());
        });

        // ------------------------------------------------------------------
        // GET /api/channels
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/channels", [this](const httplib::Request &, httplib::Response &res) {
            auto s = SnapState();
            std::ostringstream j;
            j << "[";
            bool first = true;
            for (const auto &e : s.channelList) {
                if (!first) j << ",";
                first = false;
                j << "{"
                  << "\"space\":"            << e.space            << ","
                  << "\"channel\":"          << e.channel          << ","
                  << "\"remoteControlKey\":" << e.remoteControlKey << ","
                  << "\"serviceId\":"        << e.serviceID        << ","
                  << "\"networkId\":"        << e.networkID        << ","
                  << "\"name\":\""           << JsonStr(e.name)        << "\","
                  << "\"networkName\":\""    << JsonStr(e.networkName) << "\""
                  << "}";
            }
            j << "]";
            Json(res, j.str());
        });

        // ------------------------------------------------------------------
        // POST /api/channel
        // Body: {"remoteControlKey":3}
        //    or {"space":0,"channel":5}
        // ------------------------------------------------------------------
        m_httpServer.Post("/api/channel", [this](const httplib::Request &req, httplib::Response &res) {
            const auto &body = req.body;
            auto wreq = std::make_shared<WriteRequest>();

            int rck = ParseIntField(body, "remoteControlKey");
            if (rck != INT_MIN) {
                wreq->type            = WriteRequest::Type::SET_CHANNEL_RCK;
                wreq->remoteControlKey = rck;
            } else {
                int sp = ParseIntField(body, "space");
                int ch = ParseIntField(body, "channel");
                if (sp == INT_MIN || ch == INT_MIN) {
                    Json(res, R"({"error":"remoteControlKey または space+channel が必要です"})", 400);
                    return;
                }
                wreq->type    = WriteRequest::Type::SET_CHANNEL_SPACE;
                wreq->space   = sp;
                wreq->channel = ch;
            }
            Dispatch(wreq);
            Json(res, wreq->responseJson, wreq->success ? 200 : 500);
        });

        // ------------------------------------------------------------------
        // GET /api/volume
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/volume", [this](const httplib::Request &, httplib::Response &res) {
            auto s = SnapState();
            std::ostringstream j;
            j << "{\"volume\":" << s.volume
              << ",\"mute\":"   << (s.mute ? "true" : "false") << "}";
            Json(res, j.str());
        });

        // ------------------------------------------------------------------
        // POST /api/volume
        // Body: {"volume":50} or {"mute":true} or both
        // ------------------------------------------------------------------
        m_httpServer.Post("/api/volume", [this](const httplib::Request &req, httplib::Response &res) {
            const auto &body = req.body;

            int vol = ParseIntField(body, "volume");
            if (vol != INT_MIN) {
                if (vol < 0 || vol > 100) {
                    Json(res, R"({"error":"volume は 0〜100 の範囲です"})", 400);
                    return;
                }
                auto wreq   = std::make_shared<WriteRequest>();
                wreq->type  = WriteRequest::Type::SET_VOLUME;
                wreq->volume = vol;
                Dispatch(wreq);
                if (!wreq->success) { Json(res, wreq->responseJson, 500); return; }
            }

            bool muteVal = false;
            if (ParseBoolField(body, "mute", muteVal)) {
                auto wreq  = std::make_shared<WriteRequest>();
                wreq->type = WriteRequest::Type::SET_MUTE;
                wreq->mute = muteVal;
                Dispatch(wreq);
                if (!wreq->success) { Json(res, wreq->responseJson, 500); return; }
            }

            Json(res, R"({"success":true})");
        });

        // ------------------------------------------------------------------
        // GET /api/program
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/program", [this](const httplib::Request &, httplib::Response &res) {
            auto s = SnapState();
            if (!s.hasProgramInfo) { Json(res, R"({"program":null})"); return; }
            std::ostringstream j;
            j << "{\"program\":{"
              << "\"name\":\""  << JsonStr(s.programName) << "\","
              << "\"text\":\""  << JsonStr(s.programText) << "\""
              << "}}";
            Json(res, j.str());
        });

        // ------------------------------------------------------------------
        // GET /api/program/channel
        // クエリパラメータ (いずれか):
        //   ?space=0&channel=5
        //   ?networkId=32736&serviceId=1024
        //   ?networkId=32736&serviceId=1024&transportStreamId=32736
        // オプション:
        //   &eventId=12345  指定すると EPG_EVENT_QUERY_EVENTID で当該イベントを取得
        //                   省略時は現在放送中の番組を取得 (EPG_EVENT_QUERY_TIME)
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/program/channel", [this](const httplib::Request &req, httplib::Response &res) {
            EpgQuery q = {};
            auto s = SnapState();

            if (req.has_param("space") && req.has_param("channel")) {
                // space + channel → channelList から ID を補完
                int sp = std::stoi(req.get_param_value("space"));
                int ch = std::stoi(req.get_param_value("channel"));
                bool found = false;
                for (const auto &e : s.channelList) {
                    if (e.space == sp && e.channel == ch) {
                        q.networkId = static_cast<WORD>(e.networkID);
                        q.tsId      = static_cast<WORD>(e.tsID);
                        q.serviceId = static_cast<WORD>(e.serviceID);
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    Json(res, R"({"error":"指定された space+channel が見つかりません"})", 404);
                    return;
                }
            } else if (req.has_param("networkId") && req.has_param("serviceId")) {
                q.networkId = static_cast<WORD>(std::stoi(req.get_param_value("networkId")));
                q.serviceId = static_cast<WORD>(std::stoi(req.get_param_value("serviceId")));
                if (req.has_param("transportStreamId")) {
                    q.tsId = static_cast<WORD>(std::stoi(req.get_param_value("transportStreamId")));
                } else {
                    // channelList から tsId を補完
                    for (const auto &e : s.channelList) {
                        if (e.networkID == q.networkId && e.serviceID == q.serviceId) {
                            q.tsId = static_cast<WORD>(e.tsID);
                            break;
                        }
                    }
                }
            } else {
                Json(res, R"({"error":"space+channel または networkId+serviceId が必要です"})", 400);
                return;
            }

            // eventId が指定されていれば EPG_EVENT_QUERY_EVENTID を使う
            if (req.has_param("eventId")) {
                q.eventId    = static_cast<WORD>(std::stoi(req.get_param_value("eventId")));
                q.hasEventId = true;
            }

            auto wreq = std::make_shared<WriteRequest>();
            wreq->type       = WriteRequest::Type::GET_EPG_EVENT;
            wreq->epgQueries = {q};
            Dispatch(wreq);
            Json(res, wreq->epgResultJson, wreq->success ? 200 : 500);
        });

        // ------------------------------------------------------------------
        // POST /api/program/channels
        // Body: JSON 配列
        //   [{"space":0,"channel":5}, {"networkId":32736,"serviceId":1024}, ...]
        // ------------------------------------------------------------------
        m_httpServer.Post("/api/program/channels", [this](const httplib::Request &req, httplib::Response &res) {
            auto s       = SnapState();
            auto queries = ParseEpgQueryArray(req.body, s.channelList);
            if (queries.empty()) {
                Json(res, R"({"error":"有効なチャンネル指定が1件もありません"})", 400);
                return;
            }
            auto wreq        = std::make_shared<WriteRequest>();
            wreq->type       = WriteRequest::Type::GET_EPG_EVENT;
            wreq->epgQueries = std::move(queries);
            Dispatch(wreq);
            Json(res, wreq->epgResultJson, wreq->success ? 200 : 500);
        });

        // ------------------------------------------------------------------
        // GET /api/record/status
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/record/status", [this](const httplib::Request &, httplib::Response &res) {
            auto s = SnapState();
            std::ostringstream j;
            j << "{\"status\":"    << s.recordStatus
              << ",\"recording\":" << (s.recordStatus == TVTest::RECORD_STATUS_RECORDING ? "true" : "false")
              << "}";
            Json(res, j.str());
        });

        // ------------------------------------------------------------------
        // POST /api/record/start
        // ------------------------------------------------------------------
        m_httpServer.Post("/api/record/start", [this](const httplib::Request &, httplib::Response &res) {
            auto wreq  = std::make_shared<WriteRequest>();
            wreq->type = WriteRequest::Type::START_RECORD;
            Dispatch(wreq);
            Json(res, wreq->responseJson, wreq->success ? 200 : 500);
        });

        // ------------------------------------------------------------------
        // GET /api/driver
        // 現在の BonDriver ファイル名と利用可能なドライバ一覧を返す
        // ------------------------------------------------------------------
        m_httpServer.Get("/api/driver", [this](const httplib::Request &, httplib::Response &res) {
            // 現在のドライバ名
            wchar_t cur[MAX_PATH] = {};
            m_pApp->GetDriverName(cur, MAX_PATH);

            // 利用可能なドライバを列挙
            std::ostringstream j;
            j << "{\"current\":\"" << JsonStr(std::wstring(cur)) << "\",\"drivers\":[";
            bool first = true;
            for (int i = 0; ; ++i) {
                wchar_t name[MAX_PATH] = {};
                if (m_pApp->EnumDriver(i, name, MAX_PATH) <= 0) break;
                if (!first) j << ",";
                first = false;
                j << "\"" << JsonStr(std::wstring(name)) << "\"";
            }
            j << "]}";
            Json(res, j.str());
        });

        // ------------------------------------------------------------------
        // POST /api/driver
        // Body: {"driver":"BonDriver_Proxy_S.dll"}
        //    or {"driver":"BonDriver_Proxy_S.dll","remoteControlKey":4}
        //    or {"driver":"BonDriver_Proxy_S.dll","space":0,"channel":0}
        // ------------------------------------------------------------------
        m_httpServer.Post("/api/driver", [this](const httplib::Request &req, httplib::Response &res) {
            std::string name = ParseStrField(req.body, "driver");
            if (name.empty()) {
                Json(res, R"({"error":"driver フィールドが必要です"})", 400);
                return;
            }
            auto wreq        = std::make_shared<WriteRequest>();
            wreq->type       = WriteRequest::Type::SET_DRIVER;
            wreq->driverName = StrToWStr(name);

            int rck = ParseIntField(req.body, "remoteControlKey");
            int sp  = ParseIntField(req.body, "space");
            int ch  = ParseIntField(req.body, "channel");
            if (rck != INT_MIN) {
                wreq->hasChannel       = true;
                wreq->remoteControlKey = rck;
            } else if (sp != INT_MIN && ch != INT_MIN) {
                wreq->hasChannel = true;
                wreq->space      = sp;
                wreq->channel    = ch;
            }

            Dispatch(wreq);
            Json(res, wreq->responseJson, wreq->success ? 200 : 500);
        });

        // ------------------------------------------------------------------
        // POST /api/record/stop
        // ------------------------------------------------------------------
        m_httpServer.Post("/api/record/stop", [this](const httplib::Request &, httplib::Response &res) {
            auto wreq  = std::make_shared<WriteRequest>();
            wreq->type = WriteRequest::Type::STOP_RECORD;
            Dispatch(wreq);
            Json(res, wreq->responseJson, wreq->success ? 200 : 500);
        });
    }

    // -------------------------------------------------------------------------
    // 設定の読み書き (メインスレッドから呼ぶこと)
    // -------------------------------------------------------------------------

    void LoadSettings()
    {
        std::wstring ini = GetIniFilePath();
        const wchar_t *sec = L"Settings";

        int port = static_cast<int>(
            GetPrivateProfileIntW(sec, L"Port", HTTP_PORT_DEFAULT, ini.c_str()));
        if (port < 1024 || port > 65535) port = HTTP_PORT_DEFAULT;
        m_settings.port = port;

        wchar_t buf[4096] = {};
        GetPrivateProfileStringW(sec, L"AllowList", L"", buf, _countof(buf), ini.c_str());
        m_settings.allowList = buf;

        GetPrivateProfileStringW(sec, L"DenyList", L"", buf, _countof(buf), ini.c_str());
        m_settings.denyList = buf;
    }

    void SaveSettings() const
    {
        std::wstring ini = GetIniFilePath();
        const wchar_t *sec = L"Settings";

        wchar_t portStr[16] = {};
        _itow_s(m_settings.port, portStr, 10);
        WritePrivateProfileStringW(sec, L"Port",      portStr,                  ini.c_str());
        WritePrivateProfileStringW(sec, L"AllowList", m_settings.allowList.c_str(), ini.c_str());
        WritePrivateProfileStringW(sec, L"DenyList",  m_settings.denyList.c_str(),  ini.c_str());
    }

    // CIDR ブロックリストを設定から再構築 (メインスレッドから呼ぶこと)
    void UpdateCidrBlocks()
    {
        auto newAllow = ParseCidrList(m_settings.allowList);
        auto newDeny  = ParseCidrList(m_settings.denyList);
        std::lock_guard<std::mutex> lk(m_settingsMutex);
        m_allowBlocks = std::move(newAllow);
        m_denyBlocks  = std::move(newDeny);
    }

    // 接続元 IP が許可されているか (HTTP スレッドから呼んでよい)
    bool IsIpAllowed(const std::string &ip) const
    {
        std::vector<CidrBlock> allows, denies;
        {
            std::lock_guard<std::mutex> lk(m_settingsMutex);
            allows = m_allowBlocks;
            denies = m_denyBlocks;
        }
        // 拒否リストに含まれていれば拒否
        if (!denies.empty() && IpMatchesList(ip, denies)) return false;
        // 許可リストが空でなければリストに含まれる場合のみ許可
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
            // ポート変更: サーバーを再起動
            StopServer();
            StartServer();
        }
        return true;
    }
};

// =============================================================================
// プラグインクラスファクトリ (SDK マクロが呼び出す)
// =============================================================================
TVTest::CTVTestPlugin *CreatePluginClass() { return new CTVTestHTTPPlugin; }
