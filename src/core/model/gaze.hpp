// 合并后的注视状态。头像驱动用不到它，但注视点服务、串流和 UI 都用同一份。
//
// 为什么从 sinks/sink.hpp 挪到这里：GazeEstimator 在 core/process，不能反过来
// 依赖 sinks 层；数据模型放 core/model 才是正确方向。
#pragma once

#include "core/model/frame.hpp"
#include "core/model/math.hpp"

#include <cstdint>

namespace fgb {

struct GazeState {
    bool valid = false;
    Vec3 dir_local{0.0f, 0.0f, -1.0f}; // 合并/渲染方向，HMD 局部单位向量
    float pitch_deg = 0.0f;            // 正 = 向上
    float yaw_deg   = 0.0f;            // 正 = 向右
    float vergence_m = 0.0f;
    float confidence = 0.0f;

    // 逐眼方向：gaze 通道直接给出；回退到 eyes 时由眼追反算。
    bool left_valid = false;
    bool right_valid = false;
    Vec3 left_dir_local{0.0f, 0.0f, -1.0f};
    Vec3 right_dir_local{0.0f, 0.0f, -1.0f};

    // 可选 2D 屏幕点（约定由来源决定，当前只做透传；见 ADR-0030 开放问题）
    bool point2d_valid = false;
    Vec2 left_point_2d{0.0f, 0.0f};
    Vec2 right_point_2d{0.0f, 0.0f};

    std::uint64_t capture_ts_ns = 0; // 数据捕获时刻；0 = 未知
    SourceId source            = kInvalidSource;
    std::uint32_t source_mask  = 0; // 本帧各通道贡献源的位掩码
    SourceId left_source       = kInvalidSource;
    SourceId right_source      = kInvalidSource;
    std::uint64_t left_capture_ts_ns  = 0;
    std::uint64_t right_capture_ts_ns = 0;

    // 由 GazeEstimator 维护
    bool fixation          = false;
    std::uint64_t ts_ns    = 0; // 这次更新的时刻
    float saccade_vel_deg_s = 0.0f;
    float fixation_ms       = 0.0f;
};

} // namespace fgb
