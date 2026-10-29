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
#define FGB_MAX_SHAPES 88u

/* 模块能力位。宿主据此决定这一路能参与哪些通道的合并。 */
#define FGB_CAP_EYE 0x1u
#define FGB_CAP_EXPRESSION 0x2u
#define FGB_CAP_HEAD 0x4u

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

    float gaze_dir[3];    /* HMD 局部单位向量，-Z 为前 */
    float gaze_confidence; /* 0..1 */
} FgbEyeV1;

/*
 * 宿主分配、模块写入的帧槽。v1 是进程内调用，指针直接指向宿主的缓冲；
 * 结构本身按共享内存兼容的方式设计，后续跨进程模块可以复用。
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

/* 唯一导出符号。返回的指针必须在本进程生命周期内有效。 */
FGB_MODULE_EXPORT const FgbModuleV1* fgb_module_query(uint32_t host_abi_version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FGB_MODULE_API_H */
