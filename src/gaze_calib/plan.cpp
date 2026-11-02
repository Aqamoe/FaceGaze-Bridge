#include "gaze_calib/plan.hpp"

#include <cmath>

namespace fgb::gaze_calib {
namespace {

// 外环 8 个方向的顺序固定：从上方开始顺时针，UI 与采集索引都依赖这个顺序。
constexpr float kDiagonal = 0.70710678f;

constexpr std::array<std::array<float, 2>, kTargetCount> points_for(RingAngles ring) {
    return {{
        {0.0f, 0.0f},                                      // 0 中心
        {0.0f, ring.inner},                                // 1 上
        {ring.inner, 0.0f},                                // 2 右
        {0.0f, -ring.inner},                               // 3 下
        {-ring.inner, 0.0f},                               // 4 左
        {ring.outer * kDiagonal, ring.outer * kDiagonal},   // 5 右上
        {ring.outer, 0.0f},                                // 6 右
        {ring.outer * kDiagonal, -ring.outer * kDiagonal},  // 7 右下
        {0.0f, -ring.outer},                               // 8 下
        {-ring.outer * kDiagonal, -ring.outer * kDiagonal}, // 9 左下
        {-ring.outer, 0.0f},                               // 10 左
        {-ring.outer * kDiagonal, ring.outer * kDiagonal},  // 11 左上
        {0.0f, ring.outer},                                // 12 上
    }};
}

constexpr std::array<Target, kTargetCount> make_targets(RingAngles ring) {
    std::array<Target, kTargetCount> list{};
    const std::array<std::array<float, 2>, kTargetCount> points = points_for(ring);
    for (std::size_t i = 0; i < kTargetCount; ++i) {
        list[i].yaw_deg   = points[i][0];
        list[i].pitch_deg = points[i][1];
    }
    return list;
}

} // namespace

RingAngles ring_angles(Span span) noexcept {
    return span == Span::narrow ? kRingNarrow : kRingWide;
}

const char* span_name(Span span) noexcept {
    return span == Span::narrow ? "narrow" : "wide";
}

Span span_from_name(std::string_view name) noexcept {
    return name == "narrow" ? Span::narrow : Span::wide;
}

const std::array<Target, kTargetCount>& targets(Span span) noexcept {
    static const std::array<Target, kTargetCount> wide   = make_targets(kRingWide);
    static const std::array<Target, kTargetCount> narrow = make_targets(kRingNarrow);
    return span == Span::narrow ? narrow : wide;
}

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
