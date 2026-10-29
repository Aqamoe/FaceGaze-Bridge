// 流式输出（UDP 二进制 / WebSocket JSON）的线格式测试。
//
// 这两种输出是给外部消费者的契约：字段偏移、字节序、JSON 键名一旦发出去就很难改。
// 所以这里既做纯编码测试，也把 UDP 包真正发到 loopback 收回来检查。
#include "base/clock.hpp"
#include "base/json.hpp"
#include "sinks/throttle.hpp"
#include "sinks/udp_binary_sink.hpp"
#include "sinks/ws_json_sink.hpp"

#include "fgb_udp_frame.h"
#include "test_framework.hpp"

#include <cstring>
#include <string>

using namespace fgb;

namespace {

FaceFrame sample_frame() {
    FaceFrame frame = FaceFrame::neutral();
    frame.sequence    = 42;
    frame.captureTsNs = 123'456'789;
    frame.add_channel(static_cast<std::uint8_t>(channel::kExpression | channel::kEyes |
                                                channel::kHead));

    frame.set_shape(Shape::JawOpen, 0.5f);
    frame.eyes.left.valid         = true;
    frame.eyes.left.openness      = 0.25f;
    frame.eyes.left.gazeDirLocal  = Vec3{0.1f, 0.2f, -0.97f};
    frame.eyes.right.valid        = true;
    frame.eyes.right.openness     = 0.75f;
    frame.eyes.right.gazeDirLocal = Vec3{-0.1f, 0.1f, -0.99f};
    frame.eyes.hasVergence        = true;
    frame.eyes.vergenceDistanceM  = 1.24f;
    frame.hasHeadPose             = true;
    frame.headRotLocal            = Quat{0.0f, 0.03f, 0.0f, 0.999f};
    frame.headPosLocal            = Vec3{0.0f, 0.05f, 0.0f};
    return frame;
}

GazeState sample_gaze() {
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

FgbUdpFrameV1 as_wire(const UdpBinarySink::Datagram& datagram) {
    FgbUdpFrameV1 wire{};
    std::memcpy(&wire, datagram.data(), sizeof(wire));
    return wire;
}

} // namespace

FGB_TEST(udp_binary_serializes_fixed_layout) {
    const FaceFrame frame = sample_frame();
    const GazeState gaze  = sample_gaze();
    const auto datagram   = UdpBinarySink::serialize(frame, gaze, 2'000'000'000ULL);

    FGB_CHECK(datagram.size() == FGB_UDP_FRAME_SIZE);
    const FgbUdpFrameV1 wire = as_wire(datagram);
    FGB_CHECK(wire.magic == FGB_UDP_MAGIC);
    FGB_CHECK(wire.version == FGB_UDP_VERSION);
    FGB_CHECK(wire.header_size == 8);
    FGB_CHECK(wire.sequence == 42);
    FGB_CHECK(wire.capture_ts_ns == 123'456'789);
    FGB_CHECK(wire.publish_ts_ns == 2'000'000'000ULL);
    FGB_CHECK(wire.shape_count == kShapeCount);
    FGB_CHECK(wire.attributes & FGB_UDP_ATTR_LEFT_VALID);
    FGB_CHECK(wire.attributes & FGB_UDP_ATTR_RIGHT_VALID);
    FGB_CHECK(wire.attributes & FGB_UDP_ATTR_HAS_HEAD);
    FGB_CHECK(wire.attributes & FGB_UDP_ATTR_FIXATION);
    FGB_CHECK(wire.attributes & FGB_UDP_ATTR_GAZE_VALID);
    FGB_CHECK_NEAR(wire.left_openness, 0.25, 1e-6);
    FGB_CHECK_NEAR(wire.right_gaze_dir[0], -0.1, 1e-6);
    FGB_CHECK_NEAR(wire.vergence_distance_m, 1.24, 1e-6);
    FGB_CHECK_NEAR(wire.gaze_pitch_deg, 5.0, 1e-6);
    FGB_CHECK_NEAR(wire.head_rotation[3], 0.999, 1e-6);
    FGB_CHECK_NEAR(wire.head_position[1], 0.05, 1e-6);
    FGB_CHECK_NEAR(wire.shapes[index_of(Shape::JawOpen)], 0.5, 1e-6);
    FGB_CHECK(wire.shape_valid[index_of(Shape::JawOpen)] == 1);
}

FGB_TEST(udp_binary_sink_sends_a_datagram) {
    net::UdpSocket receiver;
    FGB_CHECK(receiver.open_receiver(0)); // 系统分配，避免 CI 上固定端口被占
    const std::uint16_t port = receiver.local_port();
    FGB_CHECK(port != 0);
    receiver.set_nonblocking(true);

    UdpBinaryConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    UdpBinarySink sink{config};
    FGB_CHECK(sink.start());
    FGB_CHECK(sink.target() == "127.0.0.1:" + std::to_string(port));

    sink.submit(sample_frame(), sample_gaze(), 2'000'000'000ULL);

    std::byte buffer[2048];
    int received = 0;
    for (int attempt = 0; attempt < 50 && received <= 0; ++attempt) {
        std::string from_ip;
        std::uint16_t from_port = 0;
        received = receiver.recv_from(buffer, from_ip, from_port);
        if (received <= 0) {
            clock::sleep_for_ns(5'000'000);
        }
    }
    FGB_CHECK(received == static_cast<int>(FGB_UDP_FRAME_SIZE));

    FgbUdpFrameV1 wire{};
    std::memcpy(&wire, buffer, sizeof(wire));
    FGB_CHECK(wire.magic == FGB_UDP_MAGIC);
    FGB_CHECK_NEAR(wire.shapes[index_of(Shape::JawOpen)], 0.5, 1e-6);
    FGB_CHECK(sink.stats().messages == 1);
}

FGB_TEST(ws_json_builds_active_shapes_only) {
    FaceFrame frame = sample_frame();
    frame.set_shape(Shape::BrowInnerUpLeft, 0.0f, false);

    const std::string text =
        WsJsonSink::build_json(frame, sample_gaze(), 1'500'000'000ULL, 7, "test", false, 0.01f);

    double t = 0.0;
    std::string src;
    FGB_CHECK(json::field_number(text, "t", t));
    FGB_CHECK_NEAR(t, 1500.0, 1e-6);
    FGB_CHECK(json::field_string(text, "src", src));
    FGB_CHECK(src == "test");
    FGB_CHECK(text.find("\"JawOpen\":0.500") != std::string::npos);
    FGB_CHECK(text.find("\"ok\":1") != std::string::npos);
    FGB_CHECK(text.find("\"vergence\":1.240") != std::string::npos);
    FGB_CHECK(text.find("\"BrowInnerUpLeft\"") == std::string::npos);
    FGB_CHECK(text.find("\"head\"") != std::string::npos);
    FGB_CHECK(text.find("\"q\":[0.000,0.030,0.000,0.999]") != std::string::npos);
}

FGB_TEST(ws_json_can_send_all_shapes) {
    FaceFrame frame = sample_frame();
    frame.set_shape(Shape::BrowInnerUpLeft, 0.0f, false);

    const std::string text =
        WsJsonSink::build_json(frame, sample_gaze(), 1'500'000'000ULL, 7, "test", true, 0.01f);
    FGB_CHECK(text.find("\"BrowInnerUpLeft\":0.000") != std::string::npos);
}

FGB_TEST(sink_throttle_hits_non_tick_aligned_rates) {
    // 引擎 120 Hz tick，模拟 1 秒；30/60/90 Hz 都不与 tick 对齐
    constexpr std::uint64_t kTick = 8'333'333;
    const auto count = [](std::uint64_t interval) {
        SinkThrottle throttle;
        int due = 0;
        for (int i = 0; i < 120; ++i) {
            if (throttle.due(static_cast<std::uint64_t>(i) * kTick, interval)) {
                ++due;
            }
        }
        return due;
    };

    const int hz30 = count(1'000'000'000ULL / 30);
    const int hz60 = count(1'000'000'000ULL / 60);
    const int hz90 = count(1'000'000'000ULL / 90);
    const int hz120 = count(kTick);
    FGB_CHECK(hz30 >= 29 && hz30 <= 31);
    FGB_CHECK(hz60 >= 59 && hz60 <= 61);
    FGB_CHECK(hz90 >= 88 && hz90 <= 92);
    FGB_CHECK(hz120 == 120);
}

FGB_TEST(ws_json_sink_listens_on_requested_port) {
    WsJsonConfig config;
    config.port = 0; // 系统分配，测试不抢固定端口
    WsJsonSink sink{config};
    FGB_CHECK(sink.start());
    FGB_CHECK(sink.port() != 0);
    sink.stop();
}
