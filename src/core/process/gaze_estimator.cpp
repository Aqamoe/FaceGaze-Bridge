#include "core/process/gaze_estimator.hpp"

#include <algorithm>
#include <cmath>

namespace fgb {

void GazeEstimator::reset() noexcept {
    has_prev_     = false;
    prev_dir_     = Vec3{0.0f, 0.0f, -1.0f};
    prev_ns_      = 0;
    fixation_     = false;
    fixation_ms_  = 0.0f;
}

void GazeEstimator::update(const FaceFrame& frame, GazeState& gaze,
                           std::uint64_t now_ns) noexcept {
    gaze.ts_ns = now_ns;

    if (!gaze.valid || !frame.eyes.any_valid()) {
        reset();
        gaze.fixation          = false;
        gaze.fixation_ms       = 0.0f;
        gaze.saccade_vel_deg_s = 0.0f;
        return;
    }

    float dt_s = 0.0f;
    if (has_prev_ && now_ns > prev_ns_) {
        dt_s = static_cast<float>(now_ns - prev_ns_) / 1'000'000'000.0f;
    }

    float velocity = 0.0f;
    if (has_prev_ && dt_s > 0.0f && dt_s <= config_.max_gap_s) {
        const Vec3 a = normalize(prev_dir_);
        const Vec3 b = normalize(gaze.dir_local);
        const float cosine = std::clamp(dot(a, b), -1.0f, 1.0f);
        velocity           = std::acos(cosine) * kRadToDeg / dt_s;
    }

    gaze.saccade_vel_deg_s = velocity;

    const bool gap = dt_s > config_.max_gap_s;
    if (gap) {
        // 断档之后不要跨帧累计"注视了多久"，这一帧也不重新进入注视
        fixation_    = false;
        fixation_ms_ = 0.0f;
    } else if (fixation_) {
        if (velocity > config_.saccade_speed_deg_s || dt_s <= 0.0f) {
            fixation_    = false;
            fixation_ms_ = 0.0f;
        } else {
            fixation_ms_ += dt_s * 1000.0f;
        }
    } else if (dt_s > 0.0f && velocity < config_.fixation_speed_deg_s) {
        fixation_ms_ += dt_s * 1000.0f;
        if (fixation_ms_ >= config_.fixation_min_ms) {
            fixation_ = true;
        }
    } else {
        fixation_ms_ = 0.0f;
    }

    gaze.fixation    = fixation_;
    gaze.fixation_ms = fixation_ms_;

    prev_dir_ = gaze.dir_local;
    prev_ns_  = now_ns;
    has_prev_ = true;
}

} // namespace fgb
