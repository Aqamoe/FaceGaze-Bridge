// 合并后的注视状态。头像驱动用不到它，但注视点服务、串流和 UI 都用同一份。
//
// 为什么从 sinks/sink.hpp 挪到这里：GazeEstimator 在 core/process，不能反过来
// 依赖 sinks 层；数据模型放 core/model 才是正确方向。
#pragma once

#include "core/model/math.hpp"

#include <cstdint>

namespace fgb {

struct GazeState {
    bool valid = false;
    Vec3 dir_local{0.0f, 0.0f, -1.0f}; // HMD 局部单位向量
    float pitch_deg = 0.0f;            // 正 = 向上
    float yaw_deg   = 0.0f;            // 正 = 向右
    float vergence_m = 0.0f;
    float confidence = 0.0f;

    // 由 GazeEstimator 维护
    bool fixation          = false;
    std::uint64_t ts_ns    = 0; // 这次更新的时刻
    float saccade_vel_deg_s = 0.0f;
    float fixation_ms       = 0.0f;
};

} // namespace fgb
