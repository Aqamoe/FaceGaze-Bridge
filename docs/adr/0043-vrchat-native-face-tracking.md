# ADR-0043 · VRChat 原生面捕走独立通道，默认按头像自动切换

- 状态：已采纳
- 日期：2026-11-02
- 影响：`src/sinks/vrchat_face.*`（新增）、`src/sinks/vrchat_sink.*`、`src/osc/encoder.*`、
  `src/params/avatar_config.hpp`、`src/params/oscquery.cpp`、`src/config/outputs.*`、
  `src/cli/main.cpp`、`src/web/ipc.*`、`ui/`
- 关系：**补充** [ADR-0002](0002-eye-output-defaults-to-parameters.md)（眼追的双通道）；
  依赖 [ADR-0023](0023-oscquery-avatar-discovery.md) 的 OSCQuery 端点；配置项沿用
  [ADR-0025](0025-output-settings-persistence.md) 的 `outputs.ini`

## 背景

VRChat 从 Avatars SDK `3.10.6` + 客户端 `2026.4.1`（Live，2026-10-28）开始，把面捕做进了
`VRC Avatar Descriptor`：头像作者勾 **Use All Tracking Keys**、把 Unified Expressions 的
tracking key 映射到 blendshape，VRChat 自己求值，**不再依赖 avatar 的 Animator 参数**。

外部程序的数据入口也换了：`/tracking/face/v1`，单个 OSC blob，负载固定 102 字节，
每个 key 一字节（`byte/255.0`）；0–13 是眼（视线/睁闭/瞳孔），14–101 是 88 个表情形状。
它不占 synced parameter 预算，还会随 NetIK 那套有损同步发给远端。

## 问题

FGB 现有的表情输出只有一条路：**avatar 参数**（`FT/v2/*`、二进制位域、v1 legacy）。
这在 2026-11-02 之后出现了一个硬缺口：

1. 原生面捕头像**不会**声明 `FT/v2/*` 参数（不再需要），于是 `ParamRouter` 匹配到 0 条，
   状态页显示"命中 0"，用户看到的是"脸完全不动"。
2. 反过来，老模型没有原生面捕系统，`/tracking/face/v1` 会被整条忽略（长度错也一样）。
   两条通道不能靠"都发一遍"蒙混：在原生头像上发参数打在空地址，在原生头像上同时用
   Animator 驱动同一批 blendshape 是双驱动。

VRCFT 的做法可以当参照：它新增了 `BinaryFaceDataSender`，用 OSCQuery 的
`/avatar/avatarfullfacetracking` 当开关——只有头像开了原生面捕才发 blob，否则继续发参数。

## 决定

### 1. 第三条输出通道，独立开关

`VrchatSinkConfig::face_mode`（`outputs.ini` 的 `output.face_mode`，CLI `--vrchat-face`）：

| 值 | 行为 | 默认 | 适用 |
|---|---|---|---|
| `auto` | 头像启用原生面捕 → 发 blob；否则 → avatar 参数 | ✅ | 几乎所有用户 |
| `native` | 强制发 blob，不发参数 | | 排查、头像标记拿不到时手动兜底 |
| `parameters` | 强制 avatar 参数，不发 blob | | 老模型、回归对比 |

它和 `eye_mode`（`/tracking/eye/*`）是两个正交的开关：前者管**表情**，后者管**眼追端点**。

### 2. `auto` 的唯一判据是 OSCQuery 的 `/avatar/avatarfullfacetracking`

- 节点存在且为 `true` → 原生；`false` → 参数；**节点不存在（老客户端）→ 参数**。
- 本地头像 JSON 里没有这个字段，所以 `AvatarConfig::full_face_tracking_known`
  把"明确没开"和"不知道"分开；"不知道"一律按老模型处理。
  宁可新模型少一个功能，也不能让老模型"看起来坏了"。
- 拿不到参数表（回退固定前缀）时同样按老模型处理。

### 3. 负载布局是硬契约，与形状枚举同序

```
0–7    EyeLookOut/In/Up/Down × Right/Left
8–9    EyeClosedRight / Left（= 1 - openness）
10–13  EyeDilation / EyeConstrict × Right/Left（瞳孔，5 mm 为中性）
14–101 88 个基础形状，逐项等于 core/model/shape.hpp 的顺序
```

`static_assert(kNativeFaceEyeBytes + kShapeCount == kNativeFaceBytes)` 把"形状枚举
加减一项"挡在编译期。这里**不需要**名字表，也不接受 Blended shapes（`MouthSmile`、
`EyeClosed` 这类 42 个合成名不是原生输入）。

### 4. 通道互斥，但眼追端点照发

- 原生通道生效时**不发** avatar 参数（含 v2/v1 与眼追参数）。
- `/tracking/eye/*` 仍然照发：原生面捕头像通常也没绑定眼追参数，而 `Eye Gaze Mode:
  Bone Rotation` 的眼球旋转只能靠这两个端点。这与 VRCFT 的行为一致。

### 5. 每帧整包，内容不变则省一包，心跳强制重发

VRChat 要求完整 102 字节，不能只发改动位。FGB 按现有节流（默认 ~90 Hz）整包发送，
负载与上一包逐字节相同且不是心跳时跳过——`SinkThrottle` 的心跳是 1 Hz，所以最长
1 s 一定会重发一次，避免"追踪源停了，头像冻在最后一帧"。

### 6. 量化口径

`byte = round(clamp01(v) * 255)`，8 bit 步进约 0.39%。不做逐字节死区（去重按整包比较），
理由是否则会出现"改动小于阈值就永远不发"，而 VRChat 对漏帧没有补偿机制。

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 默认 `native` | 老模型（2022–2025 的绝大多数资产）会彻底不动，和 ADR-0002 同一个坑 |
| 无脑两条都发 | 原生头像上参数打在空地址；同时驱动 Animator 与原生系统会互相盖，难排查 |
| 从本地头像 JSON 推断 | 该文件没有 `avatarfullfacetracking`；猜错的代价是"脸不动" |
| 用 Blended shapes 当负载 | 原生只吃 102 个 base key，合成名不在协议里 |
| 只在 UI 暴露、不给 CLI/INI | 探针、回放、回归都靠命令行；配置项必须可持久化 |

## 后果

**正面**
- 2026-11-02 之后上传的原生面捕头像零配置可用，且不占同步参数预算
- 老模型行为完全不变（默认 `auto` 在拿不到标记时选参数）
- 一条消息承载整张脸，90 Hz 下带宽约 11 KB/s，比几十条参数消息更省
- 顺带补齐了 OSC 编码器的 blob 支持（`add_blob` / `get_blob`），后续回放 `/tracking/face/v1`
  或做抓包对比都不必再改底层

**负面 / 风险**
- 量化从 float 变成 8 bit，极端精细的微表情会有台阶；实测待真机确认
- `auto` 的正确性依赖 OSCQuery；纯本地 JSON 路径下只能当老模型处理（可在 UI 看到"未确认"）
- 原生面捕的"追踪源停止"语义没有官方文档，这里是按心跳 + 持续中性值处理
- 远端同步是有损的（类似 NetIK），本机看到的和远端看到的可能不一致，属预期

**已实现（2026-11-02）**：`vrchat_face.*` 负载构造 + 单测（中性帧全 0、索引 0–13、
形状偏移、直径换算瞳孔）、`VrchatSink` 的模式与互斥、OSCQuery 标记解析、CLI/INI/IPC/UI、
端到端用例（回环收包验证 blob 长度与 JawOpen 字节）。

**待验证**：真机 + 原生面捕头像的整链路口径（尤其眼字节的正负方向与 `Eye Gaze Mode`
各模式的叠加效果），需要一台装了 2026.4.1+ 客户端的机器。

## 相关

- VRChat 文档：[Face Tracking](https://creators.vrchat.com/avatars/face-tracking/)、
  [OSC Face Tracking](https://creators.vrchat.com/avatars/face-tracking/osc)
- [ADR-0002](0002-eye-output-defaults-to-parameters.md)、[ADR-0023](0023-oscquery-avatar-discovery.md)、
  [ADR-0025](0025-output-settings-persistence.md)
- 架构 §9.2 VRChat Sink；功能规格 §2.5
