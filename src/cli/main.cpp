#include "base/clock.hpp"
#include "base/log.hpp"
#include "engine/engine.hpp"
#include "capture/capture_file.hpp"
#include "params/avatar_config.hpp"
#include "params/catalog.hpp"
#include "sinks/vrchat_sink.hpp"
#include "capture/capture_file.hpp"
#include "sources/replay_source.hpp"
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
                 "  fgb-cli export <抓包> --out <csv>  把抓包跑过参数层导出 CSV（回归对比）\n"
                 "  fgb-cli params         查看参数地址解析结果（排查表情不动时先跑这个）\n"
                 "         --list          列出最近修改的头像配置\n"
                 "         --avatar-config <路径>\n"
                 "  fgb-cli version        显示版本\n"
                 "\n"
                 "run 选项:\n"
                 "  --source <类型>         vmc | vrcft | replay (默认 vmc)\n"
                 "  --record <文件>         把收到的 ReplyUpdate 原样录制（仅 vrcft）\n"
                 "  --replay <文件>         回放抓包（配合 --source replay）\n"
                 "  --replay-speed <倍率>   回放速度，0 = 不限 (默认 1.0)\n"
                 "  --replay-loop           回放到末尾后重新开始\n"
                 "  --vmc-port <端口>        VMC 监听端口 (默认 39539)\n"
                 "  --module-exe <路径>     VRCFaceTracking.ModuleProcess.exe 路径\n"
                 "  --module-dll <路径>     要加载的模块 DLL 路径\n"
                 "  --vrchat <host:port>    VRChat OSC 目标 (默认 127.0.0.1:9000)\n"
                 "  --eye-mode <模式>       parameters | native | both (默认 parameters)\n"
                 "  --params <参数集>       v2 | v1 | both (默认 v2)\n"
                 "  --param-prefix <前缀>   找不到头像配置时 v2 参数用的地址前缀\n"
                 "                          (默认 FT/v2/，因为多数头像这么命名)\n"
                 "  --avatar-config <路径>  指定头像参数表 JSON，不给则自动挑最新的\n"
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
    fgb::ReplayConfig replay_cfg;
    std::string record_path;
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
            if (v == "vmc" || v == "vrcft" || v == "replay") {
                source_kind = std::string(v);
            } else {
                std::fprintf(stderr, "未知的 source 类型\n");
                return 2;
            }
        } else if (arg == "--record") {
            record_path = std::string{next_value(arg)};
        } else if (arg == "--replay") {
            replay_cfg.path = std::string{next_value(arg)};
        } else if (arg == "--replay-speed") {
            const auto v = next_value(arg);
            replay_cfg.speed = static_cast<float>(std::atof(std::string{v}.c_str()));
        } else if (arg == "--replay-loop") {
            replay_cfg.loop = true;
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
        } else if (arg == "--params") {
            const auto v = next_value(arg);
            if (v.empty() || !fgb::parse_params_mode(v, vrchat_cfg.params_mode)) {
                std::fprintf(stderr, "未知的 params 模式\n");
                return 2;
            }
        } else if (arg == "--param-prefix") {
            const auto v = next_value(arg);
            if (v.empty()) {
                return 2;
            }
            vrchat_cfg.fallback_prefix = std::string{v};
        } else if (arg == "--avatar-config") {
            const auto v = next_value(arg);
            if (v.empty()) {
                return 2;
            }
            vrchat_cfg.avatar_config = std::string{v};
            vrchat_cfg.discover_avatar = true;
        } else if (arg == "--no-avatar-discovery") {
            vrchat_cfg.discover_avatar = false;
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
        if (!record_path.empty()) {
            cfg.record_path = record_path;
        }
        engine.add_source(std::make_unique<fgb::vrcft::VrcftSource>(cfg));
    } else if (source_kind == "replay") {
        if (replay_cfg.path.empty()) {
            std::fprintf(stderr, "--source replay 需要 --replay <文件>\n");
            return 2;
        }
        engine.add_source(std::make_unique<fgb::ReplaySource>(replay_cfg));
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

// 绑到"最可能是当前穿着"的头像上（日志优先，启发式兜底）。
// export 与 params 共用，避免两处判定逻辑走偏。
bool bind_best_avatar(fgb::params::ParamRouter& router, const std::string& avatar_path,
                      const std::string& prefix) {
    if (!avatar_path.empty()) {
        fgb::params::AvatarConfig cfg;
        return fgb::params::load_avatar_file(avatar_path, cfg) && (router.bind(cfg), true);
    }
    const std::string root = fgb::params::default_osc_root();
    const std::string id   = fgb::params::current_avatar_id();
    fgb::params::AvatarConfig current;
    if (!id.empty() && fgb::params::load_avatar_by_id(root, id, current)) {
        router.bind(current);
        return true;
    }
    const auto pick = fgb::params::pick_avatar(root);
    if (pick.valid) {
        router.bind(pick.config);
        return true;
    }
    router.use_fallback(prefix);
    return true;
}

// 把抓包逐帧跑过参数层，导出成 CSV。
//
// 这是给"改完参数层之后，表情幅度有没有变"用的回归工具：同一份抓包跑两遍，
// diff 两个 CSV 就知道动了哪个参数。没有它，每次验证都要戴上头显肉眼看。
int cmd_export(int argc, char** argv) {
    std::string capture_path;
    std::string out_path;
    std::string avatar_path;
    std::string prefix = "FT/v2/";
    fgb::ParamsMode mode = fgb::ParamsMode::v2;
    std::size_t limit    = 0;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string_view {
            return (i + 1 < argc) ? std::string_view{argv[++i]} : std::string_view{};
        };
        if (arg == "--out") {
            out_path = std::string{next()};
        } else if (arg == "--avatar-config") {
            avatar_path = std::string{next()};
        } else if (arg == "--param-prefix") {
            prefix = std::string{next()};
        } else if (arg == "--params") {
            fgb::parse_params_mode(next(), mode);
        } else if (arg == "--limit") {
            limit = static_cast<std::size_t>(std::atoll(std::string{next()}.c_str()));
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法: fgb-cli export <抓包文件> --out params.csv [--limit N]\n");
            return 0;
        } else if (!arg.empty() && arg[0] != '-') {
            capture_path = std::string{arg};
        }
    }
    if (capture_path.empty() || out_path.empty()) {
        std::fprintf(stderr, "用法: fgb-cli export <抓包文件> --out params.csv\n");
        return 2;
    }

    fgb::capture::Reader reader;
    if (!reader.open(capture_path)) {
        std::fprintf(stderr, "%s%s\n", "打开失败：", reader.error().c_str());
        return 1;
    }

    fgb::params::ParamRouter router;
    bind_best_avatar(router, avatar_path, prefix);

    std::FILE* out = std::fopen(out_path.c_str(), "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "%s%s\n", "无法写入 ", out_path.c_str());
        return 1;
    }

    // 表头：只包含真正会被发出去的参数（地址已解析）
    struct Col {
        const fgb::params::ParamEntry* entry;
        std::string name;
    };
    // 只导出"真的会发出去"的参数。
    // 注意判据是"有没有任何发送目标"，而不是"有没有主地址"——纯二进制参数
    // （只有 <名>1/2/4，没有同名 float）主地址是空的，但必须导出。
    std::vector<const fgb::params::ParamEntry*> bound;
    for (const auto& target : router.targets()) {
        if (target.entry == nullptr) {
            continue;
        }
        bool seen = false;
        for (const auto* e : bound) {
            if (e == target.entry) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            bound.push_back(target.entry);
        }
    }

    std::vector<Col> columns;
    {
        const auto groups = fgb::params::groups();
        for (std::size_t g = 0; g < groups.size(); ++g) {
            const bool is_v1 = g >= 2;
            const bool emit  = (mode == fgb::ParamsMode::both) ||
                              (mode == fgb::ParamsMode::v1 ? is_v1 : !is_v1);
            if (!emit) {
                continue;
            }
            for (const auto& entry : groups[g].entries) {
                bool used = false;
                for (const auto* e : bound) {
                    if (e == &entry) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    continue;
                }
                std::string name{groups[g].name_prefix};
                name += entry.name;
                columns.push_back({&entry, std::move(name)});
            }
        }
    }
    if (columns.empty()) {
        std::fprintf(stderr, "没有任何参数被绑定——头像可能不支持面捕\n");
        std::fclose(out);
        return 1;
    }

    std::fprintf(out, "frame,ts_ms");
    for (const Col& c : columns) {
        std::fprintf(out, ",%s", c.name.c_str());
    }
    std::fprintf(out, "\n");

    fgb::FaceFrame frame;
    std::size_t row = 0;
    std::size_t skipped = 0;
    while (reader.next()) {
        if (limit != 0 && row >= limit) {
            break;
        }
        fgb::vrcft::UpdateData data{};
        if (!fgb::vrcft::decode_update(
                std::span<const std::byte>{reader.payload(), reader.payload_size()}, data)) {
            ++skipped;
            continue;
        }
        if (!data.has_any_valid()) {
            ++skipped; // 与实时路径一致：无效帧不产出
            continue;
        }
        fgb::vrcft::ConvertOptions options;
        fgb::vrcft::apply_to_frame(data, options, frame);

        const fgb::params::EvalContext ctx{frame, options.eye_full_scale_deg};
        std::fprintf(out, "%zu,%.3f", row,
                     static_cast<double>(reader.timestamp_ns()) / 1'000'000.0);
        for (const Col& c : columns) {
            std::fprintf(out, ",%.5f", static_cast<double>(c.entry->eval(ctx)));
        }
        std::fprintf(out, "\n");
        ++row;
    }
    std::fclose(out);

    std::fprintf(stderr, "导出 %zu 帧 × %zu 参数 → %s（跳过 %zu 个无效帧）\n", row, columns.size(),
                 out_path.c_str(), skipped);
    return 0;
}

// 参数地址诊断：把"头像声明了什么"和"我们准备发到哪里"摆在一起看。
// 排查"表情不动"时这一步能省掉大半时间。
int cmd_params(int argc, char** argv) {
    std::string avatar_path;
    std::string prefix = "FT/v2/";
    fgb::ParamsMode mode = fgb::ParamsMode::v2;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string_view {
            return (i + 1 < argc) ? std::string_view{argv[++i]} : std::string_view{};
        };
        if (arg == "--avatar-config") {
            avatar_path = std::string{next()};
        } else if (arg == "--param-prefix") {
            prefix = std::string{next()};
        } else if (arg == "--params") {
            fgb::parse_params_mode(next(), mode);
        } else if (arg == "--list") {
            const auto all = fgb::params::scan_avatars(fgb::params::default_osc_root(), 20);
            std::printf("%s\n", ("最近修改的 " + std::to_string(all.size()) +
                                 " 个头像配置：").c_str());
            for (const auto& cfg : all) {
                std::printf("  %-40s %-24s %zu\n", cfg.id.c_str(), cfg.name.c_str(),
                            cfg.params.size());
            }
            return 0;
        }
    }

    fgb::params::ParamRouter router;
    if (avatar_path.empty()) {
        const std::string root = fgb::params::default_osc_root();

        // 优先用 VRChat 日志确认"当前穿着"，而不是猜文件时间
        const std::string current_id = fgb::params::current_avatar_id();
        fgb::params::AvatarConfig current;
        if (!current_id.empty() && fgb::params::load_avatar_by_id(root, current_id, current)) {
            std::printf("%s\n", ("当前穿着「" + current.name + "」(" + current.id + ")，" +
                                 std::to_string(current.params.size()) + " 个声明参数（来源：日志）")
                                    .c_str());
            router.bind(current);
        } else {
            const auto pick = fgb::params::pick_avatar(root);
            if (!pick.valid) {
                std::printf("没找到头像参数表，使用兜底前缀 %s\n", prefix.c_str());
                router.use_fallback(prefix);
            } else {
                std::printf("%s\n", ("启发式选中「" + pick.config.name + "」(" +
                                     pick.config.id + ")，" +
                                     std::to_string(pick.config.params.size()) + " 个声明参数，" +
                                     std::to_string(pick.v2_param_count) + " 个 v2")
                                        .c_str());
                router.bind(pick.config);
            }
        }
    } else {
        fgb::params::AvatarConfig cfg;
        if (!fgb::params::load_avatar_file(avatar_path, cfg)) {
            std::fprintf(stderr, "无法解析 %s\n", avatar_path.c_str());
            return 1;
        }
        std::printf("%s\n", ("使用「" + cfg.name + "」(" + cfg.id + ")，" +
                             std::to_string(cfg.params.size()) + " 个声明参数")
                                .c_str());
        router.bind(cfg);
    }

    std::printf("%s\n\n", ("命中 " + std::to_string(router.param_count()) + " / " +
                            std::to_string(router.total()) + " 个参数（" +
                            std::to_string(router.binary_param_count()) + " 个走二进制）")
                               .c_str());
    std::size_t slot = 0;
    std::size_t shown = 0;
    const auto groups = fgb::params::groups();
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const bool is_v1 = g >= 2;
        if ((mode == fgb::ParamsMode::v2 && is_v1) || (mode == fgb::ParamsMode::v1 && !is_v1)) {
            slot += groups[g].entries.size();
            continue;
        }
        for (const auto& entry : groups[g].entries) {
            const std::string& addr = router.address(slot++);
            if (addr.empty()) {
                continue;
            }
            if (shown < 40) {
                std::printf("  %-28.*s -> %s\n", static_cast<int>(entry.name.size()),
                            entry.name.data(), addr.c_str());
                ++shown;
            }
        }
    }
    if (shown == 40) {
        std::printf("  ...\n");
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
    if (cmd == "params") {
        return cmd_params(argc, argv);
    }
    if (cmd == "export") {
        return cmd_export(argc, argv);
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
