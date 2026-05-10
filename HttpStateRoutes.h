#pragma once

#include "ApiJson.h"
#include "Types.h"
#include "httplib.h"

#include <functional>

namespace TVTestHTTP {

static void SetCorsHeaders(httplib::Response &res)
{
    res.set_header("Access-Control-Allow-Origin",  "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

static void SendJson(httplib::Response &res, const std::string &body, int status = 200)
{
    SetCorsHeaders(res);
    res.status = status;
    res.set_content(body, "application/json; charset=utf-8");
}

static void RegisterStateRoutes(httplib::Server &server,
                                std::function<TVTestState()> snapState,
                                int recordingStatus)
{
    server.Get("/api/status", [snapState, recordingStatus](const httplib::Request &, httplib::Response &res) {
        auto s = snapState();
        SendJson(res, BuildStatusJson(s, recordingStatus));
    });

    server.Get("/api/channels", [snapState](const httplib::Request &, httplib::Response &res) {
        auto s = snapState();
        SendJson(res, BuildChannelsJson(s.channelList));
    });

    server.Get("/api/volume", [snapState](const httplib::Request &, httplib::Response &res) {
        auto s = snapState();
        SendJson(res, BuildVolumeJson(s));
    });

    server.Get("/api/program", [snapState](const httplib::Request &, httplib::Response &res) {
        auto s = snapState();
        SendJson(res, BuildProgramJson(s));
    });

    server.Get("/api/record/status", [snapState, recordingStatus](const httplib::Request &, httplib::Response &res) {
        auto s = snapState();
        SendJson(res, BuildRecordStatusJson(s, recordingStatus));
    });
}

} // namespace TVTestHTTP
