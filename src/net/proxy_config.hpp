// 读取用户的"系统代理"（IE / WinINET 设置）。
//
// WinHTTP 的默认代理只认 netsh winhttp 那一份配置，与 Clash / v2rayN 开关写的
// IE 代理互不相通：用户以为"开着代理"，程序却还在直连。下载与更新检查都走
// WinHTTP，所以这一层得自己把系统代理读过来（见 ADR-0039）。
#pragma once

#include <string>

namespace fgb::net {

struct SystemProxy {
    bool enabled = false; // ProxyEnable；关掉时代理值还留在注册表里，不能照用
    std::string proxy;    // 已翻译成 WinHTTP 写法 "http=host:port;https=host:port"
    std::string bypass;   // IE 的例外列表，语法与 WinHTTP 一致，直接透传
    std::string pac_url;  // AutoConfigURL；非空时只能按目标 URL 现算
};

// 每次调用都重新读注册表：用户会在程序运行中开关代理，缓存住就会一直拿旧值。
SystemProxy system_proxy();

// IE 的 ProxyServer 值 → WinHTTP 认的代理串。WinHTTP 不支持 SOCKS，只有 socks=
// 的条目会被丢掉；只配了 http= 时补一份 https=（不补的话 HTTPS 请求会直连）。
std::string proxy_for_winhttp(const std::string& raw);

} // namespace fgb::net
