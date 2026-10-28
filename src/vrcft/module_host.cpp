#include "vrcft/module_host.hpp"

#include "base/log.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>

namespace fgb::vrcft {
namespace {
constexpr char kTag[] = "vrcft-host";
}

#ifdef _WIN32
namespace {

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

} // namespace

ModuleHost::~ModuleHost() { release(); }

bool ModuleHost::launch(const std::filesystem::path& exe, const std::filesystem::path& module,
                        std::uint16_t port) noexcept {
    release();

    std::wstring cmd = quote(exe.wstring()) + L" --port " + std::to_wstring(port) +
                       L" --module-path " + quote(module.wstring()) + L" --parent-pid " +
                       std::to_wstring(::GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdInput   = INVALID_HANDLE_VALUE;
    si.hStdOutput  = INVALID_HANDLE_VALUE; // 子进程日志走 EventLog 回传，不需要控制台
    si.hStdError   = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi{};
    const std::wstring workdir = exe.parent_path().wstring();

    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, workdir.c_str(), &si, &pi);
    if (!ok) {
        FGB_ERROR(kTag, "CreateProcess 失败，GetLastError=%lu", ::GetLastError());
        return false;
    }

    process_ = reinterpret_cast<std::uintptr_t>(pi.hProcess);
    pid_     = pi.dwProcessId;
    ::CloseHandle(pi.hThread);

    // Job：句柄关闭时连同子进程一起终止
    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (!::AssignProcessToJobObject(job, reinterpret_cast<HANDLE>(process_))) {
            FGB_WARN(kTag, "AssignProcessToJobObject 失败，父进程退出可能留下孤儿子进程");
        }
        job_ = reinterpret_cast<std::uintptr_t>(job);
    }

    exit_code_ = -1;
    return true;
}

bool ModuleHost::alive() const noexcept {
    if (process_ == 0) {
        return false;
    }
    DWORD code = 0;
    if (!::GetExitCodeProcess(reinterpret_cast<HANDLE>(process_), &code)) {
        return false;
    }
    if (code == STILL_ACTIVE) {
        return true;
    }
    const_cast<ModuleHost*>(this)->exit_code_ = static_cast<std::int32_t>(code);
    return false;
}

bool ModuleHost::wait_exit(std::int32_t timeout_ms) noexcept {
    if (process_ == 0) {
        return true;
    }
    const DWORD r = ::WaitForSingleObject(reinterpret_cast<HANDLE>(process_),
                                          static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms));
    if (r == WAIT_OBJECT_0) {
        DWORD code = 0;
        ::GetExitCodeProcess(reinterpret_cast<HANDLE>(process_), &code);
        exit_code_ = static_cast<std::int32_t>(code);
        return true;
    }
    return false;
}

void ModuleHost::kill() noexcept {
    if (process_ != 0) {
        ::TerminateProcess(reinterpret_cast<HANDLE>(process_), 1);
        wait_exit(2000);
    }
}

void ModuleHost::release() noexcept {
    if (process_ != 0) {
        ::CloseHandle(reinterpret_cast<HANDLE>(process_));
        process_ = 0;
    }
    if (job_ != 0) {
        ::CloseHandle(reinterpret_cast<HANDLE>(job_)); // 触发 KILL_ON_JOB_CLOSE
        job_ = 0;
    }
    pid_ = 0;
}

#else // 非 Windows：M2 只做 Windows，其他平台保持可编译

ModuleHost::~ModuleHost() = default;
bool ModuleHost::launch(const std::filesystem::path&, const std::filesystem::path&,
                        std::uint16_t) noexcept {
    return false;
}
bool ModuleHost::alive() const noexcept { return false; }
bool ModuleHost::wait_exit(std::int32_t) noexcept { return true; }
void ModuleHost::kill() noexcept {}
void ModuleHost::release() noexcept {}

#endif

} // namespace fgb::vrcft
