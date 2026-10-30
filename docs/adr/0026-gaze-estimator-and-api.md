# ADR-0026 · 注视估计与 `/api/gaze`

- 状态：**已实施**（2026-10-30）
- 日期：2026-10-30
- 影响：`src/core/model/gaze.hpp`（新增，`GazeState` 从 sinks 上移）、
  `src/core/process/gaze_estimator.*`（新增）、`src/engine/engine.*`、
  `src/web/ipc.*`、`ui/`（状态页注视卡片）、`tools/probe_ipc.py`、`tools/probe_ui.py`
- 关系：M5.1 / M5.2；M5.3 的头姿已在 FaceFrame 里，M5.4 预测、5.5–5.8 OpenXR 未做

## 问题

`GazeState` 早就有方向、俯仰/偏航、聚散距离和置信度，但只有内部 sink 能看：

- 没有"这是扫视还是注视"的判断，第三方拿到方向也不知道该不该稳定；
- 没有对外 API，研究/脚本/串流只能通过 WS JSON 输出猜字段；
- `GazeState` 放在 `sinks/sink.hpp`，core 层的估计器无法反向依赖它。

## 决定

### 1. `GazeState` 上移到 `core/model/gaze.hpp`

新增字段：

| 字段 | 含义 |
|---|---|
| `ts_ns` | 这次更新的时刻（引擎 tick） |
| `saccade_vel_deg_s` | 相邻帧方向夹角 / 时间 |
| `fixation_ms` | 当前注视已持续多久 |

`GazeState` 是数据模型，放在 core/model；sink 只包含头文件，不改变依赖方向。

### 2. GazeEstimator：角速度 + 迟滞 + 停留时间

- 角速度 = `acos(dot(prev_dir, dir)) / dt`，与分辨率/帧率无关。
- 进入注视：速度 < **30°/s**，持续 >= **80 ms**；
- 退出注视：速度 > **80°/s**（迟滞区 30–80 不抖动）；
- 断档 > **0.5 s** 时不跨帧累计，直接退出注视，避免"卡住后一直算注视"；
- 只在引擎有新帧时更新；没有新帧时 `ts_ns` 保持旧值，消费者可据此判断数据新鲜度。

### 3. 对外两个入口，同一份 JSON

- `GET /api/gaze`：只读，curl/脚本直接拿当前快照。
- 控制面 WebSocket `{"type":"gaze"}`：**30 Hz** 推送；连接建立时立即补一帧。
- 字段：

```json
{
  "type": "gaze",
  "ts_ns": 1284733000000,
  "valid": true,
  "origin_local": [0, 0, 0],
  "dir_local": [0.02, 0.11, -0.99],
  "left_pitch_deg": 5.7,
  "left_yaw_deg": 3.9,
  "right_pitch_deg": 5.7,
  "right_yaw_deg": 2.1,
  "pitch_deg": 5.0,
  "yaw_deg": 1.0,
  "vergence_distance": 1.24,
  "confidence": 0.93,
  "fixation": true,
  "fixation_ms": 492.0,
  "saccade_vel_deg_s": 2.3,
  "head_pose": { "q": [0, 0.03, 0, 0.999], "p": [0, 0.05, 0] }
}
```

- `origin_local` 固定 0：`FaceFrame` 只有方向，没有眼睛原点模型，不假装有。
- `head_pose` 仅当这一帧有头姿时出现。
- **不返回 `origin_world` / `target_world`**：没有世界变换就不编一个。

### 4. 状态页可见

状态页新增「注视」卡片：方向、俯仰/偏航、聚散、注视/扫视、扫视速度、置信度、
头姿，以及一个 2D 小圆点（yaw ∈ [-30,30]、pitch ∈ [-20,20] 映射到框内）。
眼追无效时全部显示 `—`、圆点隐藏。

### 4.1 逐眼显示（2026-10-31 修订）

原始版本只看合并方向，聚散距离和单眼失效都没法自证，因此补上逐眼一行：

- `GazeState` 增 `left/right_pitch_deg`、`left/right_yaw_deg`；
  gaze 通道从各自方向反算，回退 eyes 时读来源缓存的 `gazePitchYawDeg`（与合并角度同源）。
- `/api/gaze` 同步输出四个字段；**客户端不要再自己反算**，符号约定只守引擎一处。
- 卡片拆成「方向（合并）」「左眼」「右眼」三行，2D 框内左右眼各一个点（L 青 / R 黄）；
  单眼失效时该行 `—`、该点隐藏，不允许拿残值继续画。
- 新增「逐眼来源」行（`left_source/right_source` + 逐眼捕获时刻差），
  两眼不同源或时间差 > 50 ms 标黄——这正是合并方向"悄悄偏一边"的典型场景。
- **来源只给 2D 点时优先显示点**（2026-10-31 真机实测后补）：`point2d_valid` 为真时，
  逐眼行显示 `2D (x,y)`（`[0,1]`、左上原点），2D 框按点画而不是按角度。
  原因：2D-only 的私有通道没有方向模型，`dir_local` 是模块填的占位 `-Z`，
  照方向显示会让两眼看起来完全一样（实测 `(0.00,0.00,-1.00) 0.0°/0.0°` ×2），
  而真实差别在点上（实测 L `0.82,0.90` vs R `0.91,0.90`）。
  原来的「屏幕点」单独一行随之删除：信息已经在这一行里，留两行只是同一数据两种精度。

为什么逐眼角度放在引擎而不是界面：UI、脚本、第三方面板各写一遍 `asin/atan2`，
正负号和归一化迟早会分歧，而 `/api/gaze` 是对外契约，必须以引擎为准。
逐眼**注视/扫视判定**仍不做：那是一套独立状态机，M5.1 只要求整体判定。

### 4.2 方向与 2D 点解耦（2026-10-31 修订二）

真机实测暴露出解析层与 SDK 契约不一致：

- SDK 写着「逐眼/合并方向与 2D 点分别有效，互不连坐」（`FGB_GAZE_LEFT/RIGHT/COMBINED`
  与 `FGB_GAZE_POINT_2D` 是两套位）；
- 但 `native_source.cpp` 里点位被写成 `POINT_2D && 该眼有效`，整段 gaze 解析还被方向位门控。
  只声明 `FGB_GAZE_POINT_2D` 的模块会被整帧丢掉，于是只给 2D 点的模块只能伪造
  `(0,0,-1)` 方向才能出数据——副作用是 `fixation` 永远「注视中」、第三方拿到假方向。

决定：

1. 解析抽到 `src/sources/native_gaze_parse.hpp`（`apply_gaze_v1` / `apply_eye_gaze_v2`），
   只按各自的位取值：`valid` 只看方向位，`pointValid` 只看点位；抽成头文件是为了能单测。
2. `GazePair::any_valid()` 把 `pointValid` 也算进去：只给点的帧必须能让 gaze 通道成立。
3. `GazeState` 增 `left_point_valid` / `right_point_valid`；`/api/gaze` 同步输出。
   `point2d_valid` 保留为「左右眼都有点」的兼容字段。
4. `GazeState::valid` 语义收紧为「有可用的合并方向」：只有点时为假，
   于是 `GazeEstimator` 直接复位（不再把恒定 -Z 当注视），JSON 也不会给假方向。
5. v2 属性路径同样解耦：`apply_eye` 只管头像眼睛，注视样本走 `apply_eye_gaze_v2`；
   只给 `FGB_ATTR_*_POINT` 的眼也会留下点。
6. 示例模块 `samples/native-gaze` 增 `FGB_SAMPLE_GAZE_POINTS_ONLY=1` 模式，
   用真实 ABI 覆盖这条路径（`native_gaze_points_only_module_reaches_engine`）。

**模块作者注意**：没有方向模型就只声明 `FGB_GAZE_POINT_2D`，不要伪造方向位。

### 5. 探针

- `tools/probe_ipc.py` 增加 `/api/gaze` 与 WS `gaze` 计数/字段断言；3 秒应收到约 90 条。
- `tools/probe_ui.py` 增加注视卡片检查：`state.gaze.valid`、方向文本、圆点可见。

## 实测证据（2026-10-30，回放数据）

```
GET /api/gaze -> valid=True pitch=-3.156 yaw=0.764 vergence=0.0
WS 3 秒：gaze 89 条（约 30 Hz），最后 fixation=True saccade=0.0°/s
UI 探针：注视中 · 492 ms，方向 (-0.00, -0.19, -0.98)，圆点可见
单测 147 个全过（新增 3 个：进入注视、扫视退出、失效/断档复位）
```

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 用位移阈值判断注视 | 依赖帧率/分辨率；角速度是物理量 |
| 一直用上一帧速度外推 | 没有新帧就继续累计注视，会把"卡住"当注视 |
| 返回世界坐标 | 没有 HMD 世界变换和眼睛原点，返回假数据比不返回更糟 |
| 单独起一个 WS 端口 | 控制面已有连接和节流，复用 `/ws` 的 `gaze` 消息即可 |
| 把估计器放进 sink | sink 是输出端，估计属于处理层；放 core/process 才能被复用 |

## 已知限制

- 左右眼方向/角度/2D 点/来源/捕获时刻都有了；逐眼置信度（`GazeSample::confidence`
  还没上抛）、瞳孔直径、逐眼注视判定仍是缺口。
- 2D-only 来源（只声明 `FGB_GAZE_POINT_2D`）的 `fixation` / `saccade` / `vergence_distance`
  仍不可用，但现在是"诚实地不可用"：`valid=false`、估计器复位、字段恒 0，
  而不是伪造 `-Z` 后永远「注视中」。要不要给 2D 点做一套点速度判定，等真实使用反馈。
- 没有注视点预测（M5.4）与世界坐标（需要头姿 + 场景深度/平面）。
- 没有 OpenXR API Layer / 眼追注入（M5.5–5.8）。
- 阈值是通用值；如果用户眼动幅度很小，注视判定会偏多，后续可做成配置/校准项。
