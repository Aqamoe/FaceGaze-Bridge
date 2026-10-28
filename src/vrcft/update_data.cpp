#include "vrcft/update_data.hpp"

#include <cmath>
#include <cstring>

namespace fgb::vrcft {
namespace {

float read_f32(const std::byte* p) noexcept {
    std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                         (static_cast<std::uint32_t>(p[2]) << 16) |
                         (static_cast<std::uint32_t>(p[3]) << 24);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::int32_t read_i32(const std::byte* p) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                     (static_cast<std::uint32_t>(p[1]) << 8) |
                                     (static_cast<std::uint32_t>(p[2]) << 16) |
                                     (static_cast<std::uint32_t>(p[3]) << 24));
}

// VRCFT 侧是 Unity 左手系（+X 右, +Y 上, +Z 前），头部欧拉角为度。
// 这里转成内部的 OpenXR 右手系四元数。
Quat euler_deg_to_quat(float yaw_deg, float pitch_deg, float roll_deg) noexcept {
    const float hy = yaw_deg * kDegToRad * 0.5f;
    const float hp = pitch_deg * kDegToRad * 0.5f;
    const float hr = roll_deg * kDegToRad * 0.5f;

    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cp = std::cos(hp), sp = std::sin(hp);
    const float cr = std::cos(hr), sr = std::sin(hr);

    // Y * X * Z 顺序（Unity 的欧拉约定），再翻 Z 轴进右手系
    Quat q;
    q.w = cy * cp * cr + sy * sp * sr;
    q.x = cy * sp * cr + sy * cp * sr;
    q.y = sy * cp * cr - cy * sp * sr;
    q.z = cy * cp * sr - sy * sp * cr;
    q.z = -q.z;
    return q;
}

} // namespace

bool UpdateData::has_any_valid() const noexcept {
    return has_eye_data() || has_expression_data() || !is_invalid(head_pitch);
}

bool UpdateData::has_eye_data() const noexcept {
    return !is_invalid(left_gaze_x) || !is_invalid(right_gaze_x) || !is_invalid(left_openness) ||
           !is_invalid(right_openness);
}

bool UpdateData::has_expression_data() const noexcept {
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        if (!is_invalid(shapes[i])) {
            return true;
        }
    }
    return false;
}

bool decode_update(std::span<const std::byte> body, UpdateData& out) noexcept {
    if (body.size() < 4) {
        return false;
    }
    const std::int32_t struct_size = read_i32(body.data());
    if (struct_size != static_cast<std::int32_t>(kUpdateStructSize)) {
        return false; // 协议版本不匹配，宁可丢弃也不要错位解析
    }
    if (body.size() < 4 + kUpdateStructSize) {
        return false;
    }

    const std::byte* p = body.data() + 4;
    out.max_dilation   = read_f32(p + 0);
    out.min_dilation   = read_f32(p + 4);
    out.left_gaze_x    = read_f32(p + 8);
    out.left_gaze_y    = read_f32(p + 12);
    out.left_pupil_mm  = read_f32(p + 16);
    out.left_openness  = read_f32(p + 20);
    out.right_gaze_x   = read_f32(p + 24);
    out.right_gaze_y   = read_f32(p + 28);
    out.right_pupil_mm = read_f32(p + 32);
    out.right_openness = read_f32(p + 36);
    out.head_yaw       = read_f32(p + 40);
    out.head_pitch     = read_f32(p + 44);
    out.head_roll      = read_f32(p + 48);
    out.head_pos_x     = read_f32(p + 52);
    out.head_pos_y     = read_f32(p + 56);
    out.head_pos_z     = read_f32(p + 60);

    for (std::size_t i = 0; i < kExpressionCount; ++i) {
        out.shapes[i] = read_f32(p + 64 + i * 4);
    }
    return true;
}

bool apply_to_frame(const UpdateData& data, const ConvertOptions& options, FaceFrame& frame) noexcept {
    const float scale = options.eye_full_scale_deg > 1e-3f ? options.eye_full_scale_deg : 30.0f;
    bool wrote_eye    = false;

    auto apply_eye = [&](EyeState& eye, float gaze_x, float gaze_y, float pupil_mm, float openness) {
        if (!is_invalid(openness)) {
            eye.openness     = clamp01(openness);
            eye.opennessConf = 1.0f;
            wrote_eye        = true;
        }
        if (!is_invalid(gaze_x) && !is_invalid(gaze_y)) {
            // VRCFT 的 Gaze 是归一化方向：x 右正，y 上正
            const float yaw_deg   = clamp_sym(gaze_x) * scale;
            const float pitch_deg = clamp_sym(gaze_y) * scale;
            eye.gazePitchYawDeg   = {pitch_deg, yaw_deg};
            eye.gazeDirLocal      = pitch_yaw_deg_to_dir(pitch_deg, yaw_deg);
            eye.gazeConf          = 1.0f;
            wrote_eye             = true;
        }
        if (!is_invalid(pupil_mm) && pupil_mm > 0.0f) {
            eye.pupilDiameterMm = pupil_mm;
        }
        if (wrote_eye) {
            eye.valid = true;
        }
    };

    apply_eye(frame.eyes.left, data.left_gaze_x, data.left_gaze_y, data.left_pupil_mm,
              data.left_openness);
    apply_eye(frame.eyes.right, data.right_gaze_x, data.right_gaze_y, data.right_pupil_mm,
              data.right_openness);

    if (!is_invalid(data.head_yaw) && !is_invalid(data.head_pitch) && !is_invalid(data.head_roll)) {
        frame.hasHeadPose  = true;
        frame.headRotLocal = euler_deg_to_quat(data.head_yaw, data.head_pitch, data.head_roll);
    }
    if (!is_invalid(data.head_pos_x) && !is_invalid(data.head_pos_y) && !is_invalid(data.head_pos_z)) {
        // Unity (x, y, z) -> OpenXR (x, y, -z)
        frame.headPosLocal = {data.head_pos_x, data.head_pos_y, -data.head_pos_z};
    }

    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const float v = data.shapes[i];
        if (is_invalid(v)) {
            continue;
        }
        frame.shapes[i]    = clamp01(v);
        frame.shapeValid[i] = 1;
    }
    return wrote_eye;
}

} // namespace fgb::vrcft
