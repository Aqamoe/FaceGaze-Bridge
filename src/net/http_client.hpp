// 极小的 HTTP(S) GET 客户端（WinHTTP）。
//
// 只服务模块库：拉 registry.vrcft.io/modules 的 JSON、下载模块包。不做 cookie、
// 不做认证、不做分块上传；重定向交给 WinHTTP 自己处理（GitHub release 会 302 到
// objects.githubusercontent.com）。
//
// 公网请求走用户的系统代理（`net/proxy_config.hpp`，ADR-0039）：WinHTTP 的默认代理
// 与 Clash 等开的 IE 代理互不相通，不显式读过来就会"开着代理也下载失败"。
//
// 不引 libcurl：发布包不想多一个 DLL，也不需要它那套多协议/异步能力。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace fgb::net {

struct HttpProgress {
    std::uint64_t received = 0;
    std::uint64_t total    = 0; // 0 表示服务器没给 Content-Length
};

// 默认上限：模块列表 8 MB，模块包 256 MB
constexpr std::size_t kDefaultTextLimit = 8u * 1024u * 1024u;
constexpr std::size_t kDefaultFileLimit = 256u * 1024u * 1024u;

// 成功时 body 为完整响应；失败时 error 是人类可读原因（含 HTTP 状态码）
//
// timeout_ms > 0 用于本机服务（OSCQuery）：超时收紧且绕过代理，避免本机端口
// 没开时卡在默认的 15 秒接收超时上。
bool http_get(const std::string& url, std::string& body, std::string& error,
              std::size_t max_bytes = kDefaultTextLimit, int timeout_ms = 0);

// 下载到文件（先写 .part，成功后再改名，避免半截文件被当成模块包）
//
// cancel 非空时每收一块就查一次：置位后立刻返回 false 并删掉 .part。
// 自动更新用它——用户在下载 100 MB 安装包的中途退出程序，不能让他等下载跑完。
bool http_download(const std::string& url, const std::filesystem::path& destination,
                   std::string& error,
                   const std::function<void(const HttpProgress&)>& progress = {},
                   std::size_t max_bytes = kDefaultFileLimit,
                   const std::atomic<bool>* cancel = nullptr);

} // namespace fgb::net
