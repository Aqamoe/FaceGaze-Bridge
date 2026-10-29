// VMC 输出：名字表两个方向的用法 + 真正发出去的包长什么样。
//
// 编码错误只会在联调时暴露（对方"没反应"，且不告诉你为什么），所以这里直接把
// 发到 loopback 的 UDP 包收回来，用 OSC 解码器逐条检查地址、类型标签与数值。
#include "osc/decoder.hpp"
#include "sinks/vmc_sink.hpp"
#include "vmc/vmc_map.hpp"

#include "base/clock.hpp"
#include "test_framework.hpp"

#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

using namespace fgb;

namespace {

constexpr std::uint16_t kTestPort = 39541;

struct Captured {
    bool got_jaw   = false;
    bool got_apply = false;
    bool got_head  = false;
    float jaw      = -1.0f;
    float blink_left = -1.0f;
    float head_pos_z = 0.0f;
    int messages   = 0;
};

// 收包直到拿到 Apply 或者超时。同一帧可能被拆成多个 bundle，所以要循环。
Captured drain(net::UdpSocket& socket, int max_rounds = 60) {
    Captured captured;
    std::byte buffer[2048];
    for (int round = 0; round < max_rounds; ++round) {
        std::string from_ip;
        std::uint16_t from_port = 0;
        const int size          = socket.recv_from(buffer, from_ip, from_port);
        if (size <= 0) {
            clock::sleep_for_ns(5'000'000); // 5 ms
            continue;
        }
        osc::for_each_message(std::span<const std::byte>{buffer, static_cast<std::size_t>(size)},
                              [&captured](const osc::MessageView& message) {
            ++captured.messages;
            if (message.address() == "/VMC/Ext/Blend/Apply") {
                captured.got_apply = true;
                return;
            }
            if (message.address() == "/VMC/Ext/Root/Pos") {
                captured.got_head = true;
                // 参数顺序：(name, px, py, pz, qx, qy, qz, qw)，名字占 index 0
                float z = 0.0f;
                if (message.get_float(3, z)) {
                    captured.head_pos_z = z;
                }
                return;
            }
            if (message.address() != "/VMC/Ext/Blend/Val") {
                return;
            }
            std::string_view name;
            float value = 0.0f;
            if (!message.get_string(0, name) || !message.get_float(1, value)) {
                return;
            }
            if (name == "jawOpen") {
                captured.got_jaw = true;
                captured.jaw     = value;
            } else if (name == "eyeBlinkLeft") {
                captured.blink_left = value;
            }
        });
        if (captured.got_apply) {
            break;
        }
    }
    return captured;
}

} // namespace

FGB_TEST(vmc_map_one_name_can_drive_several_shapes) {
    FaceFrame frame = FaceFrame::neutral();
    vmc::apply_blend(frame, "jawOpen", 0.5f);
    FGB_CHECK_NEAR(frame.shape(Shape::JawOpen), 0.5, 1e-6);
    FGB_CHECK(frame.shape_is_valid(Shape::JawOpen));

    // browInnerUp 是双侧统一的名字：两个形状都要被推
    vmc::apply_blend(frame, "browInnerUp", 0.3f);
    FGB_CHECK_NEAR(frame.shape(Shape::BrowInnerUpLeft), 0.3, 1e-6);
    FGB_CHECK_NEAR(frame.shape(Shape::BrowInnerUpRight), 0.3, 1e-6);

    // 未知名字不能改任何东西
    vmc::apply_blend(frame, "notABlendShape", 1.0f);
}

FGB_TEST(vmc_sink_emits_bundle_with_apply_and_head) {
    net::UdpSocket receiver;
    FGB_CHECK(receiver.open_receiver(kTestPort));
    receiver.set_nonblocking(true);

    VmcSinkConfig config;
    config.host            = "127.0.0.1";
    config.port            = kTestPort;
    config.min_interval_ns = 0; // 测试里不等节流
    config.send_head_pose  = true;

    VmcSink sink{config};
    FGB_CHECK(sink.start());

    FaceFrame frame = FaceFrame::neutral();
    frame.set_shape(Shape::JawOpen, 0.5f);
    frame.add_channel(channel::kExpression);
    frame.eyes.left.valid    = true;
    frame.eyes.left.openness = 0.25f; // 睁眼 0.25 → 眨眼 0.75
    frame.eyes.right.valid   = true;
    frame.add_channel(channel::kEyes);
    frame.hasHeadPose  = true;
    frame.headPosLocal = Vec3{0.1f, 0.2f, 0.3f};
    frame.headRotLocal = Quat{};
    frame.add_channel(channel::kHead);

    sink.submit(frame, GazeState{}, 1'000'000'000ULL);
    const Captured captured = drain(receiver);

    FGB_CHECK(captured.got_jaw);
    FGB_CHECK_NEAR(captured.jaw, 0.5, 1e-4);
    FGB_CHECK(captured.got_apply); // 没有 Apply 的话接收端不会提交
    FGB_CHECK(captured.got_head);
    // 输出侧要把 OpenXR 的 z 取反成 Unity 的 z
    FGB_CHECK_NEAR(captured.head_pos_z, -0.3, 1e-4);
    FGB_CHECK(captured.blink_left > 0.0f);
    FGB_CHECK(sink.stats().messages > 0);
    FGB_CHECK(sink.stats().bytes > 0);
}

FGB_TEST(vmc_sink_throttles_and_skips_unchanged) {
    net::UdpSocket receiver;
    FGB_CHECK(receiver.open_receiver(kTestPort));
    receiver.set_nonblocking(true);

    VmcSinkConfig config;
    config.host            = "127.0.0.1";
    config.port            = kTestPort;
    config.min_interval_ns = 100'000'000ULL; // 10 Hz
    config.heartbeat_ns    = 10'000'000'000ULL;
    config.send_head_pose  = false;

    VmcSink sink{config};
    FGB_CHECK(sink.start());

    FaceFrame frame = FaceFrame::neutral();
    frame.set_shape(Shape::JawOpen, 0.8f);
    frame.add_channel(channel::kExpression);

    sink.submit(frame, GazeState{}, 1'000'000'000ULL);
    const Captured first = drain(receiver);
    FGB_CHECK(first.got_apply);

    // 10 ms 后再来一帧：节流应把它压掉
    sink.submit(frame, GazeState{}, 1'010'000'000ULL);
    FGB_CHECK(sink.stats().suppressed >= 1);

    // 过了节流间隔但内容没变：不该重复发（心跳还没到）
    const std::uint64_t before = sink.stats().messages;
    sink.submit(frame, GazeState{}, 1'200'000'000ULL);
    FGB_CHECK(sink.stats().messages == before);
}
