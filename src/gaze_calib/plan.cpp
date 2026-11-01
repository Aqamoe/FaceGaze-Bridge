#include "gaze_calib/plan.hpp"

#include <cmath>

namespace fgb::gaze_calib {
namespace {

// 外环 8 个方向的顺序固定：从上方开始顺时针，UI 与采集索引都依赖这个顺序。
constexpr std::array<std::array<float, 2>, kTargetCount> kPoints{{
    {0.0f, 0.0f},                                        // 0 中心
    {0.0f, kInnerRingDeg},                               // 1 上
    {kInnerRingDeg, 0.0f},                               // 2 右
    {0.0f, -kInnerRingDeg},                              // 3 下
    {-kInnerRingDeg, 0.0f},                              // 4 左
    {kOuterRingDeg * 0.70710678f, kOuterRingDeg * 0.70710678f},   // 5 右上
    {kOuterRingDeg, 0.0f},                               // 6 右
    {kOuterRingDeg * 0.70710678f, -kOuterRingDeg * 0.70710678f},  // 7 右下
    {0.0f, -kOuterRingDeg},                              // 8 下
    {-kOuterRingDeg * 0.70710678f, -kOuterRingDeg * 0.70710678f}, // 9 左下
    {-kOuterRingDeg, 0.0f},                              // 10 左
    {-kOuterRingDeg * 0.70710678f, kOuterRingDeg * 0.70710678f},  // 11 左上
    {0.0f, kOuterRingDeg},                               // 12 上
}};

const std::array<Target, kTargetCount> kTargets = [] {
    std::array<Target, kTargetCount> list{};
    for (std::size_t i = 0; i < kTargetCount; ++i) {
        list[i].yaw_deg   = kPoints[i][0];
        list[i].pitch_deg = kPoints[i][1];
    }
    return list;
}();

} // namespace

const std::array<Target, kTargetCount>& targets() noexcept { return kTargets; }

Vec3 target_direction(const Target& target) noexcept {
    return pitch_yaw_deg_to_dir(target.pitch_deg, target.yaw_deg);
}

bool target_viewport(const Target& target, const EyeProjection& eye, Vec2& out) noexcept {
    float x = 0.0f;
    float y = 0.0f;
    if (!project_dir_to_viewport(target_direction(target), eye, x, y)) {
        return false;
    }
    out = Vec2{x, y};
    return std::isfinite(x) && std::isfinite(y);
}

} // namespace fgb::gaze_calib
