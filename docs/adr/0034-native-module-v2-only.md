# ADR-0034 · 原生模块只保留 ABI v2

- 状态：**已实施**（2026-10-31）
- 日期：2026-10-31
- 影响：`sdk/fgb_module_api.h`、`src/sources/native_source.*`、`src/sources/native_gaze_parse.hpp`、
  `src/vrcft/module_registry.*`、`src/web/ipc.cpp`、`ui/app.js`、`samples/native-*`、
  `tests/test_native_module.cpp`、`tests/test_native_host.cpp`、`docs/01`、`docs/05`
- 关系：取代 [ADR-0018](0018-native-c-abi-v1.md) 的 ABI 形态；吸收
  [ADR-0030](0030-native-module-v2-composition.md)、[ADR-0031](0031-native-module-v2-attribute-fusion.md)

## 问题

到 2026-10-31，`sdk/fgb_module_api.h` 同时提供两套不兼容的 ABI：

- **v1**：`FgbModuleV1` / `FgbFrameSlotV1`（整眼原子 `FgbEyeV1`，末尾追加 `FgbGazeV1`）；
- **v2**：`FgbModuleV2` / `FgbFrameSlotV2`（`attributes` 属性掩码 + 逐属性时间戳）。

宿主先探测 `fgb_module_query_v2`，失败回退 `fgb_module_query`。评估结论：

1. **v2 在数据能力上是 v1 的严格超集**。expression、逐眼 openness/pupil/gaze、
   合并方向、逐眼 2D 点、聚散、头姿都能一一映射（`docs/05` §12 的迁移表就是证明）。
   v1 唯一"多"出来的是 `EYE` 的整眼原子语义，而那是 v2 有意取消的约束。
2. **两条语义路径各写一遍，已经开始漂移**：伪造方向/越界点位的诊断只挂在 v1 分支，
   v2 模块写错了一声不响；v1 不写 `channelStamps`，同一模块换成 v2 后新鲜度语义变了；
   v2 不按能力位门控通道，"只给注视的模块会顶掉头像睁眼度"这条约束靠每个调用点自觉。
3. **v1 的实际装机量为 0**。v1.0.2（2026-10-30）发布的 SDK 只有最基础的 `FgbFrameSlotV1`，
   无 `gaze` 字段、无 v2；V1 的 gaze 追加与整个 v2 从未发布。项目没有在线模块注册表，
   仓库内 4 个示例中 3 个是 v1 自测夹具，没有外部模块依赖 v1。
4. 维护双 ABI 的代价是长期存在的：两份结构体（约 180 行公开契约）、两条 `apply_*`
   路径、两个加载分支、两套文档主线、双份测试基线。

## 决定

### 1. 删除 v1，`FgbModule` / `FgbFrameSlot` 成为唯一 ABI

- 删除 `FGB_MODULE_ABI_V1`、`FgbEyeV1`、`FgbGazeV1`、`FGB_GAZE_*`、`FgbFrameSlotV1`、
  `FgbHostServicesV1`、`FgbInitParamsV1`、`FgbModuleV1`、`fgb_module_query`。
- 公开类型去掉 `V2` 后缀：`FgbModule`、`FgbFrameSlot`、`FgbHostServices`、
  `FgbInitParams`、`FgbEyeAttributes`。ABI 版本仍然只由 `FGB_MODULE_ABI_V2` 与
  `abi_version` 字段表达。
- **导出符号保留 `fgb_module_query_v2`**：不占用 v1 的老符号名，老 DLL 的失败形态
  是"没有这个导出"，而不是"导出了但结构对不上"。

### 2. 能力位按"通道"收敛，属性位按"数据"收敛

- `FGB_CAP_EYE`（整眼原子）+ `FGB_CAP_EYE_ATTRIBUTES`（属性眼睛）合并为
  `FGB_CAP_EYES = 0x1`：逐眼属性可以是 openness/pupil/gaze/point 的任意子集。
  清单里 `"eye"` / `"eyes"` / `"eye_attributes"` 都解析到这一位，老清单不会失效。
- 能力位重新承担门控职责（v2 原实现只在加载时检查"非 0"）：

  | 能力位 | 决定通道 |
  |---|---|
  | `FGB_CAP_EXPRESSION` | Expression |
  | `FGB_CAP_EYES` | Eyes（头像眼睛，含聚散） |
  | `FGB_CAP_GAZE` | Gaze（渲染注视/foveation） |
  | `FGB_CAP_HEAD` | Head |

  这样界面角色、合并层选源、宿主告警三者用同一套规则。写了某类属性却没声明
  对应能力位时，宿主**告警一次并仍采用数据**（沿用本项目"只记日志、不改数据"的惯例）。

### 3. 补齐 v2 自身的语义缺口

- 注视诊断（伪造 `-Z` 指纹、视口坐标持续越界、属性与能力位不一致）移到公共路径，
  对唯一路径生效，跨进程宿主也会把它转进引擎日志。
- 删掉 `FgbEyeAttributesV2::valid`：宿主从来没读过它（以帧的 `attributes` 为准），
  留一个不生效的公开字段只会误导模块作者。一只眼"这帧没数据"的表达方式就是它的
  属性位没置。
- 加载时不再要求 `struct_size >= sizeof(FgbModule)`，改为"覆盖到 `teardown` 即可"，
  与 ADR-0018 §2 的 `min(struct_size, sizeof)` 规则对齐（追加尾部字段不再让旧模块一次性全灭）。
- `FgbInitParams` 末尾追加 `host_version`（UTF-8，0 = 旧宿主没提供）。
- 清单新增 `AbiVersion`；不等于 2 的模块在模块列表里直接标成不兼容，
  不必等到 `LoadLibrary` 才失败。

### 4. 示例、测试、文档随 ABI 一起迁移

- 4 个示例全部改成 v2 导出，`native-sine` 从"v1 兼容基准"变成"全通道 v2 参考实现"。
- 新增测试夹具 `tests/fixtures/legacy_v1_module.cpp`：只导出老符号的 DLL 必须被拒绝，
  且错误文案要指向"用当前 SDK 头重新编译"——这条路径是移除 v1 后的主要用户体验。
- `docs/05` 主线改为 v2，末尾提供 v1 → v2 迁移对照表。

## 代价与边界

- **用旧 SDK 编译的 DLL 不再可用**。当前装机量为 0，但错误文案与测试必须覆盖，
  否则将来第一个"从旧教程抄代码"的用户会看到不理解的失败。
- 能力位门控是**行为变化**：只置属性位、没声明能力位的数据现在不再进通道
  （此前 v2 实现会进）。这是一次性收敛，靠告警让写错的模块可发现。
- 逐眼 `valid` 字段删除后，"这一帧这只眼没有数据"只能靠清属性位表达；
  逐属性时间戳仍然保留，混合新旧数据的能力不受影响。
- v2 属性帧体积比 v1 略大（多 `attributes` 与逐属性时间戳），进程内与共享内存
  两种模式的帧结构都在几百字节量级，不计入延迟预算。

## 被否决的替代方案

| 方案 | 否决理由 |
|---|---|
| 保留 v1 作为冻结兼容层 | 兼容层本身没有用户；两条语义路径继续漂移，诊断/门控仍要写两遍 |
| v1 转 v2 的适配器（宿主侧统一） | 只有在存在 v1 模块时才值得；现在纯粹是新增一层转换代码 |
| SDK 内 header-only v1 shim | 救不了已编译的 DLL，等于用一个新抽象替换一个老抽象 |

## 验证计划

- 单测：214 个用例全绿（含 `native_v1_only_dll_is_rejected_with_migration_hint`、
  `native_manifest_rejects_removed_abi_version`、属性/能力位门控、2D 点空间、
  跨进程宿主 v2 模块）。
- 端到端：`native-sine` 单模块（表情 + 眼睛 + 头姿）、`native-face` + `native-gaze`
  组合、`native-openness` + `native-gaze` 属性合成，`--native-host` 下各跑一遍。
- 构建：`/W4 /WX`（`FGB_WARNINGS_AS_ERRORS=ON`）无告警。
