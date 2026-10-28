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
| 数据包 | ✅ 432 字节，`structSize=420`，3 秒 1957 个包 |
| Teardown | ✅ 确认包收到，退出码 0 |
| 无硬件时数据 | 全部 `0x4F800000`（INVALID_FLOAT），符合预期 |
| 未 Init 就 Teardown | ❌ 模块崩溃 0xC0000005（模块自身缺陷，见坑 2） |

## 8. 待补

- [ ] `SplitPacketChunk`（type=2）的分片格式 —— 目前包长 432 < MTU 未触发，需要构造大包场景
- [ ] `EventUpdateStatus`（type=204）的载荷格式
- [ ] 连接 PICO 硬件时的真实数值范围（Gaze / Openness 的实际分布）
- [ ] 连续运行时的丢包率与时间戳行为
