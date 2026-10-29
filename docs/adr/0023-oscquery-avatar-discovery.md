# ADR-0023 · OSCQuery 头像发现与换头像热重绑

- 状态：**已实施**（2026-10-30）
- 日期：2026-10-30
- 影响：`src/net/mdns.*`（新增）、`src/params/oscquery.*`（新增）、
  `src/net/http_client.*`（可选超时/绕代理）、`src/sinks/vrchat_sink.*`（发现优先级、
  后台 watcher、线程安全快照）、`src/web/ipc.cpp`（状态字段）、`src/cli/main.cpp`
  （`--no-oscquery` / `--oscquery-url` / `--avatar-poll-ms`）、`ui/`（参数来源一栏）、
  `tools/probe_oscquery.py`

## 问题

此前头像参数表只有一条路：从 VRChat 日志读当前头像 ID，再读
`%LocalLow%\VRChat\VRChat\OSC\<用户>\Avatars\<id>.json`。它有两个硬限制：

1. **换头像不会自动生效**。日志里的 ID 变了，引擎不知道；用户必须点界面的
   「重新读取」，否则参数会继续发到上一个头像的地址。
2. **Build & Test 头像没有本地 JSON**。VRChat 只为已发布头像写配置文件；
   本地测试模型只能靠"猜"或固定前缀。

VRChat 从 2023.3.1 起提供 OSCQuery：HTTP 上直接给出当前头像的
`/avatar/change`、`/avatar/name` 和整棵 `/avatar/parameters` 树，换头像时树会变。
问题在于 **HTTP 端口是动态的**——它通过 mDNS 广播 `_oscjson._tcp`，本机实测是
`127.0.0.1:65393`，固定探 9001 会超时（9001 是 VRChat 的 UDP 发送端口，不是 HTTP）。

## 决定

### 1. 地址发现优先级

```
显式 --avatar-config
  > OSCQuery（mDNS + HTTP）
  > VRChat 日志里的当前头像 ID + 本地 JSON
  > 最近配置里最像面捕头像的一份（启发式）
  > 固定前缀
```

OSCQuery 拿到的参数表与本地 JSON 等价时优先用 OSCQuery：它是"当下的事实"，
不依赖日志和文件写入时机。失败时逐级回退，任何一级失败都不影响后面。

### 2. mDNS 自己查，并留 150 ms 收集窗口

- 向 `224.0.0.251:5353` 发 PTR 查询 `_oscjson._tcp.local`，解析 PTR/SRV/A/TXT
  （含 DNS 名字压缩）。
- 同机可能同时有 OVRToolkit 等 `_oscjson` 服务。**第一个响应不一定是 VRChat**，
  所以收到第一个完整服务后再多收 150 ms，避免拿着"只有 OVRToolkit"的结果判失败。
- 选实例名以 `VRChat` 开头的服务；端口和 IPv4 从 SRV/A 记录取。
- 5353 被 Bonjour 等占用时退回临时端口；RFC 6762 允许 legacy 单播回复。

不用 Windows `DnsServiceBrowse`：它是异步回调 API，跨线程收结果、超时取消要自己
包一层，而且没法用一段固定报文做离线单测。这里报文解析是纯函数，能覆盖压缩指针、
截断报文和跨报文拼装。

### 3. HTTP 拉树，短超时且绕代理

- `http_get` 增加可选 `timeout_ms`；本机查询用 500 ms 并强制
  `WINHTTP_ACCESS_TYPE_NO_PROXY`，本机端口没开时快速失败，不走用户代理。
- 启动时拉 `/` 整棵树；轮询只拉 `/avatar/change`（几百字节）。
- 只收 `/avatar/parameters/` 下、`ACCESS & 2`（可写）且 `TYPE` 为 `f` 的 float、
  `T`/`F` 的 bool；`i`（int）、`s`（string）、`fff`（vector）不发送。
- 参数为空也是**有效结果**：这个头像没有可写的 float/bool 时绑成空表，
  比回退到固定前缀更诚实，避免把值发到不存在的地址。

### 4. 后台 watcher 负责 I/O，引擎线程负责换表

- sink 起一个后台线程，每 `--avatar-poll-ms`（默认 2000 ms）拉一次
  `/avatar/change`；ID 变了才拉整棵树。
- OSCQuery 不可用时，watcher 改读日志里的头像 ID；VRChat 重启导致端口变化时，
  HTTP 失败会清掉端点并在 2 s 后重新 mDNS；完全找不到时按 10 s 退避。
- 新参数表不直接改 `router_`，而是放进 `pending_` 并递增序号；
  引擎线程在 `submit()` 开头应用。这样 120 Hz 热路径与后台 I/O 解耦。
- 控制面（参数页/状态页）通过 `shared_mutex` 读快照，不再直接拿 `router()` 引用。
- 界面的「重新读取」只置标志并唤醒 watcher，不再从 IPC 线程直接重绑。

### 5. 显式开关与诊断

- `--no-oscquery`：只读本地 JSON（排障/对比用）。
- `--oscquery-url 127.0.0.1:65393`：跳过 mDNS，直接指定端点（探针/假服务器用）。
- `--avatar-poll-ms 0`：关闭后台轮询（手动重读也随之失效）。
- 状态页「参数来源」显示 `OSCQuery（mDNS）/ 指定配置文件 / 本地头像配置 /
  本地配置（启发式）/ 固定前缀`，三语齐全。
- `tools/probe_oscquery.py`：不启动 FGB，直接打印 mDNS 服务、HOST_INFO、头像名/ID
  和可写参数计数。

## 实测证据（2026-10-30，VRChat 正在运行）

```
mDNS：VRChat-Client-XXXXXX._oscjson._tcp.local → 127.0.0.1:65393
HOST_INFO：OSC_IP=127.0.0.1 OSC_PORT=9000 OSC_TRANSPORT=UDP
头像：Avatar-B  avtr_a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d
fgb-cli：OSCQuery 已绑定头像「Avatar-B」：45 个参数 / 156 条 OSC 目标
本地 JSON 对照：45 个参数 / 156 条目标（30 个二进制编码），完全一致
界面状态页：参数来源 = OSCQuery（mDNS）
```

换头像热重绑用一个假 OSCQuery 服务验证：端点先返回 `avtr_A`，2 秒后返回 `avtr_B`，
`fgb-cli --oscquery-url ... --avatar-poll-ms 300` 的日志出现
`头像已切换：avtr_A → avtr_B（OSCQuery）` 与 `头像参数已更新（OSCQuery）`，
tick 无丢帧。真实换头像未在用户当前会话里强制切换，留待用户自然切换时观察。

单测新增 9 个用例（OSCQuery 树/转义/空表/change，mDNS 查询报文/压缩指针/截断），
当前 133 个用例全过。

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 固定探 `127.0.0.1:9001` | 9001 是 UDP 发送端口；HTTP 端口动态，实测 65393 |
| `DnsServiceBrowse` | 异步回调 + 取消语义难包，且无法离线单测 |
| OSCQuery WebSocket 订阅 | 协议面更大；2 s 轮询 `/avatar/change` 只有几百字节，足够 |
| watcher 直接改 `router_` | 与 120 Hz `submit` 竞争；必须走 pending + 引擎线程落地 |
| 把 mDNS 放在启动关键路径上同步阻塞很久 | 500 ms 上限，且失败立刻回退本地 JSON |

## 已知限制

- 换头像识别最慢有一个轮询周期（默认 2 s）；不是事件级实时。
- OSCQuery 关闭时仍要靠日志 + 本地 JSON；Build & Test 头像在这种情况下仍可能
  没有参数表。
- mDNS 5353 与其他 Zeroconf 服务共享；退回临时端口时依赖对方支持 legacy 单播。
- CI 只覆盖报文/树解析，真实网络路径需要一台运行 VRChat 的机器
  （`python tools/probe_oscquery.py`）。
