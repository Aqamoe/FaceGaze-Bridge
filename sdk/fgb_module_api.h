/*
 * FaceGaze Bridge 原生模块 ABI（v1）
 * ==================================
 *
 * 这是模块作者唯一需要 include 的头文件。纯 C，无 STL、无异常、无 C++ 名字修饰。
 * 宿主只查找一个导出符号：
 *
 *     const FgbModuleV1* fgb_module_query(uint32_t host_abi_version);
 *
 * 三条硬性规则：
 *   1. 跨边界只传 POD；struct_size 追加字段时永远放在末尾，不重排。
 *   2. update() 由宿主在引擎线程按 tick_hz 调用；commit 只能在 initialize/update
 *      里同步调用，不要在模块自己起的线程里写帧（v1 不做跨线程保护）。
 *   3. 模块 DLL 必须带 fgb-native.json（字段与 module.json 相同）说明自己的
 *      ModuleId / ModuleName / Version / DllFileName，宿主不加载 DLL 就能列出来。
 *
 * 坐标系：右手系，+Y 上，-Z 前，单位米（与 OpenXR 一致）。
 */
#ifndef FGB_MODULE_API_H
#define FGB_MODULE_API_H

#include <stdint.h>

#define FGB_MODULE_ABI_V1 1u
#define FGB_MODULE_ABI_V2 2u
#define FGB_MAX_SHAPES 88u

/* 模块能力位。宿主据此决定这一路能参与哪些通道的合并。
 *
 * EYE 是整眼能力：声明它就必须同时给出 gaze、openness 和 pupil。
 * 只提供注视方向/注视点、不提供睁眼度的模块只能声明 GAZE，不能借用 EYE，
 * 否则头像会因为 openness 缺失而闭眼。EYE 与 GAZE 可以同时声明。
 */
#define FGB_CAP_EYE 0x1u
#define FGB_CAP_EXPRESSION 0x2u
#define FGB_CAP_HEAD 0x4u
#define FGB_CAP_GAZE 0x8u
/* v2：按属性提供眼睛数据（openness/pupil/gaze 可只给其中一部分）。
 * 具体有哪些属性看 FgbFrameSlotV2::attributes；v1 的 EYE 仍是整眼原子能力。 */
#define FGB_CAP_EYE_ATTRIBUTES 0x10u

/* 日志级别，与引擎内部一致 */
#define FGB_LOG_TRACE 0
#define FGB_LOG_DEBUG 1
#define FGB_LOG_INFO 2
#define FGB_LOG_WARN 3
#define FGB_LOG_ERROR 4

/* report_status 的状态码 */
#define FGB_STATUS_STARTING 0
#define FGB_STATUS_ACTIVE 1
#define FGB_STATUS_ERROR 2
#define FGB_STATUS_STOPPED 3

#ifdef _WIN32
#define FGB_MODULE_EXPORT __declspec(dllexport)
#else
#define FGB_MODULE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgbEyeV1 {
    uint32_t valid; /* 1 = 本帧有有效眼追数据 */
    uint32_t reserved;

    float openness;            /* 0 闭眼，1 完全睁开 */
    float openness_confidence; /* 0..1 */

    float pupil_diameter_mm; /* 0 表示不提供 */
    float pupil_dilation;    /* -1..1，0 为基线 */

    float gaze_dir[3];    /* HMD 局部单位向量，-Z 为前；这是头像眼睛方向 */
    float gaze_confidence; /* 0..1 */
} FgbEyeV1;

/* FgbGazeV1::valid 的位。逐眼/合并方向与 2D 点分别有效，互不连坐。
 *
 * 只给 2D 点的模块只声明 FGB_GAZE_POINT_2D 即可：宿主照常收下这一帧，
 * 但 GazeState 的 valid / left_valid / right_valid 为假（没有方向就不编方向），
 * 消费端看 point2d_valid / left_point_valid / right_point_valid，
 * 注视/扫视判定对这类来源不成立（直接复位，不会永远"注视中"）。
 *
 * 不要为了"让帧被收下"而伪造 FGB_GAZE_LEFT/RIGHT：假方向会进 /api/gaze、
 * 共享内存和 UDP，第三方会拿一个恒定的 (0,0,-1) 当真数据用。
 */
#define FGB_GAZE_LEFT 0x1u
#define FGB_GAZE_RIGHT 0x2u
#define FGB_GAZE_COMBINED 0x4u
#define FGB_GAZE_POINT_2D 0x8u
/* 0x10：2D 点已经映射到"该眼显示视口"的归一化坐标（ADR-0033 的规范空间）。
 *
 * 不写这一位 = 点在**来源自己的空间**里（原始相机/协议坐标，映射未知）：
 * 可以展示、可以做相对趋势分析，但**不要**用于 foveation 或任何需要
 * "屏幕上的位置"的地方。
 *
 * 只有当模块真的把像素/NDC/相机坐标标定到视口后，才同时写 POINT_2D|POINT_VIEWPORT。 */
#define FGB_GAZE_POINT_VIEWPORT 0x10u

/* ---- v2 属性位（FgbFrameSlotV2::attributes）----
 * 属性级数据不再要求 EYE 整块原子：模块只声明/填写自己真正拥有的属性。
 * 时间戳为 0 时用帧的 capture_ts_ns。
 */
#define FGB_ATTR_EXPRESSION 0x1ull
#define FGB_ATTR_LEFT_OPENNESS 0x2ull
#define FGB_ATTR_RIGHT_OPENNESS 0x4ull
#define FGB_ATTR_LEFT_PUPIL 0x8ull
#define FGB_ATTR_RIGHT_PUPIL 0x10ull
#define FGB_ATTR_LEFT_GAZE 0x20ull
#define FGB_ATTR_RIGHT_GAZE 0x40ull
#define FGB_ATTR_COMBINED_GAZE 0x80ull
#define FGB_ATTR_LEFT_POINT 0x100ull
#define FGB_ATTR_RIGHT_POINT 0x200ull
#define FGB_ATTR_VERGENCE 0x400ull
#define FGB_ATTR_HEAD 0x800ull
/* 与 FGB_GAZE_POINT_VIEWPORT 同义（帧级）：v2 的 2D 点属性是否在视口坐标里 */
#define FGB_ATTR_POINT_VIEWPORT 0x1000ull

/*
 * 独立注视通道。只声明 FGB_CAP_GAZE 的模块写这里，不要伪造 FgbEyeV1。
 *
 * 2D 点（FGB_GAZE_POINT_2D）的坐标约定见 ADR-0033。规范空间是：
 *   该眼"最终看到的那块画面"（串流/合成单眼视图）的归一化坐标，
 *   x/y ∈ [0,1]，原点左上，+x 向右，+y 向下，
 *   畸变前的采样空间（合成器再套镜头畸变），逐眼独立。
 *
 *   - 标定到规范空间后再加声明 FGB_GAZE_POINT_VIEWPORT；
 *     只写 FGB_GAZE_POINT_2D = 来源自己的原始坐标（消费端不能当屏幕位置用）。
 *   - 映射由模块负责：只有你知道原始坐标系（内参、镜像、换眼、分辨率），
 *     宿主不做像素/NDC/反投影换算。
 *   - 越界不是错误（看向画外、标定误差），宿主保留原值不裁剪。
 *   - NaN/Inf 会把该眼的点标为无效。
 */
typedef struct FgbGazeV1 {
    uint32_t valid; /* FGB_GAZE_* 位或；0 = 本帧没有注视数据 */
    uint32_t reserved;

    float left_dir[3];     /* HMD 局部单位向量，-Z 前 */
    float right_dir[3];
    float combined_dir[3];

    /* 该眼视口归一化坐标（ADR-0033），FGB_GAZE_POINT_2D 有效时读取 */
    float left_point_2d[2];
    float right_point_2d[2];

    float confidence;      /* 0..1 */
    uint64_t capture_ts_ns; /* 0 = 用帧的 capture_ts_ns */
} FgbGazeV1;

/*
 * 宿主分配、模块写入的帧槽。v1 是进程内调用，指针直接指向宿主的缓冲；
 * 结构本身按共享内存兼容的方式设计，后续跨进程模块可以复用。
 *
 * gaze 是 v1.1 在末尾追加的字段。写它之前必须先检查：
 *     slot->struct_size >= offsetof(FgbFrameSlotV1, gaze) + sizeof(FgbGazeV1)
 * 旧宿主不会分配这段内存，直接写会越界。
 */
typedef struct FgbFrameSlotV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t shape_count; /* 实际有效的形状数量，<= FGB_MAX_SHAPES */
    uint32_t channels;    /* 本帧实际携带的 FGB_CAP_* 位 */

    float shapes[FGB_MAX_SHAPES];         /* 0..1，顺序见 Unified Expressions */
    uint8_t shape_valid[FGB_MAX_SHAPES];  /* 0/1 */

    FgbEyeV1 left_eye;
    FgbEyeV1 right_eye;

    uint32_t has_vergence;
    float vergence_distance_m;

    uint32_t has_head_pose;
    float head_rotation[4]; /* x,y,z,w，HMD 局部旋转 */
    float head_position[3]; /* 米，HMD 局部 */

    uint64_t capture_ts_ns; /* 可选：数据在物理世界发生的时刻，0 = 由宿主补 */

    FgbGazeV1 gaze; /* v1.1 追加；写之前检查 struct_size */
} FgbFrameSlotV1;

typedef struct FgbHostServicesV1 {
    uint32_t struct_size;
    uint32_t abi_version;

    void (*log)(void* ctx, int level, const char* message);
    void (*report_status)(void* ctx, int state, const char* detail);

    FgbFrameSlotV1* (*acquire_frame_slot)(void* ctx);
    void (*commit_frame_slot)(void* ctx, FgbFrameSlotV1* slot, uint64_t capture_ts_ns);

    /* 按名字查 Unified Expressions 形状下标（0..87），未知返回 -1。
       模块不必硬编码下标，宿主保证与协议层顺序一致。 */
    int32_t (*shape_index)(void* ctx, const char* name);

    void* ctx;
} FgbHostServicesV1;

typedef struct FgbInitParamsV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t tick_hz;
    const char* module_dir; /* UTF-8，模块自己的目录，可用于读写 config.ini */
} FgbInitParamsV1;

typedef struct FgbModuleV1 {
    uint32_t struct_size;
    uint32_t abi_version;

    const char* id;           /* 反向域名或 GUID，与 fgb-native.json 的 ModuleId 一致 */
    const char* display_name; /* 展示名 */
    uint32_t capabilities;    /* FGB_CAP_* 位或 */

    /* 返回 1 表示成功。self 是模块自己的实例指针。 */
    int (*initialize)(void** self, const FgbHostServicesV1* host, const FgbInitParamsV1* params);
    /* 宿主按 tick 调用；用 acquire/commit 提交一帧，没有新数据可以不提交。 */
    void (*update)(void* self);
    void (*teardown)(void* self);
} FgbModuleV1;

/* ---- v2：属性级帧 ----
 *
 * 与 v1 并存：v2 模块导出 fgb_module_query_v2，宿主优先尝试它，失败再回退
 * fgb_module_query(v1)。v1 模块不需要任何改动。
 */

typedef struct FgbEyeAttributesV2 {
    uint32_t valid; /* 本眼实际填写的 FGB_ATTR_* 子集；宿主以帧的 attributes 为准 */
    uint32_t reserved;

    float openness;            /* FGB_ATTR_*_OPENNESS */
    float openness_confidence; /* 0..1 */

    float pupil_diameter_mm; /* 0 表示不提供 */
    float pupil_dilation;    /* -1..1，0 为基线 */

    float gaze_dir[3];       /* HMD 局部单位向量，-Z 前 */
    float gaze_confidence;   /* 0..1 */
    float gaze_point_2d[2];  /* 归一化屏幕点，暂定 [0,1] 左上 */

    uint64_t openness_ts_ns; /* 0 = 用帧的 capture_ts_ns */
    uint64_t pupil_ts_ns;
    uint64_t gaze_ts_ns;
} FgbEyeAttributesV2;

typedef struct FgbFrameSlotV2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t shape_count; /* <= FGB_MAX_SHAPES */
    uint32_t reserved;

    uint64_t attributes; /* FGB_ATTR_* 位或；决定下面哪些字段有效 */

    float shapes[FGB_MAX_SHAPES];
    uint8_t shape_valid[FGB_MAX_SHAPES];
    uint64_t expression_ts_ns;

    FgbEyeAttributesV2 left_eye;
    FgbEyeAttributesV2 right_eye;

    float combined_gaze_dir[3];
    float combined_gaze_confidence;
    uint64_t combined_gaze_ts_ns;

    float vergence_distance_m; /* FGB_ATTR_VERGENCE */
    uint64_t vergence_ts_ns;

    float head_rotation[4]; /* x,y,z,w */
    float head_position[3]; /* 米 */
    uint64_t head_ts_ns;

    uint64_t capture_ts_ns; /* 整帧时间戳；0 = 宿主补 */
} FgbFrameSlotV2;

typedef struct FgbHostServicesV2 {
    uint32_t struct_size;
    uint32_t abi_version;

    void (*log)(void* ctx, int level, const char* message);
    void (*report_status)(void* ctx, int state, const char* detail);

    FgbFrameSlotV2* (*acquire_frame_slot)(void* ctx);
    void (*commit_frame_slot)(void* ctx, FgbFrameSlotV2* slot, uint64_t capture_ts_ns);

    int32_t (*shape_index)(void* ctx, const char* name);

    void* ctx;
} FgbHostServicesV2;

typedef FgbInitParamsV1 FgbInitParamsV2;

typedef struct FgbModuleV2 {
    uint32_t struct_size;
    uint32_t abi_version;

    const char* id;
    const char* display_name;
    uint32_t capabilities; /* FGB_CAP_*；属性级眼睛用 FGB_CAP_EYE_ATTRIBUTES */

    int (*initialize)(void** self, const FgbHostServicesV2* host, const FgbInitParamsV2* params);
    void (*update)(void* self);
    void (*teardown)(void* self);
} FgbModuleV2;

/* 唯一 v1 导出符号。返回的指针必须在本进程生命周期内有效。 */
FGB_MODULE_EXPORT const FgbModuleV1* fgb_module_query(uint32_t host_abi_version);
/* v2 模块导出它；宿主优先尝试，找不到/返回空时回退 v1。 */
FGB_MODULE_EXPORT const FgbModuleV2* fgb_module_query_v2(uint32_t host_abi_version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FGB_MODULE_API_H */
