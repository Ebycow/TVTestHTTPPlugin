#pragma once

#include "Constants.h"
#include "JsonHelpers.h"
#include "Types.h"

namespace TVTestHTTP {

static std::string BuildStatusJson(const TVTestState &s, int recordingStatus)
{
    using json = nlohmann::json;
    json j;

    if (s.hasChannel) {
        j["channel"] = {
            {"space",             s.space},
            {"channel",           s.channel},
            {"remoteControlKey",  s.remoteControlKey},
            {"serviceId",         s.serviceID},
            {"networkId",         s.networkID},
            {"transportStreamId", s.tsID},
            {"name",              WStrToUtf8(s.channelName)},
            {"networkName",       WStrToUtf8(s.networkName)}
        };
    } else {
        j["channel"] = nullptr;
    }

    j["volume"]       = s.volume;
    j["mute"]         = s.mute;
    j["recordStatus"] = s.recordStatus;
    j["recording"]    = (s.recordStatus == recordingStatus);

    if (s.hasProgramInfo) {
        j["program"] = {
            {"name", WStrToUtf8(s.programName)},
            {"text", WStrToUtf8(s.programText)}
        };
    } else {
        j["program"] = nullptr;
    }

    return j.dump();
}

static std::string BuildChannelsJson(const std::vector<ChannelEntry> &channels)
{
    using json = nlohmann::json;
    json arr = json::array();
    for (const auto &e : channels) {
        arr.push_back({
            {"space",            e.space},
            {"channel",          e.channel},
            {"remoteControlKey", e.remoteControlKey},
            {"serviceId",        e.serviceID},
            {"networkId",        e.networkID},
            {"name",             WStrToUtf8(e.name)},
            {"networkName",      WStrToUtf8(e.networkName)}
        });
    }
    return arr.dump();
}

static std::string BuildVolumeJson(const TVTestState &s)
{
    return nlohmann::json{{"volume", s.volume}, {"mute", s.mute}}.dump();
}

static std::string BuildProgramJson(const TVTestState &s)
{
    if (!s.hasProgramInfo)
        return nlohmann::json{{"program", nullptr}}.dump();
    return nlohmann::json{{"program", {
        {"name", WStrToUtf8(s.programName)},
        {"text", WStrToUtf8(s.programText)}
    }}}.dump();
}

static std::string BuildRecordStatusJson(const TVTestState &s, int recordingStatus)
{
    return nlohmann::json{
        {"status",    s.recordStatus},
        {"recording", s.recordStatus == recordingStatus}
    }.dump();
}

static std::string BuildDriverJson(const std::wstring &currentDriver,
                                   const std::vector<std::wstring> &drivers)
{
    using json = nlohmann::json;
    json arr = json::array();
    for (const auto &d : drivers) arr.push_back(WStrToUtf8(d));
    return json{{"current", WStrToUtf8(currentDriver)}, {"drivers", arr}}.dump();
}

} // namespace TVTestHTTP
