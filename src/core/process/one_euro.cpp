#include "core/process/one_euro.hpp"

#include <cmath>

namespace fgb {
namespace {
constexpr float kPi = 3.14159265358979323846f;
} // namespace

float OneEuroFilter::alpha(float hz, float cutoff) noexcept {
    if (cutoff <= 0.0f || hz <= 0.0f) {
        return 1.0f;
    }
    const float tau = 1.0f / (2.0f * kPi * cutoff);
    const float te  = 1.0f / hz;
    return 1.0f / (1.0f + tau / te);
}

float OneEuroFilter::filter(float x) noexcept {
    if (!std::isfinite(x)) {
        return 0.0f;
    }
    // 首个样本直接采用，否则会从 0 缓慢爬上来（表现为开机时所有表情慢慢"长"出来）
    if (!primed_) {
        primed_   = true;
        x_prev_   = x;
        dx_prev_  = 0.0f;
        return x;
    }

    const float dx = (x - x_prev_) * config_.hz;

    const float a_d   = alpha(config_.hz, config_.d_cutoff);
    dx_prev_          = a_d * dx + (1.0f - a_d) * dx_prev_;
    const float cutoff = config_.min_cutoff + config_.beta * std::fabs(dx_prev_);

    const float a      = alpha(config_.hz, cutoff);
    x_prev_            = a * x + (1.0f - a) * x_prev_;
    return x_prev_;
}

FaceFilter::FaceFilter(OneEuroConfig config) : config_(config) { set_config(config); }

void FaceFilter::reset() noexcept {
    for (auto& f : shapes_) {
        f.reset();
    }
    gaze_left_x_.reset();
    gaze_left_y_.reset();
    gaze_right_x_.reset();
    gaze_right_y_.reset();
    openness_left_.reset();
    openness_right_.reset();
    pupil_left_.reset();
    pupil_right_.reset();
}

void FaceFilter::set_config(const OneEuroConfig& config) noexcept {
    config_ = config;
    for (auto& f : shapes_) {
        f.set_config(config);
    }
    gaze_left_x_.set_config(config);
    gaze_left_y_.set_config(config);
    gaze_right_x_.set_config(config);
    gaze_right_y_.set_config(config);
    openness_left_.set_config(config);
    openness_right_.set_config(config);
    pupil_left_.set_config(config);
    pupil_right_.set_config(config);
}

void FaceFilter::process(FaceFrame& frame) noexcept {
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        frame.shapes[i] = shapes_[i].filter(frame.shapes[i]);
    }

    EyeState& l = frame.eyes.left;
    EyeState& r = frame.eyes.right;

    l.gazePitchYawDeg.y = gaze_left_x_.filter(l.gazePitchYawDeg.y);
    l.gazePitchYawDeg.x = gaze_left_y_.filter(l.gazePitchYawDeg.x);
    r.gazePitchYawDeg.y = gaze_right_x_.filter(r.gazePitchYawDeg.y);
    r.gazePitchYawDeg.x = gaze_right_y_.filter(r.gazePitchYawDeg.x);

    l.openness = openness_left_.filter(l.openness);
    r.openness = openness_right_.filter(r.openness);

    l.pupilDiameterMm = pupil_left_.filter(l.pupilDiameterMm);
    r.pupilDiameterMm = pupil_right_.filter(r.pupilDiameterMm);
}

} // namespace fgb
