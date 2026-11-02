// 界面上的"运行设置"持久化：滤波参数、日志级别、标定范围。
//
// 【为什么单独一个 settings.ini】
//   outputs.ini 只讲"输出目标/模式"（见那份文件的头注释）；modules.ini 会被模块注册表
//   整份重写。滤波与标定范围属于引擎侧的处理行为，混进任何一个都会让"这次改动影响了
//   谁"变得说不清。
//
// 【约定】界面改了立刻落盘。界面上没有"保存"按钮的东西，就不该有"忘了保存"这个问题。
#pragma once

#include <filesystem>
#include <string>

namespace fgb::config {

struct EngineSettings {
    // One Euro 滤波（默认值与 core/process/one_euro.hpp 的 OneEuroConfig 一致）
    bool filter_enabled      = false;
    float filter_min_cutoff  = 1.0f;
    float filter_beta        = 0.5f;

    // 空 = 没设置过，由命令行/默认值决定。写死一个默认值会让 --verbose 永远失效。
    std::string log_level;

    // 圆盘标定的点位范围："wide"（外环 32°）/ "narrow"（外环 20°）。
    // 空 = 没设置过，用 wide。看不清边缘的用户可以选 narrow。
    std::string gaze_calib_span;

    bool operator==(const EngineSettings&) const = default;
};

std::filesystem::path default_settings_path();

// 文件缺失/读失败时返回 false，out 保持默认值（调用方继续用命令行值，不是错误）。
bool load_settings(const std::filesystem::path& path, EngineSettings& out, std::string& error);

// 先写 .tmp，旧文件留 .bak，再原子替换。数值会做一次范围钳制。
bool save_settings(const std::filesystem::path& path, const EngineSettings& settings,
                   std::string& error);

// 日志级别的合法名（trace/debug/info/warn/error/off）。未知名字按"没设置"处理，
// 不让一个手改坏的值把启动日志变成空白。
bool valid_log_level(const std::string& level) noexcept;

} // namespace fgb::config
