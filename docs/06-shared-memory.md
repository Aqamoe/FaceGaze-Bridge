# 共享内存输出现场指南

FaceGaze Bridge 可以把每一帧同时写进一块命名共享内存，给同机的 Unity / UE / 自研工具读取。
协议头：[`sdk/fgb_shared_frame.h`](../sdk/fgb_shared_frame.h)；设计取舍见
[ADR-0019](adr/0019-shared-memory-output.md)。

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
// frame.channels 位：1=eye, 2=expression, 4=head
```

C# 用 `MemoryMappedFile.OpenExisting(@"Local\FGB_Shm_fgbtest")`，按相同字段顺序用
`BinaryReader` 读取；`sequence` 为奇数时重试。

## 字段速查

| 字段 | 含义 |
|---|---|
| `sequence` | 写序号；奇数表示写入中 |
| `capture_ts_ns` / `publish_ts_ns` | 原始采集时刻 / 引擎发布时刻（ns） |
| `channels` | 1=眼追，2=表情，4=头姿 |
| `attributes` | 1=左眼有效，2=右眼有效，4=有头姿，8=fixation（M5） |
| `left/right_openness` | 睁眼度 0..1 |
| `left/right_gaze_dir` | HMD 局部单位向量，-Z 前 |
| `gaze_*` | 合并后的注视点（方向/俯仰/偏航/置信度） |
| `head_rotation/position` | 头姿，四元数 + 米 |
| `shapes[88]` / `shape_valid[88]` | Unified Expressions 形状与有效位 |

## 调试

```powershell
python tools/probe_shm.py fgbtest 3
```

会打印 magic/version、帧序号、通道、左眼睁开度与 JawOpen，用来确认写端和理解布局。
