// v2 参数派生。
//
// 输入是协议层的 88 个肌肉形状 + 眼球状态，输出是 VRChat 认的 FT/v2/* 参数。
// 两者不是一回事：avatar 绑的是 BrowDownLeft，模块给的是 BrowLowererLeft + BrowPinchLeft。
//
// 规则逐条对齐 VRCFaceTracking 的 UnifiedExpressionsParameters.cs / UnifiedSimpleExpressions.cs。
// 改这里之前先确认上游是不是也改了——这些系数（0.75 / 0.25 / 0.8 / 0.2）不是随便定的，
// 它们是 VRCFT 与 avatar 作者之间的既成约定。
#pragma once

#include "core/model/frame.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace fgb::params {

struct EvalContext {
    const FaceFrame& frame;
    float eye_full_scale_deg = 30.0f;
};

struct ParamDef {
    std::string_view name; // 不含 "FT/v2/" 前缀
    float (*eval)(const EvalContext&) noexcept;
};

// 眼追 + 瞳孔 + 开合 + 简化 + 合成参数。
// 基础形状（88 个 `v2/<枚举名>`）不在此表内，由调用方直接遍历 frame.shapes。
std::span<const ParamDef> composite_params() noexcept;

// 参数总数上限，供调用方分配去重数组
constexpr std::size_t kMaxParamCount = 256;

} // namespace fgb::params
