// VRCFT 模块兼容数据源。
//
// 一个完整会话的时序（详见 docs/adr/0003）：
//   启动进程 → 握手(原样回 ACK) → GetSupported → Init → **Status(Active)**
//   → 循环 EventUpdate/ReplyUpdate → Teardown
//
// 两个已知的坑，这里都做了防御：
//  1. 不发 Status(Active)，模块 Update() 空转，返回全 INVALID_FLOAT
//  2. 有些模块（如 VRCFTPicoModule）的 socket 重建有竞态，导致"连上了但没数据"，
//     此时重启模块进程有机会恢复，所以内置重试
#pragma once

#include "capture/capture_file.hpp"
#include "core/cont/seqlock.hpp"
#include "sources/source.hpp"
#include "vrcft/module_host.hpp"
#include "vrcft/protocol.hpp"
#include "vrcft/sandbox_server.hpp"
#include "vrcft/update_data.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace fgb::vrcft {

struct VrcftSourceConfig {
    std::filesystem::path module_process_exe;
    std::filesystem::path module_dll;

    // 多实例时的显示名（"--eye-source vrcft[2]" 靠它匹配），单实例就是 "vrcft"
    std::string display_name = "vrcft";

    int tick_hz              = 100;
    int no_data_threshold    = 30;   // 连续多少帧全无效判定"无数据"
    int max_restarts         = 3;
    float eye_full_scale_deg = 30.0f;

    // 非空时把每一个 ReplyUpdate 原样录进该文件（见 capture/capture_file.hpp）
    std::filesystem::path record_path;

    int handshake_timeout_ms = 5000;
    int init_timeout_ms      = 10000;
    int teardown_wait_ms     = 2000;
};

class VrcftSource final : public ISource {
public:
    explicit VrcftSource(VrcftSourceConfig config);
    ~VrcftSource() override;

    const char* name() const noexcept override { return config_.display_name.c_str(); }
    bool start() override;
    void stop() noexcept override;
    // 停掉当前 ModuleProcess 会话再重新拉起：VRCFT 模块本来就在外部进程里，
    // 「重启实例」不需要重启引擎。
    bool restart() override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;
    std::optional<ModuleRuntimeStatus> module_status() const override;

    // 诊断信息，供 UI 显示"为什么没数据"
    const std::string& last_error() const noexcept { return last_error_; }
    // 模块自己报的最后一条 error（已压平成一行的截断版），没有则为空
    const std::string& module_error() const noexcept { return module_error_; }
    int restart_count() const noexcept { return restarts_.load(std::memory_order_relaxed); }
    bool module_alive() const noexcept { return host_.alive(); }
    std::size_t recorded_frames() const noexcept { return recorder_.frames(); }

private:
    void run_loop() noexcept;
    bool run_session() noexcept;
    bool wait_packet(PacketType expected, int timeout_ms) noexcept;
    void drain_incoming() noexcept;
    void dispatch(std::span<const std::byte> packet) noexcept;
    void log_event(std::span<const std::byte> packet) noexcept;
    void publish() noexcept;

    VrcftSourceConfig config_;
    SandboxServer server_;
    ModuleHost host_;
    SeqLock<FaceFrame> snapshot_;
    FaceFrame pending_{};

    std::array<std::byte, kMaxPacketSize> rx_{};
    std::size_t last_len_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    capture::Writer recorder_;
    std::uint64_t capture_t0_ = 0;
    std::atomic<bool> dirty_{false};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> packets_{0};

    std::string last_error_;
    std::string module_error_;
    // 会话失败重启由 run_loop 线程写，界面在引擎线程读，必须原子
    std::atomic<int> restarts_{0};
};

} // namespace fgb::vrcft
