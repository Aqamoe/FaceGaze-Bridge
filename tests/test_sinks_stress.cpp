// 多 Sink 并发压力测试（roadmap 4.9）。
//
// 目的不是"再测一遍编码"，而是验证 5 路输出同时在 120 Hz 引擎里扇出时：
//   1. 每个 sink 的节流计数与目标帧率一致（互不干扰）；
//   2. 共享内存/UDP/VMC 真的写出了数据，且没有错误；
//   3. 单帧扇出耗时没有数量级异常（真实延迟以 Release 下的
//      tools/probe_sinks_stress.py 为准，单测在 Debug/CI 上只做粗粒度护栏）。
#include "base/clock.hpp"
#include "sinks/shm_sink.hpp"
#include "sinks/udp_binary_sink.hpp"
#include "sinks/vmc_sink.hpp"
#include "sinks/vrchat_sink.hpp"
#include "sinks/ws_json_sink.hpp"

#include "fgb_shared_frame.h"
#include "net/udp_socket.hpp"
#include "test_framework.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace fgb;

namespace {

FaceFrame stress_frame(int tick) {
    FaceFrame frame = FaceFrame::neutral();
    frame.sequence    = static_cast<std::uint64_t>(tick);
    frame.captureTsNs = static_cast<std::uint64_t>(tick) * 8'333'333ULL;
    frame.add_channel(static_cast<std::uint8_t>(channel::kExpression | channel::kEyes |
                                                channel::kHead));

    // 每 tick 都变，避免死区/去重把输出压掉，压测要的是满负荷
    const float phase = static_cast<float>(tick) * 0.05f;
    frame.set_shape(Shape::JawOpen, 0.5f + 0.4f * std::sin(phase));
    frame.set_shape(Shape::MouthCornerPullLeft, 0.5f + 0.4f * std::cos(phase));
    frame.set_shape(Shape::BrowInnerUpLeft, 0.5f + 0.4f * std::sin(phase * 0.5f));

    frame.eyes.left.valid         = true;
    frame.eyes.left.openness      = 0.5f + 0.3f * std::sin(phase);
    frame.eyes.left.gazeDirLocal  = Vec3{0.1f, 0.2f, -0.97f};
    frame.eyes.right.valid        = true;
    frame.eyes.right.openness     = 0.5f + 0.3f * std::cos(phase);
    frame.eyes.right.gazeDirLocal = Vec3{-0.1f, 0.1f, -0.99f};
    frame.eyes.hasVergence        = true;
    frame.eyes.vergenceDistanceM  = 1.24f;
    frame.hasHeadPose             = true;
    frame.headRotLocal            = Quat{0.0f, 0.03f, 0.0f, 0.999f};
    frame.headPosLocal            = Vec3{0.0f, 0.05f, 0.0f};
    return frame;
}

GazeState stress_gaze() {
    GazeState gaze;
    gaze.valid      = true;
    gaze.dir_local  = Vec3{0.01f, 0.1f, -0.99f};
    gaze.pitch_deg  = 5.0f;
    gaze.yaw_deg    = 1.0f;
    gaze.vergence_m = 1.24f;
    gaze.confidence = 0.93f;
    gaze.fixation   = true;
    return gaze;
}

std::uint16_t open_receiver(net::UdpSocket& socket) {
    if (!socket.open_receiver(0)) {
        return 0;
    }
    socket.set_nonblocking(true);
    return socket.local_port();
}

} // namespace

FGB_TEST(five_sinks_run_together_at_target_rates) {
    // 三路 UDP 消费者：VRChat OSC、VMC、UDP 二进制各一个临时端口
    net::UdpSocket vrchat_receiver;
    net::UdpSocket vmc_receiver;
    net::UdpSocket udp_receiver;
    const std::uint16_t vrchat_port = open_receiver(vrchat_receiver);
    const std::uint16_t vmc_port    = open_receiver(vmc_receiver);
    const std::uint16_t udp_port    = open_receiver(udp_receiver);
    FGB_CHECK(vrchat_port != 0);
    FGB_CHECK(vmc_port != 0);
    FGB_CHECK(udp_port != 0);

    VrchatSinkConfig vrchat_cfg;
    vrchat_cfg.host             = "127.0.0.1";
    vrchat_cfg.port             = vrchat_port;
    vrchat_cfg.discover_avatar  = false;
    vrchat_cfg.use_oscquery     = false;
    vrchat_cfg.avatar_poll_ms   = 0;
    VrchatSink vrchat{vrchat_cfg};

    VmcSinkConfig vmc_cfg;
    vmc_cfg.host            = "127.0.0.1";
    vmc_cfg.port            = vmc_port;
    vmc_cfg.min_interval_ns = 1'000'000'000ULL / 90;
    VmcSink vmc{vmc_cfg};

    ShmSinkConfig shm_cfg;
    shm_cfg.name            = "fgb-stress-" + std::to_string(::GetTickCount64());
    shm_cfg.min_interval_ns = 1'000'000'000ULL / 90;
    ShmSink shm{shm_cfg};

    WsJsonConfig ws_cfg;
    ws_cfg.port            = 0; // 系统分配；无客户端时只走 accept/poll 路径
    ws_cfg.min_interval_ns = 1'000'000'000ULL / 90;
    WsJsonSink ws{ws_cfg};

    UdpBinaryConfig udp_cfg;
    udp_cfg.host            = "127.0.0.1";
    udp_cfg.port            = udp_port;
    udp_cfg.min_interval_ns = 1'000'000'000ULL / 90;
    UdpBinarySink udp{udp_cfg};

    FGB_CHECK(vrchat.start());
    FGB_CHECK(vmc.start());
    FGB_CHECK(shm.start());
    FGB_CHECK(ws.start());
    FGB_CHECK(udp.start());

    constexpr std::uint64_t kTick      = 1'000'000'000ULL / 120;
    constexpr int kTicks               = 120 * 3; // 3 秒 @ 120 Hz
    const GazeState gaze               = stress_gaze();
    std::uint64_t vrchat_ns            = 0;
    std::uint64_t vmc_ns               = 0;
    std::uint64_t shm_ns               = 0;
    std::uint64_t ws_ns                = 0;
    std::uint64_t udp_ns               = 0;

    for (int tick = 0; tick < kTicks; ++tick) {
        const std::uint64_t now = static_cast<std::uint64_t>(tick) * kTick;
        const FaceFrame frame   = stress_frame(tick);

        auto timed = [&](auto& sink, std::uint64_t& total_ns) {
            const auto start = std::chrono::steady_clock::now();
            sink.submit(frame, gaze, now);
            total_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        };
        timed(vrchat, vrchat_ns);
        timed(vmc, vmc_ns);
        timed(shm, shm_ns);
        timed(ws, ws_ns);
        timed(udp, udp_ns);
    }

    // 90 Hz、3 秒 → 约 270 帧；节流首帧立即到期，允许 ±3 的边界差
    // 注意：不能叫 near——windows.h 把 near/far 定义成空宏
    const auto close_to = [](std::uint64_t actual, std::uint64_t expected) {
        return actual + 3 >= expected && actual <= expected + 3;
    };
    FGB_CHECK(close_to(udp.stats().messages, 270));
    FGB_CHECK(close_to(shm.stats().messages, 270));

    // VMC/WS 的 messages 不是"帧数"（前者按 OSC 消息、后者按客户端投递），
    // 这里只要求真的发出过数据且没有错误
    FGB_CHECK(vmc.stats().messages > 0);
    FGB_CHECK(vmc.stats().bytes > 0);
    FGB_CHECK(ws.stats().suppressed > 0);
    FGB_CHECK(vrchat.stats().messages > 0);
    FGB_CHECK(vrchat.stats().bytes > 0);

    for (const auto* sink : {static_cast<ISink*>(&vrchat), static_cast<ISink*>(&vmc),
                             static_cast<ISink*>(&shm), static_cast<ISink*>(&ws),
                             static_cast<ISink*>(&udp)}) {
        FGB_CHECK(sink->stats().errors == 0);
    }

    // 粗粒度护栏：单帧 5 路扇出平均耗时不应接近 tick 预算（8.3 ms）。
    // Debug 版比 Release 慢数倍，阈值留足；真实数字由 Release 探针记录。
    const double per_frame_us =
        static_cast<double>(vrchat_ns + vmc_ns + shm_ns + ws_ns + udp_ns) / kTicks / 1000.0;
    std::printf("五路扇出平均每帧 %.1f µs（VRChat %.1f / VMC %.1f / SHM %.1f / WS %.1f / UDP %.1f）\n",
                per_frame_us, static_cast<double>(vrchat_ns) / kTicks / 1000.0,
                static_cast<double>(vmc_ns) / kTicks / 1000.0,
                static_cast<double>(shm_ns) / kTicks / 1000.0,
                static_cast<double>(ws_ns) / kTicks / 1000.0,
                static_cast<double>(udp_ns) / kTicks / 1000.0);
    FGB_CHECK(per_frame_us < 4000.0);

    // 共享内存按消费者视角读回：seqlock 必须停在偶数，帧内容与最后一 tick 一致
    const std::wstring wide(shm.mapping_name().begin(), shm.mapping_name().end());
    HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    FGB_CHECK(mapping != nullptr);
    if (mapping != nullptr) {
        auto* block = static_cast<const FgbSharedBlockV1*>(
            ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(FgbSharedBlockV1)));
        FGB_CHECK(block != nullptr);
        if (block != nullptr) {
            FGB_CHECK((block->sequence & 1u) == 0);
            FGB_CHECK(block->magic == FGB_SHM_MAGIC);
            const FgbSharedFrameV1& last = block->slots[block->active_slot];
            FGB_CHECK(last.sequence == shm.stats().messages);
            ::UnmapViewOfFile(block);
        }
        ::CloseHandle(mapping);
    }

    vrchat.stop();
    vmc.stop();
    shm.stop();
    ws.stop();
    udp.stop();
}
