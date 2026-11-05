// 日志文件滚动（ADR-0046）。
//
// engine.log 是"引擎 stderr 追加到这个文件"，只追加、不轮转：上游模块一句话
// 刷一晚上就能把它写到 GB 级（真机实测 71 MB / 494k 行，其中 95% 是两条重复
// 消息），而诊断包又会整份拷走。外壳每次拉起引擎前调一次这个函数。
#pragma once

#include <cstdint>
#include <filesystem>

namespace fgb {

// 超过 max_bytes 就把 path 滚成 path.1（只留一份备份）。
// 返回 true 表示真的滚了；改名失败（旧引擎还开着句柄、杀软占用）返回 false，
// 调用方应当继续往原文件追加——写不进日志比日志太大更糟。
bool rotate_log_file(const std::filesystem::path& path, std::uintmax_t max_bytes);

} // namespace fgb
