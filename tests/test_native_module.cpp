// 原生 C ABI 模块：加载示例 DLL、读帧、模块仓识别。
#include "engine/engine.hpp"
#include "sources/native_gaze_parse.hpp"
#include "sources/native_source.hpp"
#include "vrcft/module_registry.hpp"

#include "test_framework.hpp"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace fgb;

namespace {

fs::path exe_dir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return length > 0 ? fs::path{buffer}.parent_path() : fs::current_path();
}

fs::path sample_dll() { return exe_dir() / "fgb-sample-native.dll"; }
fs::path sample_manifest() {
    return fs::path{FGB_SOURCE_DIR} / "samples" / "native-sine" / "fgb-native.json";
}
fs::path sample_face_dll() { return exe_dir() / "fgb-sample-face.dll"; }
fs::path sample_gaze_dll() { return exe_dir() / "fgb-sample-gaze.dll"; }
fs::path sample_openness_dll() { return exe_dir() / "fgb-sample-openness.dll"; }
fs::path sample_openness_manifest() {
    return fs::path{FGB_SOURCE_DIR} / "samples" / "native-openness" / "fgb-native.json";
}
fs::path sample_face_manifest() {
    return fs::path{FGB_SOURCE_DIR} / "samples" / "native-face" / "fgb-native.json";
}
fs::path sample_gaze_manifest() {
    return fs::path{FGB_SOURCE_DIR} / "samples" / "native-gaze" / "fgb-native.json";
}

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("fgb-native-" + std::to_string(++counter) + "-" +
                std::to_string(static_cast<long long>(::rand())));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void copy_into(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

} // namespace

FGB_TEST(native_sample_module_loads_and_emits_frames) {
    if (!fs::exists(sample_dll())) {
        return; // 示例模块未构建（FGB_BUILD_SAMPLE_MODULE=OFF）
    }

    NativeSourceConfig config;
    config.dll          = sample_dll();
    config.module_id    = "com.fgbbridge.sample.sine";
    config.module_name  = "Sine Sample Module";
    config.display_name = "native";
    config.tick_hz      = 100;

    NativeModuleSource source{config};
    FGB_CHECK(source.start());

    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i) {
        got = source.poll(frame);
    }
    FGB_CHECK(got);
    if (got) {
        FGB_CHECK(frame.source != kInvalidSource);
        FGB_CHECK(frame.has_channel(channel::kExpression));
        FGB_CHECK(frame.has_channel(channel::kEyes));
        FGB_CHECK(frame.has_channel(channel::kHead));
        // 头像眼睛方向来自 EYES 属性，不等于独立注视通道
        FGB_CHECK(!frame.has_channel(channel::kGaze));
        FGB_CHECK(frame.eyes.left.valid);
        FGB_CHECK(frame.eyes.left.gazeValid);
        FGB_CHECK(frame.eyes.left.openness > 0.05f && frame.eyes.left.openness < 0.95f);
        FGB_CHECK(frame.shape_is_valid(Shape::JawOpen));
        FGB_CHECK(frame.shape(Shape::JawOpen) > 0.05f);
        FGB_CHECK(frame.hasHeadPose);
        // 属性帧带逐通道时间戳：v2 的通道新鲜度不退化成整帧时间
        FGB_CHECK(frame.channelStamps[channel::index(channel::kEyes)].captureTsNs != 0);
        FGB_CHECK(frame.channelStamps[channel::index(channel::kExpression)].captureTsNs != 0);
        FGB_CHECK(frame.channelStamps[channel::index(channel::kHead)].captureTsNs != 0);
    }

    const auto status = source.module_status();
    FGB_CHECK(status.has_value());
    if (status.has_value()) {
        FGB_CHECK(status->alive);
        FGB_CHECK(status->frames > 0);
        FGB_CHECK(status->error.empty());
    }

    source.stop();
    FGB_CHECK(!source.poll(frame));
}

FGB_TEST(native_module_is_recognized_from_store_manifest) {
    TempDir root;
    const fs::path module_dir = root.path / "store" / "com.fgbbridge.sample.sine";
    copy_into(sample_manifest(), module_dir / "fgb-native.json");
    if (fs::exists(sample_dll())) {
        copy_into(sample_dll(), module_dir / fs::path{sample_dll()}.filename());
    }

    vrcft::ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    registry.rescan();
    const vrcft::ModuleInfo* module = registry.find("com.fgbbridge.sample.sine");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->kind == vrcft::ModuleKind::native);
        FGB_CHECK(module->name == "Sine Sample Module");
        FGB_CHECK(module->version == "1.0.0");
    }
}

// 进程内模块的「重启实例」：必须真的卸载再加载（DLL 被替换过才生效），
// 并且重启后还能继续出帧——之前的实现没覆盖 restart()，界面点了只得到一个错误。
FGB_TEST(native_module_in_process_restart_reloads_dll) {
    if (!fs::exists(sample_dll())) {
        return;
    }
    NativeSourceConfig config;
    config.dll          = sample_dll();
    config.module_id    = "com.fgbbridge.sample.sine";
    config.module_name  = "Sine Sample Module";
    config.display_name = "sine";
    config.tick_hz      = 120;

    NativeModuleSource source{config};
    FGB_CHECK(source.start());
    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 200 && !got; ++i) {
        got = source.poll(frame);
    }
    FGB_CHECK(got);

    FGB_CHECK(source.restart());
    const auto status = source.module_status();
    FGB_CHECK(status.has_value());
    if (status.has_value()) {
        FGB_CHECK(status->alive);
        FGB_CHECK(status->restarts == 1);
        FGB_CHECK(status->error.empty());
    }

    got = false;
    for (int i = 0; i < 200 && !got; ++i) {
        got = source.poll(frame);
    }
    FGB_CHECK(got);
    if (got) {
        FGB_CHECK(frame.has_channel(channel::kExpression));
    }
    source.stop();
}

FGB_TEST(native_gaze_only_module_emits_gaze_channel) {
    if (!fs::exists(sample_gaze_dll())) {
        return;
    }
    NativeSourceConfig config;
    config.dll          = sample_gaze_dll();
    config.module_id    = "com.fgbbridge.sample.gaze";
    config.module_name  = "Native Gaze Sample";
    config.display_name = "gaze";
    config.tick_hz      = 100;

    NativeModuleSource source{config};
    FGB_CHECK(source.start());
    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i) {
        got = source.poll(frame);
    }
    FGB_CHECK(got);
    if (got) {
        FGB_CHECK(frame.has_channel(channel::kGaze));
        FGB_CHECK(!frame.has_channel(channel::kEyes));
        FGB_CHECK(!frame.eyes.any_valid()); // GAZE 不伪造 openness/pupil
        FGB_CHECK(frame.gaze.left.valid && frame.gaze.right.valid);
        FGB_CHECK(frame.gaze.combinedValid);
        FGB_CHECK(frame.gaze.pointsValid);
        FGB_CHECK(frame.gaze.confidence > 0.5f);
        FGB_CHECK(frame.gaze.captureTsNs != 0);
    }
    const auto status = source.module_status();
    FGB_CHECK(status.has_value());
    if (status.has_value()) {
        FGB_CHECK(status->capabilities == FGB_CAP_GAZE);
    }
    source.stop();
}

// SDK 说"方向属性与点位属性分别有效，互不连坐"。只给点位的模块
// （PICO Connect 私有通道就是这样）以前会被整帧丢掉，逼得模块伪造一个 -Z 方向。
FGB_TEST(native_gaze_points_only_frame_is_kept) {
    FgbEyeAttributes eye{};
    eye.gaze_point_2d[0] = 0.82f;
    eye.gaze_point_2d[1] = 0.90f;
    eye.gaze_ts_ns       = 777;

    GazeSample out;
    native::apply_eye_gaze(eye, FGB_ATTR_LEFT_POINT, FGB_ATTR_LEFT_GAZE, FGB_ATTR_LEFT_POINT, 0,
                           out);

    FGB_CHECK(!out.valid); // 没有方向就是没有方向
    FGB_CHECK(out.pointValid); // 但"有点"也算有数据，不能丢
    FGB_CHECK_NEAR(out.point2d.x, 0.82, 1e-6);
    FGB_CHECK(out.captureTsNs == 777);
}

FGB_TEST(native_gaze_direction_and_points_stay_independent) {
    // 左眼同时有方向与点：两个属性各自生效
    FgbEyeAttributes eye{};
    eye.gaze_dir[2]      = -1.0f;
    eye.gaze_point_2d[0] = 0.1f;

    GazeSample left;
    native::apply_eye_gaze(eye, FGB_ATTR_LEFT_GAZE | FGB_ATTR_LEFT_POINT, FGB_ATTR_LEFT_GAZE,
                           FGB_ATTR_LEFT_POINT, 0, left);
    FGB_CHECK(left.valid);
    FGB_CHECK(left.pointValid);

    // 同一只眼只置点位：方向必须保持无效，不能被默认 -Z 冒充
    GazeSample right;
    native::apply_eye_gaze(eye, FGB_ATTR_RIGHT_POINT, FGB_ATTR_RIGHT_GAZE, FGB_ATTR_RIGHT_POINT,
                           0, right);
    FGB_CHECK(!right.valid);
    FGB_CHECK(right.pointValid);
    FGB_CHECK_NEAR(right.point2d.x, 0.1, 1e-6);
}

FGB_TEST(native_gaze_placeholder_direction_is_detectable) {
    GazePair fake;
    fake.left.valid      = true;
    fake.right.valid     = true;
    fake.left.pointValid = true;
    fake.right.pointValid = true;
    // 模块没有方向模型时常见的写法：方向留默认 -Z
    FGB_CHECK(native::gaze_direction_looks_placeholder(fake));

    // 真给了方向就不是这个指纹
    GazePair real;
    real.left.valid       = true;
    real.right.valid      = true;
    real.left.pointValid  = true;
    real.right.pointValid = true;
    real.left.dirLocal    = normalize(Vec3{0.02f, 0.11f, -0.99f});
    real.right.dirLocal   = real.left.dirLocal;
    FGB_CHECK(!native::gaze_direction_looks_placeholder(real));

    // 没有点也不该报（方向型模块本来就可能是正前方）
    GazePair dir_only;
    dir_only.left.valid  = true;
    dir_only.right.valid = true;
    FGB_CHECK(!native::gaze_direction_looks_placeholder(dir_only));
}

// ADR-0033：NaN/Inf 是"没算出来"，不是"看向角落"，必须逐眼否掉（另一只眼不受影响）
FGB_TEST(native_gaze_non_finite_point_is_rejected_per_eye) {
    FgbEyeAttributes bad{};
    bad.gaze_point_2d[0] = std::numeric_limits<float>::quiet_NaN();
    bad.gaze_point_2d[1] = 0.5f;

    GazeSample left;
    native::apply_eye_gaze(bad, FGB_ATTR_LEFT_POINT, FGB_ATTR_LEFT_GAZE, FGB_ATTR_LEFT_POINT, 0,
                           left);
    FGB_CHECK(!left.pointValid); // 这只眼算坏了

    FgbEyeAttributes good{};
    good.gaze_point_2d[0] = 0.4f;
    good.gaze_point_2d[1] = 0.6f;
    GazeSample right;
    native::apply_eye_gaze(good, FGB_ATTR_RIGHT_POINT, FGB_ATTR_RIGHT_GAZE, FGB_ATTR_RIGHT_POINT,
                           0, right);
    FGB_CHECK(right.pointValid); // 另一只眼照样能用
}

// ADR-0033 的越界诊断：视口坐标 [0,1]，0.25 余量内不算异常
FGB_TEST(native_gaze_point_range_diagnostic) {
    GazePair gaze;
    gaze.left.pointValid  = true;
    gaze.right.pointValid = true;
    gaze.pointViewport    = true;              // 只有声明了视口坐标才检查范围
    gaze.left.point2d     = Vec2{0.3f, 1.1f}; // 略微越界（看向下边缘）
    gaze.right.point2d    = Vec2{0.7f, 0.2f};
    FGB_CHECK(!native::gaze_point_out_of_viewport(gaze));

    gaze.left.point2d = Vec2{0.3f, 2.4f}; // 明显不是视口坐标
    FGB_CHECK(native::gaze_point_out_of_viewport(gaze));

    gaze.left.point2d = Vec2{0.3f, 0.5f};
    gaze.right.point2d = Vec2{-1.5f, 0.5f};
    FGB_CHECK(native::gaze_point_out_of_viewport(gaze));
}

FGB_TEST(native_gaze_point_attribute_alone_uses_frame_timestamp) {
    FgbEyeAttributes in{};
    in.gaze_point_2d[0] = 0.4f;
    in.gaze_point_2d[1] = 0.6f;
    in.gaze_ts_ns       = 0; // 0 = 用整帧时刻

    GazeSample sample;
    native::apply_eye_gaze(in, FGB_ATTR_LEFT_POINT, FGB_ATTR_LEFT_GAZE, FGB_ATTR_LEFT_POINT,
                           4242, sample);

    FGB_CHECK(!sample.valid); // 没给方向属性
    FGB_CHECK(sample.pointValid);
    FGB_CHECK_NEAR(sample.point2d.y, 0.6, 1e-6);
    FGB_CHECK(sample.captureTsNs == 4242);
}

// 2D-only 模块（只置点位属性）走完整 ABI：帧要留下来，
// 方向必须无效，点要一路到 Engine。示例模块用环境变量模拟这种模块。
FGB_TEST(native_gaze_points_only_module_reaches_engine) {
    if (!fs::exists(sample_gaze_dll())) {
        return;
    }
    _putenv_s("FGB_SAMPLE_GAZE_POINTS_ONLY", "1");

    NativeSourceConfig config;
    config.dll          = sample_gaze_dll();
    config.module_id    = "com.fgbbridge.sample.gaze";
    config.module_name  = "Native Gaze Sample";
    config.display_name = "gaze";
    config.tick_hz      = 100;

    Engine engine;
    engine.add_source(std::make_unique<NativeModuleSource>(config));
    engine.start();
    for (int i = 0; i < 24; ++i) {
        engine.tick_once(1'000'000ULL + static_cast<std::uint64_t>(i) * 8'333'333ULL);
    }
    const FaceFrame frame = engine.latest();
    const GazeState gaze  = engine.current_gaze();
    engine.stop();
    _putenv_s("FGB_SAMPLE_GAZE_POINTS_ONLY", "");

    FGB_CHECK(frame.has_channel(channel::kGaze)); // 只给点也算这一路在出数据
    FGB_CHECK(!gaze.valid);                       // 不拿默认 -Z 冒充方向
    FGB_CHECK(!gaze.left_valid && !gaze.right_valid);
    FGB_CHECK(gaze.point2d_valid);
    FGB_CHECK(gaze.left_point_valid && gaze.right_point_valid);
    FGB_CHECK(gaze.point_viewport); // 示例模块声明了 POINT_VIEWPORT
    FGB_CHECK(!gaze.fixation);      // 没有方向就没有注视判定
}

FGB_TEST(native_expression_only_module_does_not_claim_eyes) {
    if (!fs::exists(sample_face_dll())) {
        return;
    }
    NativeSourceConfig config;
    config.dll          = sample_face_dll();
    config.module_id    = "com.fgbbridge.sample.face";
    config.module_name  = "Native Face Sample";
    config.display_name = "face";
    config.tick_hz      = 100;

    NativeModuleSource source{config};
    FGB_CHECK(source.start());
    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i) {
        got = source.poll(frame);
    }
    FGB_CHECK(got);
    if (got) {
        FGB_CHECK(frame.has_channel(channel::kExpression));
        FGB_CHECK(!frame.has_channel(channel::kEyes));
        FGB_CHECK(!frame.has_channel(channel::kGaze));
        FGB_CHECK(!frame.eyes.any_valid());
    }
    source.stop();
}

FGB_TEST(native_full_and_gaze_modules_compose) {
    if (!fs::exists(sample_dll()) || !fs::exists(sample_gaze_dll())) {
        return;
    }
    Engine engine;
    NativeSourceConfig sine_cfg;
    sine_cfg.dll          = sample_dll();
    sine_cfg.module_id    = "com.fgbbridge.sample.sine";
    sine_cfg.module_name  = "Sine Sample Module";
    sine_cfg.display_name = "sine";
    sine_cfg.tick_hz      = 120;
    NativeSourceConfig gaze_cfg;
    gaze_cfg.dll          = sample_gaze_dll();
    gaze_cfg.module_id    = "com.fgbbridge.sample.gaze";
    gaze_cfg.module_name  = "Native Gaze Sample";
    gaze_cfg.display_name = "gaze";
    gaze_cfg.tick_hz      = 120;

    engine.add_source(std::make_unique<NativeModuleSource>(sine_cfg));
    engine.add_source(std::make_unique<NativeModuleSource>(gaze_cfg));

    MergePolicy policy;
    policy.expression_source = "sine";
    policy.eye_source        = "sine";
    policy.head_source       = "sine";
    policy.gaze_source       = "gaze";
    engine.set_merge_policy(policy);
    FGB_CHECK(engine.start());

    for (int i = 0; i < 20; ++i) {
        engine.tick_once(1'000'000'000ULL + static_cast<std::uint64_t>(i) * 8'000'000ULL);
    }
    const FaceFrame frame = engine.latest();
    FGB_CHECK(frame.has_channel(channel::kEyes));  // 头像眼睛来自完整模块
    FGB_CHECK(frame.has_channel(channel::kGaze));  // 渲染注视来自 GAZE 模块
    FGB_CHECK(frame.eyes.any_valid());
    const GazeState render = engine.current_gaze();
    FGB_CHECK(render.valid);
    FGB_CHECK(render.source == 2); // 第二个注册的源（gaze）
    engine.stop();
}

FGB_TEST(native_avatar_eye_can_take_gaze_from_gaze_module) {
    if (!fs::exists(sample_dll()) || !fs::exists(sample_gaze_dll())) {
        return;
    }
    Engine engine;
    NativeSourceConfig sine_cfg;
    sine_cfg.dll          = sample_dll();
    sine_cfg.module_id    = "com.fgbbridge.sample.sine";
    sine_cfg.module_name  = "Sine Sample Module";
    sine_cfg.display_name = "sine";
    sine_cfg.tick_hz      = 120;
    NativeSourceConfig gaze_cfg;
    gaze_cfg.dll          = sample_gaze_dll();
    gaze_cfg.module_id    = "com.fgbbridge.sample.gaze";
    gaze_cfg.module_name  = "Native Gaze Sample";
    gaze_cfg.display_name = "gaze";
    gaze_cfg.tick_hz      = 120;

    engine.add_source(std::make_unique<NativeModuleSource>(sine_cfg));
    engine.add_source(std::make_unique<NativeModuleSource>(gaze_cfg));

    MergePolicy policy;
    policy.eye_source           = "sine";
    policy.gaze_source          = "gaze";
    policy.avatar_eye_from_gaze = true;
    engine.set_merge_policy(policy);
    FGB_CHECK(engine.start());

    for (int i = 0; i < 20; ++i) {
        engine.tick_once(1'000'000'000ULL + static_cast<std::uint64_t>(i) * 8'000'000ULL);
    }
    const FaceFrame frame = engine.latest();
    FGB_CHECK(frame.eyes.any_valid());
    FGB_CHECK(frame.eyes.left.opennessSource == 1); // openness 来自完整模块
    FGB_CHECK(frame.eyes.left.gazeSource == 2);    // 方向来自注视模块
    FGB_CHECK(frame.eyes.left.gazeTsNs != 0);
    FGB_CHECK_NEAR(frame.eyes.left.gazeDirLocal.x, frame.gaze.left.dirLocal.x, 1e-6);
    FGB_CHECK_NEAR(frame.eyes.left.gazeDirLocal.z, frame.gaze.left.dirLocal.z, 1e-6);
    engine.stop();
}

FGB_TEST(native_v2_openness_module_emits_eye_attributes_without_gaze) {
    if (!fs::exists(sample_openness_dll())) {
        return;
    }
    NativeSourceConfig config;
    config.dll          = sample_openness_dll();
    config.module_id    = "com.fgbbridge.sample.openness";
    config.module_name  = "Native Openness Sample";
    config.display_name = "openness";
    config.tick_hz      = 100;

    NativeModuleSource source{config};
    FGB_CHECK(source.start());
    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i) {
        got = source.poll(frame);
    }
    FGB_CHECK(got);
    if (got) {
        FGB_CHECK(frame.has_channel(channel::kEyes));
        FGB_CHECK(!frame.has_channel(channel::kGaze));
        FGB_CHECK(frame.eyes.any_valid());
        FGB_CHECK(!frame.eyes.left.gazeValid);
        FGB_CHECK(!frame.eyes.right.gazeValid);
        FGB_CHECK(frame.eyes.left.openness > 0.0f && frame.eyes.left.openness < 1.0f);
        FGB_CHECK(frame.eyes.left.opennessTsNs != 0);
        FGB_CHECK(frame.channelStamps[channel::index(channel::kEyes)].captureTsNs != 0);
    }
    const auto status = source.module_status();
    FGB_CHECK(status.has_value());
    if (status.has_value()) {
        FGB_CHECK(status->capabilities == FGB_CAP_EYES);
    }
    source.stop();
}

FGB_TEST(native_v2_openness_and_gaze_compose_into_avatar_eyes) {
    if (!fs::exists(sample_openness_dll()) || !fs::exists(sample_gaze_dll())) {
        return;
    }
    Engine engine;
    NativeSourceConfig openness_cfg;
    openness_cfg.dll          = sample_openness_dll();
    openness_cfg.module_id    = "com.fgbbridge.sample.openness";
    openness_cfg.module_name  = "Native Openness Sample";
    openness_cfg.display_name = "openness";
    openness_cfg.tick_hz      = 120;
    NativeSourceConfig gaze_cfg;
    gaze_cfg.dll          = sample_gaze_dll();
    gaze_cfg.module_id    = "com.fgbbridge.sample.gaze";
    gaze_cfg.module_name  = "Native Gaze Sample";
    gaze_cfg.display_name = "gaze";
    gaze_cfg.tick_hz      = 120;

    engine.add_source(std::make_unique<NativeModuleSource>(openness_cfg));
    engine.add_source(std::make_unique<NativeModuleSource>(gaze_cfg));

    MergePolicy policy;
    policy.eye_source           = "openness";
    policy.gaze_source          = "gaze";
    policy.avatar_eye_from_gaze = true;
    engine.set_merge_policy(policy);
    FGB_CHECK(engine.start());

    for (int i = 0; i < 20; ++i) {
        engine.tick_once(1'000'000'000ULL + static_cast<std::uint64_t>(i) * 8'000'000ULL);
    }
    const FaceFrame frame = engine.latest();
    FGB_CHECK(frame.eyes.any_valid());
    FGB_CHECK(frame.eyes.left.opennessSource == 1); // openness 只来自 v2 属性模块
    FGB_CHECK(frame.eyes.left.gazeSource == 2);    // 方向来自 gaze 模块
    FGB_CHECK(frame.eyes.left.gazeValid);
    FGB_CHECK_NEAR(frame.eyes.left.gazeDirLocal.x, frame.gaze.left.dirLocal.x, 1e-6);
    FGB_CHECK_NEAR(frame.eyes.left.gazeDirLocal.z, frame.gaze.left.dirLocal.z, 1e-6);
    FGB_CHECK(frame.gaze.left.source == 2);
    engine.stop();
}

FGB_TEST(native_manifest_optional_capability_fields) {
    const fs::path manifests[3] = {sample_face_manifest(), sample_gaze_manifest(),
                                   sample_openness_manifest()};
    if (!fs::exists(manifests[0]) || !fs::exists(manifests[1]) || !fs::exists(manifests[2])) {
        return;
    }
    TempDir root;
    for (const fs::path& manifest : manifests) {
        const std::string dir_name = manifest.parent_path().filename().string();
        copy_into(manifest, root.path / "store" / dir_name / "fgb-native.json");
    }
    vrcft::ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    registry.rescan();

    const vrcft::ModuleInfo* face = registry.find("com.fgbbridge.sample.face");
    FGB_CHECK(face != nullptr);
    if (face != nullptr) {
        FGB_CHECK(face->declared_capabilities == FGB_CAP_EXPRESSION);
        FGB_CHECK(face->role_hint == "expression");
        FGB_CHECK(face->abi_version == 2);
        FGB_CHECK(face->host_compatible);
    }
    const vrcft::ModuleInfo* gaze = registry.find("com.fgbbridge.sample.gaze");
    FGB_CHECK(gaze != nullptr);
    if (gaze != nullptr) {
        FGB_CHECK(gaze->declared_capabilities == FGB_CAP_GAZE);
        FGB_CHECK(gaze->role_hint == "gaze");
    }
    const vrcft::ModuleInfo* openness = registry.find("com.fgbbridge.sample.openness");
    FGB_CHECK(openness != nullptr);
    if (openness != nullptr) {
        FGB_CHECK(openness->declared_capabilities == FGB_CAP_EYES);
        FGB_CHECK(openness->role_hint == "eyes");
    }
}

// ABI v1 已移除（ADR-0034）：清单写 AbiVersion 1 的模块在列表里就标成不兼容，
// 不必等 LoadLibrary 才失败。
FGB_TEST(native_manifest_rejects_removed_abi_version) {
    TempDir root;
    const fs::path dir = root.path / "store" / "com.example.legacy";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream out{dir / "fgb-native.json"};
        out << R"({
  "ModuleId": "com.example.legacy",
  "ModuleName": "Legacy Module",
  "Version": "0.9.0",
  "DllFileName": "legacy.dll",
  "AbiVersion": 1,
  "Capabilities": ["expression"]
})";
    }

    vrcft::ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    registry.rescan();
    const vrcft::ModuleInfo* module = registry.find("com.example.legacy");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->abi_version == 1);
        FGB_CHECK(!module->host_compatible);
    }
}

// ABI v1 已移除（ADR-0034）：只有老符号的 DLL 必须被拒绝，并说清怎么修，
// 不能让用户以为"这不是 FGB 模块"。
FGB_TEST(native_v1_only_dll_is_rejected_with_migration_hint) {
    const fs::path legacy = exe_dir() / "fgb-test-legacy-v1.dll";
    if (!fs::exists(legacy)) {
        return;
    }
    NativeSourceConfig config;
    config.dll         = legacy;
    config.module_id   = "com.example.legacy-v1";
    config.module_name = "Legacy V1 Fixture";
    NativeModuleSource source{config};
    FGB_CHECK(!source.start());
    FGB_CHECK(source.last_error().find("v1") != std::string::npos);
    FGB_CHECK(source.last_error().find("重新编译") != std::string::npos);
}

FGB_TEST(native_module_rejects_dll_without_export) {
    wchar_t system_dir[MAX_PATH]{};
    if (::GetSystemDirectoryW(system_dir, MAX_PATH) == 0) {
        return;
    }
    NativeSourceConfig config;
    config.dll         = fs::path{system_dir} / "kernel32.dll";
    config.module_id   = "not-a-module";
    config.module_name = "kernel32";
    NativeModuleSource source{config};
    FGB_CHECK(!source.start());
    FGB_CHECK(!source.last_error().empty());
}
