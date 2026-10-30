// 原生 C ABI 模块：加载示例 DLL、读帧、模块仓识别。
#include "engine/engine.hpp"
#include "sources/native_source.hpp"
#include "vrcft/module_registry.hpp"

#include "test_framework.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
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
        // v1 旧模块不写 gaze 字段；宿主不能凭空造出注视通道
        FGB_CHECK(!frame.has_channel(channel::kGaze));
        FGB_CHECK(frame.eyes.left.valid);
        FGB_CHECK(frame.eyes.left.openness > 0.05f && frame.eyes.left.openness < 0.95f);
        FGB_CHECK(frame.shape_is_valid(Shape::JawOpen));
        FGB_CHECK(frame.shape(Shape::JawOpen) > 0.05f);
        FGB_CHECK(frame.hasHeadPose);
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

FGB_TEST(native_manifest_optional_capability_fields) {
    const fs::path manifests[2] = {sample_face_manifest(), sample_gaze_manifest()};
    if (!fs::exists(manifests[0]) || !fs::exists(manifests[1])) {
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
        FGB_CHECK(face->min_host_version == "1.0.2");
        FGB_CHECK(face->host_compatible);
    }
    const vrcft::ModuleInfo* gaze = registry.find("com.fgbbridge.sample.gaze");
    FGB_CHECK(gaze != nullptr);
    if (gaze != nullptr) {
        FGB_CHECK(gaze->declared_capabilities == FGB_CAP_GAZE);
        FGB_CHECK(gaze->role_hint == "gaze");
    }
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
