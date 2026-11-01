// VRSS 组件的运行时：定位 provider、导出几何、驱动检查/探测、启停进程、读状态。
//
// 所有会碰硬件或提权的事情都关在这里；UI 线程只拿快照，不直接调 Win32。
#pragma once

#include "vrss/geometry.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace fgb::vrss {

struct DriverInfo {
    bool nvidia      = false;
    bool supported   = false;
    bool experimental = false;
    std::string version;
    std::string contract; // audited | experimental | none
    std::string message;
};

struct ProviderStatus {
    bool running         = false;
    bool registered      = false;
    bool fresh           = false;
    int pid              = 0;
    int consumer_count   = 0;
    std::uint64_t sent        = 0;
    std::uint64_t submit_failures = 0;
    double age_seconds    = 0.0;
    std::string delivery_state;
    std::string input_route;
    std::string last_error;
};

class Runtime {
public:
    Runtime() = default;

    // 组件是否已随包安装（runtime/vrss 下的两个 exe）。
    bool installed(std::string& error) const;

    // 用 OpenVR 导出几何；要求 SteamVR 与 HMD 已就绪。
    bool export_geometry(HmdGeometry& out, std::string& error);

    // 白名单直接通过；不在白名单时自动只读探测并落盘实验合同。
    bool check_driver(DriverInfo& out, std::string& error);

    // 启动提权 provider。contract 为空表示使用内建白名单。
    bool start_provider(int port, const std::string& contract, float area_percent,
                        std::string& error);
    bool stop_provider(std::string& error);

    // 写入 NVIDIA VRChat profile 的 VRSS 中央超采样区域（1..100%）。
    bool set_area(float percent, std::string& error);
    bool get_area(float& percent, bool& found, std::string& error);

    bool provider_alive() const;
    bool read_status(ProviderStatus& out) const;
    std::string last_error_text() const;

    // 供外壳/引擎周期性刷新 lease；provider 15s 未更新会自行退出。
    void write_lease() const;

    bool vrchat_running() const;
    bool launch_vrchat(std::string& error) const;
    bool admin_enabled() const;
    bool port_available(int port) const;

    // 最近一次驱动检查使用的合同路径（空 = 内建白名单）。
    const std::string& contract() const noexcept { return contract_; }

    static std::filesystem::path provider_exe();
    static std::filesystem::path exporter_exe();

private:
    std::string contract_;
};

} // namespace fgb::vrss
