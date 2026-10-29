#include "base/diagnostics.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#endif

namespace fgb::diagnostics {
namespace {

std::string timestamp_compact() {
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

std::filesystem::path local_appdata() {
    const char* value = std::getenv("LOCALAPPDATA");
    return value == nullptr ? std::filesystem::path{} : std::filesystem::path{value};
}

std::filesystem::path roam_appdata() {
    const char* value = std::getenv("APPDATA");
    return value == nullptr ? std::filesystem::path{} : std::filesystem::path{value};
}

void copy_if_exists(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        return;
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
#ifdef _WIN32
    if (::CopyFileW(source.wstring().c_str(), destination.wstring().c_str(), FALSE) == FALSE) {
        std::filesystem::copy_file(source, destination,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
#else
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing, ec);
#endif
}

std::string system_info(std::string_view version) {
    std::string out;
    out += "FaceGaze Bridge diagnostics\n";
    out += "version: ";
    out.append(version);
    out += "\ntime: " + timestamp_compact();
#ifdef _WIN32
    out += "\npid: " + std::to_string(::GetCurrentProcessId());

    // RtlGetVersion 不受 manifest 的兼容性撒谎影响
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        const auto fn = reinterpret_cast<RtlGetVersionFn>(
            ::GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn != nullptr) {
            RTL_OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (fn(&info) == 0) {
                out += "\nwindows: " + std::to_string(info.dwMajorVersion) + "." +
                       std::to_string(info.dwMinorVersion) + "." +
                       std::to_string(info.dwBuildNumber);
            }
        }
    }
    SYSTEM_INFO sys{};
    ::GetNativeSystemInfo(&sys);
    out += "\narch: ";
    switch (sys.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
        out += "x64";
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        out += "arm64";
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        out += "x86";
        break;
    default:
        out += std::to_string(sys.wProcessorArchitecture);
        break;
    }
    out += "\ncpus: " + std::to_string(sys.dwNumberOfProcessors);
#endif
    out += "\n";
    return out;
}

bool wait_for_process(HANDLE process, DWORD timeout_ms, DWORD& exit_code) {
    const DWORD wait = ::WaitForSingleObject(process, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        return false;
    }
    return ::GetExitCodeProcess(process, &exit_code) != FALSE;
}

bool create_zip(const std::filesystem::path& staging, const std::filesystem::path& output,
                std::string& error) {
#ifdef _WIN32
    wchar_t system_dir[MAX_PATH]{};
    if (::GetSystemDirectoryW(system_dir, MAX_PATH) == 0) {
        error = "无法定位系统目录";
        return false;
    }
    const std::filesystem::path tar =
        std::filesystem::path{system_dir} / L"tar.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(tar, ec)) {
        error = "系统缺少 tar.exe（需要 Windows 10 1803 及以上）";
        return false;
    }
    std::filesystem::remove(output, ec);
    if (!output.parent_path().empty()) {
        std::filesystem::create_directories(output.parent_path(), ec);
    }

    std::wstring command = L"\"" + tar.wstring() + L"\" -a -c -f \"" + output.wstring() + L"\" .";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(tar.wstring().c_str(), command.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, staging.wstring().c_str(), &startup,
                         &process) == FALSE) {
        error = "启动 tar.exe 失败（错误码 " + std::to_string(::GetLastError()) + "）";
        return false;
    }
    DWORD exit_code = 1;
    const bool waited = wait_for_process(process.hProcess, 60'000, exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (!waited || exit_code != 0) {
        error = "打包失败（tar 退出码 " + std::to_string(exit_code) + "）";
        return false;
    }
    if (!std::filesystem::is_regular_file(output, ec)) {
        error = "打包后没有生成 zip";
        return false;
    }
    return true;
#else
    (void)staging;
    (void)output;
    error = "当前平台不支持诊断包";
    return false;
#endif
}

} // namespace

std::filesystem::path default_bundle_path() {
    const std::filesystem::path base = local_appdata();
    if (base.empty()) {
        return {};
    }
    return base / "FaceGazeBridge" / "diagnostics" /
           ("FaceGazeBridge-diag-" + timestamp_compact() + ".zip");
}

bool build_bundle(const BundleRequest& request, std::string& error) {
    error.clear();
#ifdef _WIN32
    const std::filesystem::path output =
        request.output.empty() ? default_bundle_path() : request.output;
    if (output.empty()) {
        error = "诊断包路径不可用";
        return false;
    }

    const std::filesystem::path staging =
        std::filesystem::temp_directory_path() /
        ("fgb-diag-" + std::to_string(::GetCurrentProcessId()) + "-" + timestamp_compact());
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        error = "无法创建临时目录：" + ec.message();
        return false;
    }

    {
        std::ofstream out(staging / "system.txt", std::ios::binary | std::ios::trunc);
        out << system_info(request.version);
    }

    const std::filesystem::path local = local_appdata() / "FaceGazeBridge";
    copy_if_exists(local / "engine.log", staging / "engine.log");
    copy_if_exists(local / "calibration.ini", staging / "calibration.ini");
    copy_if_exists(local / "outputs.ini", staging / "outputs.ini");
    copy_if_exists(local / "modules.ini", staging / "modules.ini");
    copy_if_exists(local / "library.json", staging / "library.json");
    copy_if_exists(roam_appdata() / "FaceGazeBridge" / "fgb.ini", staging / "fgb.ini");
    copy_if_exists(request.exe_dir / "app.vrmanifest", staging / "app.vrmanifest");

    // 预设目录：只收 ini，不收任何二进制
    if (std::filesystem::is_directory(local / "outputs-profiles", ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(local / "outputs-profiles", ec)) {
            if (ec) {
                break;
            }
            if (entry.is_regular_file() && entry.path().extension() == ".ini") {
                copy_if_exists(entry.path(), staging / "outputs-profiles" / entry.path().filename());
            }
        }
    }

    // 崩溃转储：最多 5 份，避免诊断包过大
    if (std::filesystem::is_directory(local / "crashes", ec)) {
        int copied = 0;
        for (const auto& entry : std::filesystem::directory_iterator(local / "crashes", ec)) {
            if (ec || copied >= 5) {
                break;
            }
            if (entry.is_regular_file() &&
                (entry.path().extension() == ".dmp" || entry.path().extension() == ".txt")) {
                copy_if_exists(entry.path(), staging / "crashes" / entry.path().filename());
                if (entry.path().extension() == ".dmp") {
                    ++copied;
                }
            }
        }
    }

    // 模块列表只写目录名/大小，不复制 DLL
    if (std::filesystem::is_directory(local / "modules", ec)) {
        std::ofstream out(staging / "modules.txt", std::ios::binary | std::ios::trunc);
        for (const auto& entry : std::filesystem::directory_iterator(local / "modules", ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            std::uintmax_t size = 0;
            for (const auto& file : std::filesystem::directory_iterator(entry.path(), ec)) {
                if (file.is_regular_file(ec)) {
                    size += file.file_size(ec);
                }
            }
            out << entry.path().filename().string() << "  (" << size << " bytes)\n";
        }
    }

    for (const auto& [name, content] : request.extra_files) {
        // 附件名只允许相对路径；不能借诊断包写到目录外
        const std::filesystem::path relative{name};
        bool unsafe = name.empty() || relative.is_absolute();
        for (const auto& part : relative) {
            if (part == "..") {
                unsafe = true;
                break;
            }
        }
        if (unsafe) {
            error = "诊断包附件名非法：" + name;
            std::filesystem::remove_all(staging, ec);
            return false;
        }
        const std::filesystem::path target = staging / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out << content;
    }

    const bool ok = create_zip(staging, output, error);
    std::filesystem::remove_all(staging, ec);
    return ok;
#else
    (void)request;
    error = "当前平台不支持诊断包";
    return false;
#endif
}

} // namespace fgb::diagnostics
