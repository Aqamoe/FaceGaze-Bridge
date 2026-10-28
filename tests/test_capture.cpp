#include "capture/capture_file.hpp"
#include "sources/replay_source.hpp"
#include "vrcft/update_data.hpp"

#include "test_framework.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace fgb;
using namespace fgb::vrcft;

namespace {

void put_f32(std::byte* p, float v) { std::memcpy(p, &v, sizeof(v)); }
void put_i32(std::byte* p, std::int32_t v) { std::memcpy(p, &v, sizeof(v)); }

// 构造一条 ReplyUpdate body：structSize + 16 个眼/头 float + 89 个形状 float
std::array<std::byte, capture::kPayloadSize> make_body(float jaw_open, float left_openness) {
    std::array<std::byte, capture::kPayloadSize> body{};
    put_i32(body.data(), static_cast<std::int32_t>(kUpdateStructSize));

    std::byte* fields = body.data() + 4;
    // 布局见 update_data.hpp：4 个 dilation + 左眼 4 项 + 右眼 4 项 + 头 4 项
    put_f32(fields + 4 * 2, 0.0f);            // left_gaze_x
    put_f32(fields + 4 * 3, 0.0f);            // left_gaze_y
    put_f32(fields + 4 * 4, 3.0f);            // left_pupil_mm
    put_f32(fields + 4 * 5, left_openness);   // left_openness
    put_f32(fields + 4 * 6, 0.0f);            // right_gaze_x
    put_f32(fields + 4 * 7, 0.0f);            // right_gaze_y
    put_f32(fields + 4 * 8, 3.0f);            // right_pupil_mm
    put_f32(fields + 4 * 9, left_openness);   // right_openness

    // JawOpen 在形状数组里的下标
    const std::size_t jaw_idx = 4 * 16 + 4 * index_of(Shape::JawOpen);
    put_f32(fields + jaw_idx, jaw_open);
    return body;
}

std::string temp_path(const char* name) {
    std::string path = ".workbuddy/tmp/";
    path += name;
    return path;
}

} // namespace

FGB_TEST(capture_roundtrip) {
    const std::string path = temp_path("unit_capture.fgbrec");
    const auto body        = make_body(0.5f, 0.75f);

    {
        capture::Writer writer;
        FGB_CHECK(writer.open(path));
        FGB_CHECK(writer.write(0, body.data(), body.size()));
        FGB_CHECK(writer.write(10'000'000ULL, body.data(), body.size()));
        FGB_CHECK(writer.frames() == 2);
    }

    capture::Reader reader;
    FGB_CHECK(reader.open(path));
    FGB_CHECK(reader.estimated_frames() == 2);

    FGB_CHECK(reader.next());
    FGB_CHECK(reader.timestamp_ns() == 0);
    FGB_CHECK(reader.payload_size() == capture::kPayloadSize);
    // 原样字节：连 structSize 都要一模一样
    FGB_CHECK(std::memcmp(reader.payload(), body.data(), body.size()) == 0);

    FGB_CHECK(reader.next());
    FGB_CHECK(reader.timestamp_ns() == 10'000'000ULL);

    FGB_CHECK(!reader.next()); // 正常结束，不算错误
}

FGB_TEST(capture_rejects_foreign_file) {
    const std::string path = temp_path("unit_bad.fgbrec");
    std::FILE* f           = std::fopen(path.c_str(), "wb");
    FGB_CHECK(f != nullptr);
    std::fputs("not a capture file at all", f);
    std::fclose(f);

    capture::Reader reader;
    FGB_CHECK(!reader.open(path));
    FGB_CHECK(!reader.error().empty());
}

FGB_TEST(capture_reads_payload_as_update_data) {
    const std::string path = temp_path("unit_decode.fgbrec");
    const auto body        = make_body(0.6f, 0.4f);
    {
        capture::Writer writer;
        FGB_CHECK(writer.open(path));
        FGB_CHECK(writer.write(0, body.data(), body.size()));
    }

    capture::Reader reader;
    FGB_CHECK(reader.open(path));
    FGB_CHECK(reader.next());

    UpdateData data{};
    FGB_CHECK(decode_update(
        std::span<const std::byte>{reader.payload(), reader.payload_size()}, data));
    FGB_CHECK(std::fabs(data.shapes[index_of(Shape::JawOpen)] - 0.6f) < 1e-6f);
    FGB_CHECK(std::fabs(data.left_openness - 0.4f) < 1e-6f);
    FGB_CHECK(data.has_any_valid());
}

FGB_TEST(replay_source_feeds_frames) {
    const std::string path = temp_path("unit_replay.fgbrec");
    const auto body        = make_body(0.7f, 0.5f);
    {
        capture::Writer writer;
        FGB_CHECK(writer.open(path));
        for (int i = 0; i < 3; ++i) {
            FGB_CHECK(writer.write(static_cast<std::uint64_t>(i) * 10'000'000ULL, body.data(),
                                   body.size()));
        }
    }

    ReplayConfig cfg;
    cfg.path  = path;
    cfg.speed = 0.0f; // 不限速：一 tick 把到期的全喂完

    ReplaySource source{cfg};
    FGB_CHECK(source.start());

    FaceFrame frame;
    // 第一次 poll 就该拿到帧（首帧时间戳为 0，立即到期）
    FGB_CHECK(source.poll(frame));
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 0.7f) < 1e-6f);
    FGB_CHECK(std::fabs(frame.eyes.left.openness - 0.5f) < 1e-6f);
    FGB_CHECK(source.stats().frames >= 1);

    source.stop();
}

FGB_TEST(replay_source_reports_missing_file) {
    ReplayConfig cfg;
    cfg.path = temp_path("does_not_exist.fgbrec");
    ReplaySource source{cfg};
    FGB_CHECK(!source.start());

    FaceFrame frame;
    FGB_CHECK(!source.poll(frame));
}
