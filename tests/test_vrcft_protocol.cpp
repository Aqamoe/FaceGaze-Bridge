#include "vrcft/protocol.hpp"
#include "vrcft/update_data.hpp"

#include "test_framework.hpp"

#include <array>
#include <cmath>
#include <cstring>

using namespace fgb;
using namespace fgb::vrcft;

namespace {

void put_f32(std::byte* p, float v) { std::memcpy(p, &v, sizeof(v)); }
void put_i32(std::byte* p, std::int32_t v) { std::memcpy(p, &v, sizeof(v)); }

// 构造一个 424 字节的 ReplyUpdate 包体（structSize + 420 字节结构）
std::array<std::byte, 4 + kUpdateStructSize> make_update_body() {
    std::array<std::byte, 4 + kUpdateStructSize> body{};
    put_i32(body.data(), static_cast<std::int32_t>(kUpdateStructSize));
    return body;
}

} // namespace

FGB_TEST(vrcft_header_roundtrip) {
    std::array<std::byte, 64> buf{};
    const std::size_t n = write_simple(PacketType::EventUpdate, buf);
    FGB_CHECK(n == 8);

    PacketHeader header{};
    FGB_CHECK(parse_header(std::span<const std::byte>{buf.data(), n}, header));
    FGB_CHECK(header.type == PacketType::EventUpdate);
    FGB_CHECK(header.body.empty());

    // 错误 magic 必须被拒绝
    std::array<std::byte, 8> bad{};
    bad[0] = std::byte{0x00};
    PacketHeader ignored{};
    FGB_CHECK(!parse_header(bad, ignored));
}

FGB_TEST(vrcft_flags_packet) {
    std::array<std::byte, 16> buf{};
    const std::size_t n = write_flags(PacketType::EventInit, true, true, buf);
    FGB_CHECK(n == 9);

    // EventInit 与 ReplyGetSupported 载荷同构，用后者解析验证
    std::array<std::byte, 16> reply{};
    write_flags(PacketType::ReplyGetSupported, true, false, reply);

    ReplySupported supported{};
    FGB_CHECK(parse_reply_supported(std::span<const std::byte>{reply.data(), 9}, supported));
    FGB_CHECK(supported.eye);
    FGB_CHECK(!supported.expression);

    write_flags(PacketType::ReplyGetSupported, false, true, reply);
    FGB_CHECK(parse_reply_supported(std::span<const std::byte>{reply.data(), 9}, supported));
    FGB_CHECK(!supported.eye);
    FGB_CHECK(supported.expression);
}

FGB_TEST(vrcft_status_packet_layout) {
    std::array<std::byte, 32> buf{};
    const std::size_t n = write_status(ModuleState::Active, true, true, buf);
    FGB_CHECK(n == 14);
    // Active = 1，小端
    FGB_CHECK(buf[8] == std::byte{0x01});
    FGB_CHECK(buf[9] == std::byte{0x00});
    FGB_CHECK(buf[12] == std::byte{0x01});
    FGB_CHECK(buf[13] == std::byte{0x01});
}

FGB_TEST(vrcft_handshake_parse) {
    // magic(4) type(4) challenge(5) pid(4) pathLen(4) path
    const char* path = R"(C:\Users\x\AppData\Roaming\VRCFaceTracking\CustomLibs\VRCFTPicoModule.dll)";
    const std::size_t path_len = std::strlen(path);

    std::array<std::byte, 21 + 128> buf{};
    std::memcpy(buf.data(), kMagic.data(), 4);
    put_i32(buf.data() + 4, 1);
    std::memcpy(buf.data() + 8, kChallenge.data(), 5);
    put_i32(buf.data() + 13, 12345);
    put_i32(buf.data() + 17, static_cast<std::int32_t>(path_len));
    std::memcpy(buf.data() + 21, path, path_len);

    HandshakeInfo info{};
    FGB_CHECK(parse_handshake(std::span<const std::byte>{buf.data(), 21 + path_len}, info));
    FGB_CHECK(info.challenge_ok);
    FGB_CHECK(info.pid == 12345);
    FGB_CHECK(info.module_path == path);

    // 挑战字被篡改时必须报不通过
    buf[8] = std::byte{0x00};
    HandshakeInfo tampered{};
    FGB_CHECK(!parse_handshake(std::span<const std::byte>{buf.data(), 21 + path_len}, tampered));
    FGB_CHECK(!tampered.challenge_ok);
}

FGB_TEST(vrcft_invalid_float_is_not_nan) {
    // INVALID_FLOAT 是 0x4F800000（4294967296.0f），不是 NaN
    const float invalid = 4294967295.0f;
    FGB_CHECK(is_invalid(invalid));
    FGB_CHECK(is_invalid(4294967296.0f));
    FGB_CHECK(!is_invalid(0.0f));
    FGB_CHECK(!is_invalid(1.0f));
    FGB_CHECK(!is_invalid(-1.0f));
    FGB_CHECK(!std::isnan(invalid));
}

FGB_TEST(vrcft_decode_update) {
    auto body = make_update_body();
    std::byte* p = body.data() + 4;
    put_f32(p + 8, 0.5f);   // left_gaze_x
    put_f32(p + 12, -0.25f); // left_gaze_y
    put_f32(p + 20, 0.75f);  // left_openness
    put_f32(p + 36, 0.25f);  // right_openness
    put_f32(p + 40, 12.0f);  // head_yaw
    put_f32(p + 64 + 22 * 4, 0.6f); // JawOpen (index 22)

    UpdateData data{};
    FGB_CHECK(decode_update(body, data));
    FGB_CHECK_NEAR(data.left_gaze_x, 0.5, 1e-6);
    FGB_CHECK_NEAR(data.left_gaze_y, -0.25, 1e-6);
    FGB_CHECK_NEAR(data.left_openness, 0.75, 1e-6);
    FGB_CHECK_NEAR(data.right_openness, 0.25, 1e-6);
    FGB_CHECK_NEAR(data.head_yaw, 12.0, 1e-6);
    FGB_CHECK_NEAR(data.shapes[22], 0.6, 1e-6);
    FGB_CHECK(data.has_eye_data());
    FGB_CHECK(data.has_expression_data());

    // 结构体尺寸不符必须拒绝，不能错位解析
    auto wrong = make_update_body();
    put_i32(wrong.data(), 416);
    UpdateData rejected{};
    FGB_CHECK(!decode_update(wrong, rejected));
}

FGB_TEST(vrcft_apply_gaze_to_frame) {
    auto body = make_update_body();
    std::byte* p = body.data() + 4;
    put_f32(p + 8, 0.5f);  // 右 0.5 → yaw 15°（满量程 30°）
    put_f32(p + 12, 0.5f); // 上 0.5 → pitch 15°
    put_f32(p + 20, 1.0f); // 左眼睁开
    put_f32(p + 24, 0.5f);
    put_f32(p + 28, 0.5f);
    put_f32(p + 36, 0.0f); // 右眼闭合

    UpdateData data{};
    FGB_CHECK(decode_update(body, data));

    FaceFrame frame = FaceFrame::neutral();
    ConvertOptions options;
    FGB_CHECK(apply_to_frame(data, options, frame));

    FGB_CHECK(frame.eyes.left.valid);
    FGB_CHECK_NEAR(frame.eyes.left.openness, 1.0, 1e-6);
    FGB_CHECK_NEAR(frame.eyes.right.openness, 0.0, 1e-6);
    FGB_CHECK_NEAR(frame.eyes.left.gazePitchYawDeg.x, 15.0, 1e-4);
    FGB_CHECK_NEAR(frame.eyes.left.gazePitchYawDeg.y, 15.0, 1e-4);

    // 方向向量应当与 pitch/yaw 互逆
    const Vec2 back = dir_to_pitch_yaw_deg(frame.eyes.left.gazeDirLocal);
    FGB_CHECK_NEAR(back.x, 15.0, 1e-3);
    FGB_CHECK_NEAR(back.y, 15.0, 1e-3);
}

FGB_TEST(vrcft_apply_skips_invalid_fields) {
    auto body = make_update_body();
    std::byte* p = body.data() + 4;
    // 全部留 INVALID_FLOAT
    for (int i = 0; i < 16; ++i) {
        put_f32(p + i * 4, 4294967296.0f);
    }
    for (std::size_t i = 0; i < kExpressionCount; ++i) {
        put_f32(p + 64 + i * 4, 4294967296.0f);
    }

    UpdateData data{};
    FGB_CHECK(decode_update(body, data));
    FGB_CHECK(!data.has_any_valid());

    FaceFrame frame = FaceFrame::neutral();
    const float before = frame.eyes.left.openness;
    ConvertOptions options;
    FGB_CHECK(!apply_to_frame(data, options, frame));
    // 无效数据不得污染帧
    FGB_CHECK_NEAR(frame.eyes.left.openness, before, 1e-6);
    FGB_CHECK(!frame.hasHeadPose);
}
