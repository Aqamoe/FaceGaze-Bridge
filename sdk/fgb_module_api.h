/*
 * FaceGaze Bridge 原生模块 ABI（v2，唯一 ABI）
 * ===========================================
 *
 * 这是模块作者唯一需要 include 的头文件。纯 C，无 STL、无异常、无 C++ 名字修饰。
 * 宿主只查找一个导出符号：
 *
 *     const FgbModule* fgb_module_query_v2(uint32_t host_abi_version);
 *
 * 历史：v1（整眼原子的 `FgbEyeV1` + `FgbFrameSlotV1`）已在 2026-10-31 移除，
 * 见 docs/adr/0034-native-module-v2-only.md。当前没有任何已发布的 v1 模块，
 * 用旧 SDK 编译的 DLL 会加载失败并提示重新编译，不会被误读。
 *
 * 三条硬性规则：
 *   1. 跨边界只传 POD；`struct_size` 是第一个字段，追加字段永远放在末尾，不重排。
 *   2. `update()` 由宿主在引擎线程按 tick_hz 调用；`commit` 只能在 initialize/update
 *      里同步调用，不要在模块自己起的线程里写帧（ABI 不做跨线程保护）。
 *   3. 模块 DLL 必须带 `fgb-native.json`（字段与 module.json 相同）说明自己的
 *      ModuleId / ModuleName / Version / DllFileName，宿主不加载 DLL 就能列出来。
 *
 * 坐标系：右手系，+Y 上，-Z 前，单位米（与 OpenXR 一致）。
 * 2D 注视点：该眼显示视口归一化 [0,1]，左上原点，畸变前（ADR-0033）。
 */
#ifndef FGB_MODULE_API_H
#define FGB_MODULE_API_H

#include <stdint.h>

#define FGB_MODULE_ABI_V2 2u
#define FGB_MAX_SHAPES 88u

/* 模块能力位：声明自己能参与哪些通道的合并。宿主据此做列表、角色选择与
 * "写了自己没声明的属性" 的告警。
 *
 * EYES 是**属性级**能力：可以只给 openness、只给 pupil、只给单眼数据，
 * 具体有哪些看 `FgbFrameSlot::attributes`。不再要求"整眼原子"。
 * GAZE 是独立注视通道（逐眼/合并方向 + 可选 2D 点），不包含 openness/pupil；
 * 它不驱动头像眼睛，只驱动渲染/foveation 与 /api/gaze。
 * 只声明真正有数据的通道；合并层只认声明。
 */
#define FGB_CAP_EYES 0x1u
#define FGB_CAP_EXPRESSION 0x2u
#define FGB_CAP_HEAD 0x4u
#define FGB_CAP_GAZE 0x8u

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

/* ---- 帧属性位（FgbFrameSlot::attributes）----
 *
 * 一帧是一组按属性分组的"有效声明"：只有置了位的字段才被读取，其余保持为 0
 * 也不会被当成"闭眼/看正前方"。每只眼的 openness / pupil / gaze / point 都是
 * 独立属性，模块只填自己真正拥有的那几项。
 *
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
/* 帧级：本帧的 2D 注视点已经标定到"该眼显示视口"（ADR-0033 的规范空间）。
 *
 * 不写这一位 = 点在**来源自己的空间**里（原始相机/协议坐标，映射未知）：
 * 可以展示、可以做相对趋势分析，但**不要**用于 foveation 或任何需要
 * "屏幕上的位置"的地方。
 *
 * 只有当模块真的把像素/NDC/相机坐标标定到视口后，才同时写点位属性 + 这一位。 */
#define FGB_ATTR_POINT_VIEWPORT 0x1000ull

/*
 * 单眼属性块。宿主以帧的 `attributes` 为准，本结构没有逐眼 valid 字段：
 * 一只眼这一帧没有数据，就是它的属性位没置。
 *
 * 2D 点（FGB_ATTR_LEFT/RIGHT_POINT）的坐标约定见 ADR-0033：
 *   该眼"最终看到的那块画面"（串流/合成单眼视图）的归一化坐标，
 *   x/y ∈ [0,1]，原点左上，+x 向右，+y 向下，
 *   畸变前的采样空间（合成器再套镜头畸变），逐眼独立。
 *
 *   - 标定到规范空间后再置 FGB_ATTR_POINT_VIEWPORT；
 *     只写点位属性 = 来源自己的原始坐标（消费端不能当屏幕位置用）。
 *   - 映射由模块负责：只有你知道原始坐标系（内参、镜像、换眼、分辨率），
 *     宿主不做像素/NDC/反投影换算。
 *   - 越界不是错误（看向画外、标定误差），宿主保留原值不裁剪。
 *   - NaN/Inf 会把该眼的点标为无效（另一只眼不受影响）。
 */
typedef struct FgbEyeAttributes {
    float openness;            /* FGB_ATTR_*_OPENNESS */
    float openness_confidence; /* 0..1 */

    float pupil_diameter_mm; /* 0 表示不提供 */
    float pupil_dilation;    /* -1..1，0 为基线 */

    float gaze_dir[3];       /* HMD 局部单位向量，-Z 前 */
    float gaze_confidence;   /* 0..1 */
    float gaze_point_2d[2];  /* 归一化屏幕点，[0,1] 左上（见上） */

    uint64_t openness_ts_ns; /* 0 = 用帧的 capture_ts_ns */
    uint64_t pupil_ts_ns;
    uint64_t gaze_ts_ns; /* 方向与点位共用（逐属性没有单独的点时间戳） */
} FgbEyeAttributes;

/*
 * 宿主分配、模块写入的帧槽。指针直接指向宿主的缓冲。
 *
 * 写任何字段之前先确认宿主分配了足够的内存：结构会随后续 ABI 在末尾增长，
 * 用当前 SDK 头编译的模块在旧宿主上必须检查 `struct_size`：
 *
 *     if (slot->struct_size < sizeof(FgbFrameSlot)) { ... 少写新字段 ... }
 *
 * 反过来，新宿主对旧模块只会读取 `min(struct_size, sizeof)` 覆盖到的字段。
 */
typedef struct FgbFrameSlot {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t shape_count; /* 实际可写的形状数量，<= FGB_MAX_SHAPES */
    uint32_t reserved;

    uint64_t attributes; /* FGB_ATTR_* 位或；决定下面哪些字段有效 */

    float shapes[FGB_MAX_SHAPES];        /* 0..1，顺序见 Unified Expressions */
    uint8_t shape_valid[FGB_MAX_SHAPES]; /* 0/1；只置实际填了的形状 */
    uint64_t expression_ts_ns;

    FgbEyeAttributes left_eye;
    FgbEyeAttributes right_eye;

    float combined_gaze_dir[3]; /* FGB_ATTR_COMBINED_GAZE */
    float combined_gaze_confidence;
    uint64_t combined_gaze_ts_ns;

    float vergence_distance_m; /* FGB_ATTR_VERGENCE，米 */
    uint64_t vergence_ts_ns;

    float head_rotation[4]; /* x,y,z,w，HMD 局部 */
    float head_position[3]; /* 米，HMD 局部 */
    uint64_t head_ts_ns;

    uint64_t capture_ts_ns; /* 整帧时间戳；0 = 宿主补 */
} FgbFrameSlot;

typedef struct FgbHostServices {
    uint32_t struct_size;
    uint32_t abi_version;

    void (*log)(void* ctx, int level, const char* message);
    void (*report_status)(void* ctx, int state, const char* detail);

    FgbFrameSlot* (*acquire_frame_slot)(void* ctx);
    void (*commit_frame_slot)(void* ctx, FgbFrameSlot* slot, uint64_t capture_ts_ns);

    /* 按名字查 Unified Expressions 形状下标（0..87），未知返回 -1。
       模块不必硬编码下标，宿主保证与协议层顺序一致。 */
    int32_t (*shape_index)(void* ctx, const char* name);

    void* ctx;
} FgbHostServices;

typedef struct FgbInitParams {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t tick_hz;
    const char* module_dir; /* UTF-8，模块自己的目录，可用于读写 config.ini */

    /* 追加字段：宿主版本（UTF-8，如 "1.1.0"）。0 = 旧宿主没提供。
       只用于日志/适配提示，不要据此改变 ABI 语义。 */
    const char* host_version;
} FgbInitParams;

typedef struct FgbModule {
    uint32_t struct_size;
    uint32_t abi_version;

    const char* id;           /* 反向域名或 GUID，与 fgb-native.json 的 ModuleId 一致 */
    const char* display_name; /* 展示名 */
    uint32_t capabilities;    /* FGB_CAP_* 位或 */

    /* 返回 1 表示成功。self 是模块自己的实例指针。 */
    int (*initialize)(void** self, const FgbHostServices* host, const FgbInitParams* params);
    /* 宿主按 tick 调用；用 acquire/commit 提交一帧，没有新数据可以不提交。 */
    void (*update)(void* self);
    void (*teardown)(void* self);
} FgbModule;

/* 唯一的导出符号。返回的指针必须在本进程生命周期内有效。
 *
 * host_abi_version 是宿主支持的 ABI 版本；不等于 FGB_MODULE_ABI_V2 时返回 NULL，
 * 宿主会把它报成"ABI 版本不兼容"。 */
FGB_MODULE_EXPORT const FgbModule* fgb_module_query_v2(uint32_t host_abi_version);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FGB_MODULE_API_H */
