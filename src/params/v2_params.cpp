#include "params/v2_params.hpp"

#include "core/model/math.hpp"

#include <algorithm>
#include <cmath>

namespace fgb::params {
namespace {

// S(X) 取基础形状值，用来让下面的表尽量接近 VRCFT 源码的可读性
#define S(sh) (c.frame.shapes[index_of(Shape::sh)])

constexpr float eye_norm(float deg, float scale) noexcept {
    const float s = scale > 1e-3f ? scale : 30.0f;
    return clamp_sym(deg / s);
}

// VRCFT 的 Gaze.x → X 参数，Gaze.y → Y 参数
float eye_x(const EyeState& e, const EvalContext& c) noexcept {
    return eye_norm(e.gazePitchYawDeg.y, c.eye_full_scale_deg);
}
float eye_y(const EyeState& e, const EvalContext& c) noexcept {
    return eye_norm(e.gazePitchYawDeg.x, c.eye_full_scale_deg);
}

// UnifiedSimpleExpressions —— 8 个简化形状
float brow_up_right(const EvalContext& c) noexcept {
    return S(BrowOuterUpRight) * 0.60f + S(BrowInnerUpRight) * 0.40f;
}
float brow_up_left(const EvalContext& c) noexcept {
    return S(BrowOuterUpLeft) * 0.60f + S(BrowInnerUpLeft) * 0.40f;
}
float brow_down_right(const EvalContext& c) noexcept {
    return S(BrowLowererRight) * 0.75f + S(BrowPinchRight) * 0.25f;
}
float brow_down_left(const EvalContext& c) noexcept {
    return S(BrowLowererLeft) * 0.75f + S(BrowPinchLeft) * 0.25f;
}
float mouth_smile_right(const EvalContext& c) noexcept {
    return S(MouthCornerPullRight) * 0.8f + S(MouthCornerSlantRight) * 0.2f;
}
float mouth_smile_left(const EvalContext& c) noexcept {
    return S(MouthCornerPullLeft) * 0.8f + S(MouthCornerSlantLeft) * 0.2f;
}
// 注意是取最大值而非求和：frown 与 stretch 是同一表情的两种表现，叠加会翻倍
float mouth_sad_right(const EvalContext& c) noexcept {
    return std::max(S(MouthFrownRight), S(MouthStretchRight));
}
float mouth_sad_left(const EvalContext& c) noexcept {
    return std::max(S(MouthFrownLeft), S(MouthStretchLeft));
}

#define P(param_name, expr) \
    {param_name, [](const EvalContext& c) noexcept -> float { return (expr); }}

constexpr ParamDef kComposite[] = {
    // ---- 眼追（Vector2 参数被 VRCFT 展开成 X / Y 两个 float）----
    P("EyeX", (eye_x(c.frame.eyes.left, c) + eye_x(c.frame.eyes.right, c)) * 0.5f),
    P("EyeY", (eye_y(c.frame.eyes.left, c) + eye_y(c.frame.eyes.right, c)) * 0.5f),
    P("EyeLeftX", eye_x(c.frame.eyes.left, c)),
    P("EyeLeftY", eye_y(c.frame.eyes.left, c)),
    P("EyeRightX", eye_x(c.frame.eyes.right, c)),
    P("EyeRightY", eye_y(c.frame.eyes.right, c)),

    // ---- 瞳孔 ----
    P("PupilDilation", (c.frame.eyes.left.pupilDiameterMm + c.frame.eyes.right.pupilDiameterMm) * 0.5f),
    P("PupilDiameterLeft", c.frame.eyes.left.pupilDiameterMm * 0.1f),
    P("PupilDiameterRight", c.frame.eyes.right.pupilDiameterMm * 0.1f),
    P("PupilDiameter",
      (c.frame.eyes.left.pupilDiameterMm + c.frame.eyes.right.pupilDiameterMm) * 0.05f),

    // ---- 睁眼 / 闭眼 / 眼睑 ----
    P("EyeOpenLeft", c.frame.eyes.left.openness),
    P("EyeOpenRight", c.frame.eyes.right.openness),
    P("EyeOpen", (c.frame.eyes.left.openness + c.frame.eyes.right.openness) * 0.5f),
    P("EyeClosedLeft", 1.0f - c.frame.eyes.left.openness),
    P("EyeClosedRight", 1.0f - c.frame.eyes.right.openness),
    P("EyeClosed", 1.0f - (c.frame.eyes.left.openness + c.frame.eyes.right.openness) * 0.5f),
    // EyeLid 混入 EyeWide：睁到极限时眼睑要跟着抬，否则放大表情会被"眼皮不够高"卡住
    P("EyeLidLeft", c.frame.eyes.left.openness * 0.75f + S(EyeWideLeft) * 0.25f),
    P("EyeLidRight", c.frame.eyes.right.openness * 0.75f + S(EyeWideRight) * 0.25f),
    P("EyeLid", (c.frame.eyes.left.openness + c.frame.eyes.right.openness) * 0.5f * 0.75f +
                     (S(EyeWideRight) + S(EyeWideLeft)) * 0.5f * 0.25f),

    // ---- 眼型 ----
    P("EyeWide", std::max(S(EyeWideLeft), S(EyeWideRight))),
    P("EyeSquint", std::max(S(EyeSquintLeft), S(EyeSquintRight))),
    P("EyesSquint", std::max(S(EyeSquintLeft), S(EyeSquintRight))),

    // ---- 眉毛 ----
    P("BrowUp", (brow_up_right(c) + brow_up_left(c)) * 0.5f),
    P("BrowDown", (brow_down_right(c) + brow_down_left(c)) * 0.5f),
    P("BrowInnerUp", (S(BrowInnerUpLeft) + S(BrowInnerUpRight)) * 0.5f),
    P("BrowOuterUp", (S(BrowOuterUpLeft) + S(BrowOuterUpRight)) * 0.5f),
    // -1 = 生气，+1 = 担忧
    P("BrowExpressionRight",
      std::min(1.0f, S(BrowInnerUpRight) * 0.5f + S(BrowOuterUpRight) * 0.5f) - brow_down_right(c)),
    P("BrowExpressionLeft",
      std::min(1.0f, S(BrowInnerUpLeft) * 0.5f + S(BrowOuterUpLeft) * 0.5f) - brow_down_left(c)),
    P("BrowExpression",
      (std::min(1.0f, (S(BrowInnerUpRight) + S(BrowOuterUpRight)) * 0.5f) - brow_down_right(c) +
       std::min(1.0f, (S(BrowInnerUpLeft) + S(BrowOuterUpLeft)) * 0.5f) - brow_down_left(c)) *
          0.5f),

    // ---- 下颌 ----
    P("JawX", S(JawRight) - S(JawLeft)),
    P("JawZ", S(JawForward) - S(JawBackward)),

    // ---- 脸颊 ----
    P("CheekSquint", (S(CheekSquintLeft) + S(CheekSquintRight)) * 0.5f),
    P("CheekPuffSuckLeft", S(CheekPuffLeft) - S(CheekSuckLeft)),
    P("CheekPuffSuckRight", S(CheekPuffRight) - S(CheekSuckRight)),
    P("CheekPuffSuck",
      (S(CheekPuffRight) + S(CheekPuffLeft)) * 0.5f - (S(CheekSuckRight) + S(CheekSuckLeft)) * 0.5f),
    P("CheekSuck", (S(CheekSuckLeft) + S(CheekSuckRight)) * 0.5f),

    // ---- 嘴部横移 ----
    P("MouthUpperX", S(MouthUpperRight) - S(MouthUpperLeft)),
    P("MouthLowerX", S(MouthLowerRight) - S(MouthLowerLeft)),
    P("MouthX", (S(MouthUpperRight) + S(MouthLowerRight)) * 0.5f -
                    (S(MouthUpperLeft) + S(MouthLowerLeft)) * 0.5f),

    // ---- 唇形 ----
    P("LipSuckUpper", (S(LipSuckUpperRight) + S(LipSuckUpperLeft)) * 0.5f),
    P("LipSuckLower", (S(LipSuckLowerRight) + S(LipSuckLowerLeft)) * 0.5f),
    P("LipSuck", (S(LipSuckUpperRight) + S(LipSuckUpperLeft) + S(LipSuckLowerRight) +
                  S(LipSuckLowerLeft)) *
                     0.25f),
    P("LipFunnelUpper", (S(LipFunnelUpperRight) + S(LipFunnelUpperLeft)) * 0.5f),
    P("LipFunnelLower", (S(LipFunnelLowerRight) + S(LipFunnelLowerLeft)) * 0.5f),
    P("LipFunnel", (S(LipFunnelUpperRight) + S(LipFunnelUpperLeft) + S(LipFunnelLowerRight) +
                    S(LipFunnelLowerLeft)) *
                       0.25f),
    P("LipPuckerUpper", (S(LipPuckerUpperRight) + S(LipPuckerUpperLeft)) * 0.5f),
    P("LipPuckerLower", (S(LipPuckerLowerRight) + S(LipPuckerLowerLeft)) * 0.5f),
    P("LipPuckerRight", (S(LipPuckerUpperRight) + S(LipPuckerLowerRight)) * 0.5f),
    P("LipPuckerLeft", (S(LipPuckerUpperLeft) + S(LipPuckerLowerLeft)) * 0.5f),
    P("LipPucker", (S(LipPuckerUpperRight) + S(LipPuckerUpperLeft) + S(LipPuckerLowerRight) +
                    S(LipPuckerLowerLeft)) *
                       0.25f),

    // 紧凑参数：suck 与 funnel 互斥，差值比单值更能表达"吸"的程度
    P("LipSuckFunnelUpper", (S(LipSuckUpperRight) + S(LipSuckUpperLeft)) * 0.5f -
                                (S(LipFunnelUpperRight) + S(LipFunnelUpperLeft)) * 0.5f),
    P("LipSuckFunnelLower", (S(LipSuckLowerRight) + S(LipSuckLowerLeft)) * 0.5f -
                                (S(LipFunnelLowerRight) + S(LipFunnelLowerLeft)) * 0.5f),
    P("LipSuckFunnelLowerLeft", S(LipSuckLowerLeft) - S(LipFunnelLowerLeft)),
    P("LipSuckFunnelLowerRight", S(LipSuckLowerRight) - S(LipFunnelLowerRight)),
    P("LipSuckFunnelUpperLeft", S(LipSuckUpperLeft) - S(LipFunnelUpperLeft)),
    P("LipSuckFunnelUpperRight", S(LipSuckUpperRight) - S(LipFunnelUpperRight)),

    // ---- 嘴部开合 ----
    P("MouthUpperUp", S(MouthUpperUpRight) * 0.5f + S(MouthUpperUpLeft) * 0.5f),
    P("MouthLowerDown", S(MouthLowerDownRight) * 0.5f + S(MouthLowerDownLeft) * 0.5f),
    P("MouthOpen", S(MouthUpperUpRight) * 0.25f + S(MouthUpperUpLeft) * 0.25f +
                       S(MouthLowerDownRight) * 0.25f + S(MouthLowerDownLeft) * 0.25f),
    P("MouthStretch", (S(MouthStretchRight) + S(MouthStretchLeft)) * 0.5f),
    P("MouthTightener", (S(MouthTightenerRight) + S(MouthTightenerLeft)) * 0.5f),
    P("MouthPress", (S(MouthPressRight) + S(MouthPressLeft)) * 0.5f),
    P("MouthDimple", (S(MouthDimpleRight) + S(MouthDimpleLeft)) * 0.5f),
    P("NoseSneer", (S(NoseSneerRight) + S(NoseSneerLeft)) * 0.5f),

    P("MouthTightenerStretch", (S(MouthTightenerRight) + S(MouthTightenerLeft)) * 0.5f -
                                   (S(MouthStretchRight) + S(MouthStretchLeft)) * 0.5f),
    P("MouthTightenerStretchLeft", S(MouthTightenerLeft) - S(MouthStretchLeft)),
    P("MouthTightenerStretchRight", S(MouthTightenerRight) - S(MouthStretchRight)),

    // ---- 嘴角方向 ----
    P("MouthCornerYLeft", S(MouthCornerSlantLeft) - S(MouthFrownLeft)),
    P("MouthCornerYRight", S(MouthCornerSlantRight) - S(MouthFrownRight)),
    P("MouthCornerY", (S(MouthCornerSlantLeft) - S(MouthFrownLeft) + S(MouthCornerSlantRight) -
                       S(MouthFrownRight)) *
                          0.5f),

    P("SmileFrownRight", mouth_smile_right(c) - S(MouthFrownRight)),
    P("SmileFrownLeft", mouth_smile_left(c) - S(MouthFrownLeft)),
    P("SmileFrown", mouth_smile_right(c) * 0.5f + mouth_smile_left(c) * 0.5f - S(MouthFrownRight) * 0.5f -
                        S(MouthFrownLeft) * 0.5f),

    P("SmileSadRight", mouth_smile_right(c) - mouth_sad_right(c)),
    P("SmileSadLeft", mouth_smile_left(c) - mouth_sad_left(c)),
    P("SmileSad", (mouth_smile_left(c) + mouth_smile_right(c)) * 0.5f -
                      (mouth_sad_left(c) + mouth_sad_right(c)) * 0.5f),

    // ---- 舌头 ----
    P("TongueX", S(TongueRight) - S(TongueLeft)),
    P("TongueY", S(TongueUp) - S(TongueDown)),
    P("TongueArchY", S(TongueCurlUp) - S(TongueBendDown)),
    P("TongueShape", S(TongueFlat) - S(TongueSquish)),
};

#undef P
#undef S

} // namespace

std::span<const ParamDef> composite_params() noexcept {
    return {kComposite, sizeof(kComposite) / sizeof(kComposite[0])};
}

} // namespace fgb::params
