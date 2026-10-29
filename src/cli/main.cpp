#include "base/clock.hpp"
#include "base/log.hpp"
#include "engine/engine.hpp"
#include "capture/capture_file.hpp"
#include "core/process/one_euro.hpp"
#include "web/ipc.hpp"
#include "params/avatar_config.hpp"
#include "params/calibration.hpp"
#include "params/catalog.hpp"
#include "sinks/vmc_sink.hpp"
#include "sinks/vrchat_sink.hpp"
#include "capture/capture_file.hpp"
#include "sources/replay_source.hpp"
#include "sources/vmc_source.hpp"
#include "sources/vrcft_source.hpp"
#include "vrcft/module_registry.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef FGB_VERSION
#define FGB_VERSION "0.0.0"
#endif

namespace {

std::atomic<bool> g_stop{false};
// 桌面入口据此重启引擎（退出码 3），而不是把窗口一起关掉
std::atomic<bool> g_restart{false};
constexpr int kExitRestartEngine = 3;

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
                 "  fgb-cli calibrate <放松抓包> --express <夸张抓包> --out <档案>\n"
                 "                     从两份抓包推导逐形状校准\n"
                 "  fgb-cli params         查看参数地址解析结果（排查表情不动时先跑这个）\n"
                 "         --list          列出最近修改的头像配置\n"
                 "         --avatar-config <路径>\n"
                 "  fgb-cli modules        列出磁盘上的 VRCFT 模块与当前选择\n"
                 "         use <id|none>   指定启动时加载哪个模块（写 fgb.ini，重启生效）\n"
                 "  fgb-cli version        显示版本\n"
                 "\n"
                 "run 选项:\n"
                 "  --source <类型>         vmc | vrcft | replay，可重复；顺序即优先级 (默认 vmc)\n"
                 "  --eye-source <名>       指定眼追来自哪个源（默认自动）\n"
                 "  --expression-source <名> 指定表情来自哪个源（默认自动）\n"
                 "  --head-source <名>      指定头姿来自哪个源（默认自动）\n"
                 "  --stale-ms <毫秒>       某源多久没更新就不再参与合并 (默认 500)\n"
                 "  --record <文件>         把收到的 ReplyUpdate 原样录制（仅 vrcft）\n"
                 "  --replay <文件>         回放抓包（配合 --source replay）\n"
                 "  --replay-speed <倍率>   回放速度，0 = 不限 (默认 1.0)\n"
                 "  --replay-loop           回放到末尾后重新开始\n"
                 "  --panel <端口>          启动控制面 IPC（ws://127.0.0.1:<端口>/ws）\n"
                 "  --ui                    启动时拉起 FaceGaze Bridge 窗口（隐含 --panel 8787）\n"
                 "  --ui-dir <目录>         页面资源目录（默认 <exe>/ui）\n"
                 "  --calibration <文件>    应用校准档案（见 calibrate 命令）\n"
                 "  --filter <on|off>       一欧元滤波（默认 off，与 VRCFT 一致）\n"
                 "  --filter-cutoff <值>    滤波最小截止频率 (默认 1.0)\n"
                 "  --filter-beta <值>      滤波速度系数 (默认 0.5)\n"
                 "  --vmc-port <端口>        VMC 监听端口 (默认 39539)\n"
                 "  --module-exe <路径>     VRCFaceTracking.ModuleProcess.exe 路径\n"
                 "  --module-dll <路径>     直接指定模块 DLL（跳过模块发现）\n"
                 "  --module-id <id>        按 id 或名字选模块，可重复（见 fgb-cli modules）\n"
                 "  --vrchat <host:port>    VRChat OSC 目标 (默认 127.0.0.1:9000)\n"
                 "  --vmc-out <host:port>   额外按 VMC 协议输出一份（如 127.0.0.1:39539）\n"
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

// VRCFT 模块宿主运行时：内置 <exe>/runtime/vrcft → 本机副本 → 已安装 VRCFT。
// 找不到就让用户用 --module-exe 指定（或者先导入模块/装一份运行时）。
std::filesystem::path default_module_process_exe() {
    const auto runtime = fgb::vrcft::find_vrcft_runtime();
    return runtime.has_value() ? runtime->exe : std::filesystem::path{};
}

// 导入候选目录：VRCFT 的 CustomLibs + 我们自己 exe 旁边的 vrcft-libs。
// 这两个都只是"可以从这里导入"，引擎不会直接从里面加载任何 DLL。
void add_default_import_roots(fgb::vrcft::ModuleRegistry& registry) {
    registry.add_import_root(fgb::vrcft::vrcft_module_root(), "VRCFT");
#ifdef _WIN32
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        registry.add_import_root(std::filesystem::path{buffer}.parent_path() / L"vrcft-libs",
                                 "FGB");
    }
#endif
}

std::string module_state_text(const fgb::vrcft::ModuleInfo& module) {
    if (module.broken) {
        return "缺少 DLL";
    }
    if (!module.enabled) {
        return "已停用";
    }
    if (module.active) {
        return "启动时加载";
    }
    return "已启用";
}

bool parse_host_port(std::string_view text, std::string& host, std::uint16_t& port) {    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        host = std::string(text);
        return !host.empty();
    }
    host = std::string(text.substr(0, colon));
    return !host.empty() && parse_uint16(text.substr(colon + 1), port);
}

// 页面资源随 exe 放在 ui/ 目录（构建时由 CMake 复制过去）。
// 找不到就不发页面：引擎照跑，只是界面空白——比整个启动失败要好。
std::filesystem::path default_ui_dir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const auto dir = std::filesystem::path{buffer}.parent_path() / L"ui";
        std::error_code ec;
        if (std::filesystem::exists(dir / L"index.html", ec)) {
            return dir;
        }
    }
#endif
    return {};
}

// 界面用独立进程起（WebView2 外壳崩一次不该带走追踪输出）。
// 引擎与 UI 之间只有 WS 一条线，谁先起来都行：外壳会自己重试导航。
bool launch_ui(std::uint16_t port) {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const auto dir = std::filesystem::path{buffer}.parent_path();
    std::error_code ec;
    // 发版后的外壳叫产品名；开发目录里可能还是旧的 fgb-ui.exe
    std::filesystem::path exe = dir / L"FaceGaze Bridge.exe";
    if (!std::filesystem::exists(exe, ec)) {
        exe = dir / L"fgb-ui.exe";
    }
    if (!std::filesystem::exists(exe, ec)) {
        std::fprintf(stderr, "找不到 FaceGaze Bridge.exe（和 fgb-cli 放在同一目录），界面已跳过\n");
        return false;
    }

    std::wstring command = L"\"" + exe.wstring() + L"\" --port " + std::to_wstring(port);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                         &startup, &process) == 0) {
        std::fprintf(stderr, "启动界面失败（错误码 %lu）\n", ::GetLastError());
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
#else
    (void)port;
    return false;
#endif
}

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) {
            out.push_back('+');
        }
        out.append(name);
    }
    return out;
}

int cmd_run(int argc, char** argv) {
    fgb::VmcSourceConfig vmc_cfg;
    fgb::ReplayConfig replay_cfg;
    std::string record_path;
    std::string calibration_path;
    bool filter_enabled   = false;
    bool open_ui          = false;
    std::uint16_t panel_port = 0;
    std::filesystem::path ui_dir;
    float filter_min_cutoff = 1.0f;
    float filter_beta       = 0.5f;
    fgb::VrchatSinkConfig vrchat_cfg;
    fgb::VmcSinkConfig vmc_out_cfg;
    bool vmc_out_enabled    = false;
    fgb::VmcSink* vmc_out_raw = nullptr;
    // 可重复出现，顺序即合并优先级：排前面的通道优先
    std::vector<std::string> source_kinds;
    // --module-id 同样可重复，按 vrcft 出现的顺序配对（两个模块各喂一路时用）
    std::vector<std::string> module_ids;
    std::string merge_expression_source;
    std::string merge_eye_source;
    std::string merge_head_source;
    int stale_ms = 500;
    std::filesystem::path module_exe;
    std::filesystem::path module_dll;
    std::string active_module_id;
    std::unique_ptr<fgb::vrcft::ModuleRegistry> module_registry;
    // 每个 vrcft 实例的展示信息：界面按实例显示运行态
    std::vector<fgb::web::RunningModule> running_modules;
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
                source_kinds.push_back(std::string(v));
            } else {
                std::fprintf(stderr, "未知的 source 类型\n");
                return 2;
            }
        } else if (arg == "--eye-source") {
            merge_eye_source = std::string{next_value(arg)};
        } else if (arg == "--expression-source") {
            merge_expression_source = std::string{next_value(arg)};
        } else if (arg == "--head-source") {
            merge_head_source = std::string{next_value(arg)};
        } else if (arg == "--stale-ms") {
            const auto v = next_value(arg);
            stale_ms       = std::atoi(std::string{v}.c_str());
            if (stale_ms <= 0) {
                std::fprintf(stderr, "--stale-ms 需要正数（毫秒）\n");
                return 2;
            }
        } else if (arg == "--panel") {
            const auto v = next_value(arg);
            std::uint16_t port = 0;
            if (v.empty() || !parse_uint16(v, port)) {
                std::fprintf(stderr, "%s\n", "--panel 需要一个端口号");
                return 2;
            }
            panel_port = port;
        } else if (arg == "--ui") {
            open_ui = true;
        } else if (arg == "--ui-dir") {
            const auto v = next_value(arg);
            if (v.empty()) {
                return 2;
            }
            ui_dir = std::filesystem::path{std::string{v}};
        } else if (arg == "--calibration") {
            calibration_path = std::string{next_value(arg)};
        } else if (arg == "--filter") {
            filter_enabled = (next_value(arg) != "off");
        } else if (arg == "--filter-cutoff") {
            filter_min_cutoff = static_cast<float>(std::atof(std::string{next_value(arg)}.c_str()));
        } else if (arg == "--filter-beta") {
            filter_beta = static_cast<float>(std::atof(std::string{next_value(arg)}.c_str()));
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
        } else if (arg == "--module-id") {
            const auto v = next_value(arg);
            if (!v.empty()) {
                module_ids.emplace_back(v);
            }
        } else if (arg == "--vmc-port") {
            const auto v = next_value(arg);
            if (v.empty() || !parse_uint16(v, vmc_cfg.port)) {
                return 2;
            }
        } else if (arg == "--vmc-out") {
            const auto v = next_value(arg);
            std::string host;
            std::uint16_t port = 0;
            if (v.empty() || !parse_host_port(v, host, port)) {
                std::fprintf(stderr, "--vmc-out 需要 host:port（例如 127.0.0.1:39539）\n");
                return 2;
            }
            vmc_out_cfg.host = host;
            vmc_out_cfg.port = port;
            vmc_out_enabled  = true;
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

    if (open_ui && panel_port == 0) {
        panel_port = 8787; // --ui 隐含控制面
    }

    // 界面上的模块管理不依赖输入源：即使是 VMC 模式，也能导入/安装/启停模块。
    if (panel_port != 0 && !module_registry) {
        module_registry = std::make_unique<fgb::vrcft::ModuleRegistry>();
        add_default_import_roots(*module_registry);
        module_registry->rescan();
    }

    fgb::EngineConfig engine_cfg;
    engine_cfg.tick_hz = tick_hz;
    fgb::Engine engine{engine_cfg};

    if (source_kinds.empty()) {
        source_kinds.emplace_back("vmc"); // 没写 --source 时的默认行为，与之前一致
    }

    std::vector<std::string> source_names;
    source_names.reserve(source_kinds.size());
    std::size_t vrcft_seen = 0;
    const auto vrcft_total = static_cast<std::size_t>(
        std::count(source_kinds.begin(), source_kinds.end(), std::string{"vrcft"}));

    for (const std::string& kind : source_kinds) {
        if (kind == "vrcft") {
            const std::size_t index = vrcft_seen++;
            // 多实例时给个能区分又不能有歧义的名字，--eye-source 就是拿它匹配的
            const std::string display =
                vrcft_total > 1 ? "vrcft[" + std::to_string(index + 1) + "]" : "vrcft";

            if (module_exe.empty()) {
                module_exe = default_module_process_exe();
                if (module_exe.empty()) {
                    std::fprintf(stderr,
                                 "没有找到 VRCFT 运行时（VRCFaceTracking.ModuleProcess.exe）。\n"
                                 "  发布包应带 runtime\\vrcft；开发环境可以安装一份 VRCFT，\n"
                                 "  或用 --module-exe 指定 ModuleProcess.exe 的路径。\n");
                    if (panel_port != 0) {
                        FGB_WARN("vrcft", "跳过 vrcft 输入：没有运行时；可在界面里安装运行时或改用 VMC");
                        continue;
                    }
                    return 2;
                }
            }
            // 模块选择交给 registry：命令行 > 配置文件 > 只有一个是它
            if (!module_registry) {
                module_registry = std::make_unique<fgb::vrcft::ModuleRegistry>();
                add_default_import_roots(*module_registry);
                module_registry->rescan();
            }

            std::filesystem::path instance_dll = module_dll;
            std::string instance_module_id    = index < module_ids.size() ? module_ids[index] : std::string{};
            std::string instance_module_name;

            if (instance_dll.empty()) {
                const fgb::vrcft::ModuleInfo* module = module_registry->pick_active(instance_module_id);
                if (module == nullptr) {
                    std::fprintf(stderr, "没有确定要加载哪个模块（第 %zu 个 vrcft 输入）。\n", index + 1);
                    if (module_registry->modules().empty()) {
                        std::fprintf(stderr,
                                     "  模块仓是空的：运行 fgb-cli modules import-all 导入本机模块，\n"
                                     "  或在界面「模块」页里导入/从模块库安装。\n");
                        for (const auto& candidate : module_registry->importable()) {
                            std::fprintf(stderr, "    可导入：%-32s %s\n", candidate.info.name.c_str(),
                                         candidate.info.id.c_str());
                        }
                    } else {
                        std::fprintf(stderr, "  已安装：\n");
                        for (const auto& candidate : module_registry->modules()) {
                            std::fprintf(stderr, "    %-36s %s\n", candidate.name.c_str(),
                                         candidate.id.c_str());
                        }
                        std::fprintf(stderr,
                                     "  用 --module-id <id> 指定（可重复），或 fgb-cli modules use <id>\n");
                    }
                    if (panel_port != 0) {
                        // 界面模式：先让窗口起来，用户在「模块」页导入/启用后重启引擎
                        FGB_WARN("vrcft", "跳过 vrcft 输入：还没有选择模块，可在界面里导入或启用");
                        continue;
                    }
                    return 2;
                }
                if (module->broken) {
                    // 这里刻意用字符串拼接而不是 fprintf("%s")：MSVC 的 printf 格式检查遇到
                    // "多字节字符紧跟 %" 会把那个 % 当成 MBCS 尾字节吃掉，报 C4474「参数太多」，
                    // /WX 下直接编译失败。不跟编译器的这个 bug 较劲。
                    const std::string message = "模块「" + module->name + "」的目录里找不到 DLL\n  " +
                                                module->directory.string() + "\n";
                    std::fputs(message.c_str(), stderr);
                    if (panel_port != 0) {
                        continue;
                    }
                    return 2;
                }
                instance_dll         = module->dll;
                instance_module_id   = module->id;
                instance_module_name = module->name;
                FGB_INFO("vrcft", "%s 加载模块：%s (%s)", display.c_str(), module->name.c_str(),
                         module->version.c_str());
            } else {
                instance_module_name = instance_dll.stem().string();
                FGB_INFO("vrcft", "%s 加载模块（--module-dll 指定）：%s", display.c_str(),
                         instance_dll.string().c_str());
            }

            if (index == 0) {
                active_module_id = instance_module_id;
                // 只是让界面知道"当前用的是哪个"，不写配置：
                // 自动挑中一个不等于用户选了它，下次仍按原配置走。
                module_registry->mark_active(active_module_id);
            }

            fgb::vrcft::VrcftSourceConfig cfg;
            cfg.module_process_exe = module_exe;
            cfg.module_dll         = instance_dll;
            cfg.tick_hz            = tick_hz;
            cfg.display_name       = display;
            if (!record_path.empty()) {
                cfg.record_path = record_path;
            }
            auto vrcft_source = std::make_unique<fgb::vrcft::VrcftSource>(cfg);

            // 每个实例单独登记：界面按实例显示状态，而不是只显示第一个
            fgb::web::RunningModule running;
            running.display_name = display;
            running.module_id    = instance_module_id;
            running.module_name  = instance_module_name;
            running.source       = vrcft_source.get();
            running_modules.push_back(std::move(running));

            engine.add_source(std::move(vrcft_source));
            source_names.push_back(display);
        } else if (kind == "replay") {
            if (replay_cfg.path.empty()) {
                std::fprintf(stderr, "--source replay 需要 --replay <文件>\n");
                return 2;
            }
            engine.add_source(std::make_unique<fgb::ReplaySource>(replay_cfg));
            source_names.emplace_back("replay");
        } else {
            engine.add_source(std::make_unique<fgb::VmcSource>(vmc_cfg));
            source_names.emplace_back("vmc");
        }
    }

    {
        fgb::MergePolicy merge_policy;
        merge_policy.expression_source = merge_expression_source;
        merge_policy.eye_source        = merge_eye_source;
        merge_policy.head_source       = merge_head_source;
        merge_policy.stale_ns = static_cast<std::uint64_t>(stale_ms) * 1'000'000ULL;
        engine.set_merge_policy(merge_policy);

        auto name_or_auto = [](const std::string& name) {
            return name.empty() ? std::string{"auto"} : name;
        };
        FGB_INFO("cli", "输入源：%s（表情=%s 眼追=%s 头姿=%s，过期 %d ms）",
                 join_names(source_names).c_str(), name_or_auto(merge_expression_source).c_str(),
                 name_or_auto(merge_eye_source).c_str(), name_or_auto(merge_head_source).c_str(),
                 stale_ms);
    }
    // 后端处理顺序：先校准（把原始值拉到 0..1），再滤波（平滑已经校准过的信号）
    if (!calibration_path.empty()) {
        fgb::params::CalibrationProfile profile;
        if (fgb::params::load_profile(calibration_path, profile)) {
            FGB_INFO("cli", "校准档案：%s（%zu 个形状参与）", calibration_path.c_str(),
                     profile.active_shape_count());
            engine.add_processor(
                std::make_unique<fgb::params::CalibrationApplier>(std::move(profile)));
        } else {
            std::fprintf(stderr, "%s%s\n", "校准档案读取失败，继续但不做校准：",
                         calibration_path.c_str());
        }
    }
    // 滤波器总是挂上，用 enabled 开关：这样面板能在运行时开关，不用重启
    fgb::OneEuroConfig filter_cfg;
    filter_cfg.min_cutoff = filter_min_cutoff;
    filter_cfg.beta       = filter_beta;
    auto filter           = std::make_unique<fgb::FaceFilter>(filter_cfg);
    filter->set_enabled(filter_enabled);
    fgb::FaceFilter* filter_raw = filter.get();
    engine.add_processor(std::move(filter));
    if (filter_enabled) {
        FGB_INFO("cli", "滤波开启：minCutoff=%.2f beta=%.2f", filter_cfg.min_cutoff,
                 filter_cfg.beta);
    }

    auto sink        = std::make_unique<fgb::VrchatSink>(vrchat_cfg);
    fgb::VrchatSink* sink_raw = sink.get();
    engine.add_sink(std::move(sink));

    // 第二路输出：把同一帧按 VMC 协议发出去，给 VSeeFace / Unity 场景之类的消费者。
    // 默认不开——多播一路 UDP 是用户显式要求的行为，不该悄悄发生。
    if (vmc_out_enabled) {
        auto vmc_sink      = std::make_unique<fgb::VmcSink>(vmc_out_cfg);
        vmc_out_raw        = vmc_sink.get();
        engine.add_sink(std::move(vmc_sink));
    }
    fgb::clock::raise_timer_resolution();

    // 控制面先于引擎启动：这样"引擎启动 / 头像绑定 / 模块握手"这几行日志也能进 UI 的日志页。
    // 反过来的话，界面连上时这些事都已经发生过一遍，日志页永远是空的——
    // 而用户最先想知道的就是"刚才为什么没数据"。
    std::unique_ptr<fgb::web::IpcServer> ipc;
    if (panel_port != 0) {
        if (ui_dir.empty()) {
            ui_dir = default_ui_dir();
        }

        fgb::web::IpcContext ctx;
        ctx.engine      = &engine;
        ctx.sink        = sink_raw;
        ctx.vmc_out     = vmc_out_raw;
        ctx.filter      = filter_raw;
        ctx.source_name = join_names(source_names);
        ctx.params_mode = fgb::to_string(vrchat_cfg.params_mode);
        ctx.eye_mode    = fgb::to_string(vrchat_cfg.eye_mode);
        ctx.ui_dir      = ui_dir;
        ctx.quit        = &g_stop;
        ctx.restart_engine = &g_restart;

        ctx.modules          = module_registry.get();
        ctx.running_modules  = running_modules;
        ctx.active_module_id = active_module_id;
        if (ctx.active_module_id.empty() && module_registry) {
            ctx.active_module_id = module_registry->active_id();
        }

        ipc = std::make_unique<fgb::web::IpcServer>(std::move(ctx));
        std::string ipc_error;
        if (!ipc->start(panel_port, ipc_error)) {
            // 显式要了控制面却起不来（端口被占是最常见原因），继续跑只会变成一个
            // 没人看得见的后台引擎——桌面入口那边也无从判断，直接失败更诚实。
            std::fprintf(stderr, "%s%s\n", "控制面启动失败：", ipc_error.c_str());
            return 1;
        }
    }

    if (!engine.start()) {
        fgb::clock::restore_timer_resolution();
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#endif

    if (open_ui) {
        if (ipc) {
            launch_ui(ipc->port());
        } else {
            std::fprintf(stderr, "控制面没起来，界面已跳过\n");
        }
    }

    const std::uint64_t period    = 1'000'000'000ULL / static_cast<std::uint64_t>(tick_hz);
    const std::uint64_t started   = fgb::clock::now_ns();
    std::uint64_t next_tick       = started + period;
    std::uint64_t last_report     = started;

    std::uint32_t ipc_counter = 0;
    while (!g_stop.load(std::memory_order_acquire)) {
        engine.tick_once(fgb::clock::now_ns());

        // 每 4 个 tick 处理一次控制面（30 Hz 轮询 + 内部 10 Hz 推送）。
        // 放在主循环里而不是单独线程：引擎与面板共享同一份状态，零加锁。
        if (ipc && (++ipc_counter % 4U) == 0U) {
            ipc->pump(0);
        }

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
    if (g_restart.load(std::memory_order_acquire)) {
        FGB_INFO("cli", "按界面请求重启引擎（退出码 %d）", kExitRestartEngine);
        return kExitRestartEngine;
    }
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

// 从两份抓包推导校准档案：一份放松，一份尽量夸张。
//
// 之所以不让 calibrate 自己去录制，是不想在引擎循环里塞一个状态机 + 倒计时提示；
// 抓包已经是现成的，推导过程还能离线重跑。用户多敲一条命令，换来确定的流程。
int cmd_calibrate(int argc, char** argv) {
    std::string neutral;
    std::string express;
    std::string out = "calibration.ini";
    float gain_y    = 1.0f;

    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string_view {
            return (i + 1 < argc) ? std::string_view{argv[++i]} : std::string_view{};
        };
        if (arg == "--express") {
            express = std::string{next()};
        } else if (arg == "--out") {
            out = std::string{next()};
        } else if (arg == "--gain-y") {
            gain_y = static_cast<float>(std::atof(std::string{next()}.c_str()));
        } else if (arg == "--help" || arg == "-h") {
            std::printf("用法: fgb-cli calibrate <放松抓包> --express <夸张抓包> --out <档案>\n");
            return 0;
        } else if (!arg.empty() && arg[0] != '-') {
            neutral = std::string{arg};
        }
    }
    if (neutral.empty() || express.empty()) {
        std::fprintf(stderr, "%s\n",
                     "用法: fgb-cli calibrate <放松抓包> --express <夸张抓包> --out <档案>");
        return 2;
    }

    fgb::params::CalibrationProfile profile;
    std::string error;
    if (!fgb::params::derive_profile(neutral, express, profile, error)) {
        std::fprintf(stderr, "%s%s\n", "推导失败：", error.c_str());
        return 1;
    }
    profile.eye_gain_y = gain_y;
    profile.eye_gain_x = 1.0f;

    if (!fgb::params::save_profile(out, profile)) {
        std::fprintf(stderr, "%s%s\n", "写档案失败：", out.c_str());
        return 1;
    }
    std::printf("%s%zu%s%s\n", "校准档案已写入，", profile.active_shape_count(),
                " 个形状参与：", out.c_str());
    std::printf("%s\n", "用 --calibration 指定它来生效；觉得幅度太小可以改档案里的 gain。");
    return 0;
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

// 模块管理（命令行侧）。界面上也能做同样的事，这里保留一份是因为
// 排障时经常是"引擎起不来、界面还没有"，而模块选错正是起的来的第一大原因。
int cmd_modules(int argc, char** argv) {
    fgb::vrcft::ModuleRegistry registry;

    add_default_import_roots(registry);
    registry.rescan();

    if (argc >= 3) {
        const std::string_view action = argv[2];
        if (action == "use") {
            if (argc < 4) {
                std::fprintf(stderr, "用法: fgb-cli modules use <模块 id 或名字>|none\n");
                return 2;
            }
            const std::string_view key = argv[3];
            const bool cleared         = key == "none";
            if (!registry.set_active(cleared ? std::string_view{} : key)) {
                std::fprintf(stderr, "%s%.*s\n", "没有这个模块（或它被停用了）：",
                             static_cast<int>(key.size()), key.data());
                return 1;
            }
            std::printf("启动模块已设为：%s（重启引擎后生效）\n",
                        cleared ? "不加载" : registry.active_id().c_str());
            return 0;
        }
        if (action == "enable" || action == "disable") {
            if (argc < 4) {
                std::fprintf(stderr, "用法: fgb-cli modules %.*s <id 或名字>\n",
                             static_cast<int>(action.size()), action.data());
                return 2;
            }
            if (!registry.set_enabled(argv[3], action == "enable")) {
                std::fprintf(stderr, "%s%s\n", "没有这个模块：", argv[3]);
                return 1;
            }
            std::printf("%s%s%s\n", action == "enable" ? "已启用" : "已停用", "：", argv[3]);
            return 0;
        }
        if (action == "remove") {
            if (argc < 4) {
                std::fprintf(stderr, "用法: fgb-cli modules remove <id 或名字>\n");
                return 2;
            }
            std::string error;
            if (!registry.remove_module(argv[3], error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("已删除 %s\n", argv[3]);
            return 0;
        }
        if (action == "import") {
            if (argc < 4) {
                std::fprintf(stderr, "用法: fgb-cli modules import <id 或名字>\n");
                return 2;
            }
            std::string error;
            if (!registry.import_module(argv[3], error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("已导入 %s\n", argv[3]);
            return 0;
        }
        if (action == "import-all") {
            std::vector<std::string> ids;
            for (const auto& candidate : registry.importable()) {
                ids.push_back(candidate.info.id);
            }
            if (ids.empty()) {
                std::printf("没有待导入的模块\n");
                return 0;
            }
            std::size_t done = 0;
            for (const std::string& id : ids) {
                std::string error;
                if (registry.import_module(id, error)) {
                    ++done;
                } else {
                    std::fprintf(stderr, "%s%s%s%s\n", "导入 ", id.c_str(), " 失败：", error.c_str());
                }
            }
            std::printf("已导入 %zu / %zu 个模块\n", done, ids.size());
            return done == ids.size() ? 0 : 1;
        }
        if (action == "install") {
            if (argc < 4) {
                std::fprintf(stderr, "用法: fgb-cli modules install <模块.zip|模块.dll>\n");
                return 2;
            }
            std::string error;
            if (!registry.install_path(argv[3], error, "cli")) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            std::printf("已安装：%s\n", argv[3]);
            return 0;
        }
        std::fprintf(stderr, "未知子命令：%.*s\n", static_cast<int>(action.size()), action.data());
        return 2;
    }

    std::printf("模块仓：%s\n", registry.store_dir().string().c_str());
    std::printf("状态文件：%s\n", registry.state_path().string().c_str());
    const auto runtime = fgb::vrcft::find_vrcft_runtime();
    if (runtime.has_value()) {
        std::printf("VRCFT 运行时：%s\n", runtime->exe.string().c_str());
    } else {
        std::printf("VRCFT 运行时：未找到（A 层模块无法启动，先用 fgb-cli modules import 或装 VRCFT）\n");
    }
    std::printf("\n");

    if (registry.modules().empty()) {
        std::printf("模块仓是空的。\n");
    } else {
        std::printf("%-34s %-9s %-13s %-11s %s\n", "模块", "版本", "作者", "状态", "来源");
        std::printf("%s\n", std::string(82, '-').c_str());
        for (const auto& module : registry.modules()) {
            std::printf("%-34s %-9s %-13s %-11s %s\n", module.name.c_str(),
                        module.version.empty() ? "-" : module.version.c_str(),
                        module.author.empty() ? "-" : module.author.c_str(),
                        module_state_text(module).c_str(),
                        module.source.empty() ? "-" : module.source.c_str());
        }
    }

    if (!registry.importable().empty()) {
        std::printf("\n检测到本机有 %zu 个未导入模块：\n", registry.importable().size());
        for (const auto& candidate : registry.importable()) {
            std::printf("  %-34s %-9s [%s] %s\n", candidate.info.name.c_str(),
                        candidate.info.version.empty() ? "-" : candidate.info.version.c_str(),
                        candidate.origin.c_str(), candidate.info.id.c_str());
        }
        std::printf("  fgb-cli modules import <id>   或   fgb-cli modules import-all\n");
    }

    std::printf("\n切换：fgb-cli modules use <id>    启停：enable/disable <id>\n"
                "安装：install <.zip|.dll>       删除：remove <id>\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    const std::string_view cmd = argv[1];
    if (cmd == "run") {
        return cmd_run(argc, argv);
    }

    if (cmd == "modules") {
        return cmd_modules(argc, argv);
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
    if (cmd == "calibrate") {
        return cmd_calibrate(argc, argv);
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
