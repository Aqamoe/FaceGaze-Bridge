# ADR-0014 · VMC 输出：让同一帧也能驱动不是 VRChat 的程序

- 状态：已采纳
- 日期：2026-10-29
- 影响：新增 `vmc/vmc_map`（名字表共享）、`sinks/vmc_sink`、CLI `--vmc-out`、`status.vmc_out`、输出页一行、`tools/probe_vmc_out.py`；`sources/vmc_source` 改用共享表

## 问题

到这一步 FGB 只有一条出路：VRChat 的 OSC 参数。但表情数据的消费方不止 VRChat——
VSeeFace / Warudo / Unity 场景 / 自制工具链都认 VMC 协议，而 VMC 恰好是我们已经在用的输入协议。

于是两个具体痛点：

1. 同一张脸没法同时驱动 VRChat 里的模型和直播画面里的 Unity 角色（用户得开两套面捕）
2. 想把 FGB 当"上游"接到自己的管线里，没有任何出口

## 决定

### 1. 加一个 VmcSink，与 VrchatSink 并列

`sinks/sink.hpp` 的注释里早就写了"每个 Sink 独立节流：VRChat 要 90 Hz，VMC 要 60 Hz"，
现在把这句话兑现：

| 项 | 取值 | 理由 |
|---|---|---|
| 频率 | 60 Hz（`min_interval_ns`） | VMC 消费方多是 60 fps 的 Unity 场景 |
| 死区 | 0.005 | 与 VRChat 侧同量级；VMC 是浮点，没必要为噪声发包 |
| 心跳 | 1 s 全量重发 | 接收端可能在我们启动之后才起来，只发变化等于永远不同步 |
| 分包 | ≤1200 字节 | 与 VRChat 侧一致，避免 IP 分片 |
| 默认 | **关闭** | 多播一路 UDP 是用户显式要求的行为，不该悄悄发生 |

### 2. 只发协议里真正约定的三条消息

```
/VMC/Ext/Blend/Val  (name: string, value: float)   × N
/VMC/Ext/Root/Pos   (name: string, px,py,pz, qx,qy,qz,qw)   可选
/VMC/Ext/Blend/Apply ()                             ← 必须在最后一包
```

不发 `/VMC/Ext/Set/*`、不做 OSCQuery、不做 TCP sender：那些是"更完整的 VMC"，但当前没有
可验证的消费方，写了也没人试。

`Apply` 必须跟在最后——分包时它属于最后一个 bundle，否则接收端会拿着一半数据提交。

### 3. 名字表只有一份

输入侧（`sources/vmc_source`）是"名字 → 多个肌肉形状"，输出侧是"肌肉形状 → 名字"。
两边各维护一份必然漂移，症状是"能收不能发"或者对方不认名字，而且看起来像对方的问题。
所以把表搬到 `vmc/vmc_map`（一个 40 行的小库），两侧都 include 它。

聚合规则：一个名字映射到多个形状时（`browInnerUp` → 左右两条，`mouthFunnel` → 四条）取**最大值**。
输入侧是"一个名字同时推这几个形状"，输出时取最大才能保证往返一圈不比原来弱；
取平均会把脸"压平"，取第一条会让双侧形状丢一半。

### 4. 眼睛与头姿也照协议发

- 眼睛：VMC 没有单独的"gaze"地址，惯例是用 `eyeBlinkLeft/Right`。所以发 `1 - openness`，
  只在帧带 `channel::kEyes` 时发。
- 头姿：`/VMC/Ext/Root/Pos`，名字 `HMD`。坐标要转回 Unity 左手系（z 取反，四元数同理），
  与输入侧的换算互逆——这条有单测钉住，因为符号写反了在 VR 里看起来"头反了"但很难定位。

### 5. 界面与诊断

`status.vmc_out` = `{enabled, target, messages, bytes}`，输出页多一行
`VMC 输出  127.0.0.1:9100 · 4,906`（未开时显示"未启用"）。
`tools/probe_vmc_out.py` 按接收端视角收包，检查 Blend/Val、Apply、Root/Pos 与值抽样。

## 验证

- 单测 3 个（共 100 个用例）：
  - `vmc_map_one_name_can_drive_several_shapes`：`browInnerUp` 同时推左右两条
  - `vmc_sink_emits_bundle_with_apply_and_head`：真的绑一个 UDP 端口收包，用 OSC 解码器
    逐条检查 `jawOpen=0.5`、`Apply` 存在、`Root/Pos` 的 z 为 `-0.3`（Unity 系）、眨眼值 > 0
  - `vmc_sink_throttles_and_skips_unchanged`：节流生效、内容没变不重复发
- 实跑：`--source replay --vmc-out 127.0.0.1:9100`，探针 5 秒收到 173 个数据报、
  154 次 Apply（≈ 回放帧率 31 Hz）、42 个名字里 38 个非零；`/api/status` 的 `vmc_out.messages` 同步增长
- 界面：输出页显示目标与条数；`tools/probe_ui.py` 增加断言（开启时行内必须含目标地址）

## 代价与遗留

- VMC 没有能力声明，接收端不知道我们只发 ARKit 子集（42 个名字）；这是协议本身的问题。
- 只发 blendshape 与头姿，不发骨骼/手指（`/VMC/Ext/Bone/Pos`），FGB 也没有那些数据。
- 与 VMC **输入**共用 39539 端口：同一台机器上同时开 `--source vmc` 和 `--vmc-out 39539`
  会自己吃自己（回环）。这不是 bug，但文档里要写清楚——默认端口给了 39539，是为了对齐
  VSeeFace 的默认监听；实际联调建议显式给一个别的端口。
- 共享内存 Sink 仍没做：没有明确的消费方契约，先不写。
