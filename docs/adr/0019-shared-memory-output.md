# ADR-0019 · 共享内存输出（M4.2）

- 状态：已采纳
- 日期：2026-10-29
- 影响：新增 `sdk/fgb_shared_frame.h`、`sinks/shm_sink`、`--shm <name>`、输出页「共享内存」行、
  `tools/probe_shm.py`

## 问题

VRChat 走 OSC/UDP，VMC 走 UDP；本机的 Unity/UE/自研工具如果再走一遍网络栈，
会多出序列化、系统调用和调度抖动。M4 要求给同机消费者一条低延迟、零协议开销的通道。

## 决定

### 1. 命名映射 + 双缓冲 + seqlock

- 映射名：`Local\FGB_Shm_<name>`，由 `--shm <name>` 显式开启；默认不建映射。
- 布局在 `sdk/fgb_shared_frame.h`（纯 C，跨语言可读）：
  - 块头：magic / version / struct_size / slot_count / sequence / active_slot
  - 两个 `FgbSharedFrameV1` 槽，包含时间戳、通道/属性位、双眼、注视点、头姿、88 形状 + valid
- 写入：取非活动槽 → `sequence++`（奇数）→ 写整帧 → `sequence++`（偶数）→ 翻转 `active_slot`。
- 读取（消费者）：
  ```
  do { seq = block->sequence; if (seq & 1) continue;
       slot = block->active_slot;
       memcpy(&frame, &block->slots[slot], sizeof(frame));
       seq2 = block->sequence;
  } while (seq != seq2 || (seq2 & 1));
  ```
  允许读到旧帧，不允许读到半帧；消费者不需要锁，也不会阻塞引擎。

### 2. 每帧全量写，不在 Sink 里做节流

共享内存给的是“最新状态”，消费者按自己的刷新率读即可。默认每个引擎 tick 写一次
（120 Hz 时约 120 × 600 B ≈ 70 KB/s，可忽略）。`ShmSinkConfig::min_interval_ns`
预留给需要限频的场景，`--shm` 先用 0（每帧）。

### 3. 与模块 ABI 共用语义

- `channels` 位与模块 ABI 的 `FGB_CAP_*` 一致（eye / expression / head）。
- 形状顺序与 Unified Expressions 一致；`shape_valid` 显式标出本帧哪些形状有效。
- 属性位标出左右眼有效、是否有头姿、是否有 fixation（M5 使用）。

### 4. 只做同机、本会话

`Local\` 前缀表示同一登录会话；不跨用户、不跨会话。名字冲突时 `CreateFileMappingW`
会失败，日志给出错误码，不会静默复用别人的映射。

## 代价

- 写者每次 tick 拷贝约 600 B 到映射；相比 OSC/UDP 的编码+系统调用仍低一个量级。
- 消费者必须实现 seqlock 重试；只读一次可能拿到旧帧（旧帧是完整帧，不会错位）。
- 双缓冲占 ~1.2 KB，可忽略；没有做多读者队列/事件通知，读者自行轮询。
- 共享内存是“当前状态”，不做历史；需要逐帧不丢数据时用 `.fgbcap` 录制的思路另做 ring。

## 验证

- 单测：写一帧后按消费者视角映射读回，校验 magic/version/sequence 偶数、
  通道、JawOpen、眼睛 openness、注视角度与时间戳；stop 后映射不可再打开。
- 端到端：`fgb-cli run --source replay --replay golden.fgbrec --replay-loop --shm fgbtest`
  + `python tools/probe_shm.py fgbtest 3`，seq 连续递增、通道/形状值正常。
