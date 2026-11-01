# ADR-0042 · 统一注视管线：规范化 / 标定 / 稳定化

- 状态：**实施中**——P0（角度/方向同源）与 P1a（规范化 + 标定骨架、`gaze.ini`、
  `/api/gaze` 三态）已落地 2026-11-02；P2 圆盘标定与 P1.7 参数收敛未做
- 日期：2026-11-02
- 影响：`src/core/model/gaze.hpp`、`src/core/process/`（新增 normalizer / calibrator）、
  `src/engine/engine.cpp`、`src/params/calibration.*`、`src/sinks/*`、`src/web/ipc.*`、
  `ui/`（新增注视校准页）、`sdk/docs/gaze-service.md`
- 关系：**修订** [ADR-0033](0033-gaze-point-2d-space.md) §2（允许 FGB 侧标定，但输出必须标
  `calibrated` 而不是 `viewport`）；**收紧** [ADR-0026](0026-gaze-estimator-and-api.md) 的字段语义；
  让 [ADR-0041](0041-vrss-source-space-points.md) 的应急路径退役；实施计划见
  [13 · 注视管线](../13-gaze-pipeline-plan.md)

## 问题

VRSS 落地之后，注视数据的链路暴露出四个独立问题，它们互相掩盖，导致"调了半天不知道哪一层在起作用"：

1. **两处校准、两套单位**。面捕路线靠 `calibration.ini [eye]` 的 `gain_x/center_x`（单位：度），
   在 `CalibrationApplier` 里改 `eyes.*.gazePitchYawDeg`；VRSS 路线靠
   `outputs.ini [vrss]` 的 `x_gain/x_offset/stereo_offset`（单位：NDC），在 sink 里改。
   同一只眼睛有两套参数，改一处另一处不知道。
2. **校准后两套值不同源（bug）**。`CalibrationApplier` 只缩放了角度，没有重算
   `gazeDirLocal`；`Engine::compute_gaze` 回退到 eyes 通道时，角度取校准后的值、方向取原始值
   （`engine.cpp` L244-245）。结果是：面捕 look 参数（读角度）已校准，VRSS 方向路径与
   SHM/UDP 的方向字段（读 `dir_local`）未校准。
3. **三套表示并存**。`GazeState` 同时携带合并方向、逐眼方向、逐眼角度、逐眼 2D 点，
   有效性与坐标系各不相同，哪个是"规范注视点"没有定义，只能由每个消费端自己判断。
   实测代价：VRSS 为 2D-only 源写了 `allow_source_points` + 四个补偿参数，为一个本该
   在别处解决的问题。
4. **映射责任无人承担**。ADR-0033 §2 把"原始→视口"的映射交给模块，但现实是模块只给 raw
   （PICO：两眼横向差 0.10–0.30 且随注视深度变化），FGB 侧只好在消费端手工凑。

## 决定

### 1. 三段式管线，唯一出口

在 `SourceMerger` 之后、任何 sink 之前，插入三个明确职责的阶段：

```
合并 → GazeNormalizer → GazeCalibrator → GazeStabilizer → GazeState（唯一出口）→ 所有 sink
```

| 阶段 | 只负责 | 明确不做 |
|---|---|---|
| `GazeNormalizer` | 统一**结构**：每眼 {点, 方向, 有效性, 时间戳, 置信度} + 空间标签；跨眼镜像、单眼失效、过期判定 | 不做单位换算，不做平滑 |
| `GazeCalibrator` | 统一**口径**：把任意空间映射到逐眼视口坐标，输出 `space=calibrated`；给出残差与档案版本 | 不做平滑，不判 fixation，不因残差失败而丢帧 |
| `GazeStabilizer` | 统一**策略**：死区、平滑、（可选）预测，`GazeEstimator` 的扫视/注视判定沿用 | 不改坐标系 |

顺序固定为 **规范化 → 标定 → 稳定化**。理由：仿射与线性平滑可交换，但死区、迟滞、
fixation 阈值都与单位相关，只有先落到视口坐标，这些阈值才是可解释的"屏宽百分比"。

### 2. 规范表示：以逐眼视口点为主

- **主表示**：`left_point_2d` / `right_point_2d`，`[0,1]`、左上原点、畸变前（沿用 ADR-0033 §1）。
- **派生表示**：逐眼方向（单位向量）与逐眼俯仰/偏航角度由点 + HMD 几何反算；
  几何不可用时角度保持来源值，并在 `GazeState` 里标 `geometry_missing`。
- 合并注视点（两眼平均）只用于展示与第三方"看哪儿"，不参与 VRSS。

反过来（以角度为主）不采用：每次要投影、依赖几何、2D-only 源根本没有角度可依。

### 3. 空间标签三态

`point_space` 从二态扩成三态，进 `/api/gaze`、WS JSON、SHM/UDP 的属性位：

| 值 | 含义 | 能做什么 |
|---|---|---|
| `source` | 来源自己的坐标（未标定） | 展示、趋势 |
| `viewport` | 模块声明已标定到该眼视口（ADR-0033 原文语义） | 直接消费 |
| `calibrated` | FGB 侧标定产出 | 直接消费，附 `profile_version` 与 `residual` |

`calibrated` 与 `viewport` 分开，是为了让"模块声明的"和"我们自己算的"在数据里可区分：
出问题时能立刻回答"是模块给错了，还是我们把标定拟合错了"。

### 4. `GazeProfile`：独立档案

- 文件：`%LOCALAPPDATA%\FaceGazeBridge\gaze.ini`，与 `calibration.ini`（形状/眼睑）分离。
  理由：标定结果与硬件、头显佩戴位置绑定，换头显必须整套重来；形状校准跟人不跟头盔。
- 内容：每眼 2D 映射（仿射 6 参数；残差超阈值时升级为单应 8 参数）、每眼角度
  gain/center、`source_kind`（点型/角度型）、`created_utc`、`hmd_identity`、
  `profile_version`、`residual_p95`。
- 纯函数：`map(raw) → viewport`，输入输出都不带状态，可单测、可回放。

### 5. 圆盘标定（采集流程）

- 位置：**标定发生在 `GazeNormalizer` 之后、`GazeCalibrator` 之前**——采集的是"标定前的
  规范观察"，配 target 拟合，产出 `GazeProfile`，再由 `GazeCalibrator` 应用。
- 界面：FGB 自建的 OpenVR overlay（淡色圆盘 + 目标点 + 进度），`IVROverlay` 新增到
  `src/openvr/`。标定必须在头显里看屏幕，桌面窗口全屏只能作为降级方案。
- 点位：13 点 = 中心 + 内环 4（上/下/左/右）+ 外环 8（45° 间隔），每点采集 800 ms，
  取中位数；要求 `fixation == true` 且头显姿态角速度低于阈值（头动会让点飞）。
- 拟合顺序：仿射 → 残差 P95 > 3% 视口宽时试单应 → 仍超标则判定"该来源不适合直接标定"，
  UI 给出结论而不是硬套一个坏映射。
- 验收看**残差**，不看"看起来像不像"。

### 6. 消费端契约

- sink 只读 `GazeState` 的规范字段；**不允许**再自行做增益/偏移/翻转。
- VRSS 退化为"视口点 → NDC"的一次换算（`x = 2X-1`、`y = 2Y-1`，一次翻转，只在一处）。
- `stereo_offset` 保留在 VRSS 配置里，但**改名并归入"显示侧补偿"**：实测驱动会自行按
  `fGazeNormalizedLocation` 再投影一次、产生两眼对称外扩，那是显示路径的属性，不是注视数据
  的属性，不能算进 `GazeProfile`。
- 面捕 look 参数（`v1_params` 的 `gaze_x/gaze_y`）改为读规范角度，与 VRSS 同源。

### 7. 旧参数的去向

| 现状 | 去向 |
|---|---|
| `calibration.ini [eye]` gain/center（度） | 迁移为 `GazeProfile` 的"角度分支粗调"；文件保留只读兼容一个版本 |
| `outputs.ini [vrss] x_gain/y_gain/x_offset/y_offset` | 删除（默认 1/0）；`stereo_offset` → 显示侧补偿；`flip_y` 随 `calibrated` 一起退役 |
| `outputs.ini [vrss] allow_source_points` | 删除：是否可发由 `GazeCalibrator` 是否有可用档案决定 |

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 标定放进 Normalizer（`GazeProfile` 当 Normalizer 的输入） | Normalizer 的产出会变成"已校准"，`space` 标签失去意义；没有"标定前观察"这个取样点，残差无处解释 |
| 保持现状：每个消费端各调各的 | 一个消费端一套参数，N 个消费端 N 次真机调试；`stereo_offset` 就是这样混进来的 |
| 只靠模块声明 `FGB_ATTR_POINT_VIEWPORT` | 模块做不到（实测把仿射标定做坏过），且面捕分支根本没有点，救不了角度型来源 |
| 以角度为主表示 | 每次消费都要投影、强依赖几何；2D-only 源无角度可用 |
| 标定结果混进 `calibration.ini` | 形状校准跟人不跟头显，注视标定跟头显和佩戴位置；放一起会导致"换个头显把表情校准也清了" |
| 直接用 NVAPI 的 `fGazeOrigin_mm` 让驱动自己算 | 未文档化行为，且我们已在 ADR-0041 里证明驱动会自行加视差；先用可观测的补偿旋钮 |

## 后果

- **收益**：注视只有一个口径、一处校准、一个调试入口；VRSS/光标/第三方/面捕 look 看到的是同一份
  数据；"偏了"这类问题定位从"猜四层"变成"看残差"。
- **代价**：引入 OpenVR overlay 依赖（仅标定页需要）；标定流程要求头显在位、头不动，约 30 秒；
  迁移期会有两套参数并存，UI 必须写明哪套生效。
- **风险**：
  - 原始点型来源的 per-eye 仿射残差可能超阈值（PICO 实测两眼 dx 随深度在 0.10–0.30 变化，
    非仿射能吸收）。此时按 §5 的降级规则判"不适合直接标定"，退回"两眼中点 + 显示侧补偿"，
    并在 UI 明示精度不足。
  - `/api/gaze` 与 SHM/UDP 的 `point_space` 语义扩张属于**对外契约变更**，SDK 文档、探针、
    第三方消费者要同步；`calibrated` 是新增值，旧消费者按"非 source 即可用"判断仍然正确。
- **不做**：世界坐标反投影、漂移补偿（后台缓慢修正）、注视点预测——都不在本 ADR 范围，
  前者需要完整内参/畸变模型，后两者等管线稳定后再谈。
