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

// =============================================================================
// 定数
// =============================================================================

static const int    HTTP_PORT   = 40152;
static const UINT_PTR TIMER_ID  = 40152u;
static const UINT   TIMER_MS    = 50u;   // 書き込みリクエスト処理間隔

// =============================================================================
// 状態キャッシュ (メインスレッドで書き、HTTP スレッドで読む)
// =============================================================================

struct ChannelEntry {
    int          space;
    int          channel;
    int          remoteControlKey;
    int          serviceID;
    int          networkID;
    int          tsID;
    std::wstring name;
    std::wstring networkName;
};

// EPG クエリ用 ID セット
struct EpgQuery {
    WORD networkId  = 0;
    WORD tsId       = 0;
    WORD serviceId  = 0;
};

struct TVTestState {
    // チャンネル
    bool         hasChannel       = false;
    int          space            = 0;
    int          channel          = 0;
    int          remoteControlKey = 0;
    int          serviceID        = 0;
    int          networkID        = 0;
    int          tsID             = 0;
    std::wstring channelName;
    std::wstring networkName;

    // 音量
    int          volume           = 100;
    bool         mute             = false;

    // 録画
    int          recordStatus     = 0;   // RECORD_STATUS_*

    // 現在番組
    bool         hasProgramInfo   = false;
    std::wstring programName;
    std::wstring programText;

    // チャンネル一覧 (初期化時に取得)
    std::vector<ChannelEntry> channelList;
};

// =============================================================================
// 書き込みリクエスト (HTTP スレッド → メインスレッド)
// =============================================================================

struct WriteRequest {
    enum class Type {
        SET_CHANNEL_RCK,    // リモコンキー番号でチャンネル変更
        SET_CHANNEL_SPACE,  // space + channel インデックスで変更
        SET_VOLUME,
        SET_MUTE,
        START_RECORD,
        STOP_RECORD,
        SET_DRIVER,
        GET_EPG_EVENT,      // 任意チャンネルの現在番組取得 (単体または複数)
    };

    Type         type;
    int          remoteControlKey = 0;
    int          space            = 0;
    int          channel          = 0;
    int          volume           = 0;
    bool         mute             = false;
    std::wstring driverName;
    bool         hasChannel       = false; // ドライバ切り替え後にチャンネルを指定するか

    // GET_EPG_EVENT 用
    std::vector<EpgQuery> epgQueries;  // 取得対象 (1件または複数)
    std::string           epgResultJson; // 結果 JSON (単体は object, 複数は array)

    // 結果 (メインスレッドが書き, HTTP スレッドが読む)
    bool         success          = false;
    std::string  responseJson;
    HANDLE       hDone;

    WriteRequest()  : hDone(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~WriteRequest() { if (hDone) CloseHandle(hDone); }

    // コピー禁止
    WriteRequest(const WriteRequest &)            = delete;
    WriteRequest &operator=(const WriteRequest &) = delete;
};

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
    static const int PROGRAM_REFRESH_INTERVAL = 40;

public:
    CTVTestHTTPPlugin()  { s_pPlugin = this; }
    ~CTVTestHTTPPlugin() { s_pPlugin = nullptr; StopServer(); }

    // -------------------------------------------------------------------------
    // TVTest::CTVTestPlugin 実装
    // -------------------------------------------------------------------------

    bool GetPluginInfo(TVTest::PluginInfo *pInfo) override
    {
        pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
        pInfo->Flags          = TVTest::PLUGIN_FLAG_ENABLEDEFAULT;
        pInfo->pszPluginName  = L"TVTest HTTP API";
        pInfo->pszCopyright   = L"(c) 2026";
        pInfo->pszDescription = L"HTTP REST API でTVTestを外部から制御します (port 40152)";
        return true;
    }

    bool Initialize() override
    {
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
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);

            const auto &queries = req.epgQueries;
            if (queries.size() == 1) {
                // 単体 → JSON オブジェクト
                const auto &q = queries[0];
                TVTest::EpgEventQueryInfo qi = {};
                qi.NetworkID         = q.networkId;
                qi.TransportStreamID = q.tsId;
                qi.ServiceID         = q.serviceId;
                qi.Type              = TVTest::EPG_EVENT_QUERY_TIME;
                qi.Flags             = 0;
                qi.Time              = ft;
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
                    qi.Type              = TVTest::EPG_EVENT_QUERY_TIME;
                    qi.Flags             = 0;
                    qi.Time              = ft;
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
        m_httpThread = std::thread([this] {
            m_httpServer.listen("0.0.0.0", HTTP_PORT);
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
        // CORS プリフライト
        m_httpServer.Options(".*", [](const httplib::Request &, httplib::Response &res) {
            Cors(res);
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
};

// =============================================================================
// プラグインクラスファクトリ (SDK マクロが呼び出す)
// =============================================================================
TVTest::CTVTestPlugin *CreatePluginClass() { return new CTVTestHTTPPlugin; }
