// Unified Expressions v2 形状集。
//
// 顺序即线上协议顺序，**只能在末尾追加，永不可重排或插入**。
// 枚举与名称表由同一份 X-macro 列表生成，从机制上杜绝两者错位。
//
// 注意：眼追参数（EyeLeftX / EyeRightX / EyeY / EyeLidLeft / EyeLidRight）不在
// 此枚举内。它们由 EyeState 派生输出，见 core/model/frame.hpp 与 sinks/vrchat_sink.cpp。
// 这不是疏漏——眼追的权威数据是视线向量与睁眼度，把它们再塞进形状数组会造成
// 两处真相，迟早不一致。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fgb {

// clang-format off
#define FGB_SHAPE_LIST(X)  \
    /* brow (11) */        \
    X(BrowDownLeft)        \
    X(BrowDownRight)       \
    X(BrowInnerUp)         \
    X(BrowInnerUpLeft)     \
    X(BrowInnerUpRight)    \
    X(BrowLowererLeft)     \
    X(BrowLowererRight)    \
    X(BrowOuterUpLeft)     \
    X(BrowOuterUpRight)    \
    X(BrowPinchLeft)       \
    X(BrowPinchRight)      \
    /* eye (2) */          \
    X(EyeSquintLeft)       \
    X(EyeSquintRight)      \
    /* cheek (5) */        \
    X(CheekPuffSuck)       \
    X(CheekPuffSuckLeft)   \
    X(CheekPuffSuckRight)  \
    X(CheekSquintLeft)     \
    X(CheekSquintRight)    \
    /* jaw (5) */          \
    X(JawClench)           \
    X(JawMandibleRaise)    \
    X(JawOpen)             \
    X(JawX)                \
    X(JawZ)                \
    /* lip funnel (5) */   \
    X(LipFunnel)           \
    X(LipFunnelLowerLeft)  \
    X(LipFunnelLowerRight) \
    X(LipFunnelUpperLeft)  \
    X(LipFunnelUpperRight) \
    /* lip pucker (5) */   \
    X(LipPucker)           \
    X(LipPuckerLowerLeft)  \
    X(LipPuckerLowerRight) \
    X(LipPuckerUpperLeft)  \
    X(LipPuckerUpperRight) \
    /* lip suck (8) */     \
    X(LipSuckCornerLeft)   \
    X(LipSuckCornerRight)  \
    X(LipSuckLower)        \
    X(LipSuckLowerLeft)    \
    X(LipSuckLowerRight)   \
    X(LipSuckUpper)        \
    X(LipSuckUpperLeft)    \
    X(LipSuckUpperRight)   \
    /* mouth (27) */       \
    X(MouthClosed)         \
    X(MouthCornerPullLeft) \
    X(MouthCornerPullRight)\
    X(MouthCornerSlantLeft)\
    X(MouthCornerSlantRight)\
    X(MouthDimpleLeft)     \
    X(MouthDimpleRight)    \
    X(MouthFrownLeft)      \
    X(MouthFrownRight)     \
    X(MouthLowerDownLeft)  \
    X(MouthLowerDownRight) \
    X(MouthLowerX)         \
    X(MouthPressLeft)      \
    X(MouthPressRight)     \
    X(MouthRaiserLower)    \
    X(MouthRaiserUpper)    \
    X(MouthSmileLeft)      \
    X(MouthSmileRight)     \
    X(MouthStretchLeft)    \
    X(MouthStretchRight)   \
    X(MouthTightenerLeft)  \
    X(MouthTightenerRight) \
    X(MouthUpperDeepenLeft)\
    X(MouthUpperDeepenRight)\
    X(MouthUpperUpLeft)    \
    X(MouthUpperUpRight)   \
    X(MouthUpperX)         \
    /* nasal (4) */        \
    X(NasalConstrictLeft)  \
    X(NasalConstrictRight) \
    X(NasalDilationLeft)   \
    X(NasalDilationRight)  \
    /* neck (2) */         \
    X(NeckFlexLeft)        \
    X(NeckFlexRight)       \
    /* nose (2) */         \
    X(NoseSneerLeft)       \
    X(NoseSneerRight)      \
    /* velum / throat (2) */ \
    X(SoftPalateClose)     \
    X(ThroatSwallow)       \
    /* tongue (8) */       \
    X(TongueArchY)         \
    X(TongueOut)           \
    X(TongueRoll)          \
    X(TongueShape)         \
    X(TongueTwistLeft)     \
    X(TongueTwistRight)    \
    X(TongueX)             \
    X(TongueY)
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
