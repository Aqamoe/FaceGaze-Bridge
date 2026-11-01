#include "core/process/gaze_calibrator.hpp"

#include <algorithm>
#include <cmath>

namespace fgb {
namespace {

bool finite_point(const Vec2& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

// 把一眼的方向投影成视口点；返回 false 表示这一眼看不出屏幕位置。
bool project_eye(Vec3 dir_local, const EyeProjection& eye, Vec2& out) noexcept {
    float x = 0.0f;
    float y = 0.0f;
    if (!project_dir_to_viewport(dir_local, eye, x, y)) {
        return false;
    }
    out = Vec2{x, y};
    return true;
}

} // namespace

void GazeCalibrator::set_profile(const GazeProfile& profile) noexcept {
    profile_snapshot_.publish(profile);
    engine_copy_ = profile;
}

void GazeCalibrator::set_projection(const GazeProjection& projection) noexcept {
    projection_ = projection;
}

GazeProfile GazeCalibrator::profile() const noexcept {
    GazeProfile copy;
    if (!profile_snapshot_.read(copy)) {
        copy = GazeProfile{};
    }
    return copy;
}

void GazeCalibrator::apply(GazeState& gaze) noexcept {
    // 快照读失败说明控制线程正在替换档案（几百纳秒）；沿用上一帧比"这帧不标定"平滑。
    GazeProfile fresh;
    if (profile_snapshot_.read(fresh)) {
        engine_copy_ = fresh;
    }
    const GazeProfile& profile = engine_copy_;

    const bool has_points = gaze.left_point_valid || gaze.right_point_valid;
    const bool has_dirs   = gaze.left_valid || gaze.right_valid;

    // 原始点留档：诊断与饱和判定都要看"标定前的值"，映射之后就看不到边界了
    if (has_points && gaze.space == GazeSpace::source) {
        gaze.raw_left_point_2d  = gaze.left_point_2d;
        gaze.raw_right_point_2d = gaze.right_point_2d;
        gaze.raw_point_valid    = true;
    }

    GazeSourceKind branch = GazeSourceKind::none;

    if (has_points && gaze.space == GazeSpace::viewport) {
        // 模块已声明该眼视口坐标，无需换算；档案只做小幅校正。
        branch = GazeSourceKind::points;
    } else if (has_dirs) {
        // 有方向时优先用几何投影：它比"来源空间的点"可信得多。
        if (!projection_.valid || !projection_.eyes[0].valid() || !projection_.eyes[1].valid()) {
            gaze.geometry_missing = true;
            return; // 有方向但没几何：不改任何字段，让消费端自己判"没有位置"
        }
        Vec2 left{};
        Vec2 right{};
        const bool left_ok  = gaze.left_valid && project_eye(gaze.left_dir_local, projection_.eyes[0], left);
        const bool right_ok = gaze.right_valid && project_eye(gaze.right_dir_local, projection_.eyes[1], right);
        if (!left_ok && !right_ok) {
            gaze.geometry_missing = false;
            return; // 两眼都看不到屏幕（视线在眼后），保持原状
        }
        if (!left_ok) {
            left = right;
        }
        if (!right_ok) {
            right = left;
        }
        gaze.left_point_2d    = left;
        gaze.right_point_2d   = right;
        gaze.left_point_valid = true;
        gaze.right_point_valid = true;
        gaze.space            = GazeSpace::viewport;
        branch                = GazeSourceKind::angles;
    } else if (has_points) {
        // 只有来源空间的点：等档案来映射。
        branch = GazeSourceKind::points;
    } else {
        return;
    }

    gaze.geometry_missing = false;

    // 档案只对"拟合时用的那条分支"有效。分支不匹配时保持现状——硬套会把点推到画面外。
    if (!profile.ready() || profile.source_kind != branch) {
        return;
    }

    bool applied = false;
    for (std::size_t i = 0; i < 2; ++i) {
        Vec2& point = i == 0 ? gaze.left_point_2d : gaze.right_point_2d;
        bool& valid = i == 0 ? gaze.left_point_valid : gaze.right_point_valid;
        const GazeEyeMap& map = profile.eye(i);
        if (!valid || !finite_point(point)) {
            continue;
        }
        if (!map.active) {
            continue; // 该眼没映射：原样透传，不做"半只眼校准"
        }
        float x = 0.0f;
        float y = 0.0f;
        map.apply(point.x, point.y, x, y);
        if (!std::isfinite(x) || !std::isfinite(y)) {
            valid = false; // 映射把点算炸了：这一眼判无效，别发出去
            continue;
        }
        point   = Vec2{x, y};
        applied = true;
    }

    if (applied) {
        gaze.space           = GazeSpace::calibrated;
        gaze.profile_version = profile.version;
        gaze.residual        = profile.residual_p95;
        gaze.point_viewport  = true;
    }

    // 饱和判定：原始值贴到标定时见过的边界，就说明传感器到极限了。
    // 这时如果继续按新值算，两眼会因为各自的边界不同而越走越开（实测就是这样），
    // 所以冻结在最后一对有效位置上，并打上标记让探针/页面说清楚。
    if (profile.have_bounds && gaze.raw_point_valid && branch == GazeSourceKind::points) {
        const auto near_bound = [](float value, float lo, float hi) {
            const float span = hi - lo;
            const float eps  = span > 1e-6f ? std::max(0.004f, span * 0.04f) : 0.004f;
            return value <= lo + eps || value >= hi - eps;
        };
        const bool left_sat = gaze.left_point_valid &&
                              (near_bound(gaze.raw_left_point_2d.x, profile.input_bounds[0][0],
                                          profile.input_bounds[0][1]) ||
                               near_bound(gaze.raw_left_point_2d.y, profile.input_bounds[0][2],
                                          profile.input_bounds[0][3]));
        const bool right_sat = gaze.right_point_valid &&
                               (near_bound(gaze.raw_right_point_2d.x, profile.input_bounds[1][0],
                                           profile.input_bounds[1][1]) ||
                                near_bound(gaze.raw_right_point_2d.y, profile.input_bounds[1][2],
                                           profile.input_bounds[1][3]));
        gaze.saturated = left_sat || right_sat;
        if (gaze.saturated && have_last_) {
            gaze.left_point_2d  = last_left_;
            gaze.right_point_2d = last_right_;
        } else if (!gaze.saturated && gaze.left_point_valid && gaze.right_point_valid) {
            last_left_  = gaze.left_point_2d;
            last_right_ = gaze.right_point_2d;
            have_last_  = true;
        }
    }
}

} // namespace fgb
