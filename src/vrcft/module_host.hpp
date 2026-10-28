// 模块宿主进程的生命周期管理。
//
// 用 Job Object 保证父进程消失时子进程一起死，避免 VRCFT 那种
// "孤儿 ModuleProcess 占着硬件端口"的老问题。
#pragma once

#include <cstdint>
#include <filesystem>

namespace fgb::vrcft {

class ModuleHost {
public:
    ModuleHost() noexcept = default;
    ~ModuleHost();

    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    // 启动 VRCFaceTracking.ModuleProcess.exe。工作目录自动设为 exe 所在目录
    // （它靠这个找依赖 DLL），stdout/stderr 丢弃。
    bool launch(const std::filesystem::path& exe, const std::filesystem::path& module,
                std::uint16_t port) noexcept;

    bool alive() const noexcept;
    // 返回是否在超时内退出
    bool wait_exit(std::int32_t timeout_ms) noexcept;
    void kill() noexcept;

    std::uint32_t pid() const noexcept { return pid_; }
    std::int32_t exit_code() const noexcept { return exit_code_; }

private:
    void release() noexcept;

    std::uintptr_t process_ = 0;
    std::uintptr_t job_     = 0;
    std::uint32_t pid_      = 0;
    std::int32_t exit_code_ = -1;
};

} // namespace fgb::vrcft
