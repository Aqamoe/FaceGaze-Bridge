#include "capture/capture_file.hpp"
#include "config/ini.hpp"
#include "core/process/one_euro.hpp"
#include "params/calibration.hpp"
#include "vrcft/update_data.hpp"

#include "test_framework.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

using namespace fgb;
using namespace fgb::params;

namespace {

void put_f32(std::byte* p, float v) { std::memcpy(p, &v, sizeof(v)); }
void put_i32(std::byte* p, std::int32_t v) { std::memcpy(p, &v, sizeof(v)); }

std::string temp_path(const char* name) {
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path(ec) / "fgb-tests";
    std::filesystem::create_directories(dir, ec);
    return (dir / name).string();
}

// 一条只填了 JawOpen 的 ReplyUpdate body
std::array<std::byte, capture::kPayloadSize> body_with_jaw(float jaw) {
    std::array<std::byte, capture::kPayloadSize> body{};
    put_i32(body.data(), static_cast<std::int32_t>(vrcft::kUpdateStructSize));
    std::byte* fields = body.data() + 4;
    // 眼球四组：填充有效值，避免整帧被判为"无数据"
    put_f32(fields + 4 * 4, 3.0f);  // left_pupil_mm
    put_f32(fields + 4 * 5, 1.0f);  // left_openness
    put_f32(fields + 4 * 8, 3.0f);  // right_pupil_mm
    put_f32(fields + 4 * 9, 1.0f);  // right_openness
    put_f32(fields + 4 * 16 + 4 * index_of(Shape::JawOpen), jaw);
    return body;
}

void write_capture(const std::string& path, float jaw_value, int frames) {
    capture::Writer writer;
    writer.open(path);
    const auto body = body_with_jaw(jaw_value);
    for (int i = 0; i < frames; ++i) {
        writer.write(static_cast<std::uint64_t>(i) * 10'000'000ULL, body.data(), body.size());
    }
    writer.close();
}

} // namespace

// ---------------------------------------------------------------------------
// One Euro
// ---------------------------------------------------------------------------

FGB_TEST(one_euro_first_sample_passes_through) {
    // 首个样本必须直接采用，否则开机时所有表情会从 0 慢慢"长"出来
    OneEuroFilter filter;
    FGB_CHECK(std::fabs(filter.filter(0.8f) - 0.8f) < 1e-6f);
}

FGB_TEST(one_euro_smooths_a_step) {
    OneEuroFilter filter;
    filter.filter(0.0f);
    // 阶跃之后第一拍不该直接到 1.0
    const float first = filter.filter(1.0f);
    FGB_CHECK(first > 0.0f);
    FGB_CHECK(first < 1.0f);

    // 保持住之后要收敛到 1.0
    float last = first;
    for (int i = 0; i < 200; ++i) {
        last = filter.filter(1.0f);
    }
    FGB_CHECK(std::fabs(last - 1.0f) < 1e-3f);
}

FGB_TEST(one_euro_rejects_nan) {
    OneEuroFilter filter;
    FGB_CHECK(filter.filter(std::nanf("")) == 0.0f);
}

FGB_TEST(face_filter_applies_to_shapes_and_eyes) {
    FaceFilter filter;
    FaceFrame frame;
    frame.shapes[index_of(Shape::JawOpen)] = 1.0f;
    frame.eyes.left.openness               = 0.0f;

    filter.process(frame); // 首帧透传
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 1.0f) < 1e-6f);

    frame.shapes[index_of(Shape::JawOpen)] = 0.0f;
    frame.eyes.left.openness              = 1.0f;
    filter.process(frame);
    // 有平滑，就不会一步到位
    FGB_CHECK(frame.shapes[index_of(Shape::JawOpen)] > 0.0f);
    FGB_CHECK(frame.eyes.left.openness < 1.0f);
}

// ---------------------------------------------------------------------------
// 校准
// ---------------------------------------------------------------------------

FGB_TEST(calibration_applier_remaps_range) {
    CalibrationProfile profile;
    profile.shapes[index_of(Shape::JawOpen)].lo     = 0.2f;
    profile.shapes[index_of(Shape::JawOpen)].hi     = 0.6f;
    profile.shapes[index_of(Shape::JawOpen)].active = true;

    CalibrationApplier applier{profile};
    FaceFrame frame;

    frame.shapes[index_of(Shape::JawOpen)] = 0.2f;
    applier.process(frame);
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 0.0f) < 1e-5f);

    frame.shapes[index_of(Shape::JawOpen)] = 0.4f;
    applier.process(frame);
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 0.5f) < 1e-5f);

    frame.shapes[index_of(Shape::JawOpen)] = 0.9f; // 超出上限要夹住
    applier.process(frame);
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 1.0f) < 1e-5f);
}

FGB_TEST(calibration_leaves_uncalibrated_shapes_alone) {
    CalibrationApplier applier{CalibrationProfile{}};
    FaceFrame frame;
    frame.shapes[index_of(Shape::MouthCornerPullLeft)] = 0.37f;
    applier.process(frame);
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::MouthCornerPullLeft)] - 0.37f) < 1e-6f);
}

FGB_TEST(calibration_profile_roundtrip) {
    CalibrationProfile profile;
    profile.shapes[index_of(Shape::JawOpen)].lo     = 0.05f;
    profile.shapes[index_of(Shape::JawOpen)].hi     = 0.87f;
    profile.shapes[index_of(Shape::JawOpen)].active = true;
    profile.has_eye        = true;
    profile.eye_gain_y     = 1.3f;
    profile.eye_center_x   = 1.5f;

    const std::string path = temp_path("unit_calib.ini");
    FGB_CHECK(save_profile(path, profile));

    CalibrationProfile loaded;
    FGB_CHECK(load_profile(path, loaded));
    FGB_CHECK(loaded.active_shape_count() == 1);
    FGB_CHECK(std::fabs(loaded.shapes[index_of(Shape::JawOpen)].lo - 0.05f) < 1e-3f);
    FGB_CHECK(std::fabs(loaded.shapes[index_of(Shape::JawOpen)].hi - 0.87f) < 1e-3f);
    FGB_CHECK(loaded.has_eye);
    FGB_CHECK(std::fabs(loaded.eye_gain_y - 1.3f) < 1e-3f);
    FGB_CHECK(std::fabs(loaded.eye_center_x - 1.5f) < 1e-3f);
}

FGB_TEST(calibration_derive_from_two_captures) {
    const std::string neutral = temp_path("unit_neutral.fgbrec");
    const std::string express = temp_path("unit_express.fgbrec");
    write_capture(neutral, 0.10f, 20); // 放松时 JawOpen 稳定在 0.1
    write_capture(express, 0.90f, 20); // 夸张时到 0.9

    CalibrationProfile profile;
    std::string error;
    FGB_CHECK(derive_profile(neutral, express, profile, error));
    FGB_CHECK(error.empty());

    const ShapeRange& jaw = profile.shapes[index_of(Shape::JawOpen)];
    FGB_CHECK(jaw.active);
    FGB_CHECK(std::fabs(jaw.lo - 0.10f) < 1e-3f);
    FGB_CHECK(std::fabs(jaw.hi - 0.90f) < 1e-3f);
}

FGB_TEST(calibration_derive_skips_static_shapes) {
    // 两次录制里 MouthSmileLeft 都是 0，跨度不足，不该参与映射
    const std::string neutral = temp_path("unit_flat_a.fgbrec");
    const std::string express = temp_path("unit_flat_b.fgbrec");
    write_capture(neutral, 0.5f, 10);
    write_capture(express, 0.5f, 10);

    CalibrationProfile profile;
    std::string error;
    FGB_CHECK(derive_profile(neutral, express, profile, error));
    // JawOpen 两次一样 → 全部不激活
    FGB_CHECK(profile.active_shape_count() == 0);
}

FGB_TEST(calibration_derive_reports_missing_file) {
    CalibrationProfile profile;
    std::string error;
    FGB_CHECK(!derive_profile(temp_path("nope.fgbrec"), temp_path("nope2.fgbrec"), profile, error));
    FGB_CHECK(!error.empty());
}
