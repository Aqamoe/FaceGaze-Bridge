// 圆盘标定的点位与"目标 → 该眼视口坐标"的换算（ADR-0042 §5）。
//
// 为什么用**视线方向**而不是屏幕像素来定义目标：
//   每个头显、每只眼的投影都不一样，写死"屏幕 30% 处"在不同设备上落到不同视角；
//   而"视线偏 10°"是同一件事。目标的视口坐标再由当前 HMD 的投影算出来，
//   两只眼因此天然一致（同一条视线、各自的投影）。
//
// 为什么有两套范围（Span）：
//   这两个数决定的是**标定在屏幕的哪一块被约束**：越大越贴近边缘，但让人看 32° 的
//   圆点对一部分人是吃力的（镜片/瞳距不合适时尤其），连圆点在哪儿都看不清就没法标。
//   所以给两档：
//     · wide  ：内环 18°、外环 32°，覆盖到 x≈0.27–0.90，边缘也进了拟合（默认）
//     · narrow：内环 10°、外环 20°，只覆盖中间 1/3，看不全外圈时的退路
//   窗口外的边缘只能靠仿射线性外推，精度会差一些——能看全就选 wide。
#pragma once

#include "core/model/gaze_projection.hpp"
#include "core/model/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fgb::gaze_calib {

// 13 点：中心 + 内环 4（上下左右）+ 外环 8（45° 间隔），见 ADR-0042 §5。
inline constexpr std::size_t kTargetCount = 13;

struct Target {
    float yaw_deg   = 0.0f; // 正 = 右
    float pitch_deg = 0.0f; // 正 = 上
};

// 标定范围的两档点位半径（视角，度）。
struct RingAngles {
    float inner = 0.0f;
    float outer = 0.0f;
};

inline constexpr RingAngles kRingWide{18.0f, 32.0f};
inline constexpr RingAngles kRingNarrow{10.0f, 20.0f};

enum class Span : std::uint8_t {
    wide   = 0, // 覆盖到屏幕边缘（默认）
    narrow = 1, // 只覆盖中间，看不全外圈时用
};

RingAngles ring_angles(Span span) noexcept;

// "wide" / "narrow"：settings.ini 与 IPC 用同一套名字。
const char* span_name(Span span) noexcept;
Span span_from_name(std::string_view name) noexcept; // 未知名字 → wide

const std::array<Target, kTargetCount>& targets(Span span = Span::wide) noexcept;

// 目标 → HMD 局部的单位方向（与 EyeState::gazeDirLocal 同一坐标系）。
Vec3 target_direction(const Target& target) noexcept;

// 目标 → 该眼视口点。投影不可用时返回 false（标定不该在缺几何时进行）。
bool target_viewport(const Target& target, const EyeProjection& eye, Vec2& out) noexcept;

} // namespace fgb::gaze_calib
