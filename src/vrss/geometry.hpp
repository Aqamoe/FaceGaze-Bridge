// HMD 几何：VRSS provider 注册与方向型注视换算共用同一份数据。
//
// 2D 视口点不需要几何；只有"给的是方向而不是屏幕位置"的来源才需要
// OpenVR 的逐眼投影与 eye-to-head 旋转。缺几何时方向路径自动停用。
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fgb::vrss {

struct EyeGeometry {
    // OpenVR GetProjectionRaw：left, right, top, bottom（切空间）。
    std::array<float, 4> projection{};
    // 行主序 3x3 旋转（eye -> head）。
    std::array<float, 9> eye_to_head{};

    bool valid() const noexcept;
};

struct HmdGeometry {
    EyeGeometry eyes[2];
    std::uint32_t vendor = 0;
    std::uint32_t product = 0;
    std::string identity_source;
    std::string fingerprint;

    bool valid() const noexcept { return eyes[0].valid() && eyes[1].valid(); }
};

bool parse_geometry(std::string_view text, HmdGeometry& out, std::string& error);
bool load_geometry(const std::filesystem::path& path, HmdGeometry& out, std::string& error);

} // namespace fgb::vrss
