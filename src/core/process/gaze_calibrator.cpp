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
        // 换档案 = 换映射：饱和历史（上一帧原始值、连续帧计数）必须跟着作废，
        // 否则新档案的前几帧会拿旧档案的原始值去比"值有没有变"。
        if (fresh.version != engine_copy_.version) {
            reset_saturation_state();
        }
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

    // 饱和判定："贴住见过的极值" 且 "连续多帧原始值不再变化"。
    //
    // 【极值参照谁】用**本会话观测到的原始值包络**（只扩不缩），首帧由档案的
    // input_bounds 播种。不能直接用档案：那是"标定时看到过哪儿"，13 点圆盘只覆盖
    // 屏幕中间一块（窄范围 ±20° 时只有 x 0.42..0.75），拿它当传感器极限，正常看
    // 一眼边缘就会被判饱和、位置被冻结在那个小框里。
    // 包络会随着用户真正看向哪里一起长；只要曾经看过更远，那里就不再算边界。
    //
    // 【为什么还要"值不变"】到极值不等于到极限：稳定的注视同样停在极值上。真饱和的
    // 特征是**值被钉住**——再看远也没有新数据。所以要求连续 kSaturatedHoldFrames 帧
    // 值不变，才认为传感器到极限并冻结（冻结是为了不让两眼按各自边界越走越开）。
    if (profile.have_bounds && gaze.raw_point_valid && branch == GazeSourceKind::points) {
        if (!have_env_) {
            for (std::size_t eye = 0; eye < 2; ++eye) {
                env_lo_[eye] = Vec2{profile.input_bounds[eye][0], profile.input_bounds[eye][2]};
                env_hi_[eye] = Vec2{profile.input_bounds[eye][1], profile.input_bounds[eye][3]};
            }
            have_env_ = true;
        }
        const auto at_edge = [](float value, float lo, float hi) {
            const float span = hi - lo;
            const float eps  = span > 1e-6f ? std::max(0.004f, span * 0.04f) : 0.004f;
            return value <= lo + eps || value >= hi - eps;
        };
        const auto unchanged = [](const Vec2& now, const Vec2& prev) {
            return std::fabs(now.x - prev.x) <= kStuckEpsilon &&
                   std::fabs(now.y - prev.y) <= kStuckEpsilon;
        };

        const bool left_at  = gaze.left_point_valid &&
                              (at_edge(gaze.raw_left_point_2d.x, env_lo_[0].x, env_hi_[0].x) ||
                               at_edge(gaze.raw_left_point_2d.y, env_lo_[0].y, env_hi_[0].y));
        const bool right_at = gaze.right_point_valid &&
                              (at_edge(gaze.raw_right_point_2d.x, env_lo_[1].x, env_hi_[1].x) ||
                               at_edge(gaze.raw_right_point_2d.y, env_lo_[1].y, env_hi_[1].y));
        // 没有上一帧可比时不能判"不变"：第一帧贴边只是"刚开始看边缘"
        const bool left_stuck =
            left_at && have_prev_raw_ && unchanged(gaze.raw_left_point_2d, prev_left_raw_);
        const bool right_stuck =
            right_at && have_prev_raw_ && unchanged(gaze.raw_right_point_2d, prev_right_raw_);

        left_hold_  = left_stuck ? left_hold_ + 1 : 0;
        right_hold_ = right_stuck ? right_hold_ + 1 : 0;
        prev_left_raw_  = gaze.raw_left_point_2d;
        prev_right_raw_ = gaze.raw_right_point_2d;
        have_prev_raw_  = true;

        // 判完再并入当前样本：先并入的话"当前值"永远等于包络边界，等于没有参照
        if (gaze.left_point_valid) {
            env_lo_[0].x = std::min(env_lo_[0].x, gaze.raw_left_point_2d.x);
            env_lo_[0].y = std::min(env_lo_[0].y, gaze.raw_left_point_2d.y);
            env_hi_[0].x = std::max(env_hi_[0].x, gaze.raw_left_point_2d.x);
            env_hi_[0].y = std::max(env_hi_[0].y, gaze.raw_left_point_2d.y);
        }
        if (gaze.right_point_valid) {
            env_lo_[1].x = std::min(env_lo_[1].x, gaze.raw_right_point_2d.x);
            env_lo_[1].y = std::min(env_lo_[1].y, gaze.raw_right_point_2d.y);
            env_hi_[1].x = std::max(env_hi_[1].x, gaze.raw_right_point_2d.x);
            env_hi_[1].y = std::max(env_hi_[1].y, gaze.raw_right_point_2d.y);
        }

        gaze.saturated = left_hold_ >= kSaturatedHoldFrames ||
                         right_hold_ >= kSaturatedHoldFrames;
        if (gaze.saturated && have_last_) {
            gaze.left_point_2d  = last_left_;
            gaze.right_point_2d = last_right_;
        } else if (gaze.left_point_valid && gaze.right_point_valid) {
            last_left_  = gaze.left_point_2d;
            last_right_ = gaze.right_point_2d;
            have_last_  = true;
        }
    } else {
        // 没有范围信息（旧档案）或不是点型分支：不判饱和，也就不会冻结
        gaze.saturated  = false;
        left_hold_      = 0;
        right_hold_     = 0;
        have_prev_raw_  = false;
    }
}

} // namespace fgb
