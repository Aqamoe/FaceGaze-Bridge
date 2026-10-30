# ADR-0030 · 原生模块 v2：注视通道与多模块组合

- 状态：**已实施（v1.1 + v2 阶段 A/B/C.2）**；属性级细节见
  [ADR-0031](0031-native-module-v2-attribute-fusion.md)，跨进程宿主见
  [ADR-0032](0032-native-host-process.md)。
  **后续**：[ADR-0034](0034-native-module-v2-only.md) 删除了 v1 ABI，文中的
  `FgbGazeV1` / `FGB_GAZE_*` / `FgbEyeV1` 现由 `FgbEyeAttributes` + `FGB_ATTR_*` 表达
- 日期：2026-10-30
- 影响：`sdk/fgb_module_api.h`、`sources/native_source`、`engine/source_merger`、
  `engine/engine`（注视服务）、`sinks/*`、`ui/`（模块页 / 输出页）、模块清单
- 关系：接 [ADR-0013](0013-source-merging.md)（按通道合并）、
  [ADR-0015](0015-multi-instance-modules.md)（多实例）、
  [ADR-0018](0018-native-c-abi-v1.md)（v1 ABI）、
  [ADR-0024](0024-streaming-outputs.md)（流式输出）、
  [ADR-0026](0026-gaze-estimator-and-api.md)（注视服务）

## 问题

v1 原生模块只有三个粗粒度能力位：`EXPRESSION / EYE / HEAD`；`EYE` 是**整块原子能力**，
一个模块要么同时提供 gaze、openness、pupil，要么都不提供。由此产生几个问题：

1. **无法表达“只提供注视点”的模块**。PICO 互联的面捕模块和眼动模块是两条独立的数据链，
   只给注视点的模块如果声明 `EYE`，就必须伪造 openness，否则头像会闭眼；如果不声明，
   注视点又进不了引擎。
2. **只能按通道选源，不能按字段组合**。`SourceMerger` 目前把一帧拆成
   `Expression / Eyes / Head` 三路；眼睛内部不拆分，所以“A 给 openness、B 给 gaze”
   做不到，左右眼分别来自不同源也做不到。
3. **注视点没有独立出口**。`GazeState` 是从合并后的眼睛反算的合并视线，只有
   pitch/yaw；没有逐眼方向、时间戳、来源，也没有“只给渲染、不驱动 avatar”的通道。
   现有 SHM/UDP/WS 输出也无法区分“头像眼睛用的视线”和“foveation 用的视线”。
4. **时间与新鲜度不够细**。一个 `capture_ts_ns` 代表整帧，合并层只有一个 500 ms
   `stale_ns`；面捕 30 Hz 可以接受，注视点 500 ms 已经完全不能用于 foveation。
5. **多模块只到 CLI**。`--source vrcft --module-id A --source vrcft --module-id B`
   可以跑两个实例，但界面仍是单选，角色分配、启动配置、故障隔离都没有产品化。
6. **v1 进程内加载**。PICO 内部 RPC、厂商 SDK 这类更新频繁、稳定性不可控的模块
   一旦崩溃会带走引擎；模块更新也只能重启引擎。

PICO 场景是典型例子：用户希望用模块 A 的面捕数据 + 模块 B 的眼动数据组成头像输入，
同时模块 B 的原生注视只用于 foveation；或者模块 A 出完整面捕，模块 B 只出注视点。
这需要模块能力、合并策略和输出路径三者解耦。

## 决定

### 1. 模块类型由能力位组合表达，不引入硬编码枚举

保留 `FGB_CAP_EXPRESSION / FGB_CAP_EYE / FGB_CAP_HEAD`，新增：

```c
#define FGB_CAP_GAZE 0x8u /* 独立注视：方向/逐眼/可选 2D 点，不包含 openness/pupil */
```

模块类型只是能力的组合，宿主和界面按能力识别：

| 类型 | 能力位 | 说明 |
|---|---|---|
| 面捕模块 | `EXPRESSION` | 88 个形状，无眼动 |
| 眼动模块 | `EYE` | gaze + openness + pupil（整眼） |
| 注视模块 | `GAZE` | 只提供注视方向/点，不碰 openness |
| 完整模块 | `EXPRESSION \| EYE \| GAZE \| HEAD` | 头显 APK 或完整厂商 SDK |
| 头姿模块 | `HEAD` | 只有头姿 |

`EYE` 的语义保持不变：声明它就必须同时提供有效的 gaze 与 openness。只给注视点的模块
必须声明 `GAZE`，不能借用 `EYE`。

### 2. 帧槽末尾追加 `FgbGazeV1`，v1 模块不受影响

在 `FgbFrameSlotV1` 末尾追加（模块必须先检查 `slot->struct_size` 再写新字段）：

```c
typedef struct FgbGazeV1 {
    uint32_t valid;       /* bit0 左眼, bit1 右眼, bit2 合并, bit3 2D 点有效 */
    uint32_t reserved;
    float left_dir[3];    /* HMD 局部，单位向量，-Z 前 */
    float right_dir[3];
    float combined_dir[3];
    float left_point_2d[2];   /* 可选，归一化屏幕点；坐标约定见开放问题 */
    float right_point_2d[2];
    float confidence;
    uint64_t capture_ts_ns;   /* 0 = 用帧的 capture_ts_ns */
} FgbGazeV1;
```

`FgbEyeV1` 保持不变；`EYE` 与 `GAZE` 可以同时出现，也可以只出现一个。旧模块不写
`gaze` 字段，新宿主按“没有 GAZE 通道”处理；新模块在旧宿主上若只声明 `GAZE`，旧宿主
会因“没有已知能力位”拒绝加载，不会误读。

### 3. 合并层增加第四个通道 `Gaze`

```text
Expression  88 形状
Eyes        openness / pupil / vergence（整眼）
Gaze        注视方向 / 逐眼 / 可选 2D 点
Head        头姿
```

- `MergePolicy` 增加 `gaze_source`；CLI 增加 `--gaze-source`，界面增加“渲染注视来源”。
- 每个通道独立选源：显式指定 → 注册顺序中第一个“新鲜且提供该通道”的源 → 清空。
- 各通道独立新鲜度：面捕 500 ms、眼睛 250 ms、**注视 100 ms**（可配置）。
- `FaceFrame` 增加 `GazePair`/`gaze` 字段与 `channel::kGaze` 位；合并结果记录每个通道的
  来源与捕获时间，供状态页显示“面捕 A（20 ms）、注视 B（8 ms）”。

### 4. 头像输出与渲染输出分路

- **Avatar**：`VrchatSink` 默认继续用 `eyes` 通道；`EYE` 仍要求 openness + gaze。
- **Render gaze**：新增/扩展输出使用 `gaze` 通道（优先），没有时回退到 `eyes` 反算。
  `GazeState` 扩展为逐眼方向、可选 2D 点、`capture_ts_ns`、来源、置信度。
- SHM/UDP/WS 输出增加版本化字段；旧字段保留。后续可加 `avatar_eye_from_gaze` 开关，
  但**默认不把渲染注视混进头像**，避免“面捕 A + 注视 B”时 avatar 眼睛被 B 意外接管。
- Foveation 消费端仍然自己套 HMD 的 per-eye transform/projection；FGB 只保证
  头显局部方向、时间戳、来源和有效性。

### 5. 多模块产品化

- `modules.ini` 从“启动时加载一个”扩展为按实例列表：`实例名 → module_id / role`。
- 界面模块页显示能力徽章（面捕/眼睛/注视/头姿），允许为每个角色选择来源；
  缺少某个角色时明确提示“头像会用中性值/渲染注视不可用”，不静默回退。
- 实例名、角色配置都用 `module_id` 稳定标识，不依赖 `native[1]` 这类顺序名。
- CLI 保留现有重复 `--source` 写法，新增 `--gaze-source`；后续再收敛为显式绑定语法。

### 6. v2（按需）才做属性级组合与跨进程宿主

v1.1 的“整通道组合”已覆盖：

- A 面捕 + B 整眼 → 头像 = A 形状 + B 眼睛；
- A 完整 + B 注视 → 头像 = A，foveation = B；
- A 面捕 + B 整眼 + C 注视 → 三路各取所需。

以下场景才需要 v2：

- A 的 openness + B 的 gaze 拼成头像眼睛；
- 左眼来自 A、右眼来自 B；
- 逐字段时间戳/置信度融合；
- 模块崩溃隔离与热更新。

v2 设计方向：`FgbFrameSlotV2` 携带属性掩码、逐组时间戳/来源，合并策略升级为
“属性 → 有序源列表”；同时引入 `fgb-host-native.exe`（每模块一个进程 + 共享内存
ring），脆弱/第三方模块默认走跨进程，稳定可信模块可选 in-proc 低延迟模式。
在此之前，PICO 内部 RPC 这类模块应作为实验模块，不承诺崩溃隔离。

### 7. PICO 模块拆分建议

| 模块 | 能力 | 数据来源 | 更新节奏 |
|---|---|---|---|
| `pico-connect-face` | `EXPRESSION` | PICO 互联面捕协议 | 跟随面捕格式 |
| `pico-connect-gaze` | `GAZE`（可选 `EYE` 的 openness/pupil） | PICO 互联眼动（按可用接口适配） | 跟随 PICO 互联版本 |
| `pico-full`（可选） | `EXPRESSION \| EYE \| GAZE \| HEAD` | 头显 APK（PicoFacialBridge 类） | 跟随 APK |

用户按角色组合：面捕用 face 模块，渲染注视用 gaze 模块；不再要求一个模块包打全部。
PICO 互联内部接口属于未公开/易变接口，模块清单应记录适配的 PICO 互联版本，
并在版本不匹配时拒绝启用或降级，而不是静默无数据。

### 8. 模块清单增加可选字段

`fgb-native.json` 在现有字段上追加（旧清单仍有效）：

```json
{
  "AbiVersion": 1,
  "Capabilities": ["expression", "gaze"],
  "RoleHint": "gaze",
  "MinHostVersion": "1.0.2"
}
```

宿主加载前用于列表/兼容性提示，加载后再以 `fgb_module_query` 的运行时声明为准；
两者不一致时以运行时为准并告警。模块仓不要求重写旧清单。

## 组合示例

```text
A = pico-connect-face  (EXPRESSION)
B = pico-connect-gaze  (GAZE)

roles:
  expression = A
  eyes       = A        # A 的 EyeLook/EyeBlink 只作为头像代理
  gaze       = B        # 只看方向，不碰 openness
  head       = A
  render     = B
```

```text
A = pico-full          (EXPRESSION | EYE | GAZE | HEAD)
B = pico-connect-gaze  (GAZE)

avatar  = A（完整面捕）
render  = B（更准的原生注视）
```

## 代价与边界

- v1.1 仍是“整通道替换”，不做加权融合；gaze 与 eyes 可能来自不同时刻，状态页必须
  显示各自年龄，渲染端要按时间戳拒绝过期数据。
- `FgbEyeV1` 的整眼原子性保留；只给 gaze 的模块不要声明 `EYE`，否则头像会因
  openness 缺失而闭眼。这个约束要写进模块指南。
- 多模块增加配置、UI 和故障面；默认仍应能“单模块开箱即用”。
- v2 属性级组合和跨进程宿主成本高，必须等真实硬件/真实组合需求，不提前做。
- PICO 内部 RPC/注入类模块的合规性、版本脆弱性不因 ABI 设计而消失；模块化只是把
  影响限制在一个可替换的 DLL/进程里。

## 验证计划

- 单测：能力位组合、`GAZE` 通道选源、100 ms 独立过期、缺失 gaze 清空、
  逐通道来源/时间戳、旧 v1 模块兼容、旧宿主拒绝 gaze-only 模块。
- 示例：把 `native-sine` 拆成 `native-face`（EXPRESSION）与 `native-gaze`（GAZE），
  两个同时运行；验证头像用 face、渲染用 gaze，互换角色后结果互换。
- 界面探针：模块能力徽章、角色选择、缺角色提示、状态页各通道年龄。
- 硬件：PICO 面捕模块 + 注视模块同时运行；测量 gaze 端到端延迟、中心/边缘误差，
  并验证 foveation 消费端在 gaze 过期时立即停止。

## 实施结果（v1.1，2026-10-30）

已落地 §1–§5、§8 的整通道组合；§6 的 v2 不在本次范围。

| 层 | 实现 |
|---|---|
| ABI | `FGB_CAP_GAZE`、`FgbGazeV1` 追加到 `FgbFrameSlotV1` 末尾；写 gaze 前必须检查 `struct_size`。`EYE` 语义不变。 |
| 合并 | `FaceFrame` 增加 `GazePair` 与 `channel::kGaze`；`SourceMerger` 四通道独立选源，过期阈值默认 500/250/100/500 ms，可逐通道配置。 |
| 渲染 | `GazeState` 扩展逐眼方向、2D 点、`capture_ts_ns`、来源；渲染注视优先 `gaze`，缺失时回退 `eyes`。VRChat 头像仍只读 `eyes`。 |
| 输出 | SHM/UDP 升到 v2，在末尾追加注视字段；WS JSON 增加顶层 `gaze` 对象。旧字段保留，旧消费者按 `version`/`struct_size` 判断。 |
| 配置 | `modules.ini` 增加 `[instances]`（实例名 → module_id）与 `[roles]`（角色 → module_id/源名）；CLI 增加 `--gaze-source` 与逐通道 `--*-stale-ms`。 |
| 清单 | `fgb-native.json` 支持可选 `Capabilities` / `RoleHint` / `MinHostVersion`；运行时能力以 `fgb_module_query` 为准。 |
| 界面 | 模块页显示能力徽章、启动实例列表与四角色来源选择；状态页显示第四通道与注视来源/年龄。 |
| 样例 | `native-face`（仅 EXPRESSION）与 `native-gaze`（仅 GAZE）可同时运行；`native-sine` 保留为旧模块兼容基准。 |

v1.1 已覆盖 §6 列出的前三个目标场景（A 面捕 + B 整眼、A 完整 + B 注视、
A 面捕 + B 整眼 + C 注视）。属性级组合（A 的 openness + B 的 gaze、左右眼分源）
与跨进程宿主仍按 §6 暂缓：需要 `FgbFrameSlotV2` 的属性掩码、逐组时间戳/来源和
`fgb-host-native.exe`，在拿到真实硬件组合需求前不提前做。

验证：182 个单测全绿（能力组合、GAZE 选源、100 ms 独立过期、逐通道时间戳、
旧 v1 模块兼容、线格式、清单字段、实例/角色持久化）；`native-face` + `native-gaze`
同时运行，头像眼睛未被注视源接管；SHM/UDP/WS 探针与 WebView2 UI 探针通过。

## 开放问题

- ~~2D 注视点的坐标约定（归一化范围、原点、是否按眼、是否含镜头畸变）~~
  **已定（2026-10-31）**：见 [ADR-0033](0033-gaze-point-2d-space.md)——
  该眼显示视口归一化、`[0,1]`、左上原点、畸变前、逐眼独立、越界保留、NaN 否掉，
  映射责任在模块；不引入"空间标签"字段。
- 是否在 ABI 中直接带 per-eye origin/收敛距离；这决定 foveation 近场精度。
- 每源校准档案的存储键（module_id + 硬件序列）与校准时机。
- 跨进程宿主的延迟预算（共享内存 ring + 唤醒）与热更新策略。
- PICO 互联内部接口的版本探测、许可证与分发边界。
