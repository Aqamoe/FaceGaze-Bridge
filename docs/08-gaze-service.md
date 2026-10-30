# 08 · 注视点服务（`/api/gaze` 与 WS `gaze`）

> 面向：脚本/研究工具/串流软件/自研客户端。
> 设计取舍见 [ADR-0026](adr/0026-gaze-estimator-and-api.md)。

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
  "point2d_valid": false,
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
  "head_pose": { "q": [0, 0.03, 0, 0.999], "p": [0, 0.05, 0] }
}
```

## 字段

| 字段 | 含义 |
|---|---|
| `ts_ns` | 这次估计的引擎时刻（单调时钟，纳秒）。没有新帧时保持旧值 |
| `capture_ts_ns` | 注视数据捕获时刻；0 = 未知 |
| `valid` | 有可用的合并方向（gaze 通道或眼睛） |
| `left_valid` / `right_valid` | 逐眼方向是否有效 |
| `left_dir_local` / `right_dir_local` | 逐眼 HMD 局部单位向量 |
| `point2d_valid` / `left_point_2d` / `right_point_2d` | 来源提供的 2D 屏幕点；约定尚未对齐 |
| `source` / `source_age_ms` / `source_id` / `source_mask` | 来源实例名、数据年龄、源序号、通道位掩码 |
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

### 注视判定

- 进入：角速度 < **30°/s** 且持续 >= **80 ms**；
- 退出：角速度 > **80°/s**（30–80 是迟滞区）；
- 断档 > **0.5 s** 不跨帧累计，直接退出注视。

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
- **只有合并方向**：左右眼独立方向/瞳孔字段、眨眼分类尚未加入。
- **本地回环**：控制面只绑 `127.0.0.1`，无鉴权。
- **没有新帧时 `ts_ns` 不前进**：消费者应按 `ts_ns` 判断是否过期，而不是假设 30 Hz 一定有新数据。
- **没有 OpenXR 注入**：M5.5–5.8 的 API Layer、ETFR、ALVR 对接未做。
