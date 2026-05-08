#pragma once

// Requires TVTestPlugin.h, EpgHelper.h, HttpStateRoutes.h
// to be included before this header (TVTest SDK types and SendJson must be visible)

#include "Types.h"
#include "JsonHelpers.h"
#include "TtrecHelper.h"
#include "httplib.h"

#include <functional>
#include <memory>

namespace TVTestHTTP {

struct DynamicRouteContext {
    TVTest::CTVTestApp                                 *app;
    std::function<TVTestState()>                        snapState;
    std::function<void(std::shared_ptr<WriteRequest>)>  dispatch;
};

// GET /api/driver, GET /api/ttrec/reserves, POST /api/channel, POST /api/volume,
// POST /api/driver, POST /api/record/start, POST /api/record/stop,
// GET /api/program/channel, POST /api/program/channels,
// POST /api/ttrec/reserve/default
static void RegisterDynamicRoutes(httplib::Server &server, DynamicRouteContext ctx)
{
    // ------------------------------------------------------------------
    // GET /api/driver
    // ------------------------------------------------------------------
    server.Get("/api/driver", [ctx](const httplib::Request &, httplib::Response &res) {
        wchar_t cur[MAX_PATH] = {};
        ctx.app->GetDriverName(cur, MAX_PATH);

        std::vector<std::wstring> drivers;
        for (int i = 0; ; ++i) {
            wchar_t name[MAX_PATH] = {};
            if (ctx.app->EnumDriver(i, name, MAX_PATH) <= 0) break;
            drivers.emplace_back(name);
        }
        SendJson(res, BuildDriverJson(cur, drivers));
    });

    // ------------------------------------------------------------------
    // GET /api/ttrec/reserves
    // ------------------------------------------------------------------
    server.Get("/api/ttrec/reserves", [](const httplib::Request &, httplib::Response &res) {
        HWND hwndTTRec = FindTTRecWindowInCurrentProcess();
        if (!hwndTTRec) {
            SendJson(res, R"({"error":"TTRec が見つかりません。TVTest に TTRec プラグインを導入して有効化してください"})", 500);
            return;
        }
        std::string json;
        if (!LoadTtrecReservesJson(hwndTTRec, json)) {
            SendJson(res, R"({"error":"TTRec のパスを取得できません"})", 500);
            return;
        }
        SendJson(res, json);
    });

    // ------------------------------------------------------------------
    // POST /api/channel
    // Body: {"remoteControlKey":3}  or  {"space":0,"channel":5}
    // ------------------------------------------------------------------
    server.Post("/api/channel", [ctx](const httplib::Request &req, httplib::Response &res) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            SendJson(res, R"({"error":"JSON の解析に失敗しました"})", 400);
            return;
        }
        auto wreq = std::make_shared<WriteRequest>();

        if (auto it = body.find("remoteControlKey"); it != body.end() && it->is_number_integer()) {
            wreq->type             = WriteRequest::Type::SET_CHANNEL_RCK;
            wreq->remoteControlKey = it->get<int>();
        } else {
            auto sp = body.find("space"), ch = body.find("channel");
            if (sp == body.end() || !sp->is_number_integer() ||
                ch == body.end() || !ch->is_number_integer()) {
                SendJson(res, R"({"error":"remoteControlKey または space+channel が必要です"})", 400);
                return;
            }
            wreq->type    = WriteRequest::Type::SET_CHANNEL_SPACE;
            wreq->space   = sp->get<int>();
            wreq->channel = ch->get<int>();
        }
        ctx.dispatch(wreq);
        SendJson(res, wreq->responseJson, wreq->success ? 200 : 500);
    });

    // ------------------------------------------------------------------
    // POST /api/volume
    // Body: {"volume":50}  or  {"mute":true}  or both
    // ------------------------------------------------------------------
    server.Post("/api/volume", [ctx](const httplib::Request &req, httplib::Response &res) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            SendJson(res, R"({"error":"JSON の解析に失敗しました"})", 400);
            return;
        }

        if (auto it = body.find("volume"); it != body.end() && it->is_number_integer()) {
            int vol = it->get<int>();
            if (vol < 0 || vol > 100) {
                SendJson(res, R"({"error":"volume は 0〜100 の範囲です"})", 400);
                return;
            }
            auto wreq    = std::make_shared<WriteRequest>();
            wreq->type   = WriteRequest::Type::SET_VOLUME;
            wreq->volume = vol;
            ctx.dispatch(wreq);
            if (!wreq->success) { SendJson(res, wreq->responseJson, 500); return; }
        }

        if (auto it = body.find("mute"); it != body.end() && it->is_boolean()) {
            auto wreq  = std::make_shared<WriteRequest>();
            wreq->type = WriteRequest::Type::SET_MUTE;
            wreq->mute = it->get<bool>();
            ctx.dispatch(wreq);
            if (!wreq->success) { SendJson(res, wreq->responseJson, 500); return; }
        }

        SendJson(res, R"({"success":true})");
    });

    // ------------------------------------------------------------------
    // GET /api/program/channel
    // Query: ?space=0&channel=5  or  ?networkId=X&serviceId=Y[&transportStreamId=Z]
    //        &eventId=N (optional)
    // ------------------------------------------------------------------
    server.Get("/api/program/channel", [ctx](const httplib::Request &req, httplib::Response &res) {
        EpgQuery q = {};
        auto s = ctx.snapState();

        if (req.has_param("space") && req.has_param("channel")) {
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
                SendJson(res, R"({"error":"指定された space+channel が見つかりません"})", 404);
                return;
            }
        } else if (req.has_param("networkId") && req.has_param("serviceId")) {
            q.networkId = static_cast<WORD>(std::stoi(req.get_param_value("networkId")));
            q.serviceId = static_cast<WORD>(std::stoi(req.get_param_value("serviceId")));
            if (req.has_param("transportStreamId")) {
                q.tsId = static_cast<WORD>(std::stoi(req.get_param_value("transportStreamId")));
            } else {
                for (const auto &e : s.channelList) {
                    if (e.networkID == q.networkId && e.serviceID == q.serviceId) {
                        q.tsId = static_cast<WORD>(e.tsID);
                        break;
                    }
                }
            }
        } else {
            SendJson(res, R"({"error":"space+channel または networkId+serviceId が必要です"})", 400);
            return;
        }

        if (req.has_param("eventId")) {
            q.eventId    = static_cast<WORD>(std::stoi(req.get_param_value("eventId")));
            q.hasEventId = true;
        }

        auto wreq        = std::make_shared<WriteRequest>();
        wreq->type       = WriteRequest::Type::GET_EPG_EVENT;
        wreq->epgQueries = {q};
        ctx.dispatch(wreq);
        SendJson(res, wreq->epgResultJson, wreq->success ? 200 : 500);
    });

    // ------------------------------------------------------------------
    // POST /api/program/channels
    // Body: [{"space":0,"channel":5}, {"networkId":X,"serviceId":Y}, ...]
    // ------------------------------------------------------------------
    server.Post("/api/program/channels", [ctx](const httplib::Request &req, httplib::Response &res) {
        auto s       = ctx.snapState();
        auto queries = ParseEpgQueryArray(req.body, s.channelList);
        if (queries.empty()) {
            SendJson(res, R"({"error":"有効なチャンネル指定が1件もありません"})", 400);
            return;
        }
        auto wreq        = std::make_shared<WriteRequest>();
        wreq->type       = WriteRequest::Type::GET_EPG_EVENT;
        wreq->epgQueries = std::move(queries);
        ctx.dispatch(wreq);
        SendJson(res, wreq->epgResultJson, wreq->success ? 200 : 500);
    });

    // ------------------------------------------------------------------
    // POST /api/record/start
    // ------------------------------------------------------------------
    server.Post("/api/record/start", [ctx](const httplib::Request &, httplib::Response &res) {
        auto wreq  = std::make_shared<WriteRequest>();
        wreq->type = WriteRequest::Type::START_RECORD;
        ctx.dispatch(wreq);
        SendJson(res, wreq->responseJson, wreq->success ? 200 : 500);
    });

    // ------------------------------------------------------------------
    // POST /api/record/stop
    // ------------------------------------------------------------------
    server.Post("/api/record/stop", [ctx](const httplib::Request &, httplib::Response &res) {
        auto wreq  = std::make_shared<WriteRequest>();
        wreq->type = WriteRequest::Type::STOP_RECORD;
        ctx.dispatch(wreq);
        SendJson(res, wreq->responseJson, wreq->success ? 200 : 500);
    });

    // ------------------------------------------------------------------
    // POST /api/driver
    // Body: {"driver":"BonDriver_Proxy_S.dll"}
    //    or {"driver":"...","remoteControlKey":4}
    //    or {"driver":"...","space":0,"channel":0}
    // ------------------------------------------------------------------
    server.Post("/api/driver", [ctx](const httplib::Request &req, httplib::Response &res) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            SendJson(res, R"({"error":"JSON の解析に失敗しました"})", 400);
            return;
        }
        auto nameIt = body.find("driver");
        if (nameIt == body.end() || !nameIt->is_string() || nameIt->get<std::string>().empty()) {
            SendJson(res, R"({"error":"driver フィールドが必要です"})", 400);
            return;
        }
        auto wreq        = std::make_shared<WriteRequest>();
        wreq->type       = WriteRequest::Type::SET_DRIVER;
        wreq->driverName = StrToWStr(nameIt->get<std::string>());

        auto rckIt = body.find("remoteControlKey");
        auto spIt  = body.find("space");
        auto chIt  = body.find("channel");
        if (rckIt != body.end() && rckIt->is_number_integer()) {
            wreq->hasChannel       = true;
            wreq->remoteControlKey = rckIt->get<int>();
        } else if (spIt != body.end() && spIt->is_number_integer() &&
                   chIt != body.end() && chIt->is_number_integer()) {
            wreq->hasChannel = true;
            wreq->space      = spIt->get<int>();
            wreq->channel    = chIt->get<int>();
        }

        ctx.dispatch(wreq);
        SendJson(res, wreq->responseJson, wreq->success ? 200 : 500);
    });

    // ------------------------------------------------------------------
    // POST /api/ttrec/reserve/default
    // Body: {"onid":X,"tsid":Y,"sid":Z,"eid":E,"startTime":"...","duration":N}
    // ------------------------------------------------------------------
    server.Post("/api/ttrec/reserve/default", [ctx](const httplib::Request &req, httplib::Response &res) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            SendJson(res, R"({"error":"JSON の解析に失敗しました"})", 400);
            return;
        }

        auto state = ctx.snapState();
        EpgQuery q = {};
        if (!ResolveEpgQuery(body, state.channelList, q)) {
            SendJson(res, R"({"error":"onid+sid(+tsid) または networkId+serviceId(+transportStreamId) または space+channel が必要です"})", 400);
            return;
        }

        auto getInt = [&](const char *k1, const char *k2 = nullptr) -> int {
            auto it = body.find(k1);
            if (it != body.end() && it->is_number_integer()) return it->get<int>();
            if (k2) { it = body.find(k2); if (it != body.end() && it->is_number_integer()) return it->get<int>(); }
            return -1;
        };
        int eventId  = getInt("eid", "eventId");
        int duration = getInt("duration");
        std::string startTimeText;
        if (auto it = body.find("startTime"); it != body.end() && it->is_string())
            startTimeText = it->get<std::string>();

        if (eventId < 0 || duration < 0 || startTimeText.empty()) {
            SendJson(res, R"({"error":"eid(eventId)・startTime・duration が必要です"})", 400);
            return;
        }
        if (eventId > 0xFFFF) {
            SendJson(res, R"({"error":"eventId または duration の値が不正です"})", 400);
            return;
        }

        SYSTEMTIME startTime = {};
        if (!ParseIso8601Local(startTimeText, startTime)) {
            SendJson(res, R"({"error":"startTime は YYYY-MM-DDTHH:MM:SS 形式で指定してください"})", 400);
            return;
        }

        auto wreq        = std::make_shared<WriteRequest>();
        wreq->type       = WriteRequest::Type::TTREC_RESERVE_DEFAULT;
        wreq->epgQuery   = q;
        wreq->eventId    = static_cast<WORD>(eventId);
        wreq->startTime  = startTime;
        wreq->duration   = static_cast<DWORD>(duration);
        ctx.dispatch(wreq);
        SendJson(res, wreq->responseJson, wreq->success ? 200 : 500);
    });
}

} // namespace TVTestHTTP
