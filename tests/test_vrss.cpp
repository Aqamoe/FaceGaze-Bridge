// VRSS 注视输入：只验证换算与"该不该发"，不碰真实 UDP/NVIDIA。
#include "sinks/vrss_gaze_sink.hpp"
#include "vrss/geometry.hpp"
#include "vrss/identity.hpp"

#include "test_framework.hpp"

#include <cstdint>

using namespace fgb;

namespace {

constexpr std::uint64_t kNow = 1'000'000'000;

const char* kGeometryText =
    "format=1\n"
    "vendor=4660\n"
    "product=22136\n"
    "identitySource=openvr-edid\n"
    "projection0=-0.5,0.5,-0.5,0.5\n"
    "eyeToHead0=1,0,0,0,1,0,0,0,1\n"
    "projection1=-0.5,0.5,-0.5,0.5\n"
    "eyeToHead1=1,0,0,0,1,0,0,0,1\n";

VrssSinkConfig point_config() {
    VrssSinkConfig config;
    config.rate_hz = 90;
    return config;
}

} // namespace

FGB_TEST(vrss_point_to_ndc_corners) {
    float x = 0.0f, y = 0.0f;
    VrssGazeSink::point_to_ndc(0.0f, 0.0f, x, y);
    FGB_CHECK_NEAR(x, -1.0, 1e-6);
    FGB_CHECK_NEAR(y, 1.0, 1e-6);
    VrssGazeSink::point_to_ndc(0.5f, 0.5f, x, y);
    FGB_CHECK_NEAR(x, 0.0, 1e-6);
    FGB_CHECK_NEAR(y, 0.0, 1e-6);
    VrssGazeSink::point_to_ndc(1.0f, 1.0f, x, y);
    FGB_CHECK_NEAR(x, 1.0, 1e-6);
    FGB_CHECK_NEAR(y, -1.0, 1e-6);
}

// 持久身份：写入格式必须与 runtime.cpp 的注入一致（0x 前缀大写十六进制），
// 读取要同时接受手工写进去的十进制，否则"用户手动设的值"会被误判为无效。
FGB_TEST(vrss_identity_env_text_roundtrip) {
    FGB_CHECK(vrss::identity_env_text(0xFEEE) == "0xFEEE");
    FGB_CHECK(vrss::identity_env_text(0x0001) == "0x1");
    std::uint32_t value = 0;
    FGB_CHECK(vrss::parse_identity_env_text("0xFEEE", value));
    FGB_CHECK(value == 0xFEEE);
    FGB_CHECK(vrss::parse_identity_env_text(" 65262 ", value));
    FGB_CHECK(value == 65262);
    FGB_CHECK(vrss::parse_identity_env_text("0X1", value));
    FGB_CHECK(value == 1);
    FGB_CHECK(!vrss::parse_identity_env_text("", value));
    FGB_CHECK(!vrss::parse_identity_env_text("0x10000", value));
    FGB_CHECK(!vrss::parse_identity_env_text("abc", value));
}

// 有真 EDID 的头显不能覆盖身份：把 0xFEEE 写进去反而会让游戏和 provider 都对不上。
FGB_TEST(vrss_identity_override_only_without_edid) {
    FGB_CHECK(!vrss::needs_identity_override("openvr-edid"));
    FGB_CHECK(vrss::needs_identity_override("candidate-auto"));
    FGB_CHECK(vrss::needs_identity_override("candidate-manual"));
    FGB_CHECK(vrss::needs_identity_override("tuner-session"));
    // 身份来源不明时不动环境变量，交给流程报错。
    FGB_CHECK(!vrss::needs_identity_override(""));
}

FGB_TEST(vrss_geometry_parses_and_rejects_bad_projection) {
    vrss::HmdGeometry geometry;
    std::string error;
    FGB_CHECK(vrss::parse_geometry(kGeometryText, geometry, error));
    FGB_CHECK(geometry.valid());
    FGB_CHECK(geometry.vendor == 4660);
    FGB_CHECK(geometry.product == 22136);

    const std::string broken =
        "vendor=1\nproduct=2\nprojection0=0.5,0.5,-0.5,0.5\n"
        "eyeToHead0=1,0,0,0,1,0,0,0,1\n"
        "projection1=-0.5,0.5,-0.5,0.5\neyeToHead1=1,0,0,0,1,0,0,0,1\n";
    FGB_CHECK(!vrss::parse_geometry(broken, geometry, error));
}

FGB_TEST(vrss_build_ndc_uses_viewport_points) {
    GazeState gaze;
    gaze.point_viewport         = true;
    gaze.capture_ts_ns          = kNow;
    gaze.left_point_valid       = true;
    gaze.right_point_valid      = true;
    gaze.left_point_2d          = {0.25f, 0.5f};
    gaze.right_point_2d         = {0.75f, 0.5f};

    VrssSinkConfig config = point_config();
    std::array<float, 4> ndc{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK_NEAR(ndc[0], -0.5, 1e-6);
    FGB_CHECK_NEAR(ndc[1], 0.0, 1e-6);
    FGB_CHECK_NEAR(ndc[2], 0.5, 1e-6);
    FGB_CHECK_NEAR(ndc[3], 0.0, 1e-6);
}

FGB_TEST(vrss_build_ndc_mirrors_single_eye) {
    GazeState gaze;
    gaze.point_viewport    = true;
    gaze.capture_ts_ns     = kNow;
    gaze.left_point_valid  = true;
    gaze.left_point_2d     = {0.2f, 0.4f};

    VrssSinkConfig config = point_config();
    std::array<float, 4> ndc{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK_NEAR(ndc[0], -0.6, 1e-6);
    FGB_CHECK_NEAR(ndc[2], -0.6, 1e-6); // provider 只收 validMask=3
    FGB_CHECK_NEAR(ndc[3], ndc[1], 1e-6);
}

FGB_TEST(vrss_build_ndc_rejects_stale_and_empty) {
    VrssSinkConfig config = point_config();
    std::array<float, 4> ndc{};

    GazeState empty;
    FGB_CHECK(!VrssGazeSink::build_ndc(empty, kNow, config, ndc));

    GazeState stale;
    stale.point_viewport   = true;
    stale.capture_ts_ns    = kNow - 100'000'000;
    stale.left_point_valid = true;
    FGB_CHECK(!VrssGazeSink::build_ndc(stale, kNow, config, ndc));
}

FGB_TEST(vrss_direction_to_ndc_sign_and_geometry) {
    vrss::HmdGeometry geometry;
    std::string error;
    FGB_CHECK(vrss::parse_geometry(kGeometryText, geometry, error));

    float x = 0.0f, y = 0.0f;
    FGB_CHECK(VrssGazeSink::direction_to_ndc({0.0f, 0.0f, -1.0f}, geometry.eyes[0], x, y));
    FGB_CHECK_NEAR(x, 0.0, 1e-6);
    FGB_CHECK_NEAR(y, 0.0, 1e-6);

    // +Y 是上；NVIDIA 的 normalized location 也是 +Y 向上，所以向上看得到正的 NDC y。
    FGB_CHECK(VrssGazeSink::direction_to_ndc({0.0f, 0.5f, -0.866f}, geometry.eyes[0], x, y));
    FGB_CHECK(y > 0.0f);

    VrssSinkConfig config = point_config();
    config.geometry       = geometry;
    GazeState gaze;
    gaze.capture_ts_ns = kNow;
    gaze.left_valid    = true;
    gaze.right_valid   = true;
    gaze.left_dir_local  = {0.0f, 0.5f, -0.866f};
    gaze.right_dir_local = {0.0f, 0.5f, -0.866f};
    std::array<float, 4> ndc{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK(ndc[1] > 0.0f); // 内部约定向上为正
    // 发给驱动前统一翻转一次（驱动 +Y 向下），默认不需要用户开任何开关
    FGB_CHECK(VrssGazeSink::driver_y(ndc[1], false) < 0.0f);
}

FGB_TEST(vrss_direction_requires_geometry) {
    VrssSinkConfig config = point_config();
    GazeState gaze;
    gaze.capture_ts_ns  = kNow;
    gaze.left_valid     = true;
    gaze.right_valid    = true;
    gaze.left_dir_local = {0.0f, 0.0f, -1.0f};
    gaze.right_dir_local = {0.0f, 0.0f, -1.0f};
    std::array<float, 4> ndc{};
    FGB_CHECK(!VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
}

// 只给"来源空间"的点（PICO Connect Gaze 这种逐眼 raw 点）：两眼 x 能差 0.3，
// 当屏幕位置用会让高质量区整体偏掉，所以 VRSS 必须拒收并说清原因。
FGB_TEST(vrss_rejects_source_space_points_with_reason) {
    GazeState gaze;
    gaze.capture_ts_ns    = kNow;
    gaze.left_point_valid = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d    = {0.73f, 0.48f};
    gaze.right_point_2d   = {0.43f, 0.48f};
    gaze.point_viewport   = false; // 来源空间

    VrssSinkConfig config = point_config();
    std::array<float, 4> ndc{};
    FGB_CHECK(!VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK(VrssGazeSink::diagnose(gaze, config, kNow) == VrssNoData::SourceSpace);
}

FGB_TEST(vrss_diagnose_explains_each_refusal) {
    VrssSinkConfig config = point_config();
    std::array<float, 4> ndc{};

    GazeState empty;
    FGB_CHECK(VrssGazeSink::diagnose(empty, config, kNow) == VrssNoData::NoGaze);

    // 视口点但过期
    GazeState stale;
    stale.point_viewport   = true;
    stale.capture_ts_ns    = kNow - 100'000'000;
    stale.left_point_valid = true;
    {
        GazeState with_ts = stale;
        with_ts.left_capture_ts_ns = kNow - 100'000'000;
        FGB_CHECK(VrssGazeSink::diagnose(with_ts, config, kNow) == VrssNoData::StaleGaze);
    }

    // 方向型但有几何
    GazeState dir;
    dir.capture_ts_ns    = kNow;
    dir.left_valid       = true;
    dir.right_valid      = true;
    dir.left_dir_local   = {0.0f, 0.0f, -1.0f};
    dir.right_dir_local  = {0.0f, 0.0f, -1.0f};
    FGB_CHECK(VrssGazeSink::diagnose(dir, config, kNow) == VrssNoData::NoGeometry);

    vrss::HmdGeometry geometry;
    std::string error;
    FGB_CHECK(vrss::parse_geometry(kGeometryText, geometry, error));
    config.geometry = geometry;
    FGB_CHECK(VrssGazeSink::diagnose(dir, config, kNow) == VrssNoData::NoGaze); // 几何有了就能发
    FGB_CHECK(VrssGazeSink::build_ndc(dir, kNow, config, ndc));
}

FGB_TEST(vrss_no_data_codes_are_stable) {
    FGB_CHECK(std::string(vrss_no_data_code(VrssNoData::SourceSpace)) == "source-space");
    FGB_CHECK(std::string(vrss_no_data_code(VrssNoData::None)) == "ok");
    FGB_CHECK(std::string(vrss_no_data_message(VrssNoData::SourceSpace)) != "");
}

// 打开"允许原始点"后：按两眼中点算一个位置给两眼（视差被折掉，两眼不会互相推开）。
FGB_TEST(vrss_source_points_use_midpoint_when_allowed) {
    GazeState gaze;
    gaze.capture_ts_ns     = kNow;
    gaze.point_viewport    = false;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.73f, 0.48f};
    gaze.right_point_2d    = {0.43f, 0.48f};

    VrssSinkConfig config = point_config();
    config.allow_source_points = true;
    std::array<float, 4> ndc{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK_NEAR(ndc[0], 2.0 * 0.58 - 1.0, 1e-5); // 中点 0.58
    // 来源空间 y 原点在下方（+y 向上），所以是 2y-1，不是视口的 1-2y。
    FGB_CHECK_NEAR(ndc[1], 2.0 * 0.48 - 1.0, 1e-5);
    FGB_CHECK_NEAR(ndc[2], ndc[0], 1e-6); // 两眼同一个位置
    FGB_CHECK_NEAR(ndc[3], ndc[1], 1e-6);

    // 关掉开关就必须继续拒收：默认行为不能因为这次改动变松。
    config.allow_source_points = false;
    FGB_CHECK(!VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
}

// 增益与中心偏移：ndc = point * gain + offset，两眼共用同一个偏移。
FGB_TEST(vrss_gain_and_center_offset) {
    GazeState gaze;
    gaze.point_viewport    = true;
    gaze.capture_ts_ns     = kNow;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.25f, 0.75f};
    gaze.right_point_2d    = {0.75f, 0.25f};

    VrssSinkConfig config = point_config();
    config.x_gain   = 1.5f;
    config.y_gain   = 0.5f;
    config.x_offset = 0.1f;
    config.y_offset = -0.2f;

    std::array<float, 4> ndc{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    // 左眼点 x=0.25 → ndc -0.5；*1.5 + 0.1 = -0.65
    FGB_CHECK_NEAR(ndc[0], -0.65, 1e-5);
    // y=0.75 → ndc -0.5；*0.5 - 0.2 = -0.45
    FGB_CHECK_NEAR(ndc[1], -0.45, 1e-5);
    // 右眼 x=0.75 → ndc 0.5；*1.5 + 0.1 = 0.85
    FGB_CHECK_NEAR(ndc[2], 0.85, 1e-5);
    // 右眼 y=0.25 → ndc +0.5；*0.5 - 0.2 = 0.05
    FGB_CHECK_NEAR(ndc[3], 0.05, 1e-5);

    // 驱动侧的 Y 翻转是最后一步、只改符号：内部 NDC 与偏移方向都不受影响。
    FGB_CHECK_NEAR(VrssGazeSink::driver_y(0.5f, false), -0.5f, 1e-6);
    FGB_CHECK_NEAR(VrssGazeSink::driver_y(0.5f, true), 0.5f, 1e-6);
    FGB_CHECK_NEAR(VrssGazeSink::driver_y(0.0f, false), 0.0f, 1e-6);
}

// 双眼补偿：一正一负的横移。正值内收（左眼右移、右眼左移），用来抵消驱动侧视差。
FGB_TEST(vrss_stereo_offset_pulls_eyes_together) {
    GazeState gaze;
    gaze.point_viewport    = true;
    gaze.capture_ts_ns     = kNow;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.25f, 0.5f};
    gaze.right_point_2d    = {0.75f, 0.5f};

    VrssSinkConfig config = point_config();
    std::array<float, 4> ndc{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK_NEAR(ndc[0], -0.5, 1e-6);
    FGB_CHECK_NEAR(ndc[2], 0.5, 1e-6);

    config.stereo_offset = 0.1f; // 内收
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK_NEAR(ndc[0], -0.4, 1e-6);
    FGB_CHECK_NEAR(ndc[2], 0.4, 1e-6);
    // y 不受影响
    FGB_CHECK_NEAR(ndc[1], 0.0, 1e-6);
    FGB_CHECK_NEAR(ndc[3], 0.0, 1e-6);

    config.stereo_offset = -0.2f; // 外扩
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    FGB_CHECK_NEAR(ndc[0], -0.7, 1e-6);
    FGB_CHECK_NEAR(ndc[2], 0.7, 1e-6);
}

// 标定后的位置照样套 gain/offset——它们是**显示侧微调**（补驱动摆放区域的偏差），
// 与注视校准不是一回事。双眼内收同理，两者必须同时可用。
FGB_TEST(vrss_display_trim_applies_after_calibration) {
    GazeState gaze;
    gaze.point_viewport    = true;
    gaze.space             = GazeSpace::calibrated;
    gaze.capture_ts_ns     = kNow;
    gaze.left_point_valid  = true;
    gaze.right_point_valid = true;
    gaze.left_point_2d     = {0.25f, 0.5f};
    gaze.right_point_2d    = {0.75f, 0.5f};

    VrssSinkConfig config = point_config();
    config.x_gain         = 2.0f;
    config.y_gain         = 1.8f;
    config.x_offset       = 0.1f;
    config.y_offset       = 0.2f;
    config.stereo_offset  = 0.05f;

    std::array<float, 4> ndc{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc));
    // 左眼点 -0.5 → *2.0 + 0.1 + 内收 0.05
    FGB_CHECK_NEAR(ndc[0], -0.5f * 2.0f + 0.1f + 0.05f, 1e-5);
    FGB_CHECK_NEAR(ndc[1], 0.2f, 1e-5);
    FGB_CHECK_NEAR(ndc[2], 1.0f, 1e-5); // 0.5*2+0.1-0.05=1.05 被夹到 1

    // 未标定时同一套参数，结果一致：显示侧微调与注视空间无关
    gaze.space = GazeSpace::viewport;
    std::array<float, 4> ndc2{};
    FGB_CHECK(VrssGazeSink::build_ndc(gaze, kNow, config, ndc2));
    FGB_CHECK_NEAR(ndc2[0], ndc[0], 1e-6);
    FGB_CHECK_NEAR(ndc2[1], ndc[1], 1e-6);
}
