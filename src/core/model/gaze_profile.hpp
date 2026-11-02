// 注视标定档案（ADR-0042 §4）。
//
// 与 `calibration.ini` 分开：形状校准跟人不跟头显，注视标定跟头显和佩戴位置走。
// 换头显时应当整套重来，不能顺手把表情校准也清了。
//
// 档案只描述"怎么把标定前的观察映射到该眼视口"，不含时间相关的决策（平滑、死区、
// fixation）——那是稳定化阶段的事，混进来就没法回放与单测。
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace fgb {

// 标定时生效的"角度粗调"（calibration.ini 的 [eye]）。
//
// 【为什么档案要记它】
// 圆盘标定拟合的是"角度粗调之后"的观察值：输入一旦被改，gaze.ini 里的仿射
// 就作用在错位的坐标上——中心可能还准、越往边缘越偏。不记它，运行时就只剩
// "用户记不记得自己调过上面那四行"这一个判断依据，等于没有。
struct EyeAngleTrim {
    // 档案按 4 位小数写盘，1e-3 足够吸收往返误差，又不会把真实的 0.001 改动吞掉
    static constexpr float kTolerance = 1e-3f;

    bool active    = false;
    float gain_x   = 1.0f;
    float gain_y   = 1.0f;
    float center_x = 0.0f;
    float center_y = 0.0f;

    // 关掉开关等于恒等：开关状态本身不该让映射"过期"
    EyeAngleTrim effective() const noexcept {
        return active ? *this : EyeAngleTrim{};
    }

    // 比"效果"而不是比字段：active=false 与 active=true+恒等，落到数据上是同一件事
    bool same_effect(const EyeAngleTrim& other) const noexcept {
        const EyeAngleTrim a = effective();
        const EyeAngleTrim b = other.effective();
        return std::fabs(a.gain_x - b.gain_x) <= kTolerance &&
               std::fabs(a.gain_y - b.gain_y) <= kTolerance &&
               std::fabs(a.center_x - b.center_x) <= kTolerance &&
               std::fabs(a.center_y - b.center_y) <= kTolerance;
    }
};

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
    // 标定时实际见过的每眼输入范围（min_x, max_x, min_y, max_y）。
    //
    // 语义只是"标定采样见过的范围"，**不是传感器量程**：圆盘只覆盖屏幕中间一块，
    // 所以这个框天然比传感器能给的窄。运行期它只当"极值参照"的**初始值**——真正的
    // 参照是 `GazeCalibrator` 在会话中观测到的包络（只扩不缩），而且还要配"值连续
    // 多帧不变"才算饱和。单独拿这个框判饱和会把"看屏幕边缘"误判成"传感器到极限"，
    // 把注视点锁在中心小矩形里（2026-11-03 实测踩到）。
    std::array<std::array<float, 4>, 2> input_bounds{};
    bool have_bounds = false;

    // 拟合当时生效的角度粗调。改动它不会让仿射"失效报错"，只会让它悄悄偏掉，
    // 所以只能靠比对指纹来发现（TrimState）。
    EyeAngleTrim trim{};
    bool have_trim = false; // 旧档案没写这一条：无从判断，不是"一致"

    bool ready() const noexcept {
        return version != 0 && source_kind != GazeSourceKind::none &&
               (eyes[0].active || eyes[1].active);
    }
    // 该眼有没有可用映射；没有就原样透传，不要"半只眼校准"。
    const GazeEyeMap& eye(std::size_t index) const noexcept {
        return eyes[index == 0 ? 0 : 1];
    }
};

// 当前的角度粗调与档案里记的是否一致。"未知"要单独一档：把旧档案硬判成
// 一致（可能其实是错的）或过期（可能其实没动过）都是在给用户假信息。
enum class TrimState : std::uint8_t {
    unknown = 0,
    matches = 1,
    stale   = 2,
};

const char* trim_state_name(TrimState state) noexcept;
TrimState trim_state(const GazeProfile& profile, const EyeAngleTrim& current) noexcept;

} // namespace fgb
