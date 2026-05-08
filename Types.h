#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <queue>
#include <memory>

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
    WORD eventId    = 0;     // hasEventId == true のときのみ有効
    bool hasEventId = false; // true: EPG_EVENT_QUERY_EVENTID / false: EPG_EVENT_QUERY_TIME
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
        TTREC_RESERVE_DEFAULT,
    };

    Type         type;
    int          remoteControlKey = 0;
    int          space            = 0;
    int          channel          = 0;
    int          volume           = 0;
    bool         mute             = false;
    std::wstring driverName;
    bool         hasChannel       = false; // ドライバ切り替え後にチャンネルを指定するか
    EpgQuery      epgQuery        = {};
    WORD          eventId         = 0;
    SYSTEMTIME    startTime       = {};
    DWORD         duration        = 0;

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
// 設定
// =============================================================================

struct PluginSettings {
    int          port      = HTTP_PORT_DEFAULT;
    std::wstring allowList; // カンマ区切り CIDR (例: 192.168.1.0/24,10.0.0.0/8)
    std::wstring denyList;  // カンマ区切り CIDR
};

// =============================================================================
// IP フィルター (CIDR マッチング)
// =============================================================================

struct CidrBlock {
    uint32_t addr = 0; // ネットワークアドレス (ホストバイトオーダー)
    uint32_t mask = 0; // サブネットマスク (ホストバイトオーダー)
};
