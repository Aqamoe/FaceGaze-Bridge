# 01 · 系统架构设计

> 本文回答三个问题：VRCFT 是怎么工作的、C++ 重写的边界应该画在哪、FGB 的架构长什么样。

---

## 1. VRCFT 技术解剖

在动手重写前，先把被重写对象拆开看清楚。以下结论基于 VRCFaceTracking v5.x（.NET 8，后续版本升到 .NET 10）的公开文档、SDK 文档与源码结构。

### 1.1 程序集划分

| 程序集 | 职责 | 语言 |
|---|---|---|
| `VRCFaceTracking` | WinUI 3 前端，卡片式页面（Dashboard / Modules / Parameters / Output / Settings） | C# |
| `VRCFaceTracking.Core` | 引擎：模块管理、数据突变管线、参数生成、OSC 收发、OSCQuery、头像配置解析、SteamVR/OpenVR 集成 | C# |
| `VRCFaceTracking.SDK` | 模块开发者的抽象基类 `ExtTrackingModule` + `ModuleMetadata` + `ModuleState` | C# |
| `VRCFaceTracking.ModuleProcess` | 模块沙箱宿主进程，加载单个模块 DLL 并回收数据 | C# |
| `fti_osc` | OSC 序列化的原生库（Windows/macOS/Linux 三份二进制） | C/C++ |

值得注意：VRCFT 已经把最热路径（OSC 编码）下沉到了原生库 `fti_osc`。这说明**在 .NET 里做高频 OSC 是有性能压力的**——这是 C++ 重写最硬的理由之一。

### 1.2 数据流（v5 沙箱模型）

```
硬件 / 驱动 SDK
      │
      ▼
┌──────────────────────────────────────┐
│ VRCFaceTracking.ModuleProcess.exe    │  ← 每个模块一个进程
│  ├ AssemblyLoadContext 加载模块 DLL  │
│  ├ 校验引用 VRCFaceTracking.Core≥5.0 │
│  ├ 循环调用 ExtTrackingModule.Update()│  ~100 Hz
│  └ 读回 UnifiedTracking.Data（静态）  │
└──────────────┬───────────────────────┘
               │ UDP 全双工 · ReplyUpdatePacket · 分片(PartialPacket)
               ▼
┌──────────────────────────────────────┐
│ VRCFaceTracking.Core                 │
│  ├ VrcftSandboxServer 收包           │
│  ├ 写入全局 UnifiedTracking.Data      │
│  ├ UnifiedTrackingMutator（校准/滤波）│
│  ├ 参数生成（v2 全量 + v1 legacy）    │
│  └ ParameterSenderService → OSC      │
└──────────────┬───────────────────────┘
               │ OSC over UDP :9000
               ▼
        VRChat（avatar 参数 / 原生眼追）
```

### 1.3 模块生命周期协议

模块宿主的握手与数据同步是一套自定义 UDP 协议，包类型包括：

| 包 | 方向 | 语义 |
|---|---|---|
| `HandshakePacket(pid, modulePath)` | Host → Core | 沙箱进程自报身份，Core 校验 PID 后回 ACK |
| `EventInitGetSupported` | Core → Host | 询问模块支持能力 |
| `EventInitPacket(eye, expression)` | Core → Host | 下发初始化意图 |
| `ReplySupportedPacket` / `ReplyInitPacket` | Host → Core | 回报能力与实际初始化结果 |
| `ReplyUpdatePacket` | Host → Core | 每帧统一追踪数据（分片传输） |
| `EventUpdatePacket` | Core → Host | 手动 tick（`--vrcft-nothread` 模式） |
| `EventTeardownPacket` / `ReplyTeardownPacket` | 双向 | 优雅卸载 |
| `EventStatusUpdatePacket` / `EventLogPacket` | Host → Core | 状态与日志回传 |

另有 `ParentWatchdog` 线程：主进程消失后 10 秒内自行优雅退出，防止硬件句柄泄漏。退出码 `0/‑1/‑2/‑3` 分别表示正常/参数错误/网络超时/崩溃。

**这个设计对我们极其有利**：Core 与 Host 之间的边界是一个网络协议，而不是函数调用。意味着 **FGB 只要实现协议的另一端，就能复用整套 .NET 模块生态，无需自己写 CLR 宿主。**

### 1.4 VRCFT 的结构性局限

这些不是"做得不好"，而是架构选择带来的天花板，也正是 FGB 的机会：

| 局限 | 后果 |
|---|---|
| 单进程单实例、面向单一消费者 | 一次只能服务一个目标程序（VRChat）；要同时喂 VMC 和 Unity 就得跑多份 |
| `UnifiedTracking.Data` 是全局静态单例 | 无法多来源并行、无法 A/B 对比、无法录制回放 |
| 数据模型只有 `float`，无置信度/时间戳 | 无法做延迟补偿、无法表达"这一帧不可信" |
| 校准能力几乎为零（依赖硬件自校准） | 廉价硬件 / 摄像头方案效果差 |
| 模块与引擎强耦合（模块直接引用 Core） | 模块生态锁定在 .NET，性能无法突破 |
| 无注视点/空间数据的对外接口 | 只能驱动头像，做不了渲染侧的事 |

---

## 2. C++ 重写的边界

**诚实的结论：不该"全部用 C++ 重写"。**

把 VRCFT 逐行翻译成 C++ 会得到两个坏结果：一是丢掉整个 .NET 模块生态（这是 FGB 最大的存量优势），二是用 C++ 重写 UI 会付出 3–5 倍工作量却得到更差的界面。

正确的做法是**按"热路径 / 冷路径"切分**：

| 模块 | 语言 | 理由 |
|---|---|---|
| 采集与融合（每帧执行） | **C++** | 微秒级预算、无 GC、cache 友好 |
| 映射/滤波/校准解算 | **C++** | 同上，且需要 SIMD |
| OSC 编解码与发送 | **C++** | 每秒上万次序列化，VRCFT 都要为此下沉原生库 |
| 协议接入（VMC/UDP/共享内存） | **C++** | 同上 |
| OpenXR / D3D / GPU 侧 | **C++** | 无替代方案 |
| VRCFT .NET 模块加载 | **C#（宿主进程）** | 唯一能加载 .NET 程序集的方式 |
| UI | **TypeScript（WebView2）** | 迭代速度 > 原生感，且可跨平台复用 |
| 配置/注册表/更新 | **C++ 或 TS** | 无所谓，按便利性 |

一句话：**C++ 做引擎，C# 做兼容桥，TS 做脸。**

有人会问「为什么不干脆放弃 VRCFT 模块兼容」。因为那等于放弃 Meta Quest Pro / PICO / Vive / Index / Varjo / iPhone LiveLink / MediaPipe 等几十个现成硬件适配——一个团队两年也补不齐。兼容是杠杆，不是包袱。

---

## 3. 总体架构

### 3.1 分层图

```
┌───────────────────────────────────────────────────────────────────────┐
│  Shell 层                                                              │
│  FaceGaze Bridge (WebView2)  fgb-cli  fgb-tray  [Unity/UE 插件]        │
└───────────────────────────────┬───────────────────────────────────────┘
                                │ 本地 IPC（WebSocket + JSON / 命名管道 + 二进制）
┌───────────────────────────────▼───────────────────────────────────────┐
│  Service 层：fgb-service                                               │
│  · 会话管理  · 配置  · 模块安装/更新  · 日志  · 遥测（可选、默认关）      │
└───────────────────────────────┬───────────────────────────────────────┘
┌───────────────────────────────▼───────────────────────────────────────┐
│  Core 层（无 I/O，纯计算，可单测）                                       │
│                                                                        │
│   ┌────────────┐   ┌────────────┐   ┌──────────┐   ┌───────────────┐  │
│   │ SourceMgr  │──▶│  Framer    │──▶│ Resolver │──▶│ FilterChain   │  │
│   │ 生命周期    │   │ 时间对齐    │   │ raw→统一  │   │ 平滑/限速/死区 │  │
│   │ 健康/重启   │   │ 丢帧/外推   │   │ 形态      │   │               │  │
│   └────────────┘   └────────────┘   └──────────┘   └───────┬───────┘  │
│                                                             ▼          │
│   ┌────────────┐   ┌────────────┐   ┌──────────┐   ┌───────────────┐  │
│   │ Calibrator │◀──│GazeEstimator│◀─│  Frame   │◀──│  Snapshot     │  │
│   │ 偏移/漂移   │   │ 注视/聚散/眨眼│  │  Store   │   │  (seqlock)    │  │
│   └────────────┘   └────────────┘   └──────────┘   └───────┬───────┘  │
│                                                             ▼          │
│                                                    ┌───────────────┐  │
│                                                    │  SinkManager  │  │
│                                                    │ 节流/相关性/扇出│  │
│                                                    └───────┬───────┘  │
└────────────────────────────────────────────────────────────┼──────────┘
┌────────────────────────────────────────────────────────────▼──────────┐
│  HAL / I/O 层                                                          │
│                                                                        │
│  输入                             输出                                  │
│  ├ fgb-host-vrcft.exe (.NET)      ├ OSC/UDP → VRChat (参数 + 原生眼追)  │
│  ├ fgb-host-native.exe (C++)      ├ OSCQuery（mDNS → 动态端口，参数裁剪）│
│  ├ VMC in (UDP 39539)             ├ VMC out (UDP 39539)                │
│  ├ OSC in (任意地址映射)           ├ 共享内存 (Unity/UE/OpenXR Layer)   │
│  ├ OpenXR 眼追 (XR_EXT_eye_gaze)   ├ WebSocket JSON (第三方应用)        │
│  ├ ALVR / VD / PICO UDP            ├ UDP 二进制 (串流软件, 最低延迟)     │
│  └ 摄像头 (MediaPipe / OSF 子进程)  └ 音频 (双耳渲染, 后期)              │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.2 设计原则

1. **Core 无 I/O**。Core 层不打开 socket、不读文件、不知道 VRChat 存在。它只做 `Frame in → Frame out`。这让 Core 可以 100% 单元测试，也让它能被嵌进别人的进程。
2. **单向数据流**。数据只从 Source 流向 Sink，反向只有控制信令。没有回调地狱。
3. **无锁快照**。核心链路零 mutex，用 SPSC ring + seqlock 发布状态。
4. **降级优先**。任何来源失效都必须有明确定义的 fallback，绝不允许"整体不工作"。
5. **一切可观测**。每个阶段都能吐出时间戳和中间值，否则延迟问题永远查不出来。

---

## 4. 进程模型

### 4.1 为什么是多进程

硬件 SDK 是稳定性黑洞：SRanipal 会挂、PICO 的 UDP 会卡死、摄像头驱动会蓝屏。VRCFT v5 引入 `ModuleProcess` 正是为此。FGB 继承这个结论并推广到所有输入源。

```
fgb-service.exe            ← 主进程，UI 与引擎
├── fgb-host-vrcft.exe     ← 每个 VRCFT 模块一个（可由用户配置为共享进程）
├── fgb-host-native.exe    ← 每个原生模块一个（可选 in-proc 低延迟模式）
├── fgb-cam-mediapipe.exe  ← 摄像头推理子进程（模型崩溃不影响主体）
└── fgb-openxr-layer.dll   ← 注入到目标进程的 OpenXR API Layer（独立地址空间）
```

### 4.2 进程间通道选型

| 通道 | 用途 | 理由 |
|---|---|---|
| **共享内存 ring + eventfd/Event** | 原生模块 → Core 的帧传输 | 零拷贝，亚微秒级 |
| **UDP 回环** | VRCFT 模块兼容通道 | 必须匹配 VRCFT 协议 |
| **命名管道 + 长度前缀二进制** | Core ↔ Service 控制面 | 双向、可靠、Windows 原生 |
| **WebSocket + JSON** | UI / 第三方 / 调试 | 人类可读、跨语言、浏览器原生 |
| **UDP 单播** | 对外输出（OSC/VMC） | 协议要求 |

### 4.3 崩溃隔离与自愈

- 每个 host 进程由 Core 的 `SourceMgr` 监管，心跳超时 500 ms 判定失联。
- 失联后按 `restart_policy`（`never` / `on-failure` / `always`）处理，退避序列 0.5s → 1s → 2s → 5s → 10s，连续 5 次失败则标记为 `faulted` 并在 UI 高亮。
- host 进程被杀死时，Core 立即将该源标记为 `lost`，触发 fallback 链（见 §7.3），保证输出不中断。
- 崩溃转储由 Crashpad 收集到本地，**默认不上传**。

---

## 5. 线程与调度

### 5.1 线程清单

| 线程 | 频率 | 职责 | 实时性要求 |
|---|---|---|---|
| `SourceThread × N` | 硬件决定（30–200 Hz） | 拉取/接收原始帧，写入 ring | 中 |
| `MergeThread` | 固定 tick（默认 120 Hz） | 合并、对齐、外推、发布快照 | **高（抖动 < 1 ms）** |
| `SinkThread` | 固定 tick（默认 90 Hz） | 扇出到各输出，节流 | 中高 |
| `GazeThread` | 120 Hz | 聚散度、扫视/注视检测、眨眼分类 | 中 |
| `ControlThread` | 事件驱动 | IPC、配置热重载、模块管理 | 低 |
| `TelemetryThread` | 1 Hz | 指标聚合 | 低 |

### 5.2 定时精度

Windows 默认定时器分辨率 15.6 ms，对 120 Hz（8.33 ms）完全不够。方案：

```cpp
// 1. 提升系统时钟分辨率（进程级，退出时恢复）
timeBeginPeriod(1);

// 2. 使用高精度等待对象
HANDLE timer = CreateWaitableTimerExW(
    nullptr, nullptr,
    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,  // Win10 1803+
    TIMER_ALL_ACCESS);

// 3. 混合等待：粗睡 + 自旋收尾，把抖动压到 <200 µs
```

实测目标：`MergeThread` 的唤醒抖动 P99 < 500 µs。

### 5.3 无锁数据通路

```cpp
// 每个 source 一个 SPSC ring，容量 = 2 的幂，元素为定长 Frame
template <typename T, size_t N>
class SpscRing {
    alignas(64) std::atomic<uint64_t> head_{0};   // 生产者写
    alignas(64) std::atomic<uint64_t> tail_{0};   // 消费者写
    alignas(64) std::array<T, N> buf_;
public:
    bool try_push(const T& v) noexcept;
    bool try_pop(T& out) noexcept;
};

// 全局快照用 seqlock：读者永不阻塞写者
class SnapshotStore {
    alignas(64) std::atomic<uint64_t> seq_{0};
    UnifiedFrame frame_[2];
    std::atomic<int> active_{0};
public:
    void publish(const UnifiedFrame& f) noexcept {
        const uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);       // odd = 写入中
        std::atomic_thread_fence(std::memory_order_release);
        frame_[active_.load(std::memory_order_relaxed)] = f;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);       // even = 稳定
    }
    bool read(UnifiedFrame& out) const noexcept;            // 重试直到 seq 为偶数且前后一致
};
```

**为什么不用 `std::mutex`**：输出线程若被 UI 线程抢锁 1 ms，就会造成一次输出抖动；而抖动比延迟更影响观感。

---

## 6. 数据模型

### 6.1 核心帧结构

VRCFT 的 `UnifiedTracking.Data` 只有形状数组和眼球状态，缺三样关键信息：**时间、置信度、来源**。FGB 的帧结构补上：

```cpp
namespace fgb {

inline constexpr uint32_t kAbiVersion = 1;
inline constexpr size_t   kShapeCount = static_cast<size_t>(Shape::Count); // ~120

enum class Shape : uint16_t {
    // 与 VRCFT Unified Expressions v2 一一对应，顺序即线上协议顺序
    EyeLidLeft, EyeLidRight, EyeWideLeft, EyeWideRight,
    EyeSquintLeft, EyeSquintRight,
    BrowDownLeft, BrowDownRight, BrowInnerUp, BrowOuterUpLeft, BrowOuterUpRight,
    CheekPuffLeft, CheekPuffRight, CheekSquintLeft, CheekSquintRight,
    JawOpen, JawX, JawZ, JawClench, JawMandibleRaise,
    MouthClosed, MouthX, MouthUpperX, MouthLowerX,
    MouthUpperUpLeft, MouthUpperUpRight, MouthLowerDownLeft, MouthLowerDownRight,
    MouthCornerPullLeft, MouthCornerPullRight,
    MouthCornerSlantLeft, MouthCornerSlantRight,
    MouthFrownLeft, MouthFrownRight,
    MouthDimpleLeft, MouthDimpleRight,
    MouthStretchLeft, MouthStretchRight,
    MouthPressLeft, MouthPressRight,
    MouthTightenerLeft, MouthTightenerRight,
    MouthRaiserUpper, MouthRaiserLower,
    LipPucker, LipPuckerUpperLeft, LipPuckerUpperRight, LipPuckerLowerLeft, LipPuckerLowerRight,
    LipFunnel, LipFunnelUpperLeft, LipFunnelUpperRight, LipFunnelLowerLeft, LipFunnelLowerRight,
    LipSuckUpper, LipSuckLower,
    NoseSneerLeft, NoseSneerRight,
    NasalDilationLeft, NasalDilationRight, NasalConstrictLeft, NasalConstrictRight,
    TongueOut, TongueX, TongueY, TongueRoll, TongueArchY, TongueShape,
    TongueTwistLeft, TongueTwistRight,
    NeckFlexLeft, NeckFlexRight,
    SoftPalateClose, ThroatSwallow,
    // ... 完整列表随 UE v2 规范对齐
    Count
};

// 单眼状态：带有效性，而不是用 0 伪装
struct EyeState {
    bool      valid           = false;
    float     openness        = 0.0f;   // 0 = 闭合, 1 = 完全睁开
    float     opennessConf    = 0.0f;   // 0..1
    float     pupilDiameterMm = 0.0f;   // 0 表示不可用
    float     pupilDilation   = 0.0f;   // -1..1，0 为基线
    Vec3      gazeDirLocal    = {0, 0, 1};  // HMD 局部，单位向量，OpenXR 右手系
    Vec2      gazePitchYawDeg = {0, 0};     // 缓存的球坐标，避免重复三角函数
    float     gazeConf        = 0.0f;
};

struct EyePair {
    EyeState left, right;
    bool     hasVergence      = false;
    float    vergenceDistanceM = 0.0f;  // 双眼汇聚距离（米），>0 有效
};

struct FaceFrame {
    uint64_t  captureTsNs   = 0;   // 采集时刻（单调时钟）
    uint64_t  publishTsNs   = 0;   // 引擎发布时刻
    uint64_t  sequence      = 0;   // 单调递增，用于丢帧检测

    EyePair   eyes;
    std::array<float, kShapeCount> shapes{};      // 0..1，未定义项为 0
    std::array<uint8_t, kShapeCount> shapeValid{}; // 位图化后可压缩

    // 溯源：这一帧到底是谁给的
    SourceId  source          = kInvalidSource;
    uint32_t  contributingSources = 0;  // 位掩码（允许多源融合）
    float     qualityScore    = 1.0f;   // 0..1，综合置信度

    // 头部姿态（做注视点渲染必需，头像驱动不需要）
    bool      hasHeadPose = false;
    Quat      headRotLocal{};
    Vec3      headPosLocal{};
};

} // namespace fgb
```

### 6.2 三层数据：Raw → Unified → Posed

```
Raw（原始）      各来源的原生格式（ARKit 52 维、SRanipal、MediaPipe 478 点…）
   │  归一化：单位、坐标系、取值范围
   ▼
Unified（统一）  120 维 UE v2 形状 + 眼球状态，无量纲 0..1
   │  姿态化：叠加头部位姿、时间戳
   ▼
Posed（姿态化）  世界空间注视射线、聚散点、头部位姿 —— 供渲染/音频使用
```

**为什么要保留 Raw 层**：不同来源的"张嘴"物理含义不同（ARKit 的 `jawOpen` 是相对下颌旋转角，MediaPipe 的是几何比值）。统一层一旦压平就回不去了，导致无法做跨源融合、无法做来源特定的二次校准、也无法在调试时回答"到底是哪一步出错了"。Raw 层按需开启（默认只在录制时保存）。

### 6.3 遗留参数（v1）

VRCFT 同时输出 v1 legacy 参数（`EyeLidLeft`、`MouthSmileLeft` 等）和 v2 统一参数。v1 由 v2 派生：

- `EyeLidLeft = 1 - EyeLidLeft`（v1 里 1 表示睁眼，v2 里 1 表示闭眼 —— **这是历史上最经典的踩坑点**）
- `MouthSmileLeft = max(MouthCornerPullLeft, MouthCornerSlantLeft)`
- `JawOpen` 直接映射

这套派生规则在 VRCFT 里有专门的 `LipShapeMerger` / `UnifiedSimplifier`，FGB 需要逐条复刻以保证老头像不翻车。**这是最容易"看起来做完了但用户头像变形"的地方，必须用官方 avatar 做像素级对比验收。**

---

## 7. 时间与坐标规范

### 7.1 时间

| 项 | 规定 |
|---|---|
| 时钟源 | `QueryPerformanceCounter`（单调），启动时与 `GetSystemTimePreciseAsFileTime` 建立一次映射 |
| 时间戳语义 | `captureTsNs` = 数据"在物理世界发生的时刻"，不是"我们收到的时刻" |
| 来源能力差异 | OpenXR 可给 `predictedDisplayTime`；UDP 源只能给 `arrivalTs - estimatedLatency`；摄像头源用帧采集回调时间 |
| 对齐策略 | `MergeThread` 在 `now - targetLatency` 处对两帧做线性插值；无历史则外推（最多 2 帧，超出则冻结并标记 `stale`） |
| 陈旧判定 | 某源超过 `3 × 标称周期` 无数据 → `stale`；超过 500 ms → `lost` |

**为什么要这么较真**：VRChat 的眼追有 10 秒超时重置，但用户的真实痛点是"眨眼比表情慢半拍"——这是端到端时间不对齐造成的，不是网络问题。

### 7.2 坐标

内部统一采用 **OpenXR 约定：右手系，+Y 上，−Z 前，米**。理由：

1. OpenXR 是唯一跨厂商的权威标准；
2. 主流硬件 SDK（Meta、PICO、Varjo、HTC）原生都是右手系；
3. `glm` 默认右手系，减少转换。

转换集中在一个地方：

```cpp
// 唯一允许出现坐标系转换的文件：core/coord.hpp
namespace fgb::coord {
    // OpenXR (RH, -Z fwd) → Unity/VRChat (LH, +Z fwd)
    constexpr Vec3 to_unity(Vec3 v) { return { v.x, v.y, -v.z }; }
    // Unity 欧拉（度，ZXY 序）→ 方向向量
    Vec3 unity_euler_to_dir(float pitchDeg, float yawDeg);
}
```

**纪律**：除 `coord.hpp` 外，任何文件出现 `-v.z` 这类字面量都视为 bug。用一个 grep 规则卡在 CI 里。

### 7.3 Fallback 链

```
优先级 1: 专用硬件眼追（Quest Pro / Vive Pro Eye / PICO / Varjo）
优先级 2: 摄像头眼追（MediaPipe Iris / OpenSeeFace）
优先级 3: 从 VRChat 反向读取 /tracking/vrsystem/head/pose 推断（仅当用户明确开启）
优先级 4: 中性姿态（眼睛睁开、注视正前）
```

表情同理。**关键规则**：降级时不是"停止发送"，而是**发送中性值并降低发送频率**。停止发送的后果分两种：`native` 模式下 VRChat 会在 10 s 后把控制权交还内置自动注视，头像"先僵住再自己乱动"；`parameters` 模式下参数会定格在最后一帧，表情僵在某个鬼脸。两者都很难看，持续发中性值可以避免。

---

## 8. 模块系统：三层并存

### 8.1 A 层 · VRCFT 兼容（首要目标）

**原理**：不写 CLR 宿主，而是实现 `VrcftSandboxServer` 的对端，直接驱动官方 `VRCFaceTracking.ModuleProcess.exe`。

```
fgb-service.exe
   │  ① 扫描模块：%AppData%\VRCFaceTracking\CustomLibs\*.dll + metadata.json
   │  ② CreateProcess("VRCFaceTracking.ModuleProcess.exe", "--port <p> --module <dll>")
   │  ③ UDP :<p> 收 HandshakePacket(pid, modulePath) → 校验 → 回 ACK
   │  ④ 发 EventInitGetSupported → 收 ReplySupportedPacket
   │  ⑤ 发 EventInitPacket(eye, expr) → 收 ReplyInitPacket
   │  ⑥ 循环收 ReplyUpdatePacket（分片重组）→ 解码为 FaceFrame
   │  ⑦ 退出时发 EventTeardownPacket → 等 ReplyTeardownPacket → 收割进程
   ▼
FaceFrame
```

**三个必须做对的细节：**

1. **分片重组**。`ReplyUpdatePacket` 会被 `PartialPacket` 切分，UDP 不保证顺序。需要维护 per-source 的重组缓冲，带超时丢弃（建议 100 ms），否则一个丢包会让后续所有帧错位。
2. **版本指纹**。协议字段在不同 VRCFT 版本间变过。启动时先做一次探测握手，根据 `ReplySupportedPacket` 的字段布局判定协议版本，选择对应解码器。未知版本 → 降级为"仅透传，不解码"，并在 UI 提示。
3. **自带一份 pinned 版本**。不依赖用户已安装 VRCFT。发布包里带一份经测试的 `ModuleProcess.exe` + `VRCFaceTracking.Core.dll` + `VRCFaceTracking.SDK.dll`（Apache-2.0 允许，保留 `LICENSE`/`NOTICE`）。用户已装 VRCFT 时可选"使用系统安装的版本"。

**性能预算**：单模块 100 Hz × 约 400 B/帧 = 40 KB/s，UDP 回环 + 一次反序列化，CPU 占用可忽略。真正的开销在 .NET 侧（模块自己的 `Update()`），不在我们这边。

**风险与兜底**：若某天 VRCFT 改了协议，A 层会失效。兜底方案是 §8.1 的自带 pinned 版本 —— 即"协议变了但我们不跟着变"，直到我们主动升级。这条兜底必须在 M0 就验证。

### 8.2 B 层 · 原生 C ABI

> **v1 已实现**（2026-10-29，见 [ADR-0018](adr/0018-native-c-abi-v1.md)）：进程内加载、
> `sdk/fgb_module_api.h`、`samples/native-sine`。与下面的纸面设计相比多了
> `shape_index(name)` 服务（模块不硬编码形状下标），帧槽 v1 是宿主堆内存，布局按跨进程兼容设计。
>
> **v1.1 已实现**（2026-10-30，见 [ADR-0030](adr/0030-native-module-v2-composition.md)）：
> 模块能力扩展 `FGB_CAP_GAZE`，合并层增加独立注视通道，注视新鲜度与头像眼睛分开；
> 多模块按角色组合（面捕 / 眼睛 / 注视 / 头姿），头像输出与 foveation 渲染输出分路。
> 属性级组合的分阶段设计见 [ADR-0031](adr/0031-native-module-v2-attribute-fusion.md)：
> 阶段 A 已支持左右眼/左右注视分源与“A 的 openness + B 的 gaze”合成（默认关闭）；
> 属性掩码 ABI 与跨进程模块宿主仍是后续阶段。

给新模块用的高性能接口。设计要点：

```c
/* fgb_module_api.h —— 唯一需要分发的头文件，纯 C，无 STL */
#define FGB_MODULE_ABI_V1 1

typedef struct FgbHostServicesV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void  (*log)(void* ctx, int level, const char* msg);
    void  (*report_status)(void* ctx, int state, const char* detail);
    /* 由宿主分配、模块写入的共享内存帧槽 */
    FgbFrameSlotV1* (*acquire_frame_slot)(void* ctx);
    void  (*commit_frame_slot)(void* ctx, FgbFrameSlotV1*, uint64_t capture_ts_ns);
    void* ctx;
} FgbHostServicesV1;

typedef struct FgbModuleV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;            // 反向域名，如 "com.example.questpro"
    const char* display_name;
    uint32_t capabilities;     // FGB_CAP_EYE | FGB_CAP_EXPRESSION | FGB_CAP_GAZE | FGB_CAP_HEAD

    bool (*initialize)(void** self, const FgbHostServicesV1*, const FgbInitParamsV1*);
    void (*update)(void* self);          /* 宿主按 tick 调用，或模块自驱 push */
    void (*teardown)(void* self);
} FgbModuleV1;

/* 唯一导出符号 —— 避免 C++ name mangling 与 STL ABI 问题 */
FGB_EXPORT const FgbModuleV1* fgb_module_query(uint32_t host_abi_version);
```

三条铁律：

- **只导出 `fgb_module_query` 一个符号**。模块内部随便用 C++20、随便用 STL，跨边界只走 POD。
- **`struct_size` 前向兼容**。宿主按 `min(module.struct_size, host.expected)` 读取，新字段追加在尾部，永不重排。
- **帧数据走共享内存，不走函数返回**。避免每次调用都拷贝 500 字节。

### 8.3 C 层 · 协议直连

这一层是差异化关键：**用户不需要装任何模块，填入 IP/端口即可**。

| 协议 | 端口 | 典型来源 | 备注 |
|---|---|---|---|
| VMC | UDP 39539 | VSeeFace / Warudo / 自制 Unity | ✅ 双向，同时是输出格式 |
| OSC（通用） | UDP 9103（可配） | 任意面捕 App | ✅ `osc_map.ini` 地址→形状/眼睛映射 |
| ARKit LiveLink | UDP 11111 | iPhone (Live Link Face) | ✅ 61 曲线二进制：52 blendshape + 头姿 + 眼姿；未真机验收 |
| MeowFace / iFacialMocap | UDP/HTTP | Android/iOS | 格式相近，需适配 |
| ALVR | UDP（协商） | ALVR | 眼追 + 面捕 |
| Virtual Desktop | UDP（协商） | Quest Pro/3 经 VD | 需逆向，注意版本 |
| PICO Connect | UDP（协商） | PICO 4 Pro/Enterprise | 面捕与眼动建议拆成两个原生模块，见 [ADR-0030](adr/0030-native-module-v2-composition.md) |
| OpenXR 眼追 | 进程内 | 任意 OpenXR 应用 | `XR_EXT_eye_gaze_interaction` 只有合并视线 |

**优先级建议**：VMC → OSC 通用 → ARKit LiveLink → OpenXR → ALVR/VD/PICO（涉及逆向，放后）。
前三条已落地，细节见 [ADR-0029](adr/0029-arkit-livelink-and-osc-input.md)。

---

## 9. 输出系统

### 9.1 Sink 抽象

```cpp
class ISink {
public:
    virtual ~ISink() = default;
    virtual SinkId   id() const noexcept = 0;
    virtual bool     start(const SinkConfig&) = 0;
    virtual void     stop() noexcept = 0;
    // 引擎每 tick 调用；sink 自行决定是否真的发送
    virtual void     submit(const FaceFrame&, const GazeState&, uint64_t now_ns) = 0;
    virtual SinkStats stats() const noexcept = 0;   // 发送量/丢弃量/延迟
};
```

每个 Sink 独立节流。核心策略三条：

1. **死区过滤**：值变化 < ε 且距上次发送 < `minInterval` 则不发送。ε 按参数类型分档（眼睛视线 ε=0.002，嘴部 ε=0.01）。
2. **Bundle 聚合**：一次 tick 内所有变化打包成单个 OSC bundle（一个 UDP 包），把 syscall 从 N 次降到 1 次。
3. **心跳重发**：每 1 s 无条件全量重发一次。两种模式各有理由——`native` 模式是防 VRChat 的 10 s 无数据超时（超时后控制权交还内置自动注视）；`parameters` 模式是防参数卡在最后一帧（表情定格同样难看）。

### 9.2 VRChat Sink（重点）

**默认路径 · avatar 参数（表情 + 眼追）**
```
/avatar/parameters/FT/v2/EyeLeftX      float  -1..1    1.0 = 看向右
/avatar/parameters/FT/v2/EyeLeftY      float  -1..1    1.0 = 看向上
/avatar/parameters/FT/v2/EyeRightX     float  -1..1
/avatar/parameters/FT/v2/EyeRightY     float  -1..1
/avatar/parameters/FT/v2/EyeLidLeft    float   0..1    1.0 = 闭眼
/avatar/parameters/FT/v2/EyeLidRight   float   0..1
/avatar/parameters/FT/v2/<ShapeName>   float   0..1
```
这是 VRCFT 的事实标准，也是绝大多数现存模型**唯一**的消费方式。FGB 默认行为必须与之完全一致——包括 v1 legacy 参数的符号约定（v1 的 `EyeLid` 1 = 睁眼，v2 的 `EyeLid` 1 = 闭眼，这是历史上最常见的踩坑点）。

float 拆 bool 的 `BinaryBaseParameter` 编码（`Param1/2/4/8...`）同样需要复刻，供只支持 bool 参数的场景。

**可选路径 · 原生眼追（模式开关，非默认）**
```
/tracking/eye/LeftRightPitchYaw   ,ffff   左pitch 左yaw 右pitch 右yaw（度）
/tracking/eye/EyesClosedAmount    ,f      0..1
```

它的理论优势是延迟略低、不占 avatar 参数配额；但代价是**绕过 avatar 的 Animator 图层**，只对专门做了原生眼追支持的头像有效。大量模型（2023 年前的老模型、只绑了 `FT/v2/*` 的模型）在这条路径下眼睛完全不动。

因此它是显式可选，而不是默认。配置项 `vrchat.eye_output_mode`：

| 值 | 行为 | 适用场景 |
|---|---|---|
| `parameters`（默认） | 眼追写 avatar 参数 | 兼容性最好，与 VRCFT 行为一致 |
| `native` | 只发 `/tracking/eye/*` | 明确支持原生眼追的头像 |
| `both` | 两者都发 | 调试期、过渡期 |

切换入口放在输出页显眼位置，并在 UI 上直说"切到 native 后，未适配原生眼追的模型眼睛会不动"——这条提示能省掉大量用户困惑。

**关键优化 · 参数相关性过滤**

VRChat 的 avatar Animator 参数有数量上限（约 256），且发不存在的参数纯属浪费。做法：

1. 通过 mDNS 找 VRChat 的 OSCQuery 端点（`_oscjson._tcp`，HTTP 端口是动态的，
   不是 9001），再拉 `http://127.0.0.1:<port>/avatar/parameters`；
2. 拿不到 OSCQuery 时，直接解析本地 avatar 配置 JSON
   （`%LocalAppData%Low\VRChat\VRChat\OSC\...\Avatars\*.json`，路径随版本变化，需容错）；
3. 只发送实际存在的参数。

已实现见 [ADR-0023](adr/0023-oscquery-avatar-discovery.md)：后台轮询 `/avatar/change`，
换头像后热重绑，不用手动「重新读取」。

VRCFT 的 `Relevance` 机制就是这个思路，必须复刻——否则 130+ 参数 × 90 Hz = 每秒上万条消息，VRChat 端会明显掉帧。

**自动配置（未实现）**：VRCFT 会改写 VRChat 的 `config.json` 强制打开 OSC。FGB 原计划
做同样的事（改前备份、改后提示、单独还原）。实测本机 `config.json` 里没有 OSC 字段，
OSC 开关在游戏内 `Options > OSC`，写入位置未确认；在没有可靠证据前不动用户配置。

### 9.3 其他 Sink

| Sink | 格式 | 场景 |
|---|---|---|
| VMC | UDP 39539，`/VMC/Ext/Blend/Val`、`/VMC/Ext/Blend/Apply` | Warudo / VSeeFace / Unity |
| 共享内存 | 固定布局 ring，双缓冲 + 版本号 | Unity/UE 插件、OpenXR Layer（微秒级） |
| WebSocket JSON | 每帧一个 JSON（回环，默认 30 Hz） | 网页/脚本/第三方集成；[§07](07-streaming-outputs.md) |
| UDP 二进制 | 656 字节紧凑 struct（小端，一个数据报一帧） | 串流软件（最低延迟）；[§07](07-streaming-outputs.md) |
| OSC 自定义 | 用户配置映射 | 高级用户 |

### 9.4 多目标并发

这是 VRCFT 做不到、FGB 必须做对的事：**同一帧数据同时喂给 VRChat + VMC + 共享内存 + 串流软件**。

难点不在并发（每个 Sink 独立），而在**节流策略冲突**：VRChat 需要 90 Hz 高频，VMC 需要 60 Hz 稳定，共享内存需要每帧全量。因此节流必须 per-sink 配置，不能全局统一。

---

## 10. 校准管线

VRCFT 基本没有校准（依赖硬件出厂校准）。这是廉价硬件和摄像头方案效果差的根因，也是 FGB 最容易做出差异的地方。

### 10.1 三层校准

| 层 | 内容 | 何时执行 |
|---|---|---|
| **中立位** | 记录"放松状态"的基线形状值，后续全部相对它偏移 | 首次运行 / 用户手动重训 |
| **注视偏移** | 解算 yaw/pitch 的系统性偏差（2D 仿射 + 二次项） | 3 点 / 5 点校准 |
| **漂移补偿** | 检测随时间的零点漂移并缓慢修正 | 持续，后台 |

### 10.2 校准流程（用户体验）

```
第 1 步：视觉引导 —— 让人眼依次注视 5 个点（居中 + 四角），每点 1.5 秒
第 2 步：采集 —— 记录每点的 (rawYaw, rawPitch) → (targetYaw, targetPitch)
第 3 步：拟合 —— 最小二乘解 2×3 仿射矩阵 + 二次项系数
第 4 步：验证 —— 随机 3 点复测，残差 > 3° 则提示重做
```

**难点**：用户"看着屏幕上的点"时，眼睛实际注视方向取决于屏幕距离和头部位置，所以必须拿到 HMD 在世界空间的位置，否则校准解出来是错的。这就是为什么 §6.1 的帧结构里要有 `headPose`——**即使头像驱动不需要它**。

### 10.3 眨眼检测

不同来源的眨眼信号差异极大（眼睑 blend 值 / 眼部纵横比 / 红外相机反射）。统一抽象为：

- 有 `openness` 直接给 → 用
- 只有几何（EAR）→ 自适应阈值（滑动窗口中位数 ± k×MAD）
- 都没有 → 从眼睑形状推断

眨眼事件同时输出离散信号（`blink` 上升沿/下降沿），给需要事件而非连续的消费者（如眼动输入、疲劳检测）。

### 10.4 校准工作台与预览通路（规划，ADR-0021）

当前校准是离线档案，界面只能看参数值，看不到"调范围后脸会怎样"。规划中的校准工作台
不改校准算法，只增加两条东西：**可解释的预览数据**和**可编辑的运行时档案**。

```
source → sanitize → PreviewTap(raw) → Calibration → OneEuro → Snapshot → sinks
                         │                                      │
                         └────── 30 Hz preview（页面打开时） ────┘
```

- `PreviewTap` 是独立的 processor，始终挂在校准之前；它把校准前的 88 个原始形状
  和眼球状态写进 seqlock 快照。即使没有校准档案，它也能工作。
- `preview` 是独立的 IPC 消息（30 Hz），只在「校准」页打开时推送；不复用 10 Hz
  的 `params`，避免为了一个页面把热力图的消息频率抬高。
- 校准档案支持运行时更新：控制线程构造新 profile，双缓冲/COW 交换；引擎线程每帧
  读活动版本，热路径不加锁。界面改 lo/hi 立即影响输出，保存才写盘。
- 默认档案放在 `%LOCALAPPDATA%\FaceGazeBridge\calibration.ini`；`--calibration`
  显式指定时优先。这样界面保存的映射重启后仍然生效。
- UI 侧是自研 WebGL2 小渲染器（不引前端框架）：轨道相机、~24 个 morph、
  顶点区域位掩码高亮；WebGL2 不可用时降级为 SVG 示意图。模型与映射表都数据驱动，
  不在 JS 里硬编码形状分支。

**边界**：它是校准的示波器，不是头像预览器。不导入 VRChat 头像、不承诺逐顶点一致、
v1 不做照片级写实。完整取舍见 [ADR-0021](adr/0021-calibration-workbench-3d-preview.md)。

---

## 11. 配置与状态

### 11.1 配置文件布局

```
%AppData%\FaceGazeBridge\
├── config.toml              # 主配置（人类可读，支持手改）
├── profiles/
│   ├── vrchat.toml          # 场景预设
│   └── dev.toml
├── calibration/
│   ├── default.json         # 校准数据（含时间戳，过期提醒重做）
│   └── per-source/<id>.json
├── modules/                 # 原生模块
├── vrcft-libs/              # VRCFT 模块（也可指向 %AppData%\VRCFaceTracking\CustomLibs）
├── logs/                    # 滚动日志，默认保留 7 天
├── captures/                # .fgbcap 录制回放
└── cache/
```

**为什么用 TOML 而不是 JSON**：配置要给人改。JSON 不支持注释、尾逗号，手改体验差。运行时交换（IPC、共享内存）仍用二进制。

> **实现现状（2026-10-29）**：上面是早期的目标布局；实际落地在
> `%LOCALAPPDATA%\FaceGazeBridge`（DLL 不跟漫游配置走）：`modules\<id>\`、
> `modules.ini`、`library.json`、`engine.log`，校准档案是 `calibration.ini`
> （[ADR-0008](adr/0008-calibration-and-filtering.md)）。TOML 主配置尚未落地，
> 新增文件以现有 INI/JSON 风格为准。

### 11.2 不可变配置 / 热重载

配置变更走"构建新 Config 对象 → 原子交换 shared_ptr → 各组件在下一 tick 感知"。禁止原地修改，避免读到半更新的状态。

### 11.3 SteamVR 启动项（规划，ADR-0020）

FGB 不直接改 SteamVR 的配置或注册表。随包提供 `app.vrmanifest`，运行时用官方
OpenVR `IVRApplications` 注册，并用 `SetApplicationAutoLaunch` 控制是否随 SteamVR
启动：

```
UI 设置页（开/关） → WebSocket IPC → 引擎（src/openvr/）
   │  定位 SteamVR：%LOCALAPPDATA%\openvr\openvrpaths.vrpath → runtime
   │  加载 <SteamVR>/bin/win64/openvr_api.dll（不随包分发）
   ▼
VR_Init(VRApplication_Background)   ← 不会反向拉起 SteamVR，也不保活
   ▼
AddApplicationManifest(<exe>/app.vrmanifest, false)
SetApplicationAutoLaunch("com.facegazebridge.desktop", true)
GetApplicationAutoLaunch(...)       ← 读回校验，失败给具体原因
```

OpenVR 调用放在引擎而不是外壳：外壳仍然只负责窗口、托盘与引擎生命周期，开关通过
WebSocket 走控制面；命令行/探针/托盘共用同一套入口（托盘已实现，见
[ADR-0022](adr/0022-tray-shell-and-close-semantics.md)）。

关键约束：

- `SetApplicationAutoLaunch` 只对 dashboard overlay 有效，所以 manifest 里
  `is_dashboard_overlay: true`。FGB **不实现 VR 内面板**，只借用这个自启动机制。
- `VRApplication_Background` 在 SteamVR 未运行时初始化失败——设置开关必须在
  SteamVR 正在运行时操作；界面要把"先启动 SteamVR"说清楚。
- 自启动拉起的是 `FaceGaze Bridge.exe --tray`，维持单入口与 Job 生命周期；
  不直接拉起 `fgb-cli`（控制台黑框、生命周期脱钩）。**托盘这一前置已完成**
  （[ADR-0022](adr/0022-tray-shell-and-close-semantics.md)）：`--tray` 只进托盘、
  不弹窗口；关窗也只是收进托盘。
- 不自动注册、不写注册表、不注册 Steam 库应用。完整状态机与风险见
  [ADR-0020](adr/0020-steamvr-autostart.md)。

---

## 12. 延迟预算

端到端 = 硬件曝光 → VRChat 收到 OSC。

| 阶段 | 目标 | 说明 |
|---|---|---|
| 硬件采集 + 驱动 | 5–15 ms | 不可控，取决于设备 |
| 模块 → 引擎（进程/IPC） | < 1 ms | 共享内存 ≈ 0.05 ms；VRCFT UDP ≈ 0.2 ms |
| 融合 + 解算 + 滤波 | < 0.5 ms | 单线程 120 Hz，预算 8.33 ms，占用 6% |
| 引擎 → 网络 | < 0.3 ms | bundle 编码 + `sendto` |
| 网络 → VRChat 应用 | 1–3 ms | 本机回环 |
| **合计（软件侧）** | **< 2 ms** | |

**注意**：整条 VR 链路的真实延迟瓶颈在串流（VD 无线约 40–60 ms）和硬件曝光，不在本软件。但"软件侧 < 2 ms"是必须守住的，否则叠加后用户能感知到嘴型不同步。

**可测性**：每个阶段打时间戳，`fgb-cli diag latency` 输出 P50/P95/P99 分布。没有测量就没有优化。

---

## 13. 关键技术决策记录（ADR）

| # | 决策 | 备选 | 理由 |
|---|---|---|---|
| 1 | C++20 而非 C++23 | C++17 | 需要 `concepts`/`std::span`/`format` 提升可维护性；C++23 的 MSVC 支持仍不稳 |
| 2 | CMake + vcpkg | MSBuild / Conan | 跨平台、IDE 无关、依赖可复现 |
| 3 | WebView2 UI | Qt / WinUI3 / ImGui | 迭代速度；Win10+ 自带运行时；前端可复用于未来 Web 端 |
| 4 | 自研 OSC 编解码 | oscpack / tinyosc | 热路径，需要零分配与 bundle 复用；OSC 1.0 规范足够简单 |
| 5 | seqlock 发布快照 | mutex / RCU | 读者永不阻塞写者，抖动可控 |
| 6 | 内部右手系 | Unity 左手系 | OpenXR 标准，减少 SDK 适配转换 |
| 7 | 模块多进程 | 全 in-proc | 硬件 SDK 稳定性；VRCFT 已验证 |
| 8 | 兼容层驱动官方 host | 自己写 CLR 宿主 | 省 6+ 人月，且天然跟随上游修复 |
| 9 | TOML 配置 | JSON / YAML | 可注释、可手改、解析器轻 |
| 10 | Apache-2.0 | MIT / GPL | 与生态一致；允许商业闭源集成；专利条款保护 |

---

## 14. 与 VRCFT 的架构差异一览

| 维度 | VRCFT v5 | FGB |
|---|---|---|
| 语言 | C# + 原生 OSC 库 | C++20 核心 + C# 兼容宿主 + TS UI |
| 进程模型 | 主进程 + 模块沙箱 | 主进程 + 多类宿主 + 可选 in-proc |
| 数据模型 | 全局静态单例，float only | 不可变帧 + 时间戳 + 置信度 + 溯源 |
| 输入 | 仅 .NET 模块 | 三层（.NET 模块 / 原生 ABI / 协议直连） |
| 输出 | 仅 OSC → VRChat | 多 Sink 并发（OSC/VMC/共享内存/WS/UDP） |
| 眼追路径 | avatar 参数 | avatar 参数（默认，与 VRCFT 一致）+ 原生通道可切换 |
| 校准 | 依赖硬件 | 三层校准 + 漂移补偿 |
| 视频/姿态 | 无 | 头部位姿、聚散度、扫视/注视事件 |
| 可录制回放 | 无 | `.fgbcap` 格式 |
| 定位 | VRChat 头像驱动工具 | 追踪数据中间件 |

---

## 15. 下一步

架构确认后进入 M0 技术预研，需验证 5 个假设（见 [docs/03-roadmap.md](03-roadmap.md#m0--技术预研)）。任一假设证伪都会改变架构，因此**不要跳过**。
