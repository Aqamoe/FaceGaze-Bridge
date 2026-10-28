// 最小数学类型。刻意不引 glm：core 层保持零依赖，且这里只用到 6 个函数。
//
// 坐标系约定（全项目唯一）：右手系，+Y 上，-Z 前，单位米。
// 与 OpenXR 一致。转 Unity / VRChat 的左手系只在 core/coord 一处做。
#pragma once

#include <cmath>

namespace fgb {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator*(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }

constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }

constexpr float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline float length(Vec3 v) noexcept { return std::sqrt(dot(v, v)); }

inline Vec3 normalize(Vec3 v) noexcept {
    const float len = length(v);
    if (len <= 1e-8f) {
        return {0.0f, 0.0f, -1.0f};
    }
    const float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

constexpr float kRadToDeg = 57.29577951308232f;
constexpr float kDegToRad = 0.017453292519943295f;

// 注视方向（-Z 为前）→ 俯仰/偏航（度）。
// 约定：pitch 正值 = 向上，yaw 正值 = 向右。朝正前方时二者均为 0。
inline Vec2 dir_to_pitch_yaw_deg(Vec3 dir) noexcept {
    const Vec3 n = normalize(dir);
    const float pitch = std::asin(n.y > 1.0f ? 1.0f : (n.y < -1.0f ? -1.0f : n.y)) * kRadToDeg;
    const float yaw   = std::atan2(-n.x, -n.z) * kRadToDeg;
    return {pitch, yaw};
}

// 俯仰/偏航（度）→ 方向向量，与上面互逆。
inline Vec3 pitch_yaw_deg_to_dir(float pitchDeg, float yawDeg) noexcept {
    const float p = pitchDeg * kDegToRad;
    const float y = yawDeg * kDegToRad;
    const float cp = std::cos(p);
    return {-std::sin(y) * cp, std::sin(p), -std::cos(y) * cp};
}

constexpr float clamp01(float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
constexpr float clamp_sym(float v) noexcept { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

} // namespace fgb
