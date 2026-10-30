# ADR-0029 · ARKit LiveLink 与通用 OSC 输入

- 状态：**已实施**（2026-10-30）
- 日期：2026-10-30
- 影响：`src/sources/livelink_source.*`、`src/sources/osc_source.*`、
  `src/sources/osc_target.*`、`src/config/osc_map.*`、`src/osc/decoder.*`、
  `src/core/model/math.hpp`、`src/cli/main.cpp`、`tests/test_livelink.cpp`、
  `tests/test_osc_input.cpp`、`tools/probe_inputs.py`
- 关系：roadmap P0 输入协议（VMC 之外的另外两条）；M2/M3 的输入侧补齐

## 问题

`02-features.md` 把「VMC / 通用 OSC / ARKit LiveLink」都列为 P0，但此前只有 VMC：

- iPhone 用户只能安装 VRCFT 的 LiveLink 模块，多一层 .NET 运行时；
- 任何发 OSC 的面捕 App / 自制脚本都无法接入，除非它碰巧说 VMC；
- 计划里的"地址 → 形状映射表"没有落点。

## 决定

### 1. LiveLink 走 UE 的二进制协议，不是 ARKit-OSC

Live Link Face（iOS）默认发给 UE 的是 **binary LiveLink**，端口 11111：

```
u8 version | i32+str device_id | i32+str subject | frame_time(16) | u8 count | f32[count]
```

`count` 通常为 61：0..51 = Apple ARKit 52 blendshape（官方顺序），52..54 = 头姿
Yaw/Pitch/Roll，55..60 = 左右眼 Yaw/Pitch/Roll。没有头部位移。

- 解析器容忍前面多 4 字节大端长度（回放/转发工具会加）；
- 只带 52 条曲线时不算错，只是没有头姿；
- 所有字段大端，字符串是 UTF-8 字节数（不是 UE 内存里的 TCHAR 宽度）。

**为什么不做 ARKit-OSC 变体**：那不是一个有规范的名字；需要的人可以直接用通用
OSC 输入映射 `/ARKit/*`。把两条语义不同的协议揉进一个源，排障时无法判断是哪层错。

### 2. 眼睛用 `eyeLook*`，不用曲线 55..60

眼姿从 52 个 blendshape 里的 `eyeLookIn/Out/Up/Down` 推导：

- 左眼 X = in − out，右眼 X = out − in（+X = 佩戴者右侧）；
- Y = up − down（+Y = 上）；
- 归一化后按 **±30°** 映射到 `EyeState`。

曲线 55..60 的单位/符号没有一手文档（PyLiveLinkFace 只记录为 -1..1，
`blender_livelinkface` 还报告横屏会翻转）。`eyeLook*` 的语义在 Apple 文档里
是明确的，先选可解释的那条。

### 3. 头姿单位与符号是**推导值**，集中成一个常量

- 归一化 -1..1 → **±90°**（`kLiveLinkHeadMaxDeg`）；
- UE 左手（X 前 / Y 右 / Z 上）→ OpenXR 右手（-Z 前）：pitch 保持，yaw/roll 取反；
- 三个角全为 0 时不声明 head 通道，避免盖掉真正的头姿源。

**未在 iPhone 上验收。** 如果实机方向相反或幅度不对，只改
`kLiveLinkHeadMaxDeg` 与 `quat_from_euler_deg` 的入参符号，不动解析布局。
这一条必须在下一次真机回归时补上（见"后果"）。

### 4. 通用 OSC：独立映射文件，一行一条

端口默认 **9103**（避开 VRChat 的 9000/9001 与项目自己的 9101/9102），默认只绑
回环；`--osc-in-lan` 才绑所有网卡。映射表：

```ini
[input]
port=9103

[map]
/avatar/parameters/JawOpen = JawOpen
/avatar/parameters/EyeLidLeft = EyeLidLeft, invert
/eye/x = EyeLeftX, scale=0.8, offset=0.1
```

- 地址是 ini 的键，天然去重（重复地址后者覆盖）；
- 值语法 `Target[, scale=..][, offset=..][, invert]`；
- 目标 = 88 个统一形状名（大小写不敏感）或 `EyeLidLeft/Right`、`EyeLeftX/Y`、
  `EyeRightX/Y`；
- 同一地址每次到达覆盖当前值；不同地址映射到同一形状时相加；
- 没有 Apply 包：一批消息到达就发布一帧，值保持到发送端停下（合并层按 500 ms
  判定过期）；
- 首次 `--source osc` 时文件不存在会生成空表并提示，不静默什么都不做。

**为什么用 ini 而不是 JSON**：项目已有 ini 读写与手改容错，映射表是"标量→标量"，
JSON 只会让用户多踩逗号/转义。**为什么不塞进 outputs.ini**：输入和输出的生命周期
不同，改映射不该连带重写输出目标。

### 5. 顺手修正视线方向的符号

`dir_to_pitch_yaw_deg` / `pitch_yaw_deg_to_dir` 原来把 yaw 正方向算成了"向左"，
与 `math.hpp` 注释、`08-gaze-service.md` 和 v2 `EyeX` 的约定（正值 = 右）相反。
新输入要产出正确的共享内存/UDP 视线方向，因此把这对互逆函数改成与文档一致。
旧的往返测试不受影响（两处同改）。

### 6. CLI

```
--source osc|livelink
--osc-map <file>       默认 %LOCALAPPDATA%\FaceGazeBridge\osc_map.ini
--osc-in-port <port>   覆盖文件里的端口
--osc-in-lan           OSC 绑所有网卡（LiveLink 必须绑所有网卡，无开关）
--livelink-port <port> 默认 11111
```

`--source` 现在对未知类型报错，不再默默退回 VMC。

## 后果

- **LiveLink 未在真机验收**：手头没有 iPhone。解析布局用 LLV 项目里一段真实
  录制（320 字节/61 曲线）核对过，但单位/符号是推导值；README 与用户手册已标注。
- LiveLink 必须绑定所有网卡：Windows 防火墙会弹窗。文档要求用户只放行专用网络。
- 通用 OSC 的"表格编辑器"还没做：目前是文件 + CLI。界面表格需要输入页/重启链路，
  作为后续切片；文件格式已经按界面表格的字段设计（地址/目标/scale/offset/invert），
  接界面时不用改协议。
- 多个 `--source osc` 会抢同一个端口，第二个启动失败——与 VMC 的既有行为一致。
- 映射目标写错不会崩：启动时逐条警告并跳过，状态页会显示该源没有对应通道。

## 验证

- `tests/test_livelink.cpp`：61/52 曲线、长度前缀、blink/openness、look* 方向、
  头姿四元数、坏包拒绝、真实 UDP。
- `tests/test_osc_input.cpp`：映射语法、目标解析、scale/offset/invert、保存/加载、
  真实 UDP 收包。
- `tools/probe_inputs.py`：自己拉起引擎，两路各发真实数据报，断言
  `/api/params` 与 `/api/gaze`；Release/Debug 均通过。
