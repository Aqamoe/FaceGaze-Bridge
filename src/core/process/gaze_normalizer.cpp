#include "core/process/gaze_normalizer.hpp"

#include <cmath>

namespace fgb {
namespace {

// ts == 0 表示来源没给时间戳：不能因为"不知道时间"就丢数据。
bool fresh(std::uint64_t ts, std::uint64_t now_ns, std::uint64_t limit_ns) noexcept {
    if (ts == 0) {
        return true;
    }
    if (ts > now_ns) {
        return true; // 轻微时钟偏差按有效处理
    }
    return now_ns - ts <= limit_ns;
}

bool finite_point(const Vec2& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

} // namespace

void GazeNormalizer::apply(GazeState& gaze, std::uint64_t now_ns) const noexcept {
    // 1) 空间标签：只管"来源声明了什么"，标定升级是下一段的事。
    //    point_viewport 是模块按 ADR-0033 声明的位，没有它就只能算来源空间。
    gaze.space = gaze.point_viewport ? GazeSpace::viewport : GazeSpace::source;

    const std::uint64_t left_ts  = gaze.left_capture_ts_ns != 0 ? gaze.left_capture_ts_ns
                                                               : gaze.capture_ts_ns;
    const std::uint64_t right_ts = gaze.right_capture_ts_ns != 0 ? gaze.right_capture_ts_ns
                                                                : gaze.capture_ts_ns;

    // 2) 过期：逐眼各自判，一眼过期不连坐另一眼。
    if (gaze.left_point_valid && (!fresh(left_ts, now_ns, kStaleLimitNs) ||
                                  !finite_point(gaze.left_point_2d))) {
        gaze.left_point_valid = false;
    }
    if (gaze.right_point_valid && (!fresh(right_ts, now_ns, kStaleLimitNs) ||
                                   !finite_point(gaze.right_point_2d))) {
        gaze.right_point_valid = false;
    }
    if (gaze.left_valid && !fresh(left_ts, now_ns, kStaleLimitNs)) {
        gaze.left_valid = false;
    }
    if (gaze.right_valid && !fresh(right_ts, now_ns, kStaleLimitNs)) {
        gaze.right_valid = false;
    }

    // 3) 单眼点镜像：消费端（VRSS 的 validMask、光标）要两眼都在，否则会停发。
    //    只镜像点、不镜像方向——方向关系到聚散，凭一眼编另一眼会让 vergence 变假。
    if (gaze.space != GazeSpace::source) {
        if (gaze.left_point_valid && !gaze.right_point_valid) {
            gaze.right_point_2d    = gaze.left_point_2d;
            gaze.right_point_valid = true;
            gaze.right_capture_ts_ns = gaze.left_capture_ts_ns;
        } else if (!gaze.left_point_valid && gaze.right_point_valid) {
            gaze.left_point_2d    = gaze.right_point_2d;
            gaze.left_point_valid = true;
            gaze.left_capture_ts_ns = gaze.right_capture_ts_ns;
        }
    }

    // 4) 兼容字段：新代码读 space，旧 sink 还在读这个布尔。
    gaze.point_viewport = gaze.space != GazeSpace::source;
    gaze.point2d_valid  = gaze.left_point_valid && gaze.right_point_valid;
}

} // namespace fgb
