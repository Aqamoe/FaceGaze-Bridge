// 原生 C ABI 模块（进程内加载）。
//
// v1 的线程模型：宿主在引擎线程按 tick 调用 update()，模块在 update/initialize 里
// 同步提交帧；不做跨线程保护。需要自驱线程的模块留到跨进程 ABI v2。
#pragma once

#include "fgb_module_api.h"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace fgb {

struct NativeSourceConfig {
    std::filesystem::path dll;
    std::string module_id;
    std::string module_name;
    std::string display_name = "native";
    int tick_hz              = 100;
    // 跨进程宿主用它把模块日志转发到共享内存；进程内模式留空。
    std::function<void(int, const char*)> log_sink;
};

class NativeModuleSource final : public ISource {
public:
    explicit NativeModuleSource(NativeSourceConfig config);
    ~NativeModuleSource() override;

    const char* name() const noexcept override { return config_.display_name.c_str(); }
    bool start() override;
    void stop() noexcept override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;
    std::optional<ModuleRuntimeStatus> module_status() const override;

    const std::string& last_error() const noexcept { return last_error_; }

private:
    static void host_log(void* ctx, int level, const char* message);
    static void host_report(void* ctx, int state, const char* detail);
    static FgbFrameSlotV1* host_acquire(void* ctx);
    static void host_commit(void* ctx, FgbFrameSlotV1* slot, uint64_t capture_ts_ns);
    static std::int32_t host_shape_index(void* ctx, const char* name);
    static FgbFrameSlotV2* host_acquire_v2(void* ctx);
    static void host_commit_v2(void* ctx, FgbFrameSlotV2* slot, uint64_t capture_ts_ns);

    void apply_slot(const FgbFrameSlotV1& slot, std::uint64_t capture_ts_ns);
    void apply_slot_v2(const FgbFrameSlotV2& slot, std::uint64_t capture_ts_ns);

    NativeSourceConfig config_;
    std::string module_dir_utf8_;

    void* library_            = nullptr; // HMODULE
    const FgbModuleV1* api_   = nullptr;
    const FgbModuleV2* api_v2_ = nullptr;
    bool v2_                  = false;
    void* self_               = nullptr;
    FgbHostServicesV1 services_{};
    FgbHostServicesV2 services_v2_{};
    FgbFrameSlotV1 slot_{};
    FgbFrameSlotV2 slot_v2_{};
    FaceFrame pending_{};
    bool has_frame_   = false;
    bool initialized_ = false;
    std::string last_error_;

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<bool> alive_{false};
};

} // namespace fgb
