# 共享内存输出现场指南

FaceGaze Bridge 可以把每一帧同时写进一块命名共享内存，给同机的 Unity / UE / 自研工具读取。
协议头：[`fgb_shared_frame.h`](../fgb_shared_frame.h)；设计取舍见
[ADR-0019](https://github.com/Aqamoe/FaceGaze-Bridge/blob/main/docs/adr/0019-shared-memory-output.md)。

## 开启

```powershell
fgb-cli.exe run --source vrcft --shm fgbtest
# 或界面启动时加参数（快捷方式目标后追加）：--shm fgbtest
```

映射名是 `Local\FGB_Shm_fgbtest`。同一时间只能有一个写者；名字冲突会在引擎日志报错。

## 消费者读取（C/C++）

```cpp
#include "fgb_shared_frame.h"

HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\FGB_Shm_fgbtest");
auto* block = static_cast<const FgbSharedBlockV1*>(
    MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(FgbSharedBlockV1)));

FgbSharedFrameV1 frame;
uint64_t seq, seq2;
do {
    seq = block->sequence;
    if (seq & 1) continue;
    uint32_t slot = block->active_slot;
    if (slot >= block->slot_count) continue;
    memcpy(&frame, &block->slots[slot], sizeof(frame));
    seq2 = block->sequence;
} while (seq != seq2 || (seq2 & 1));

// frame.shapes[22] == JawOpen（顺序见 Unified Expressions）
// frame.channels 位：1=expression, 2=eyes, 4=head, 8=gaze（v1.1）
// 先检查 block->version / struct_size：v2 在末尾追加了独立注视字段
```

C# 用 `MemoryMappedFile.OpenExisting(@"Local\FGB_Shm_fgbtest")`，按相同字段顺序用
`BinaryReader` 读取；`sequence` 为奇数时重试。

## 字段速查

| 字段 | 含义 |
|---|---|
| `sequence` | 写序号；奇数表示写入中 |
| `capture_ts_ns` / `publish_ts_ns` | 原始采集时刻 / 引擎发布时刻（ns） |
| `channels` | 1=表情，2=眼追，4=头姿，8=独立注视 |
| `attributes` | 1=左眼有效，2=右眼有效，4=有头姿，8=fixation，16=渲染注视有效 |
| `left/right_openness` | 睁眼度 0..1 |
| `left/right_gaze_dir` | 头像眼睛的 HMD 局部方向，-Z 前 |
| `gaze_dir` / `gaze_pitch_deg` / `gaze_yaw_deg` | 渲染注视；有 gaze 通道时来自它，否则由眼睛反算 |
| `gaze_valid` | 1=左眼，2=右眼，4=合并方向，8=2D 点有效 |
| `gaze_source` / `gaze_source_mask` | 注视来源序号 / 本帧各通道贡献源位掩码 |
| `gaze_capture_ts_ns` | 注视数据捕获时刻；0 = 未知 |
| `gaze_left/right_dir` | 逐眼 HMD 局部单位向量 |
| `gaze_left/right_point_2d` | 归一化屏幕点（暂定 [0,1] 左上；约定待对齐） |
| `head_rotation/position` | 头姿，四元数 + 米 |
| `shapes[88]` / `shape_valid[88]` | Unified Expressions 形状与有效位 |

`version` 目前是 **2**；追加字段只加在末尾，旧字段偏移不变。

## 调试

```powershell
python tools/probe_shm.py fgbtest 3
```

会打印 magic/version、帧序号、通道、左眼睁开度、JawOpen 与注视来源，用来确认写端和理解布局。
