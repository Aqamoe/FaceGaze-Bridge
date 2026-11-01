// 标定用的最小 OpenVR overlay：一个圆点，挂到头显。
//
// 为什么要 overlay 而不是"桌面窗口全屏"：标定要在头显里看目标点，桌面窗口经过串流
// 合成后位置会随窗口大小/缩放变化，标出来的映射在其他场景就不对了（ADR-0042 §5）。
//
// 生命周期与 steamvr::Manager 分开：那边管"随 SteamVR 自启动"，这边只在标定期间存在。
// 两者各自 VR_Init/Shutdown 一次（OpenVR 内部引用计数），互不影响。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vr {
class IVROverlay;
}

namespace fgb::overlay {

class GazeDot {
public:
    GazeDot();
    ~GazeDot();

    GazeDot(const GazeDot&)            = delete;
    GazeDot& operator=(const GazeDot&) = delete;

    // 初始化 OpenVR（Background）并创建 overlay。失败时 error 说明原因。
    bool open(std::string& error);
    void close() noexcept;
    bool ready() const noexcept;

    // 按视线角度（度，正 yaw 向右、正 pitch 向上）把圆点摆在 distance_m 处。
    // radius_deg 是圆点的角半径，按距离换算成米，保证在不同距离看起来一样大。
    bool show_at(float yaw_deg, float pitch_deg, float radius_deg, float distance_m,
                 std::string& error);
    void hide() noexcept;
    // 整块 overlay 的透明度：标定结束时淡出，避免突然消失。
    void set_alpha(float alpha) noexcept;

private:
    struct Api;
    Api* api_       = nullptr;
    void* module_   = nullptr;
    vr::IVROverlay* overlay_ = nullptr;
    std::uint64_t handle_    = 0;
    std::vector<unsigned char> pixels_;
    bool initialized_ = false;
};

} // namespace fgb::overlay
