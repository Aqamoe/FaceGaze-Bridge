# ADR-0039 · 下载与更新走用户的系统代理

- 状态：**已实施**（2026-11-01）
- 日期：2026-11-01
- 影响：`src/net/proxy_config.*`（新）、`src/net/http_client.cpp`、`tests/test_proxy.cpp`（新）、
  `docs/10-security-review.md`
- 关系：受 [ADR-0036](0036-auto-update.md)（自动更新）与
  [ADR-0035](0035-native-module-registry.md)（原生模块库）约束——两条链路都靠 WinHTTP 下载

## 问题

`WinHttpOpen` 用的 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` 只读 WinHTTP 自己那份配置
（`netsh winhttp show proxy`，即 `HKLM\…\Internet Settings\Connections\WinHttpSettings`），
与 Clash / v2rayN 开关写的 IE/WinINET 代理（`HKCU\…\Internet Settings\ProxyEnable` / `ProxyServer`）
**互不相通**。

实测一台开发机：Clash 的系统代理开着（`ProxyServer = 127.0.0.1:7890`），
`netsh winhttp show proxy` 仍是 `Direct access (no proxy server)`。于是：

- 开代理：程序看不到，依旧直连 GitHub；
- 关代理：直连本来就不稳。

症状是"模块装不上、更新包下不来"，日志里 `WinHTTP 错误 12152`（响应非法，连接被重置）
与 `12002`（超时）交替出现；同一台机器上浏览器、`curl -x 127.0.0.1:7890` 都能正常下载。

## 决定

1. **公网请求前读 IE 代理**：`ProxyEnable` 为真时把 `ProxyServer` 翻译成
   `http=host:port;https=host:port`，以 `WINHTTP_ACCESS_TYPE_NAMED_PROXY` 建 session。
2. **`AutoConfigURL` 用 `WinHttpGetProxyForUrl` 按目标 URL 现算**——PAC 是一段脚本，
   没法预先翻译成 host:port；解析失败就保留原设置（不能因为脚本拉不到把本来通的请求弄断）。
3. **只填了 `socks=` 视为没有代理**：WinHTTP 不支持 SOCKS，把 SOCKS 端口当 HTTP 代理只会更糟；
   只填 `http=` 时补一份 `https=`，否则所有 HTTPS 请求会直连。
4. **`ProxyEnable = 0` 时不使用残留的 `ProxyServer`**：关掉系统代理后注册表里还留着上次的值。
5. **`127.0.0.1` 的本机请求（OSCQuery）继续 `NO_PROXY`**；没有系统代理时退回原来的
   `DEFAULT_PROXY`，尊重 `netsh winhttp` 这类手工配置。
6. **不缓存**：用户会在程序运行中开关代理，缓存住就会一直拿旧值。

## 权衡

- 不引 libcurl：发布包不想多一个 DLL，WinHTTP 已够用。
- 不加界面上的代理设置项：系统代理就是用户已经在维护的那份配置，再叠一层只会不一致。
- 系统代理指向的软件没在跑时，请求会更快失败（12029）——这是用户配置造成的，
  比"以为走代理其实直连"更容易排查。
- 网络本身不稳时仍会失败，本次改动只保证"代理开着就一定走代理"。

## 验证

- 单测 `tests/test_proxy.cpp`：单 host、只填 `http=`、大小写、空值、`socks=` 丢弃、带 scheme 前缀。
- 端到端（同一台机器，系统代理 127.0.0.1:7890）：探针进程调 `net::http_get` 取
  `https://api.ipify.org`，出口 IP = 代理节点 IP；`net::http_download` 下载 GitHub release
  资产成功（99131 字节）。改动之前同样的 `http_download` 报 12152 / 12002。
