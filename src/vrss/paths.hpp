// VRSS 组件使用的固定路径。集中在这里，避免 sink / provider / UI 各拼一份。
#pragma once

#include <filesystem>

namespace fgb::vrss {

// %LOCALAPPDATA%\FaceGazeBridge\vrss
std::filesystem::path data_dir();
// data_dir()\run：provider 的 status.json / error.txt / stop.request / geometry.txt
std::filesystem::path run_dir();
// data_dir()\contracts：实验驱动合同（按镜像哈希命名）
std::filesystem::path contracts_dir();
// 当前 exe 同目录下的 runtime\vrss
std::filesystem::path runtime_dir();

} // namespace fgb::vrss
