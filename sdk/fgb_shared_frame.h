/*
 * FaceGaze Bridge 共享内存输出（v1）
 * ==================================
 *
 * 用途：同一帧数据以零拷贝方式给本机其它进程（Unity / UE / 自研工具）读取，
 * 延迟低于 OSC/UDP，且不需要网络栈。
 *
 * 映射名：Local\FGB_Shm_<name>，name 由 fgb-cli --shm <name> 指定。
 * 大小：sizeof(FgbSharedBlockV1)，导出时用 --shm-name 对齐。
 *
 * 读法（seqlock，允许读到旧帧，但不允许读到半帧）：
 *
 *   do {
 *     seq = block->sequence;          // 奇数 = 正在写
 *     if (seq & 1) continue;
 *     slot = block->active_slot;
 *     memcpy(&frame, &block->slots[slot], sizeof(frame));
 *     seq2 = block->sequence;
 *   } while (seq != seq2 || (seq2 & 1));
 *
 * 坐标系与 ABI 相同：右手系，+Y 上，-Z 前，单位米。
 * 结构字段只允许末尾追加；版本号变化时同步修改本文档。
 */
#ifndef FGB_SHARED_FRAME_H
#define FGB_SHARED_FRAME_H

#include <stdint.h>

#define FGB_SHM_MAGIC 0x53424746u /* "FBGS" little-endian */
#define FGB_SHM_VERSION 2u
#define FGB_SHM_MAX_SHAPES 88u
#define FGB_SHM_SLOTS 2u

#define FGB_SHM_ATTR_LEFT_VALID 0x1u
#define FGB_SHM_ATTR_RIGHT_VALID 0x2u
#define FGB_SHM_ATTR_HAS_HEAD 0x4u
#define FGB_SHM_ATTR_FIXATION 0x8u
#define FGB_SHM_ATTR_GAZE_VALID 0x10u

/* FgbSharedFrameV1::gaze_valid 的位（ADR-0030 v1.1 追加） */
#define FGB_SHM_GAZE_LEFT_VALID 0x1u
#define FGB_SHM_GAZE_RIGHT_VALID 0x2u
#define FGB_SHM_GAZE_COMBINED_VALID 0x4u
#define FGB_SHM_GAZE_POINT_2D_VALID 0x8u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgbSharedFrameV1 {
    uint64_t sequence;       /* 引擎发布序号，单调递增 */
    uint64_t capture_ts_ns;  /* 数据在物理世界发生的时刻 */
    uint64_t publish_ts_ns;  /* 引擎发布时刻 */

    uint32_t channels;       /* 内部通道位：1=表情 2=眼追 4=头姿 8=注视 */
    uint32_t attributes;     /* FGB_SHM_ATTR_* 位 */
    uint32_t shape_count;    /* <= FGB_SHM_MAX_SHAPES */
    uint32_t reserved;

    float left_openness;
    float left_openness_confidence;
    float left_gaze_dir[3];
    float left_gaze_confidence;
    float right_openness;
    float right_openness_confidence;
    float right_gaze_dir[3];
    float right_gaze_confidence;

    float vergence_distance_m;

    /* 合并后的注视点（M5 使用；没有有效眼追时 valid=0） */
    float gaze_dir[3];
    float gaze_pitch_deg;
    float gaze_yaw_deg;
    float gaze_confidence;

    float head_rotation[4]; /* x,y,z,w */
    float head_position[3]; /* 米 */

    float shapes[FGB_SHM_MAX_SHAPES];        /* 0..1 */
    uint8_t shape_valid[FGB_SHM_MAX_SHAPES]; /* 0/1 */

    /* ---- v1.1 追加：独立注视通道（渲染/foveation 用，不驱动头像眼睛）----
     * 旧字段 gaze_dir / gaze_pitch_deg / gaze_yaw_deg 仍然有效，语义变为“渲染注视”：
     * 有 gaze 通道时来自它，否则由眼睛反算。 */
    uint32_t gaze_valid;       /* FGB_SHM_GAZE_* 位 */
    uint32_t gaze_source;      /* 源序号，0 = 未知/由眼睛反算 */
    uint32_t gaze_source_mask; /* 本帧各通道贡献源位掩码 */
    uint32_t gaze_reserved;
    uint64_t gaze_capture_ts_ns; /* 注视数据捕获时刻 */
    float gaze_left_dir[3];    /* HMD 局部单位向量 */
    float gaze_right_dir[3];
    float gaze_left_point_2d[2];  /* 归一化屏幕点；约定由来源定义 */
    float gaze_right_point_2d[2];
} FgbSharedFrameV1;

typedef struct FgbSharedBlockV1 {
    uint32_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t slot_count;

    uint64_t sequence;   /* seqlock：奇数写入中 */
    uint32_t active_slot;
    uint32_t reserved;

    FgbSharedFrameV1 slots[FGB_SHM_SLOTS];
} FgbSharedBlockV1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FGB_SHARED_FRAME_H */
