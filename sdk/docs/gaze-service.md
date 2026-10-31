# 08 · 注视点服务（`/api/gaze` 与 WS `gaze`）

> 面向：脚本/研究工具/串流软件/自研客户端。
> 设计取舍见 [ADR-0026](https://github.com/Aqamoe/FaceGaze-Bridge/blob/main/docs/adr/0026-gaze-estimator-and-api.md)。

FGB 优先使用独立注视通道（`GAZE`），没有时把左右眼合并成一条 HMD 局部视线，
并在引擎里判断"扫视 / 注视"。头像驱动不使用这份数据，只有渲染/foveation 消费端用。
控制面（`--panel` 或 `--ui`）起来后，有两个入口拿到同一份数据：

| 入口 | 方式 | 频率 |
|---|---|---|
| `GET /api/gaze` | curl / HTTP | 请求时快照 |
| 控制面 WebSocket `/ws` | `{"type":"gaze"}` 文本帧 | 30 Hz，连接时立即补一帧 |

```powershell
fgb-cli run --source vrcft --panel 8787
curl http://127.0.0.1:8787/api/gaze
```

```json
{
  "type": "gaze",
  "ts_ns": 1284733000000,
  "capture_ts_ns": 1284732900000,
  "valid": true,
  "origin_local": [0, 0, 0],
  "dir_local": [0.02, 0.11, -0.99],
  "left_valid": true,
  "right_valid": true,
  "left_dir_local": [0.03, 0.10, -0.99],
  "right_dir_local": [0.01, 0.10, -0.99],
  "left_pitch_deg": 5.7,
  "left_yaw_deg": 3.9,
  "right_pitch_deg": 5.7,
  "right_yaw_deg": 2.1,
  "point2d_valid": false,
  "left_point_valid": false,
  "right_point_valid": false,
  "point_space": "source",
  "left_point_2d": [0, 0],
  "right_point_2d": [0, 0],
  "pitch_deg": 5.0,
  "yaw_deg": 1.0,
  "vergence_distance": 1.24,
  "confidence": 0.93,
  "fixation": true,
  "fixation_ms": 492.0,
  "saccade_vel_deg_s": 2.3,
  "source": "native:gaze",
  "source_age_ms": 8,
  "source_id": 2,
  "source_mask": 9,
  "left_source": 2,
  "right_source": 2,
  "left_capture_ts_ns": 1284732900000,
  "right_capture_ts_ns": 1284732900000,
  "head_pose": { "q": [0, 0.03, 0, 0.999], "p": [0, 0.05, 0] }
}
```

## 字段

| 字段 | 含义 |
|---|---|
| `ts_ns` | 这次估计的引擎时刻（单调时钟，纳秒）。没有新帧时保持旧值 |
| `capture_ts_ns` | 注视数据捕获时刻；0 = 未知 |
| `valid` | 有可用的合并方向（gaze 通道或眼睛）。**只给 2D 点的来源为 `false`** |
| `left_valid` / `right_valid` | 逐眼方向是否有效 |
| `left_dir_local` / `right_dir_local` | 逐眼 HMD 局部单位向量 |
| `left_pitch_deg` / `left_yaw_deg` / `right_pitch_deg` / `right_yaw_deg` | 逐眼俯仰/偏航（与 `dir_local` 同一约定）；无效眼为 0，先看 `left_valid` / `right_valid` |
| `left_point_valid` / `right_point_valid` | 逐眼 2D 点是否有效（与方向有效**无关**；2D-only 源只有这个为真） |
| `point_space` | 点所在坐标系：`"viewport"` = 该眼视口归一化（可用于 foveation）；`"source"` = 来源自己的原始坐标，只能展示/看趋势 |
| `point2d_valid` / `left_point_2d` / `right_point_2d` | 该眼视口归一化坐标（[ADR-0033](https://github.com/Aqamoe/FaceGaze-Bridge/blob/main/docs/adr/0033-gaze-point-2d-space.md)）：`[0,1]`、左上原点、畸变前、逐眼独立；`point2d_valid` 是"左右眼都有点"的兼容字段 |
| `source` / `source_age_ms` / `source_id` / `source_mask` | 来源实例名、数据年龄、源序号、通道位掩码 |
| `left_source` / `right_source` | 逐眼来源序号（左右注视可来自不同源） |
| `left_capture_ts_ns` / `right_capture_ts_ns` | 逐眼捕获时刻 |
| `origin_local` | 眼睛原点；当前固定 `[0,0,0]`（没有建模，见"能力边界"） |
| `dir_local` | HMD 局部单位向量，右手系，-Z 前 |
| `pitch_deg` / `yaw_deg` | 俯仰（正 = 上）/ 偏航（正 = 右） |
| `vergence_distance` | 聚散距离（米）；0 = 来源不提供 |
| `confidence` | 眼追置信度 0..1 |
| `fixation` | 是否处于注视 |
| `fixation_ms` | 当前注视已持续多久 |
| `saccade_vel_deg_s` | 相邻帧角速度；扫视时可达数百°/s |
| `head_pose` | 仅当这一帧有头姿：`q` = [x,y,z,w]，`p` = 米 |

### 来源选择

- 合并层有独立 `Gaze` 通道时优先用它；没有时回退到 `Eyes` 反算。
- 默认按注册顺序选第一个“新鲜且提供该通道”的源；`--gaze-source <源名>`
  或界面「模块 → 渲染注视」可显式指定。
- 注视通道默认 **100 ms** 过期（`--gaze-stale-ms` 可改），比表情/眼睛更短；
  消费端应检查 `source_age_ms`，过期数据不要用于 foveation。

### 2D 注视点坐标（ADR-0033）

`left_point_2d` / `right_point_2d` 是**该眼显示视口**的归一化坐标：

```text
(0,0) ─────────── x ──────────► (1,0)
  │
  y      左眼、右眼各自一块视口（按眼分开）
  ▼
(0,1)                          (1,1)
```

- `x/y ∈ [0,1]`、原点左上、+x 向右、+y 向下；**畸变前**的采样空间，逐眼独立；
- **只有 `point_space="viewport"` 才是这个规范空间**。模块没声明 `POINT_VIEWPORT` 时
  `point_space="source"`：数值是来源自己的归一化原始坐标（相机图像/私有协议），
  可以用来看两眼差异和趋势，**不要**当成屏幕位置去做 foveation；
- SHM/UDP 用 `gaze_valid` 的 `FGB_*_GAZE_POINT_VIEWPORT` 位（0x10）表达同一件事；
- 映射由来源模块负责（它才知道内参/镜像/换眼/分辨率），FGB 不做像素/NDC/反投影换算；
- 越界不是错误（看向画外、标定误差），FGB **保留原值不裁剪**；`NaN`/`Inf` 会把该眼的
  点标为无效，另一只眼不受影响；
- 换到别的坐标：`ndc = (2x-1, 1-2y)`、像素 `= (x·W, y·H)`，视口尺寸由消费端掌握；
- 眼动相机图像坐标**不是**这个空间，模块必须自己标定后再报。

### 注视判定

- 进入：角速度 < **30°/s** 且持续 >= **80 ms**；
- 退出：角速度 > **80°/s**（30–80 是迟滞区）；
- 断档 > **0.5 s** 不跨帧累计，直接退出注视。
- 没有方向的来源（只给 2D 点）不参与判定：`valid` 为假，状态直接清空、不累计。

## 最小客户端

### HTTP

```python
import json, urllib.request

with urllib.request.urlopen("http://127.0.0.1:8787/api/gaze", timeout=2) as r:
    g = json.load(r)
print(g["valid"], g["fixation"], g["pitch_deg"], g["yaw_deg"], g["vergence_distance"])
```

### WebSocket

与控制面同一个连接；只挑 `type == "gaze"` 的文本帧。现成例子：
`tools/probe_ipc.py`（手写握手 + 帧解码，无第三方库）。

## 能力边界

- **局部坐标，不是世界注视点**：没有 HMD 世界变换、眼睛原点和场景深度，
  所以不返回 `origin_world` / `target_world`。需要世界坐标时由客户端拿头姿自己做。
- **没有预测**：M5.4 的速度外推未实现；管线延迟补偿还没有。
- **逐眼数据不全**：左右眼方向/角度/2D 点/来源与捕获时刻已提供；
  逐眼置信度、瞳孔直径、逐眼注视判定、眨眼分类尚未加入。
  控制面状态页把左右眼分行显示，单眼失效时该行为 `—`。
- **只给 2D 点的来源**（`FGB_ATTR_LEFT/RIGHT_POINT`，没有方向模型的原生模块）：
  帧会被正常接收，`point2d_valid` / `left_point_valid` / `right_point_valid` 为真，
  而 `valid` / `left_valid` / `right_valid` 为假——**没有方向就不返回方向**，
  `pitch_deg` / `yaw_deg` 保持 0 且不应被消费。
  这类来源没有角速度可算，`fixation` / `fixation_ms` / `saccade_vel_deg_s` 恒为假/0，
  `vergence_distance` 也是 0；消费端看 `point2d_valid` 而不是 `fixation`。
  状态页对它们**优先显示 2D 点**（坐标约定见上文 §2D 注视点坐标），2D 框也按点画。
- **方向与 2D 点在 SDK 层是两条独立属性**（`FGB_ATTR_LEFT/RIGHT/COMBINED_GAZE` 与
  `FGB_ATTR_LEFT/RIGHT_POINT`），解析层也按这个约定走（`sources/native_gaze_parse.hpp`）。
  **模块只给点时就只置点位属性**：伪造一个 `-Z` 方向会让
  `valid` 变真、`fixation` 永远「注视中」，第三方还会拿到假方向。
- **本地回环**：控制面只绑 `127.0.0.1`，无鉴权。
- **没有新帧时 `ts_ns` 不前进**：消费者应按 `ts_ns` 判断是否过期，而不是假设 30 Hz 一定有新数据。
- **没有 OpenXR 注入**：M5.5–5.8 的 API Layer、ETFR、ALVR 对接未做。
