#include "capture/capture_file.hpp"
#include "config/ini.hpp"
#include "core/process/one_euro.hpp"
#include "params/calibration.hpp"
#include "params/preview_tap.hpp"
#include "vrcft/update_data.hpp"

#include "test_framework.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

// ---------------------------------------------------------------------------
// 校准预览取样（ADR-0021 P1）
// ---------------------------------------------------------------------------

FGB_TEST(preview_tap_is_off_until_enabled) {
    PreviewTap tap;
    FaceFrame frame;
    frame.sequence                                  = 42;
    frame.shapes[index_of(Shape::JawOpen)]          = 0.42f;

    // 校准页没开时不该取样：引擎热路径每次 tick 都跑，白拷一份没有意义
    tap.process(frame);
    PreviewFrame snapshot;
    const bool read_ok = tap.latest(snapshot);
    FGB_CHECK(!read_ok || !snapshot.valid);

    tap.set_enabled(true);
    tap.process(frame);
    FGB_CHECK(tap.latest(snapshot));
    FGB_CHECK(snapshot.valid);
    FGB_CHECK(snapshot.sequence == 42);
    FGB_CHECK(std::fabs(snapshot.shapes[index_of(Shape::JawOpen)] - 0.42f) < 1e-6f);
}

FGB_TEST(preview_tap_sees_value_before_calibration) {
    // 校准把 0.20..0.60 拉到 0..1，取样器必须仍看到原始 0.40
    CalibrationProfile profile;
    auto& range                                = profile.shapes[index_of(Shape::JawOpen)];
    range.lo                                   = 0.2f;
    range.hi                                   = 0.6f;
    range.active                               = true;

    PreviewTap tap;
    tap.set_enabled(true);
    CalibrationApplier applier{profile};

    FaceFrame frame;
    frame.shapes[index_of(Shape::JawOpen)] = 0.40f;
    // 顺序与引擎一致：先取样，再校准
    tap.process(frame);
    applier.process(frame);

    PreviewFrame snapshot;
    FGB_CHECK(tap.latest(snapshot));
    FGB_CHECK(std::fabs(snapshot.shapes[index_of(Shape::JawOpen)] - 0.40f) < 1e-6f);
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 0.50f) < 1e-5f);
}

FGB_TEST(calibration_hot_swap_takes_effect_next_frame) {
    CalibrationApplier applier;
    FaceFrame frame;
    frame.shapes[index_of(Shape::JawOpen)] = 0.5f;
    applier.process(frame);
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 0.5f) < 1e-6f); // 默认透传

    // 界面拖滑杆：控制线程 set_profile，下一帧就生效（不重启引擎）
    CalibrationProfile profile;
    auto& range   = profile.shapes[index_of(Shape::JawOpen)];
    range.lo      = 0.5f;
    range.hi      = 1.0f;
    range.active  = true;
    applier.set_profile(profile);

    frame.shapes[index_of(Shape::JawOpen)] = 0.75f;
    applier.process(frame);
    FGB_CHECK(std::fabs(frame.shapes[index_of(Shape::JawOpen)] - 0.5f) < 1e-5f);
    FGB_CHECK(std::fabs(applier.profile().shapes[index_of(Shape::JawOpen)].hi - 1.0f) < 1e-6f);
}

FGB_TEST(calibration_profile_equality_ignores_inactive_numbers) {
    CalibrationProfile a;
    CalibrationProfile b;
    // 未激活的形状数值不参与比较：取消勾选后再改数值不算"有改动"
    a.shapes[index_of(Shape::JawOpen)].lo = 0.1f;
    FGB_CHECK(a == b);

    b.shapes[index_of(Shape::JawOpen)].active = true;
    FGB_CHECK(a != b);
}

FGB_TEST(calibration_atomic_save_keeps_previous_as_backup) {
    const std::string path = temp_path("atomic_calib.ini");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".bak", ec);

    CalibrationProfile first;
    first.shapes[index_of(Shape::JawOpen)].lo     = 0.10f;
    first.shapes[index_of(Shape::JawOpen)].hi     = 0.90f;
    first.shapes[index_of(Shape::JawOpen)].active = true;

    std::string error;
    FGB_CHECK(save_profile_atomic(path, first, error));
    FGB_CHECK(error.empty());
    FGB_CHECK(std::filesystem::exists(path));

    CalibrationProfile second = first;
    second.shapes[index_of(Shape::JawOpen)].hi = 0.70f;
    FGB_CHECK(save_profile_atomic(path, second, error));

    CalibrationProfile loaded;
    FGB_CHECK(load_profile(path, loaded));
    FGB_CHECK(std::fabs(loaded.shapes[index_of(Shape::JawOpen)].hi - 0.70f) < 1e-3f);

    // 改错了要能拿回上一版：.bak 是第二道保险（第一道是「重新加载」）
    CalibrationProfile backup;
    FGB_CHECK(load_profile(path + ".bak", backup));
    FGB_CHECK(std::fabs(backup.shapes[index_of(Shape::JawOpen)].hi - 0.90f) < 1e-3f);

    // 临时文件不能留在磁盘上
    FGB_CHECK(!std::filesystem::exists(path + ".tmp"));
}

// ---------------------------------------------------------------------------
// 映射曲线（ADR-0031）
// ---------------------------------------------------------------------------

FGB_TEST(curve_presets_hit_both_ends_and_stay_monotonic) {
    // 端点必须固定在 0 和 1：否则会出现"最高只到 0.8"这种没法解释的状态
    for (std::uint8_t id = 0; id < static_cast<std::uint8_t>(CurveKind::count); ++id) {
        FGB_CHECK_NEAR(apply_curve(nullptr, 0, id, 0.5f, 0.0f), 0.0f, 1e-5f);
        FGB_CHECK_NEAR(apply_curve(nullptr, 0, id, 0.5f, 1.0f), 1.0f, 1e-5f);

        // 单调不减：曲线一旦让表情反向，VRM 里的脸就会抽
        float prev = -1.0f;
        for (int i = 0; i <= 40; ++i) {
            const float t = static_cast<float>(i) / 40.0f;
            const float v = apply_curve(nullptr, 0, id, 0.5f, t);
            FGB_CHECK(v >= 0.0f);
            FGB_CHECK(v <= 1.0f);
            FGB_CHECK(v >= prev - 1e-5f);
            prev = v;
        }
    }
}

FGB_TEST(curve_ease_in_holds_back_small_motion) {
    // 缓入压住小动作、缓出放大轻动作：这是两条最容易用错的曲线，得钉住方向
    const auto ease_in  = static_cast<std::uint8_t>(CurveKind::ease_in);
    const auto ease_out = static_cast<std::uint8_t>(CurveKind::ease_out);
    const float linear  = apply_curve(nullptr, 0, 0, 0.5f, 0.3f);
    FGB_CHECK(apply_curve(nullptr, 0, ease_in, 0.5f, 0.3f) < linear);
    FGB_CHECK(apply_curve(nullptr, 0, ease_out, 0.5f, 0.3f) > linear);
}

FGB_TEST(curve_step_uses_a_narrow_band_not_a_hard_jump) {
    const auto step = static_cast<std::uint8_t>(CurveKind::step);
    FGB_CHECK_NEAR(apply_curve(nullptr, 0, step, 0.5f, 0.40f), 0.0f, 1e-5f);
    FGB_CHECK_NEAR(apply_curve(nullptr, 0, step, 0.5f, 0.60f), 1.0f, 1e-5f);
    const float mid = apply_curve(nullptr, 0, step, 0.5f, 0.5f);
    FGB_CHECK(mid > 0.0f);
    FGB_CHECK(mid < 1.0f);
}

FGB_TEST(curve_custom_is_smooth_monotone_and_passes_through_points) {
    CurveShape curve;
    curve.count    = 3;
    curve.points[0] = CurvePoint{0.0f, 0.0f};
    curve.points[1] = CurvePoint{0.5f, 0.9f};
    curve.points[2] = CurvePoint{1.0f, 1.0f};

    const auto id = static_cast<std::uint8_t>(kCurveCustomBase);

    // 穿过控制点：这是"拖到哪儿就是哪儿"的全部承诺
    FGB_CHECK_NEAR(apply_curve(&curve, 1, id, 0.5f, 0.5f), 0.9f, 2e-3f);
    FGB_CHECK_NEAR(apply_curve(&curve, 1, id, 0.5f, 0.0f), 0.0f, 1e-5f);
    FGB_CHECK_NEAR(apply_curve(&curve, 1, id, 0.5f, 1.0f), 1.0f, 1e-5f);

    // 单调、不过冲：面捕输出一旦局部反向，脸就会抽
    float prev = -1.0f;
    for (int i = 0; i <= 64; ++i) {
        const float v = apply_curve(&curve, 1, id, 0.5f, static_cast<float>(i) / 64.0f);
        FGB_CHECK(v >= prev - 1e-5f);
        FGB_CHECK(v >= -1e-5f);
        FGB_CHECK(v <= 1.0f + 1e-5f);
        prev = v;
    }

    // 控制点两侧的斜率必须连续——折线在这里会从 1.8 直接掉到 0.2，
    // 那正是"一段一段"的来源，也是这条曲线不做折线的原因
    const float h = 1e-3f;
    const float left  = (apply_curve(&curve, 1, id, 0.5f, 0.5f) -
                         apply_curve(&curve, 1, id, 0.5f, 0.5f - h)) / h;
    const float right = (apply_curve(&curve, 1, id, 0.5f, 0.5f + h) -
                         apply_curve(&curve, 1, id, 0.5f, 0.5f)) / h;
    FGB_CHECK(std::fabs(left - right) < 0.05f);

    // 指向不存在的槽：退回线性，而不是把这个形状吞成 0
    FGB_CHECK_NEAR(
        apply_curve(&curve, 1, static_cast<std::uint8_t>(kCurveCustomBase + 3), 0.5f, 0.4f), 0.4f,
        1e-5f);
}

FGB_TEST(curve_sanitize_anchors_ends_and_sorts_points) {
    CurveShape curve;
    curve.count    = 3;
    curve.points[0] = CurvePoint{0.8f, 0.2f}; // 乱序
    curve.points[1] = CurvePoint{0.1f, 0.7f};
    curve.points[2] = CurvePoint{0.4f, 1.6f}; // 越界

    sanitize_curve(curve);
    FGB_CHECK(curve.count >= 2);
    FGB_CHECK_NEAR(curve.points[0].x, 0.0f, 1e-5f);
    FGB_CHECK_NEAR(curve.points[0].y, 0.0f, 1e-5f);
    FGB_CHECK_NEAR(curve.points[curve.count - 1].x, 1.0f, 1e-5f);
    FGB_CHECK_NEAR(curve.points[curve.count - 1].y, 1.0f, 1e-5f);
    for (std::size_t i = 1; i < curve.count; ++i) {
        FGB_CHECK(curve.points[i].x > curve.points[i - 1].x);
        FGB_CHECK(curve.points[i].y >= 0.0f);
        FGB_CHECK(curve.points[i].y <= 1.0f);
    }
}

FGB_TEST(curve_profile_roundtrip_keeps_curves_and_custom_points) {
    const std::string path = temp_path("curve_calib.ini");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    CalibrationProfile profile;
    auto& jaw = profile.shapes[index_of(Shape::JawOpen)];
    jaw.lo     = 0.1f;
    jaw.hi     = 0.9f;
    jaw.active = true;
    jaw.curve  = static_cast<std::uint8_t>(CurveKind::smooth);

    auto& smile = profile.shapes[index_of(Shape::MouthCornerPullLeft)];
    smile.lo     = 0.05f;
    smile.hi     = 0.85f;
    smile.active = true;
    smile.curve  = static_cast<std::uint8_t>(kCurveCustomBase + 1);

    CurveShape custom;
    custom.count    = 3;
    custom.points[0] = CurvePoint{0.0f, 0.0f};
    custom.points[1] = CurvePoint{0.3f, 0.55f};
    custom.points[2] = CurvePoint{1.0f, 1.0f};
    profile.custom_curves[1] = custom;

    FGB_CHECK(save_profile(path, profile));

    CalibrationProfile loaded;
    FGB_CHECK(load_profile(path, loaded));
    FGB_CHECK(loaded.shapes[index_of(Shape::JawOpen)].curve ==
              static_cast<std::uint8_t>(CurveKind::smooth));
    FGB_CHECK(loaded.shapes[index_of(Shape::MouthCornerPullLeft)].curve ==
              static_cast<std::uint8_t>(kCurveCustomBase + 1));
    FGB_CHECK(loaded.custom_curves[1].count == 3);
    FGB_CHECK_NEAR(loaded.custom_curves[1].points[1].y, 0.55f, 1e-3f);

    // 曲线本身也要参与"有未保存改动"的判定：漏了它，拖完曲线点界面仍显示已保存
    CalibrationProfile touched = loaded;
    touched.custom_curves[1].points[1].y += 0.1f;
    FGB_CHECK(touched != loaded);
}

FGB_TEST(curve_legacy_profile_without_curve_column_loads_as_linear) {
    // 旧档案只有 lo hi 两段；新增的曲线字段缺省必须是线性，否则升级即改手感
    const std::string path = temp_path("legacy_calib.ini");
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        FGB_CHECK(file != nullptr);
        std::fputs("[shapes]\nJawOpen = 0.2000 0.8000\n", file);
        std::fclose(file);
    }

    CalibrationProfile loaded;
    FGB_CHECK(load_profile(path, loaded));
    const ShapeRange& jaw = loaded.shapes[index_of(Shape::JawOpen)];
    FGB_CHECK(jaw.active);
    FGB_CHECK_NEAR(jaw.lo, 0.2f, 1e-4f);
    FGB_CHECK_NEAR(jaw.hi, 0.8f, 1e-4f);
    FGB_CHECK(jaw.curve == static_cast<std::uint8_t>(CurveKind::linear));
}

