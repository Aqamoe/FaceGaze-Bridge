// UDP 二进制输出：给串流软件/自研客户端的低延迟全量帧。
//
// 与 VMC / WebSocket JSON 的分工：
//   VMC        给认 VMC 的现成软件
//   WS JSON    给网页/脚本，可读性优先
//   UDP binary 给"每帧都要、不想解析 JSON"的消费者（注视点编码、自研引擎）
//
// 一个数据报 = 一帧，布局固定且小端；不发名字表，形状顺序与共享内存一致。
// 协议见 sdk/fgb_udp_frame.h 与 sdk/docs/streaming-outputs.md。
#pragma once

#include "net/udp_socket.hpp"
#include "sinks/sink.hpp"
#include "sinks/throttle.hpp"

#include "fgb_udp_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fgb {

struct UdpBinaryConfig {
    std::string host    = "127.0.0.1";
    std::uint16_t port  = 0; // 0 = 未启用（CLI 必须显式给 host:port）
    std::uint64_t min_interval_ns = 0; // 0 = 每个引擎 tick 都发
};

class UdpBinarySink final : public ISink {
public:
    using Datagram = std::array<std::byte, FGB_UDP_FRAME_SIZE>;

    explicit UdpBinarySink(UdpBinaryConfig config);
    ~UdpBinarySink() override = default;

    const char* name() const noexcept override { return "udp-out"; }
    bool start() override;
    void stop() noexcept override;
    void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) override;
    SinkStats stats() const noexcept override { return stats_; }

    // 供测试：把一帧编码成线上字节
    static Datagram serialize(const FaceFrame& frame, const GazeState& gaze,
                              std::uint64_t now_ns) noexcept;

    const UdpBinaryConfig& config() const noexcept { return config_; }
    std::string target() const;

private:
    UdpBinaryConfig config_;
    net::UdpSocket socket_;
    SinkStats stats_{};
    SinkThrottle throttle_;
    bool started_ = false;
};

} // namespace fgb
