#include "gaze_calib/collector.hpp"

#include <algorithm>
#include <cmath>

namespace fgb::gaze_calib {
namespace {

float median_of(std::array<float, Collector::kMaxSamples>& values, std::size_t count) noexcept {
    if (count == 0) {
        return 0.0f;
    }
    std::sort(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count));
    const std::size_t mid = count / 2;
    if ((count & 1u) != 0u) {
        return values[mid];
    }
    return 0.5f * (values[mid - 1] + values[mid]);
}

// 中位数绝对偏差：对跳变比标准差稳，阈值可以直接写成"视口宽的比例"。
float mad_of(std::array<float, Collector::kMaxSamples> values, std::size_t count,
             float center) noexcept {
    if (count == 0) {
        return 0.0f;
    }
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = std::fabs(values[i] - center);
    }
    return median_of(values, count);
}

} // namespace

void Collector::reset() noexcept {
    buffers_  = {};
    accepted_ = 0;
}

bool Collector::add(std::size_t target, const GazeState& gaze) noexcept {
    if (target >= kTargetCount) {
        return false;
    }
    if (!gaze.left_point_valid || !gaze.right_point_valid) {
        return false; // 两眼都要有位置：单眼的标定只会拟合出半张映射
    }
    if (!std::isfinite(gaze.left_point_2d.x) || !std::isfinite(gaze.left_point_2d.y) ||
        !std::isfinite(gaze.right_point_2d.x) || !std::isfinite(gaze.right_point_2d.y)) {
        return false;
    }
    Buffer& buffer = buffers_[target];
    if (buffer.count >= kMaxSamples) {
        return true; // 已经够了，不再追加但也算"这帧被接受"
    }
    buffer.left_x[buffer.count]  = gaze.left_point_2d.x;
    buffer.left_y[buffer.count]  = gaze.left_point_2d.y;
    buffer.right_x[buffer.count] = gaze.right_point_2d.x;
    buffer.right_y[buffer.count] = gaze.right_point_2d.y;
    ++buffer.count;
    ++accepted_;
    return true;
}

TargetSamples Collector::result(std::size_t target) const noexcept {
    TargetSamples out;
    if (target >= kTargetCount) {
        return out;
    }
    const Buffer& buffer = buffers_[target];
    out.count            = buffer.count;
    if (buffer.count == 0) {
        return out;
    }
    // 拷贝出来排序：原始缓冲要保留，重复读结果时不能因为排序改变语义
    std::array<float, kMaxSamples> scratch = buffer.left_x;
    out.left.x                             = median_of(scratch, buffer.count);
    scratch                                = buffer.left_y;
    out.left.y                             = median_of(scratch, buffer.count);
    scratch                                = buffer.right_x;
    out.right.x                            = median_of(scratch, buffer.count);
    scratch                                = buffer.right_y;
    out.right.y                            = median_of(scratch, buffer.count);

    out.spread_left  = 0.5f * (mad_of(buffer.left_x, buffer.count, out.left.x) +
                               mad_of(buffer.left_y, buffer.count, out.left.y));
    out.spread_right = 0.5f * (mad_of(buffer.right_x, buffer.count, out.right.x) +
                               mad_of(buffer.right_y, buffer.count, out.right.y));

    out.ok = buffer.count >= kMinSamples && out.spread_left <= kMaxSpread &&
             out.spread_right <= kMaxSpread;
    return out;
}

bool Collector::complete() const noexcept {
    for (std::size_t i = 0; i < kTargetCount; ++i) {
        if (!result(i).ok) {
            return false;
        }
    }
    return true;
}

} // namespace fgb::gaze_calib
