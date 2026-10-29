// 原生 C ABI 模块：加载示例 DLL、读帧、模块仓识别。
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
