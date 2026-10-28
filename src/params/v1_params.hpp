// v1 legacy 参数（VRCFT 4.x / SRanipal 时代）。
//
// 老模型绑的是这些名字：JawOpen、EyeLidLeft、MouthSmileLeft、SmileSadRight…
// 它们没有 v2/ 前缀，和 v2 参数是两套独立命名空间。
//
// 结构分三层，与 VRCFT 一致：
//   1. 37 个 SRanipal 唇形（UnifiedSRanMapper）——从肌肉形状重新解释成 SRanipal 语义
//   2. 91 条合成规则（LipShapeMerger）——(正形状集, 负形状集, 混合方式) 的三元组
//      → 由 tools/gen_v1_tables.py 从上游源码生成，见 v1_lip_tables.inc
//   3. 眼追一族（EyeTrackingParams）——Expanded / ExpandedSqueeze / Toggle 等
//
// 【为什么合成规则要生成而不是手抄】
// 91 条规则里每条只是形状组合的排列，手抄错位不会编译失败，只会让某个表情在某个
// 方向上悄悄失灵。生成保证"表"是上游源码的确定性投影。
#pragma once

#include "params/catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fgb::params {

// 顺序即索引，必须与 VRCFaceTracking 的 SRanipal_LipShape_v2 一致
// （原枚举的显式值不连续，这里按值升序重排，保证数组下标 == 语义）
enum class SRanipalShape : std::uint8_t {
    JawRight = 0,
    JawLeft,
    JawForward,
    JawOpen,
    MouthApeShape,
    MouthUpperRight,
    MouthUpperLeft,
    MouthLowerRight,
    MouthLowerLeft,
    MouthUpperOverturn,
    MouthLowerOverturn,
    MouthPout,
    MouthSmileRight,
    MouthSmileLeft,
    MouthSadRight,
    MouthSadLeft,
    CheekPuffRight,
    CheekPuffLeft,
    CheekSuck,
    MouthUpperUpRight,
    MouthUpperUpLeft,
    MouthLowerDownRight,
    MouthLowerDownLeft,
    MouthUpperInside,
    MouthLowerInside,
    MouthLowerOverlay,
    TongueLongStep1,
    TongueLeft,
    TongueRight,
    TongueUp,
    TongueDown,
    TongueRoll,
    TongueLongStep2,
    TongueUpRightMorph,
    TongueUpLeftMorph,
    TongueDownRightMorph,
    TongueDownLeftMorph,
    Count,
};

inline constexpr std::size_t kSRanipalCount = static_cast<std::size_t>(SRanipalShape::Count);

std::string_view sranipal_name(SRanipalShape s) noexcept;

// SRanipal 语义下的单个唇形值（UnifiedSRanMapper 的等价实现）
float sranipal_value(SRanipalShape s, const EvalContext& c) noexcept;

} // namespace fgb::params
