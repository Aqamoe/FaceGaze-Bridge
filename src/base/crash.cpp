#include "base/crash.hpp"

#include "base/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

namespace fgb::crash {
namespace {

std::filesystem::path g_crash_dir;
std::string g_version;
std::atomic<bool> g_writing{false};

constexpr char kTag[] = "crash";

std::string timestamp_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d-%02d%02d%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                  local.tm_min, local.tm_sec);
    return buffer;
}

#ifdef _WIN32
void write_sidecar(const std::filesystem::path& dump_path, const char* reason,
                   EXCEPTION_POINTERS* info) noexcept {
    try {
        std::filesystem::path text_path = dump_path;
        text_path.replace_extension(".txt");
        std::ofstream out(text_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << "FaceGaze Bridge crash report\n";
        out << "version: " << (g_version.empty() ? "unknown" : g_version) << "\n";
        out << "time: " << timestamp_name() << "\n";
        out << "reason: " << (reason == nullptr ? "" : reason) << "\n";
        out << "pid: " << ::GetCurrentProcessId() << "\n";
        if (info != nullptr && info->ExceptionRecord != nullptr) {
            out << "exception_code: 0x" << std::hex
                << static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode) << std::dec << "\n";
            out << "exception_address: 0x" << std::hex
                << reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress) << std::dec
                << "\n";
        }
        wchar_t module[MAX_PATH]{};
        if (::GetModuleFileNameW(nullptr, module, MAX_PATH) > 0) {
            std::wstring wide(module);
            out << "module: ";
            for (const wchar_t c : wide) {
                out << static_cast<char>(c < 128 ? c : '?');
            }
            out << "\n";
        }
        out.flush();
    } catch (...) {
        // 崩溃路径上不再抛
    }
}

bool write_minidump_impl(const std::filesystem::path& path, EXCEPTION_POINTERS* info,
                         std::string& error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const HANDLE file = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "无法创建转储文件（错误码 " + std::to_string(::GetLastError()) + "）";
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exception_info{};
    exception_info.ThreadId          = ::GetCurrentThreadId();
    exception_info.ExceptionPointers = info;
    exception_info.ClientPointers    = FALSE;

    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithHandleData);
    const BOOL ok = ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, type,
                                        info != nullptr ? &exception_info : nullptr, nullptr,
                                        nullptr);
    ::CloseHandle(file);
    if (ok == FALSE) {
        error = "MiniDumpWriteDump 失败（错误码 " + std::to_string(::GetLastError()) + "）";
        return false;
    }
    return true;
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
    if (g_writing.exchange(true, std::memory_order_acq_rel)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    try {
        if (!g_crash_dir.empty()) {
            const std::filesystem::path dump =
                g_crash_dir / ("crash_" + timestamp_name() + "_" +
                               std::to_string(::GetCurrentProcessId()) + ".dmp");
            std::string error;
            if (write_minidump_impl(dump, info, error)) {
                write_sidecar(dump, "unhandled exception", info);
                prune_old_dumps(g_crash_dir, 20);
            } else {
                write_sidecar(dump, error.c_str(), info);
            }
        }
    } catch (...) {
    }
    return EXCEPTION_EXECUTE_HANDLER; // 让进程按未处理异常正常终止
}
#endif

} // namespace

std::filesystem::path default_crash_dir() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "crashes";
}

void install(const std::filesystem::path& crash_dir, std::string_view version) {
    g_crash_dir = crash_dir;
    g_version.assign(version);
    std::error_code ec;
    if (!g_crash_dir.empty()) {
        std::filesystem::create_directories(g_crash_dir, ec);
    }
#ifdef _WIN32
    ::SetUnhandledExceptionFilter(&unhandled_filter);
    std::set_terminate([] {
        if (!g_writing.exchange(true, std::memory_order_acq_rel) && !g_crash_dir.empty()) {
            try {
                const std::filesystem::path dump =
                    g_crash_dir / ("crash_" + timestamp_name() + "_" +
                                   std::to_string(::GetCurrentProcessId()) + ".dmp");
                std::string error;
                if (write_minidump_impl(dump, nullptr, error)) {
                    write_sidecar(dump, "std::terminate", nullptr);
                } else {
                    write_sidecar(dump, error.c_str(), nullptr);
                }
                prune_old_dumps(g_crash_dir, 20);
            } catch (...) {
            }
        }
        std::_Exit(3); // terminate 处理器必须终止进程；不能用 abort() 再次进入
    });
    FGB_INFO(kTag, "崩溃转储目录：%s", g_crash_dir.string().c_str());
#endif
}

bool write_minidump(const std::filesystem::path& path, std::string& error) {
    error.clear();
#ifdef _WIN32
    return write_minidump_impl(path, nullptr, error);
#else
    (void)path;
    error = "当前平台不支持 MiniDump";
    return false;
#endif
}

void prune_old_dumps(const std::filesystem::path& crash_dir, std::size_t max_files) {
    std::error_code ec;
    if (crash_dir.empty() || !std::filesystem::is_directory(crash_dir, ec)) {
        return;
    }
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
    };
    std::vector<Entry> dumps;
    for (const auto& entry : std::filesystem::directory_iterator(crash_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".dmp") {
            continue;
        }
        dumps.push_back({entry.path(), entry.last_write_time(ec)});
    }
    if (dumps.size() <= max_files) {
        return;
    }
    std::sort(dumps.begin(), dumps.end(), [](const Entry& a, const Entry& b) {
        return a.time > b.time; // 新的在前
    });
    for (std::size_t i = max_files; i < dumps.size(); ++i) {
        std::filesystem::remove(dumps[i].path, ec);
        std::filesystem::path sidecar = dumps[i].path;
        sidecar.replace_extension(".txt");
        std::filesystem::remove(sidecar, ec);
    }
}

} // namespace fgb::crash
