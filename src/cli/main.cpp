#include "base/clock.hpp"
#include "base/crash.hpp"
#include "base/diagnostics.hpp"
#include "base/log.hpp"
#include "engine/engine.hpp"
#include "capture/capture_file.hpp"
#include "config/osc_map.hpp"
#include "config/outputs.hpp"
#include "core/process/one_euro.hpp"
#include "net/update_check.hpp"
#include "openvr/steamvr.hpp"
#include "web/ipc.hpp"
#include "params/avatar_config.hpp"
#include "params/calibration.hpp"
#include "params/catalog.hpp"
#include "sinks/shm_sink.hpp"
#include "sinks/udp_binary_sink.hpp"
#include "sinks/vmc_sink.hpp"
#include "sinks/vrchat_sink.hpp"
#include "sinks/ws_json_sink.hpp"
#include "capture/capture_file.hpp"
#include "sources/livelink_source.hpp"
#include "sources/native_host_source.hpp"
#include "sources/native_source.hpp"
#include "sources/osc_source.hpp"
#include "sources/replay_source.hpp"
#include "sources/vmc_source.hpp"
#include "sources/vrcft_source.hpp"
#include "vrcft/module_registry.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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
                 "  fgb-cli diag bundle [--out <zip>]  导出诊断包（日志/配置/崩溃转储）\n"
                 "  fgb-cli update-check [--url <地址>] 检查 GitHub Releases 是否有新版本\n"
                 "  fgb-cli steamvr remove 移除 SteamVR 启动项注册（卸载前用）\n"
                 "  fgb-cli version        显示版本\n"
                 "\n"
                 "run 选项:\n"
                 "  --source <类型>         vmc | livelink | osc | vrcft | replay，可重复；顺序即优先级 (默认 vmc)\n"
                 "  --eye-source <名>       指定眼追来自哪个源（默认自动）\n"
                 "  --eye-left-source <名>  左眼单独选源（覆盖 --eye-source）\n"
                 "  --eye-right-source <名> 右眼单独选源（覆盖 --eye-source）\n"
                 "  --gaze-source <名>      指定渲染注视来自哪个源（默认自动）\n"
                 "  --gaze-left-source <名> 左眼注视单独选源\n"
                 "  --gaze-right-source <名> 右眼注视单独选源\n"
                 "  --avatar-eye-from-gaze  头像眼睛用 Gaze 方向，保留 Eyes 的 openness/pupil\n"
                 "  --attr-source <属性=源[,源]> 逐属性有序源，如 left-openness=A,B\n"
                 "                          属性：left/right-openness、left/right-pupil、\n"
                 "                          left/right-gaze、vergence\n"
                 "  --attr-fuse              属性列表里有多个新鲜源时按置信度加权平均\n"
                 "  --expression-source <名> 指定表情来自哪个源（默认自动）\n"
                 "  --head-source <名>      指定头姿来自哪个源（默认自动）\n"
                 "  --stale-ms <毫秒>       同时设置所有通道的过期阈值\n"
                 "  --expression-stale-ms <毫秒> 表情过期阈值 (默认 500)\n"
                 "  --eye-stale-ms <毫秒>   眼追过期阈值 (默认 250)\n"
                 "  --gaze-stale-ms <毫秒>  注视过期阈值 (默认 100)\n"
                 "  --head-stale-ms <毫秒>  头姿过期阈值 (默认 500)\n"
                 "  --record <文件>         把收到的 ReplyUpdate 原样录制（仅 vrcft）\n"
                 "  --replay <文件>         回放抓包（配合 --source replay）\n"
                 "  --replay-speed <倍率>   回放速度，0 = 不限 (默认 1.0)\n"
                 "  --replay-loop           回放到末尾后重新开始\n"
                 "  --panel <端口>          启动控制面 IPC（ws://127.0.0.1:<端口>/ws）\n"
                 "  --ui                    启动时拉起 FaceGaze Bridge 窗口（隐含 --panel 8787）\n"
                 "  --ui-dir <目录>         页面资源目录（默认 <exe>/ui）\n"
                 "  --calibration <文件>    应用校准档案（默认读 %%LOCALAPPDATA%%\\FaceGazeBridge\\calibration.ini）\n"
                 "  --filter <on|off>       一欧元滤波（默认 off，与 VRCFT 一致）\n"
                 "  --filter-cutoff <值>    滤波最小截止频率 (默认 1.0)\n"
                 "  --filter-beta <值>      滤波速度系数 (默认 0.5)\n"
                 "  --vmc-port <端口>        VMC 监听端口 (默认 39539)\n"
                 "  --livelink-port <端口>  ARKit LiveLink 监听端口 (默认 11111)\n"
                 "  --osc-in-port <端口>    通用 OSC 监听端口（默认读 osc_map.ini）\n"
                 "  --osc-map <文件>        OSC 地址→形状映射表（默认 %%LOCALAPPDATA%%\\FaceGazeBridge\\osc_map.ini）\n"
                 "  --osc-in-lan            通用 OSC 绑定所有网卡（默认只绑回环）\n"
                 "  --module-exe <路径>     VRCFaceTracking.ModuleProcess.exe 路径\n"
                 "  --module-dll <路径>     直接指定模块 DLL（跳过模块发现）\n"
                 "  --module-id <id>        按 id 或名字选模块，可重复（见 fgb-cli modules）\n"
                 "  --native-host           原生模块走独立宿主进程（崩溃隔离，默认关）\n"
                 "  --native-host-exe <路径> 指定 fgb-host-native.exe（默认 exe 旁边）\n"
                 "  --no-host-restart       宿主崩溃后不自动重启（默认最多重试 3 次）\n"
                 "  --vrchat <host:port>    VRChat OSC 目标 (默认 127.0.0.1:9000)\n"
                 "  --vmc-out <host:port>   额外按 VMC 协议输出一份（如 127.0.0.1:39539）\n"
                 "  --shm <名字>            共享内存输出（Local\\FGB_Shm_<名字>，给 Unity/UE 读取）\n"
                 "  --ws-out <端口>         WebSocket JSON 输出（本机回环，ws://127.0.0.1:<端口>/ws）\n"
                 "  --ws-out-rate <hz>      WS JSON 帧率，0 = 每 tick (默认 30)\n"
                 "  --ws-out-all            发全部 88 个形状（默认只发 > 0.01 的有效形状）\n"
                 "  --udp-out <host:port>   UDP 二进制输出（一帧一个数据报）\n"
                 "  --udp-out-rate <hz>     UDP 二进制帧率，0 = 每 tick (默认 0)\n"
                 "  --outputs-config <路径> 输出设置文件（默认 %%LOCALAPPDATA%%\\FaceGazeBridge\\outputs.ini）\n"
                 "  --no-saved-outputs      本次忽略已保存的输出设置（命令行参数仍然生效）\n"
                 "  --eye-mode <模式>       parameters | native | both (默认 parameters)\n"
                 "  --params <参数集>       v2 | v1 | both (默认 v2)\n"
                 "  --param-prefix <前缀>   找不到头像配置时 v2 参数用的地址前缀\n"
                 "                          (默认 FT/v2/，因为多数头像这么命名)\n"
                 "  --avatar-config <路径>  指定头像参数表 JSON，不给则自动挑最新的\n"
                 "  --no-oscquery           关闭 OSCQuery 自动发现（只读本地头像 JSON）\n"
                 "  --oscquery-url <地址>   显式指定 OSCQuery 基地址（跳过 mDNS，如 127.0.0.1:65393）\n"
                 "  --avatar-poll-ms <毫秒> 轮询换头像的间隔，0 关闭 (默认 2000)\n"
                 "  --tick-hz <hz>          引擎 tick 频率 (默认 120)\n"
                 "  --duration <秒>         运行指定秒数后退出，0 为不限 (默认 0)\n"
                 "  --verbose               打开调试日志\n");
}

bool parse_attr_source(std::string_view text, fgb::AttributeSourceList& out,
                       std::string& error) {
    const std::size_t eq = text.find('=');
    if (eq == std::string_view::npos || eq == 0 || eq + 1 >= text.size()) {
        error = "格式应为 <属性>=<源1[,源2...]>";
        return false;
    }
    std::string attribute{text.substr(0, eq)};
    std::transform(attribute.begin(), attribute.end(), attribute.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::replace(attribute.begin(), attribute.end(), '_', '-');
    static constexpr std::string_view kAttributes[] = {
        "left-openness", "right-openness", "left-pupil", "right-pupil",
        "left-gaze",     "right-gaze",     "vergence",
    };
    const bool known = std::any_of(std::begin(kAttributes), std::end(kAttributes),
                                   [&attribute](std::string_view name) {
                                       return name == attribute;
                                   });
    if (!known) {
        error = "未知属性：" + attribute;
        return false;
    }

    std::vector<std::string> sources;
    std::string_view rest = text.substr(eq + 1);
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view part =
            comma == std::string_view::npos ? rest : rest.substr(0, comma);
        std::size_t begin = 0;
        std::size_t end   = part.size();
        while (begin < end && (part[begin] == ' ' || part[begin] == '\t')) {
            ++begin;
        }
        while (end > begin && (part[end - 1] == ' ' || part[end - 1] == '\t')) {
            --end;
        }
        if (end > begin) {
            sources.emplace_back(part.substr(begin, end - begin));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    if (sources.empty()) {
        error = "没有给出源名";
        return false;
    }
    out.attribute = std::move(attribute);
    out.sources   = std::move(sources);
    return true;
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

// app.vrmanifest 与产品 exe 都平铺在 fgb-cli 旁边（发布布局）。
std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path{buffer}.parent_path();
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
    fgb::LiveLinkSourceConfig livelink_cfg;
    std::uint16_t osc_port_override = 0;
    bool osc_lan                    = false;
    std::filesystem::path osc_map_path = fgb::config::default_osc_map_path();
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
    bool vmc_out_enabled      = false;
    fgb::VmcSink* vmc_out_raw = nullptr;

    fgb::WsJsonConfig ws_out_cfg;
    bool ws_out_enabled       = false;
    fgb::WsJsonSink* ws_out_raw = nullptr;

    fgb::UdpBinaryConfig udp_out_cfg;
    bool udp_out_enabled           = false;
    fgb::UdpBinarySink* udp_out_raw = nullptr;

    std::string shm_name;
    fgb::ShmSink* shm_out_raw = nullptr;
    // 可重复出现，顺序即合并优先级：排前面的通道优先
    std::vector<std::string> source_kinds;
    // --module-id 同样可重复，按 vrcft 出现的顺序配对（两个模块各喂一路时用）
    std::vector<std::string> module_ids;
    std::string merge_expression_source;
    std::string merge_eye_source;
    std::string merge_eye_left_source;
    std::string merge_eye_right_source;
    std::string merge_gaze_source;
    std::string merge_gaze_left_source;
    std::string merge_gaze_right_source;
    std::string merge_head_source;
    bool avatar_eye_from_gaze = false;
    bool attr_fuse = false;
    std::vector<fgb::AttributeSourceList> attribute_sources;
    int expression_stale_ms = 500;
    int eye_stale_ms        = 250;
    int gaze_stale_ms       = 100;
    int head_stale_ms       = 500;
    std::filesystem::path module_exe;
    std::filesystem::path module_dll;
    bool native_host = false;
    bool host_auto_restart = true;
    std::filesystem::path native_host_exe;
    int native_host_counter = 0;
    std::string active_module_id;
    std::unique_ptr<fgb::vrcft::ModuleRegistry> module_registry;
    // 每个 vrcft 实例的展示信息：界面按实例显示运行态
    std::vector<fgb::web::RunningModule> running_modules;
    int tick_hz      = 120;
    int duration_sec = 0;
    bool verbose     = false;

    // 输出设置先于命令行加载：命令行永远覆盖保存值。--no-saved-outputs 需要
    // 在正式解析前就知道，所以这里先扫一遍（值只用于这两个开关）。
    fgb::config::OutputOptions saved_outputs;
    std::filesystem::path outputs_path = fgb::config::default_outputs_path();
    std::filesystem::path outputs_profiles_dir = fgb::config::default_profiles_dir();
    bool apply_saved_outputs           = true;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--no-saved-outputs") {
            apply_saved_outputs = false;
        } else if (arg == "--outputs-config" && i + 1 < argc) {
            outputs_path = argv[++i];
        }
    }

    std::string outputs_error;
    const bool outputs_loaded =
        !outputs_path.empty() && fgb::config::load_outputs(outputs_path, saved_outputs, outputs_error);
    if (!outputs_loaded && !outputs_error.empty() && !outputs_path.empty() &&
        std::filesystem::exists(outputs_path)) {
        FGB_WARN("cli", "输出设置读取失败（%s），本次使用默认值", outputs_error.c_str());
    }
    if (apply_saved_outputs && outputs_loaded) {
        vrchat_cfg.host    = saved_outputs.vrchat_host;
        vrchat_cfg.port    = saved_outputs.vrchat_port;
        vrchat_cfg.enabled = saved_outputs.vrchat_enabled;
        if (!vrchat_cfg.enabled) {
            FGB_INFO("cli", "VRChat OSC 输出已按保存设置关闭（头像参数发现保留）");
        }
        fgb::EyeOutputMode eye_mode;
        if (fgb::parse_eye_output_mode(saved_outputs.eye_mode, eye_mode)) {
            vrchat_cfg.eye_mode = eye_mode;
        }
        fgb::ParamsMode params_mode;
        if (fgb::parse_params_mode(saved_outputs.params_mode, params_mode)) {
            vrchat_cfg.params_mode = params_mode;
        }

        vmc_out_enabled  = saved_outputs.vmc_enabled;
        vmc_out_cfg.host = saved_outputs.vmc_host;
        vmc_out_cfg.port = saved_outputs.vmc_port;

        if (saved_outputs.shm_enabled) {
            shm_name = saved_outputs.shm_name;
        }

        ws_out_enabled = saved_outputs.ws_enabled;
        ws_out_cfg.port = saved_outputs.ws_port;
        ws_out_cfg.min_interval_ns =
            saved_outputs.ws_rate_hz == 0 ? 0 : 1'000'000'000ULL / saved_outputs.ws_rate_hz;
        ws_out_cfg.all_shapes = saved_outputs.ws_all_shapes;

        udp_out_enabled = saved_outputs.udp_enabled;
        udp_out_cfg.host = saved_outputs.udp_host;
        udp_out_cfg.port = saved_outputs.udp_port;
        udp_out_cfg.min_interval_ns =
            saved_outputs.udp_rate_hz == 0 ? 0 : 1'000'000'000ULL / saved_outputs.udp_rate_hz;
    }

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
            if (v == "vmc" || v == "vrcft" || v == "replay" || v == "osc" || v == "livelink") {
                source_kinds.push_back(std::string(v));
            } else {
                std::fprintf(stderr, "未知的 source 类型（vmc | livelink | osc | vrcft | replay）\n");
                return 2;
            }
        } else if (arg == "--eye-source") {
            merge_eye_source = std::string{next_value(arg)};
        } else if (arg == "--eye-left-source") {
            merge_eye_left_source = std::string{next_value(arg)};
        } else if (arg == "--eye-right-source") {
            merge_eye_right_source = std::string{next_value(arg)};
        } else if (arg == "--gaze-source") {
            merge_gaze_source = std::string{next_value(arg)};
        } else if (arg == "--gaze-left-source") {
            merge_gaze_left_source = std::string{next_value(arg)};
        } else if (arg == "--gaze-right-source") {
            merge_gaze_right_source = std::string{next_value(arg)};
        } else if (arg == "--avatar-eye-from-gaze") {
            avatar_eye_from_gaze = true;
        } else if (arg == "--attr-fuse") {
            attr_fuse = true;
        } else if (arg == "--attr-source") {
            fgb::AttributeSourceList entry;
            std::string error;
            if (!parse_attr_source(next_value(arg), entry, error)) {
                std::fputs("--attr-source 无效：", stderr);
                std::fputs(error.c_str(), stderr);
                std::fputc('\n', stderr);
                return 2;
            }
            const auto existing = std::find_if(
                attribute_sources.begin(), attribute_sources.end(),
                [&entry](const fgb::AttributeSourceList& item) {
                    return item.attribute == entry.attribute;
                });
            if (existing != attribute_sources.end()) {
                existing->sources = std::move(entry.sources);
            } else {
                attribute_sources.push_back(std::move(entry));
            }
        } else if (arg == "--expression-source") {
            merge_expression_source = std::string{next_value(arg)};
        } else if (arg == "--head-source") {
            merge_head_source = std::string{next_value(arg)};
        } else if (arg == "--stale-ms") {
            const auto v = next_value(arg);
            const int value = std::atoi(std::string{v}.c_str());
            if (value <= 0) {
                std::fprintf(stderr, "--stale-ms 需要正数（毫秒）\n");
                return 2;
            }
            expression_stale_ms = value;
            eye_stale_ms        = value;
            gaze_stale_ms       = value;
            head_stale_ms       = value;
        } else if (arg == "--expression-stale-ms" || arg == "--eye-stale-ms" ||
                   arg == "--gaze-stale-ms" || arg == "--head-stale-ms") {
            const auto v = next_value(arg);
            const int value = std::atoi(std::string{v}.c_str());
            if (value <= 0) {
                std::fprintf(stderr, "%.*s 需要正数（毫秒）\n", static_cast<int>(arg.size()),
                             arg.data());
                return 2;
            }
            if (arg == "--expression-stale-ms") {
                expression_stale_ms = value;
            } else if (arg == "--eye-stale-ms") {
                eye_stale_ms = value;
            } else if (arg == "--gaze-stale-ms") {
                gaze_stale_ms = value;
            } else {
                head_stale_ms = value;
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
        } else if (arg == "--native-host") {
            native_host = true;
        } else if (arg == "--native-host-exe") {
            const auto v = next_value(arg);
            if (!v.empty()) {
                native_host_exe = std::filesystem::path{std::string{v}};
            }
        } else if (arg == "--no-host-restart") {
            host_auto_restart = false;
        } else if (arg == "--vmc-port") {
            const auto v = next_value(arg);
            if (v.empty() || !parse_uint16(v, vmc_cfg.port)) {
                return 2;
            }
        } else if (arg == "--livelink-port") {
            const auto v = next_value(arg);
            if (v.empty() || !parse_uint16(v, livelink_cfg.port)) {
                return 2;
            }
        } else if (arg == "--osc-in-port") {
            const auto v = next_value(arg);
            if (v.empty() || !parse_uint16(v, osc_port_override)) {
                return 2;
            }
        } else if (arg == "--osc-map") {
            const auto v = next_value(arg);
            if (v.empty()) {
                std::fprintf(stderr, "--osc-map 需要一个文件路径\n");
                return 2;
            }
            osc_map_path = std::string{v};
        } else if (arg == "--osc-in-lan") {
            osc_lan = true;
        } else if (arg == "--shm") {
            const auto v = next_value(arg);
            if (v.empty()) {
                std::fprintf(stderr, "--shm 需要一个名字（映射名会是 Local\\FGB_Shm_<名字>）\n");
                return 2;
            }
            shm_name = std::string{v};
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
        } else if (arg == "--ws-out") {
            const auto v = next_value(arg);
            std::uint16_t port = 0;
            if (v.empty() || !parse_uint16(v, port)) {
                std::fprintf(stderr, "--ws-out 需要端口（例如 9101，0 表示系统分配）\n");
                return 2;
            }
            ws_out_cfg.port = port;
            ws_out_enabled  = true;
        } else if (arg == "--ws-out-rate") {
            const auto v = next_value(arg);
            std::uint16_t hz = 0;
            if (v.empty() || !parse_uint16(v, hz)) {
                return 2;
            }
            ws_out_cfg.min_interval_ns = hz == 0 ? 0 : 1'000'000'000ULL / hz;
        } else if (arg == "--ws-out-all") {
            ws_out_cfg.all_shapes = true;
        } else if (arg == "--udp-out") {
            const auto v = next_value(arg);
            std::string host;
            std::uint16_t port = 0;
            if (v.empty() || !parse_host_port(v, host, port)) {
                std::fprintf(stderr, "--udp-out 需要 host:port（例如 127.0.0.1:9102）\n");
                return 2;
            }
            udp_out_cfg.host = host;
            udp_out_cfg.port = port;
            udp_out_enabled  = true;
        } else if (arg == "--udp-out-rate") {
            const auto v = next_value(arg);
            std::uint16_t hz = 0;
            if (v.empty() || !parse_uint16(v, hz)) {
                return 2;
            }
            udp_out_cfg.min_interval_ns = hz == 0 ? 0 : 1'000'000'000ULL / hz;
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
        } else if (arg == "--no-saved-outputs") {
            // 预扫描阶段已经处理
        } else if (arg == "--outputs-config") {
            if (next_value(arg).empty()) {
                return 2;
            }
        } else if (arg == "--outputs-profiles") {
            const auto v = next_value(arg);
            if (v.empty()) {
                return 2;
            }
            outputs_profiles_dir = fgb::config::path_from_utf8(v);
        } else if (arg == "--no-oscquery") {
            vrchat_cfg.use_oscquery = false;
        } else if (arg == "--oscquery-url") {
            const auto v = next_value(arg);
            if (v.empty()) {
                return 2;
            }
            vrchat_cfg.oscquery_url    = std::string{v};
            vrchat_cfg.use_oscquery    = true;
            vrchat_cfg.discover_avatar = true;
        } else if (arg == "--avatar-poll-ms") {
            const auto v = next_value(arg);
            std::uint16_t ms = 0;
            if (v.empty() || !parse_uint16(v, ms)) {
                return 2;
            }
            vrchat_cfg.avatar_poll_ms = ms;
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

    // 通用 OSC 输入的映射表只在真的选了该源时读取/生成；不选就不碰用户文件
    fgb::OscSourceConfig osc_cfg;
    if (std::find(source_kinds.begin(), source_kinds.end(), std::string{"osc"}) !=
        source_kinds.end()) {
        fgb::config::OscMapOptions map;
        std::string map_error;
        const bool loaded = fgb::config::load_osc_map(osc_map_path, map, map_error);
        if (!loaded) {
            std::error_code ec;
            if (std::filesystem::exists(osc_map_path, ec)) {
                std::fprintf(stderr, "OSC 映射表读取失败 (%s): %s\n", osc_map_path.string().c_str(),
                             map_error.c_str());
                return 2;
            }
            // 第一次用：生成一份空表，用户照着注释填；不生成的话连端口都不知道去哪儿改
            map.port = osc_port_override != 0 ? osc_port_override : map.port;
            if (!fgb::config::save_osc_map(osc_map_path, map, map_error)) {
                std::fprintf(stderr, "无法创建 OSC 映射表 (%s): %s\n",
                             osc_map_path.string().c_str(), map_error.c_str());
                return 2;
            }
            FGB_WARN("osc", "已生成空的映射表 %s；填好「地址 = 形状」后重启引擎",
                     osc_map_path.string().c_str());
        }
        osc_cfg.mappings      = std::move(map.mappings);
        osc_cfg.port          = osc_port_override != 0 ? osc_port_override : map.port;
        osc_cfg.any_interface = osc_lan;
    }

    std::vector<std::string> source_names;
    source_names.reserve(source_kinds.size());
    std::size_t vrcft_seen = 0;
    bool instances_consumed = false;
    const auto vrcft_total = static_cast<std::size_t>(
        std::count(source_kinds.begin(), source_kinds.end(), std::string{"vrcft"}));

    struct ModuleLaunch {
        std::filesystem::path dll;
        std::string module_id;
        std::string module_name;
        std::string instance_name;
        bool native = false;
        bool host_process = false;
    };

    auto launch_module = [&](const ModuleLaunch& launch, const std::string& display) -> bool {
        // 原生模块可以走独立宿主进程（崩溃隔离），否则走进程内 C ABI。
        if (launch.native && (launch.host_process || native_host)) {
            const std::filesystem::path host_exe =
                native_host_exe.empty() ? (executable_dir() / L"fgb-host-native.exe")
                                        : native_host_exe;
            std::error_code ec;
            if (std::filesystem::exists(host_exe, ec)) {
                const int index = ++native_host_counter;
                std::filesystem::path log_dir = fgb::config::default_outputs_path().parent_path();
                if (log_dir.empty()) {
                    log_dir = executable_dir();
                }
                fgb::NativeHostConfig host_cfg;
                host_cfg.host_exe    = host_exe;
                host_cfg.module_dll  = launch.dll;
                host_cfg.log_path    = log_dir / "logs" /
                                       ("native-host-" + std::to_string(index) + ".log");
                host_cfg.module_id   = launch.module_id;
                host_cfg.module_name = launch.module_name;
                host_cfg.display_name = display;
                host_cfg.shm_name    = "module-" + std::to_string(index);
                host_cfg.tick_hz     = tick_hz;
                host_cfg.auto_restart = host_auto_restart;
                if (!record_path.empty()) {
                    FGB_WARN("native-host", "宿主模式暂不支持 --record，已忽略");
                }
                auto host_source = std::make_unique<fgb::NativeHostSource>(host_cfg);
                FGB_INFO("native-host", "%s 使用宿主进程：%s", display.c_str(),
                         host_exe.string().c_str());

                fgb::web::RunningModule running;
                running.display_name = display;
                running.module_id    = launch.module_id;
                running.module_name  = launch.module_name;
                running.host_process = true;
                running.source       = host_source.get();
                running_modules.push_back(std::move(running));

                engine.add_source(std::move(host_source));
                source_names.push_back(display);
                return true;
            }
            FGB_WARN("native-host", "找不到 %s，%s 回退进程内加载", host_exe.string().c_str(),
                     display.c_str());
        }

        if (launch.native) {
            fgb::NativeSourceConfig cfg;
            cfg.dll          = launch.dll;
            cfg.module_id    = launch.module_id;
            cfg.module_name  = launch.module_name;
            cfg.display_name = display;
            cfg.tick_hz      = tick_hz;
            if (!record_path.empty()) {
                FGB_WARN("native", "原生模块暂不支持 --record，已忽略");
            }
            auto native_source = std::make_unique<fgb::NativeModuleSource>(cfg);
            FGB_INFO("native", "%s 加载原生模块：%s (%s)", display.c_str(),
                     launch.module_name.c_str(), launch.module_id.c_str());

            fgb::web::RunningModule running;
            running.display_name = display;
            running.module_id    = launch.module_id;
            running.module_name  = launch.module_name;
            running.source       = native_source.get();
            running_modules.push_back(std::move(running));

            engine.add_source(std::move(native_source));
            source_names.push_back(display);
            return true;
        }

        if (module_exe.empty()) {
            module_exe = default_module_process_exe();
            if (module_exe.empty()) {
                std::fprintf(stderr,
                             "没有找到 VRCFT 运行时（VRCFaceTracking.ModuleProcess.exe）。\n"
                             "  发布包应带 runtime\\vrcft；开发环境可以安装一份 VRCFT，\n"
                             "  或用 --module-exe 指定 ModuleProcess.exe 的路径。\n");
                return false;
            }
        }

        fgb::vrcft::VrcftSourceConfig cfg;
        cfg.module_process_exe = module_exe;
        cfg.module_dll         = launch.dll;
        cfg.tick_hz            = tick_hz;
        cfg.display_name       = display;
        if (!record_path.empty()) {
            cfg.record_path = record_path;
        }
        auto vrcft_source = std::make_unique<fgb::vrcft::VrcftSource>(cfg);
        FGB_INFO("vrcft", "%s 加载模块：%s", display.c_str(), launch.module_name.c_str());

        // 每个实例单独登记：界面按实例显示状态，而不是只显示第一个
        fgb::web::RunningModule running;
        running.display_name = display;
        running.module_id    = launch.module_id;
        running.module_name  = launch.module_name;
        running.source       = vrcft_source.get();
        running_modules.push_back(std::move(running));

        engine.add_source(std::move(vrcft_source));
        source_names.push_back(display);
        return true;
    };

    for (const std::string& kind : source_kinds) {
        if (kind == "vrcft") {
            const std::size_t index = vrcft_seen++;

            // 模块选择交给 registry：命令行 > 配置文件 > 只有一个是它
            if (!module_registry) {
                module_registry = std::make_unique<fgb::vrcft::ModuleRegistry>();
                add_default_import_roots(*module_registry);
                module_registry->rescan();
            }

            const bool explicit_id =
                index < module_ids.size() && !module_ids[index].empty();

            // 配置了 [instances] 且没有逐条 --module-id：一次把列表里的实例都拉起来。
            // 只消费一次，避免重复 --source vrcft 时把同一批实例装两遍。
            if (module_dll.empty() && !explicit_id && !instances_consumed &&
                !module_registry->instances().empty()) {
                instances_consumed = true;
                bool any_started = false;
                for (const fgb::vrcft::ModuleInstance& instance : module_registry->instances()) {
                    const fgb::vrcft::ModuleInfo* module = module_registry->find(instance.module_id);
                    if (module == nullptr) {
                        FGB_WARN("modules", "实例 %s 指向未安装的模块 %s，已跳过",
                                 instance.name.c_str(), instance.module_id.c_str());
                        continue;
                    }
                    if (!module->enabled) {
                        FGB_WARN("modules", "实例 %s 的模块 %s 已停用，已跳过", instance.name.c_str(),
                                 module->name.c_str());
                        continue;
                    }
                    if (module->broken) {
                        FGB_WARN("modules", "实例 %s 的模块 %s 缺少 DLL，已跳过", instance.name.c_str(),
                                 module->name.c_str());
                        continue;
                    }
                    const bool native = module->kind == fgb::vrcft::ModuleKind::native;
                    const std::string display =
                        (native ? "native:" : "vrcft:") + instance.name;
                    if (active_module_id.empty()) {
                        active_module_id = module->id;
                        module_registry->mark_active(active_module_id);
                    }
                    const ModuleLaunch launch{module->dll, module->id, module->name, instance.name,
                                              native, instance.host_process};
                    if (launch_module(launch, display)) {
                        any_started = true;
                    } else if (panel_port == 0) {
                        return 2;
                    }
                }
                if (!any_started) {
                    if (panel_port != 0) {
                        FGB_WARN("modules", "配置的实例没有一个能启动；可在界面里检查模块状态");
                        continue;
                    }
                    std::fprintf(stderr, "配置的实例没有一个能启动（见上面的原因）\n");
                    return 2;
                }
                continue;
            }

            std::filesystem::path instance_dll = module_dll;
            std::string instance_module_id    = explicit_id ? module_ids[index] : std::string{};
            std::string instance_module_name;
            bool is_native = false;

            if (instance_dll.empty()) {
                const fgb::vrcft::ModuleInfo* module = module_registry->pick_active(instance_module_id);
                if (module == nullptr) {
                    std::fprintf(stderr, "没有确定要加载哪个模块（第 %zu 个输入）。\n", index + 1);
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
                        FGB_WARN("modules", "跳过模块输入：还没有选择模块，可在界面里导入或启用");
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
                is_native            = module->kind == fgb::vrcft::ModuleKind::native;
            } else {
                instance_module_name = instance_dll.stem().string();
            }

            if (index == 0) {
                active_module_id = instance_module_id;
                // 只是让界面知道"当前用的是哪个"，不写配置：
                // 自动挑中一个不等于用户选了它，下次仍按原配置走。
                module_registry->mark_active(active_module_id);
            }

            // 多实例时给个能区分又不能有歧义的名字，--eye-source 就是拿它匹配的
            const std::string display =
                is_native ? (vrcft_total > 1 ? "native[" + std::to_string(index + 1) + "]"
                                             : "native")
                          : (vrcft_total > 1 ? "vrcft[" + std::to_string(index + 1) + "]"
                                             : "vrcft");
            const ModuleLaunch launch{instance_dll, instance_module_id, instance_module_name,
                                      std::string{}, is_native, native_host};
            if (!launch_module(launch, display) && panel_port == 0) {
                return 2;
            }
        } else if (kind == "replay") {
            if (replay_cfg.path.empty()) {
                std::fprintf(stderr, "--source replay 需要 --replay <文件>\n");
                return 2;
            }
            engine.add_source(std::make_unique<fgb::ReplaySource>(replay_cfg));
            source_names.emplace_back("replay");
        } else if (kind == "livelink") {
            engine.add_source(std::make_unique<fgb::LiveLinkSource>(livelink_cfg));
            source_names.emplace_back("livelink");
        } else if (kind == "osc") {
            engine.add_source(std::make_unique<fgb::OscSource>(osc_cfg));
            source_names.emplace_back("osc");
        } else {
            engine.add_source(std::make_unique<fgb::VmcSource>(vmc_cfg));
            source_names.emplace_back("vmc");
        }
    }

    {
        // modules.ini 的 [roles] 用 module_id 绑定；这里翻译成实际实例名。
        // 命令行显式 --*-source 永远优先，配置只是默认值。
        if (module_registry) {
            const auto lower = [](std::string text) {
                std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return text;
            };
            const auto resolve_role = [&](const std::string& value) -> std::string {
                if (value.empty()) {
                    return {};
                }
                const std::string needle = lower(value);
                for (const fgb::web::RunningModule& module : running_modules) {
                    if (lower(module.module_id) == needle) {
                        return module.display_name;
                    }
                }
                for (const std::string& name : source_names) {
                    if (lower(name) == needle) {
                        return name;
                    }
                }
                FGB_WARN("modules",
                         "角色配置指向 %s，但它没有在本次运行的实例中；该通道回退到自动选择",
                         value.c_str());
                return {};
            };
            if (merge_expression_source.empty()) {
                merge_expression_source = resolve_role(module_registry->roles().expression);
            }
            if (merge_eye_source.empty()) {
                merge_eye_source = resolve_role(module_registry->roles().eyes);
            }
            if (merge_gaze_source.empty()) {
                merge_gaze_source = resolve_role(module_registry->roles().gaze);
            }
            if (merge_head_source.empty()) {
                merge_head_source = resolve_role(module_registry->roles().head);
            }
        }

        fgb::MergePolicy merge_policy;
        merge_policy.expression_source = merge_expression_source;
        merge_policy.eye_source        = merge_eye_source;
        merge_policy.eye_left_source   = merge_eye_left_source;
        merge_policy.eye_right_source  = merge_eye_right_source;
        merge_policy.gaze_source       = merge_gaze_source;
        merge_policy.gaze_left_source  = merge_gaze_left_source;
        merge_policy.gaze_right_source = merge_gaze_right_source;
        merge_policy.head_source       = merge_head_source;
        merge_policy.avatar_eye_from_gaze = avatar_eye_from_gaze;
        merge_policy.attribute_sources   = attribute_sources;
        merge_policy.fuse_attributes     = attr_fuse;
        merge_policy.expression_stale_ns =
            static_cast<std::uint64_t>(expression_stale_ms) * 1'000'000ULL;
        merge_policy.eye_stale_ns = static_cast<std::uint64_t>(eye_stale_ms) * 1'000'000ULL;
        merge_policy.gaze_stale_ns = static_cast<std::uint64_t>(gaze_stale_ms) * 1'000'000ULL;
        merge_policy.head_stale_ns = static_cast<std::uint64_t>(head_stale_ms) * 1'000'000ULL;
        engine.set_merge_policy(merge_policy);

        auto name_or_auto = [](const std::string& name) {
            return name.empty() ? std::string{"auto"} : name;
        };
        const std::string eye_left_name = name_or_auto(
            merge_eye_left_source.empty() ? merge_eye_source : merge_eye_left_source);
        const std::string eye_right_name = name_or_auto(
            merge_eye_right_source.empty() ? merge_eye_source : merge_eye_right_source);
        const std::string gaze_left_name = name_or_auto(
            merge_gaze_left_source.empty() ? merge_gaze_source : merge_gaze_left_source);
        const std::string gaze_right_name = name_or_auto(
            merge_gaze_right_source.empty() ? merge_gaze_source : merge_gaze_right_source);
        FGB_INFO("cli",
                 "输入源：%s（表情=%s 眼追=%s/%s 注视=%s/%s 头姿=%s，头像注视=%s，"
                 "过期 %d/%d/%d/%d ms）",
                 join_names(source_names).c_str(), name_or_auto(merge_expression_source).c_str(),
                 eye_left_name.c_str(), eye_right_name.c_str(), gaze_left_name.c_str(),
                 gaze_right_name.c_str(), name_or_auto(merge_head_source).c_str(),
                 avatar_eye_from_gaze ? "gaze" : "eyes", expression_stale_ms, eye_stale_ms,
                 gaze_stale_ms, head_stale_ms);
        if (!attribute_sources.empty()) {
            FGB_INFO("cli", "逐属性源：%zu 组（%s，见 --attr-source）", attribute_sources.size(),
                     attr_fuse ? "多源加权" : "首个可用");
        }
    }
    // 校准链路的顺序：取样（校准前的原始值）→ 校准 → 滤波。
    // 取样只服务于界面预览，默认关闭；校准与滤波才是真正改变输出的两步。
    fgb::params::PreviewTap* preview_raw = nullptr;
    if (panel_port != 0) {
        auto preview   = std::make_unique<fgb::params::PreviewTap>();
        preview_raw    = preview.get();
        engine.add_processor(std::move(preview));
    }

    const bool calibration_explicit = !calibration_path.empty();
    std::string calibration_load_path = calibration_path;
    if (!calibration_explicit) {
        // 默认档案：界面保存到这里，下次启动自动生效（--calibration 显式指定时优先）
        const auto fallback = fgb::params::default_calibration_path();
        std::error_code ec;
        if (std::filesystem::exists(fallback, ec)) {
            calibration_load_path = fallback.string();
        }
    }

    fgb::params::CalibrationProfile calibration_profile;
    bool calibration_loaded = false;
    if (!calibration_load_path.empty()) {
        if (fgb::params::load_profile(calibration_load_path, calibration_profile)) {
            calibration_loaded = true;
            FGB_INFO("cli", "校准档案：%s（%zu 个形状参与）", calibration_load_path.c_str(),
                     calibration_profile.active_shape_count());
        } else if (calibration_explicit) {
            std::fprintf(stderr, "%s%s\n", "校准档案读取失败，继续但不做校准：",
                         calibration_load_path.c_str());
        } else {
            FGB_WARN("calib", "默认校准档案读取失败，按不做校准继续：%s",
                     calibration_load_path.c_str());
        }
    }

    // 面板模式下即使没有档案也挂一个（全透传）：否则用户打开校准页时没有可写的 profile，
    // 只能重启引擎才能开始校准（ADR-0021 §热更新）
    fgb::params::CalibrationApplier* calibration_raw = nullptr;
    {
        auto applier = std::make_unique<fgb::params::CalibrationApplier>(calibration_profile);
        calibration_raw = applier.get();
        engine.add_processor(std::move(applier));
    }
    // 界面保存的目标：显式 --calibration 优先，否则写默认档案
    const std::string calibration_save_path =
        calibration_explicit ? calibration_path : fgb::params::default_calibration_path().string();
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

    // 共享内存输出：默认不开，--shm <name> 显式要求才建映射
    if (!shm_name.empty()) {
        fgb::ShmSinkConfig shm_cfg;
        shm_cfg.name = shm_name;
        auto shm_sink = std::make_unique<fgb::ShmSink>(shm_cfg);
        shm_out_raw   = shm_sink.get();
        engine.add_sink(std::move(shm_sink));
    }

    // WebSocket JSON / UDP 二进制：都是显式开启；WS 只绑回环，UDP 目标由用户给
    if (ws_out_enabled) {
        ws_out_cfg.source_name = source_names.empty() ? "fgb" : join_names(source_names);
        auto ws_sink = std::make_unique<fgb::WsJsonSink>(ws_out_cfg);
        ws_out_raw   = ws_sink.get();
        engine.add_sink(std::move(ws_sink));
    }
    if (udp_out_enabled) {
        auto udp_sink = std::make_unique<fgb::UdpBinarySink>(udp_out_cfg);
        udp_out_raw   = udp_sink.get();
        engine.add_sink(std::move(udp_sink));
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
        ctx.shm_out     = shm_out_raw;
        ctx.ws_out      = ws_out_raw;
        ctx.udp_out     = udp_out_raw;
        ctx.filter      = filter_raw;
        ctx.source_name  = join_names(source_names);
        ctx.source_names = source_names;
        ctx.params_mode  = fgb::to_string(vrchat_cfg.params_mode);
        ctx.eye_mode     = fgb::to_string(vrchat_cfg.eye_mode);
        ctx.outputs      = saved_outputs;
        ctx.outputs_path = outputs_path;
        ctx.outputs_profiles_dir = outputs_profiles_dir;
        ctx.ui_dir      = ui_dir;
        ctx.exe_dir     = executable_dir();
        ctx.quit        = &g_stop;
        ctx.restart_engine = &g_restart;
        ctx.preview     = preview_raw;
        ctx.calib       = calibration_raw;
        ctx.calib_path  = calibration_save_path;
        ctx.calib_loaded = calibration_loaded;

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
            fgb::vrcft::InstallOutcome outcome;
            // replace=true：同 ID 的新包按升级处理，和界面的"安装压缩包"一致
            if (!registry.install_path(argv[3], error, "cli", true, nullptr, &outcome)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            if (outcome.upgraded) {
                std::printf("已升级：%s %s → %s\n",
                            outcome.name.empty() ? outcome.id.c_str() : outcome.name.c_str(),
                            outcome.previous_version.empty() ? "?" : outcome.previous_version.c_str(),
                            outcome.version.empty() ? "?" : outcome.version.c_str());
            } else {
                std::printf("已安装：%s\n", argv[3]);
            }
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
        std::printf("%-30s %-7s %-9s %-13s %-11s %s\n", "模块", "类型", "版本", "作者",
                    "状态", "来源");
        std::printf("%s\n", std::string(82, '-').c_str());
        for (const auto& module : registry.modules()) {
            const char* kind = module.kind == fgb::vrcft::ModuleKind::native ? "native" : "vrcft";
            std::printf("%-30s %-7s %-9s %-13s %-11s %s\n", module.name.c_str(), kind,
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

int cmd_diag(int argc, char** argv) {
    fgb::diagnostics::BundleRequest request;
    request.exe_dir = executable_dir();
    request.version = FGB_VERSION;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--out" && i + 1 < argc) {
            request.output = std::filesystem::path{argv[++i]};
        }
    }

    if (request.output.empty()) {
        request.output = fgb::diagnostics::default_bundle_path();
    }
    std::string error;
    if (!fgb::diagnostics::build_bundle(request, error)) {
        std::fprintf(stderr, "导出诊断包失败：%s\n", error.c_str());
        return 1;
    }
    std::printf("诊断包：%s\n", request.output.string().c_str());
    return 0;
}

int cmd_update_check(int argc, char** argv) {
    std::string url = fgb::update::kDefaultUpdateUrl;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            url = argv[++i];
        }
    }

    fgb::update::ReleaseInfo release;
    std::string error;
    if (!fgb::update::check_latest(url, release, error)) {
        std::fprintf(stderr, "检查更新失败：%s\n", error.c_str());
        return 1;
    }
    const int compare = fgb::update::compare_versions(release.tag, FGB_VERSION);
    // 中文/全角标点紧跟 %s 会触发 MSVC C4474（见 AGENT 已知坑），先拼成 string
    std::string summary = "当前版本：" + std::string{FGB_VERSION} + "\n最新版本：" + release.tag +
                          (compare > 0 ? "（有新版本）" : "（已是最新）") + "\n";
    if (!release.name.empty()) {
        summary += "发布标题：" + release.name + "\n";
    }
    if (!release.url.empty()) {
        summary += "下载页：" + release.url + "\n";
    }
    std::fputs(summary.c_str(), stdout);
    return 0;
}

int cmd_steamvr(int argc, char** argv) {
    if (argc < 3 || std::string_view{argv[2]} != "remove") {
        std::fprintf(stderr, "用法：fgb-cli steamvr remove\n");
        return 2;
    }
    fgb::steamvr::Manager manager(executable_dir());
    const fgb::steamvr::Status status = manager.remove_registration();
    if (!status.ok) {
        const std::string message =
            "移除 SteamVR 注册失败：" + status.reason + " " + status.detail + "\n";
        std::fputs(message.c_str(), stderr);
        return 1;
    }
    const std::string message = "已移除 SteamVR 启动项注册：" + status.app_key + "\n";
    std::fputs(message.c_str(), stdout);
    return 0;
}

int main(int argc, char** argv) {
    // 所有子命令都装：模块加载崩了、参数表生成崩了，同样要留下转储
    fgb::crash::install(fgb::crash::default_crash_dir(), FGB_VERSION);

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
    if (cmd == "diag") {
        return cmd_diag(argc, argv);
    }
    if (cmd == "update-check") {
        return cmd_update_check(argc, argv);
    }
    if (cmd == "steamvr") {
        return cmd_steamvr(argc, argv);
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
