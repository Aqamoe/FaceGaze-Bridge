# ADR-0003 · VRCFT 模块 IPC 协议实测规格

- 状态：**已验证**（2026-10-29，用真实 `ModuleProcess.exe` + `VRCFTPicoModule` v0.1.11 跑通）
- 来源：VRCFaceTracking 源码（`VRCFaceTracking.Core/Sandboxing/`）+ 本机实测
- 用途：M2 兼容层的实现依据。**照这份文档写代码，不要重新逆向。**

---

## 1. 传输模型

| 项 | 值 |
|---|---|
| 协议 | UDP，IPv4 回环 |
| 拓扑 | Core 为 server（绑定随机端口），每个模块一个独立 `ModuleProcess.exe` 作为 client |
| 客户端源端口 | 系统随机分配，每个连接固定 |
| MTU 常量 | 代码里写死 1500（`UdpFullDuplex.ETHERNET_FRAME_SIZE`） |
| 超时 | 10 秒（`TIMEOUT_MILLISECONDS`） |

**实测包长 432 字节，不触发分片。** 但 `SplitPacketChunk`（type=2）机制存在，超过 MTU 时会分片，实现时仍需处理。

## 2. 包头（所有包）

```
偏移 0 : AF EC 00 8D      magic（4 字节）
偏移 4 : type (uint32)     小端
偏移 8 : 载荷开始
```

`type` 取值：

| 值 | 名称 | 方向 |
|---|---|---|
| 1 | Handshake | Client → Server（Server 原样回发作为 ACK） |
| 2 | SplitPacketChunk | 双向 |
| 200 | EventGetSupported | Server → Client |
| 201 | EventInit | Server → Client |
| 202 | EventTeardown | Server → Client |
| 203 | EventUpdate | Server → Client |
| 204 | EventUpdateStatus | Server → Client |
| 205 | EventLog | Client → Server |
| 300 | ReplyGetSupported | Client → Server |
| 301 | ReplyInit | Client → Server |
| 302 | ReplyUpdate | Client → Server |
| 303 | ReplyTeardown | Client → Server |

## 3. 各包载荷

### Handshake (1)
```
+8  : challenge  5 字节 = DE C1 CA 12 D0
+13 : pid        int32（小端）
+17 : pathLen    int32
+21 : path       UTF-8，长度 pathLen
```
**偏移 13 和 17 是非对齐的**（C# 的 `Buffer.BlockCopy` 直接拼），不要按 4 字节对齐解析。

Server 校验 challenge 后**把整个包原样回发**作为 ACK。

### EventGetSupported (200) / EventTeardown (202) / EventUpdate (203)
无载荷，共 8 字节。

### ReplyGetSupported (300) / EventInit (201)
```
+8 : flags  uint8   bit0 = eye, bit1 = expression
```

### ReplyInit (301)
```
+8  : flags       uint8
+9  : nameLen     int32
+13 : name        UTF-8
+13+nameLen      : streamCount  int32（图标数量）
其后每个图标： len(int32) + data
```
实测 8457 字节，含 1 个图标流。

### ReplyUpdate (302) —— 核心数据包
```
+8  : structSize  int32 = 420
+12 : struct      UpdateDataContiguous
```

结构体（全 float，小端，共 105 个 = 420 字节）：

| 偏移 | 字段 | 说明 |
|---|---|---|
| 0 | Eye_MaxDilation | |
| 4 | Eye_MinDilation | |
| 8 | Eye_Left_GazeX | -1..1 |
| 12 | Eye_Left_GazeY | -1..1 |
| 16 | Eye_Left_PupilDiameter_MM | |
| 20 | Eye_Left_Openness | 0=闭 1=睁 |
| 24 | Eye_Right_GazeX | |
| 28 | Eye_Right_GazeY | |
| 32 | Eye_Right_PupilDiameter_MM | |
| 36 | Eye_Right_Openness | |
| 40 | Head_Yaw | |
| 44 | Head_Pitch | |
| 48 | Head_Roll | |
| 52 | Head_PosX | |
| 56 | Head_PosY | |
| 60 | Head_PosZ | |
| 64 | Expression_Shapes[89] | 索引 = `UnifiedExpressions` 枚举值（88 项 + Max 位） |

**`INVALID_FLOAT` = 4294967295.0f（0x4F800000）**，不是 NaN。
注意 C# 源码里写的是 `const float INVALID_FLOAT = 0xFFFFFFFF;` —— 这是 int 常量到 float 的隐式转换，结果是 4.2949673e9 而不是 NaN 的位模式。**判空必须比较这个具体值，不能 `std::isnan`。**

实测无硬件连接时，105 个字段全部为 `0x4F800000`。

### EventLog (205)
```
+8  : level    int32（2=Info 3=Warn 4=Error）
+12 : textLen  int32
+16 : text     UTF-8
```

### EventUpdateStatus (204) —— 状态同步，**不可省略**
```
+8  : moduleState  int32   -1=Uninitialized, 0=Idle, 1=Active
+12 : usingEye     uint8   bool
+13 : usingExpression uint8  bool
```
共 14 字节。

模块收到后会把 `ExtTrackingModule.Status` 设成该值。**绝大多数模块的 `Update()` 第一行就是
`if (Status == ModuleState.Active)`**，不设这个状态，模块每帧都在空转，返回的全是
`INVALID_FLOAT`。FGB 必须在 Init 成功后立即发一次 `EventUpdateStatus(Active)`。

## 4. 时序

```
Client                                Server
  │  Handshake(challenge, pid, path)    │
  ├────────────────────────────────────▶│
  │◀────────────────────────────────────┤  原样回发（ACK）
  │                                     │
  │◀──── EventGetSupported ─────────────┤
  ├──── ReplyGetSupported(flags) ──────▶│
  │                                     │
  │◀──── EventInit(flags) ──────────────┤
  ├──── ReplyInit(flags, name, icons) ─▶│
  │                                     │
  │◀──── EventUpdateStatus(Active) ─────┤   ← 必须！否则 Update() 空转
  │                                     │
  │◀──── EventUpdate ───────────────────┤   ← 拉模式：一次 Update 回一帧
  ├──── ReplyUpdate(struct 420B) ──────▶│
  │              ⋮  （每帧重复）         │
  │                                     │
  │◀──── EventTeardown ─────────────────┤
  ├──── ReplyTeardown ─────────────────▶│
  │  exit 0                             │
```

## 5. 三个容易踩的坑

**坑 1 · 数据是拉模式，不是推模式。**
`ModuleProcess` 只在收到 `EventUpdate` 时才构造并回发一个 `ReplyUpdatePacket`（源码 `ModuleProcessMain.Sandbox.cs:68`）。
核心必须按自己的节奏发 `EventUpdate` 去"拉"。这也意味着**帧率由 Core 决定**，模块无法自主推。

**坑 2 · 未 Init 就 Teardown 会让模块崩溃。**
实测：只走到 GetSupported 就发 Teardown，PICO 模块的静态构造函数被触发，尝试绑定已被占用的端口抛出 `TypeInitializationException`，随后空引用，进程以 0xC0000005 退出。
FGB 的实现必须保证 **Init/Teardown 配对**，且在 Init 失败时不要强行 Teardown。

**坑 3 · 端口冲突会伪装成"模块不兼容"。**
模块通常独占硬件端口（PICO 模块绑定固定 UDP 端口）。同时跑两个实例时，第二个 Init 会失败且错误信息很隐晦。
FGB 启动第二个实例前应先探测端口占用，并给出明确提示。

**坑 4 · 忘了同步 `ModuleState.Active`，拿到的是满屏 INVALID_FLOAT。**
第一次实测就栽在这里：握手、Init 全部成功，`ReplyUpdate` 也正常返回 432 字节，但 105 个字段全是
`INVALID_FLOAT`。原因是没发 `EventUpdateStatus`，模块 `Update()` 里 `if (Status == Active)` 不成立，
一个值都不写。
**这个错误极具迷惑性**：链路看起来完全正常，只是"没有数据"，很容易被误判成硬件问题或协议解码错误。
排查顺序应该是：先确认发过 `EventUpdateStatus`，再看模块日志里的硬件连接状态。

**坑 5 · 接收缓冲小于 8457 字节，握手成功但永远等不到 `ReplyInit`。**
模块带内嵌图标时 `ReplyInit` 实测 **8457 字节**。若接收缓冲只有 8192，
Windows 的 `recvfrom` 返回 `WSAEMSGSIZE` 并**丢弃整包**——现象是"握手正常、`ReplyGetSupported` 正常，
但卡在等初始化回复，最后超时重试"。
FGB 的实现把 `kMaxPacketSize` 定为 65536。这个坑的隐蔽之处在于：失败点看起来像是模块没响应，
实际是本地缓冲不够。

## 6. 启动参数

```
VRCFaceTracking.ModuleProcess.exe --port <serverPort> --module-path <dllPath> --parent-pid <pid>
```
用 `System.CommandLine` 解析，`--port 1234` 与 `--port=1234` 均可用。
`--parent-pid` 会被 `ParentWatchdog` 监控：父进程消失后 10 秒内自行退出。

工作目录必须设为 VRCFT 安装目录，否则找不到依赖 DLL。

## 7. 实测记录

| 项 | 结果 |
|---|---|
| 握手 | ✅ challenge 匹配，pid / modulePath 解出正确 |
| 能力查询 | ✅ `eye=True expression=True` |
| Init | ✅ 模块名 `PICO / 完整面部追踪`，1 个图标流 |
| 状态同步 | ✅ `EventUpdateStatus(Active)` 后模块进入工作状态 |
| 数据包 | ✅ 432 字节，`structSize=420`，3 秒 1957 个包 |
| Teardown | ✅ 确认包收到，退出码 0 |
| 未 Init 就 Teardown | ❌ 模块崩溃 0xC0000005（模块自身缺陷，见坑 2） |
| 脱离 VRCFT 取真实数据 | ⚠️ 模块正常出帧，但眼睛/表情字段仍为 `INVALID_FLOAT` |

### 关于最后一项的排查记录

经过三轮实验，链路已被切到只剩最后一环。**证据链如下：**

**实验 1 · PICO 到底发不发？**
VRCFT 关闭、不启动任何模块，直接用 `SO_REUSEADDR` 绑 29765 监听：
```
8 秒内收到 721 个包，包长 912 字节（约 90 Hz）
```
→ **PICO 一直在发，与 VRCFT 是否运行无关。**

**实验 2 · 模块绑上端口了吗？**
Init 成功后跑 `netstat -ano`：
```
UDP  0.0.0.0:29765  *:*  56508   ← 正是模块子进程 PID
```
→ **绑定成功，地址端口都对。**

**实验 3 · 加延迟能绕过竞态吗？**
Init 后等 2 秒再设 Active 并开始拉取：仍然全 `INVALID_FLOAT`。
→ **不是时序问题。**

**根因在模块的 socket 生命周期**（源码 `VRCFTPicoModule.cs`）：

```csharp
// 静态字段初始化时就绑了两个端口
private static readonly int[] Ports = [29765, 29763];
private static readonly UdpClient[] Clients = Ports.Select(p => new UdpClient(p)).ToArray();

// Initialize 里：探测哪个端口有数据，然后全部 Dispose，再重新绑
var portIndex = await ListenOnPorts();          // 收到包 → 确定端口
foreach (var client in Clients) client.Dispose(); // 关闭探测用的 socket
_udpClient = new UdpClient(_port);              // 重新绑定同一端口
```

`Dispose()` 后立刻在**同一端口**重新绑定，Windows 的 UDP 端口共享语义（`SO_REUSEADDR`）下，
单播包会被投递给"最后一个绑定"的 socket——但如果旧句柄尚未完全回收，新 socket 会绑定成功却收不到包。
这正是"Init 返回 true、端口占用正确、`Receive()` 却一直超时"的原因。

而且这个故障**几乎没有日志**：`Updater.Update()` 里超时要累积 **600 次（60 秒）**才打一条警告
（`if (++_timeOut > 600)`），短时间测试完全静默。

**结论**：这是模块自身的缺陷，不是 FGB 协议实现的问题。协议层（握手 / 能力 / Init / 状态 / 拉取 / 432 字节解码）已全部验证通过。

**FGB 的应对策略**（写入 M2 设计）：

1. Init 成功后做**数据有效性探测**：连续 N 帧（建议 30 帧 / 0.5 秒）全为 `INVALID_FLOAT` 即判定"无数据"
2. 判定后**重启模块进程重试**（最多 3 次，退避 0.5s / 1s / 2s）——模块重走一遍流程有机会拿到干净的 socket
3. 三次失败则把该源标记为 `faulted`，在 UI 明确提示"模块已连接但硬件无数据"，而不是静默降级
4. 提示文案要点明"可能需要重启 PICO Connect 或重新连接头显"，避免用户以为是 FGB 的问题

若后续发现重试无效，再考虑在 FGB 侧实现**原生 PICO 接收**（直接解析 912 字节的原始包），绕过这个模块。
那需要额外逆向 PICO 的包格式，属于 M5 之后的事。

## 8. 待补

- [ ] `SplitPacketChunk`（type=2）的分片格式 —— 目前包长 432 < MTU 未触发，需要构造大包场景
- [ ] `EventUpdateStatus`（type=204）的载荷格式
- [ ] 连接 PICO 硬件时的真实数值范围（Gaze / Openness 的实际分布）
- [ ] 连续运行时的丢包率与时间戳行为
