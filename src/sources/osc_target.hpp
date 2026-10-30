// OSC 映射目标：统一形状名，或眼睛专用目标。
//
// 眼睛单独建目标而不是塞进 88 个形状里，是为了守住"形状数组只有肌肉形状"的约定：
// EyeLidLeft 是睁眼度，EyeLeftX 是归一化视线，它们和 v2 参数同名但语义是眼追，
// 塞进 shapes[] 会和 eyeBlend(导出时从 openness 派生) 造成两处真相。
#pragma once

#include "core/model/shape.hpp"

#include <string_view>

namespace fgb {

enum class OscTargetKind : unsigned char {
    Shape,         // shapes[] 里的肌肉形状
    EyeOpenLeft,   // 0 = 睁，1 = 闭（与 v2 EyeLid 一致）
    EyeOpenRight,
    EyeGazeXLeft,  // -1..1，正 = 佩戴者右侧
    EyeGazeYLeft,  // -1..1，正 = 上
    EyeGazeXRight,
    EyeGazeYRight,
};

struct OscTarget {
    OscTargetKind kind = OscTargetKind::Shape;
    Shape shape        = Shape::JawOpen; // kind == Shape 时有效
};

// 大小写不敏感：手写配置时 JawOpen / jawopen 都认。
bool parse_osc_target(std::string_view name, OscTarget& out) noexcept;

// 有符号目标（视线轴）的 invert 是取反，0..1 目标是 1-v。
constexpr bool osc_target_is_signed(OscTargetKind kind) noexcept {
    return kind == OscTargetKind::EyeGazeXLeft || kind == OscTargetKind::EyeGazeYLeft ||
           kind == OscTargetKind::EyeGazeXRight || kind == OscTargetKind::EyeGazeYRight;
}

} // namespace fgb
