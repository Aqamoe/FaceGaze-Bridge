#include "sinks/vrchat_face.hpp"

#include "core/model/math.hpp"

#include <cmath>

namespace fgb {
namespace {

std::uint8_t quantize(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(clamp01(value) * 255.0f));
}

// 角度 → [-1,1] 的单侧分量。与 v2 的 EyeX / EyeY 用同一个满量程，
// 否则同一次眼动在两条输出路径上的幅度会不一致。
float gaze_component(float deg, float scale) noexcept {
    const float s = scale > 1e-3f ? scale : 30.0f;
    return clamp_sym(deg / s);
}

// 瞳孔按 -1..1 传：正 = 放大，负 = 收缩。模块直接给了归一化值就照用；
// 只有毫米直径时按 VRCFT 的口径换算（5 mm 为中性，0..10 mm → -1..+1）。
float pupil_weight(const EyeState& eye) noexcept {
    if (eye.pupilValid && std::isfinite(eye.pupilDilation) && eye.pupilDilation != 0.0f) {
        return clamp_sym(eye.pupilDilation);
    }
    if (std::isfinite(eye.pupilDiameterMm) && eye.pupilDiameterMm > 0.0f) {
        return clamp_sym(eye.pupilDiameterMm / 5.0f - 1.0f);
    }
    return 0.0f;
}

float positive(float value) noexcept { return value > 0.0f ? value : 0.0f; }
float negative(float value) noexcept { return value < 0.0f ? -value : 0.0f; }

} // namespace

void build_native_face_payload(const FaceFrame& frame, float eye_full_scale_deg,
                               std::span<std::uint8_t, kNativeFaceBytes> out) noexcept {
    const EyeState& right = frame.eyes.right;
    const EyeState& left  = frame.eyes.left;

    // FGB 内部口径：yaw 正 = 向右看，pitch 正 = 向上看。
    // 右眼的"向外"是 +yaw，左眼的"向外"是 -yaw——这就是索引 0/1 与 4/5 不对称的原因。
    const float yaw_r   = gaze_component(right.gazePitchYawDeg.y, eye_full_scale_deg);
    const float pitch_r = gaze_component(right.gazePitchYawDeg.x, eye_full_scale_deg);
    const float yaw_l   = gaze_component(left.gazePitchYawDeg.y, eye_full_scale_deg);
    const float pitch_l = gaze_component(left.gazePitchYawDeg.x, eye_full_scale_deg);

    out[0] = quantize(positive(yaw_r));    // EyeLookOutRight
    out[1] = quantize(negative(yaw_r));    // EyeLookInRight
    out[2] = quantize(positive(pitch_r));  // EyeLookUpRight
    out[3] = quantize(negative(pitch_r));  // EyeLookDownRight
    out[4] = quantize(negative(yaw_l));    // EyeLookOutLeft
    out[5] = quantize(positive(yaw_l));    // EyeLookInLeft
    out[6] = quantize(positive(pitch_l));  // EyeLookUpLeft
    out[7] = quantize(negative(pitch_l));  // EyeLookDownLeft

    out[8]  = quantize(1.0f - right.openness); // EyeClosedRight
    out[9]  = quantize(1.0f - left.openness);  // EyeClosedLeft
    const float pupil_r = pupil_weight(right);
    const float pupil_l = pupil_weight(left);
    out[10] = quantize(positive(pupil_r)); // EyeDilationRight
    out[11] = quantize(positive(pupil_l)); // EyeDilationLeft
    out[12] = quantize(negative(pupil_r)); // EyeConstrictRight
    out[13] = quantize(negative(pupil_l)); // EyeConstrictLeft

    for (std::size_t i = 0; i < kShapeCount; ++i) {
        out[kNativeFaceEyeBytes + i] = quantize(frame.shapes[i]);
    }
}

} // namespace fgb
