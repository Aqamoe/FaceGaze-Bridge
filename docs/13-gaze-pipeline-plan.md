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
| 13 点目标（中心 + 内环 18° ×4 + 外环 32° ×8；另有 10°/20° 的窄范围可选），目标视线 → 逐眼视口坐标 | `gaze_calib/plan.*` |
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


### 2026-11-02 · 首次真机标定结果与两处修正

真机数据：`gaze.ini v1`，`source_kind=points`，**残差 P95 = 1.51%**（阈值 3%），
两眼映射接近一致（缩放 0.728/0.690，交叉项 ≈ 0），说明"原始点 → 视口坐标"这条换算是准的。

随后暴露的两个问题：

1. **拟合完成没有热加载**：写盘只保证下次启动可用，当前进程仍拿空档案 →
   页面显示"已完成"但档案"未标定"、残差 `—`。已在 `Session::finish` 补上 `apply_`。
2. **显示侧误差**：标定后区域"中间准、越往边上越偏外圈"，两眼还会对称分开。
   前者是**增益型**误差（过中心点，随离心度增长），后者是驱动侧的对称视差——
   两者都在"视口坐标 → 驱动区域"这一段，注视数据本身是准的。
   → VRSS 的 gain/offset 语义改为**显示侧微调**（不再被禁用），`stereo_offset` 不变；
   页面文案说明"中间准、边上偏外时调小增益"。

配套工具（本轮新增）：**注视点探针** `gaze_calib/probe.*`
- 校准页「显示注视点测试」按钮，头显里画两个点：青=左眼、品红=右眼；
- 点由 `unproject_viewport` 从"系统认为的视口坐标"反算方向后摆放，与
  `project_dir_to_viewport` 严格互逆（单测覆盖）；
- 判断方法：点若落在你真正看的方向上 → 数据没问题，剩下的偏差调显示侧；
  点本身偏 → 重标或换映射。


### 2026-11-02 · 来源可达范围不足 [0,1]（PICO 只有 0.31..0.74）

实测：PICO 原生逐眼点的水平/垂直都到不了 0/1，只在中段 0.31..0.74 之间有效；
再往外看数值贴边不再变化。后果是**边缘位置被截断**：区域走到某个角度就停住，
而且两眼各自的边界略有差异，于是两个点"分开"——看起来像标定错了，其实是数据没了。

处理（本轮）：

| 措施 | 位置 |
|---|---|
| 档案记录标定时实际见过的每眼输入范围 `input_bounds`（min/max × 2 轴） | `GazeProfile`、`Session::finish`、`gaze.ini` |
| 运行时判定饱和（贴边 4% 跨度内）→ `GazeState.saturated` | `GazeCalibrator::apply` |
| 饱和时**冻结在最后一对有效位置**，不让两眼按各自的边界越走越开 | 同上（`last_left_/last_right_`） |
| `/api/gaze` 暴露 `raw_left_point`/`raw_right_point`/`raw_point_valid`/`saturated` | `web/ipc.cpp` |
| 探针在饱和时提示"已到传感器范围边缘，位置保持不变" | `gaze_calib/probe.cpp` |

**注意**：`input_bounds` 要重标一次（v2）才会写进档案；v1 档案没有这份范围，
饱和检测不会生效（其余行为不变）。

### 2026-11-02 · 角度粗调指纹：标定后再动 `[eye]` 会被判过期

`[eye]` 的 gain/center 是圆盘标定的**上游**（P2.5：先角度校正、再投影、再拟合仿射）。
改它不会让仿射报错，只会让它悄悄偏（中心还准、边缘偏外圈），以前唯一的判断依据是
"用户记不记得自己调过"。

| 措施 | 位置 |
|---|---|
| `EyeAngleTrim`（active + 4 参数）记进档案顶层 `eye_trim` | `core/model/gaze_profile.hpp`、`params/gaze_profile.cpp` |
| 标定完成时由控制面提供当时生效的粗调，写进 `result_.trim`（**仅角度分支**：点型分支拟合的是原始 2D 点，粗调不参与，记了就是误报） | `gaze_calib/session.*`、`cli/main.cpp` |
| `trim_state()`：`match` / `stale` / `unknown`，比"效果"而非字段（开关关 = 恒等） | `core/model/gaze_profile.cpp` |
| `/api/gaze_calib` 暴露 `eye_trim` 与 `eye_trim_saved`（数值）；页面在 `stale` 时点亮「映射已过期」，并把标定时的粗调写进「档案」一行供核对 | `web/ipc.cpp`、`ui/` |
| 页面顺序改成 ① 角度粗调 → ② 圆盘标定 →（眼睑开合），两步相邻且带序号，不再被眼睑隔开 | `ui/index.html`、`ui/style.css` |

旧档案没有这条 → `unknown`，不冒充一致也不误报过期；改动粗调后要重标一次才会写入指纹。

### 2026-11-03 · 标定后注视点被关在中心小矩形里（饱和判定用错了范围）

**症状**：不标定时（`allow_source_points` 直通）注视点能到画面边缘；做完圆盘标定后
只能在一块居中的矩形里移动，往外看就停住。

**根因链**（三处配合）：

1. 13 点圆盘内环 10°、外环 20°，投影后只覆盖视口中间一块（PICO 实测：左眼
   x 0.42–0.75、右眼 x 0.25–0.58、y 0.31–0.69，即约屏宽 1/3 × 屏高 0.39）；
2. `Session::finish` 把这 13 个采样点的原始值 min/max 写进 `input_bounds`，
   语义被当成"传感器量程"，实际只是"这套点位覆盖到的范围"；
3. `GazeCalibrator::apply` 用 `near_bound` 判饱和——判据是 `v <= lo+eps || v >= hi-eps`，
   **框外的任何值都命中**，命中即把两眼换成 `last_left_/last_right_`。于是注视点
   永远出不了那个框。

**修法**：

| 措施 | 位置 |
|---|---|
| 饱和判定改成「贴边」**且**「连续 `kSaturatedHoldFrames`（6）帧原始值不变（±1e-5）」；真饱和的特征是不只是到极值，而是**值被钉住** | `core/process/gaze_calibrator.*` |
| 出框但仍在下落/上升的值不再判饱和，按仿射正常外推（sink 侧 `clamp_ndc` 兜底） | 同上 |
| 换档案（版本号变化）时作废饱和历史，避免用旧档案的原始值比"有没有变" | 同上 `apply` |
| 外环 20°→35°、内环 10°→18°（后调整为 32°/18° 并做成可选，见下一条）：覆盖推到 x 0.27–0.90，边缘才真正进拟合 | `gaze_calib/plan.hpp` |
| `input_bounds` 的注释改成"标定采样范围"，不再声称是传感器量程 | `core/model/gaze_profile.hpp`、`gaze_calib/session.cpp` |
| 单测改为 `gaze_calibrator_freezes_only_when_raw_is_pinned`：出框但值在变 → 跟随；贴边且被钉住 6 帧 → 冻结；回框内 → 恢复 | `tests/test_gaze_pipeline.cpp` |

**后续（同日第二轮已处理）**：`input_bounds` 不是传感器真实量程这一点，改成运行期
自学习包络，见下一条「设置只改内存不落盘」里的最后两行。

### 2026-11-03 · 设置只改内存不落盘；标定范围可选

**问题一**：设置页的 One Euro 滤波（开关 / minCutoff / beta）和日志级别改完只作用于内存，
重启回到默认值。`set_filter` / `set_log_level` 的处理函数里没有任何写盘动作——页面写着
"改动立即生效"，用户自然以为会记住。

**问题二**：外环定在 32°（上一轮从 20° 提上来的 35° 太大）对一部分人来说看不全——眼动
到那个角度很吃力、圆点又贴着镜片边缘，看不清就没法标。但缩小范围又会让边缘回到"纯外推"。

**改动**：

| 措施 | 位置 |
|---|---|
| 新增 `settings.ini`（`[filter]` / `[log]` / `[gaze_calib]`），原子写 + `.bak`，数值越界钳制、未知键忽略 | `config/settings.*` |
| 启动时把 `settings.ini` 当默认值，命令行覆盖（`--filter` / `--verbose` 仍然优先） | `cli/main.cpp` |
| `set_filter` / `set_log_level` 改完立刻 `persist_settings()`；保存失败时"已生效但保存失败"如实上报，不假装成功 | `web/ipc.cpp` |
| 状态里新增 `log_level`，设置页下拉框跟着引擎真实级别走（重启/--verbose 后不会再显示错档） | `web/ipc.cpp`、`ui/app.js` |
| 标定范围两档：`Span::wide`（内环 18° / 外环 32°）与 `Span::narrow`（10° / 20°），点位按 span 生成 | `gaze_calib/plan.*` |
| `Session::set_span()`，采集与拟合都按当前 span 取点位；运行中不接受切换 | `gaze_calib/session.*`、`web/ipc.cpp` |
| 校准页新增「标定范围」下拉（三语），选择存 `settings.ini`，`gaze_calib` 状态消息带 `span` | `ui/index.html`、`ui/app.js`、`ui/i18n.js` |

**为什么不加"保存"按钮**：这两项本来就是即时生效的，再加一步确认只会制造"忘了点保存"
的新问题。规则统一成"界面改了 = 已经存了"。

**顺带解耦量程**：饱和判定的"极值参照"从档案的 `input_bounds` 换成 `GazeCalibrator`
在**本会话观测到的原始值包络**（逐眼、只扩不缩，首帧由档案播种）。理由是窄范围标定
的窗口只有中间 1/3，拿它当极限会让「窗口边缘的稳定注视」被判成饱和；包络会跟着用户
真正看到哪儿一起长，只要看过更远，那里就不再算边界。不落盘：每次开机重新学一遍只花
几秒，而写盘反而会把一次性的极端值永久固化。

**兼容**：没有 `settings.ini` 时行为与之前完全一致（默认滤波关闭、级别 info、范围 wide）。

**实测补一个根因**（同日晚）：`settings.ini` 其实写对了（`min_cutoff = 0.5000`），
但重启后界面仍显示 1.0。原因是 `FaceFilter` 的 `live_min_cutoff_/live_beta_` 初值写死
1.0/0.5，`set_config()` 只改 `config_` 不同步这两个原子量；`process()` 第一帧拿它和
`config_` 比，判"面板把参数改成了 1.0"并把启动参数覆盖回去（`--filter-cutoff` 其实
一直没生效，只是没人发现）。修法：`set_config()` 把两个值同时写进 `live_*`，并加两条
回归用例（启动参数活过第一帧 / 面板滑杆仍然生效）。
