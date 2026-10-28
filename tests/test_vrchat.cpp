#include "sinks/vrchat_sink.hpp"

#include "test_framework.hpp"

using namespace fgb;

namespace {

EyePair make_eyes(float pitch_deg, float yaw_deg, float openness) {
    EyePair eyes;
    eyes.left.valid  = true;
    eyes.right.valid = true;

    eyes.left.gazePitchYawDeg  = {pitch_deg, yaw_deg};
    eyes.right.gazePitchYawDeg = {pitch_deg, yaw_deg};
    eyes.left.openness         = openness;
    eyes.right.openness        = openness;
    return eyes;
}

} // namespace

FGB_TEST(eye_params_axis_signs) {
    float out[kEyeParamCount] = {};

    // 向上看 20 度：Y 轴为正（v2 约定 1.0 = 看向上）
    VrchatSink::compute_eye_params(make_eyes(20.0f, 0.0f, 1.0f), 30.0f, out);
    FGB_CHECK_NEAR(out[1], 20.0f / 30.0f, 1e-4); // EyeLeftY
    FGB_CHECK_NEAR(out[3], 20.0f / 30.0f, 1e-4); // EyeRightY
    FGB_CHECK_NEAR(out[0], 0.0f, 1e-4);
    FGB_CHECK_NEAR(out[2], 0.0f, 1e-4);

    // 向右看 30 度：X 轴为正（v2 约定 1.0 = 看向右）
    VrchatSink::compute_eye_params(make_eyes(0.0f, 30.0f, 1.0f), 30.0f, out);
    FGB_CHECK_NEAR(out[0], 1.0f, 1e-4);
    FGB_CHECK_NEAR(out[2], 1.0f, 1e-4);

    // 向左看：X 为负
    VrchatSink::compute_eye_params(make_eyes(0.0f, -15.0f, 1.0f), 30.0f, out);
    FGB_CHECK_NEAR(out[0], -0.5f, 1e-4);
}

FGB_TEST(eye_params_lid_convention) {
    float out[kEyeParamCount] = {};

    // v2 的 EyeLid：1.0 = 闭眼，0.0 = 完全睁开
    VrchatSink::compute_eye_params(make_eyes(0.0f, 0.0f, 1.0f), 30.0f, out);
    FGB_CHECK_NEAR(out[4], 0.0f, 1e-6);
    FGB_CHECK_NEAR(out[5], 0.0f, 1e-6);

    VrchatSink::compute_eye_params(make_eyes(0.0f, 0.0f, 0.0f), 30.0f, out);
    FGB_CHECK_NEAR(out[4], 1.0f, 1e-6);
    FGB_CHECK_NEAR(out[5], 1.0f, 1e-6);

    VrchatSink::compute_eye_params(make_eyes(0.0f, 0.0f, 0.5f), 30.0f, out);
    FGB_CHECK_NEAR(out[4], 0.5f, 1e-6);
}

FGB_TEST(eye_params_clamped_to_unit_range) {
    float out[kEyeParamCount] = {};

    // 超出满量程必须钳制，不能让 avatar 的眼睛翻到脑后面
    VrchatSink::compute_eye_params(make_eyes(90.0f, 90.0f, 1.0f), 30.0f, out);
    FGB_CHECK_NEAR(out[0], 1.0f, 1e-6);
    FGB_CHECK_NEAR(out[1], 1.0f, 1e-6);

    VrchatSink::compute_eye_params(make_eyes(-90.0f, -90.0f, 1.0f), 30.0f, out);
    FGB_CHECK_NEAR(out[0], -1.0f, 1e-6);
    FGB_CHECK_NEAR(out[1], -1.0f, 1e-6);
}

FGB_TEST(eye_params_handle_zero_scale_gracefully) {
    float out[kEyeParamCount] = {};
    // 配置成 0 不应该除零，应回退到 30 度满量程
    VrchatSink::compute_eye_params(make_eyes(15.0f, 15.0f, 1.0f), 0.0f, out);
    FGB_CHECK_NEAR(out[0], 0.5f, 1e-4);
    FGB_CHECK_NEAR(out[1], 0.5f, 1e-4);
}

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
