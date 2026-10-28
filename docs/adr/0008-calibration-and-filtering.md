# ADR-0008 · 逐形状校准与 One Euro 滤波

- 状态：已采纳
- 日期：2026-10-29
- 影响：`params/calibration`、`core/process/one_euro`、`engine` 的新增后处理链

## 问题

同一个模块，同一个表情，在不同人脸上出来的数值差别很大：嘴张到最大可能是 0.6，
也可能是 0.95。不映射，要么永远张不开，要么一直像在喊叫。

VRCFT 把这件事交给**模块自己**——比如 PICO 模块的 `config.ini` 里有
`jaw=(floor=0.05,gain=1)`。问题是这格式每个模块都不一样，换模块就得重配一遍。

## 校准：放在核心层，从两份抓包推导

放在核心层，一次校准对所有模块生效。算法很朴素：

```
lo = 放松录制的 中位数      （用中位数不用最小值，抗单帧抖动）
hi = 夸张录制的 95 分位      （同理，躲开偶发尖峰）
out = clamp((raw - lo) / (hi - lo), 0, 1)
```

`hi - lo < 0.05` 的形状不参与映射——它在这两次录制里几乎没动，硬映射只会把噪声放大。

### 为什么不在引擎里做"实时向导"

实时向导需要在引擎循环里塞状态机、在 CLI 里做倒计时，出错面大且不好测。
抓包已经是现成的（ADR-0007），推导过程可以离线重跑、单测覆盖。用户多敲两条命令：

```bash
fgb-cli run --source vrcft --record neutral.fgbrec --duration 5   # 放松
fgb-cli run --source vrcft --record express.fgbrec --duration 5   # 尽量夸张
fgb-cli calibrate neutral.fgbrec --express express.fgbrec --out me.ini
fgb-cli run --source vrcft --calibration me.ini
```

档案是 INI，手改也方便：

```ini
[eye]
active = 1
gain_y = 1.3
center_x = 1.7243      # 中性时的视线偏置（度）
[shapes]
JawOpen = 0.0464 0.1802
```

视线单独处理：中性时减去中心偏置，再乘增益。`center` 是用户天生的眼位偏差，
实测这台机器上 y 方向偏 −5.6°，不做补偿的话"看正前方"会被识别成"向下看"。

## 滤波：One Euro，默认关

参数与 VRCFT 的 `Mutation/Filter.cs` 完全一致（`minCutoff=1.0`、`beta=0.5`、
`dCutoff=0.1`、`hz=10`），**默认也是关**——VRCFT 里它是个 `_isActive = false`
的可选 mutation，我们跟随这个默认值，否则同一份输入在两边的手感会不一致。

两个实现细节：

1. **首个样本直接透传**。不这么做的话，滤波器会从 0 开始缓慢爬升，表现为开机时
   所有表情"慢慢长出来"，很难看。
2. **`hz` 固定 10 而不是真实采样率**。数学上不严谨（数据实际约 100 Hz 到达），
   但 beta 的实际强度就建立在这个数值上。目标是行为一致，不是论文式的正确。

## 结构

引擎新增后处理链（`IFaceProcessor`），在 source 产出帧之后、发布快照之前执行：

```
source → sanitize → [校准] → [滤波] → 发布 → sinks
```

放在引擎而不是 sink，是因为它处理的是**表情数据本身**。以后加 VMC sink 时，
滤波和校准应该同样生效，而不是只有 VRChat 那一路被处理。

## 一个 C++ 细节（踩了两次）

`IniWriter` 原本有三个重载：`kv(key, string_view)`、`kv(key, double)`、`kv(key, bool)`。
`kv("JawOpen", line)` 里 `line` 是 `char*`，**指针到 bool 是标准转换、到 string_view 是
自定义转换**，于是它稳稳地调用了 bool 重载——写进去一个 `1`，编译不报错。
序列化 quietly 出错，靠往返测试才发现。

修法不是加 `static_cast`，而是把 bool 版本改名 `kv_bool`：这个类不能出现
"传字符串却写了个 1"的合法路径。

## 验证

- 67 个单元测试（新增 10 个）：滤波阶跃响应/NaN、校准映射与边界、档案往返、
  从抓包推导、静态形状被跳过、缺文件报错
- 真实抓包推导：25 秒 PICO 数据得到 29 个有效形状，`JawOpen` 区间 `0.046–0.180`，
  视线中心 `(1.72°, −5.58°)`
- `run --calibration --filter` 在回放链路上跑通，零告警

## 遗留

- 校准目前只覆盖 88 个肌肉形状和视线，**不覆盖头部姿态**
- 没有"实时预览校准效果"的界面，只能改档案再试
- `gain` 是手改的值，没有自动推导（自动推 gain 需要"期望幅度"这种主观输入）
