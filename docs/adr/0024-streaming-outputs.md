# ADR-0024 · 流式输出：WebSocket JSON 与 UDP 二进制

- 状态：**已实施**（2026-10-30）；v1.1 在 UDP/WS 末尾追加注视字段（见 ADR-0030）
- 日期：2026-10-30
- 影响：`src/sinks/ws_json_sink.*`、`src/sinks/udp_binary_sink.*`、`src/sinks/throttle.hpp`
  （新增）、`sdk/fgb_udp_frame.h`（新增）、`src/cli/main.cpp`、`src/web/ipc.*`、`ui/`、
  `tools/probe_ws_out.py`、`tools/probe_udp_out.py`、`docs/07-streaming-outputs.md`
- 关系：补完 [docs/02-features.md](../02-features.md) §3 的多目标输出；共享内存见
  [ADR-0019](0019-shared-memory-output.md)

## 问题

多输出层已经有了 VRChat OSC、VMC 和共享内存，但缺两个消费者群体：

1. **网页/脚本/研究工具**：不想写共享内存读取端，只想连一个 WebSocket 拿 JSON。
2. **串流软件/自研客户端**：要固定偏移的低延迟帧，不想解析 JSON，也不想依赖 VMC。

同时发现现有 sink 的节流有一个共同误差：引擎 tick 是 120 Hz（8.333 ms），而 90/60/30 Hz
的间隔都不是 tick 的整数倍。用 `now - last >= interval` 会把 90 量化成 60、60 量化成 40、
30 量化成 24。这个误差不是新输出特有的，四个 sink 都有。

## 决定

### 1. WebSocket JSON：回环、多客户端、只发活跃形状

- 复用控制面的 `net::WsServer`，独立端口，只绑 `127.0.0.1`，路径 `/ws`。
- 默认 30 Hz；`--ws-out-rate` 可改，0 表示每 tick。多个客户端同时广播。
- 默认为"只发有效且 > 0.01 的形状"；`--ws-out-all` 才发全部 88 个。理由：网页热力图
  和脚本调试通常只关心在动的形状，减少 3/4 的文本体积。
- 没有客户端时不做 JSON 编码；单个客户端出站缓冲超过 `WsServer` 的 512 KB 上限就断开，
  重连即可，不保留历史。
- 格式见 [docs/07-streaming-outputs.md](../07-streaming-outputs.md) §1，与
  [docs/02-features.md](../02-features.md) §3.3 的字段一致。

### 2. UDP 二进制：一个数据报一帧，小端固定布局

- `sdk/fgb_udp_frame.h` 定义 `FgbUdpFrameV1`，`#pragma pack(1)`，固定 592 字节：
  88 个形状占 352 字节，加上有效性 88 字节，仍远低于 1200 字节的避免分片上限。
- 字段顺序与共享内存尽量一致（眼、注视、头、形状），消费者可以按偏移直接读。
- `attributes` 显式区分左/右眼有效、头姿、fixation、合并注视有效；无效的数值仍在
  （例如 `gaze_dir` 保持默认），但消费者应按属性位判断。
- 默认每个 tick 发（120 Hz）；`--udp-out-rate` 可限速。不做重传/分片；丢帧等下一帧。
- 目标由 `--udp-out host:port` 显式给出，默认关闭。

### 3. 节流统一改成截止时间调度

新增 `SinkThrottle`：维护 `next_send`，允许 1 ms 调度松弛；落后超过 4 个间隔时重新起算，
避免暂停/调试后补发积压帧。VRChat、VMC、共享内存、WS、UDP 全部改用它。

效果（120 Hz tick 模拟 1 秒）：

| 目标 | 旧实现 | 新实现 |
|---|---|---|
| 30 Hz | 24 Hz | 30 Hz |
| 60 Hz | 40 Hz | 60 Hz |
| 90 Hz | 60 Hz | 90 Hz |
| 120 Hz | 120 Hz | 120 Hz |

### 4. 可见性：CLI、状态页、探针

- CLI：`--ws-out` / `--ws-out-rate` / `--ws-out-all` / `--udp-out` / `--udp-out-rate`。
- `/api/status` 增加 `ws_out`（目标、客户端数、消息/字节）与 `udp_out`（目标、消息/字节）；
  输出页增加两行。
- 探针：`tools/probe_ws_out.py`（握手 + JSON 字段 + 实测帧率）与
  `tools/probe_udp_out.py`（长度 + magic/version + 属性位 + 非零形状）。

## 实测证据（2026-10-30，回放数据，输出指向空端口）

```
ws-out: ws://127.0.0.1:9101/ws（30.0 Hz，只发活跃形状）
udp-out: 127.0.0.1:9102（592 字节/帧，每 tick）

probe_ws_out.py  → 122 帧 / 4.0 秒（约 30.5 Hz），字段 OK
probe_udp_out.py → 481 帧 / 4.0 秒（约 120.2 Hz），magic/version/attrs/shapes OK
/api/status      → ws_out.enabled=true, udp_out.enabled=true，目标与计数正确
```

单测新增 6 个（UDP 布局/收包、WS JSON 活跃/全量、监听、节流速率），当前 139 个全过。

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| Protobuf / FlatBuffers | 引 Schema 工具链；本项目要的是"打开 Wireshark/文本编辑器就能看" |
| gRPC / MQTT | 传输层和依赖都太重，回环 UDP/TCP 已经够 |
| 复用控制面 WS 端口 | 控制面是 UI 私有协议（catalog/params/log），和数据流混在一起会让第三方客户端误连 |
| UDP 也发 JSON | 文本体积大、解析开销高；要可读性就用 WS JSON |
| 逐帧增量编码 | 接收端要维护状态，晚加入的客户端永远不同步；全量帧更简单 |

## 已知限制

- WS 只绑回环、无鉴权；它不是公网服务。
- UDP 无送达保证，目标由用户给；形状顺序是契约，增删形状要升 `version`。
- WS 慢客户端会被断开，不保留历史；需要可靠/可回放请用 `.fgbcap` 或共享内存。
- 尚未做：Unity C# 示例包（4.3）、配置预设（4.8）、5 sink 并发压力测试（4.9）。
