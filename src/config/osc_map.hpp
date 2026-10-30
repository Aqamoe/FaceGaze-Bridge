// 通用 OSC 输入的映射表（地址 → 形状/眼睛目标）。
//
// 一行一条映射：地址是 ini 的键，所以天然去重（重复地址后者覆盖前者）。
// 值语法 `Target[, scale=..][, offset=..][, invert]`，和界面表格一一对应。
//
// 单独一个 osc_map.ini，不塞进 outputs.ini：输入和输出的生命周期不同，
// 改输入映射不该连带重写输出目标（ADR-0029）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fgb::config {

struct OscMapping {
    std::string address; // "/avatar/parameters/JawOpen"
    std::string target;  // "JawOpen" | "EyeLidLeft" | "EyeLeftX" …
    float scale          = 1.0f;
    float offset         = 0.0f;
    bool invert          = false;
};

struct OscMapOptions {
    std::uint16_t port = 9103;
    std::vector<OscMapping> mappings;
};

std::filesystem::path default_osc_map_path();

// value 侧语法解析。失败时 error 说清楚哪个字段不对。
bool parse_osc_mapping(std::string_view address, std::string_view spec, OscMapping& out,
                       std::string& error);
std::string format_osc_mapping(const OscMapping& mapping);

// 文件不存在/读失败/有坏行时返回 false；out 保持默认值，调用方不要应用半份配置。
bool load_osc_map(const std::filesystem::path& path, OscMapOptions& out, std::string& error);

// 原子写（.tmp → 替换，旧文件留 .bak）。目录不存在会自动创建。
bool save_osc_map(const std::filesystem::path& path, const OscMapOptions& options,
                  std::string& error);

} // namespace fgb::config
