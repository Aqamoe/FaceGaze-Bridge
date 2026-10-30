// 跨进程原生模块输入（V2 阶段 C）。
//
// 引擎不再直接 LoadLibrary：fgb-host-native.exe 负责加载模块并写共享内存，
// 本类只负责进程生命周期与 seqlock 读帧。模块崩溃时引擎主循环不受影响。
#pragma once

#include "fgb_native_host_shm.h"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fgb {

struct NativeHostConfig {
    std::filesystem::path host_exe;
    std::filesystem::path module_dll;
    std::filesystem::path log_path;
    std::string module_id;
    std::string module_name;
    std::string display_name = "native-host";
    std::string shm_name;
    int tick_hz = 100;
};

class NativeHostSource final : public ISource {
public:
    explicit NativeHostSource(NativeHostConfig config);
    ~NativeHostSource() override;

    const char* name() const noexcept override { return config_.display_name.c_str(); }
    bool start() override;
    void stop() noexcept override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;
    std::optional<ModuleRuntimeStatus> module_status() const override;

    const std::string& last_error() const noexcept { return last_error_; }
    const std::string& mapping_name() const noexcept { return mapping_name_; }
    std::uint32_t host_pid() const noexcept { return pid_; }

private:
    bool spawn();
    bool open_block();
    bool wait_ready();
    void close_handles() noexcept;
    bool process_alive() const noexcept;
    void mark_process_exit();
    std::string block_error() const;
    bool read_frame(FaceFrame& out);

    NativeHostConfig config_;
    std::string mapping_name_;
    std::string last_error_;

    void* process_ = nullptr; // HANDLE
    void* job_     = nullptr; // HANDLE
    void* mapping_ = nullptr; // HANDLE
    void* view_    = nullptr; // FgbNativeHostBlockV1*
    std::uint32_t pid_ = 0;
    bool started_      = false;
    bool exited_       = false;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t last_heartbeat_ns_ = 0;
    std::atomic<std::uint64_t> frames_{0};
    std::uint32_t capabilities_ = 0;
};

} // namespace fgb