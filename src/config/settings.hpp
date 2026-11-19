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

    // 眨眼保持（GazeStabilizer）：闭眼期间冻结注视点，避免眼动向量在眨眼时
    // 把点顶得上下跳。默认开——人眨眼时注视本来就不该动。
    bool gaze_blink_hold = true;
    // 判闭眼的 openness 阈值。眼睑校准（eyelid_lo/gain）改过之后，来源的
    // "全睁"可能明显低于 1.0，这时把阈值一起调高才判得出来。
    float gaze_blink_enter = 0.25f;
    // 睁开后最多再等多久（来源回弹"先低一点再回来"时，等它回到原处再交接）。
    // 还看到往下沉一点就把这个值加大（300–400），太迟钝就减小。
    float gaze_blink_tail_ms = 250.0f;
    // 与保持值差多少（度）以内算"已经回到原处"。残余下沉幅度小时可以调小到 1.5。
    float gaze_blink_handoff_deg = 2.5f;

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
