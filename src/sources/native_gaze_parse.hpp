// 模块 ABI 的注视属性 → GazePair / GazeSample 的映射。
//
// 为什么单独抽出来：SDK 明文写着「方向属性与 2D 点属性分别有效，互不连坐」
// （fgb_module_api.h 的 FGB_ATTR_*_GAZE / FGB_ATTR_*_POINT 注释），但这条路径
// 曾经把点位写在方向位里，只声明点位的模块会被整帧丢掉——PICO Connect 的私有
// 注视通道只能伪造一个 (0,0,-1) 方向来绕过，副作用是注视/扫视判定永远「注视中」。
//
// 抽成 inline 头文件是为了能单测：这段映射不需要真的加载 DLL。
#pragma once

#include "core/model/frame.hpp"
#include "fgb_module_api.h"

#include <cmath>
#include <cstdint>

namespace fgb::native {

namespace detail {

// 视口归一化坐标（ADR-0033）的合法性：NaN/Inf 说明模块没算出结果，
// 不能当成 (0,0) 或 (1,1) 这种"角落里"-like 的假点。
inline bool usable_point(float x, float y) noexcept {
    return std::isfinite(x) && std::isfinite(y);
}

} // namespace detail

// 单眼的注视方向与 2D 点属性（FgbEyeAttributes）。
// 两者是独立属性位：只给点的眼也有数据，方向保持默认正前方且标记无效。
inline void apply_eye_gaze(const FgbEyeAttributes& in, std::uint64_t attrs,
                           std::uint64_t gaze_bit, std::uint64_t point_bit,
                           std::uint64_t frame_ts_ns, GazeSample& out) noexcept {
    const bool has_gaze  = (attrs & gaze_bit) != 0;
    const bool has_point = (attrs & point_bit) != 0;
    if (!has_gaze && !has_point) {
        return;
    }
    // 逐属性没有单独的点时间戳，两路都用 gaze_ts_ns（0 = 用整帧时刻）
    const std::uint64_t ts = in.gaze_ts_ns != 0 ? in.gaze_ts_ns : frame_ts_ns;
    if (has_gaze) {
        out.valid       = true;
        out.dirLocal    = normalize(Vec3{in.gaze_dir[0], in.gaze_dir[1], in.gaze_dir[2]});
        out.confidence  = clamp01(in.gaze_confidence);
        out.captureTsNs = ts;
    }
    if (has_point) {
        out.pointValid = detail::usable_point(in.gaze_point_2d[0], in.gaze_point_2d[1]);
        out.point2d    = Vec2{in.gaze_point_2d[0], in.gaze_point_2d[1]};
        if (!has_gaze) {
            out.captureTsNs = ts;
        }
    }
}

// 诊断指纹：声明了方向属性、方向却正好是默认的 -Z、同时又给了 2D 点。
// 这是"模块没有方向模型，为了出数据补了一个假方向"的典型写法（PICO 私有通道踩过）。
// 只用来记一条日志，绝不替模块改数据——数据怎么来就怎么给。
inline bool gaze_direction_looks_placeholder(const GazePair& gaze) noexcept {
    const auto is_minus_z = [](const Vec3& v) {
        return v.x == 0.0f && v.y == 0.0f && v.z == -1.0f;
    };
    return gaze.left.valid && gaze.left.pointValid && is_minus_z(gaze.left.dirLocal) &&
           gaze.right.valid && gaze.right.pointValid && is_minus_z(gaze.right.dirLocal);
}

// 诊断用：声明了视口坐标的点应该在 [0,1]（ADR-0033）。留 0.25 余量给标定误差、
// 边缘外视线和"看向画外"；明显超出说明模块标定错了，或者声明错了属性。
// 来源自己的空间没有约定范围，不参与这个检查。只记日志，不改数据。
inline bool gaze_point_out_of_viewport(const GazePair& gaze, float tolerance = 0.25f) noexcept {
    if (!gaze.pointViewport) {
        return false;
    }
    const auto bad = [tolerance](float v) {
        return v < -tolerance || v > 1.0f + tolerance;
    };
    return (gaze.left.pointValid && (bad(gaze.left.point2d.x) || bad(gaze.left.point2d.y))) ||
           (gaze.right.pointValid && (bad(gaze.right.point2d.x) || bad(gaze.right.point2d.y)));
}

} // namespace fgb::native
