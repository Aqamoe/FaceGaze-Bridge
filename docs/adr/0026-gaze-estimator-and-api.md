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

- 只有合并后的方向，没有左右眼独立方向/瞳孔数据（后续需要再加字段）。
- 没有注视点预测（M5.4）与世界坐标（需要头姿 + 场景深度/平面）。
- 没有 OpenXR API Layer / 眼追注入（M5.5–5.8）。
- 阈值是通用值；如果用户眼动幅度很小，注视判定会偏多，后续可做成配置/校准项。
