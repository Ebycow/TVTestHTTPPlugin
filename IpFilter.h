#pragma once

// Requires winsock2.h (included via httplib.h) before this header.
// inet_pton and in_addr are provided by winsock2.h / ws2tcpip.h.

#include "Types.h"
#include "JsonHelpers.h"

// =============================================================================
// IP フィルター (CIDR マッチング)
// =============================================================================

// "x.x.x.x/prefix" または "x.x.x.x" を CidrBlock に変換。失敗時 false
static bool ParseCidr(const std::string &s, CidrBlock &block)
{
    // 前後の空白を除去
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    size_t last = s.find_last_not_of(" \t\r\n");
    std::string t = s.substr(first, last - first + 1);

    int prefix = 32;
    size_t slash = t.find('/');
    std::string ipStr = (slash != std::string::npos) ? t.substr(0, slash) : t;
    if (slash != std::string::npos) {
        try { prefix = std::stoi(t.substr(slash + 1)); }
        catch (...) { return false; }
    }
    if (prefix < 0 || prefix > 32) return false;

    in_addr ia = {};
    if (inet_pton(AF_INET, ipStr.c_str(), &ia) != 1) return false;

    block.addr = ntohl(ia.s_addr);
    block.mask = (prefix == 0) ? 0u : (~0u << (32 - prefix));
    return true;
}

// ipStr が blocks のいずれかにマッチするか
static bool IpMatchesList(const std::string &ipStr, const std::vector<CidrBlock> &blocks)
{
    if (blocks.empty()) return false;
    in_addr ia = {};
    if (inet_pton(AF_INET, ipStr.c_str(), &ia) != 1) return false;
    uint32_t ip = ntohl(ia.s_addr);
    for (const auto &b : blocks) {
        if ((ip & b.mask) == (b.addr & b.mask)) return true;
    }
    return false;
}

// カンマ・改行区切りの CIDR 文字列をパースして CidrBlock のリストを返す
static std::vector<CidrBlock> ParseCidrList(const std::wstring &wlist)
{
    std::vector<CidrBlock> result;
    std::string list = WStrToUtf8(wlist);
    std::string cur;
    for (char c : list) {
        if (c == ',' || c == '\n' || c == '\r') {
            if (!cur.empty()) {
                CidrBlock b;
                if (ParseCidr(cur, b)) result.push_back(b);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        CidrBlock b;
        if (ParseCidr(cur, b)) result.push_back(b);
    }
    return result;
}
