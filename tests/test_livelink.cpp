// ARKit LiveLink 解析与 UDP 源。
//
// 协议布局按 Epic 的 AppleARKitLiveLinkSource.cpp；字段顺序用真实抓包
// （61 曲线、320 字节帧）核对过。这里用合成的 61 条曲线钉住：
// 52 个 ARKit 名字的顺序、blink → openness、look* → 视线方向、头姿四元数、
// 以及"没有头姿时不抢头姿通道"。
#include "base/clock.hpp"
#include "net/udp_socket.hpp"
#include "sources/livelink_source.hpp"

#include "test_framework.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace fgb;

namespace {

constexpr float kPi = 3.14159265358979323846f;

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(value & 0xFF));
}

void put_f32(std::vector<std::byte>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

void put_string(std::vector<std::byte>& out, const std::string& text) {
    put_u32(out, static_cast<std::uint32_t>(text.size()));
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
}

std::vector<std::byte> make_frame(const std::vector<float>& curves,
                                  const std::string& subject = "iPhone",
                                  std::uint8_t version          = 6,
                                  bool length_prefix            = false) {
    std::vector<std::byte> body;
    body.push_back(static_cast<std::byte>(version));
    put_string(body, "DEADC0DE-1337-1337-1337-CAFEBABE");
    put_string(body, subject);
    put_u32(body, 42);    // frame_number
    put_f32(body, 0.25f); // sub_frame
    put_u32(body, 60);    // fps 分子
    put_u32(body, 1);     // fps 分母
    body.push_back(static_cast<std::byte>(curves.size()));
    for (const float value : curves) {
        put_f32(body, value);
    }
    if (!length_prefix) {
        return body;
    }
    std::vector<std::byte> framed;
    put_u32(framed, static_cast<std::uint32_t>(body.size()));
    framed.insert(framed.end(), body.begin(), body.end());
    return framed;
}

std::vector<float> neutral_curves(std::size_t count = 61) { return std::vector<float>(count, 0.0f); }

bool wait_for_frame(LiveLinkSource& source, FaceFrame& frame, int timeout_ms = 1000) {
    const std::uint64_t deadline = clock::now_ns() + static_cast<std::uint64_t>(timeout_ms) * 1'000'000ULL;
    while (clock::now_ns() < deadline) {
        if (source.poll(frame)) {
            return true;
        }
        clock::sleep_for_ns(2'000'000);
    }
    return false;
}

constexpr std::uint16_t kTestPort = 19111;

} // namespace

FGB_TEST(livelink_parses_shapes_eyes_and_head) {
    auto curves       = neutral_curves();
    curves[0]         = 1.0f;  // eyeBlinkLeft → 闭眼
    curves[17]        = 0.5f;  // jawOpen
    curves[4]         = 1.0f;  // eyeLookUpLeft
    curves[52]        = 0.5f;  // HeadYaw，归一化 → 45°

    FaceFrame frame;
    LiveLinkFrameInfo info;
    FGB_CHECK(parse_livelink_frame(make_frame(curves), frame, &info));

    FGB_CHECK(info.version == 6);
    FGB_CHECK(info.curve_count == 61);
    FGB_CHECK(info.subject == "iPhone");

    FGB_CHECK_NEAR(frame.shapes[index_of(Shape::JawOpen)], 0.5, 1e-5);
    FGB_CHECK(frame.shape_is_valid(Shape::JawOpen));
    FGB_CHECK(frame.eyes.left.valid);
    FGB_CHECK_NEAR(frame.eyes.left.openness, 0.0, 1e-5);
    FGB_CHECK_NEAR(frame.eyes.left.gazePitchYawDeg.x, 30.0, 1e-3);
    FGB_CHECK_NEAR(frame.eyes.left.gazeDirLocal.y, 0.5, 1e-3); // sin(30°)

    FGB_CHECK(frame.hasHeadPose);
    FGB_CHECK(frame.has_channel(channel::kExpression));
    FGB_CHECK(frame.has_channel(channel::kEyes));
    FGB_CHECK(frame.has_channel(channel::kHead));
    // UE yaw + → OpenXR yaw −：45° 对应 qy = sin(-22.5°)
    FGB_CHECK_NEAR(frame.headRotLocal.y, std::sin(-0.125f * kPi), 1e-4);
    FGB_CHECK_NEAR(frame.headRotLocal.w, std::cos(0.125f * kPi), 1e-4);
}

FGB_TEST(livelink_eye_direction_uses_look_shapes) {
    auto curves                        = neutral_curves();
    curves[3]                          = 1.0f; // 左眼向外 → 佩戴者左侧
    curves[10]                         = 1.0f; // 右眼向外 → 佩戴者右侧

    FaceFrame frame;
    FGB_CHECK(parse_livelink_frame(make_frame(curves), frame));
    FGB_CHECK_NEAR(frame.eyes.left.gazePitchYawDeg.y, -30.0, 1e-3);
    FGB_CHECK_NEAR(frame.eyes.left.gazeDirLocal.x, -0.5, 1e-3); // yaw 正 = 右
    FGB_CHECK_NEAR(frame.eyes.right.gazePitchYawDeg.y, 30.0, 1e-3);
    FGB_CHECK_NEAR(frame.eyes.right.gazeDirLocal.x, 0.5, 1e-3);
}

FGB_TEST(livelink_accepts_length_prefix_and_52_curves) {
    auto curves = neutral_curves(52);
    curves[17]  = 0.25f;

    FaceFrame frame;
    LiveLinkFrameInfo info;
    FGB_CHECK(parse_livelink_frame(make_frame(curves, "iPhone", 6, true), frame, &info));
    FGB_CHECK(info.curve_count == 52);
    FGB_CHECK_NEAR(frame.shapes[index_of(Shape::JawOpen)], 0.25, 1e-5);
    // 52 条曲线没有头姿：不能声明 head 通道，否则会盖掉真正的头姿源
    FGB_CHECK(!frame.hasHeadPose);
    FGB_CHECK(!frame.has_channel(channel::kHead));
    FGB_CHECK(frame.has_channel(channel::kEyes));
}

FGB_TEST(livelink_rejects_malformed_packets) {
    auto curves = neutral_curves();
    auto packet = make_frame(curves);

    FaceFrame frame;
    FGB_CHECK(!parse_livelink_frame(std::span<const std::byte>{}, frame));
    FGB_CHECK(!parse_livelink_frame(std::span<const std::byte>{packet.data(), 20}, frame));

    auto bad_count = make_frame(neutral_curves(62));
    FGB_CHECK(!parse_livelink_frame(bad_count, frame));

    auto zero_curves = make_frame({});
    FGB_CHECK(!parse_livelink_frame(zero_curves, frame));

    // 声称 61 条曲线但只带了一半数据
    auto truncated = make_frame(curves);
    truncated.resize(truncated.size() - 100);
    FGB_CHECK(!parse_livelink_frame(truncated, frame));
}

FGB_TEST(livelink_source_receives_udp) {
    LiveLinkSource source{LiveLinkSourceConfig{kTestPort}};
    FGB_CHECK(source.start());

    net::UdpSocket sender;
    FGB_CHECK(sender.open_sender());

    auto curves = neutral_curves();
    curves[17]  = 0.6f;
    const auto packet = make_frame(curves);
    FGB_CHECK(sender.send_to(packet, "127.0.0.1", kTestPort));

    FaceFrame frame;
    FGB_CHECK(wait_for_frame(source, frame));
    FGB_CHECK_NEAR(frame.shapes[index_of(Shape::JawOpen)], 0.6, 1e-5);

    const SourceStats stats = source.stats();
    FGB_CHECK(stats.packets >= 1);
    FGB_CHECK(stats.frames >= 1);
    FGB_CHECK(stats.parse_errors == 0);

    source.stop();
}
