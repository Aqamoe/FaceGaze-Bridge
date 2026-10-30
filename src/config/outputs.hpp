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
#include <vector>

namespace fgb::config {

struct OutputOptions {
    std::string vrchat_host = "127.0.0.1";
    std::uint16_t vrchat_port = 9000;
    // 默认开启：绝大多数用户只给 VRChat 用。关掉只停 OSC 发送，头像参数发现照旧
    bool vrchat_enabled       = true;
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

// ---- 命名输出预设（roadmap 4.8）----
//
// 每个预设一个 ini，放在独立的目录里；文件内容与 outputs.ini 相同，外加
// `[profile] name=`（文件名会被清洗，展示名以文件内为准）。
// 单独放目录而不是往 outputs.ini 里塞多节：清单/删除/备份都简单，
// 坏掉一个预设也不会影响当前正在用的那份。

struct OutputProfile {
    std::string name;
    std::filesystem::path path;
};

std::filesystem::path default_profiles_dir();

// Windows 上 path(std::string) 按 ANSI 代码页解释，中文预设名会抛 std::filesystem_error。
// 凡是来自用户文本/UTF-8 文件的路径段都走这里。
std::filesystem::path path_from_utf8(std::string_view text);

// 名称要能当文件名，也要能显示：拒绝路径分隔符、控制字符、首尾空白、`.`/`..`
bool validate_profile_name(std::string_view name, std::string& error);

// 目录不存在时返回 true + 空列表（还没建过预设不是错误）
bool list_output_profiles(const std::filesystem::path& dir, std::vector<OutputProfile>& out,
                          std::string& error);
bool load_output_profile(const std::filesystem::path& file, OutputOptions& out, std::string& name,
                         std::string& error);
// 同名覆盖；没有同名就新建。原子写入，不碰 outputs.ini。
bool save_output_profile(const std::filesystem::path& dir, std::string_view name,
                         const OutputOptions& options, std::string& error);
bool delete_output_profile(const std::filesystem::path& dir, std::string_view name,
                           std::string& error);

} // namespace fgb::config
