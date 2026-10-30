# 07 · 流式输出（WebSocket JSON / UDP 二进制）

> 面向：网页/脚本开发者、串流工具作者、自研引擎集成者。
> 共享内存输出另见 [06-shared-memory.md](06-shared-memory.md)；VRChat/VMC 输出见 README。

FGB 的同一帧可以同时送给多个消费者。除 VRChat OSC、VMC、共享内存外，还有两种
"给程序读"的流式输出，默认都不开启，必须显式给参数：

| 输出 | 开启方式 | 传输 | 适合 |
|---|---|---|---|
| WebSocket JSON | `--ws-out <端口>` | 本机 TCP 回环 | 网页、Node/Python 脚本、调试面板 |
| UDP 二进制 | `--udp-out <host:port>` | UDP 一个数据报一帧 | 串流软件、自研客户端、最低延迟 |

```powershell
# 同时开两种；WS 30 Hz，UDP 每个引擎 tick（120 Hz）
fgb-cli run --source vrcft --ws-out 9101 --udp-out 127.0.0.1:9102

# 只开 WS，改 60 Hz，并发送全部 88 个形状
fgb-cli run --source vrcft --ws-out 9101 --ws-out-rate 60 --ws-out-all
```

两者都是**全量帧**：即使某个形状这一帧没变也会带上（UDP 由 `shape_valid` 标记，
WS 默认只发有效且 > 0.01 的形状）。接收端不需要维护上一帧状态。

**设置可以持久化**：界面「输出」页可以直接改这些目标/开关，保存到
`%LOCALAPPDATA%\FaceGazeBridge\outputs.ini`，点「保存并重启引擎」生效。
优先级是 **命令行参数 > outputs.ini > 默认值**；某次启动想忽略保存值就加
`--no-saved-outputs`，想用另一份文件就加 `--outputs-config <路径>`。
详见 [ADR-0025](adr/0025-output-settings-persistence.md)。

---

## 1. WebSocket JSON

### 连接

```
ws://127.0.0.1:<port>/ws
```

只绑回环，无鉴权、无 TLS。`GET http://127.0.0.1:<port>/` 返回一个小的服务信息 JSON，
可以用来探测端口是否在监听。

### 帧格式

每帧一个文本帧，内容是一个 JSON 对象：

```json
{
  "t": 1284733,
  "seq": 88421,
  "src": "vrcft",
  "eye": {
    "left":  { "open": 0.92, "gaze": [0.02, 0.11, -0.99], "pupil": 4.8, "ok": 1 },
    "right": { "open": 0.90, "gaze": [0.01, 0.10, -0.99], "pupil": 4.7, "ok": 1 },
    "vergence": 1.24
  },
  "gaze": {
    "ok": 1,
    "left":  { "ok": 1, "dir": [0.03, 0.10, -0.99], "point_ok": 1, "point": [0.42, 0.61] },
    "right": { "ok": 1, "dir": [0.01, 0.10, -0.99], "point_ok": 1, "point": [0.45, 0.58] },
    "dir": [0.02, 0.10, -0.99],
    "ts_ns": 1284733000000,
    "src": 2,
    "mask": 9,
    "point_space": "viewport"
  },
  "head": { "q": [0, 0.03, 0, 0.999], "p": [0, 0.05, 0] },
  "shapes": { "JawOpen": 0.31, "MouthSmileLeft": 0.44, "BrowInnerUp": 0.12 }
}
```

| 字段 | 含义 |
|---|---|
| `t` | 引擎发布时刻，毫秒（单调时钟） |
| `seq` | 本次连接建立后 FGB 的发送序号；只在真正广播时递增 |
| `src` | 输入源标签（`vrcft`、`vmc`、`replay` 或合并后的名字） |
| `eye.left/right` | 头像眼睛：`open` 0..1、`gaze` HMD 局部单位向量、`pupil` 毫米（0=不提供）、`ok` 1/0 |
| `eye.vergence` | 聚散距离（米）；没有有效眼追时为 0 |
| `gaze` | 渲染注视（v1.1）：优先独立 `GAZE` 通道，没有时由眼睛反算；`ok` 1/0 |
| `gaze.left/right` | 逐眼 `ok`、`dir`、`point_ok`、`point`（归一化屏幕点，约定待对齐） |
| `gaze.ts_ns` / `gaze.src` / `gaze.mask` | 捕获时刻 / 来源序号 / 各通道贡献源位掩码 |
| `head` | 仅当这一帧有头姿时出现：`q` = [x,y,z,w]，`p` = 米 |
| `shapes` | 形状名 → 值（0..1）。默认只含有效且 > 0.01 的；`--ws-out-all` 发全部 88 个 |

坐标系与共享内存/模块 ABI 一致：右手系，+Y 上，-Z 前，单位米。

### 频率与背压

- 默认 30 Hz（`--ws-out-rate <hz>` 可改；0 = 每个 tick）。
- 支持多个客户端同时连接；每帧向所有客户端广播。
- 某个客户端不读、出站缓冲超过 512 KB 时会被断开；重连即可，服务端不保留历史。
- 没有客户端时不做 JSON 编码，也不计入 `messages`。
- 端口被占用时只关闭 WS 这一路，引擎继续运行；状态里 `ws_out.listening=false`，
  输出页会显示「未监听（端口被占用？）」。

### 最小客户端（Python）

```python
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:9101/ws") as ws:
        while True:
            frame = json.loads(await ws.recv())
            print(frame["seq"], frame["eye"]["left"]["open"], frame["shapes"].get("JawOpen"))

asyncio.run(main())
```

---

## 2. UDP 二进制

### 传输

一个 UDP 数据报 = 一帧，固定 **656 字节**（v2 起追加注视字段），小端，1 字节对齐。默认每个引擎 tick
发送（`--udp-out-rate <hz>` 可限速，0 = 每 tick）。不做分片、不重传；丢帧就等下一帧。

### 布局

C 定义在 [`sdk/fgb_udp_frame.h`](../sdk/fgb_udp_frame.h)，安装包在 `sdk/fgb_udp_frame.h`。
字段偏移：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | u32 | `magic` = `0x55424746`（"FGBU" 小端） |
| 4 | u16 | `version` = 2 |
| 6 | u16 | `header_size` = 8 |
| 8 | u64 | `sequence`（引擎发布序号） |
| 16 | u64 | `capture_ts_ns` |
| 24 | u64 | `publish_ts_ns` |
| 32 | u32 | `channels`（1=表情 2=眼追 4=头姿 8=独立注视） |
| 36 | u32 | `attributes`（见下） |
| 40 | u32 | `shape_count` = 88 |
| 44 | u32 | `reserved` |
| 48 | f32×6 | 左眼：open、open_conf、gaze[3]、gaze_conf |
| 72 | f32×6 | 右眼：同上 |
| 96 | f32 | `vergence_distance_m` |
| 100 | f32×3 | 合并注视方向 `gaze_dir[3]` |
| 112 | f32 | `gaze_pitch_deg`（正 = 向上） |
| 116 | f32 | `gaze_yaw_deg`（正 = 向右） |
| 120 | f32 | `gaze_confidence` |
| 124 | f32×4 | `head_rotation` = [x,y,z,w]（无头姿时为单位四元数） |
| 140 | f32×3 | `head_position`（米） |
| 152 | f32×88 | `shapes[88]`（0..1，顺序见下） |
| 504 | u8×88 | `shape_valid[88]`（0/1） |
| 592 | u32 | `gaze_valid`（1=左眼 2=右眼 4=合并 8=2D 点 16=点在视口坐标，见 ADR-0033） |
| 596 | u32 | `gaze_source`（源序号，0=未知/眼睛反算） |
| 600 | u32 | `gaze_source_mask`（各通道贡献源位掩码） |
| 604 | u32 | `gaze_reserved` |
| 608 | u64 | `gaze_capture_ts_ns` |
| 616 | f32×3 | `gaze_left_dir` |
| 628 | f32×3 | `gaze_right_dir` |
| 640 | f32×2 | `gaze_left_point_2d` |
| 648 | f32×2 | `gaze_right_point_2d` |

`attributes` 位：

| 位 | 含义 |
|---|---|
| `0x01` | 左眼有效 |
| `0x02` | 右眼有效 |
| `0x04` | 这一帧有头姿 |
| `0x08` | 注视处于 fixation |
| `0x10` | 渲染注视 `gaze_*` 有效 |

形状顺序与共享内存、`core/model/shape.hpp` 的 `kShapeNames` 一致（Unified Expressions，
88 项；例如 `JawOpen` 是索引 22）。没有名字表，消费者按固定顺序读。

### 最小客户端（Python）

```python
import socket, struct

FMT = "<IHHQQQIIII" + "f" * 26 + "f" * 88 + "B" * 88 + "IIIIQ" + "f" * 10
assert struct.calcsize(FMT) == 656

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 9102))
while True:
    packet, _ = sock.recvfrom(2048)
    v = struct.unpack(FMT, packet)
    seq, attrs, shapes = v[3], v[7], v[36:124]
    print(seq, hex(attrs), shapes[22])  # JawOpen
```

C/C++ 直接 `recvfrom()` 后 `memcpy` 到 `FgbUdpFrameV1`，检查 `magic/version` 即可；
`static_assert` 已保证结构体大小与偏移。

---

## 3. 探针

```powershell
# WS：握手、字段、实际帧率
python tools/probe_ws_out.py 9101 5

# UDP：报文长度、magic/version、属性位、非零形状抽样
python tools/probe_udp_out.py 9102 5
```

没有硬件时用回放：

```powershell
fgb-cli run --source replay --replay .workbuddy/tmp/golden.fgbrec --replay-loop ^
    --ws-out 9101 --udp-out 127.0.0.1:9102 --vrchat 127.0.0.1:65500
```

`--vrchat` 指到一个没人监听的端口，避免测试数据动到正在用的 VRChat 头像。

---

## 4. 能力边界

- **本机/局域网信任模型**：WS 只绑回环，但无鉴权；UDP 目标由用户给，默认关闭。
- **不保证送达**：UDP 丢包是正常的；WS 慢客户端会被断开。需要可靠传输请用共享内存或自己加应用层协议。
- **帧是快照，不是增量**：接收端不需要维护状态；代价是每帧都带 88 个形状（UDP 约 656 字节）。
- **形状顺序是契约**：增删形状要升 `version`；消费者应检查 `magic/version`。
- **不做历史回放/录制**：需要回放用 `.fgbcap`（`--record` / `--replay`）。
