// 通用 OSC 输入：映射表语法、目标解析、缩放/偏移/反相，以及真实 UDP 收包。
//
// 映射表是用户手写/界面生成的，语法错误必须在加载时就说清楚；这里把
// "缺目标 / 地址没斜杠 / scale 不是数字" 都钉成用例，避免用户看到"没数据"却
// 不知道哪一行错了。
#include "base/clock.hpp"
#include "config/osc_map.hpp"
#include "net/udp_socket.hpp"
#include "osc/encoder.hpp"
#include "sources/osc_source.hpp"
#include "sources/osc_target.hpp"

#include "test_framework.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace fgb;

namespace {

constexpr std::uint16_t kTestPort = 19103;

bool wait_for_frame(OscSource& source, FaceFrame& frame, int timeout_ms = 1000) {
    const std::uint64_t deadline =
        clock::now_ns() + static_cast<std::uint64_t>(timeout_ms) * 1'000'000ULL;
    while (clock::now_ns() < deadline) {
        if (source.poll(frame)) {
            return true;
        }
        clock::sleep_for_ns(2'000'000);
    }
    return false;
}

void send_float(net::UdpSocket& sender, std::string_view address, float value) {
    std::byte buffer[256];
    osc::BundleWriter writer(buffer);
    writer.write_floats(address, std::span<const float>{&value, 1});
    sender.send_to(writer.data(), "127.0.0.1", kTestPort);
}

} // namespace

FGB_TEST(osc_map_parse_variants) {
    config::OscMapping mapping;
    std::string error;

    FGB_CHECK(config::parse_osc_mapping("/avatar/parameters/JawOpen", "JawOpen", mapping, error));
    FGB_CHECK(mapping.target == "JawOpen");
    FGB_CHECK_NEAR(mapping.scale, 1.0, 1e-6);
    FGB_CHECK_NEAR(mapping.offset, 0.0, 1e-6);
    FGB_CHECK(!mapping.invert);

    FGB_CHECK(config::parse_osc_mapping("/x", "MouthSmileLeft, scale=2.5, offset=-0.25, invert",
                                        mapping, error));
    FGB_CHECK(mapping.target == "MouthSmileLeft");
    FGB_CHECK_NEAR(mapping.scale, 2.5, 1e-6);
    FGB_CHECK_NEAR(mapping.offset, -0.25, 1e-6);
    FGB_CHECK(mapping.invert);

    const std::string formatted = config::format_osc_mapping(mapping);
    config::OscMapping reparsed;
    FGB_CHECK(config::parse_osc_mapping("/x", formatted, reparsed, error));
    FGB_CHECK(reparsed.target == mapping.target);
    FGB_CHECK_NEAR(reparsed.scale, mapping.scale, 1e-6);
    FGB_CHECK_NEAR(reparsed.offset, mapping.offset, 1e-6);
    FGB_CHECK(reparsed.invert);
}

FGB_TEST(osc_map_rejects_bad_entries) {
    config::OscMapping mapping;
    std::string error;

    FGB_CHECK(!config::parse_osc_mapping("no-slash", "JawOpen", mapping, error));
    FGB_CHECK(!config::parse_osc_mapping("/x", "", mapping, error));
    FGB_CHECK(!config::parse_osc_mapping("/x", "JawOpen, scale=abc", mapping, error));
    FGB_CHECK(!config::parse_osc_mapping("/x", "JawOpen, wat=1", mapping, error));
    FGB_CHECK(!config::parse_osc_mapping("/x", "Bad Name", mapping, error));
    FGB_CHECK(!config::parse_osc_mapping("/x", "JawOpen, invert=maybe", mapping, error));
}

FGB_TEST(osc_map_save_load_roundtrip) {
    const auto path = std::filesystem::temp_directory_path() / "fgb-osc-map-test.ini";
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".bak");

    config::OscMapOptions saved;
    saved.port = 9123;
    saved.mappings.push_back({"/avatar/parameters/JawOpen", "JawOpen", 1.0f, 0.0f, false});
    saved.mappings.push_back({"/avatar/parameters/EyeLidLeft", "EyeLidLeft", 1.0f, 0.0f, true});
    saved.mappings.push_back({"/eye/x", "EyeLeftX", 0.8f, 0.1f, false});

    std::string error;
    FGB_CHECK(config::save_osc_map(path, saved, error));

    config::OscMapOptions loaded;
    FGB_CHECK(config::load_osc_map(path, loaded, error));
    FGB_CHECK(loaded.port == 9123);
    FGB_CHECK(loaded.mappings.size() == 3);
    if (loaded.mappings.size() == 3) {
        FGB_CHECK(loaded.mappings[0].address == "/avatar/parameters/JawOpen");
        FGB_CHECK(loaded.mappings[0].target == "JawOpen");
        FGB_CHECK(loaded.mappings[1].target == "EyeLidLeft");
        FGB_CHECK(loaded.mappings[1].invert);
        FGB_CHECK_NEAR(loaded.mappings[2].scale, 0.8, 1e-6);
        FGB_CHECK_NEAR(loaded.mappings[2].offset, 0.1, 1e-6);
    }

    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".bak");
}

FGB_TEST(osc_targets_parse) {
    OscTarget target;
    FGB_CHECK(parse_osc_target("JawOpen", target));
    FGB_CHECK(target.kind == OscTargetKind::Shape);
    FGB_CHECK(target.shape == Shape::JawOpen);

    FGB_CHECK(parse_osc_target("jawopen", target));
    FGB_CHECK(target.shape == Shape::JawOpen);

    FGB_CHECK(parse_osc_target("EyeLidLeft", target));
    FGB_CHECK(target.kind == OscTargetKind::EyeOpenLeft);
    FGB_CHECK(parse_osc_target("EyeLeftX", target));
    FGB_CHECK(target.kind == OscTargetKind::EyeGazeXLeft);
    FGB_CHECK(osc_target_is_signed(target.kind));

    FGB_CHECK(!parse_osc_target("NotAShape", target));
    FGB_CHECK(!parse_osc_target("", target));
}

FGB_TEST(osc_value_transform_and_invert) {
    config::OscMapping mapping;
    mapping.scale  = 2.0f;
    mapping.offset = -0.5f;

    FGB_CHECK_NEAR(apply_osc_value(0.5f, mapping, OscTargetKind::Shape), 0.5f, 1e-6);

    mapping.invert = true;
    FGB_CHECK_NEAR(apply_osc_value(0.5f, mapping, OscTargetKind::Shape), 0.5f, 1e-6);
    // 视线轴的反相是取负，不是 1-v
    FGB_CHECK_NEAR(apply_osc_value(0.5f, mapping, OscTargetKind::EyeGazeXLeft), -0.5f, 1e-6);
}

FGB_TEST(osc_source_receives_and_maps) {
    OscSourceConfig config;
    config.port = kTestPort;
    config.mappings = {
        {"/jaw", "JawOpen", 1.0f, 0.0f, false},
        {"/lid", "EyeLidLeft", 1.0f, 0.0f, false},
        {"/ex", "EyeLeftX", 1.0f, 0.0f, false},
        {"/ey", "EyeLeftY", 1.0f, 0.0f, false},
    };

    OscSource source{config};
    FGB_CHECK(source.start());

    net::UdpSocket sender;
    FGB_CHECK(sender.open_sender());

    FaceFrame frame;
    bool complete = false;
    const std::uint64_t deadline = clock::now_ns() + 2'000'000'000ULL;
    // 四个包只发一轮时，CI 上偶发 UDP 丢一个包就整条用例失败；重发到收齐或超时。
    while (clock::now_ns() < deadline) {
        send_float(sender, "/jaw", 0.75f);
        send_float(sender, "/lid", 0.25f);
        send_float(sender, "/ex", 0.5f);
        send_float(sender, "/ey", -0.5f);

        const std::uint64_t round_deadline = clock::now_ns() + 100'000'000ULL;
        while (clock::now_ns() < round_deadline) {
            if (wait_for_frame(source, frame, 20) && frame.shape_is_valid(Shape::JawOpen) &&
                frame.eyes.left.valid && frame.has_channel(channel::kExpression) &&
                frame.eyes.left.gazeConf > 0.0f) {
                complete = true;
                break;
            }
        }
        if (complete) {
            break;
        }
    }
    FGB_CHECK(complete);
    FGB_CHECK_NEAR(frame.shapes[index_of(Shape::JawOpen)], 0.75, 1e-5);
    FGB_CHECK_NEAR(frame.eyes.left.openness, 0.75, 1e-5); // EyeLid 1 = 闭眼
    FGB_CHECK_NEAR(frame.eyes.left.gazePitchYawDeg.y, 15.0, 1e-3);
    FGB_CHECK_NEAR(frame.eyes.left.gazePitchYawDeg.x, -15.0, 1e-3);
    FGB_CHECK(!frame.has_channel(channel::kHead));

    const SourceStats stats = source.stats();
    FGB_CHECK(stats.packets >= 4);
    FGB_CHECK(stats.frames >= 1);

    source.stop();
}
