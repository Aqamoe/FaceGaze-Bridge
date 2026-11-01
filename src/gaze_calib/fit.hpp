// 标定拟合：逐眼 2D 仿射 + 残差（ADR-0042 §5）。
//
// 只用仿射（6 自由度）：13 个点里 6 个自由度还能被数据约束住；单应 8 自由度在
// 点噪声稍大时会拟合出"穿过标定点、中间乱扭"的映射——那种映射在头显里表现为
// 注视点在某些区域突然甩出去，比线性偏差难查得多。
#pragma once

#include "core/model/gaze_profile.hpp"
#include "core/model/math.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace fgb::gaze_calib {

struct Pair {
    Vec2 observed; // 标定前的观察（来源空间的点，或投影出来的视口点）
    Vec2 target;   // 该眼视口坐标（由目标视线经 HMD 投影算出）
};

struct EyeFit {
    bool ok          = false;
    GazeEyeMap map{};
    float residual_p95 = -1.0f; // 视口宽比例；-1 = 未计算
    std::size_t used   = 0;
    const char* reason = "";    // ok=false 时说明原因（英文标识，中文交给 UI）
};

// 最小二乘拟合 X' = a·x + b·y + c、Y' = d·x + e·y + f。
// 拒绝：样本太少、输入点几乎共线、拟合结果离谱（会把手抖拟合成放大 20 倍）。
EyeFit fit_affine(const std::vector<Pair>& pairs) noexcept;

// 把左右眼拟合结果组成档案。任一眼失败则整体失败（不允许"半只眼标定"）。
bool build_profile(GazeSourceKind kind, const EyeFit& left, const EyeFit& right,
                   std::uint32_t version, std::string created_utc, std::string hmd_identity,
                   GazeProfile& out);

} // namespace fgb::gaze_calib
