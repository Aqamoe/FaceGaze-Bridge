// Unified Expressions 形状集 —— 与 VRCFaceTracking 的协议层严格对齐。
//
// 【这是协议，不是偏好】
// 顺序取自 VRCFaceTracking.Core/Params/Expressions/UnifiedExpressions.cs 的
// `public enum UnifiedExpressions`（88 项，Max = 88）。IPC 数据包里的
// Expression_Shapes 数组就是按这个顺序排列的 float[89]，索引错一位，
// 整张脸就会错位。**只能在末尾追加，永不可重排或插入。**
//
// 【两层结构，别混淆】
//   本枚举 = 肌肉层基础形状（BrowLowererLeft、JawRight、MouthCornerPullLeft…）
//   v2 参数 = 输出层的合成参数（BrowDownLeft、JawX、MouthSmileLeft…）
// 后者由前者派生，例如：
//   v2/MouthSmileLeft  = MouthCornerPullLeft + MouthCornerSlantLeft
//   v2/JawX            = JawRight - JawLeft
//   v2/EyeLidLeft      = Openness * 0.75 + EyeWideLeft * 0.25
// 派生规则见 VRCFaceTracking.Core/Params/Expressions/UnifiedExpressionsParameters.cs，
// 实现放在 sinks/ 层，不进数据模型。
//
// 眼追（EyeLeftX / EyeRightX / EyeLid 等）同样不属于此枚举：权威数据是
// EyeState 里的视线向量与睁眼度，形状数组里再存一份会造成两处真相。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fgb {

// clang-format off
#define FGB_SHAPE_LIST(X)          \
    /* eye (4) */                  \
    X(EyeSquintRight)              \
    X(EyeSquintLeft)               \
    X(EyeWideRight)                \
    X(EyeWideLeft)                 \
    /* brow (8) */                 \
    X(BrowPinchRight)              \
    X(BrowPinchLeft)               \
    X(BrowLowererRight)            \
    X(BrowLowererLeft)             \
    X(BrowInnerUpRight)            \
    X(BrowInnerUpLeft)             \
    X(BrowOuterUpRight)            \
    X(BrowOuterUpLeft)             \
    /* nose (4) */                 \
    X(NasalDilationRight)          \
    X(NasalDilationLeft)           \
    X(NasalConstrictRight)         \
    X(NasalConstrictLeft)          \
    /* cheek (6) */                \
    X(CheekSquintRight)            \
    X(CheekSquintLeft)             \
    X(CheekPuffRight)              \
    X(CheekPuffLeft)               \
    X(CheekSuckRight)              \
    X(CheekSuckLeft)               \
    /* jaw (8) */                  \
    X(JawOpen)                     \
    X(JawRight)                    \
    X(JawLeft)                     \
    X(JawForward)                  \
    X(JawBackward)                 \
    X(JawClench)                   \
    X(JawMandibleRaise)            \
    X(MouthClosed)                 \
    /* lip suck (6) */             \
    X(LipSuckUpperRight)           \
    X(LipSuckUpperLeft)            \
    X(LipSuckLowerRight)           \
    X(LipSuckLowerLeft)            \
    X(LipSuckCornerRight)          \
    X(LipSuckCornerLeft)           \
    /* lip funnel (4) */           \
    X(LipFunnelUpperRight)         \
    X(LipFunnelUpperLeft)          \
    X(LipFunnelLowerRight)         \
    X(LipFunnelLowerLeft)          \
    /* lip pucker (4) */           \
    X(LipPuckerUpperRight)         \
    X(LipPuckerUpperLeft)          \
    X(LipPuckerLowerRight)         \
    X(LipPuckerLowerLeft)          \
    /* upper lip / lower lip (10) */ \
    X(MouthUpperUpRight)           \
    X(MouthUpperUpLeft)            \
    X(MouthUpperDeepenRight)       \
    X(MouthUpperDeepenLeft)        \
    X(NoseSneerRight)              \
    X(NoseSneerLeft)               \
    X(MouthLowerDownRight)         \
    X(MouthLowerDownLeft)          \
    X(MouthUpperRight)             \
    X(MouthUpperLeft)              \
    X(MouthLowerRight)             \
    X(MouthLowerLeft)              \
    /* smile (4) */                \
    X(MouthCornerPullRight)        \
    X(MouthCornerPullLeft)         \
    X(MouthCornerSlantRight)       \
    X(MouthCornerSlantLeft)        \
    /* sad (4) */                  \
    X(MouthFrownRight)             \
    X(MouthFrownLeft)              \
    X(MouthStretchRight)           \
    X(MouthStretchLeft)            \
    /* mouth detail (8) */         \
    X(MouthDimpleRight)            \
    X(MouthDimpleLeft)             \
    X(MouthRaiserUpper)            \
    X(MouthRaiserLower)            \
    X(MouthPressRight)             \
    X(MouthPressLeft)              \
    X(MouthTightenerRight)         \
    X(MouthTightenerLeft)          \
    /* tongue (12) */              \
    X(TongueOut)                   \
    X(TongueUp)                    \
    X(TongueDown)                  \
    X(TongueRight)                 \
    X(TongueLeft)                  \
    X(TongueRoll)                  \
    X(TongueBendDown)              \
    X(TongueCurlUp)                \
    X(TongueSquish)                \
    X(TongueFlat)                  \
    X(TongueTwistRight)            \
    X(TongueTwistLeft)             \
    /* velum / neck (4) */         \
    X(SoftPalateClose)             \
    X(ThroatSwallow)               \
    X(NeckFlexRight)               \
    X(NeckFlexLeft)
// clang-format on

enum class Shape : std::uint16_t {
#define FGB_SHAPE_ENUM_ENTRY(name) name,
    FGB_SHAPE_LIST(FGB_SHAPE_ENUM_ENTRY)
#undef FGB_SHAPE_ENUM_ENTRY
        Count
};

inline constexpr std::size_t kShapeCount = static_cast<std::size_t>(Shape::Count);

inline constexpr const char* kShapeNames[kShapeCount] = {
#define FGB_SHAPE_NAME_ENTRY(name) #name,
    FGB_SHAPE_LIST(FGB_SHAPE_NAME_ENTRY)
#undef FGB_SHAPE_NAME_ENTRY
};

constexpr std::size_t index_of(Shape s) noexcept { return static_cast<std::size_t>(s); }

constexpr bool is_valid(Shape s) noexcept { return index_of(s) < kShapeCount; }

inline std::string_view shape_name(Shape s) noexcept {
    return is_valid(s) ? std::string_view{kShapeNames[index_of(s)]} : std::string_view{};
}

// 反查，用于 VMC / OSC 输入的地址映射。未知名称返回 false。
bool shape_from_name(std::string_view name, Shape& out) noexcept;

} // namespace fgb
