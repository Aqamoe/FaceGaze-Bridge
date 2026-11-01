// 注视标定档案的读写（`gaze.ini`）。
//
// 格式刻意扁平：一眼一段，人能手改，坏行只丢那一行而不是整个档案。
#pragma once

#include "core/model/gaze_profile.hpp"

#include <filesystem>
#include <string>

namespace fgb::params {

// %LOCALAPPDATA%\FaceGazeBridge\gaze.ini
std::filesystem::path default_gaze_profile_path();

// 读不到文件返回 false（error 说明原因）；字段越界按"跳过该字段"处理，不整份作废。
bool load_gaze_profile(const std::string& path, GazeProfile& profile, std::string& error);

// 先写 .tmp 再原子替换：半截档案比没有档案更危险（会被当成"标定过"）。
bool save_gaze_profile(const std::string& path, const GazeProfile& profile, std::string& error);

} // namespace fgb::params
