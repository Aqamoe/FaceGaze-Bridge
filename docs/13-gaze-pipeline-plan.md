# 13 · 注视管线实施计划（规范化 / 标定 / 稳定化）

- 状态：**实施中**（P0 ✅ · P1a ✅ · P2 代码完成，待真机验证 · P1.7/P3 未开始）
- 日期：2026-11-02
- 决策：[ADR-0042](adr/0042-unified-gaze-pipeline.md)
- 关联：[ADR-0026](adr/0026-gaze-estimator-and-api.md)（注视估计与 API）、
  [ADR-0033](adr/0033-gaze-point-2d-space.md)（2D 点坐标约定）、
  [ADR-0041](adr/0041-vrss-source-space-points.md)（VRSS 遇到来源空间点）、
  [01-architecture §10](01-architecture.md#10-校准管线)（校准管线）、
  [12 · VRSS 融合](12-vrss-integration-plan.md)

---

## 0. 目标与边界

### 0.1 目标

1. 全链路只有**一个**规范注视点（`GazeState`），sink 只读、不再各自换算。
2. 全链路只有**一处**注视校准（`GazeProfile`），由 VR 圆盘标定拟合，带残差。
3. 注视源的两条分支（模块 2D 点 / 面捕角度+方向）在规范化后**同构**，下游不再分支。
4. 校准结果可解释：每眼残差、档案版本、来源空间都在数据里，出问题能定位到层。

### 0.2 明确不做

| 不做 | 原因 |
|---|---|
| 世界坐标反投影 / 视线命中点 | 需要完整内参与畸变模型；2D 点没有深度 |
| 漂移补偿（后台缓慢修正零点） | 先有稳定标定，再谈漂移；否则两种修正互相打架 |
| 注视点预测（速度外推） | M5.4，与本次正交 |
| 每应用（per-game）偏移 | 显示侧补偿（`stereo_offset`）已够；按应用分档会把两套参数问题搬回来 |
| 在 FGB 里做镜头畸变校正 | 畸变由合成器施加，标定在畸变前空间做（ADR-0033 §1） |

---

## 1. 管线与不变量

```
SourceMerger ──▶ GazeNormalizer ──▶ GazeCalibrator ──▶ GazeStabilizer ──▶ GazeState
                （结构/有效性）      （口径/视口点）      （死区/平滑/注视判定）      │
                                                                        ┌────────┴────────┐
                                                                   VRSS · WS/SHM/UDP · 面捕 look · /api/gaze
```

不变量（违反即视为 bug，单测覆盖）：

1. **顺序固定**：规范化 → 标定 → 稳定化。平滑不得在标定之前。
2. **标定是纯函数**：`map(raw) → viewport`，不带状态、不做时间相关决策。
3. **空间标签只降不升**：没有经过拟合映射的数据不得标 `viewport` 或 `calibrated`。
4. **单眼失效不连坐**：一眼无效时另一眼照常输出，镜像规则只在 sink 侧、且保持 `< 150 ms`。
5. **sink 不做校准**：sink 里出现 gain/offset/flip 就是违规。

### 1.1 输出空间标签规则

| 情形 | `point_space` |
|---|---|
| 经 `GazeProfile` 拟合映射 | `calibrated`（附 `profile_version`、`residual`） |
| 模块声明 `FGB_ATTR_POINT_VIEWPORT` 且直通 | `viewport` |
| 角度/方向经 HMD 几何投影 | `viewport`（几何换算不是标定） |
| 以上都不成立 | `source`（VRSS 等消费端拒绝使用） |

---

## 2. P0 · 止血：角度与方向不同源

**为什么先做**：这是现存 bug，且会让 P1 的验收数据不可信——调 `[eye] gain` 时
`/api/gaze` 的方向不变，会让人误判标定没生效。

| # | 内容 | 位置 |
|---|---|---|
| P0.1 | 校准角度后由角度重算方向：`gazeDirLocal = pitch_yaw_to_dir(校正后角度)`；方向与角度必须同源 | `src/params/calibration.cpp` L655-663 |
| P0.2 | 回退分支不再"角度用校准值、方向用原始值"，统一由同一个方向函数派生 | `src/engine/engine.cpp` L244-245 |
| P0.3 | 单测：给定 gain/center，`dir_local` 与 `pitch/yaw` 满足同一三角函数关系 | `tests/test_calibration.cpp` |
| P0.4 | VRSS 的 `x_gain/y_gain/x_offset/y_offset` 默认归 1/0，UI 标注"临时补偿，将由注视校准取代" | `ui/index.html`、`i18n.js` |

**验收**：改 `calibration.ini [eye] gain_x` 后，`/api/gaze` 的 `dir_local` 与 `yaw_deg`
同步变化，比值符合增益；`fgb-tests` 全绿。

---

## 3. P1 · 规范层：结构与口径分开

| # | 内容 | 位置 |
|---|---|---|
| P1.1 | `GazeState` 增字段：`point_space`（枚举三态）、`profile_version`、`residual`、`geometry_missing` | `src/core/model/gaze.hpp` |
| P1.2 | `GazeNormalizer`：统一结构、跨眼镜像、单眼失效、过期判定、空间标签（不做单位换算） | 新增 `src/core/process/gaze_normalizer.{hpp,cpp}` |
| P1.3 | `GazeProfile` 结构 + 读写（`gaze.ini`，带版本、残差、HMD 身份） | 新增 `src/params/gaze_profile.{hpp,cpp}` |
| P1.4 | `GazeCalibrator`：按 `profile.source_kind` 选转换器（源点→仿射 / 角度→角度校正+几何投影 / viewport 直通）；无档案时按 §1.1 规则降级 | 新增 `src/core/process/gaze_calibrator.{hpp,cpp}` |
| P1.5 | 引擎接线：两段挂在合并之后、`compute_gaze` 之前；`GazeEstimator` 保持在稳定化位置 | `src/engine/engine.{hpp,cpp}` |
| P1.6 | 消费端改造：VRSS 只做点→NDC；WS/SHM/UDP 输出 `point_space`；面捕 look 读规范角度 | `sinks/*`、`params/v1_params.cpp` |
| P1.7 | **推迟到 P2 之后**：VRSS 参数收敛（删 `allow_source_points`、`flip_y`、四个 gain/offset；`stereo_offset` 归显示侧） | `outputs.*`、`vrss_gaze_sink.*`、`ui/` |
| P1.8 | 单测：标签规则、单眼失效不连坐、档案分支不匹配不套用、档案读写往返 | `tests/test_gaze_pipeline.cpp` |

> **排期修订（2026-11-02）**：P1.7 不能在 P2 之前做。`GazeProfile` 要到 P2 才有产出，
> P1 阶段标定是恒等的——先删 VRSS 的位置补偿，用户当前已调好的 VRSS 会直接失效。
> 顺序改为 **P1a 管线骨架（保留 VRSS 参数）→ P2 圆盘标定产出档案 → P1.7 拆参数**。

**验收**：
- `/api/gaze` 的 `point_space` 在四种情形下取值符合 §1.1；
- 2D-only 源（PICO）在没有档案时仍报 `source`，VRSS 拒绝发送且原因来自 `GazeCalibrator`；
- 有任意一版手工档案（哪怕恒等仿射）时，同源数据报 `calibrated` 并能驱动 VRSS；
- `tools/probe_ipc.py` 通过，`point_space` 出现在 `/api/gaze`。

---

## 4. P2 · 圆盘标定：采集、拟合、残差

| # | 内容 | 位置 |
|---|---|---|
| P2.1 | OpenVR overlay 封装：创建/销毁、贴图、跟随头显、可见性 | 新增 `src/openvr/overlay.{hpp,cpp}` |
| P2.2 | 标定界面：淡色圆盘 + 当前目标点 + 进度 + 中止；13 点（中心 + 内环 4 + 外环 8） | 新增 `src/gaze_calib/overlay_ui.{hpp,cpp}` |
| P2.3 | 采集：每点 800 ms，取中位数；仅采 `fixation == true` 且头部角速度低于阈值；每点至少 N 帧否则判该点无效 | 新增 `src/gaze_calib/collector.{hpp,cpp}` |
| P2.4 | 拟合：每眼仿射（6 参数，正规方程）→ 残差 P95 > 3% 视口宽时试单应（8 参数）→ 仍超标判"不适合直接标定" | 新增 `src/gaze_calib/fit.{hpp,cpp}` |
| P2.5 | 角度分支：先角度 gain/center 校正，再经几何投影，然后拟合一次线性修正 | 同上 |
| P2.6 | 落盘与复用：`gaze.ini` 原子写；HMD 身份或不匹配时提示重标 | `src/params/gaze_profile.cpp` |
| P2.7 | 侧栏「注视校准」页：开始/中止/进度、每眼残差、档案版本与时间、恢复默认 | `ui/`（三语） |
| P2.8 | 单测：合成数据（已知仿射 + 噪声）反解参数、残差计算、单应分支、退化输入（点共线）不崩 | `tests/test_gaze_calib.cpp` |

**验收（真机）**：
- 白墙/远景下 13 点采集 30 秒内完成，每眼残差 P95 < 3% 视口宽；
- 头显内四方向 + 中心，高质量区/光标跟到位，两眼不分离；
- 连续两次标定，同一点的位置差异 < 2% 视口宽；
- 拔掉头显或 SteamVR 未开时，标定页明确提示（不进入采集）。

---

## 5. P3 · 契约收敛与文档

| # | 内容 |
|---|---|
| P3.1 | `/api/gaze`、WS `gaze`：新增 `point_space`、`profile_version`、`residual`、`geometry_missing`；SHM/UDP 用属性位表达同一件事（不改布局） |
| P3.2 | `sdk/docs/gaze-service.md`：写清"消费端只读规范点，不得自行校准"；给出四种 `point_space` 的处理建议 |
| P3.3 | 删除 `calibration.ini [eye]` 的写入路径（保留读取兼容一个版本）；`outputs.ini [vrss]` 清理已弃键 |
| P3.4 | `docs/01` §10、`docs/09` §5/§11、`docs/02` §4 同步；ADR-0033 增补"FGB 侧标定"的修订说明 |
| P3.5 | 探针：`tools/probe_ipc.py` 输出 `point_space`/`residual`；新增 `tools/probe_gaze_calib.py`（回放档案、校验映射单调） |

---

## 6. 待定事项

| # | 问题 | 倾向 |
|---|---|---|
| Q1 | 标定页是否用注视停留（dwell）确认，还是手柄/键鼠点「开始」 | 先用键鼠+倒计时；dwell 需先有稳定光标，因果循环 |
| Q2 | 13 点是否够；是否需要第二遍随机 3 点做验证 | 13 点先做，验证遍用 3 点常驻（成本低） |
| Q3 | 角度分支要不要二次项（01 §10.2 曾设想 2 次项） | 先仿射；残差超标再升级，避免自由度换拟合噪声 |
| Q4 | `gaze.ini` 是否随头显身份分档（一套档案多头盔） | 首版单档案 + 身份指纹提示；分档等有第二台头显的实测再说 |

---

## 7. 风险

| 风险 | 级别 | 缓解 |
|---|---|---|
| 原始点型来源（PICO）两眼 dx 随深度变化 0.10–0.30，仿射残差可能超标 | 高 | §4 P2.4 的降级规则；UI 明示"该来源不适合直接标定"，退回中点+显示侧补偿 |
| `point_space` 语义扩张属于对外契约变更 | 中 | `calibrated` 是新增值，旧消费者按"非 source 即可用"仍正确；SDK 文档同步 |
| 标定期间头动导致点飞、拟合出坏映射 | 中 | 头显角速度阈值 + 每点中位数 + 残差门槛，三重拦截 |
| overlay 在 PICO 串流 / 部分 runtime 上不可用 | 中 | 真机验收项；不可用时降级为桌面全屏窗口标定并标注精度较低 |
| 迁移期两套参数并存（`[eye]` 与新档案） | 中 | P1 就把 `[eye]` 降级为"角度粗调"并在 UI 标注哪套生效；P3 删除 |
| 规范层引入额外延迟（120 Hz 热路径） | 低 | 两段都是纯算术（< 2 µs），零分配；`tick_stats` 现有 p95 可对照 |

---

## 8. 验收清单

- [ ] P0：`dir_local` 与角度同源，改增益两者一起动
- [ ] P1：`point_space` 四态符合 §1.1；VRSS 只做点→NDC；sink 里搜不到 gain/offset
- [ ] P2：13 点标定完成 ≤ 40 s，每眼残差 P95 < 3%，四方向与中心都到位
- [ ] P2：连续两次标定差异 < 2%；退化输入不崩
- [ ] P3：`/api/gaze`/WS/SHM/UDP 字段一致；SDK 文档说明"不得自行校准"
- [ ] 回归：`fgb-tests` 全绿；`probe_ipc`、`probe_ui`、`probe_vrss_flow` 通过
- [ ] 真机：VRSS 高质量区跟随，两眼重合；面捕 look 与 VRSS 同源（同一帧改增益两者同时变）


---

## 9. 进度

### 2026-11-02 · P0 完成（`db38bc9`）

`CalibrationApplier` 校准角度后重算 `gazeDirLocal`；`compute_gaze` 回退分支的方向也由角度
派生，合并方向不再混用原始值。2 个单测覆盖"角度→方向→角度"闭环与 `has_eye=false` 透传。

### 2026-11-02 · P1a 完成

| 落地 | 位置 |
|---|---|
| 空间三态 `GazeSpace` + `profile_version` / `residual` / `geometry_missing` | `core/model/gaze.hpp` |
| 规范注视点管线（规范化 → 标定），`compute_gaze` 拆成 `assemble_gaze` + 两段 | `core/process/gaze_normalizer.*`、`core/process/gaze_calibrator.*`、`engine/engine.*` |
| 逐眼投影与"方向→视口点"唯一实现 | `core/model/gaze_projection.hpp`（`project_dir_to_viewport`） |
| 标定档案结构 + `gaze.ini` 读写（原子替换、坏行只丢该行） | `core/model/gaze_profile.*`、`params/gaze_profile.*` |
| 启动时注入几何与档案（几何同时喂 VRSS 与管线） | `cli/main.cpp` |
| `/api/gaze` 输出 `point_space`(三态) / `profile_version` / `residual` / `geometry_missing` | `web/ipc.cpp` |
| 10 个单测：标签、过期、镜像、投影、缺几何、无档案降级、分支不匹配、档案往返 | `tests/test_gaze_pipeline.cpp` |

已知取舍（P2 之前保持不变）：

- VRSS 的 `allow_source_points` / `flip_y` / 四个位置参数仍在，位置校准仍由它们 + 未来的档案
  两处负责；P1.7 会删除前两者，把 `stereo_offset` 归为显示侧补偿。
- 方向型来源现在会由管线产出逐眼视口点，VRSS 走的是"点分支"而不是自己的方向分支；
  两条路径的公式已统一到 `project_dir_to_viewport`，数值一致（P1.7 再删旧路径）。
- 过期判定（50 ms）从 VRSS 上移到规范化层：语义不变，但来源如果给的是硬件时钟而不是引擎
  时钟，会被判过期——与 VRSS 原来的行为一致，不新增风险面。

### 2026-11-02 · P2 核心完成（点位 / 采集 / 拟合）

不依赖 OpenVR 的那一半先落地，全部有单测（合成数据反解参数、噪声残差、共线拒绝）：

| 落地 | 位置 |
|---|---|
| 13 点目标（中心 + 内环 10° ×4 + 外环 20° ×8），目标视线 → 逐眼视口坐标 | `gaze_calib/plan.*` |
| 采集：每目标 ≥20 帧、取中位数、离散度（MAD）≤6% 视口宽才算有效；单眼帧直接拒 | `gaze_calib/collector.*` |
| 拟合：逐眼 6 参数仿射（正规方程 + 列主元消元），残差 P95；拒绝样本不足、共线、参数离谱 | `gaze_calib/fit.*` |
| 档案组装：任一眼失败则整体失败；残差取两眼更差的那只 | `gaze_calib/fit.cpp` `build_profile` |

还没做（需要头显迭代，且我这边无法验证）：

- `src/openvr/overlay.*`：IVROverlay 封装（创建/贴图/跟随头显/可见性）；
- `gaze_calib/session.*`：把"眨眼确认开始 → 逐点 800 ms → 拟合 → 落盘 → 热加载档案"
  串成状态机，并处理"中途头动/丢点"的重试；
- IPC 命令（`gaze_calib_start` / `gaze_calib_cancel` / `set_gaze_profile`）与侧栏「注视校准」页；
- 关键约定：**开始标定前先把当前档案清空**（恒等），否则采到的是"已校准后"的值，
  再拟合就是二次校准。


### 2026-11-02 · P2 会话/界面完成（待真机验证）

| 落地 | 位置 |
|---|---|
| `IVROverlay` 圆点：动态加载 openvr_api.dll、`SetOverlayRaw` 贴抗锯齿白点、按视线角度挂到头显、按距离换算角半径 | `openvr/overlay.*` |
| 会话状态机：开始即清空档案 → 每点 0.5 s 静置后开采 → 25 帧中位数/离散度达标即进下一点 → 拟合 → 落盘 → 热加载；超时（8 s/点）与失败都回滚到开始前的档案 | `gaze_calib/session.*` |
| `tick` 由控制面每帧驱动（无独立线程），页面关掉也继续跑 | `web/ipc.cpp` `pump` |
| 命令 `gaze_calib_start` / `gaze_calib_cancel` / `gaze_calib_clear`，`gaze_calib` 消息 2 Hz，`GET /api/gaze_calib` | `web/ipc.*` |
| 校准页新增「注视点校准（VR 圆盘）」卡片：状态/进度/残差/档案版本 + 开始/取消/删除（三语） | `ui/` |
| overlay 与会话随引擎生命周期创建，退出时 `cancel` + `close` | `cli/main.cpp` |

已在本机验证：`/api/gaze_calib` 字段齐全、`gaze_calib_start` 返回 ack 并进入 running、
overlay 创建成功（日志 `overlay 就绪`）、无注视数据时 8 s 后按预期失败并给出可操作原因、
`probe_ipc.py` PASS（新增消息不影响既有类型）。

**未验证**：真机里的圆点位置/大小/可见性，13 点实际手感，残差量级。这些只能进头显看。
