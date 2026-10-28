// WebSocket 协议里我们真正用到的那一小块。
//
// UI 与引擎之间的控制面走 `ws://127.0.0.1:<port>/ws`：浏览器原生支持、人类可读、
// 随手就能用脚本验；而真要做产品界面时，WebView2 里的页面和浏览器里的页面是同一份代码。
//
// 刻意不引第三方 WebSocket 库：这里的帧格式是 RFC 6455 的一个子集（文本 / ping / close，
// 服务端不掩码、不分片发送），一百多行能写对，而且能被单测逐字节验证。
// 不做的事：TLS、扩展协商、压缩、服务端分片。回环上的本机控制面不需要这些。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace fgb::net::ws {

constexpr std::uint8_t kOpContinuation = 0x0;
constexpr std::uint8_t kOpText         = 0x1;
constexpr std::uint8_t kOpBinary       = 0x2;
constexpr std::uint8_t kOpClose        = 0x8;
constexpr std::uint8_t kOpPing         = 0x9;
constexpr std::uint8_t kOpPong         = 0xA;

// 单帧上限。参数快照约 20 KB，留足余量同时挡住误用。
constexpr std::size_t kMaxPayload = 1u << 20;

// ---- 握手所需的两个小工具（顺便单测掉最易写错的部分）----
std::string sha1(std::string_view data);   // 返回 20 字节原始摘要
std::string base64(std::string_view data); // 标准字母表，带 '=' 补齐
// RFC 6455 §1.3：base64(sha1(client_key + GUID))
std::string accept_key(std::string_view client_key);

// ---- 帧 ----
struct Frame {
    bool fin            = true;
    std::uint8_t opcode = 0;
    bool masked         = false;
    std::string payload;
    std::size_t consumed = 0; // 本帧在输入缓冲里占用的字节数
};

enum class ParseResult {
    ok,         // 解析出一帧
    incomplete, // 还需要更多字节
    error,      // 协议错误，connection 应关闭
};

ParseResult parse_frame(std::string_view buffer, Frame& out, std::string& error);

// 追加一个服务端帧（不掩码）
void append_frame(std::string& out, std::uint8_t opcode, std::string_view payload);

// ---- HTTP 升级请求 ----
// 同一端口既接 WebSocket 升级，也接几个只读诊断接口（curl 用得着）。
enum class HttpParse {
    ok,
    incomplete,
    error,
};

struct HttpHead {
    std::string method;
    std::string path;  // 不含查询串
    std::string query; // '?' 之后
    std::string upgrade;      // 小写化后的 Upgrade 头
    std::string ws_key;       // Sec-WebSocket-Key
    std::string ws_version;   // Sec-WebSocket-Version
    std::size_t head_bytes = 0;
};

HttpParse parse_http_head(std::string_view raw, HttpHead& out);

std::string http_response(int status, std::string_view content_type, std::string_view body);

} // namespace fgb::net::ws
