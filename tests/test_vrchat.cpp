#include "sinks/vrchat_face.hpp"
#include "sinks/vrchat_sink.hpp"

#include "core/model/frame.hpp"
#include "core/model/shape.hpp"
#include "net/udp_socket.hpp"
#include "osc/decoder.hpp"

#include "test_framework.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

using namespace fgb;

FGB_TEST(eye_mode_parsing) {
    EyeOutputMode mode = EyeOutputMode::both;
    FGB_CHECK(parse_eye_output_mode("parameters", mode));
    FGB_CHECK(mode == EyeOutputMode::parameters);
    FGB_CHECK(parse_eye_output_mode("native", mode));
    FGB_CHECK(mode == EyeOutputMode::native);
    FGB_CHECK(parse_eye_output_mode("both", mode));
    FGB_CHECK(mode == EyeOutputMode::both);
    FGB_CHECK(!parse_eye_output_mode("PARAMETERS", mode));
    FGB_CHECK(!parse_eye_output_mode("", mode));

    FGB_CHECK(std::string_view{to_string(EyeOutputMode::parameters)} == "parameters");
}

FGB_TEST(face_mode_parsing) {
    FaceOutputMode mode = FaceOutputMode::parameters;
    FGB_CHECK(parse_face_output_mode("auto", mode));
    FGB_CHECK(mode == FaceOutputMode::auto_);
    FGB_CHECK(parse_face_output_mode("native", mode));
    FGB_CHECK(mode == FaceOutputMode::native);
    FGB_CHECK(parse_face_output_mode("parameters", mode));
    FGB_CHECK(mode == FaceOutputMode::parameters);
    FGB_CHECK(parse_face_output_mode("params", mode));
    FGB_CHECK(mode == FaceOutputMode::parameters);
    FGB_CHECK(!parse_face_output_mode("AUTO", mode));
    FGB_CHECK(!parse_face_output_mode("blob", mode));

    FGB_CHECK(std::string_view{to_string(FaceOutputMode::auto_)} == "auto");
    FGB_CHECK(std::string_view{to_string(FaceOutputMode::native)} == "native");
    FGB_CHECK(std::string_view{to_string(FaceOutputMode::parameters)} == "parameters");
}

FGB_TEST(native_face_payload_is_zero_for_neutral_frame) {
    // 中性帧 = 睁眼正视、无瞳孔、无表情。整包必须全 0，
    // 否则用户会看到"不说话也在动脸"。
    const FaceFrame frame = FaceFrame::neutral();
    std::array<std::uint8_t, kNativeFaceBytes> bytes{};
    build_native_face_payload(frame, 30.0f, bytes);

    bool all_zero = true;
    for (std::uint8_t value : bytes) {
        all_zero = all_zero && value == 0;
    }
    FGB_CHECK(all_zero);
}

FGB_TEST(native_face_payload_layout) {
    FaceFrame frame = FaceFrame::neutral();
    // 眼：右眼向右上，左眼向左下（满量程 30°）
    frame.eyes.right.gazePitchYawDeg = {15.0f, 30.0f};
    frame.eyes.left.gazePitchYawDeg  = {-15.0f, -30.0f};
    frame.eyes.right.openness        = 0.0f;
    frame.eyes.left.openness         = 0.5f;
    frame.eyes.right.pupilValid      = true;
    frame.eyes.right.pupilDilation   = 0.5f;  // 放大
    frame.eyes.left.pupilValid       = true;
    frame.eyes.left.pupilDilation    = -0.25f; // 收缩
    frame.set_shape(Shape::JawOpen, 1.0f);
    frame.set_shape(Shape::TongueOut, 0.5f);

    std::array<std::uint8_t, kNativeFaceBytes> bytes{};
    build_native_face_payload(frame, 30.0f, bytes);

    // 0-7：LookOut/In/Up/Down × Right/Left；左右眼的"向外"方向相反
    FGB_CHECK(bytes[0] == 255); // EyeLookOutRight = +yaw
    FGB_CHECK(bytes[1] == 0);   // EyeLookInRight
    FGB_CHECK(bytes[2] == 128); // EyeLookUpRight = +pitch (15/30 = 0.5)
    FGB_CHECK(bytes[3] == 0);   // EyeLookDownRight
    FGB_CHECK(bytes[4] == 255); // EyeLookOutLeft = -yaw
    FGB_CHECK(bytes[5] == 0);   // EyeLookInLeft
    FGB_CHECK(bytes[6] == 0);   // EyeLookUpLeft
    FGB_CHECK(bytes[7] == 128); // EyeLookDownLeft = -pitch

    // 8-9：睁眼度取反
    FGB_CHECK(bytes[8] == 255); // EyeClosedRight
    FGB_CHECK(bytes[9] == 128); // EyeClosedLeft

    // 10-13：瞳孔放大与收缩是互斥的两组字节
    FGB_CHECK(bytes[10] == 128); // EyeDilationRight
    FGB_CHECK(bytes[11] == 0);   // EyeDilationLeft
    FGB_CHECK(bytes[12] == 0);   // EyeConstrictRight
    FGB_CHECK(bytes[13] == 64);  // EyeConstrictLeft

    // 14-101：形状按枚举顺序一一对应，中间不插表
    const auto jaw   = kNativeFaceEyeBytes + index_of(Shape::JawOpen);
    const auto tongue = kNativeFaceEyeBytes + index_of(Shape::TongueOut);
    FGB_CHECK(bytes[jaw] == 255);
    FGB_CHECK(bytes[tongue] == 128);
    FGB_CHECK(bytes[kNativeFaceEyeBytes] == 0);  // EyeSquintRight
    FGB_CHECK(bytes[kNativeFaceBytes - 1] == 0); // NeckFlexLeft
}

FGB_TEST(native_face_payload_pupil_from_diameter) {
    // 只给毫米直径时按 VRCFT 口径：5 mm 中性，10 mm 满放大，0 没有意义
    FaceFrame frame = FaceFrame::neutral();
    frame.eyes.right.pupilValid      = false;
    frame.eyes.right.pupilDiameterMm = 10.0f;
    frame.eyes.left.pupilValid       = false;
    frame.eyes.left.pupilDiameterMm  = 2.5f;

    std::array<std::uint8_t, kNativeFaceBytes> bytes{};
    build_native_face_payload(frame, 30.0f, bytes);

    FGB_CHECK(bytes[10] == 255); // EyeDilationRight
    FGB_CHECK(bytes[12] == 0);   // EyeConstrictRight
    FGB_CHECK(bytes[11] == 0);   // EyeDilationLeft
    FGB_CHECK(bytes[13] == 128); // EyeConstrictLeft (2.5/5-1 = -0.5)
}

namespace {

// 起一个回环接收端，给 sink 一个真实出口；返回端口 0 表示失败
std::uint16_t open_receiver(net::UdpSocket& socket) {
    if (!socket.open_receiver(0)) {
        return 0;
    }
    socket.set_nonblocking(true);
    return socket.local_port();
}

// UDP 是异步的：等一小会儿再判定"没收到"，避免在 CI 上偶发失败
int recv_datagram(net::UdpSocket& socket, std::span<std::byte> buffer) {
    std::string from_ip;
    std::uint16_t from_port = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int received = socket.recv_from(buffer, from_ip, from_port);
        if (received > 0) {
            return received;
        }
        if (received < 0) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 0;
}

VrchatSinkConfig loopback_config(std::uint16_t port) {
    VrchatSinkConfig cfg;
    cfg.host             = "127.0.0.1";
    cfg.port             = port;
    cfg.discover_avatar  = false;
    cfg.use_oscquery     = false;
    cfg.avatar_poll_ms   = 0;
    return cfg;
}

} // namespace

FGB_TEST(vrchat_sink_sends_native_face_blob) {
    net::UdpSocket receiver;
    const std::uint16_t port = open_receiver(receiver);
    FGB_CHECK(port != 0);
    if (port == 0) {
        return;
    }

    VrchatSinkConfig cfg = loopback_config(port);
    cfg.face_mode        = FaceOutputMode::native;
    VrchatSink sink{cfg};
    FGB_CHECK(sink.start());

    FaceFrame frame = FaceFrame::neutral();
    frame.set_shape(Shape::JawOpen, 1.0f);
    GazeState gaze;
    sink.submit(frame, gaze, 1'000'000'000ULL);
    sink.stop();

    std::byte buffer[2048];
    const int received = recv_datagram(receiver, buffer);
    FGB_CHECK(received > 0);
    if (received <= 0) {
        return;
    }

    bool seen_face = false;
    bool seen_jaw  = false;
    const std::size_t count = osc::for_each_message(
        std::span<const std::byte>{buffer, static_cast<std::size_t>(received)},
        [&](const osc::MessageView& msg) {
            if (msg.address() == kAddrNativeFace) {
                seen_face = true;
                std::span<const std::byte> blob;
                FGB_CHECK(msg.get_blob(0, blob));
                FGB_CHECK(blob.size() == kNativeFaceBytes);
                if (blob.size() == kNativeFaceBytes) {
                    const std::size_t jaw = kNativeFaceEyeBytes + index_of(Shape::JawOpen);
                    seen_jaw = static_cast<std::uint8_t>(blob[jaw]) == 255;
                }
            }
            // 原生面捕模式下不应再往 avatar 参数里写表情
            FGB_CHECK(msg.address().rfind("/avatar/parameters/", 0) != 0);
        });
    FGB_CHECK(count > 0);
    FGB_CHECK(seen_face);
    FGB_CHECK(seen_jaw);
}

FGB_TEST(vrchat_sink_auto_falls_back_to_params_without_avatar_info) {
    net::UdpSocket receiver;
    const std::uint16_t port = open_receiver(receiver);
    FGB_CHECK(port != 0);
    if (port == 0) {
        return;
    }

    // 没有头像参数表时 full_face_tracking 是"不知道"，auto 必须按老模型处理
    VrchatSink sink{loopback_config(port)};
    FGB_CHECK(sink.start());

    FaceFrame frame = FaceFrame::neutral();
    frame.set_shape(Shape::JawOpen, 1.0f);
    GazeState gaze;
    sink.submit(frame, gaze, 1'000'000'000ULL);
    sink.stop();

    FGB_CHECK(!sink.status().face_native);
    FGB_CHECK(!sink.status().face_native_known);

    std::byte buffer[2048];
    const int received = recv_datagram(receiver, buffer);
    FGB_CHECK(received > 0);
    if (received <= 0) {
        return;
    }

    bool seen_face = false;
    bool seen_param = false;
    osc::for_each_message(std::span<const std::byte>{buffer, static_cast<std::size_t>(received)},
                          [&](const osc::MessageView& msg) {
                              if (msg.address() == kAddrNativeFace) {
                                  seen_face = true;
                              }
                              if (msg.address().rfind("/avatar/parameters/", 0) == 0) {
                                  seen_param = true;
                              }
                          });
    FGB_CHECK(!seen_face);
    FGB_CHECK(seen_param);
}
