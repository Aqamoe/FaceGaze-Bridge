# ADR-0010 · 界面回到 WebView2 外壳 + WebSocket IPC

> 后续：发版形态在 [ADR-0016](0016-single-entry-desktop-app.md) 收敛为单入口
> `FaceGaze Bridge.exe`（窗口拉起引擎、关窗即停）。本文关于 IPC 与页面同源的所有决定不变。

- 状态：已采纳（取代 [ADR-0009](0009-local-control-panel.md) 的"浏览器 + HTTP 轮询"形态）
- 日期：2026-10-29
- 影响：新增 `net/ws_codec`、`net/ws_server`、`web/ipc`、`ui/`、`src/ui`（`fgb-ui`）；删除 `web/panel.*`

## 问题

ADR-0009 用"HTTP + 内嵌单页"当第一版控制面板，理由是**可验证性**：接口能用 curl 打、页面能被脚本检查，
不需要人肉点一个看不见的窗口。这个理由至今成立，但它把**形态**带偏了：

- 产品界面是"另开一个浏览器标签输 `http://127.0.0.1:8787/`"。双击图标 → 得到浏览器，
  点错标签就丢了面板，关掉浏览器就等于关掉设置入口。这不是桌面应用的形态。
- 页面 500 ms 轮询，参数热力图（365 个参数）在轮询下永远差半拍；滑杆改动要等下一轮才回显。
- 架构里（`01-architecture.md` §4）写的是 `fgb-ui (WebView2)` + `本地 IPC（WebSocket + JSON）`，
  实现与文档已经不一致。

## 决定

### 1. 界面是 WebView2 窗口，不是浏览器

`fgb-ui.exe` 是一个 Win32 窗口 + WebView2 控件。它只做三件事：建窗口、起 WebView2、
导航到控制面地址。没有协议逻辑、没有业务状态——外壳里能少一行就少一行。

界面与引擎**分进程**（架构 §4 的 Shell 层 / Service 层）：

```
fgb-cli run --ui            ← 引擎（追踪数据流在这里，崩不起）
   │  ws://127.0.0.1:8787/ws
fgb-ui --port 8787          ← 外壳（崩了重开即可，输出不受影响）
```

`--ui` 只是 `CreateProcess` 拉一次外壳；外壳自己带重连与导航重试，
所以"先起 UI 还是先起引擎"都无所谓。

### 2. 页面从引擎的 `http://127.0.0.1:<port>/` 加载

不用 `NavigateToString`，也不用虚拟主机映射 `https://fgb.local`，因为：
https 页面连 `ws://127.0.0.1` 会撞 Chromium 的 mixed content / Private Network Access，
在虚拟主机这种"看起来像公网域名"的源上尤其不可靠。
页面与 WebSocket**同源同端口**（http），这一类问题直接消失，顺便还能在浏览器里开同一个地址调试。

代价是引擎要发静态文件，于是 `web/ipc.cpp` 里有一个约 40 行的静态处理器：
只发已知扩展名（html/js/css/json/svg/png/woff2/ico）、拒绝 `..` 与反斜杠、用 `weakly_canonical`
做目录前缀校验。资源在 `<exe>/ui/`，构建时由 CMake 复制过去，改 UI 不用重编 C++。

### 3. IPC 走 WebSocket，推送而不是轮询

| 方向 | 消息 | 频率 |
|---|---|---|
| 引擎 → UI | `hello`（协议版本、端口） | 连接建立 |
| 引擎 → UI | `catalog`（365 个参数名 + 是否已绑定） | 连接建立 / 换头像后 |
| 引擎 → UI | `status`（帧率、tick、迟到、OSC 统计、头像、滤波状态） | 10 Hz |
| 引擎 → UI | `params`（365 个数值，与 catalog 顺序对应） | 10 Hz |
| 引擎 → UI | `log`（级别、tag、正文） | 有日志时 |
| UI → 引擎 | `set_filter` / `reload_avatar` / `set_log_level` / `quit` | 按操作 |
| 引擎 → UI | `ack`（命令、ok、error） | 每条命令 |

**参数名只发一次**：`catalog` 之后每帧只发数值数组（365 个数字 ≈ 2.4 KB，10 Hz ≈ 24 KB/s）。
轮询做不到这一点——每次都只能把名字带上，或者让页面自己猜顺序。

`log` 在连接建立时会补发最近 120 行。否则 UI 一打开日志页是空的，
而"为什么没数据"恰恰写在连接之前的那几行里。

### 4. 仍然保留只读 HTTP 接口

`GET /api/status`、`GET /api/params`、`GET /healthz` 还在，它们不是给界面用的，是给
`curl` 和 `tools/probe_ipc.py` 用的。ADR-0009 那条"接口要能脚本验证"的结论继续有效，
只是它不该成为界面形态的约束。

### 5. 自己写 WebSocket 而不是引库

需要的东西只有 RFC 6455 的一小块：握手（SHA-1 + Base64）、掩码帧解析、文本/ping/close、
服务端不分片发送。约 400 行，`tests/test_ws.cpp` 有 RFC 原文向量（含 §1.3 的期望 accept key）
和构造帧的往返测试。引库反而会拖进 asio/TLS，并且事件循环塞不进引擎的 120 Hz 主循环。

发送仍然是"主循环里非阻塞 select"：每 4 个 tick 调一次 `pump(0)`，
引擎与面板共享同一份状态，零加锁（与 ADR-0009 的取舍相同）。

## 验证

自动化的部分（都是同一台机器上真跑，不是推理）：

- `fgb-tests`：**85 个用例通过**，其中 11 个是本层新增（SHA-1 已知向量、Base64、
  accept key、掩码帧、扩展长度、超大帧拒绝、不完整帧、HTTP 升级头解析）
- `python tools/probe_ipc.py 8791`（回放 25 s PICO 真实抓包）：
  HTTP 200 + WS 101；`catalog` 365 名 / 42 已绑定；3 秒内 `status` 39 条、`params` 37 条；
  `set_filter` 回执 `ok=true`，随后 `status.filter.enabled` 变 true
- `python tools/probe_ui.py 9222 8791`（WebView2 + 远程调试端口）：
  页面 `connected=true`、`catalog=values=cells=365`、参数页 42 个可见格子 / 230 个非零值、
  日志页有内容 —— 证明窗口里显示的是真实数据，而不只是"有个窗口"
- 人工核对截图：1180×780 原生窗口，侧栏 + 状态卡（119.4 fps / 1111 帧 / 673 条 OSC）、
  参数热力图（颜色深浅随值变化、悬停显示完整参数名）

失败路径也验了：引擎没起来时外壳不白屏，标题变"等待引擎…"并每 1.5 s 重试导航。

## 遗留

- **没有认证**。只绑 `127.0.0.1`，但同机其他程序可以连。与 ADR-0009 相同的接受。
- 页面仍是原生 JS，没有框架也没有构建步骤（`04-engineering.md` 规划的 TS+Vite+Svelte 还没落地）。
  等页面超过千行或者需要类型约束时再上，现在引构建链只会让"改个滑杆"变成"先 npm i"。
- 还不能在界面里改输出目标 / 参数集 / 校准档案 —— 消息格式已经留了位置，缺的是后端接口。
- 没有托盘常驻。关窗口 = 关界面，引擎继续跑（这是有意的）。

## 下一步

托盘壳（Win32 消息循环 + 图标菜单）作为第二层包装，把"启动/停止/打开界面"收进右键菜单；
再把 `set_params_mode`、`set_eye_mode`、`load_calibration` 三个命令补进 IPC。
