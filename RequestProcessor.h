#pragma once

// Requires TVTestPlugin.h and EpgHelper.h to be included before this header

#include "Types.h"
#include "Constants.h"

#include <functional>
#include <mutex>

// =============================================================================
// 書き込みリクエストの処理 (メインスレッドから呼ぶこと)
// =============================================================================

struct RequestProcessContext {
    TVTest::CTVTestApp   *app;
    std::mutex           *stateMutex;
    TVTestState          *state;
    std::function<void()> refreshChannel;
    std::function<void()> refreshChannelList;
};

static void ProcessWriteRequest(WriteRequest &req, const RequestProcessContext &ctx)
{
    switch (req.type) {

    case WriteRequest::Type::SET_CHANNEL_RCK: {
        bool found = false;
        std::vector<ChannelEntry> list;
        {
            std::lock_guard<std::mutex> lk(*ctx.stateMutex);
            list = ctx.state->channelList;
        }
        for (const auto &e : list) {
            if (e.remoteControlKey == req.remoteControlKey) {
                req.success = ctx.app->SetChannel(e.space, e.channel) != FALSE;
                found = true;
                break;
            }
        }
        if (!found) {
            req.success      = false;
            req.responseJson = R"({"error":"指定リモコンキーのチャンネルが見つかりません"})";
        } else if (req.success) {
            req.responseJson = R"({"success":true})";
            ctx.refreshChannel();
        } else {
            req.responseJson = R"({"error":"チャンネル変更に失敗しました"})";
        }
        break;
    }

    case WriteRequest::Type::SET_CHANNEL_SPACE:
        req.success = ctx.app->SetChannel(req.space, req.channel) != FALSE;
        if (req.success) {
            req.responseJson = R"({"success":true})";
            ctx.refreshChannel();
        } else {
            req.responseJson = R"({"error":"チャンネル変更に失敗しました"})";
        }
        break;

    case WriteRequest::Type::SET_VOLUME:
        req.success      = ctx.app->SetVolume(req.volume) != FALSE;
        req.responseJson = req.success ? R"({"success":true})"
                                       : R"({"error":"音量設定に失敗しました"})";
        break;

    case WriteRequest::Type::SET_MUTE:
        req.success      = ctx.app->SetMute(req.mute) != FALSE;
        req.responseJson = req.success ? R"({"success":true})"
                                       : R"({"error":"ミュート設定に失敗しました"})";
        break;

    case WriteRequest::Type::START_RECORD: {
        TVTest::RecordInfo ri = {};
        ri.Size          = sizeof(ri);
        ri.Mask          = 0;
        ri.StartTimeSpec = TVTest::RECORD_START_NOTSPECIFIED;
        ri.StopTimeSpec  = TVTest::RECORD_STOP_NOTSPECIFIED;
        req.success      = ctx.app->StartRecord(&ri) != FALSE;
        req.responseJson = req.success ? R"({"success":true})"
                                       : R"({"error":"録画開始に失敗しました"})";
        break;
    }

    case WriteRequest::Type::STOP_RECORD:
        req.success      = ctx.app->StopRecord() != FALSE;
        req.responseJson = req.success ? R"({"success":true})"
                                       : R"({"error":"録画停止に失敗しました"})";
        break;

    case WriteRequest::Type::SET_DRIVER:
        req.success = ctx.app->SetDriverName(req.driverName.c_str());
        if (req.success) {
            ctx.refreshChannelList();
            if (req.hasChannel) {
                if (req.remoteControlKey != 0) {
                    bool found = false;
                    std::vector<ChannelEntry> list;
                    {
                        std::lock_guard<std::mutex> lk(*ctx.stateMutex);
                        list = ctx.state->channelList;
                    }
                    for (const auto &e : list) {
                        if (e.remoteControlKey == req.remoteControlKey) {
                            ctx.app->SetChannel(e.space, e.channel);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        req.responseJson = R"({"error":"ドライバ切り替え成功。ただし指定リモコンキーが見つかりません"})";
                        ctx.refreshChannel();
                        break;
                    }
                } else {
                    ctx.app->SetChannel(req.space, req.channel);
                }
            }
            ctx.refreshChannel();
            req.responseJson = R"({"success":true})";
        } else {
            req.responseJson = R"({"error":"BonDriver の切り替えに失敗しました"})";
        }
        break;

    case WriteRequest::Type::GET_EPG_EVENT: {
        FILETIME ft = {};
        if ([&]{ for (const auto &q : req.epgQueries) if (!q.hasEventId) return true; return false; }())
            GetSystemTimeAsFileTime(&ft);

        auto queryOne = [&](const EpgQuery &q) -> nlohmann::json {
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
            TVTest::EpgEventInfo *pEvent = ctx.app->GetEpgEventInfo(&qi);
            auto j = BuildEpgEventJson(q, pEvent);
            if (pEvent) ctx.app->FreeEpgEventInfo(pEvent);
            return j;
        };

        const auto &queries = req.epgQueries;
        if (queries.size() == 1) {
            req.epgResultJson = queryOne(queries[0]).dump();
        } else {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto &q : queries) arr.push_back(queryOne(q));
            req.epgResultJson = arr.dump();
        }
        req.success = true;
        break;
    }

    case WriteRequest::Type::TTREC_RESERVE_DEFAULT: {
        {
            TVTest::EpgEventQueryInfo qi = {};
            qi.NetworkID         = req.epgQuery.networkId;
            qi.TransportStreamID = req.epgQuery.tsId;
            qi.ServiceID         = req.epgQuery.serviceId;
            qi.Type              = TVTest::EPG_EVENT_QUERY_EVENTID;
            qi.EventID           = req.eventId;
            qi.Flags             = 0;
            TVTest::EpgEventInfo *pEvent = ctx.app->GetEpgEventInfo(&qi);
            if (!pEvent) {
                req.responseJson =
                    R"({"error":"指定された番組の EPG 情報が見つかりません。TVTest の番組表が取得されているか確認してください"})";
                break;
            }
            ctx.app->FreeEpgEventInfo(pEvent);
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
        param.ID                          = TTREC_COMMAND_RESERVE_DEFAULT;
        param.Action                      = TVTest::PROGRAMGUIDE_COMMAND_ACTION_MOUSE;
        param.Program.NetworkID           = req.epgQuery.networkId;
        param.Program.TransportStreamID   = req.epgQuery.tsId;
        param.Program.ServiceID           = req.epgQuery.serviceId;
        param.Program.EventID             = req.eventId;
        param.Program.StartTime           = req.startTime;
        param.Program.Duration            = req.duration;

        LRESULT rv = ::SendMessage(
            hwndTTRec,
            WM_TTREC_EVENT_PROGRAMGUIDE_COMMAND,
            TTREC_COMMAND_RESERVE_DEFAULT,
            reinterpret_cast<LPARAM>(&param));

        req.success      = rv != FALSE;
        req.responseJson = req.success
            ? R"({"success":true,"mode":"default","note":"TTRec のデフォルト予約設定に従って追加しました。TTRec 側のデフォルトが「見るだけ」の場合は見るだけ予約になります"})"
            : R"({"error":"TTRec への予約追加に失敗しました。TTRec が有効で、対象番組の EPG 情報が利用可能か確認してください"})";
        break;
    }
    }

    SetEvent(req.hDone);
}
