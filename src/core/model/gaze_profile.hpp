// 注视标定档案（ADR-0042 §4）。
//
// 与 `calibration.ini` 分开：形状校准跟人不跟头显，注视标定跟头显和佩戴位置走。
// 换头显时应当整套重来，不能顺手把表情校准也清了。
//
// 档案只描述"怎么把标定前的观察映射到该眼视口"，不含时间相关的决策（平滑、死区、
// fixation）——那是稳定化阶段的事，混进来就没法回放与单测。
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace fgb {

// 逐眼 2D 仿射：X' = a·x + b·y + c，Y' = d·x + e·y + f。
// 先只做仿射：13 个点拟合 6 个自由度，残差还能解释；单应要 8 个自由度，
// 点噪声稍大就会拟合出"穿过标定点但中间乱扭"的映射（ADR-0042 §5 的升级条件）。
struct GazeEyeMap {
    bool active = false;
    float a = 1.0f, b = 0.0f, c = 0.0f;
    float d = 0.0f, e = 1.0f, f = 0.0f;

    void apply(float x, float y, float& out_x, float& out_y) const noexcept {
        out_x = a * x + b * y + c;
        out_y = d * x + e * y + f;
    }
};

// 标定针对的是哪条输入分支。换了分支（点型↔角度型）旧映射必须作废：
// 两者的输入量纲完全不同，硬套只会把注视点推到画面外。
enum class GazeSourceKind : std::uint8_t {
    none   = 0,
    points = 1, // 模块给的逐眼 2D 点（来源空间）
    angles = 2, // 面捕角度/方向（经几何投影后再修正）
};

const char* gaze_source_kind_name(GazeSourceKind kind) noexcept;
GazeSourceKind gaze_source_kind_from_name(const std::string& name) noexcept;

struct GazeProfile {
    std::uint32_t version = 0; // 0 = 未标定
    GazeSourceKind source_kind = GazeSourceKind::none;
    std::string created_utc;   // 标定完成时间（排障用，不参与计算）
    std::string hmd_identity;  // 头显身份指纹；与当前几何不一致时提示重标
    std::array<GazeEyeMap, 2> eyes{}; // 0 = 左，1 = 右
    float residual_p95 = -1.0f;       // 拟合残差（视口宽比例）；< 0 = 未知

    bool ready() const noexcept {
        return version != 0 && source_kind != GazeSourceKind::none &&
               (eyes[0].active || eyes[1].active);
    }
    // 该眼有没有可用映射；没有就原样透传，不要"半只眼校准"。
    const GazeEyeMap& eye(std::size_t index) const noexcept {
        return eyes[index == 0 ? 0 : 1];
    }
};

} // namespace fgb
