// 未处理异常的最小转储。
//
// 不引 Crashpad：这里只需要"进程崩了能留下一个能被 WinDbg/VS 打开的 dmp"，
// Windows 自带的 MiniDumpWriteDump（DbgHelp）就够，且不增加发布包体积。
//
// 处理函数本身不保证一定能写成功（崩溃可能发生在堆损坏之后），所以：
//   - 用静态原子量防止递归进入；
//   - 写失败时只留一条 .txt 说明，不抛异常；
//   - 转储与说明都放在 %LOCALAPPDATA%\FaceGazeBridge\crashes。
#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace fgb::crash {

std::filesystem::path default_crash_dir();

// 安装 SetUnhandledExceptionFilter + std::set_terminate。
// version 会写进转储旁边的 .txt，方便对照发布包。
void install(const std::filesystem::path& crash_dir, std::string_view version);

// 直接写一份当前进程的最小转储（单元测试/诊断用）。
bool write_minidump(const std::filesystem::path& path, std::string& error);

// 只保留最近 max_files 份 dmp（按修改时间），避免长跑机器被转储占满。
void prune_old_dumps(const std::filesystem::path& crash_dir, std::size_t max_files);

} // namespace fgb::crash
