// v2 参数派生。
//
// 输入是协议层的 88 个肌肉形状 + 眼球状态，输出是 VRChat 认的 FT/v2/* 参数。
// 两者不是一回事：avatar 绑的是 BrowDownLeft，模块给的是 BrowLowererLeft + BrowPinchLeft。
//
// 规则逐条对齐 VRCFaceTracking 的 UnifiedExpressionsParameters.cs / UnifiedSimpleExpressions.cs。
// 改这里之前先确认上游是不是也改了——这些系数（0.75 / 0.25 / 0.8 / 0.2）不是随便定的，
// 它们是 VRCFT 与 avatar 作者之间的既成约定。
#pragma once

#include "params/catalog.hpp"

namespace fgb::params {

// 眼追 + 瞳孔 + 开合 + 简化 + 合成参数。
// 基础形状（88 个 `<形状名>`）不在此表内，由 catalog::v2_base() 提供。
std::span<const ParamEntry> composite_params() noexcept;

// v1 legacy 层要用简化形状做输入（SRanipal 的 MouthSmile 是 CornerPull 与
// CornerSlant 的加权和，不是直接取值），所以这两条单独暴露出来。
float simple_mouth_smile_left(const EvalContext& c) noexcept;
float simple_mouth_smile_right(const EvalContext& c) noexcept;

} // namespace fgb::params
