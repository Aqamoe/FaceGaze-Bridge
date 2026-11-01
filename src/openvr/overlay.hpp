// 标定/探针用的最小 OpenVR overlay：N 个彩色圆点，挂在头显上。
//
// 为什么要 overlay 而不是"桌面窗口全屏"：标定与探针都要在头显里看位置，桌面窗口
// 经过串流合成后位置会随窗口大小/缩放变化（ADR-0042 §5）。
//
// 一次 `open` 只做一次 VR_Init，N 个点共享同一个 IVROverlay 会话；每个点各有纹理与
// 句柄，颜色在生成纹理时染进像素（overlay 不支持单独着色）。
//
// 生命周期与 steamvr::Manager 分开：那边管"随 SteamVR 自启动"，这边只在标定/探针期间存在。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr {
class IVROverlay;
}

namespace fgb::overlay {

struct Color {
    unsigned char r = 255;
    unsigned char g = 255;
    unsigned char b = 255;
};

class Dots {
public:
    Dots();
    ~Dots();

    Dots(const Dots&)            = delete;
    Dots& operator=(const Dots&) = delete;

    // 初始化 OpenVR（Background）并创建 count 个 overlay。失败时 error 说明原因。
    bool open(std::size_t count, const std::vector<Color>& colors, std::string& error);
    void close() noexcept;
    bool ready() const noexcept { return overlay_ != nullptr && !handles_.empty(); }
    std::size_t count() const noexcept { return handles_.size(); }

    // 按视线角度（度，正 yaw 向右、正 pitch 向上）把第 index 个点摆在 distance_m 处。
    // radius_deg 是圆点的角半径，按距离换算成米，保证不同距离看起来一样大。
    bool show(std::size_t index, float yaw_deg, float pitch_deg, float radius_deg,
              float distance_m, std::string& error);
    void hide(std::size_t index) noexcept;
    void hide_all() noexcept;
    // 整块 overlay 的透明度：结束后淡出，避免突然消失。
    void set_alpha(float alpha) noexcept;

private:
    struct Api;
    Api* api_       = nullptr;
    void* module_   = nullptr;
    vr::IVROverlay* overlay_ = nullptr;
    std::vector<std::uint64_t> handles_;
    std::vector<std::vector<unsigned char>> pixels_;
    bool initialized_ = false;
};

} // namespace fgb::overlay
