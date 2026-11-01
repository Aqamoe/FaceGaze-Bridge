// 注视点探针：把头显里"系统认为的左右眼注视点"画成两个圆点（ADR-0042 §8 的调试工具）。
//
// 为什么需要它：标定只能保证"原始读数 → 视口坐标"这条换算是对的（残差可测），
// 而"视口坐标 → 驱动把区域放到哪"这一段在驱动里，我们看不到。把系统认为的注视点
// 画回头显，用户就能回答一个物理问题：这个点是不是我眼睛真正看的方向？
//   · 点在你看到的地方 → 注视数据没问题，偏差来自显示路径（VRSS 的区域摆放）；
//   · 点本身就偏 → 标定/来源有问题，该重标或改映射。
//
// 颜色约定：左眼青、右眼品红。两眼点分开说明存在视差（或驱动侧的对称视差补偿没调好）。
#pragma once

#include "core/model/gaze.hpp"
#include "core/model/gaze_projection.hpp"
#include "openvr/overlay.hpp"

#include <string>

namespace fgb::gaze_calib {

class Probe {
public:
    void set_projection(const GazeProjection& projection) noexcept { projection_ = projection; }

    bool start(std::string& error);
    void stop() noexcept;
    bool active() const noexcept { return active_; }
    const std::string& message() const noexcept { return message_; }

    // 每帧调用：把两个点挪到当前注视位置。没有可用点时隐藏，避免"点卡在上一处"。
    void tick(const GazeState& gaze) noexcept;

private:
    overlay::Dots dots_;
    GazeProjection projection_{};
    bool active_ = false;
    bool showing_ = false;
    std::string message_;
};

} // namespace fgb::gaze_calib
