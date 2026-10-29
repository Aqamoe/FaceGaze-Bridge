// 注视估计：把合并后的视线方向变成"在扫视 / 在注视"。
//
// 输入是 Engine::compute_gaze() 的合并方向（左右眼平均），输出写回同一个 GazeState：
//   saccade_vel_deg_s  相邻帧方向夹角 / 时间
//   fixation           角速度低于入口阈值并持续 >= fixation_min_ms
//   fixation_ms        当前注视已持续多久
//
// 为什么用角速度而不是"位移小于阈值"：位移依赖分辨率/帧率，角速度是物理量。
// 阈值取 VR 常见值（注视 < 30°/s，扫视 > 80°/s），有迟滞，避免在边界抖动。
#pragma once

#include "core/model/frame.hpp"
#include "core/model/gaze.hpp"

#include <cstdint>

namespace fgb {

struct GazeEstimatorConfig {
    float fixation_speed_deg_s = 30.0f; // 低于此速度视为"停了"
    float saccade_speed_deg_s  = 80.0f; // 高于此速度退出注视（迟滞）
    float fixation_min_ms      = 80.0f; // 停多久才算一次注视
    float max_gap_s            = 0.5f;  // 数据断档超过它就不跨帧算速度
};

class GazeEstimator {
public:
    explicit GazeEstimator(GazeEstimatorConfig config = {}) : config_(config) {}

    void update(const FaceFrame& frame, GazeState& gaze, std::uint64_t now_ns) noexcept;
    void reset() noexcept;

private:
    GazeEstimatorConfig config_;
    bool has_prev_     = false;
    Vec3 prev_dir_{0.0f, 0.0f, -1.0f};
    std::uint64_t prev_ns_ = 0;
    bool fixation_         = false;
    float fixation_ms_     = 0.0f;
};

} // namespace fgb
