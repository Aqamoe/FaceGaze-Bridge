#include "base/clock.hpp"
#include "base/log.hpp"
#include "engine/engine.hpp"
#include "sinks/vrchat_sink.hpp"
#include "sources/vmc_source.hpp"
#include "sources/vrcft_source.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef FGB_VERSION
#define FGB_VERSION "0.0.0"
#endif

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}
#endif

void print_usage() {
    std::fprintf(stderr,
                 "FaceGaze Bridge " FGB_VERSION "\n"
                 "\n"
                 "用法:\n"
                 "  fgb-cli run [选项]     启动桥接\n"
                 "  fgb-cli shapes         列出全部统一形状\n"
                 "  fgb-cli version        显示版本\n"
                 "\n"
                 "run 选项:\n"
                 "  --source <类型>         vmc | vrcft (默认 vmc)\n"
                 "  --vmc-port <端口>        VMC 监听端口 (默认 39539)\n"
                 "  --module-exe <路径>     VRCFaceTracking.ModuleProcess.exe 路径\n"
                 "  --module-dll <路径>     要加载的模块 DLL 路径\n"
                 "  --vrchat <host:port>    VRChat OSC 目标 (默认 127.0.0.1:9000)\n"
                 "  --eye-mode <模式>       parameters | native | both (默认 parameters)\n"
                 "  --tick-hz <hz>          引擎 tick 频率 (默认 120)\n"
                 "  --duration <秒>         运行指定秒数后退出，0 为不限 (默认 0)\n"
                 "  --verbose               打开调试日志\n");
}

bool parse_uint16(std::string_view text, std::uint16_t& out) {
    if (text.empty()) {
        return false;
    }
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + static_cast<unsigned>(c - '0');
        if (value > 65535u) {
            return false;
        }
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

// VRCFT 的默认安装位置（Steam 版）。找不到就让用户用 --module-exe 指定。
std::filesystem::path default_vrcft_dir() {
    const std::filesystem::path candidates[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\VRCFaceTracking",
        "C:\\Program Files\\Steam\\steamapps\\common\\VRCFaceTracking",
    };
    std::error_code ec;
    for (const auto& dir : candidates) {
        if (std::filesystem::exists(dir, ec)) {
            return dir;
        }
    }
    return {};
}

// 在 %APPDATA%\VRCFaceTracking\CustomLibs 下找第一个模块 DLL
std::filesystem::path find_module_dll() {
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr) {
        return {};
    }
    const std::filesystem::path base =
        std::filesystem::path(appdata) / "VRCFaceTracking" / "CustomLibs";
    std::error_code ec;
    if (!std::filesystem::exists(base, ec)) {
        return {};
    }
    for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->path().extension() == ".dll") {
            return it->path();
        }
    }
    return {};
}

bool parse_host_port(std::string_view text, std::string& host, std::uint16_t& port) {    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        host = std::string(text);
        return !host.empty();
    }
    host = std::string(text.substr(0, colon));
    return !host.empty() && parse_uint16(text.substr(colon + 1), port);
}

int cmd_run(int argc, char** argv) {
    fgb::VmcSourceConfig vmc_cfg;
    fgb::VrchatSinkConfig vrchat_cfg;
    std::string source_kind;
    std::filesystem::path module_exe;
    std::filesystem::path module_dll;
    int tick_hz      = 120;
    int duration_sec = 0;
    bool verbose     = false;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto next_value            = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %.*s 缺少值\n", static_cast<int>(name.size()), name.data());
                return {};
            }
            return argv[++i];
        };

        if (arg == "--source") {
            const auto v = next_value(arg);
            if (v == "vmc" || v == "vrcft") {
                source_kind = std::string(v);
            } else {
                std::fprintf(stderr, "未知的 source 类型\n");
                return 2;
            }
        } else if (arg == "--module-exe") {
            const auto v = next_value(arg);
            if (!v.empty()) {
                module_exe = std::string(v);
            }
        } else if (arg == "--module-dll") {
            const auto v = next_value(arg);
            if (!v.empty()) {
                module_dll = std::string(v);
            }
        } else if (arg == "--vmc-port") {
            const auto v = next_value(arg);
            if (v.empty() || !parse_uint16(v, vmc_cfg.port)) {
                return 2;
            }
        } else if (arg == "--vrchat") {
            const auto v = next_value(arg);
            if (v.empty() || !parse_host_port(v, vrchat_cfg.host, vrchat_cfg.port)) {
                return 2;
            }
        } else if (arg == "--eye-mode") {
            const auto v = next_value(arg);
            if (v.empty() || !fgb::parse_eye_output_mode(v, vrchat_cfg.eye_mode)) {
                std::fprintf(stderr, "未知的 eye-mode\n");
                return 2;
            }
        } else if (arg == "--tick-hz") {
            const auto v = next_value(arg);
            std::uint16_t hz = 0;
            if (v.empty() || !parse_uint16(v, hz) || hz == 0) {
                return 2;
            }
            tick_hz = hz;
        } else if (arg == "--duration") {
            const auto v = next_value(arg);
            std::uint16_t sec = 0;
            if (v.empty() || !parse_uint16(v, sec)) {
                return 2;
            }
            duration_sec = sec;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %.*s\n", static_cast<int>(arg.size()), arg.data());
            return 2;
        }
    }

    fgb::log::set_level(verbose ? fgb::log::Level::debug : fgb::log::Level::info);

    fgb::EngineConfig engine_cfg;
    engine_cfg.tick_hz = tick_hz;
    fgb::Engine engine{engine_cfg};

    if (source_kind == "vrcft") {
        if (module_exe.empty()) {
            const auto dir = default_vrcft_dir();
            if (!dir.empty()) {
                module_exe = dir / "VRCFaceTracking.ModuleProcess.exe";
            }
        }
        if (module_dll.empty()) {
            module_dll = find_module_dll();
        }
        fgb::vrcft::VrcftSourceConfig cfg;
        cfg.module_process_exe = module_exe;
        cfg.module_dll         = module_dll;
        cfg.tick_hz            = tick_hz;
        engine.add_source(std::make_unique<fgb::vrcft::VrcftSource>(cfg));
    } else {
        engine.add_source(std::make_unique<fgb::VmcSource>(vmc_cfg));
    }
    engine.add_sink(std::make_unique<fgb::VrchatSink>(vrchat_cfg));
    fgb::clock::raise_timer_resolution();

    if (!engine.start()) {
        fgb::clock::restore_timer_resolution();
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#endif

    const std::uint64_t period    = 1'000'000'000ULL / static_cast<std::uint64_t>(tick_hz);
    const std::uint64_t started   = fgb::clock::now_ns();
    std::uint64_t next_tick       = started + period;
    std::uint64_t last_report     = started;

    while (!g_stop.load(std::memory_order_acquire)) {
        engine.tick_once(fgb::clock::now_ns());

        fgb::clock::sleep_until_ns(next_tick);
        next_tick += period;

        const std::uint64_t now = fgb::clock::now_ns();
        if (next_tick < now) {
            next_tick = now + period;
        }

        if (now - last_report >= 2'000'000'000ULL) {
            const auto st = engine.stats();
            std::fprintf(stderr, "[stat] tick=%llu frames=%llu late=%llu\n",
                         static_cast<unsigned long long>(st.ticks),
                         static_cast<unsigned long long>(st.frames_in),
                         static_cast<unsigned long long>(st.late_ticks));
            last_report = now;
        }

        if (duration_sec > 0 && (now - started) >= static_cast<std::uint64_t>(duration_sec) * 1'000'000'000ULL) {
            break;
        }
    }

    engine.stop();
    fgb::clock::restore_timer_resolution();

    const auto st = engine.stats();
    std::fprintf(stderr, "[done] tick=%llu frames=%llu late=%llu\n",
                 static_cast<unsigned long long>(st.ticks),
                 static_cast<unsigned long long>(st.frames_in),
                 static_cast<unsigned long long>(st.late_ticks));
    return 0;
}

int cmd_shapes() {
    std::printf("共 %zu 个统一形状\n", fgb::kShapeCount);
    for (std::size_t i = 0; i < fgb::kShapeCount; ++i) {
        std::printf("  %3zu  FT/v2/%s\n", i, fgb::kShapeNames[i]);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    const std::string_view cmd = argv[1];
    if (cmd == "run") {
        return cmd_run(argc, argv);
    }
    if (cmd == "shapes") {
        return cmd_shapes();
    }
    if (cmd == "version" || cmd == "--version") {
        std::printf("FaceGaze Bridge " FGB_VERSION "\n");
        return 0;
    }
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_usage();
        return 0;
    }

    std::fprintf(stderr, "未知命令: %.*s\n\n", static_cast<int>(cmd.size()), cmd.data());
    print_usage();
    return 2;
}
