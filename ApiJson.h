#pragma once

#include "Constants.h"
#include "JsonHelpers.h"
#include "Types.h"

#include <sstream>
#include <string>
#include <vector>

namespace TVTestHTTP {

static std::string BuildStatusJson(const TVTestState &s, int recordingStatus)
{
    std::ostringstream j;
    j << "{";

    if (s.hasChannel) {
        j << "\"channel\":{"
          << "\"space\":"             << s.space             << ","
          << "\"channel\":"           << s.channel           << ","
          << "\"remoteControlKey\":"  << s.remoteControlKey  << ","
          << "\"serviceId\":"         << s.serviceID         << ","
          << "\"networkId\":"         << s.networkID         << ","
          << "\"transportStreamId\":" << s.tsID              << ","
          << "\"name\":\""            << JsonStr(s.channelName) << "\","
          << "\"networkName\":\""     << JsonStr(s.networkName) << "\""
          << "},";
    } else {
        j << "\"channel\":null,";
    }

    j << "\"volume\":"       << s.volume                         << ","
      << "\"mute\":"         << (s.mute ? "true" : "false")      << ","
      << "\"recordStatus\":" << s.recordStatus                   << ","
      << "\"recording\":"    << (s.recordStatus == recordingStatus ? "true" : "false");

    if (s.hasProgramInfo) {
        j << ",\"program\":{"
          << "\"name\":\"" << JsonStr(s.programName) << "\","
          << "\"text\":\"" << JsonStr(s.programText) << "\""
          << "}";
    } else {
        j << ",\"program\":null";
    }

    j << "}";
    return j.str();
}

static std::string BuildChannelsJson(const std::vector<ChannelEntry> &channels)
{
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (const auto &e : channels) {
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
    return j.str();
}

static std::string BuildVolumeJson(const TVTestState &s)
{
    std::ostringstream j;
    j << "{\"volume\":" << s.volume
      << ",\"mute\":"   << (s.mute ? "true" : "false") << "}";
    return j.str();
}

static std::string BuildProgramJson(const TVTestState &s)
{
    if (!s.hasProgramInfo) return R"({"program":null})";

    std::ostringstream j;
    j << "{\"program\":{"
      << "\"name\":\"" << JsonStr(s.programName) << "\","
      << "\"text\":\"" << JsonStr(s.programText) << "\""
      << "}}";
    return j.str();
}

static std::string BuildRecordStatusJson(const TVTestState &s, int recordingStatus)
{
    std::ostringstream j;
    j << "{\"status\":"    << s.recordStatus
      << ",\"recording\":" << (s.recordStatus == recordingStatus ? "true" : "false")
      << "}";
    return j.str();
}

static std::string BuildDriverJson(const std::wstring &currentDriver,
                                   const std::vector<std::wstring> &drivers)
{
    std::ostringstream j;
    j << "{\"current\":\"" << JsonStr(currentDriver) << "\",\"drivers\":[";
    bool first = true;
    for (const auto &driver : drivers) {
        if (!first) j << ",";
        first = false;
        j << "\"" << JsonStr(driver) << "\"";
    }
    j << "]}";
    return j.str();
}

} // namespace TVTestHTTP
