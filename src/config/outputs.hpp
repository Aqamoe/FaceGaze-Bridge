// 输出设置的持久化。
//
// 界面能改的东西必须能跨重启保留，否则用户每次启动都得重新填一遍。这里只存
// "输出目标/模式/开关"，不碰输入源与模块选择（后者在 modules.ini）。
//
// 单独一个 outputs.ini，而不是塞进 modules.ini：模块注册表会整份重写自己的状态，
// 两拨代码共用一个文件迟早互相覆盖。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fgb::config {

struct OutputOptions {
    std::string vrchat_host = "127.0.0.1";
    std::uint16_t vrchat_port = 9000;
    std::string eye_mode      = "parameters"; // parameters | native | both
    std::string params_mode   = "v2";         // v2 | v1 | both

    bool vmc_enabled         = false;
    std::string vmc_host     = "127.0.0.1";
    std::uint16_t vmc_port   = 39539;

    bool shm_enabled  = false;
    std::string shm_name = "default";

    bool ws_enabled    = false;
    std::uint16_t ws_port = 9101;
    int ws_rate_hz     = 30; // 0 = 每 tick
    bool ws_all_shapes = false;

    bool udp_enabled       = false;
    std::string udp_host   = "127.0.0.1";
    std::uint16_t udp_port = 9102;
    int udp_rate_hz        = 0; // 0 = 每 tick

    bool operator==(const OutputOptions&) const = default;
};

std::filesystem::path default_outputs_path();

// host:port（仅 IPv4/主机名；端口 1..65535）
bool parse_target(std::string_view text, std::string& host, std::uint16_t& port) noexcept;
std::string format_target(std::string_view host, std::uint16_t port);

bool validate_outputs(const OutputOptions& options, std::string& error);

// 文件不存在/读失败/校验失败时返回 false；out 会回到默认值，调用方不要应用半份配置。
bool load_outputs(const std::filesystem::path& path, OutputOptions& out, std::string& error);

// 先写 .tmp，旧文件留 .bak，再原子替换。
bool save_outputs(const std::filesystem::path& path, const OutputOptions& options,
                  std::string& error);

} // namespace fgb::config
