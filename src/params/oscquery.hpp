// VRChat OSCQuery 发现。
//
// OSCQuery 比读本地头像 JSON 更直接：VRChat 在 HTTP 上给出当前头像的
// `/avatar/change`（头像 ID）、`/avatar/name` 和整个 `/avatar/parameters` 树，
// 换头像时树会跟着变——这是"实时感知换头像"最可靠的来源。
//
// 端口不是固定的：VRChat 通过 mDNS 广播 `_oscjson._tcp`（本机实测 65393），
// 所以先走 mDNS 找端点，找到后用 HTTP 拉树；失败再退回本地配置 JSON 那条老路。
#pragma once

#include "params/avatar_config.hpp"

#include <cstdint>
#include <string>

namespace fgb::params {

struct OscQueryEndpoint {
    std::string instance; // VRChat-Client-XXXXXX._oscjson._tcp.local
    std::string address;  // 127.0.0.1
    std::uint16_t port = 0;

    std::string base_url() const;
};

// 在本地链路找 VRChat 的 OSCQuery 端点。多个 _oscjson._tcp 服务时优先 VRChat。
bool discover_oscquery_endpoint(OscQueryEndpoint& out, std::string& error,
                                int timeout_ms = 800);

// 解析 OSCQuery 树。只收 /avatar/parameters 下、可写（ACCESS 含 2）的 float / bool 叶子。
// bool 在 OSCQuery 里的 TYPE 是 "T"；int("i")、string("s")、vector("fff") 都不参与。
bool parse_oscquery_tree(std::string_view json, AvatarConfig& out, std::string& error);

// 只解析 /avatar/change（轮询用，响应只有几百字节）。
bool parse_oscquery_avatar_change(std::string_view json, std::string& id);

// HTTP GET + 解析。base_url 形如 http://127.0.0.1:65393/（自动补尾斜杠）。
bool fetch_oscquery_avatar(const std::string& base_url, AvatarConfig& out, std::string& error,
                           int timeout_ms = 1500);
bool fetch_oscquery_avatar_change(const std::string& base_url, std::string& id,
                                  std::string& error, int timeout_ms = 800);

// 显式 URL（--oscquery-url）用：补协议头与尾斜杠，方便用户只写 127.0.0.1:65393。
std::string normalize_oscquery_base_url(std::string_view url);

} // namespace fgb::params
