// 模块 ABI 的注视字段 → GazePair / GazeSample 的映射。
//
// 为什么单独抽出来：SDK 明文写着「逐眼/合并方向与 2D 点分别有效，互不连坐」
// （fgb_module_api.h 的 FGB_GAZE_* 注释），但这条路径以前把点位写在方向位的 if 里，
// 只声明 FGB_GAZE_POINT_2D 的模块会被整帧丢掉——PICO Connect 的私有注视通道只能
// 伪造一个 (0,0,-1) 方向来绕过，副作用是注视/扫视判定永远「注视中」。
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

// v1 独立注视通道（FgbFrameSlotV1::gaze）。
// 方向位与 POINT_2D 位各自独立：只有点的帧也照样收，方向保持默认正前方且标记无效。
inline void apply_gaze_v1(const FgbGazeV1& in, GazePair& out) noexcept {
    const bool left_dir  = (in.valid & FGB_GAZE_LEFT) != 0;
    const bool right_dir = (in.valid & FGB_GAZE_RIGHT) != 0;
    const bool combined  = (in.valid & FGB_GAZE_COMBINED) != 0;
    const bool points    = (in.valid & FGB_GAZE_POINT_2D) != 0;
    if (!left_dir && !right_dir && !combined && !points) {
        return; // 本帧没有注视数据，保持上一次的 GazePair 不动
    }

    out.left.valid  = left_dir;
    out.right.valid = right_dir;
    if (left_dir) {
        out.left.dirLocal = normalize(Vec3{in.left_dir[0], in.left_dir[1], in.left_dir[2]});
    }
    if (right_dir) {
        out.right.dirLocal = normalize(Vec3{in.right_dir[0], in.right_dir[1], in.right_dir[2]});
    }
    out.combinedValid = combined;
    if (combined) {
        out.combinedDirLocal =
            normalize(Vec3{in.combined_dir[0], in.combined_dir[1], in.combined_dir[2]});
    }

    // 点位有效性只看 POINT_2D 位：挂在方向上就等于要求模块伪造方向。
    // 但 NaN/Inf 要单独否掉——那是"没算出来"，不是"看向角落"。
    out.left.pointValid =
        points && detail::usable_point(in.left_point_2d[0], in.left_point_2d[1]);
    out.right.pointValid =
        points && detail::usable_point(in.right_point_2d[0], in.right_point_2d[1]);
    out.pointsValid = out.left.pointValid && out.right.pointValid;
    // 有没有标定到视口（ADR-0033）。默认按"来源空间"处理：raw 数据很常见，
    // 猜成视口坐标会把 foveation 带到错误的位置。
    out.pointViewport = (in.valid & FGB_GAZE_POINT_VIEWPORT) != 0;
    out.left.point2d  = Vec2{in.left_point_2d[0], in.left_point_2d[1]};
    out.right.point2d = Vec2{in.right_point_2d[0], in.right_point_2d[1]};

    out.confidence              = clamp01(in.confidence);
    out.captureTsNs             = in.capture_ts_ns;
    out.left.confidence         = out.confidence;
    out.right.confidence        = out.confidence;
    out.left.captureTsNs        = in.capture_ts_ns;
    out.right.captureTsNs       = in.capture_ts_ns;
}

// 诊断指纹：声明了方向位、方向却正好是默认的 -Z、同时又给了 2D 点。
// 这是"模块没有方向模型，为了出数据补了一个假方向"的典型写法（PICO 私有通道踩过）。
// 只用来记一条日志，绝不替模块改数据——数据怎么来就怎么给。
inline bool gaze_direction_looks_placeholder(const GazePair& gaze) noexcept {
    const auto is_minus_z = [](const Vec3& v) {
        return v.x == 0.0f && v.y == 0.0f && v.z == -1.0f;
    };
    return gaze.left.valid && gaze.left.pointValid && is_minus_z(gaze.left.dirLocal) &&
           gaze.right.valid && gaze.right.pointValid && is_minus_z(gaze.right.dirLocal);
}

// v2 属性级（FgbFrameSlotV2 的 left_eye/right_eye）。
// 单眼方向与单眼点位同样是两个独立属性位：只给点的眼也有数据。
inline void apply_eye_gaze_v2(const FgbEyeAttributesV2& in, std::uint64_t attrs,
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

// 诊断用：声明了 POINT_VIEWPORT 的点应该在 [0,1]（ADR-0033）。留 0.25 余量给标定误差、
// 边缘外视线和"看向画外"；明显超出说明模块标定错了，或者声明错了位。
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
