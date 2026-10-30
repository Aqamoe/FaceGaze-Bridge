# ADR-0031 · 原生模块 v2：属性级眼睛合成

- 状态：**已实施（阶段 A/B/B.2）**（2026-10-30）。**后续**：
  [ADR-0034](0034-native-module-v2-only.md) 移除了 v1 ABI，公开类型去掉 `V2` 后缀
  （`FgbFrameSlotV2` → `FgbFrameSlot`），`FGB_CAP_EYE_ATTRIBUTES` 并入 `FGB_CAP_EYES`
- 日期：2026-10-30
- 影响：`src/core/model/frame.hpp`、`src/engine/source_merger.*`、`src/engine/engine.cpp`、
  `src/cli/main.cpp`、`src/web/ipc.cpp`、后续 `sdk/fgb_module_api.h`
- 关系：接 [ADR-0030](0030-native-module-v2-composition.md) §6；v1.1 的整通道组合保持不变

## 问题

v1.1 把一帧拆成 `Expression / Eyes / Gaze / Head` 四个整通道：

- “A 的 openness + B 的 gaze”做不到——头像眼睛只能整块来自一个 `EYE` 源；
- 左眼来自 A、右眼来自 B 做不到——`Eyes` 通道不拆左右；
- 逐字段时间戳/置信度没有进入合并结果，状态页只能看到“通道级”来源。

这不影响 v1.1 的目标场景（A 面捕 + B 整眼、A 完整 + B 注视），但确实限制了
“B 只出注视、A 只出睁眼度”和“左右眼分别来自两个模块”的组合。

## 分阶段决定

V2 不一次性重写 ABI 和宿主，拆成三个阶段，每个阶段都能单独回滚：

### 阶段 A · 合并模型属性化（本 ADR 实施）

不新增 ABI，先把内部模型和合并策略升级：

- `EyeState` 增加逐字段来源与捕获时间：`opennessSource/opennessTsNs`、
  `pupilSource/pupilTsNs`、`gazeSource/gazeTsNs`；
- `GazeSample` 增加 `source`、`captureTsNs`、`confidence`、`pointValid`；
- `EyePair` 增加 `vergenceSource/vergenceTsNs`；
- `MergePolicy` 增加：
  - `eye_left_source / eye_right_source`（左右眼可来自不同 `EYE` 源）；
  - `gaze_left_source / gaze_right_source`（左右注视可来自不同 `GAZE` 源）；
  - `avatar_eye_from_gaze`（默认 false）：头像眼睛保留 `EYE` 源的 openness/pupil，
    方向改为使用融合后的 `Gaze` 通道。
- 合并结果逐眼记录来源与时间戳；`Eyes` 通道的整通道时间戳在左右源不同时取最大值，
  来源记为未知（由逐眼字段给出真实来源）。
- 默认策略与 v1.1 完全一致：不配置新字段时，左右眼来自同一个 `EYE` 源，
  头像眼睛不会被 `Gaze` 通道改写。

阶段 A 能表达：

| 需求 | 做法 |
|---|---|
| A 的 openness + B 的 gaze（头像眼睛） | `eye_source=A`、`gaze_source=B`、`avatar_eye_from_gaze=true` |
| 左眼 A、右眼 B | `eye_left_source=A`、`eye_right_source=B` |
| 左/右渲染注视分源 | `gaze_left_source=A`、`gaze_right_source=B` |
| v1.1 行为 | 不设新参数（默认） |

阶段 A 仍受 v1 ABI 限制：模块要提供 openness 必须声明 `EYE`（同时给 gaze）。
“只给 openness、不给 gaze”的模块要等阶段 B 的属性掩码。

### 阶段 B · ABI v2 属性掩码

新增 `FgbFrameSlotV2`（或 v1 末尾追加属性块）与按属性分组的有效性/时间戳：

```text
EXPRESSION / LEFT_OPENNESS / RIGHT_OPENNESS / LEFT_PUPIL / RIGHT_PUPIL
LEFT_GAZE / RIGHT_GAZE / COMBINED_GAZE / LEFT_POINT / RIGHT_POINT
VERGENCE / HEAD
```

每个属性组携带自己的 `capture_ts_ns` 与置信度；宿主按属性位读取，不再要求
`EYE` 整块原子。v1 模块继续走现有路径，v2 模块通过 `fgb_module_query(2)` 选择。
合并策略从“通道 → 一个源”升级为“属性 → 有序源列表”。

### 阶段 C · 跨进程宿主

引入 `fgb-host-native.exe`：每个模块一个进程，宿主与引擎之间用共享内存 ring
交换 v2 帧。目标是模块崩溃不带走引擎、模块可热更新。跨进程只承载 v2 帧；
稳定可信模块可继续选择 in-proc 低延迟模式。

阶段 C 的延迟预算、ring 协议、崩溃回收与热更新策略在实施前单独写 ADR。

## 阶段 A 的兼容性

- `MergePolicy` 新字段默认空/false；不配置时输出与 v1.1 逐字节一致（除新增的
  诊断字段）。
- `Frame.eyes` 仍是 `VrchatSink` 的唯一来源；`avatar_eye_from_gaze` 只在显式开启时
  改写方向，openness/pupil 始终来自 `EYE` 源。
- 属性级来源/时间戳目前只进入内部模型和状态报告，不改变 SHM/UDP/WS 的 v2 线上字段；
  线上值仍是合成后的头像眼睛/渲染注视。
- 如果左右眼来自不同源，`Eyes` 通道的整通道 `ChannelStamp.source` 记为未知；
  消费端需要逐眼来源时应读取新增的诊断字段。

## 代价与边界

- 阶段 A 只解决“整眼源之间的属性拼装”，不能把“只给 openness”的模块拼进来；
  后者必须等阶段 B。
- 左右眼分源会让两眼的 openness/置信度不同步；状态页必须按眼显示来源与年龄，
  不能只显示一个“眼追来源”。
- 逐字段时间戳增加了 `FaceFrame` 体积和合并分支；仍保持每 tick 零分配。
- 属性级组合让配置面变大；默认仍必须“单模块开箱即用”，高级参数只放在 CLI
  与高级设置，不塞进首屏。

## 验证计划（阶段 A）

- 单测：默认策略与 v1.1 等价；A openness + B gaze 合成；左眼 A/右眼 B；
  左右注视分源；单侧 stale 不清掉另一侧；逐眼来源/时间戳正确。
- 端到端：`native-sine`（EYE/EXPRESSION/HEAD）+ `native-gaze`（GAZE），
  开启 `avatar_eye_from_gaze` 后头像眼睛方向来自 gaze、openness 仍来自 sine。
- 状态/探针：`/api/status` 的 sources 增加逐眼来源；UI 输入源仍按通道显示，
  不因高级配置改变首屏布局。

## 阶段 B 实施记录（2026-10-30）

`FgbModuleV2` / `fgb_module_query_v2` / `FgbFrameSlotV2` 已落地；属性位覆盖
expression、左右 openness/pupil/gaze、combined gaze、左右 2D 点、vergence、head。
宿主优先加载 v2，失败回退 v1；v2 模块用 `FGB_CAP_EYE_ATTRIBUTES` 表示按属性提供
眼睛。`EyeState.gazeValid` 区分“真的给了方向”和“默认正前方”，所以只给 openness
的模块不会产生渲染注视。

新增样例 `native-openness`（只给 openness/pupil，无 gaze）；单测覆盖 v2 加载、
属性到 `FaceFrame` 的转换、与 `native-gaze` 组合成“A openness + B gaze”的头像眼睛，
以及 v1 模块继续走原路径。阶段 B **仍是进程内加载**；跨进程宿主留待阶段 C。

B.2 已实现：`MergePolicy::attribute_sources` 与 CLI `--attr-source
left-openness=A,B` 支持“属性 → 有序源列表”；`EyeState` 增加
`opennessValid/pupilValid`，同一只眼的 openness、pupil、gaze 可来自不同源，
未列出的属性回退到整眼/整通道策略。列表内默认取第一个新鲜且确实提供该属性的源，
不存在的属性清空，不会偷偷回退到列表外。

`--attr-fuse` 开启多源融合：openness/gaze 按置信度加权平均，pupil 等权平均，
合成字段的来源记为未知、时间戳取最新；vergence 没有置信度，仍取第一个。
这个模式默认关闭，避免正常配置下把两个源“平均”成不直观的值。

## 阶段 A 实施记录（2026-10-30）

已落地：逐字段 `EyeState` 来源/时间戳、`GazeSample` 逐眼来源/置信度/2D 点有效位、
`EyePair.vergenceSource`；`MergePolicy` 增加左右眼/左右注视源与
`avatar_eye_from_gaze`；CLI 增加 `--eye-left-source / --eye-right-source /
--gaze-left-source / --gaze-right-source / --avatar-eye-from-gaze`。

验证：当前全量 202 个单测全绿（默认兼容、A openness + B gaze、左右眼/左右注视分源、
单侧过期回退、逐字段来源/时间戳）；`native-sine + native-gaze` 开启
`--avatar-eye-from-gaze` 后，SHM 中头像眼睛方向与渲染注视一致、openness 仍来自
sine。UI 首屏未改，逐眼来源先通过 `/api/status` 的 sources 字段和内部模型提供。

## 开放问题

- 左右眼来自不同源时，`vergence` 取哪一侧？阶段 A 先取“有 vergence 的那一侧”，
  两侧都有时按左眼优先；阶段 B 应改为属性级选源。
- `avatar_eye_from_gaze` 是否要进入 `modules.ini` 的 `[roles]`？阶段 A 只做 CLI
  开关，避免和现有四角色选择混淆。
- 逐字段置信度是否参与加权融合？阶段 A 仍不做加权，只透传/选择。
- 阶段 B 的 `FgbFrameSlotV2` 是独立结构还是 v1 末尾追加属性块，需要结合
  跨进程 ring 的版本协商一起定。
