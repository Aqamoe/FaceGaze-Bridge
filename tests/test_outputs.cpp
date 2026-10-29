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
