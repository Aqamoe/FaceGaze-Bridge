// 标定采集：每个目标攒一小段样本，取中位数并给出离散度（ADR-0042 §5）。
//
// 为什么取中位数而不是均值：注视点里有扫视、眨眼、丢帧，均值会被一两次跳变拉走；
// 中位数只要过半样本是对的就不受影响。
//
// 为什么还要看离散度：样本数够 + 离散度小才说明"这人真的在看这个点"。
// 只数样本会把"眼睛乱飘时点了一堆帧"当成有效数据，拟合出来偏差很大却看不出来。
#pragma once

#include "core/model/gaze.hpp"
#include "gaze_calib/plan.hpp"

#include <array>
#include <cstddef>

namespace fgb::gaze_calib {

struct TargetSamples {
    std::size_t count   = 0;
    Vec2 left{0.0f, 0.0f};
    Vec2 right{0.0f, 0.0f};
    float spread_left   = 0.0f; // 中位数绝对偏差（视口宽比例）
    float spread_right  = 0.0f;
    bool ok             = false;
};

class Collector {
public:
    static constexpr std::size_t kMinSamples = 20;
    static constexpr std::size_t kMaxSamples = 240;
    static constexpr float kMaxSpread        = 0.06f;

    void reset() noexcept;

    // 采一帧。返回 false 表示这帧被拒（该眼没有有效点，或不是本次标定的目标）。
    bool add(std::size_t target, const GazeState& gaze) noexcept;

    TargetSamples result(std::size_t target) const noexcept;
    std::size_t accepted() const noexcept { return accepted_; }
    bool complete() const noexcept; // 每个目标都 ok

private:
    struct Buffer {
        std::array<float, kMaxSamples> left_x{};
        std::array<float, kMaxSamples> left_y{};
        std::array<float, kMaxSamples> right_x{};
        std::array<float, kMaxSamples> right_y{};
        std::size_t count = 0;
    };

    std::array<Buffer, kTargetCount> buffers_{};
    std::size_t accepted_ = 0;
};

} // namespace fgb::gaze_calib
