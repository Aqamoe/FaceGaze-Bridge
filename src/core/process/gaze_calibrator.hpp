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
    SeqLock<GazeProfile> profile_snapshot_{};
    GazeProfile engine_copy_{};   // 只由引擎线程读写：读快照失败时沿用上一帧
    GazeProjection projection_{}; // 单次整块写入，值语义即可
};

} // namespace fgb
