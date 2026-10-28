// VMC (VirtualMotionCapture) 协议输入。
//
// 最省事的通用入口：VSeeFace / Warudo / 自制 Unity 都能直接吐这个，不需要装模块。
// 协议：UDP 39539，/VMC/Ext/Blend/Val (name, value) 逐条更新，/VMC/Ext/Blend/Apply 提交一帧。
#pragma once

#include "core/cont/seqlock.hpp"
#include "net/udp_socket.hpp"
#include "osc/decoder.hpp"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace fgb {

struct VmcSourceConfig {
    std::uint16_t port = 39539;
};

class VmcSource final : public ISource {
public:
    explicit VmcSource(VmcSourceConfig config);
    ~VmcSource() override;

    const char* name() const noexcept override { return "vmc"; }
    bool start() override;
    void stop() noexcept override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;

private:
    void receive_loop() noexcept;
    void handle_message(const osc::MessageView& message) noexcept;
    void handle_blend_value(std::string_view name, float value) noexcept;
    void handle_apply() noexcept;

    VmcSourceConfig config_;
    net::UdpSocket socket_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> dirty_{false};

    SeqLock<FaceFrame> snapshot_;
    FaceFrame pending_{};

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> parse_errors_{0};
};

} // namespace fgb
