#include "gaze_calib/probe.hpp"

#include "base/log.hpp"

#include <array>
#include <cmath>

namespace fgb::gaze_calib {
namespace {

constexpr char kTag[] = "gaze-probe";
// 放在 1.2 m：比标定用的 1.5 m 近一点，看起来更贴近"屏幕上哪儿"的直觉
constexpr float kDistanceM  = 1.2f;
constexpr float kRadiusDeg  = 0.5f;

const std::vector<overlay::Color> kColors = {
    overlay::Color{80, 220, 255},  // 左眼：青
    overlay::Color{255, 90, 210},  // 右眼：品红
};

} // namespace

bool Probe::start(std::string& error) {
    error.clear();
    if (!projection_.valid) {
        error = "没有头显几何，无法把注视点画回头显";
        return false;
    }
    if (!dots_.open(2, kColors, error)) {
        return false;
    }
    active_  = true;
    showing_ = false;
    message_ = "把两个圆点对准你正在看的方向";
    FGB_INFO(kTag, "注视点探针已开启");
    return true;
}

void Probe::stop() noexcept {
    dots_.hide_all();
    dots_.close();
    active_  = false;
    showing_ = false;
    message_.clear();
    FGB_INFO(kTag, "注视点探针已关闭");
}

void Probe::tick(const GazeState& gaze) noexcept {
    if (!active_) {
        return;
    }
    // 只画"已经落到视口坐标"的点：来源空间的原始点画出来没有意义（它不代表屏幕位置）
    const bool usable = (gaze.left_point_valid && gaze.right_point_valid) &&
                        gaze.space != GazeSpace::source;
    if (!usable) {
        if (showing_) {
            dots_.hide_all();
            showing_ = false;
        }
        message_ = gaze.geometry_missing ? "缺少头显几何" : "当前没有可用的注视点";
        return;
    }

    const std::array<Vec2, 2> points{gaze.left_point_2d, gaze.right_point_2d};
    for (std::size_t i = 0; i < 2; ++i) {
        Vec3 dir{};
        if (!unproject_viewport(points[i], projection_.eyes[i], dir)) {
            continue;
        }
        const Vec2 angles = dir_to_pitch_yaw_deg(dir);
        std::string error;
        // angles.x = pitch（+上）、angles.y = yaw（+右），与 overlay::show 的参数同序
        dots_.show(i, angles.y, angles.x, kRadiusDeg, kDistanceM, error);
    }
    showing_ = true;
    // 饱和时位置会保持不动（两眼各自的输入边界不同，继续按新值算会越走越开）
    message_ = gaze.saturated ? "已到传感器范围边缘，位置保持不变（青=左眼 品红=右眼）"
                              : "青=左眼 品红=右眼";
}

} // namespace fgb::gaze_calib
