// 注视管线第二段：把"任意空间"映射到逐眼视口点（ADR-0042 §1）。
//
// 两种情况分开对待：
//   · 模块声明了视口坐标（`space == viewport`）→ 有档案就再套一层校正，没有就直通；
//   · 只有方向/角度 → 用头显几何投影成视口点（几何换算不是标定，标签仍是 viewport）；
//   · 只有来源空间的点 → 只有拟合档案能救它，没有档案就保持 `source`，
//     下游（VRSS 等）据此拒绝使用，而不是拿一个"大概是屏幕"的位置去用。
//
// 这一段是纯函数式的：输入 GazeState + 档案 + 投影，输出 GazeState。
// 不做平滑、不判 fixation、不丢帧——那些都属于稳定化，混进来就没法回放验证。
#pragma once

#include "core/cont/seqlock.hpp"
#include "core/model/gaze.hpp"
#include "core/model/gaze_profile.hpp"
#include "core/model/gaze_projection.hpp"

namespace fgb {

class GazeCalibrator {
public:
    // 判饱和要"贴住极值 + 连续这么多帧原始值不变"。120 Hz 下约 50 ms，30 Hz 下 200 ms：
    // 短于人手/眼球自然停留的抖动周期，又长于单帧毛刺。
    static constexpr int kSaturatedHoldFrames = 6;
    // 原始值"没变"的容差（视口宽比例）。真卡住的传感器给的是同一个数（差 0），
    // 这里刻意取得极小：宁可漏判一次饱和（边缘略偏），也不要因为信号被平滑过
    // 就误判成饱和（那会把整个屏幕锁在一小块里）。
    static constexpr float kStuckEpsilon = 1e-5f;

    // 控制线程调用：界面换档案时实时生效，不需要重启引擎。
    void set_profile(const GazeProfile& profile) noexcept;
    // 头显几何（per-eye 投影）。没有几何时方向型来源只能停在"有方向没位置"。
    void set_projection(const GazeProjection& projection) noexcept;
    // 返回拷贝：档案随时可能被换掉，返回引用会让调用方读到半截。
    GazeProfile profile() const noexcept;
    GazeProjection projection() const noexcept { return projection_; }

    // 引擎线程每帧调用。
    void apply(GazeState& gaze) noexcept;

private:
    void reset_saturation_state() noexcept {
        last_left_      = Vec2{0.0f, 0.0f};
        last_right_     = Vec2{0.0f, 0.0f};
        have_last_      = false;
        prev_left_raw_  = Vec2{0.0f, 0.0f};
        prev_right_raw_ = Vec2{0.0f, 0.0f};
        have_prev_raw_  = false;
        left_hold_      = 0;
        right_hold_     = 0;
        env_lo_[0] = env_lo_[1] = Vec2{0.0f, 0.0f};
        env_hi_[0] = env_hi_[1] = Vec2{0.0f, 0.0f};
        have_env_  = false;
    }

    SeqLock<GazeProfile> profile_snapshot_{};
    GazeProfile engine_copy_{};   // 只由引擎线程读写：读快照失败时沿用上一帧
    GazeProjection projection_{}; // 单次整块写入，值语义即可
    // 饱和时的"最后一对好位置"：传感器到极限后，继续按新值算只会让两眼越走越开，
    // 保持上一对有效位置比给一个假位置更接近事实。
    Vec2 last_left_{0.0f, 0.0f};
    Vec2 last_right_{0.0f, 0.0f};
    bool have_last_ = false;
    // 上一帧的原始值 + 连续"贴边且不变"的帧数（见 apply 里的饱和判定）
    Vec2 prev_left_raw_{0.0f, 0.0f};
    Vec2 prev_right_raw_{0.0f, 0.0f};
    bool have_prev_raw_ = false;
    int left_hold_      = 0;
    int right_hold_     = 0;
    // 本会话观测到的原始值包络（逐眼、只扩不缩），首帧由档案的 input_bounds 播种。
    // 饱和判定拿它当"极值"参照：档案记的是"标定时看到过哪儿"，窄范围标定时远小于
    // 真实量程，拿它当极限会把边缘的稳定注视误判成饱和。
    Vec2 env_lo_[2]{};
    Vec2 env_hi_[2]{};
    bool have_env_ = false;
};

} // namespace fgb
