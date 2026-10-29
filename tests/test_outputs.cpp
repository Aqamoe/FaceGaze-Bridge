// 输出设置的读写与校验。
//
// 这份文件是"界面保存 → 下次启动生效"的桥：写错一个键，用户重启后输出就悄悄没了。
#include "config/outputs.hpp"

#include "test_framework.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fgb;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("fgb-outputs-" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

config::OutputOptions sample() {
    config::OutputOptions out;
    out.vrchat_host  = "127.0.0.2";
    out.vrchat_port  = 9001;
    out.eye_mode     = "both";
    out.params_mode  = "v1";
    out.vmc_enabled  = true;
    out.vmc_host     = "127.0.0.3";
    out.vmc_port     = 39540;
    out.shm_enabled  = true;
    out.shm_name     = "unity";
    out.ws_enabled   = true;
    out.ws_port      = 9201;
    out.ws_rate_hz   = 60;
    out.ws_all_shapes = true;
    out.udp_enabled  = true;
    out.udp_host     = "127.0.0.4";
    out.udp_port     = 9301;
    out.udp_rate_hz  = 90;
    return out;
}

} // namespace

FGB_TEST(outputs_parse_target) {
    std::string host;
    std::uint16_t port = 0;
    FGB_CHECK(config::parse_target("127.0.0.1:9000", host, port));
    FGB_CHECK(host == "127.0.0.1");
    FGB_CHECK(port == 9000);
    FGB_CHECK(config::parse_target(" localhost:1 ", host, port));
    FGB_CHECK(host == "localhost");
    FGB_CHECK(port == 1);

    FGB_CHECK(!config::parse_target("", host, port));
    FGB_CHECK(!config::parse_target("127.0.0.1", host, port));
    FGB_CHECK(!config::parse_target(":9000", host, port));
    FGB_CHECK(!config::parse_target("127.0.0.1:0", host, port));
    FGB_CHECK(!config::parse_target("127.0.0.1:70000", host, port));
    FGB_CHECK(config::format_target("127.0.0.1", 9000) == "127.0.0.1:9000");
}

FGB_TEST(outputs_validate_rejects_bad_values) {
    config::OutputOptions options;
    std::string error;
    FGB_CHECK(config::validate_outputs(options, error));
    FGB_CHECK(error.empty());

    options = {};
    options.eye_mode = "warp";
    FGB_CHECK(!config::validate_outputs(options, error));
    FGB_CHECK(!error.empty());

    options = {};
    options.ws_enabled = true;
    options.ws_port    = 0;
    FGB_CHECK(!config::validate_outputs(options, error));

    options            = {};
    options.udp_enabled = true;
    options.udp_host    = "";
    FGB_CHECK(!config::validate_outputs(options, error));
}

FGB_TEST(outputs_save_load_roundtrip) {
    TempDir temp;
    const fs::path path = temp.path / "outputs.ini";
    const config::OutputOptions original = sample();

    std::string error;
    FGB_CHECK(config::save_outputs(path, original, error));
    FGB_CHECK(error.empty());
    FGB_CHECK(fs::exists(path));

    config::OutputOptions loaded;
    FGB_CHECK(config::load_outputs(path, loaded, error));
    FGB_CHECK(loaded == original);

    // 再存一次应留下上一版 .bak
    FGB_CHECK(config::save_outputs(path, original, error));
    FGB_CHECK(fs::exists(path.string() + ".bak"));
}

FGB_TEST(output_profiles_roundtrip_and_listing) {
    TempDir temp;
    const fs::path dir = temp.path / "profiles";
    std::string error;

    const config::OutputOptions high = sample();
    FGB_CHECK(config::save_output_profile(dir, "VR 高帧率", high, error));
    FGB_CHECK(error.empty());

    config::OutputOptions desktop = sample();
    desktop.ws_enabled   = false;
    desktop.udp_enabled  = false;
    desktop.shm_enabled  = false;
    desktop.vmc_enabled  = false;
    FGB_CHECK(config::save_output_profile(dir, "桌面", desktop, error));

    std::vector<config::OutputProfile> profiles;
    FGB_CHECK(config::list_output_profiles(dir, profiles, error));
    FGB_CHECK(profiles.size() == 2);

    bool found = false;
    for (const config::OutputProfile& profile : profiles) {
        if (profile.name != "VR 高帧率") {
            continue;
        }
        found = true;
        config::OutputOptions loaded;
        std::string name;
        FGB_CHECK(config::load_output_profile(profile.path, loaded, name, error));
        FGB_CHECK(name == "VR 高帧率");
        FGB_CHECK(loaded == high);
    }
    FGB_CHECK(found);

    // 同名再保存 = 覆盖，不新增文件
    desktop.ws_enabled = true;
    desktop.ws_port    = 9333;
    FGB_CHECK(config::save_output_profile(dir, "桌面", desktop, error));
    FGB_CHECK(config::list_output_profiles(dir, profiles, error));
    FGB_CHECK(profiles.size() == 2);

    FGB_CHECK(config::delete_output_profile(dir, "桌面", error));
    FGB_CHECK(config::list_output_profiles(dir, profiles, error));
    FGB_CHECK(profiles.size() == 1);
    FGB_CHECK(!config::delete_output_profile(dir, "不存在", error));
    FGB_CHECK(!error.empty());
}

FGB_TEST(output_profiles_reject_bad_names_and_values) {
    TempDir temp;
    const fs::path dir = temp.path / "profiles";
    std::string error;

    for (const char* name : {"", " a", "a ", "a/b", "a\\b", "a:b", "a*b", "..", "."}) {
        FGB_CHECK(!config::save_output_profile(dir, name, sample(), error));
        FGB_CHECK(!error.empty());
    }
    FGB_CHECK(!config::save_output_profile(dir, std::string(80, 'x'), sample(), error));
    FGB_CHECK(!config::save_output_profile(dir, "ok", config::OutputOptions{.eye_mode = "warp"},
                                          error));

    config::OutputOptions loaded;
    std::string name;
    FGB_CHECK(!config::load_output_profile(dir / "missing.ini", loaded, name, error));
}

FGB_TEST(output_profiles_skip_corrupt_files) {
    TempDir temp;
    const fs::path dir = temp.path / "profiles";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream out(dir / "broken.ini");
        out << "[profile]\nname = 坏预设\n[ws]\nenabled = 1\nport = 0\n";
    }

    std::string error;
    FGB_CHECK(config::save_output_profile(dir, "好的", sample(), error));
    std::vector<config::OutputProfile> profiles;
    FGB_CHECK(config::list_output_profiles(dir, profiles, error));
    FGB_CHECK(profiles.size() == 1);
    FGB_CHECK(profiles.front().name == "好的");
}

FGB_TEST(outputs_load_missing_or_invalid_falls_back) {
    TempDir temp;
    config::OutputOptions options;
    std::string error;

    FGB_CHECK(!config::load_outputs(temp.path / "missing.ini", options, error));
    FGB_CHECK(options == config::OutputOptions{});

    const fs::path bad = temp.path / "bad.ini";
    {
        std::ofstream out(bad);
        out << "[ws]\nenabled = 1\nport = 0\n";
    }
    FGB_CHECK(!config::load_outputs(bad, options, error));
    FGB_CHECK(!error.empty());
    FGB_CHECK(options == config::OutputOptions{});
}
