// 圆盘标定的点位与"目标 → 该眼视口坐标"的换算（ADR-0042 §5）。
//
// 为什么用**视线方向**而不是屏幕像素来定义目标：
//   每个头显、每只眼的投影都不一样，写死"屏幕 30% 处"在不同设备上落到不同视角；
//   而"视线偏 10°"是同一件事。目标的视口坐标再由当前 HMD 的投影算出来，
//   两只眼因此天然一致（同一条视线、各自的投影）。
#pragma once

#include "core/model/gaze_projection.hpp"
#include "core/model/math.hpp"

#include <array>
#include <cstddef>

namespace fgb::gaze_calib {

// 13 点：中心 + 内环 4（上下左右）+ 外环 8（45° 间隔），见 ADR-0042 §5。
inline constexpr std::size_t kTargetCount = 13;

struct Target {
    float yaw_deg   = 0.0f; // 正 = 右
    float pitch_deg = 0.0f; // 正 = 上
};

const std::array<Target, kTargetCount>& targets() noexcept;

// 目标 → HMD 局部的单位方向（与 EyeState::gazeDirLocal 同一坐标系）。
Vec3 target_direction(const Target& target) noexcept;

// 目标 → 该眼视口点。投影不可用时返回 false（标定不该在缺几何时进行）。
bool target_viewport(const Target& target, const EyeProjection& eye, Vec2& out) noexcept;

// 内环/外环的视角，供 UI 显示"请注视这个点"和自检残差尺度。
constexpr float kInnerRingDeg = 10.0f;
constexpr float kOuterRingDeg = 20.0f;

} // namespace fgb::gaze_calib
