#include "params/v1_params.hpp"

#include "core/model/math.hpp"
#include "params/v2_params.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fgb::params {
namespace {

#define S(sh) (c.frame.shapes[index_of(Shape::sh)])

constexpr float kAvg(float a, float b) noexcept { return (a + b) * 0.5f; }

// ---------------------------------------------------------------------------
// SRanipal 唇形（等价于 VRCFT UnifiedSRanMapper.ShapeMap）
// ---------------------------------------------------------------------------

// VRCFT 的 Squeeze：越睁眼越不参与，用来把"眯眼"和"闭眼"区分开
float squeeze(const EyeState& e, float squint) noexcept {
    const float o = std::clamp(e.openness, 0.0f, 1.0f);
    return (1.0f - std::pow(o, 0.15f)) * squint;
}

float sranipal_sad(const EvalContext& c, float stretchSide, float smileSide) noexcept {
    // 原文：max(0, (avg(frownL, frownR) > stretch ? avg : stretch) - smile)
    const float frown = kAvg(S(MouthFrownRight), S(MouthFrownLeft));
    const float base  = frown > stretchSide ? frown : stretchSide;
    return std::max(0.0f, base - smileSide);
}

using SRanipalFn = float (*)(const EvalContext&) noexcept;

float sr_jaw_open(const EvalContext& c) noexcept {
    return std::clamp(S(JawOpen) - S(MouthClosed), 0.0f, 1.0f);
}
float sr_upper_overturn(const EvalContext& c) noexcept {
    return kAvg(S(LipFunnelUpperLeft), S(LipFunnelUpperRight));
}
float sr_lower_overturn(const EvalContext& c) noexcept {
    return kAvg(S(LipFunnelLowerLeft), S(LipFunnelLowerRight));
}
float sr_pout(const EvalContext& c) noexcept {
    return (S(LipPuckerUpperLeft) + S(LipPuckerUpperRight) + S(LipPuckerLowerLeft) +
            S(LipPuckerLowerRight)) *
           0.25f;
}
float sr_smile_right(const EvalContext& c) noexcept {
    return std::max(simple_mouth_smile_right(c), S(MouthDimpleRight));
}
float sr_smile_left(const EvalContext& c) noexcept {
    return std::max(simple_mouth_smile_left(c), S(MouthDimpleLeft));
}
float sr_sad_right(const EvalContext& c) noexcept {
    return sranipal_sad(c, S(MouthStretchRight), sr_smile_right(c));
}
float sr_sad_left(const EvalContext& c) noexcept {
    return sranipal_sad(c, S(MouthStretchLeft), sr_smile_left(c));
}
float sr_cheek_suck(const EvalContext& c) noexcept {
    return kAvg(S(CheekSuckLeft), S(CheekSuckRight));
}
float sr_upper_up_right(const EvalContext& c) noexcept {
    return std::max(0.0f, S(MouthUpperUpRight) +
                               (1.0f - S(LipPuckerUpperRight)) * S(LipFunnelUpperRight));
}
float sr_upper_up_left(const EvalContext& c) noexcept {
    return std::max(0.0f, S(MouthUpperUpLeft) +
                               (1.0f - S(LipPuckerUpperLeft)) * S(LipFunnelUpperLeft));
}
float sr_lower_down_right(const EvalContext& c) noexcept {
    return std::max(0.0f, S(MouthLowerDownRight) +
                               (1.0f - S(LipPuckerLowerRight)) * S(LipFunnelLowerRight));
}
float sr_lower_down_left(const EvalContext& c) noexcept {
    return std::max(0.0f, S(MouthLowerDownLeft) +
                               (1.0f - S(LipPuckerLowerLeft)) * S(LipFunnelLowerLeft));
}
float sr_upper_inside(const EvalContext& c) noexcept {
    return std::max(0.0f, kAvg(S(LipSuckUpperLeft), S(LipSuckUpperRight)));
}
float sr_lower_inside(const EvalContext& c) noexcept {
    return std::max(0.0f, kAvg(S(LipSuckLowerLeft), S(LipSuckLowerRight)));
}
float sr_tongue_step1(const EvalContext& c) noexcept { return std::min(1.0f, S(TongueOut) * 2.0f); }
float sr_tongue_step2(const EvalContext& c) noexcept {
    return std::min(1.0f, std::max(0.0f, S(TongueOut) * 2.0f - 1.0f));
}
float sr_zero(const EvalContext&) noexcept { return 0.0f; }

#define SR(sh) [](const EvalContext& c) noexcept -> float { return S(sh); }

constexpr SRanipalFn kSRanipal[] = {
    // 索引与 SRanipalShape 一一对应，顺序错 = 表情错位
    SR(JawRight),          // 0  JawRight
    SR(JawLeft),           // 1  JawLeft
    SR(JawForward),        // 2  JawForward
    sr_jaw_open,           // 3  JawOpen
    SR(MouthClosed),       // 4  MouthApeShape
    SR(MouthUpperRight),   // 5
    SR(MouthUpperLeft),    // 6
    SR(MouthLowerRight),   // 7
    SR(MouthLowerLeft),    // 8
    sr_upper_overturn,     // 9  MouthUpperOverturn
    sr_lower_overturn,     // 10 MouthLowerOverturn
    sr_pout,               // 11 MouthPout
    sr_smile_right,        // 12
    sr_smile_left,         // 13
    sr_sad_right,          // 14
    sr_sad_left,           // 15
    SR(CheekPuffRight),    // 16
    SR(CheekPuffLeft),     // 17
    sr_cheek_suck,         // 18 CheekSuck
    sr_upper_up_right,     // 19
    sr_upper_up_left,      // 20
    sr_lower_down_right,   // 21
    sr_lower_down_left,    // 22
    sr_upper_inside,       // 23 MouthUpperInside
    sr_lower_inside,       // 24 MouthLowerInside
    SR(MouthRaiserLower),  // 25 MouthLowerOverlay
    sr_tongue_step1,       // 26 TongueLongStep1
    SR(TongueLeft),        // 27
    SR(TongueRight),       // 28
    SR(TongueUp),          // 29
    SR(TongueDown),        // 30
    SR(TongueRoll),        // 31
    sr_tongue_step2,       // 32 TongueLongStep2
    sr_zero,               // 33 TongueUpRightMorph
    sr_zero,               // 34 TongueUpLeftMorph
    sr_zero,               // 35 TongueDownRightMorph
    sr_zero,               // 36 TongueDownLeftMorph
};

static_assert(sizeof(kSRanipal) / sizeof(kSRanipal[0]) == kSRanipalCount,
              "SRanipal 映射表条数与枚举不一致");

constexpr std::string_view kSRanipalNames[kSRanipalCount] = {
    "JawRight",           "JawLeft",           "JawForward",      "JawOpen",
    "MouthApeShape",      "MouthUpperRight",   "MouthUpperLeft",  "MouthLowerRight",
    "MouthLowerLeft",     "MouthUpperOverturn", "MouthLowerOverturn", "MouthPout",
    "MouthSmileRight",    "MouthSmileLeft",    "MouthSadRight",   "MouthSadLeft",
    "CheekPuffRight",     "CheekPuffLeft",     "CheekSuck",       "MouthUpperUpRight",
    "MouthUpperUpLeft",   "MouthLowerDownRight", "MouthLowerDownLeft", "MouthUpperInside",
    "MouthLowerInside",   "MouthLowerOverlay", "TongueLongStep1", "TongueLeft",
    "TongueRight",        "TongueUp",          "TongueDown",      "TongueRoll",
    "TongueLongStep2",    "TongueUpRightMorph", "TongueUpLeftMorph", "TongueDownRightMorph",
    "TongueDownLeftMorph",
};

// ---------------------------------------------------------------------------
// 合成规则（LipShapeMerger.MergedShapes，由脚本生成）
// ---------------------------------------------------------------------------

enum class Blend : std::uint8_t {
    Diff,     // PositiveNegativeShape：            正 - 负
    DiffStep, // PositiveNegativeShape(steps: true)：正 - 负 - 1
    Avg,      // PositiveNegativeAveragedShape：     均值(正) - 均值(负)
    Max,      // PositiveNegativeAveragedShape(max)：最大值(正) - 最大值(负)
};

struct MergeRuleDef {
    std::string_view name;
    Blend blend;
    std::uint8_t posBegin;
    std::uint8_t posCount;
    std::uint8_t negBegin;
    std::uint8_t negCount;
};

#include "params/v1_lip_tables.inc"

float rule_value(const MergeRuleDef& r, const EvalContext& c) noexcept {
    auto at = [&](std::size_t i) { return sranipal_value(static_cast<SRanipalShape>(kMergePool[i]), c); };

    float positive = 0.0f;
    if (r.blend == Blend::Max) {
        positive = -1.0f;
        for (std::size_t i = 0; i < r.posCount; ++i) {
            positive = std::max(positive, at(r.posBegin + i));
        }
        if (r.posCount == 0) {
            positive = 0.0f;
        }
    } else {
        for (std::size_t i = 0; i < r.posCount; ++i) {
            positive += at(r.posBegin + i);
        }
        if (r.blend != Blend::Diff && r.blend != Blend::DiffStep && r.posCount > 0) {
            positive /= static_cast<float>(r.posCount);
        }
    }

    float negative = 0.0f;
    if (r.blend == Blend::Max) {
        negative = -1.0f;
        for (std::size_t i = 0; i < r.negCount; ++i) {
            negative = std::max(negative, at(r.negBegin + i));
        }
        if (r.negCount == 0) {
            negative = 0.0f;
        }
    } else {
        for (std::size_t i = 0; i < r.negCount; ++i) {
            negative += at(r.negBegin + i);
        }
        if (r.blend != Blend::Diff && r.blend != Blend::DiffStep && r.negCount > 0) {
            negative /= static_cast<float>(r.negCount);
        }
    }

    const float diff = positive - negative;
    return r.blend == Blend::DiffStep ? diff - 1.0f : diff;
}

// ---------------------------------------------------------------------------
// 眼追 legacy（EyeTrackingParams.ParameterList 的 float / bool 部分）
// ---------------------------------------------------------------------------

constexpr float kEyeScaleDeg = 30.0f; // 与 v2 一致：30° 视为满量程

float gaze_x(const EyeState& e) noexcept { return clamp_sym(e.gazePitchYawDeg.y / kEyeScaleDeg); }
float gaze_y(const EyeState& e) noexcept { return clamp_sym(e.gazePitchYawDeg.x / kEyeScaleDeg); }

float lid_expanded(const EvalContext& c, const EyeState& e, Shape wide) noexcept {
    return c.frame.shapes[index_of(wide)] * 0.2f + e.openness * 0.8f;
}
float lid_expanded_squeeze(const EvalContext& c, const EyeState& e, Shape wide,
                           Shape squint) noexcept {
    return lid_expanded(c, e, wide) - squeeze(e, c.frame.shapes[index_of(squint)]);
}

#define P(name, expr) \
    {name, ParamKind::Float, [](const EvalContext& c) noexcept -> float { return (expr); }}
#define B(name, expr) \
    {name, ParamKind::Bool, [](const EvalContext& c) noexcept -> float { return (expr) ? 1.0f : 0.0f; }}

constexpr ParamEntry kLegacyEye[] = {
    // ---- 视线（Vector2 在 OSC 里拆成 X / Y 两条 float）----
    P("EyesX", (gaze_x(c.frame.eyes.left) + gaze_x(c.frame.eyes.right)) * 0.5f),
    P("EyesY", (gaze_y(c.frame.eyes.left) + gaze_y(c.frame.eyes.right)) * 0.5f),
    P("LeftEyeX", gaze_x(c.frame.eyes.left)),
    P("LeftEyeY", gaze_y(c.frame.eyes.left)),
    P("RightEyeX", gaze_x(c.frame.eyes.right)),
    P("RightEyeY", gaze_y(c.frame.eyes.right)),

    // ---- 睁大 / 眯眼 ----
    P("LeftEyeWiden", S(EyeWideLeft)),
    P("RightEyeWiden", S(EyeWideRight)),
    P("EyeWiden", (S(EyeWideLeft) + S(EyeWideRight)) * 0.5f),
    P("LeftEyeSqueeze", S(EyeSquintLeft)),
    P("RightEyeSqueeze", S(EyeSquintRight)),
    P("EyesSqueeze", (S(EyeSquintLeft) + S(EyeSquintRight)) * 0.5f),

    // ---- 瞳孔 ----
    P("EyesDilation", (c.frame.eyes.left.pupilDiameterMm + c.frame.eyes.right.pupilDiameterMm) * 0.5f),
    P("EyesPupilDiameter",
      (c.frame.eyes.left.pupilDiameterMm + c.frame.eyes.right.pupilDiameterMm) * 0.5f),

    // ---- 眼睑 ----
    P("LeftEyeLid", c.frame.eyes.left.openness),
    P("RightEyeLid", c.frame.eyes.right.openness),
    P("CombinedEyeLid", (c.frame.eyes.left.openness + c.frame.eyes.right.openness) * 0.5f),
    P("LeftEyeLidExpanded", lid_expanded(c, c.frame.eyes.left, Shape::EyeWideLeft)),
    P("RightEyeLidExpanded", lid_expanded(c, c.frame.eyes.right, Shape::EyeWideRight)),
    P("EyeLidExpanded", (S(EyeWideLeft) + S(EyeWideRight)) * 0.1f +
                            (c.frame.eyes.left.openness + c.frame.eyes.right.openness) * 0.4f),
    P("LeftEyeLidExpandedSqueeze",
      lid_expanded_squeeze(c, c.frame.eyes.left, Shape::EyeWideLeft, Shape::EyeSquintLeft)),
    P("RightEyeLidExpandedSqueeze",
      lid_expanded_squeeze(c, c.frame.eyes.right, Shape::EyeWideRight, Shape::EyeSquintRight)),
    P("EyeLidExpandedSqueeze",
      ((S(EyeWideLeft) + S(EyeWideRight)) * 0.2f +
       (c.frame.eyes.left.openness + c.frame.eyes.right.openness) * 0.8f -
       squeeze(c.frame.eyes.left, S(EyeSquintLeft)) -
       squeeze(c.frame.eyes.right, S(EyeSquintRight))) *
          0.5f),

    // ---- 眉毛 ----
    P("BrowsInnerUp", std::max(S(BrowInnerUpLeft), S(BrowInnerUpRight))),
    P("BrowInnerUpLeft", S(BrowInnerUpLeft)),
    P("BrowInnerUpRight", S(BrowInnerUpRight)),
    P("BrowsOuterUp", std::max(S(BrowOuterUpLeft), S(BrowOuterUpRight))),
    P("BrowOuterUpLeft", S(BrowOuterUpLeft)),
    P("BrowOuterUpRight", S(BrowOuterUpRight)),
    P("BrowsDown", std::max(kAvg(S(BrowPinchLeft), S(BrowLowererLeft)),
                            kAvg(S(BrowPinchRight), S(BrowLowererRight)))),
    P("BrowDownLeft", kAvg(S(BrowPinchLeft), S(BrowLowererLeft))),
    P("BrowDownRight", kAvg(S(BrowPinchRight), S(BrowLowererRight))),

    // ---- 鼻 / 颊 / 唇（Quest Pro 时代追加的一批 legacy 名）----
    P("NoseSneer", kAvg(S(NoseSneerLeft), S(NoseSneerRight))),
    P("NoseSneerLeft", S(NoseSneerLeft)),
    P("NoseSneerRight", S(NoseSneerRight)),
    P("CheeksSquint", kAvg(S(CheekSquintLeft), S(CheekSquintRight))),
    P("CheekSquintLeft", S(CheekSquintLeft)),
    P("CheekSquintRight", S(CheekSquintRight)),
    P("MouthDimple", kAvg(S(MouthDimpleLeft), S(MouthDimpleRight))),
    P("MouthDimpleLeft", S(MouthDimpleLeft)),
    P("MouthDimpleRight", S(MouthDimpleRight)),
    P("MouthPress", kAvg(S(MouthPressLeft), S(MouthPressRight))),
    P("MouthPressLeft", S(MouthPressLeft)),
    P("MouthPressRight", S(MouthPressRight)),
    P("MouthStretch", kAvg(S(MouthStretchLeft), S(MouthStretchRight))),
    P("MouthStretchLeft", S(MouthStretchLeft)),
    P("MouthStretchRight", S(MouthStretchRight)),
    P("MouthTightener", kAvg(S(MouthTightenerLeft), S(MouthTightenerRight))),
    P("MouthTightenerLeft", S(MouthTightenerLeft)),
    P("MouthTightenerRight", S(MouthTightenerRight)),
    P("MouthRaiserLower", S(MouthRaiserLower)),
    P("MouthRaiserUpper", S(MouthRaiserUpper)),
    P("EyesSquint", kAvg(S(EyeSquintLeft), S(EyeSquintRight))),
    P("EyeSquintLeft", S(EyeSquintLeft)),
    P("EyeSquintRight", S(EyeSquintRight)),

    // ---- 布尔开关（老模型用它们区分"睁大"与"眯眼"）----
    B("LeftEyeWidenToggle", lid_expanded(c, c.frame.eyes.left, Shape::EyeWideLeft) > 0.8f),
    B("RightEyeWidenToggle", lid_expanded(c, c.frame.eyes.right, Shape::EyeWideRight) > 0.8f),
    B("EyesWidenToggle",
      (lid_expanded(c, c.frame.eyes.left, Shape::EyeWideLeft) +
       lid_expanded(c, c.frame.eyes.right, Shape::EyeWideRight)) *
              0.5f >
          0.8f),
    B("LeftEyeSqueezeToggle",
      lid_expanded_squeeze(c, c.frame.eyes.left, Shape::EyeWideLeft, Shape::EyeSquintLeft) < 0.0f),
    B("RightEyeSqueezeToggle",
      lid_expanded_squeeze(c, c.frame.eyes.right, Shape::EyeWideRight, Shape::EyeSquintRight) < 0.0f),
    B("EyesSqueezeToggle",
      (lid_expanded_squeeze(c, c.frame.eyes.left, Shape::EyeWideLeft, Shape::EyeSquintLeft) +
       lid_expanded_squeeze(c, c.frame.eyes.right, Shape::EyeWideRight, Shape::EyeSquintRight)) *
              0.5f <
          0.0f),
};

#undef P
#undef B
#undef SR
#undef S

// 每条合成规则需要一个独立的函数指针（不能捕获），用索引展开生成
constexpr std::size_t kMergeRuleCount = sizeof(kMergeRules) / sizeof(kMergeRules[0]);

template <std::size_t I>
float merge_eval(const EvalContext& c) noexcept {
    return rule_value(kMergeRules[I], c);
}

template <std::size_t... I>
constexpr auto make_merge_entries(std::index_sequence<I...>) noexcept {
    return std::array<ParamEntry, sizeof...(I)>{ParamEntry{kMergeRules[I].name, ParamKind::Float,
                                                           &merge_eval<I>}...};
}

constexpr auto kMergeEntries = make_merge_entries(std::make_index_sequence<kMergeRuleCount>{});

// 37 个 SRanipal + 91 条合成 + 眼追，按固定顺序拼成一张表
std::vector<ParamEntry> build_v1_all() {
    std::vector<ParamEntry> out;
    out.reserve(kSRanipalCount + kMergeRuleCount + (sizeof(kLegacyEye) / sizeof(kLegacyEye[0])));

    for (std::size_t i = 0; i < kSRanipalCount; ++i) {
        out.push_back({kSRanipalNames[i], ParamKind::Float, kSRanipal[i]});
    }
    for (const ParamEntry& e : kMergeEntries) {
        out.push_back(e);
    }
    for (const ParamEntry& e : kLegacyEye) {
        out.push_back(e);
    }
    return out;
}

} // namespace

std::string_view sranipal_name(SRanipalShape s) noexcept {
    const auto i = static_cast<std::size_t>(s);
    return i < kSRanipalCount ? kSRanipalNames[i] : std::string_view{};
}

float sranipal_value(SRanipalShape s, const EvalContext& c) noexcept {
    const auto i = static_cast<std::size_t>(s);
    return i < kSRanipalCount ? kSRanipal[i](c) : 0.0f;
}

std::span<const ParamEntry> v1_all() noexcept {
    // 首次调用时构建（约 168 条），之后只读
    static const std::vector<ParamEntry> table = build_v1_all();
    return {table.data(), table.size()};
}

} // namespace fgb::params
