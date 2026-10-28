# ADR-0007 · 录制回放与参数导出

- 状态：已采纳
- 日期：2026-10-29
- 影响：`capture/`、`sources/replay_source`、`fgb-cli export`

## 问题

每次改参数层都要"戴头显 → 关 VRCFT → 肉眼确认"，成本高且结论只有"动了/没动"一档。
ADR-0006 那次验收就是这样：只知道表情会动，不知道幅度对不对、有没有哪一版改坏了。

调表情本质是**改系数**，而系数的问题只有对比才看得出来。

## 决策

### 录原始字节，不录解码后的帧

抓包文件里存的是 `ReplyUpdate` 的 body 原样（`structSize` + 420 字节），不是 `FaceFrame`。

理由：如果录解码后的帧，解码层的 bug 会被永久性地"录进文件里"，以后再也复现不出来。
回放时走**完全相同的** `decode_update` + `apply_to_frame`，测的才是真实链路。

文件布局（小端）：

```
"FGBREC01"            8 字节 magic
version               u32
payload_size          u32   (424)
reserved              u64
---- 记录序列 ----
timestamp_ns          u64   相对首帧
payload               424 字节
```

25 秒真实数据 ≈ 2943 帧 / 1.27 MB，约 430 字节/帧。

### 三个工具

| 命令 | 作用 |
|---|---|
| `fgb-cli run --source vrcft --record x.fgbrec` | 边跑边录 |
| `fgb-cli run --source replay --replay x.fgbrec` | 按原始时间戳回放（`--replay-speed` 可加速） |
| `fgb-cli export x.fgbrec --out p.csv` | 逐帧跑过参数层，导出参数值矩阵 |

`export` 是关键：表头是参数逻辑名（`v2/JawOpen`），每行一帧。改完参数层跑两遍、
diff 两个 CSV，就知道动了哪个参数、动了多少。**不需要头显。**

列只包含"真的会发出去"的参数（判据是有任何发送目标，而不是有主地址——纯二进制参数
没有主地址，漏掉它们会让 diff 少掉大半张嘴的参数，这个坑已经踩过一次）。

### 回放的取舍

回放按录制时间戳节流，`poll()` 一 tick 可能消费多帧、只保留最后一帧的状态。
所以**回放不是逐帧精确的**：引擎 120 Hz、录制约 118 Hz，两者不同步时会丢帧。
需要逐帧精确时用 `export`——它不经过引擎，直接顺序解码。

## 验证

- `tools/check_binary.py` 同款思路的 `check_replay`：回放 25 秒抓包，收到 156 个地址、
  25 个非零 float（`EyeLidLeft = 0.54`、`JawOpen = 0.019`…），与录制时一致
- 57 个单元测试，其中 5 个覆盖录制读写、坏文件拒绝、回放解码

## 遗留

- **没有和 VRCFT 的输出做逐参数对齐**。要做得先抓 VRCFT 发给 VRChat 的 OSC
  （ADR-0001 那种抓法），再和我们的 CSV 按参数名对齐。这是"能不能替代 VRCFT"
  的最后一道量化关口
- 抓包不含 OSC 侧信息（只有模块数据），所以它衡量的是"参数层"，不是"发送层"
