// 逐眼投影参数与"方向 → 视口点"的换算。
//
// 为什么放在 core/model 而不是继续留在 VRSS 里：
//   方向型来源要把方向落到"屏幕上哪儿"，这件事不只 VRSS 需要（将来光标、串口输出、
//   第三方消费者都要）。公式只能有一份，否则符号/单位迟早分叉。
//
// 坐标约定：
//   输入方向是 HMD 局部左手系（x 右、y 上、-z 前），与 EyeState::gazeDirLocal 一致；
//   输出是 ADR-0033 的视口点（[0,1]、左上原点、+y 向下）。
//   中间借 OpenVR 的投影切线 (left,right,top,bottom)，与上游 VRSS provider 同式。
#pragma once

#include "core/model/math.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace fgb {

// 单眼投影：直接沿用 OpenVR `GetProjectionRaw` 的四元组 (l, r, t, b) 的**符号约定**——
// OpenVR 的 t 是负数、b 是正数（它按 +y 向下记账），所以校验条件也是 l<0<r、t<0<b。
struct EyeProjection {
    std::array<float, 4> tangent{-0.5f, 0.5f, -0.5f, 0.5f}; // l, r, t(<0), b(>0)
    // eye_to_head 的旋转部分（行优先 3×3，与 EyeGeometry::eye_to_head 同序）。
    std::array<float, 9> rotation{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    bool valid() const noexcept {
        const float l = tangent[0], r = tangent[1], t = tangent[2], b = tangent[3];
        return std::isfinite(l) && std::isfinite(r) && std::isfinite(t) && std::isfinite(b) &&
               l < 0.0f && r > 0.0f && t < 0.0f && b > 0.0f;
    }
};

struct GazeProjection {
    EyeProjection eyes[2]{}; // 0 = 左，1 = 右
    bool valid = false;

    static GazeProjection symmetric(float tanHalfX = 1.0f, float tanHalfY = 1.0f) noexcept {
        GazeProjection projection;
        for (EyeProjection& eye : projection.eyes) {
            eye.tangent = {-tanHalfX, tanHalfX, -tanHalfY, tanHalfY};
        }
        projection.valid = tanHalfX > 0.0f && tanHalfY > 0.0f;
        return projection;
    }
};

// 方向（HMD 局部）→ 该眼视口点。返回 false 表示视线不在眼前方或参数不合法。
inline bool project_dir_to_viewport(Vec3 dir_local, const EyeProjection& eye, float& x,
                                    float& y) noexcept {
    if (!eye.valid()) {
        return false;
    }
    const float len = length(dir_local);
    if (!std::isfinite(len) || len <= 1e-6f) {
        return false;
    }
    const Vec3 dir = dir_local * (1.0f / len);
    const float* r = eye.rotation.data();
    // v = R^T · dir：把 HMD 局部方向转到眼科坐标
    const Vec3 v{r[0] * dir.x + r[3] * dir.y + r[6] * dir.z,
                 r[1] * dir.x + r[4] * dir.y + r[7] * dir.z,
                 r[2] * dir.x + r[5] * dir.y + r[8] * dir.z};
    if (!(v.z < -1e-6f)) {
        return false; // 视线不在眼前方
    }
    const float* p = eye.tangent.data();
    const float nx = v.x / -v.z;
    const float ny = v.y / -v.z;
    // 视口坐标：x 向右、y 向下（ADR-0033 §1）
    x = (nx - p[0]) / (p[1] - p[0]);
    y = (p[3] - ny) / (p[3] - p[2]);
    return std::isfinite(x) && std::isfinite(y);
}

// 视口点 → HMD 局部方向。探针要"把系统认为的注视点画回来"，就得反着算一次。
// 与 project_dir_to_viewport 严格互逆（单测覆盖）。
inline bool unproject_viewport(Vec2 uv, const EyeProjection& eye, Vec3& dir_local) noexcept {
    if (!eye.valid()) {
        return false;
    }
    const float* p = eye.tangent.data();
    const float nx = p[0] + uv.x * (p[1] - p[0]);
    const float ny = p[3] - uv.y * (p[3] - p[2]);
    if (!std::isfinite(nx) || !std::isfinite(ny)) {
        return false;
    }
    const float* r = eye.rotation.data();
    // 眼科坐标 → HMD 局部：v_head = R · (nx, ny, -1)
    const Vec3 local{nx, ny, -1.0f};
    const Vec3 head{r[0] * local.x + r[1] * local.y + r[2] * local.z,
                    r[3] * local.x + r[4] * local.y + r[5] * local.z,
                    r[6] * local.x + r[7] * local.y + r[8] * local.z};
    const float len = length(head);
    if (!std::isfinite(len) || len <= 1e-6f) {
        return false;
    }
    dir_local = head * (1.0f / len);
    return true;
}

} // namespace fgb
