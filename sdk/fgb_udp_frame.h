/*
 * FaceGaze Bridge UDP 二进制输出（v1）
 * ====================================
 *
 * 用途：把同一帧以固定小端布局发给串流软件 / 自研客户端。一个数据报就是完整一帧，
 * 不做分片；88 个形状时总长 656 字节（v2），远低于 1200 字节的避免分片上限。
 *
 * 约定：
 *   - 本机回环 / 局域网 UDP，无握手、无重传；丢一帧就丢一帧，下一帧是全量
 *   - 所有字段小端；结构体 1 字节对齐（见 FGB_UDP_FRAME_SIZE）
 *   - 坐标系与 shared memory / 模块 ABI 一致：右手系，+Y 上，-Z 前，单位米
 *   - shapes 的顺序就是 core/model/shape.hpp 的 kShapeNames 顺序（与共享内存一致）
 *   - 没有名字表：消费者按固定顺序读，或参照 docs/07-streaming-outputs.md
 *
 * 读法：
 *   recvfrom() 收到正好 FGB_UDP_FRAME_SIZE 字节 → memcpy 到 FgbUdpFrameV1
 *   → 检查 magic/version → 读 attributes 判断眼追/头姿是否有效。
 */
#ifndef FGB_UDP_FRAME_H
#define FGB_UDP_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define FGB_UDP_MAGIC 0x55424746u /* "FGBU" little-endian */
#define FGB_UDP_VERSION 2u
#define FGB_UDP_MAX_SHAPES 88u
#define FGB_UDP_FRAME_SIZE 656u

#define FGB_UDP_ATTR_LEFT_VALID 0x01u
#define FGB_UDP_ATTR_RIGHT_VALID 0x02u
#define FGB_UDP_ATTR_HAS_HEAD 0x04u
#define FGB_UDP_ATTR_FIXATION 0x08u
#define FGB_UDP_ATTR_GAZE_VALID 0x10u

/* FgbUdpFrameV1::gaze_valid 的位（ADR-0030 v1.1 追加） */
#define FGB_UDP_GAZE_LEFT_VALID 0x01u
#define FGB_UDP_GAZE_RIGHT_VALID 0x02u
#define FGB_UDP_GAZE_COMBINED_VALID 0x04u
#define FGB_UDP_GAZE_POINT_2D_VALID 0x08u
/* 点在视口归一化空间（ADR-0033）；不设 = 来源原始坐标，不能当屏幕位置用 */
#define FGB_UDP_GAZE_POINT_VIEWPORT 0x10u

#ifdef _MSC_VER
#pragma pack(push, 1)
#define FGB_UDP_PACKED
#else
#define FGB_UDP_PACKED __attribute__((packed))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FGB_UDP_PACKED FgbUdpFrameV1 {
    uint32_t magic;       /* FGB_UDP_MAGIC */
    uint16_t version;     /* FGB_UDP_VERSION */
    uint16_t header_size; /* magic+version+header_size = 8 */

    uint64_t sequence;       /* 引擎发布序号 */
    uint64_t capture_ts_ns;  /* 数据在物理世界发生的时刻 */
    uint64_t publish_ts_ns;  /* 引擎发布时刻 */

    uint32_t channels;     /* channel::kExpression / kEyes / kHead / kGaze */
    uint32_t attributes;   /* FGB_UDP_ATTR_* */
    uint32_t shape_count;  /* <= FGB_UDP_MAX_SHAPES */
    uint32_t reserved;

    /* 左眼 */
    float left_openness;
    float left_openness_confidence;
    float left_gaze_dir[3];
    float left_gaze_confidence;

    /* 右眼 */
    float right_openness;
    float right_openness_confidence;
    float right_gaze_dir[3];
    float right_gaze_confidence;

    float vergence_distance_m;

    /* 合并注视（M5 使用） */
    float gaze_dir[3];
    float gaze_pitch_deg; /* 正 = 向上 */
    float gaze_yaw_deg;   /* 正 = 向右 */
    float gaze_confidence;

    /* 头姿（HMD 局部，右手系） */
    float head_rotation[4]; /* x, y, z, w */
    float head_position[3]; /* 米 */

    float shapes[FGB_UDP_MAX_SHAPES];        /* 0..1 */
    uint8_t shape_valid[FGB_UDP_MAX_SHAPES]; /* 0/1 */

    /* ---- v1.1 追加：独立注视通道（渲染/foveation 用）---- */
    uint32_t gaze_valid;       /* FGB_UDP_GAZE_* 位 */
    uint32_t gaze_source;      /* 源序号，0 = 未知/由眼睛反算 */
    uint32_t gaze_source_mask; /* 本帧各通道贡献源位掩码 */
    uint32_t gaze_reserved;
    uint64_t gaze_capture_ts_ns;
    float gaze_left_dir[3];
    float gaze_right_dir[3];
    /* 该眼视口归一化坐标：[0,1]，左上原点，畸变前（ADR-0033） */
    float gaze_left_point_2d[2];
    float gaze_right_point_2d[2];
} FgbUdpFrameV1;

#ifdef __cplusplus
} /* extern "C" */

static_assert(sizeof(FgbUdpFrameV1) == FGB_UDP_FRAME_SIZE, "UDP 帧布局变了，同步改文档与探针");
static_assert(offsetof(FgbUdpFrameV1, shapes) == 152, "shapes 偏移变了");
static_assert(offsetof(FgbUdpFrameV1, shape_valid) == 504, "shape_valid 偏移变了");
static_assert(offsetof(FgbUdpFrameV1, gaze_valid) == 592, "v1.1 注视字段偏移变了");
#endif

#ifdef _MSC_VER
#pragma pack(pop)
#endif

#undef FGB_UDP_PACKED

#endif /* FGB_UDP_FRAME_H */
