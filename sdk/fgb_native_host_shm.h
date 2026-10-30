/*
 * FaceGaze Bridge 原生模块跨进程宿主（阶段 C）
 * ============================================
 *
 * 一条命名的共享内存连接 fgb-host-native.exe 与引擎。宿主负责加载模块并把
 * FaceFrame 转成下面的稳定 POD；引擎只读，不直接接触模块 ABI。
 *
 * 读法（seqlock，和共享内存输出相同）：
 *
 *   do {
 *     seq = block->sequence;
 *     if (seq & 1) continue;
 *     slot = block->active_slot;
 *     memcpy(&frame, &block->slots[slot], sizeof(frame));
 *     seq2 = block->sequence;
 *   } while (seq != seq2 || (seq2 & 1));
 *
 * 结构字段只允许末尾追加；version 变化时双方必须拒绝旧布局。
 * 映射名：Local\FGB_NativeHost_<name>
 */
#ifndef FGB_NATIVE_HOST_SHM_H
#define FGB_NATIVE_HOST_SHM_H

#include <stdint.h>

#define FGB_HOST_SHM_MAGIC 0x48424746u /* "FGBH" little-endian */
#define FGB_HOST_SHM_VERSION 1u
#define FGB_HOST_SHM_SLOTS 2u
#define FGB_HOST_SHM_MAX_SHAPES 88u
#define FGB_HOST_SHM_ERROR_MAX 256u
#define FGB_HOST_SHM_LOG_ENTRIES 32u
#define FGB_HOST_SHM_LOG_TEXT 192u

/* block->state */
#define FGB_HOST_STATE_STARTING 0u
#define FGB_HOST_STATE_ACTIVE 1u
#define FGB_HOST_STATE_ERROR 2u
#define FGB_HOST_STATE_STOPPED 3u

/* 与 core/model/frame.hpp 的 channel 位一致：1=表情 2=眼追 4=头姿 8=注视 */
#define FGB_HOST_CHANNEL_EXPRESSION 0x1u
#define FGB_HOST_CHANNEL_EYES 0x2u
#define FGB_HOST_CHANNEL_HEAD 0x4u
#define FGB_HOST_CHANNEL_GAZE 0x8u

/* FgbNativeHostFrameV1::points_valid 的位（ADR-0033）。
 * 低两位编码，旧读者按 `!= 0` 判断"有点"仍然正确。 */
#define FGB_HOST_POINTS_VALID 0x1u    /* 左右眼都有 2D 点 */
#define FGB_HOST_POINTS_VIEWPORT 0x2u /* 点在视口归一化空间，否则是来源原始坐标 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgbNativeHostEyeV1 {
    uint32_t valid;
    uint32_t gaze_valid;
    float openness;
    float openness_confidence;
    float pupil_diameter_mm;
    float pupil_dilation;
    float gaze_dir[3];
    float gaze_confidence;
    uint64_t openness_ts_ns;
    uint64_t pupil_ts_ns;
    uint64_t gaze_ts_ns;
} FgbNativeHostEyeV1;

typedef struct FgbNativeHostGazeSampleV1 {
    uint32_t valid;
    uint32_t point_valid;
    float dir[3];
    float point2d[2];
    float confidence;
    uint64_t capture_ts_ns;
} FgbNativeHostGazeSampleV1;

typedef struct FgbNativeHostFrameV1 {
    uint64_t sequence;
    uint64_t capture_ts_ns;
    uint64_t publish_ts_ns;
    uint32_t channels; /* FGB_HOST_CHANNEL_* */
    uint32_t has_head_pose;
    float quality;

    /* 逐通道捕获时间；0 = 未知，由 engine 用 capture_ts_ns 补 */
    uint64_t expression_ts_ns;
    uint64_t eyes_ts_ns;
    uint64_t gaze_ts_ns;
    uint64_t head_ts_ns;

    FgbNativeHostEyeV1 left_eye;
    FgbNativeHostEyeV1 right_eye;

    uint32_t has_vergence;
    float vergence_distance_m;
    uint64_t vergence_ts_ns;

    FgbNativeHostGazeSampleV1 gaze_left;
    FgbNativeHostGazeSampleV1 gaze_right;
    uint32_t combined_gaze_valid;
    uint32_t points_valid;
    float combined_gaze_dir[3];
    float combined_gaze_confidence;
    uint64_t combined_gaze_ts_ns;

    float head_rotation[4]; /* x,y,z,w */
    float head_position[3]; /* 米 */

    float shapes[FGB_HOST_SHM_MAX_SHAPES];
    uint8_t shape_valid[FGB_HOST_SHM_MAX_SHAPES];
} FgbNativeHostFrameV1;

typedef struct FgbNativeHostLogV1 {
    uint32_t sequence;
    uint32_t level; /* FGB_LOG_* */
    char text[FGB_HOST_SHM_LOG_TEXT];
} FgbNativeHostLogV1;

typedef struct FgbNativeHostBlockV1 {
    uint32_t magic;
    uint32_t version;
    uint32_t struct_size;
    uint32_t slot_count;

    uint64_t sequence; /* seqlock：奇数 = 写入中 */
    uint32_t active_slot;
    uint32_t state; /* FGB_HOST_STATE_* */
    uint32_t frames;
    uint32_t capabilities; /* FGB_CAP_* */
    uint64_t heartbeat_ns;
    uint32_t stop_requested;
    uint32_t reserved;
    char error[FGB_HOST_SHM_ERROR_MAX]; /* UTF-8 */

    /* 模块日志环：sequence 单调递增；消费者只读自己还没读过的序号。 */
    uint32_t log_head;
    uint32_t log_reserved;
    FgbNativeHostLogV1 logs[FGB_HOST_SHM_LOG_ENTRIES];

    FgbNativeHostFrameV1 slots[FGB_HOST_SHM_SLOTS];
} FgbNativeHostBlockV1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FGB_NATIVE_HOST_SHM_H */