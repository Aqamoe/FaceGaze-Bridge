#include "web/ipc.hpp"

#include "base/diagnostics.hpp"
#include "base/json.hpp"
#include "base/log.hpp"
#include "net/update_check.hpp"
#include "params/avatar_config.hpp"
#include "params/calibration.hpp"
#include "params/catalog.hpp"
#include "params/preview_tap.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef FGB_VERSION
#define FGB_VERSION "0.0.0"
#endif

namespace fgb::web {

using json::JsonWriter;

namespace {

constexpr char kTag[] = "ipc";

// 模块库的安装相位从"下载中/安装中"落到"空闲/失败"就是这次装完了。
// 成功与否看落到哪一端，界面和服务端用同一条判断，谁也不猜。
bool install_phase_finished(const std::string& previous, const std::string& current) {
    const bool was_busy = previous == "downloading" || previous == "installing";
    const bool done     = current == "idle" || current == "error";
    return was_busy && done;
}

// 实例在启动配置里、但引擎启动时还没加载（用户刚添加）：和"名字根本不存在"
// 分开说，否则界面只剩一句 unknown instance。
std::string instance_missing_error(const vrcft::ModuleRegistry* registry,
                                   const std::string& name) {
    if (registry != nullptr) {
        const auto& instances = registry->instances();
        if (std::any_of(instances.begin(), instances.end(),
                        [&name](const vrcft::ModuleInstance& instance) {
                            return instance.name == name;
                        })) {
            return "实例「" + name + "」尚未加载（新增实例需重启引擎后生效）";
        }
    }
    return "未知实例：" + name;
}

// 模块页链接来自 registry 或 module.json，界面只负责展示。这里再回查一遍元数据，
// 避免页面脚本被改坏后把一条 ws 命令变成"用默认浏览器打开任意 URL"。
bool known_module_page(const vrcft::ModuleRegistry* registry, const vrcft::ModuleLibrary* library,
                       const vrcft::NativeLibrary* native_library, std::string_view url) {
    if (url.empty() || url.size() > 2048) {
        return false;
    }
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return false;
    }
    const auto matches = [url](const std::string& candidate) {
        return !candidate.empty() && candidate == url;
    };
    if (registry != nullptr) {
        for (const vrcft::ModuleInfo& module : registry->modules()) {
            if (matches(module.page_url)) {
                return true;
            }
        }
    }
    if (library != nullptr) {
        const vrcft::LibrarySnapshot snapshot = library->snapshot();
        for (const vrcft::LibraryEntry& entry : snapshot.entries) {
            if (matches(entry.page)) {
                return true;
            }
        }
    }
    if (native_library != nullptr) {
        const vrcft::NativeLibrarySnapshot snapshot = native_library->snapshot();
        for (const vrcft::NativeEntry& entry : snapshot.registry.modules) {
            if (matches(entry.homepage) || matches(entry.icon) || matches(entry.latest.release_url)) {
                return true;
            }
        }
    }
    return false;
}

bool percent_decode(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size());
    const auto hex = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (i + 2 >= text.size()) {
            return false;
        }
        const int high = hex(text[i + 1]);
        const int low  = hex(text[i + 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return true;
}

bool open_in_browser(std::string_view url) {
#ifdef _WIN32
    const int length =
        ::MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), wide.data(),
                          length);
    const auto result = reinterpret_cast<INT_PTR>(
        ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
    (void)url;
    return false;
#endif
}

// 推送节奏。10 Hz 对人眼足够（热力图看不出 100 ms 的台阶），
// 又远低于引擎 120 Hz，不会把主循环的时间预算吃掉。
constexpr auto kStatusPeriod = std::chrono::milliseconds(100);
constexpr auto kParamsPeriod = std::chrono::milliseconds(100);
// 注视点比热力图需要更细的时间分辨率：扫视只有几十毫秒
constexpr auto kGazePeriod = std::chrono::milliseconds(33);
// 校准页的面部动画需要比热力图更顺：100 ms 台阶在动态表情上肉眼可见（ADR-0021）
constexpr auto kPreviewPeriod = std::chrono::milliseconds(33);

// 日志积压上限：UI 没连上时日志不该无限吃内存
constexpr std::size_t kMaxLogBacklog = 256;
// 新连接补发的历史行数：够看清"刚才为什么没数据"，又不至于把面板刷满
constexpr std::size_t kRecentLogCount = 120;

const char* level_name(log::Level level) noexcept {
    switch (level) {
    case log::Level::trace:
        return "trace";
    case log::Level::debug:
        return "debug";
    case log::Level::info:
        return "info";
    case log::Level::warn:
        return "warn";
    case log::Level::error:
        return "error";
    default:
        return "off";
    }
}

bool parse_level(std::string_view text, log::Level& out) noexcept {
    if (text == "trace") {
        out = log::Level::trace;
    } else if (text == "debug") {
        out = log::Level::debug;
    } else if (text == "info") {
        out = log::Level::info;
    } else if (text == "warn") {
        out = log::Level::warn;
    } else if (text == "error") {
        out = log::Level::error;
    } else if (text == "off") {
        out = log::Level::off;
    } else {
        return false;
    }
    return true;
}

std::string_view mime_type(std::string_view extension) noexcept {
    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".js" || extension == ".mjs") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".json") {
        return "application/json; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".woff2") {
        return "font/woff2";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    if (extension == ".fgbface") {
        // 模型二进制：页面 fetch 后交给 WebGL，不需要浏览器理解内容
        return "application/octet-stream";
    }
    return {}; // 未知类型一律不发，避免把配置文件、二进制当静态资源漏出去
}

// set_outputs 与保存预设共用：payload 里缺省的字段保持原值
bool parse_output_patch(std::string_view text, config::OutputOptions& next, std::string& error) {
    std::string target;
    std::string mode;
    bool flag     = false;
    double number = 0.0;

    if (json::field_string(text, "vrchat", target) &&
        !config::parse_target(target, next.vrchat_host, next.vrchat_port)) {
        error = "vrchat 需要 host:port";
    }
    if (error.empty() && json::field_string(text, "eye_mode", mode)) {
        next.eye_mode = mode;
    }
    if (error.empty() && json::field_string(text, "params_mode", mode)) {
        next.params_mode = mode;
    }
    if (error.empty() && json::field_bool(text, "vrchat_enabled", flag)) {
        next.vrchat_enabled = flag;
    }
    if (error.empty() && json::field_bool(text, "vmc_enabled", flag)) {
        next.vmc_enabled = flag;
    }
    if (error.empty() && json::field_string(text, "vmc_target", target) &&
        !config::parse_target(target, next.vmc_host, next.vmc_port)) {
        error = "vmc_target 需要 host:port";
    }
    if (error.empty() && json::field_bool(text, "shm_enabled", flag)) {
        next.shm_enabled = flag;
    }
    if (error.empty() && json::field_string(text, "shm_name", mode)) {
        next.shm_name = mode;
    }
    if (error.empty() && json::field_bool(text, "ws_enabled", flag)) {
        next.ws_enabled = flag;
    }
    if (error.empty() && json::field_number(text, "ws_port", number)) {
        if (number < 1 || number > 65535) {
            error = "ws_port 必须是 1..65535";
        } else {
            next.ws_port = static_cast<std::uint16_t>(number);
        }
    }
    if (error.empty() && json::field_number(text, "ws_rate", number)) {
        if (number < 0 || number > 1000) {
            error = "ws_rate 必须在 0..1000";
        } else {
            next.ws_rate_hz = static_cast<int>(number);
        }
    }
    if (error.empty() && json::field_bool(text, "ws_all", flag)) {
        next.ws_all_shapes = flag;
    }
    if (error.empty() && json::field_bool(text, "udp_enabled", flag)) {
        next.udp_enabled = flag;
    }
    if (error.empty() && json::field_string(text, "udp_target", target) &&
        !config::parse_target(target, next.udp_host, next.udp_port)) {
        error = "udp_target 需要 host:port";
    }
    if (error.empty() && json::field_number(text, "udp_rate", number)) {
        if (number < 0 || number > 1000) {
            error = "udp_rate 必须在 0..1000";
        } else {
            next.udp_rate_hz = static_cast<int>(number);
        }
    }
    return error.empty();
}

std::string make_log_message(int level, std::string_view tag, std::string_view message) {
    std::string out;
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("log");
    writer.key("level").str(level_name(static_cast<log::Level>(level)));
    writer.key("tag").str(tag);
    writer.key("message").str(message);
    writer.obj_end();
    return out;
}

bool is_v2_group(const params::Group& group) noexcept {
    return group.name_prefix.rfind("v2/", 0) == 0;
}

bool mode_allows(const std::string& params_mode, bool v2) noexcept {
    if (params_mode == "both") {
        return true;
    }
    return v2 ? params_mode != "v1" : params_mode == "v1";
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(bytes - 1));
    return out;
}
#endif


} // namespace

IpcServer::IpcServer(IpcContext context) : ctx_(std::move(context)) {
    if (ctx_.modules != nullptr) {
        library_ = std::make_unique<vrcft::ModuleLibrary>(ctx_.modules);
        library_->load_cache();
        // 6 小时内不重复联网：启动时先用缓存，过期了才在后台刷新
        library_->refresh_if_stale(std::chrono::hours(6));

        // 原生模块库是另一份索引（registry schema v2），独立缓存与刷新节奏
        native_library_ = std::make_unique<vrcft::NativeLibrary>(ctx_.modules);
        native_library_->load_cache();
        native_library_->refresh_if_stale(std::chrono::hours(6));
    }

    // SteamVR 卡片只在发布布局（app.vrmanifest 与 exe 同目录）下可用；
    // 命令行开发实例没有 exe_dir 时直接不挂服务，界面显示"不可用"。
    if (!ctx_.exe_dir.empty()) {
        steamvr_ = std::make_unique<steamvr::Service>(ctx_.exe_dir);
    }

    // 自动更新（ADR-0036）：数据目录取不到就整个功能不启用，界面显示不可用，
    // 而不是让人对着"检查更新"一直转圈。
    if (const std::filesystem::path data_dir = update::default_data_dir(); !data_dir.empty()) {
        updater_ = std::make_unique<update::Manager>(data_dir, ctx_.exe_dir, FGB_VERSION,
                                                     ctx_.update_assume_installed);
        // 启动 10 秒后再查：模块握手与首帧远比"有没有新版本"重要
        auto_update_due_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }

    // 启动时读到的档案就是"磁盘上的版本"：界面据此判断哪些改动还没保存
    if (ctx_.calib != nullptr) {
        saved_profile_ = ctx_.calib->profile();
        calib_loaded_  = ctx_.calib_loaded;
    }

    server_.on_open = [this](std::intptr_t conn) {
        FGB_DEBUG(kTag, "UI 已连接");
        std::string message;
        build_hello(message);
        server_.send_text(conn, message);

        if (catalog_dirty_) {
            refresh_catalog();
        }
        server_.send_text(conn, catalog_json_);

        build_modules(message);
        server_.send_text(conn, message);

        if (library_) {
            build_library(message);
            server_.send_text(conn, message);
        }

        build_status(message);
        server_.send_text(conn, message);
        build_params(message);
        server_.send_text(conn, message);
        build_gaze(message);
        server_.send_text(conn, message);
        build_calibration(message);
        server_.send_text(conn, message);
        build_steamvr(message);
        server_.send_text(conn, message);
        // 更新状态也补发一次：用户可能"下载完就关窗口"，再打开时要看到"可安装"
        build_update(message);
        server_.send_text(conn, message);
        build_output_profiles(message);
        server_.send_text(conn, message);
        send_backlog(conn);
    };

    server_.on_close = [this](std::intptr_t) {
        FGB_DEBUG(kTag, "UI 已断开");
        // 页面走了就停取样：可能还有人连着（第二个页面/探针），所以只看连接数
        if (server_.ready_count() == 0) {
            preview_wanted_ = false;
            if (ctx_.preview != nullptr) {
                ctx_.preview->set_enabled(false);
            }
        }
    };

    server_.on_text = [this](std::intptr_t conn, std::string_view text) {
        handle_text(conn, text);
    };

    server_.on_http = [this](const net::ws::HttpHead& head, std::string& body,
                             std::string& content_type) {
        return handle_http(head, body, content_type);
    };
}

IpcServer::~IpcServer() {
    if (diag_thread_.joinable()) {
        diag_thread_.join(); // 先等后台打包结束，避免它和 stop() 抢 server_
    }
    updater_.reset(); // 更新线程：析构里 join，别让它在 server_ 停掉后还往外发
    stop();
}

bool IpcServer::start(std::uint16_t port, std::string& error) {
    if (!server_.listen(port, error)) {
        return false;
    }

    // 日志推送给 UI。回调里只入队，真正发出去在 pump()——日志线程（模块宿主等）
    // 不该因为 UI 的连接状态而阻塞。
    log::set_callback([this](log::Level level, std::string_view tag, std::string_view message) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        LogLine line{static_cast<int>(level), std::string{tag}, std::string{message}};
        if (logs_.size() >= kMaxLogBacklog) {
            logs_.pop_front();
        }
        logs_.push_back(line);
        if (recent_logs_.size() >= kRecentLogCount) {
            recent_logs_.pop_front();
        }
        recent_logs_.push_back(std::move(line));
    });

    FGB_INFO(kTag, "控制面 IPC：ws://127.0.0.1:%u/ws", static_cast<unsigned>(server_.port()));
    if (!ctx_.ui_dir.empty()) {
        FGB_INFO(kTag, "界面页面：http://127.0.0.1:%u/（%s）", static_cast<unsigned>(server_.port()),
                 ctx_.ui_dir.string().c_str());
    }
    return true;
}

void IpcServer::stop() {
    log::set_callback({});
    if (library_) {
        library_->stop();
    }
    if (native_library_) {
        native_library_->stop();
    }
    server_.close();
}

void IpcServer::refresh_catalog() {
    build_catalog(catalog_json_);
    catalog_dirty_ = false;
}

void IpcServer::build_hello(std::string& out) const {
    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("hello");
    writer.key("protocol").integer(1);
    writer.key("app").str("FaceGazeBridge");
    writer.key("version").str(FGB_VERSION);
    writer.key("port").integer(static_cast<long long>(server_.port()));
    writer.obj_end();
}

void IpcServer::build_catalog(std::string& out) {
    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("catalog");

    // 快照一次：引擎线程可能正在因为换头像重绑 router
    const std::vector<std::string> addresses =
        ctx_.sink != nullptr ? ctx_.sink->bound_addresses() : std::vector<std::string>{};
    std::size_t index = 0;

    writer.key("names").arr_begin();
    for (const params::Group& group : params::groups()) {
        for (const params::ParamEntry& entry : group.entries) {
            std::string name{group.name_prefix};
            name.append(entry.name);
            writer.str(name);
            ++index;
        }
    }
    writer.arr_end();

    index = 0;
    writer.key("sent").arr_begin();
    for (const params::Group& group : params::groups()) {
        const bool allowed = mode_allows(ctx_.params_mode, is_v2_group(group));
        for (const params::ParamEntry& entry : group.entries) {
            (void)entry;
            const bool bound = index < addresses.size() && !addresses[index].empty();
            writer.boolean(allowed && bound);
            ++index;
        }
    }
    writer.arr_end();

    writer.key("count").integer(static_cast<long long>(index));
    writer.obj_end();
}

void IpcServer::build_metrics(std::string& out) {
    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();

    const EngineStats stats = ctx_.engine != nullptr ? ctx_.engine->stats() : EngineStats{};

    writer.key("source").str(ctx_.source_name);
    writer.key("source_ok").boolean(stats.frames_in > 0);
    writer.key("frames_in").integer(static_cast<long long>(stats.frames_in));
    writer.key("ticks").integer(static_cast<long long>(stats.ticks));
    writer.key("late_ticks").integer(static_cast<long long>(stats.late_ticks));
    writer.key("resumes").integer(static_cast<long long>(stats.resumes));
    writer.key("fps").num(last_fps_, 1);

    // tick 工作时间：看引擎主循环本身有没有被某一路输出拖慢
    const TickStats::Snapshot work =
        ctx_.engine != nullptr ? ctx_.engine->tick_stats() : TickStats::Snapshot{};
    writer.key("tick_work");
    writer.obj_begin();
    writer.key("avg_us").num(work.avg_us, 3);
    writer.key("p50_us").num(work.p50_us, 3);
    writer.key("p95_us").num(work.p95_us, 3);
    writer.key("max_us").num(work.max_us, 3);
    writer.key("samples").integer(static_cast<long long>(work.samples));
    writer.obj_end();

    // 进程资源 1 Hz 采样：状态页要看到长跑里有没有内存/句柄增长
    const auto now = std::chrono::steady_clock::now();
    if (last_process_sample_.time_since_epoch().count() == 0 ||
        now - last_process_sample_ >= std::chrono::seconds(1)) {
        last_process_        = process_sampler_.sample();
        last_process_sample_ = now;
    }
    writer.key("process");
    writer.obj_begin();
    writer.key("cpu_percent").num(last_process_.cpu_percent, 1);
    writer.key("working_set_mb").num(last_process_.working_set_mb, 1);
    writer.key("handles").integer(static_cast<long long>(last_process_.handles));
    writer.obj_end();

    const RouterStatus router_status =
        ctx_.sink != nullptr ? ctx_.sink->status() : RouterStatus{};
    if (ctx_.sink != nullptr) {
        const SinkStats sink_stats = ctx_.sink->stats();
        writer.key("messages").integer(static_cast<long long>(sink_stats.messages));
        writer.key("bytes").integer(static_cast<long long>(sink_stats.bytes));
    } else {
        writer.key("messages").integer(0);
        writer.key("bytes").integer(0);
    }
    writer.key("avatar_name").str(router_status.avatar_name);
    writer.key("avatar_id").str(router_status.avatar_id);
    writer.key("avatar_source").str(router_status.avatar_source);
    writer.key("bound_to_avatar").boolean(router_status.bound);
    writer.key("param_count").integer(static_cast<long long>(router_status.param_count));
    writer.key("target_count").integer(static_cast<long long>(router_status.target_count));
    writer.key("binary_count").integer(static_cast<long long>(router_status.binary_count));
    writer.key("matched").integer(static_cast<long long>(router_status.matched));

    writer.key("params_mode").str(ctx_.params_mode);
    writer.key("eye_mode").str(ctx_.eye_mode);
    writer.key("clients").integer(static_cast<long long>(server_.ready_count()));

    // 每个模块实例的运行态。多实例时按 display_name 区分（与 --eye-source 用的是同一个名字）
    writer.key("modules").arr_begin();
    for (const RunningModule& module : ctx_.running_modules) {
        writer.obj_begin();
        writer.key("name").str(module.display_name);
        writer.key("module_id").str(module.module_id);
        writer.key("module_name").str(module.module_name);
        writer.key("state").str(module_state(module));

        // 模块自己报的错优先：它比"等待初始化超时"这类描述具体
        ModuleRuntimeStatus status;
        if (module.source != nullptr) {
            const auto runtime = module.source->module_status();
            if (runtime.has_value()) {
                status = *runtime;
            }
        }
        writer.key("error").str(status.error);
        writer.key("restarts").integer(static_cast<long long>(status.restarts));
        writer.key("alive").boolean(status.alive);
        writer.key("capabilities").integer(static_cast<long long>(status.capabilities));
        writer.key("host_process").boolean(module.host_process);
        writer.obj_end();
    }
    writer.arr_end();

    // 第二路输出（VMC）：界面上要能确认"确实发出去了多少"，否则联调只能靠猜
    writer.key("shm_out");
    writer.obj_begin();
    if (ctx_.shm_out != nullptr) {
        const SinkStats shm_stats = ctx_.shm_out->stats();
        writer.key("enabled").boolean(true);
        writer.key("name").str(ctx_.shm_out->mapping_name());
        writer.key("messages").integer(static_cast<long long>(shm_stats.messages));
        writer.key("bytes").integer(static_cast<long long>(shm_stats.bytes));
    } else {
        writer.key("enabled").boolean(false);
        writer.key("name").str("");
        writer.key("messages").integer(0);
        writer.key("bytes").integer(0);
    }
    writer.obj_end();

    writer.key("vmc_out");
    writer.obj_begin();
    if (ctx_.vmc_out != nullptr) {
        const SinkStats vmc_stats = ctx_.vmc_out->stats();
        writer.key("enabled").boolean(true);
        writer.key("target").str(ctx_.vmc_out->target());
        writer.key("messages").integer(static_cast<long long>(vmc_stats.messages));
        writer.key("bytes").integer(static_cast<long long>(vmc_stats.bytes));
    } else {
        writer.key("enabled").boolean(false);
        writer.key("target").str("");
        writer.key("messages").integer(0);
        writer.key("bytes").integer(0);
    }
    writer.obj_end();

    writer.key("ws_out");
    writer.obj_begin();
    if (ctx_.ws_out != nullptr) {
        const SinkStats ws_stats = ctx_.ws_out->stats();
        writer.key("enabled").boolean(true);
        writer.key("listening").boolean(ctx_.ws_out->listening());
        writer.key("target").str("ws://127.0.0.1:" + std::to_string(ctx_.ws_out->port()) + "/ws");
        writer.key("clients").integer(static_cast<long long>(ctx_.ws_out->clients()));
        writer.key("messages").integer(static_cast<long long>(ws_stats.messages));
        writer.key("bytes").integer(static_cast<long long>(ws_stats.bytes));
    } else {
        writer.key("enabled").boolean(false);
        writer.key("listening").boolean(false);
        writer.key("target").str("");
        writer.key("clients").integer(0);
        writer.key("messages").integer(0);
        writer.key("bytes").integer(0);
    }
    writer.obj_end();

    writer.key("udp_out");
    writer.obj_begin();
    if (ctx_.udp_out != nullptr) {
        const SinkStats udp_stats = ctx_.udp_out->stats();
        writer.key("enabled").boolean(true);
        writer.key("target").str(ctx_.udp_out->target());
        writer.key("messages").integer(static_cast<long long>(udp_stats.messages));
        writer.key("bytes").integer(static_cast<long long>(udp_stats.bytes));
    } else {
        writer.key("enabled").boolean(false);
        writer.key("target").str("");
        writer.key("messages").integer(0);
        writer.key("bytes").integer(0);
    }
    writer.obj_end();

    // 保存的输出设置（输出页表单用）。与上面的 *_out 是两回事：
    // 上面是"这次实际开了什么"，这里是"下次启动会按什么开"。
    writer.key("outputs");
    writer.obj_begin();
    writer.key("vrchat").str(
        config::format_target(ctx_.outputs.vrchat_host, ctx_.outputs.vrchat_port));
    writer.key("eye_mode").str(ctx_.outputs.eye_mode);
    writer.key("params_mode").str(ctx_.outputs.params_mode);
    writer.key("vrchat_enabled").boolean(ctx_.outputs.vrchat_enabled);
    writer.key("vmc_enabled").boolean(ctx_.outputs.vmc_enabled);
    writer.key("vmc_target").str(
        config::format_target(ctx_.outputs.vmc_host, ctx_.outputs.vmc_port));
    writer.key("shm_enabled").boolean(ctx_.outputs.shm_enabled);
    writer.key("shm_name").str(ctx_.outputs.shm_name);
    writer.key("ws_enabled").boolean(ctx_.outputs.ws_enabled);
    writer.key("ws_port").integer(static_cast<long long>(ctx_.outputs.ws_port));
    writer.key("ws_rate").integer(ctx_.outputs.ws_rate_hz);
    writer.key("ws_all").boolean(ctx_.outputs.ws_all_shapes);
    writer.key("udp_enabled").boolean(ctx_.outputs.udp_enabled);
    writer.key("udp_target").str(
        config::format_target(ctx_.outputs.udp_host, ctx_.outputs.udp_port));
    writer.key("udp_rate").integer(ctx_.outputs.udp_rate_hz);
    writer.obj_end();

    // 每个输入源的贡献：多源合并后，"谁在喂哪一路"必须看得见，
    // 否则用户只会看到"眼睛不动"，然后去怀疑滤波。
    writer.key("sources").arr_begin();
    if (ctx_.engine != nullptr) {
        for (const SourceReport& report : ctx_.engine->source_report()) {
            writer.obj_begin();
            writer.key("name").str(report.name);
            writer.key("frames").integer(static_cast<long long>(report.frames));
            writer.key("age_ms").num(static_cast<double>(report.age_ns) / 1'000'000.0);
            writer.key("fresh").boolean(report.fresh);
            writer.key("expression").boolean(report.used_expression);
            writer.key("eyes").boolean(report.used_eyes);
            writer.key("gaze").boolean(report.used_gaze);
            writer.key("head").boolean(report.used_head);
            writer.key("eyes_left").boolean(report.used_eyes_left);
            writer.key("eyes_right").boolean(report.used_eyes_right);
            writer.key("gaze_left").boolean(report.used_gaze_left);
            writer.key("gaze_right").boolean(report.used_gaze_right);
            writer.key("has_expression").boolean(report.provides_expression);
            writer.key("has_eyes").boolean(report.provides_eyes);
            writer.key("has_gaze").boolean(report.provides_gaze);
            writer.key("has_head").boolean(report.provides_head);
            writer.obj_end();
        }
    }
    writer.arr_end();

    writer.key("filter");
    writer.obj_begin();
    if (ctx_.filter != nullptr) {
        writer.key("available").boolean(true);
        writer.key("enabled").boolean(ctx_.filter->enabled());
        writer.key("min_cutoff").num(ctx_.filter->min_cutoff(), 2);
        writer.key("beta").num(ctx_.filter->beta(), 2);
    } else {
        writer.key("available").boolean(false);
        writer.key("enabled").boolean(false);
        writer.key("min_cutoff").num(0.0, 2);
        writer.key("beta").num(0.0, 2);
    }
    writer.obj_end();

    writer.obj_end();
}

void IpcServer::build_status(std::string& out) {
    // 状态消息 = {"type":"status"} + 指标
    std::string metrics;
    build_metrics(metrics);
    out.assign(R"({"type":"status",)");
    out.append(metrics, 1, metrics.size() - 1); // 跳过 metrics 自己的 '{'
}

void IpcServer::build_gaze(std::string& out) {
    const FaceFrame frame = ctx_.engine != nullptr ? ctx_.engine->latest() : FaceFrame::neutral();
    const GazeState gaze  = ctx_.engine != nullptr ? ctx_.engine->current_gaze() : GazeState{};

    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("gaze");
    writer.key("ts_ns").integer(static_cast<long long>(gaze.ts_ns));
    writer.key("capture_ts_ns").integer(static_cast<long long>(gaze.capture_ts_ns));
    writer.key("valid").boolean(gaze.valid);
    // 眼原点还没有建模（FaceFrame 只带方向）；先固定 0，别假装有世界坐标
    writer.key("origin_local").arr_begin();
    writer.num(0.0, 3);
    writer.num(0.0, 3);
    writer.num(0.0, 3);
    writer.arr_end();
    writer.key("dir_local").arr_begin();
    writer.num(gaze.dir_local.x, 3);
    writer.num(gaze.dir_local.y, 3);
    writer.num(gaze.dir_local.z, 3);
    writer.arr_end();
    writer.key("left_valid").boolean(gaze.left_valid);
    writer.key("right_valid").boolean(gaze.right_valid);
    writer.key("left_dir_local").arr_begin();
    writer.num(gaze.left_dir_local.x, 3);
    writer.num(gaze.left_dir_local.y, 3);
    writer.num(gaze.left_dir_local.z, 3);
    writer.arr_end();
    writer.key("right_dir_local").arr_begin();
    writer.num(gaze.right_dir_local.x, 3);
    writer.num(gaze.right_dir_local.y, 3);
    writer.num(gaze.right_dir_local.z, 3);
    writer.arr_end();
    // 逐眼角度由引擎算好（与 dir_local 同一约定），客户端不要自己反算
    writer.key("left_pitch_deg").num(gaze.left_pitch_deg, 3);
    writer.key("left_yaw_deg").num(gaze.left_yaw_deg, 3);
    writer.key("right_pitch_deg").num(gaze.right_pitch_deg, 3);
    writer.key("right_yaw_deg").num(gaze.right_yaw_deg, 3);
    writer.key("point2d_valid").boolean(gaze.point2d_valid);
    // 逐眼点有效性：只给 2D 点的源 left/right_valid 为假，这几个字段才是真信号
    writer.key("left_point_valid").boolean(gaze.left_point_valid);
    writer.key("right_point_valid").boolean(gaze.right_point_valid);
    // 点的坐标系：viewport = 该眼视口归一化（ADR-0033，可直接用于 foveation）；
    // source = 来源自己的原始坐标，只能展示/看趋势
    writer.key("point_space").str(gaze.point_viewport ? "viewport" : "source");
    writer.key("left_point_2d").arr_begin();
    writer.num(gaze.left_point_2d.x, 4);
    writer.num(gaze.left_point_2d.y, 4);
    writer.arr_end();
    writer.key("right_point_2d").arr_begin();
    writer.num(gaze.right_point_2d.x, 4);
    writer.num(gaze.right_point_2d.y, 4);
    writer.arr_end();
    writer.key("pitch_deg").num(gaze.pitch_deg, 3);
    writer.key("yaw_deg").num(gaze.yaw_deg, 3);
    writer.key("vergence_distance").num(gaze.vergence_m, 3);
    writer.key("confidence").num(gaze.confidence, 3);
    writer.key("fixation").boolean(gaze.fixation);
    writer.key("fixation_ms").num(gaze.fixation_ms, 1);
    writer.key("saccade_vel_deg_s").num(gaze.saccade_vel_deg_s, 1);

    // 逐通道来源：状态页要显示"注视来自哪个实例、多久前"。源名在合并层的 report 里。
    std::string gaze_source;
    std::uint64_t gaze_age_ms = 0;
    if (ctx_.engine != nullptr) {
        for (const SourceReport& report : ctx_.engine->source_report()) {
            if (report.used_gaze) {
                gaze_source = report.name;
                gaze_age_ms = report.age_ns / 1'000'000ULL;
                break;
            }
        }
    }
    writer.key("source").str(gaze_source);
    writer.key("source_age_ms").integer(static_cast<long long>(gaze_age_ms));
    writer.key("source_id").integer(static_cast<long long>(gaze.source));
    writer.key("source_mask").integer(static_cast<long long>(gaze.source_mask));
    writer.key("left_source").integer(static_cast<long long>(gaze.left_source));
    writer.key("right_source").integer(static_cast<long long>(gaze.right_source));
    writer.key("left_capture_ts_ns")
        .integer(static_cast<long long>(gaze.left_capture_ts_ns));
    writer.key("right_capture_ts_ns")
        .integer(static_cast<long long>(gaze.right_capture_ts_ns));
    if (frame.hasHeadPose) {
        writer.key("head_pose").obj_begin();
        writer.key("q").arr_begin();
        writer.num(frame.headRotLocal.x, 3);
        writer.num(frame.headRotLocal.y, 3);
        writer.num(frame.headRotLocal.z, 3);
        writer.num(frame.headRotLocal.w, 3);
        writer.arr_end();
        writer.key("p").arr_begin();
        writer.num(frame.headPosLocal.x, 3);
        writer.num(frame.headPosLocal.y, 3);
        writer.num(frame.headPosLocal.z, 3);
        writer.arr_end();
        writer.obj_end();
    }
    writer.obj_end();
}

void IpcServer::build_params(std::string& out) {
    const FaceFrame frame = ctx_.engine != nullptr ? ctx_.engine->latest() : FaceFrame::neutral();
    const params::EvalContext eval{frame, 30.0f};

    out.clear();
    out.reserve(params::kMaxParamCount * 8);
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("params");
    writer.key("values").arr_begin();
    for (const params::Group& group : params::groups()) {
        for (const params::ParamEntry& entry : group.entries) {
            writer.num(static_cast<double>(entry.eval(eval)), 3);
        }
    }
    writer.arr_end();
    writer.obj_end();
}

// 校准档案全量。88 个形状一次给全（约 4 KB），页面自己在本地过滤/搜索：
// 按需分页会让"拖动滑杆的下一帧"多一次往返，实时感就没了。
void IpcServer::build_calibration(std::string& out) {
    out.clear();
    const params::CalibrationProfile profile =
        ctx_.calib != nullptr ? ctx_.calib->profile() : params::CalibrationProfile{};
    const bool available = ctx_.calib != nullptr;

    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("calibration");
    writer.key("available").boolean(available);
    writer.key("path").str(ctx_.calib_path.string());
    writer.key("default_path").str(params::default_calibration_path().string());
    // dirty 是"内存里的档案和磁盘上的不一样"：界面据此决定要不要提示未保存
    writer.key("dirty").boolean(available && profile != saved_profile_);
    writer.key("loaded").boolean(calib_loaded_);
    writer.key("active").integer(static_cast<long long>(profile.active_shape_count()));

    writer.key("shapes").arr_begin();
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const params::ShapeRange& range = profile.shapes[i];
        writer.obj_begin();
        writer.key("name").str(shape_name(static_cast<Shape>(i)));
        writer.key("lo").num(static_cast<double>(range.lo), 4);
        writer.key("hi").num(static_cast<double>(range.hi), 4);
        writer.key("active").boolean(range.active);
        // 曲线只给文字形式 + 阈值：界面据此画缩略图和选中态，不需要理解枚举值
        writer.key("curve").str(params::curve_to_text(range.curve));
        writer.key("curve_param").num(static_cast<double>(range.curve_param), 3);
        writer.obj_end();
    }
    writer.arr_end();

    // 自定义曲线槽。空槽给空串，界面按槽号索引，不用再猜"这一条是不是存在"。
    writer.key("curves").arr_begin();
    for (const params::CurveShape& curve : profile.custom_curves) {
        if (curve.count < 2) {
            writer.str("");
            continue;
        }
        std::string line;
        for (std::size_t p = 0; p < curve.count; ++p) {
            if (p != 0) {
                line += ' ';
            }
            char point[32];
            std::snprintf(point, sizeof(point), "%.3f,%.3f",
                          static_cast<double>(curve.points[p].x),
                          static_cast<double>(curve.points[p].y));
            line += point;
        }
        writer.str(line);
    }
    writer.arr_end();

    writer.key("eye").obj_begin();
    writer.key("active").boolean(profile.has_eye);
    writer.key("gain_x").num(static_cast<double>(profile.eye_gain_x), 3);
    writer.key("gain_y").num(static_cast<double>(profile.eye_gain_y), 3);
    writer.key("center_x").num(static_cast<double>(profile.eye_center_x), 3);
    writer.key("center_y").num(static_cast<double>(profile.eye_center_y), 3);
    // 眼睑两项：不依赖 has_eye（视线校准开关），只调它们也能立刻见效
    writer.key("eyelid_gain").num(static_cast<double>(profile.eyelid_gain), 3);
    writer.key("eyelid_lo").num(static_cast<double>(profile.eyelid_lo), 3);
    writer.obj_end();

    writer.obj_end();
}

// 校准页的 30 Hz 预览：raw 是校准前的原始值，out 是校准+滤波之后真正发出去的值。
// 两个都给，是为了让用户看清"我这一条 lo/hi 把原始值压到了哪里"，
// 只给一个的话，滑杆调错时页面和 VRChat 里的差别没人能解释。
void IpcServer::build_preview(std::string& out) {
    out.clear();
    params::PreviewFrame raw;
    const bool have_raw = ctx_.preview != nullptr && ctx_.preview->latest(raw);
    const FaceFrame frame = ctx_.engine != nullptr ? ctx_.engine->latest() : FaceFrame::neutral();

    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("preview");
    writer.key("seq").integer(static_cast<long long>(have_raw ? raw.sequence : frame.sequence));
    writer.key("raw_valid").boolean(have_raw && raw.valid);

    writer.key("raw").arr_begin();
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        writer.num(static_cast<double>(have_raw ? raw.shapes[i] : 0.0f), 4);
    }
    writer.arr_end();

    writer.key("out").arr_begin();
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        writer.num(static_cast<double>(frame.shapes[i]), 4);
    }
    writer.arr_end();

    writer.key("eye").obj_begin();
    writer.key("valid").boolean(have_raw && raw.eyes_valid);
    writer.key("open_l").num(static_cast<double>(have_raw ? raw.openness_l : 1.0f), 3);
    writer.key("open_r").num(static_cast<double>(have_raw ? raw.openness_r : 1.0f), 3);
    writer.key("x").num(static_cast<double>(have_raw ? raw.gaze_x_deg : 0.0f), 2);
    writer.key("y").num(static_cast<double>(have_raw ? raw.gaze_y_deg : 0.0f), 2);
    // 校准后的角度（左右平均），和形状的 out 同一个口径：界面上「原 → 出」才说得通
    writer.key("out_x").num(
        static_cast<double>(0.5f * (frame.eyes.left.gazePitchYawDeg.y +
                                    frame.eyes.right.gazePitchYawDeg.y)), 2);
    writer.key("out_y").num(
        static_cast<double>(0.5f * (frame.eyes.left.gazePitchYawDeg.x +
                                    frame.eyes.right.gazePitchYawDeg.x)), 2);
    writer.obj_end();

    writer.obj_end();
}

// SteamVR 状态是后台线程算出来的：这里只读快照，绝不在主循环里调 OpenVR。
void IpcServer::build_steamvr(std::string& out) const {
    out.clear();
    const steamvr::Status status =
        steamvr_ != nullptr ? steamvr_->snapshot() : steamvr::Status{};

    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("steamvr");
    writer.key("available").boolean(steamvr_ != nullptr);
    writer.key("state").str(steamvr::state_name(status.state));
    writer.key("ok").boolean(status.ok);
    writer.key("reason").str(status.reason.empty() ? "not_checked" : status.reason);
    writer.key("detail").str(status.detail);
    writer.key("action").str(status.action.empty() ? "refresh" : status.action);
    writer.key("busy").boolean(steamvr_ != nullptr && steamvr_->busy());
    writer.key("runtime_found").boolean(status.runtime_found);
    writer.key("steamvr_running").boolean(status.steamvr_running);
    writer.key("registered").boolean(status.registered);
    writer.key("autolaunch").boolean(status.autolaunch);
    writer.key("runtime_dir").str(status.runtime_dir);
    writer.key("manifest_path").str(status.manifest_path);
    writer.key("binary_path").str(status.binary_path);
    writer.key("app_key").str(status.app_key);
    writer.obj_end();
}

void IpcServer::build_output_profiles(std::string& out) const {
    out.clear();
    std::vector<config::OutputProfile> profiles;
    std::string error;

    std::string active;
    if (!ctx_.outputs_profiles_dir.empty() &&
        config::list_output_profiles(ctx_.outputs_profiles_dir, profiles, error)) {
        // 不额外记"当前预设"：和保存值逐字段比较，用户手改后自然回到"自定义"
        for (const config::OutputProfile& profile : profiles) {
            config::OutputOptions options;
            std::string name;
            if (config::load_output_profile(profile.path, options, name, error) &&
                options == ctx_.outputs) {
                active = name;
                break;
            }
        }
    }

    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("outputs_profiles");
    writer.key("dir").str(ctx_.outputs_profiles_dir.string());
    writer.key("active").str(active);
    if (!error.empty()) {
        writer.key("error").str(error);
    }
    writer.key("profiles").arr_begin();
    for (const config::OutputProfile& profile : profiles) {
        writer.str(profile.name);
    }
    writer.arr_end();
    writer.obj_end();
}

void IpcServer::broadcast_output_profiles() {
    std::string message;
    build_output_profiles(message);
    server_.broadcast(message);
}

void IpcServer::build_update(std::string& out) const {
    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("update");
    if (updater_ == nullptr) {
        // 没有用户数据目录（LOCALAPPDATA 缺失）：明确说不可用，别让界面一直转圈
        writer.key("state").str("idle");
        writer.key("ok").boolean(false);
        writer.key("current").str(FGB_VERSION);
        writer.key("available").boolean(false);
        writer.key("eligible").boolean(false);
        writer.key("error").str("更新不可用（没有用户数据目录）");
        writer.obj_end();
        return;
    }
    const update::Snapshot snap = updater_->snapshot();
    writer.key("state").str(update::stage_name(snap.stage));
    // ok 保留给老界面（它只看 ok/available）：只有"检查失败"才叫不 ok
    writer.key("ok").boolean(snap.stage != update::Stage::failed);
    writer.key("current").str(snap.current.empty() ? FGB_VERSION : snap.current);
    writer.key("tag").str(snap.tag);
    writer.key("latest").str(snap.latest);
    writer.key("name").str(snap.name);
    writer.key("url").str(snap.page_url);
    writer.key("available").boolean(snap.available);
    writer.key("eligible").boolean(snap.eligible);
    writer.key("skipped").boolean(snap.skipped);
    writer.key("progress").obj_begin();
    writer.key("received").integer(static_cast<long long>(snap.received));
    writer.key("total").integer(static_cast<long long>(snap.total));
    writer.obj_end();
    if (snap.staged.valid) {
        writer.key("staged").obj_begin();
        writer.key("version").str(snap.staged.version);
        writer.key("size").integer(static_cast<long long>(snap.staged.size));
        writer.obj_end();
    }
    if (!snap.error.empty()) {
        writer.key("error").str(snap.error);
    }
    writer.obj_end();
}

void IpcServer::broadcast_update() {
    std::string message;
    build_update(message);
    server_.broadcast(message);
}

void IpcServer::tick_auto_update() {
    if (updater_ == nullptr) {
        return;
    }
    if (!auto_update_kicked_ && ctx_.auto_update &&
        std::chrono::steady_clock::now() >= auto_update_due_) {
        auto_update_kicked_ = true;
        // manual=false：管理器自己按 update.ini 的 next_check_at 判断该不该查
        updater_->start_check(update::kDefaultUpdateUrl, false);
    }
    update::Snapshot snapshot;
    if (updater_->take(snapshot)) {
        broadcast_update();
    }
}

void IpcServer::start_diagnostics_export(const std::filesystem::path& output) {
    if (diag_running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (diag_thread_.joinable()) {
        diag_thread_.join();
    }

    diagnostics::BundleRequest request;
    // 默认路径只生成一次：打包含义是"构建前定名"，否则跨秒时返回路径会和
    // build_bundle 内部再生成的名字差一秒
    request.output  = output.empty() ? diagnostics::default_bundle_path() : output;
    request.exe_dir = ctx_.exe_dir;
    request.version = FGB_VERSION;

    diag_thread_ = std::thread([this, request] {
        std::string error;
        const bool ok = diagnostics::build_bundle(request, error);

        std::string message;
        JsonWriter writer{message};
        writer.obj_begin();
        writer.key("type").str("diagnostics");
        writer.key("ok").boolean(ok);
        writer.key("path").str(ok ? request.output.string() : "");
        if (!error.empty()) {
            writer.key("error").str(error);
        }
        writer.obj_end();

        {
            std::lock_guard<std::mutex> lock(diag_mutex_);
            diag_result_         = std::move(message);
            diag_result_pending_ = true;
        }
        diag_running_.store(false, std::memory_order_release);
        if (ok) {
            FGB_INFO(kTag, "诊断包已导出：%s", request.output.string().c_str());
        } else {
            FGB_WARN(kTag, "诊断包导出失败：%s", error.c_str());
        }
    });
}

void IpcServer::broadcast_calibration() {
    std::string message;
    build_calibration(message);
    server_.broadcast(message);
}

// 单个实例的运行态。界面上的红点必须配一句人话，
// 所以这里只回有限几种状态，细节靠 modules[].error。
const char* IpcServer::module_state(const RunningModule& module) const noexcept {
    if (module.source == nullptr) {
        return "external"; // 模块不归我们管（例如 --module-dll 手工指定）
    }
    const auto status = module.source->module_status();
    if (!status.has_value()) {
        return "external";
    }
    // 模块自己报了错就是故障，哪怕进程还活着（它在重试）
    if (!status->error.empty()) {
        return "error";
    }
    if (status->frames > 0 && status->alive) {
        return "active";
    }
    return status->alive ? "starting" : "stopped";
}

void IpcServer::build_modules(std::string& out) {
    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("modules");

    const vrcft::ModuleRegistry* registry = ctx_.modules;
    writer.key("active").str(ctx_.active_module_id);
    writer.key("store").str(registry != nullptr ? registry->store_dir().string() : std::string{});
    writer.key("state_path")
        .str(registry != nullptr ? registry->state_path().string() : std::string{});
    writer.key("picking").boolean(picker_ != nullptr && picker_->running.load());
    // 这个字段描述的是"注册表中的选择是否有对应实例在跑"，不是某个实例的状态
    const bool selected_running =
        !ctx_.active_module_id.empty() &&
        std::any_of(ctx_.running_modules.begin(), ctx_.running_modules.end(),
                    [this](const RunningModule& module) {
                        return module.module_id == ctx_.active_module_id;
                    });
    writer.key("state").str(selected_running ? "active" : (ctx_.active_module_id.empty() ? "none" : "installed"));

    // 配置的实例是否都真的在跑。名字是 "vrcft:face" / "native:gaze"，
    // 这里按 module_id + 后缀匹配，不依赖实例顺序。
    bool instances_match = true;
    if (registry != nullptr) {
        for (const vrcft::ModuleInstance& instance : registry->instances()) {
            const vrcft::ModuleInfo* module = registry->find(instance.module_id);
            // 引擎启动时同样会跳过这些实例，不该因为它们永远提示"配置已变更"
            if (module == nullptr || !module->enabled || module->broken) {
                continue;
            }
            // host_process 只对原生模块有意义：VRCFT 实例永远跑在 ModuleProcess.exe 里，
            // 配置里残留的 true 不代表加载方式不同，拿它比较会让提示永远消不掉。
            const bool native = module->kind == vrcft::ModuleKind::native;
            bool found = false;
            for (const RunningModule& running : ctx_.running_modules) {
                if (running.module_id != instance.module_id ||
                    (native && running.host_process != instance.host_process)) {
                    continue;
                }
                const std::string suffix = ":" + instance.name;
                if (running.display_name.size() >= suffix.size() &&
                    running.display_name.compare(running.display_name.size() - suffix.size(),
                                                 suffix.size(), suffix) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                instances_match = false;
                break;
            }
        }
    }

    // 选了启动模块但没在跑（或者改了启停/实例列表）：界面提示用户点"重启引擎"
    writer.key("restart_needed").boolean(
        (!ctx_.active_module_id.empty() && !selected_running) || !instances_match);

    writer.key("instances").arr_begin();
    if (registry != nullptr) {
        for (const vrcft::ModuleInstance& instance : registry->instances()) {
            const vrcft::ModuleInfo* module = registry->find(instance.module_id);
            writer.obj_begin();
            writer.key("name").str(instance.name);
            writer.key("module_id").str(instance.module_id);
            writer.key("module_name").str(module != nullptr ? module->name : instance.module_id);
            writer.key("enabled").boolean(module != nullptr && module->enabled && !module->broken);
            // kind 决定界面要不要给「独立进程」开关：只有原生模块有两种加载方式，
            // VRCFT 模块固定跑在 ModuleProcess.exe 里。
            writer.key("kind").str(module != nullptr && module->kind == vrcft::ModuleKind::native
                                       ? "native"
                                       : "vrcft");
            writer.key("host_process").boolean(instance.host_process);
            writer.obj_end();
        }
    }
    writer.arr_end();

    writer.key("roles").obj_begin();
    const vrcft::ModuleRoles roles = registry != nullptr ? registry->roles() : vrcft::ModuleRoles{};
    writer.key("expression").str(roles.expression);
    writer.key("eyes").str(roles.eyes);
    writer.key("gaze").str(roles.gaze);
    writer.key("head").str(roles.head);
    writer.obj_end();

    const auto runtime = vrcft::find_vrcft_runtime();
    writer.key("runtime").obj_begin();
    writer.key("available").boolean(runtime.has_value());
    writer.key("path").str(runtime.has_value() ? runtime->exe.string() : std::string{});
    writer.key("origin").str(runtime.has_value() ? runtime->origin : std::string{});
    writer.obj_end();

    writer.key("items").arr_begin();
    if (registry != nullptr) {
        for (const vrcft::ModuleInfo& module : registry->modules()) {
            writer.obj_begin();
            writer.key("id").str(module.id);
            writer.key("name").str(module.name);
            writer.key("author").str(module.author);
            writer.key("version").str(module.version);
            writer.key("description").str(module.description);
            writer.key("usage").str(module.usage);
            writer.key("page").str(module.page_url);
            // 没有图标文件也可能从 DLL 资源里取；让界面先请求，404 时自然回退头像
            writer.key("icon").boolean(!module.icon.empty() || !module.dll.empty());
            writer.key("dir").str(module.directory.string());
            writer.key("dll").str(module.dll.string());
            writer.key("metadata").boolean(module.metadata_ok);
            writer.key("broken").boolean(module.broken);
            writer.key("active").boolean(module.active);
            writer.key("enabled").boolean(module.enabled);
            writer.key("source").str(module.source);
            writer.key("installed_at").str(module.installed_at);
            writer.key("bytes").integer(static_cast<long long>(module.bytes));
            writer.key("kind").str(module.kind == vrcft::ModuleKind::native ? "native" : "vrcft");
            writer.key("declared_capabilities")
                .integer(static_cast<long long>(module.declared_capabilities));
            writer.key("role_hint").str(module.role_hint);
            writer.key("min_host_version").str(module.min_host_version);
            writer.key("abi_version").integer(static_cast<long long>(module.abi_version));
            writer.key("host_compatible").boolean(module.host_compatible);

            // 这个模块现在是不是被某个实例加载着？是的话状态来自那个实例，
            // 而不是注册表里"下次启动加载"的选择——两者可能不一致（用户刚改过选择还没重启）。
            const RunningModule* running = nullptr;
            for (const RunningModule& candidate : ctx_.running_modules) {
                if (candidate.module_id == module.id) {
                    running = &candidate;
                    break;
                }
            }

            const char* state = module.enabled ? "installed" : "disabled";
            if (module.broken) {
                state = "broken";
            } else if (running != nullptr) {
                state = module_state(*running);
            }
            writer.key("state").str(state);
            writer.key("running").boolean(running != nullptr);
            writer.key("instance").str(running != nullptr ? running->display_name : std::string{});
            // 运行时能力以模块自报为准（清单可能没写或写错）
            std::uint32_t capabilities = module.declared_capabilities;
            if (running != nullptr && running->source != nullptr) {
                const auto runtime_status = running->source->module_status();
                if (runtime_status.has_value() && runtime_status->capabilities != 0) {
                    capabilities = runtime_status->capabilities;
                }
            }
            writer.key("capabilities").integer(static_cast<long long>(capabilities));
            writer.obj_end();
        }
    }
    writer.arr_end();

    // 本机检测到、还没导入的模块：只用于界面提示，引擎不会直接加载它们
    writer.key("importable").arr_begin();
    if (registry != nullptr) {
        for (const vrcft::ImportCandidate& candidate : registry->importable()) {
            writer.obj_begin();
            writer.key("id").str(candidate.info.id);
            writer.key("name").str(candidate.info.name);
            writer.key("author").str(candidate.info.author);
            writer.key("version").str(candidate.info.version);
            writer.key("description").str(candidate.info.description);
            writer.key("broken").boolean(candidate.info.broken);
            writer.key("origin").str(candidate.origin);
            writer.key("dir").str(candidate.info.directory.string());
            writer.obj_end();
        }
    }
    writer.arr_end();

    writer.obj_end();
}

void IpcServer::build_library(std::string& out) {
    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("library");

    vrcft::LibrarySnapshot snapshot;
    if (library_) {
        snapshot = library_->snapshot();
    }
    writer.key("status").obj_begin();
    writer.key("stale").boolean(snapshot.status.stale);
    writer.key("fetched_at").str(snapshot.status.fetched_at);
    writer.key("refreshing").boolean(snapshot.status.refreshing);
    writer.key("refresh_error").str(snapshot.status.refresh_error);
    writer.key("phase").str(snapshot.status.phase);
    writer.key("error").str(snapshot.status.error);
    writer.key("install_id").str(snapshot.status.install_id);
    writer.key("received").integer(static_cast<long long>(snapshot.status.received));
    writer.key("total").integer(static_cast<long long>(snapshot.status.total));
    writer.obj_end();

    writer.key("items").arr_begin();
    for (const vrcft::LibraryEntry& entry : snapshot.entries) {
        writer.obj_begin();
        writer.key("id").str(entry.id);
        writer.key("name").str(entry.name);
        writer.key("author").str(entry.author);
        writer.key("version").str(entry.version);
        writer.key("description").str(entry.description);
        writer.key("usage").str(entry.usage);
        writer.key("page").str(entry.page);
        writer.key("downloads").integer(entry.downloads);
        writer.key("ratings").integer(entry.ratings);
        writer.key("rating").num(entry.rating, 2);
        writer.key("updated").str(entry.last_updated);
        writer.obj_end();
    }
    writer.arr_end();

    append_native_library(writer);

    writer.obj_end();
}

namespace {

void write_strings(json::JsonWriter& writer, const std::vector<std::string>& items) {
    writer.arr_begin();
    for (const std::string& item : items) {
        writer.str(item);
    }
    writer.arr_end();
}

void write_native_version(json::JsonWriter& writer, const vrcft::NativeVersion& version) {
    writer.key("version").str(version.version);
    writer.key("channel").str(version.channel);
    writer.key("published").str(version.published_at);
    writer.key("download_url").str(version.download_url);
    writer.key("file_name").str(version.file_name);
    writer.key("size").integer(static_cast<long long>(version.size));
    writer.key("sha256").str(version.sha256);
    writer.key("abi_version").integer(version.abi_version);
    writer.key("downloads").integer(version.downloads);
    writer.key("notes").str(version.notes);
    writer.key("release_url").str(version.release_url);
}

} // namespace

void IpcServer::append_native_library(json::JsonWriter& writer) {
    vrcft::NativeLibrarySnapshot snapshot;
    if (native_library_) {
        snapshot = native_library_->snapshot();
    }

    writer.key("native").obj_begin();

    writer.key("registry").obj_begin();
    writer.key("title").str(snapshot.registry.title);
    writer.key("description").str(snapshot.registry.description);
    writer.key("homepage").str(snapshot.registry.homepage);
    writer.key("generated_at").str(snapshot.registry.generated_at);
    writer.obj_end();

    writer.key("status").obj_begin();
    writer.key("stale").boolean(snapshot.status.stale);
    writer.key("fetched_at").str(snapshot.status.fetched_at);
    writer.key("refreshing").boolean(snapshot.status.refreshing);
    writer.key("refresh_error").str(snapshot.status.refresh_error);
    writer.key("phase").str(snapshot.status.phase);
    writer.key("error").str(snapshot.status.error);
    writer.key("install_id").str(snapshot.status.install_id);
    writer.key("install_version").str(snapshot.status.install_version);
    writer.key("received").integer(static_cast<long long>(snapshot.status.received));
    writer.key("total").integer(static_cast<long long>(snapshot.status.total));
    writer.obj_end();

    writer.key("items").arr_begin();
    for (const vrcft::NativeEntry& entry : snapshot.registry.modules) {
        writer.obj_begin();
        writer.key("id").str(entry.id);
        writer.key("name").str(entry.name);
        writer.key("author").str(entry.author);
        writer.key("author_url").str(entry.author_url);
        writer.key("summary").str(entry.summary);
        writer.key("description").str(entry.description);
        writer.key("usage").str(entry.usage);
        writer.key("homepage").str(entry.homepage);
        writer.key("repo").str(entry.repo);
        writer.key("license").str(entry.license);
        writer.key("icon").str(entry.icon);
        writer.key("screenshots");
        write_strings(writer, entry.screenshots);
        writer.key("tags");
        write_strings(writer, entry.tags);
        writer.key("capabilities");
        write_strings(writer, entry.capabilities);
        writer.key("default_role").str(entry.default_role);
        writer.key("channel").str(entry.channel);
        writer.key("featured").boolean(entry.featured);
        writer.key("verified").boolean(entry.verified);
        writer.key("archived").boolean(entry.archived);

        writer.key("stats").obj_begin();
        writer.key("downloads").integer(entry.stats_downloads);
        writer.key("stars").integer(entry.stats_stars);
        writer.obj_end();

        writer.key("published").str(entry.published_at);
        writer.key("updated").str(entry.updated_at);

        writer.key("requires").obj_begin();
        writer.key("abi").integer(entry.requires_abi);
        writer.key("min_host_version").str(entry.min_host_version);
        writer.key("os");
        write_strings(writer, entry.requires_os);
        writer.key("arch");
        write_strings(writer, entry.requires_arch);
        writer.obj_end();

        // 兼容性由引擎判定：界面只按 code 出本地化文案，不自己猜版本号
        writer.key("compatible").boolean(entry.compatible);
        writer.key("incompatible_code").str(entry.incompatible_code);
        writer.key("incompatible_reason").str(entry.incompatible_reason);

        writer.key("latest").obj_begin();
        write_native_version(writer, entry.latest);
        writer.obj_end();

        writer.key("versions").arr_begin();
        for (const vrcft::NativeVersion& version : entry.versions) {
            writer.obj_begin();
            write_native_version(writer, version);
            writer.obj_end();
        }
        writer.arr_end();

        // i18n 键是 BCP-47 语言标签；界面按当前语言取，缺了就回落到顶层字段
        writer.key("i18n").obj_begin();
        for (const auto& [lang, text] : entry.i18n) {
            writer.key(lang).obj_begin();
            writer.key("summary").str(text.summary);
            writer.key("description").str(text.description);
            writer.key("usage").str(text.usage);
            writer.obj_end();
        }
        writer.obj_end();

        writer.obj_end();
    }
    writer.arr_end();

    writer.obj_end();
}


void IpcServer::flush_logs() {
    std::deque<LogLine> pending;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        pending.swap(logs_);
    }
    if (pending.empty()) {
        return;
    }

    for (const LogLine& line : pending) {
        server_.broadcast(make_log_message(line.level, line.tag, line.message));
    }
}

// 新连接补发最近的日志：否则 UI 一打开，日志页是空的，
// 而"为什么没数据"恰恰写在连接之前的那几行里。
void IpcServer::send_backlog(std::intptr_t conn) {
    std::deque<LogLine> lines;
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        lines = recent_logs_;
    }
    for (const LogLine& line : lines) {
        server_.send_text(conn, make_log_message(line.level, line.tag, line.message));
    }
}

void IpcServer::handle_text(std::intptr_t conn, std::string_view text) {
    std::string type;
    bool ok                 = false;
    std::string error;
    bool modules_touched    = false;
    bool modules_dirty_only = false;
    bool library_touched    = false;
    bool calibration_changed = false;
    bool steamvr_touched     = false;
    bool outputs_touched     = false;
    bool profiles_touched    = false;
    std::string modules_message;
    std::string ack_message;
    bool engine_restarting = false;
    bool update_installing = false;

    const bool has_type = json::field_string(text, "type", type);
    FGB_DEBUG(kTag, "命令：%.*s", static_cast<int>(text.size()), text.data());
    if (has_type && type == "set_filter") {
        if (ctx_.filter == nullptr) {
            error = "filter not available";
        } else {
            bool enabled = ctx_.filter->enabled();
            double min_cutoff = ctx_.filter->min_cutoff();
            double beta       = ctx_.filter->beta();
            json::field_bool(text, "enabled", enabled);
            json::field_number(text, "min_cutoff", min_cutoff);
            json::field_number(text, "beta", beta);
            min_cutoff = std::clamp(min_cutoff, 0.01, 20.0);
            beta       = std::clamp(beta, 0.0, 5.0);
            ctx_.filter->set_enabled(enabled);
            ctx_.filter->set_live_params(static_cast<float>(min_cutoff), static_cast<float>(beta));
            ok = true;
        }
    } else if (has_type && type == "set_outputs") {
        config::OutputOptions next = ctx_.outputs;
        if (parse_output_patch(text, next, error)) {
            if (ctx_.outputs_path.empty()) {
                error = "输出设置路径不可用";
            } else if (!config::save_outputs(ctx_.outputs_path, next, error)) {
                // save_outputs 已经写好 error
            } else {
                ctx_.outputs    = next;
                ok              = true;
                outputs_touched = true;
                // OSC 发送开关可以热生效；端口/目标类改动仍需重启引擎
                if (ctx_.sink != nullptr) {
                    ctx_.sink->set_enabled(next.vrchat_enabled);
                }
                ack_message     = "输出设置已保存，重启引擎后生效";
            }
        }
    } else if (has_type && (type == "save_output_profile" ||
                            type == "apply_output_profile" ||
                            type == "delete_output_profile")) {
        std::string name;
        if (!json::field_string(text, "name", name) || name.empty()) {
            error = "缺少预设名称";
        } else if (ctx_.outputs_profiles_dir.empty()) {
            error = "预设目录不可用";
        } else if (type == "save_output_profile") {
            config::OutputOptions next = ctx_.outputs;
            if (parse_output_patch(text, next, error) &&
                config::save_output_profile(ctx_.outputs_profiles_dir, name, next, error)) {
                ok              = true;
                profiles_touched = true;
                ack_message     = "预设已保存";
            }
        } else if (type == "apply_output_profile") {
            std::vector<config::OutputProfile> profiles;
            if (!config::list_output_profiles(ctx_.outputs_profiles_dir, profiles, error)) {
                // list_output_profiles 已经写好 error
            } else {
                bool found = false;
                config::OutputOptions loaded;
                std::string display_name;
                for (const config::OutputProfile& profile : profiles) {
                    if (profile.name != name) {
                        continue;
                    }
                    found = config::load_output_profile(profile.path, loaded, display_name, error);
                    break;
                }
                if (!found) {
                    error = "预设不存在";
                } else if (ctx_.outputs_path.empty()) {
                    error = "输出设置路径不可用";
                } else if (!config::save_outputs(ctx_.outputs_path, loaded, error)) {
                    // save_outputs 已经写好 error
                } else {
                    ctx_.outputs     = loaded;
                    ok               = true;
                    outputs_touched  = true;
                    profiles_touched = true;
                    if (ctx_.sink != nullptr) {
                        ctx_.sink->set_enabled(loaded.vrchat_enabled);
                    }
                    ack_message      = "预设「" + name + "」已应用，重启引擎后生效";
                }
            }
        } else if (config::delete_output_profile(ctx_.outputs_profiles_dir, name, error)) {
            ok               = true;
            profiles_touched = true;
            ack_message      = "预设「" + name + "」已删除";
        }
    } else if (has_type && type == "set_preview") {
        bool enabled = false;
        json::field_bool(text, "enabled", enabled);
        if (ctx_.preview == nullptr) {
            error = "preview not available";
        } else {
            preview_wanted_ = enabled;
            ctx_.preview->set_enabled(enabled);
            if (enabled) {
                // 立刻回一帧：页面切过来时滑杆和数值不该先空白 33 ms
                last_preview_ = std::chrono::steady_clock::time_point{};
            }
            ok = true;
        }
    } else if (has_type && type == "set_calibration_shape") {
        std::string name;
        json::field_string(text, "name", name);
        Shape shape{};
        if (ctx_.calib == nullptr) {
            error = "calibration not available";
        } else if (!shape_from_name(name, shape)) {
            error = "unknown shape: " + name;
        } else {
            params::CalibrationProfile profile = ctx_.calib->profile();
            params::ShapeRange& range          = profile.shapes[index_of(shape)];
            double value                       = 0.0;
            const bool lo_given                = json::field_number(text, "lo", value);
            if (lo_given) {
                range.lo = clamp01(static_cast<float>(value));
            }
            const bool hi_given = json::field_number(text, "hi", value);
            if (hi_given) {
                range.hi = clamp01(static_cast<float>(value));
            }

            // 双滑块上两端不允许交叉：越界时把被拖的那一端顶回去，而不是交换。
            // 交换会让两个拇指在手指底下瞬间对调，用户完全没法继续控制，所以这里
            // 改成"碰壁"——拖到贴住就停住，松手后还能往回拖。
            if (range.hi - range.lo < params::kMinSpan) {
                if (lo_given && !hi_given) {
                    range.lo = range.hi - params::kMinSpan;
                    if (range.lo < 0.0f) {
                        range.lo = 0.0f;
                        range.hi = params::kMinSpan;
                    }
                } else if (hi_given && !lo_given) {
                    range.hi = range.lo + params::kMinSpan;
                    if (range.hi > 1.0f) {
                        range.hi = 1.0f;
                        range.lo = 1.0f - params::kMinSpan;
                    }
                } else if (range.lo > range.hi) {
                    std::swap(range.lo, range.hi); // 两端一起给且倒挂：兜底，不丢用户的意图
                }
            }

            std::string curve_text;
            if (json::field_string(text, "curve", curve_text)) {
                range.curve = params::curve_from_text(curve_text);
            }
            if (json::field_number(text, "curve_param", value)) {
                range.curve_param = static_cast<float>(std::clamp(value, 0.0, 1.0));
            }

            bool active = range.active;
            if (json::field_bool(text, "active", active)) {
                range.active = active;
                // 刚启用的形状如果区间是"没调过"的状态（跨度近 0），给一个能用的默认：
                // 否则用户勾了启用、拖滑杆，输出却一点没变
                if (active && range.hi - range.lo < params::kMinSpan) {
                    range.lo = 0.0f;
                    range.hi = 1.0f;
                }
            }
            ctx_.calib->set_profile(profile); // 实时生效：预览与实际输出同时变
            calibration_changed = true;
            ok                  = true;
        }
    } else if (has_type && type == "set_calibration_curve") {
        // 自定义曲线：槽号 + 一串归一化控制点。点用字符串传，是因为 base/json 只做
        // "一层对象里取标量"，为 8×2 的数组单开一个嵌套解析器不值当。
        double slot = 0.0;
        std::string points;
        if (ctx_.calib == nullptr) {
            error = "calibration not available";
        } else if (!json::field_number(text, "slot", slot) || slot < 0 ||
                   slot >= static_cast<double>(params::kMaxCustomCurves)) {
            error = "曲线槽号超出范围";
        } else if (!json::field_string(text, "points", points)) {
            error = "缺少控制点";
        } else {
            params::CalibrationProfile profile = ctx_.calib->profile();
            params::CurveShape curve;
            std::size_t begin = 0;
            while (begin < points.size() && curve.count < params::kMaxCurvePoints) {
                const std::size_t end    = points.find(' ', begin);
                const std::string token  = points.substr(begin, end == std::string::npos
                                                                     ? std::string::npos
                                                                     : end - begin);
                const std::size_t comma  = token.find(',');
                if (comma != std::string::npos) {
                    curve.points[curve.count].x =
                        static_cast<float>(std::atof(token.substr(0, comma).c_str()));
                    curve.points[curve.count].y =
                        static_cast<float>(std::atof(token.substr(comma + 1).c_str()));
                    ++curve.count;
                }
                if (end == std::string::npos) {
                    break;
                }
                begin = end + 1;
            }
            params::sanitize_curve(curve);
            profile.custom_curves[static_cast<std::size_t>(slot)] = curve;
            ctx_.calib->set_profile(profile);
            calibration_changed = true;
            ok                  = true;
        }
    } else if (has_type && type == "set_calibration_eye") {
        if (ctx_.calib == nullptr) {
            error = "calibration not available";
        } else {
            params::CalibrationProfile profile = ctx_.calib->profile();
            double value                       = 0.0;
            if (json::field_number(text, "gain_x", value)) {
                profile.eye_gain_x = static_cast<float>(std::clamp(value, 0.1, 5.0));
            }
            if (json::field_number(text, "gain_y", value)) {
                profile.eye_gain_y = static_cast<float>(std::clamp(value, 0.1, 5.0));
            }
            if (json::field_number(text, "center_x", value)) {
                profile.eye_center_x = static_cast<float>(std::clamp(value, -60.0, 60.0));
            }
            if (json::field_number(text, "center_y", value)) {
                profile.eye_center_y = static_cast<float>(std::clamp(value, -60.0, 60.0));
            }
            if (json::field_number(text, "eyelid_gain", value)) {
                profile.eyelid_gain = static_cast<float>(std::clamp(value, 0.2, 4.0));
            }
            if (json::field_number(text, "eyelid_lo", value)) {
                profile.eyelid_lo = static_cast<float>(std::clamp(value, 0.0, 0.5));
            }
            bool active = profile.has_eye;
            if (json::field_bool(text, "active", active)) {
                profile.has_eye = active;
            }
            ctx_.calib->set_profile(profile);
            calibration_changed = true;
            ok                  = true;
        }
    } else if (has_type && type == "capture_calibration") {
        std::string name;
        std::string which = "lo";
        json::field_string(text, "shape", name);
        json::field_string(text, "which", which);
        params::PreviewFrame raw;
        if (ctx_.calib == nullptr) {
            error = "calibration not available";
        } else if (ctx_.preview == nullptr || !ctx_.preview->latest(raw) || !raw.valid) {
            error = "没有实时数据：请先打开校准页（预览取样只在校准页打开时开启）";
        } else {
            params::CalibrationProfile profile = ctx_.calib->profile();
            bool apply                         = false;
            if (name.empty()) {
                // 空 shape = 取当前视线角度作为中心：用户平视前方时点一下
                if (!raw.eyes_valid) {
                    error = "输入源没有眼追数据";
                } else {
                    profile.eye_center_x = raw.gaze_x_deg;
                    profile.eye_center_y = raw.gaze_y_deg;
                    profile.has_eye      = true;
                    apply                = true;
                    ack_message          = "已把当前视线角度设为中心";
                }
            } else {
                Shape shape{};
                if (!shape_from_name(name, shape)) {
                    error = "unknown shape: " + name;
                } else {
                    params::ShapeRange& range = profile.shapes[index_of(shape)];
                    const float current       = clamp01(raw.shapes[index_of(shape)]);
                    if (which == "hi") {
                        range.hi = current;
                    } else {
                        range.lo = current;
                    }
                    if (range.lo > range.hi) {
                        std::swap(range.lo, range.hi);
                    }
                    apply = true;
                    // 两端几乎一样时不启用，但必须告诉用户为什么——否则就是
                    // "点了取当前，什么都没发生"的黑箱
                    if (range.hi - range.lo >= params::kMinSpan) {
                        range.active = true;
                        ack_message  = "已取当前值为 " + which;
                    } else {
                        range.active = false;
                        error        = "当前值与该形状的另一端几乎相同，跨度太小，未启用";
                    }
                }
            }
            if (apply) {
                ctx_.calib->set_profile(profile);
                calibration_changed = true;
                // 跨度太小不算成功：页面要弹提示，而不是静静地把启用勾去掉
                ok = error.empty();
            }
        }
    } else if (has_type && type == "save_calibration") {
        if (ctx_.calib == nullptr) {
            error = "calibration not available";
        } else if (ctx_.calib_path.empty()) {
            error = "no calibration path";
        } else {
            const params::CalibrationProfile profile = ctx_.calib->profile();
            std::string save_error;
            if (params::save_profile_atomic(ctx_.calib_path.string(), profile, save_error)) {
                saved_profile_      = profile;
                calib_loaded_       = true;
                calibration_changed = true;
                ok                  = true;
                ack_message         = "校准已保存：" + ctx_.calib_path.string();
                FGB_INFO(kTag, "校准档案已保存：%s（%zu 个形状）", ctx_.calib_path.string().c_str(),
                         profile.active_shape_count());
            } else {
                error = save_error;
            }
        }
    } else if (has_type && type == "reload_calibration") {
        if (ctx_.calib == nullptr) {
            error = "calibration not available";
        } else if (ctx_.calib_path.empty()) {
            error = "no calibration path";
        } else {
            params::CalibrationProfile profile;
            if (!params::load_profile(ctx_.calib_path.string(), profile)) {
                error = "读取失败：" + ctx_.calib_path.string();
            } else {
                ctx_.calib->set_profile(profile);
                saved_profile_      = profile;
                calib_loaded_       = true;
                calibration_changed = true;
                ok                  = true;
                ack_message         = "已重新加载校准档案";
            }
        }
    } else if (has_type && type == "reset_calibration") {
        if (ctx_.calib == nullptr) {
            error = "calibration not available";
        } else {
            // 只改内存，不删文件：用户点错了还能「重新加载」拿回来
            ctx_.calib->set_profile(params::CalibrationProfile{});
            calibration_changed = true;
            ok                  = true;
            ack_message         = "已恢复默认（全部形状透传）";
        }
    } else if (has_type && type == "refresh_steamvr") {
        if (steamvr_ == nullptr) {
            error = "steamvr not available";
        } else {
            steamvr_->request(steamvr::Service::Op::Refresh);
            steamvr_touched = true;
            ack_message     = "正在检测 SteamVR…";
            ok              = true;
        }
    } else if (has_type && type == "set_steamvr_autolaunch") {
        bool enabled = false;
        if (steamvr_ == nullptr) {
            error = "steamvr not available";
        } else if (!json::field_bool(text, "enabled", enabled)) {
            error = "missing enabled";
        } else {
            steamvr_->request(enabled ? steamvr::Service::Op::Enable
                                      : steamvr::Service::Op::Disable);
            steamvr_touched = true;
            ack_message     = enabled ? "正在注册 SteamVR 启动项…" : "正在关闭 SteamVR 启动项…";
            ok              = true;
        }
    } else if (has_type && type == "remove_steamvr_manifest") {
        if (steamvr_ == nullptr) {
            error = "steamvr not available";
        } else {
            steamvr_->request(steamvr::Service::Op::Remove);
            steamvr_touched = true;
            ack_message     = "正在移除 SteamVR 注册…";
            ok              = true;
        }
    } else if (has_type && type == "reload_avatar") {
        if (ctx_.sink == nullptr) {
            error = "sink not available";
        } else {
            ctx_.sink->reload_avatar();
            catalog_dirty_ = true;
            refresh_catalog();
            server_.broadcast(catalog_json_);
            ok = true;
        }
    } else if (has_type && type == "scan_modules") {
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else {
            ctx_.modules->rescan();
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            modules_message       = "模块列表已刷新";
            ack_message = "扫描完成：" + std::to_string(ctx_.modules->modules().size()) +
                          " 个已安装，" + std::to_string(ctx_.modules->importable().size()) +
                          " 个可导入";
            ok = true;
        }
    } else if (has_type && type == "set_module") {
        std::string id;
        json::field_string(text, "id", id); // 空串 = 不加载任何模块
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (ctx_.modules->set_active(id)) {
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            modules_message       = "启动模块已更新（重启引擎后生效）";
            ok                    = true;
        } else {
            error = "unknown module";
        }
    } else if (has_type && type == "set_module_enabled") {
        std::string id;
        bool enabled = true;
        json::field_string(text, "id", id);
        json::field_bool(text, "enabled", enabled);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (ctx_.modules->set_enabled(id, enabled)) {
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            modules_message       = enabled ? "模块已启用" : "模块已停用";
            ok                    = true;
        } else {
            error = "unknown module";
        }
    } else if (has_type && type == "add_instance") {
        std::string id;
        std::string name;
        bool host_process = false;
        json::field_string(text, "id", id);
        json::field_string(text, "name", name);
        const bool host_given = json::field_bool(text, "host_process", host_process);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (id.empty() || name.empty()) {
            error = "missing module id or instance name";
        } else {
            if (!host_given) {
                // 没显式指定时看模块类型：原生模块默认独立进程（崩溃不带走引擎，
                // 也才能在不重启引擎的情况下换 DLL）；VRCFT 模块本来就跑在
                // ModuleProcess.exe 里，这个开关对它没有意义。
                const vrcft::ModuleInfo* module = ctx_.modules->find(id);
                host_process =
                    module != nullptr && module->kind == vrcft::ModuleKind::native;
            }
            if (ctx_.modules->add_instance(name, id, host_process)) {
                modules_touched = true;
                modules_message = "启动实例已添加（重启引擎后生效）";
                ack_message     = "实例 " + name + " 已添加";
                ok              = true;
            } else {
                error = "无法添加实例（名字重复或模块不可用）";
            }
        }
    } else if (has_type && type == "remove_instance") {
        std::string name;
        json::field_string(text, "name", name);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (ctx_.modules->remove_instance(name)) {
            modules_touched = true;
            modules_message = "启动实例已移除（重启引擎后生效）";
            ack_message     = "实例 " + name + " 已移除";
            ok              = true;
        } else {
            error = "unknown instance";
        }
    } else if (has_type && type == "set_instance_host") {
        std::string name;
        bool host_process = false;
        json::field_string(text, "name", name);
        json::field_bool(text, "host_process", host_process);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (ctx_.modules->set_instance_host(name, host_process)) {
            modules_touched = true;
            modules_message = host_process ? "实例已改为独立进程（重启引擎后生效）"
                                           : "实例已改为进程内加载（重启引擎后生效）";
            ack_message     = "实例 " + name + " 的加载方式已更新";
            ok              = true;
        } else {
            error = "unknown instance";
        }
    } else if (has_type && type == "restart_instance") {
        std::string name;
        json::field_string(text, "name", name);
        if (ctx_.engine == nullptr) {
            error = "engine not available";
        } else if (name.empty()) {
            error = "missing instance name";
        } else {
            const RunningModule* target = nullptr;
            const std::string suffix = ":" + name;
            for (const RunningModule& module : ctx_.running_modules) {
                if (module.display_name == name ||
                    (module.display_name.size() >= suffix.size() &&
                     module.display_name.compare(module.display_name.size() - suffix.size(),
                                                 suffix.size(), suffix) == 0)) {
                    target = &module;
                    break;
                }
            }
            std::string restart_error;
            if (target == nullptr) {
                error = instance_missing_error(ctx_.modules, name);
            } else if (ctx_.engine->restart_source(target->display_name, restart_error)) {
                modules_touched = true;
                modules_message = "实例已重启";
                ack_message     = target->display_name + " 已重启";
                ok              = true;
            } else {
                error = restart_error;
            }
        }
    } else if (has_type && (type == "stop_instance" || type == "start_instance")) {
        std::string name;
        json::field_string(text, "name", name);
        if (ctx_.engine == nullptr) {
            error = "engine not available";
        } else if (name.empty()) {
            error = "missing instance name";
        } else {
            const RunningModule* target = nullptr;
            const std::string suffix = ":" + name;
            for (const RunningModule& module : ctx_.running_modules) {
                if (module.display_name == name ||
                    (module.display_name.size() >= suffix.size() &&
                     module.display_name.compare(module.display_name.size() - suffix.size(),
                                                 suffix.size(), suffix) == 0)) {
                    target = &module;
                    break;
                }
            }
            std::string op_error;
            const bool start = type == "start_instance";
            const bool changed =
                target != nullptr &&
                (start ? ctx_.engine->start_source(target->display_name, op_error)
                       : ctx_.engine->stop_source(target->display_name, op_error));
            if (target == nullptr) {
                error = instance_missing_error(ctx_.modules, name);
            } else if (changed) {
                modules_touched = true;
                modules_message = start ? "实例已启动" : "实例已停止";
                ack_message     = target->display_name + (start ? " 已启动" : " 已停止");
                ok              = true;
            } else {
                error = op_error;
            }
        }
    } else if (has_type && type == "set_module_roles") {
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else {
            vrcft::ModuleRoles roles;
            json::field_string(text, "expression", roles.expression);
            json::field_string(text, "eyes", roles.eyes);
            json::field_string(text, "gaze", roles.gaze);
            json::field_string(text, "head", roles.head);
            ctx_.modules->set_roles(roles);

            if (ctx_.engine != nullptr) {
                const auto lower = [](std::string value) {
                    std::transform(value.begin(), value.end(), value.begin(),
                                   [](unsigned char c) {
                                       return static_cast<char>(std::tolower(c));
                                   });
                    return value;
                };
                const vrcft::ModuleRoles saved = ctx_.modules->roles();
                const auto resolve = [&](const std::string& value) -> std::string {
                    if (value.empty()) {
                        return {};
                    }
                    const std::string needle = lower(value);
                    for (const RunningModule& module : ctx_.running_modules) {
                        if (lower(module.module_id) == needle) {
                            return module.display_name;
                        }
                    }
                    for (const std::string& name : ctx_.source_names) {
                        if (lower(name) == needle) {
                            return name;
                        }
                    }
                    return {};
                };
                MergePolicy policy = ctx_.engine->merge_policy();
                policy.expression_source = resolve(saved.expression);
                policy.eye_source        = resolve(saved.eyes);
                policy.gaze_source       = resolve(saved.gaze);
                policy.head_source       = resolve(saved.head);
                ctx_.engine->set_merge_policy(policy);
            }

            modules_touched = true;
            modules_message = "角色来源已更新（立即生效）";
            ack_message     = "角色来源已保存";
            ok              = true;
        }
    } else if (has_type && type == "delete_module") {
        std::string id;
        json::field_string(text, "id", id);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (const vrcft::ModuleInfo* module = ctx_.modules->find(id); module == nullptr) {
            error = "模块未安装";
        } else {
            const std::string name = module->name.empty() ? id : module->name;

            // DLL 被运行中的实例锁着，删不掉。以前是把"请先停止实例"丢给用户，
            // 现在直接替他停：确认框里已经写明会停实例，用户点确定就是授权。
            const std::vector<std::string> stopped = stop_module_instances(id);
            std::string remove_error;
            bool removed = false;
            // 停宿主会带走模块自己拉起的子进程（如采集 tap），它们释放文件比宿主晚
            // 几十毫秒。只有真的动过实例才值得等这一下；外部程序占用等多久都没用。
            const int attempts = stopped.empty() ? 1 : 10;
            for (int attempt = 0; attempt < attempts; ++attempt) {
                if (attempt > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                }
                remove_error.clear();
                if (ctx_.modules->remove_module(id, remove_error)) {
                    removed = true;
                    break;
                }
            }
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            if (removed) {
                // 文件没了，实例也不可能再起来：把源从引擎里摘掉，别让界面继续显示它
                forget_module_instances(id);
                modules_message = "模块已删除";
                ack_message     = name + " 已删除";
                if (!stopped.empty()) {
                    ack_message += "（已自动停止 " + std::to_string(stopped.size()) + " 个运行中的实例）";
                }
                ok = true;
            } else {
                modules_message = remove_error;
                error           = remove_error;
                // 没删成就得把实例恢复，否则用户既没删掉模块也丢了追踪
                const std::string failed = restart_instances(stopped, nullptr);
                if (!failed.empty()) {
                    error += "；" + failed;
                }
                ack_message = name + " 删除失败：" + error;
            }
        }
    } else if (has_type && type == "import_module") {
        std::string id;
        json::field_string(text, "id", id);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else {
            std::string import_error;
            const bool imported = ctx_.modules->import_module(id, import_error);
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            modules_message       = imported ? "模块已导入" : import_error;
            ok                    = imported;
            if (!imported) {
                error = import_error;
            }
        }
    } else if (has_type && type == "import_modules") {
        std::string ids_text;
        json::field_string(text, "ids", ids_text);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (ids_text.empty()) {
            error = "missing module ids";
        } else {
            std::vector<std::string> ids;
            std::size_t begin = 0;
            while (begin <= ids_text.size()) {
                const std::size_t comma = ids_text.find(',', begin);
                const std::size_t end = comma == std::string::npos ? ids_text.size() : comma;
                if (end > begin) {
                    ids.push_back(ids_text.substr(begin, end - begin));
                }
                if (comma == std::string::npos) {
                    break;
                }
                begin = comma + 1;
            }
            std::size_t done = 0;
            std::string first_error;
            for (const std::string& id : ids) {
                std::string import_error;
                if (ctx_.modules->import_module(id, import_error)) {
                    ++done;
                } else if (first_error.empty()) {
                    first_error = import_error;
                }
            }
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            modules_message       = "已导入 " + std::to_string(done) + " 个模块";
            ack_message           = modules_message;
            ok                    = done == ids.size();
            if (!ok) {
                error = first_error.empty() ? "部分模块导入失败" : first_error;
            }
        }
    } else if (has_type && type == "import_all") {
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else {
            std::vector<std::string> ids;
            for (const vrcft::ImportCandidate& candidate : ctx_.modules->importable()) {
                ids.push_back(candidate.info.id);
            }
            std::size_t done = 0;
            std::string first_error;
            for (const std::string& id : ids) {
                std::string import_error;
                if (ctx_.modules->import_module(id, import_error)) {
                    ++done;
                } else if (first_error.empty()) {
                    first_error = import_error;
                }
            }
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            modules_message       = "已导入 " + std::to_string(done) + " 个模块";
            ok                    = done == ids.size();
            if (!ok) {
                error = first_error.empty() ? "部分模块导入失败" : first_error;
            }
        }
    } else if (has_type && type == "install_module_archive") {
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (picker_ != nullptr && picker_->running.load()) {
            error = "文件选择器已经打开";
        } else {
            start_picker();
            modules_dirty_only = true;
            ok                 = true;
        }
    } else if (has_type && type == "open_modules_dir") {
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else {
#ifdef _WIN32
            ::ShellExecuteW(nullptr, L"open", ctx_.modules->store_dir().c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
#endif
            ok = true;
        }
    } else if (has_type && type == "restart_engine") {
        if (ctx_.restart_engine != nullptr && ctx_.quit != nullptr) {
            ctx_.restart_engine->store(true, std::memory_order_release);
            ctx_.quit->store(true, std::memory_order_release);
            engine_restarting = true;
            ok                = true;
        } else {
            error = "restart not available";
        }
    } else if (has_type && type == "refresh_library") {
        if (library_ == nullptr) {
            error = "library not available";
        } else if (library_->refresh()) {
            library_touched = true;
            ok              = true;
        } else {
            error = "模块库正在刷新或下载，稍后再试";
        }
    } else if (has_type && type == "install_library") {
        std::string id;
        json::field_string(text, "id", id);
        const vrcft::LibrarySnapshot snapshot =
            library_ != nullptr ? library_->snapshot() : vrcft::LibrarySnapshot{};
        const bool downloading = snapshot.status.phase == "downloading" ||
                                 snapshot.status.phase == "installing";
        if (library_ == nullptr) {
            error = "library not available";
        } else if (id.empty()) {
            error = "missing module id";
        } else if (downloading) {
            error = "模块库正在下载，稍后再试";
        } else {
            // 更新要替换的 DLL 正被运行中的实例加载着：先把它们停下，装完再拉起来。
            // 用户点确认时弹窗已经写明这一步，不需要他再去实例行手动点「停止」。
            ModuleJob job;
            job.module_id = id;
            job.update    = ctx_.modules != nullptr && ctx_.modules->find(id) != nullptr;
            for (const vrcft::LibraryEntry& entry : snapshot.entries) {
                if (entry.id == id) {
                    job.name    = entry.name;
                    job.version = entry.version;
                    break;
                }
            }
            job.stopped_instances = stop_module_instances(id);
            if (library_->install(id)) {
                job.active      = true;
                vrcft_job_      = job;
                library_touched = true;
                ok              = true;
                ack_message     = job.stopped_instances.empty()
                                      ? (job.update ? "正在下载更新包…" : "正在下载安装包…")
                                      : "已停止运行中的实例，正在下载" +
                                            std::string(job.update ? "更新包…" : "安装包…");
            } else {
                const vrcft::LibrarySnapshot failed = library_->snapshot();
                error = failed.status.error.empty() ? "模块库正在刷新或下载，稍后再试"
                                                    : failed.status.error;
                // 下载都没开始就把实例恢复了，别让用户白白丢追踪
                const std::string restore = restart_instances(job.stopped_instances, nullptr);
                if (!restore.empty()) {
                    error += "；" + restore;
                }
                ack_message     = "安装未开始：" + error;
                library_touched = true;
            }
        }
    } else if (has_type && type == "refresh_native_library") {
        if (native_library_ == nullptr) {
            error = "native library not available";
        } else if (native_library_->refresh()) {
            library_touched = true;
            ok              = true;
        } else {
            error = "原生模块库正在刷新，稍后再试";
        }
    } else if (has_type && type == "install_native_library") {
        std::string id;
        std::string version;
        json::field_string(text, "id", id);
        json::field_string(text, "version", version);
        const vrcft::NativeLibrarySnapshot snapshot =
            native_library_ != nullptr ? native_library_->snapshot() : vrcft::NativeLibrarySnapshot{};
        const bool downloading = snapshot.status.phase == "downloading" ||
                                 snapshot.status.phase == "installing";
        if (native_library_ == nullptr) {
            error = "native library not available";
        } else if (id.empty()) {
            error = "missing module id";
        } else if (downloading) {
            error = "原生模块库正在下载，稍后再试";
        } else {
            ModuleJob job;
            job.module_id = id;
            job.update    = ctx_.modules != nullptr && ctx_.modules->find(id) != nullptr;
            for (const vrcft::NativeEntry& entry : snapshot.registry.modules) {
                if (entry.id == id) {
                    job.name = entry.name;
                    const vrcft::NativeVersion* selected = vrcft::select_version(entry, version);
                    job.version = selected != nullptr ? selected->version : entry.latest.version;
                    break;
                }
            }
            job.stopped_instances = stop_module_instances(id);
            if (native_library_->install(id, version)) {
                job.active      = true;
                native_job_     = job;
                library_touched = true;
                ok              = true;
                ack_message     = job.stopped_instances.empty()
                                      ? (job.update ? "正在下载更新包…" : "正在下载安装包…")
                                      : "已停止运行中的实例，正在下载" +
                                            std::string(job.update ? "更新包…" : "安装包…");
            } else {
                // install() 把"条目不存在/版本不存在/不兼容/正忙"写进了状态，
                // 界面已经能看到；ack 里也带上同一句，避免点了没反应。
                const vrcft::NativeLibrarySnapshot failed = native_library_->snapshot();
                error = failed.status.error.empty() ? "原生模块库正在下载，稍后再试"
                                                    : failed.status.error;
                const std::string restore = restart_instances(job.stopped_instances, nullptr);
                if (!restore.empty()) {
                    error += "；" + restore;
                }
                ack_message     = "安装未开始：" + error;
                library_touched = true;
            }
        }
    } else if (has_type && type == "export_diagnostics") {
        if (diag_running_.load(std::memory_order_acquire)) {
            error = "诊断包正在导出，请稍候";
        } else {
            std::string output;
            json::field_string(text, "out", output);
            start_diagnostics_export(output.empty() ? std::filesystem::path{}
                                                    : config::path_from_utf8(output));
            ok          = true;
            ack_message = "正在导出诊断包…";
        }
    } else if (has_type && type == "check_updates") {
        std::string url;
        json::field_string(text, "url", url);
        if (updater_ == nullptr) {
            error = "更新不可用（没有用户数据目录）";
        } else if (updater_->busy()) {
            error = "更新检查或下载正在进行";
        } else {
            // 手动检查一律绕过节流；skipped_version 也不拦（用户可能想反悔）
            updater_->start_check(url.empty() ? update::kDefaultUpdateUrl : url, true);
            ok          = true;
            ack_message = "正在检查更新…";
        }
    } else if (has_type && type == "install_update") {
        std::string install_error;
        if (updater_ == nullptr) {
            error = "更新不可用（没有用户数据目录）";
        } else if (!updater_->start_install(install_error)) {
            error = install_error;
        } else {
            ok                = true;
            ack_message       = "正在安装，程序即将重启";
            update_installing = true;
        }
    } else if (has_type && type == "skip_update_version") {
        if (updater_ == nullptr) {
            error = "更新不可用（没有用户数据目录）";
        } else {
            updater_->skip_current_version();
            ok          = true;
            ack_message = "已忽略这个版本";
        }
    } else if (has_type && type == "open_update_page") {
        std::string url;
        json::field_string(text, "url", url);
        constexpr std::string_view kAuthor = "https://github.com/Aqamoe";
        const bool allowed = url == kAuthor ||
            (url.size() > kAuthor.size() &&
             url.compare(0, kAuthor.size(), kAuthor) == 0 && url[kAuthor.size()] == '/');
        if (!allowed) {
            error = "只允许打开 Aqamoe 的 GitHub 页面";
        } else {
            const int length = ::MultiByteToWideChar(CP_UTF8, 0, url.data(),
                                                     static_cast<int>(url.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()),
                                  wide.data(), length);
            const auto result = reinterpret_cast<INT_PTR>(
                ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (result <= 32) {
                error = "无法打开浏览器";
            } else {
                ok = true;
            }
        }
    } else if (has_type && type == "open_module_page") {
        std::string url;
        json::field_string(text, "url", url);
        if (!known_module_page(ctx_.modules, library_.get(), native_library_.get(), url)) {
            error = "链接不在已安装模块或模块库的元数据里";
        } else if (!open_in_browser(url)) {
            error = "无法打开浏览器";
        } else {
            ok = true;
        }
    } else if (has_type && type == "set_log_level") {
        std::string level_text;
        log::Level level = log::level();
        if (json::field_string(text, "level", level_text) && parse_level(level_text, level)) {
            log::set_level(level);
            ok = true;
        } else {
            error = "unknown level";
        }
    } else if (has_type && type == "ping") {
        server_.send_text(conn, R"({"type":"pong"})");
        return;
    } else if (has_type && type == "quit") {
        if (ctx_.quit != nullptr) {
            ctx_.quit->store(true, std::memory_order_release);
            ok = true;
        } else {
            error = "quit not available";
        }
    } else {
        error = "unknown command";
    }

    std::string reply;
    JsonWriter writer{reply};
    writer.obj_begin();
    writer.key("type").str("ack");
    writer.key("command").str(type);
    writer.key("ok").boolean(ok);
    if (!error.empty()) {
        writer.key("error").str(error);
    }
    if (!ack_message.empty()) {
        writer.key("message").str(ack_message);
    }
    if (engine_restarting) {
        writer.key("restarting").boolean(true);
    }
    if (outputs_touched) {
        writer.key("restart_needed").boolean(true);
    }
    writer.obj_end();
    server_.send_text(conn, reply);

    if (update_installing) {
        // 顺序很重要：先把"正在安装"推出去并真的发一帧，再让引擎退出（退出码 4）。
        // 反过来的话界面一个字都收不到，用户只会觉得"点了没反应"。
        std::string message;
        build_update(message);
        server_.broadcast(message);
        server_.poll(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        server_.poll(0);
        if (ctx_.apply_update != nullptr) {
            ctx_.apply_update->store(true, std::memory_order_release);
        }
        if (ctx_.quit != nullptr) {
            ctx_.quit->store(true, std::memory_order_release);
        }
        FGB_INFO(kTag, "安装器已启动，引擎退出以覆盖安装");
        return;
    }

    // 忽略某个版本后立刻推一次：多开页面时各自状态要一致
    if (ok && type == "skip_update_version") {
        broadcast_update();
    }

    if (modules_touched) {
        after_module_change(ok, ok ? modules_message : error);
    } else if (modules_dirty_only && ok) {
        std::string message;
        build_modules(message);
        server_.broadcast(message);
    }
    // 失败也要推：界面靠 status.phase/error 显示"为什么没开始"，不能只弹一句 toast
    if (library_touched) {
        std::string message;
        build_library(message);
        server_.broadcast(message);
    }
    if (ok && (type == "set_filter" || outputs_touched)) {
        std::string status;
        build_status(status);
        server_.broadcast(status);
    }
    if (ok && (profiles_touched || outputs_touched)) {
        broadcast_output_profiles();
    }
    if (calibration_changed) {
        // 无论成功与否都推一次：失败时（例如跨度太小没启用）页面要看到真实状态，
        // 不能停在用户以为成功的那一版
        broadcast_calibration();
    }
    if (steamvr_touched) {
        // 立刻推一次 busy 状态：多个页面同时开着时，按钮要一起禁用
        std::string message;
        build_steamvr(message);
        server_.broadcast(message);
    }
}

bool IpcServer::handle_http(const net::ws::HttpHead& head, std::string& body,
                            std::string& content_type) {
    content_type = "application/json; charset=utf-8";

    // 控制类接口只给本机回环，GET/POST 都接受：外壳用最简单的 socket 发 POST，
    // curl 排障时懒得带 -X 也能用（照旧只绑 127.0.0.1）。
    if (head.method != "GET" && head.method != "POST") {
        return false;
    }

    constexpr std::string_view kIconPrefix = "/module-icon/";
    if (head.path.rfind(kIconPrefix, 0) == 0) {
        std::string_view id = std::string_view{head.path}.substr(kIconPrefix.size());
        const std::size_t query = id.find('?');
        if (query != std::string_view::npos) {
            id = id.substr(0, query);
        }
        return serve_module_icon(id, body, content_type);
    }

    if (head.path == "/api/quit" || head.path == "/api/restart") {
        // 托盘外壳据此实现「停止引擎 / 重启引擎」，不需要理解 WebSocket 协议
        if (ctx_.quit == nullptr) {
            return false;
        }
        const bool restart = head.path == "/api/restart";
        if (restart && ctx_.restart_engine == nullptr) {
            return false;
        }
        ctx_.quit->store(true, std::memory_order_release);
        if (restart) {
            ctx_.restart_engine->store(true, std::memory_order_release);
        }
        FGB_INFO(kTag, "%s", restart ? "收到控制面请求：重启引擎" : "收到控制面请求：停止引擎");
        body = R"({"ok":true})";
        return true;
    }

    if (head.path == "/api/status") {
        build_metrics(body);
        return true;
    }

    if (head.path == "/api/calibration") {
        build_calibration(body);
        return true;
    }

    // 只读预览：探针用 curl 就能断言"原始值/输出值与页面一致"，不必先起 WebSocket
    if (head.path == "/api/preview") {
        build_preview(body);
        return true;
    }

    // SteamVR 状态快照：和 WS 的 steamvr 消息同源；动作只能走 WS 命令
    if (head.path == "/api/steamvr") {
        build_steamvr(body);
        return true;
    }

    // 更新状态快照：探针用 curl 就能断言"下载到哪一步"，不必先起 WebSocket
    if (head.path == "/api/update") {
        build_update(body);
        return true;
    }

    if (head.path == "/api/gaze") {
        build_gaze(body);
        return true;
    }

    if (head.path == "/api/params") {
        // curl 用：名字与数值一次给全，不依赖先收 catalog
        const FaceFrame frame       = ctx_.engine != nullptr ? ctx_.engine->latest()
                                                             : FaceFrame::neutral();
        const params::EvalContext eval{frame, 30.0f};
        body.clear();
        JsonWriter writer{body};
        writer.obj_begin();
        writer.key("params").arr_begin();
        for (const params::Group& group : params::groups()) {
            for (const params::ParamEntry& entry : group.entries) {
                writer.obj_begin();
                std::string name{group.name_prefix};
                name.append(entry.name);
                writer.key("name").str(name);
                writer.key("value").num(static_cast<double>(entry.eval(eval)), 3);
                writer.obj_end();
            }
        }
        writer.arr_end();
        writer.obj_end();
        return true;
    }

    if (head.path == "/healthz") {
        body.clear();
        JsonWriter writer{body};
        writer.obj_begin();
        writer.key("ok").boolean(true);
        writer.key("clients").integer(static_cast<long long>(server_.ready_count()));
        writer.obj_end();
        return true;
    }

    return serve_static(head.path, body, content_type);
}

bool IpcServer::serve_static(std::string_view path, std::string& body,
                             std::string& content_type) const {
    if (ctx_.ui_dir.empty()) {
        return false;
    }

    std::string relative{path};
    if (relative == "/" || relative.empty()) {
        relative = "/index.html";
    }
    if (relative.size() > 512 || relative.find("..") != std::string::npos ||
        relative.find('\\') != std::string::npos || relative.find('\0') != std::string::npos) {
        return false;
    }
    if (relative.front() == '/') {
        relative.erase(0, 1);
    }

    const std::string extension = std::filesystem::path(relative).extension().string();
    const std::string_view mime = mime_type(extension);
    if (mime.empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(ctx_.ui_dir, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path target = std::filesystem::weakly_canonical(root / relative, ec);
    if (ec) {
        return false;
    }

    // 前缀校验：即使请求里混进了软链接，也不能读到 ui 目录之外。
    // 必须要求分隔符，否则 root=C:\app\ui 会放行兄弟目录 C:\app\uievil。
    const std::wstring root_text   = root.wstring();
    const std::wstring target_text = target.wstring();
    if (target_text.size() <= root_text.size() ||
        target_text.compare(0, root_text.size(), root_text) != 0) {
        return false;
    }
    const wchar_t next = target_text[root_text.size()];
    if (next != L'\\' && next != L'/') {
        return false;
    }

    std::ifstream input(target, std::ios::binary);
    if (!input) {
        return false;
    }
    body.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    content_type = std::string{mime};
    return true;
}

bool IpcServer::serve_module_icon(std::string_view encoded_id, std::string& body,
                                  std::string& content_type) const {
    if (ctx_.modules == nullptr) {
        return false;
    }
    std::string id;
    if (!percent_decode(encoded_id, id) || id.empty() || id.size() > 256) {
        return false;
    }
    for (const char ch : id) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7F || ch == '/' || ch == '\\') {
            return false;
        }
    }

    const vrcft::ModuleInfo* module = ctx_.modules->find(id);
    if (module == nullptr) {
        return false;
    }

    if (module->icon.empty()) {
        // 模块没带图标文件时，从 DLL 的 Win32 图标资源取（VRCFT 模块常见）
        if (module->dll.empty()) {
            return false;
        }
        std::error_code stamp_ec;
        const std::uintmax_t dll_size = std::filesystem::file_size(module->dll, stamp_ec);
        const auto modified = std::filesystem::last_write_time(module->dll, stamp_ec);
        const std::string stamp = module->dll.string() + "|" + std::to_string(dll_size) + "|" +
                                  std::to_string(modified.time_since_epoch().count());
        const auto cached = icon_cache_.find(module->id);
        if (cached != icon_cache_.end() && cached->second.stamp == stamp) {
            if (cached->second.bytes.empty()) {
                return false; // 上次提取失败（DLL 没图标），别每次刷新都重新 LoadLibrary
            }
            body = cached->second.bytes;
            content_type = cached->second.content_type;
            return true;
        }
        std::string icon;
        std::string mime;
        if (!vrcft::read_dll_icon(module->dll, icon, mime) || icon.empty() ||
            icon.size() > 1024 * 1024) {
            icon_cache_[module->id] = IconCacheEntry{stamp, {}, {}};
            return false;
        }
        IconCacheEntry entry;
        entry.stamp        = stamp;
        entry.content_type = std::move(mime);
        entry.bytes        = std::move(icon);
        auto inserted = icon_cache_.insert_or_assign(module->id, std::move(entry));
        body = inserted.first->second.bytes;
        content_type = inserted.first->second.content_type;
        return true;
    }

    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(module->directory, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path target = std::filesystem::weakly_canonical(module->icon, ec);
    if (ec) {
        return false;
    }
    // 扫描时已经校验过，这里再挡一次：图标路径被替换/软链接后也不能读出模块目录
    const std::wstring root_text   = root.wstring();
    const std::wstring target_text = target.wstring();
    if (target_text.size() <= root_text.size() ||
        target_text.compare(0, root_text.size(), root_text) != 0) {
        return false;
    }
    const wchar_t next = target_text[root_text.size()];
    if (next != L'\\' && next != L'/') {
        return false;
    }

    std::string extension = target.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string_view mime = mime_type(extension);
    if (mime.rfind("image/", 0) != 0) {
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(target, ec);
    if (ec || size == 0 || size > 1024 * 1024) {
        return false;
    }
    std::ifstream input(target, std::ios::binary);
    if (!input) {
        return false;
    }
    body.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    content_type = std::string{mime};
    return true;
}

void IpcServer::start_picker() {
#ifdef _WIN32
    auto state = std::make_shared<PickerState>();
    picker_    = state;
    std::thread([state] {
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner   = ::GetForegroundWindow();
        dialog.lpstrFilter = L"模块安装包 (*.zip;*.dll)\0*.zip;*.dll\0所有文件 (*.*)\0*.*\0\0";
        dialog.lpstrFile   = file;
        dialog.nMaxFile    = MAX_PATH;
        dialog.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (::GetOpenFileNameW(&dialog) == TRUE) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->path = wide_to_utf8(file);
            state->ready.store(true);
        }
        state->running.store(false);
        ::CoUninitialize();
    }).detach();
#else
    auto state = std::make_shared<PickerState>();
    state->running.store(false);
    picker_ = state;
#endif
}

std::vector<std::string> IpcServer::stop_module_instances(const std::string& module_id) {
    std::vector<std::string> stopped;
    if (ctx_.engine == nullptr || module_id.empty()) {
        return stopped;
    }
    for (const RunningModule& module : ctx_.running_modules) {
        if (module.source == nullptr || module.module_id != module_id) {
            continue;
        }
        // 已经停了的实例不必再动（再停一次也没事，但会让"已停止 N 个"变成假话）
        const auto status = module.source->module_status();
        if (status.has_value() && !status->alive) {
            continue;
        }
        std::string stop_error;
        if (!ctx_.engine->stop_source(module.display_name, stop_error)) {
            FGB_WARN(kTag, "停止实例 %s 失败：%s", module.display_name.c_str(),
                     stop_error.c_str());
            continue;
        }
        stopped.push_back(module.display_name);
    }
    return stopped;
}

bool IpcServer::start_instance(const std::string& display_name, std::string& error) {
    if (ctx_.engine == nullptr) {
        error = "引擎未运行，实例无法自动恢复";
        return false;
    }
    return ctx_.engine->start_source(display_name, error);
}

// 模块被删掉之后，它的实例不可能再起来。只 stop 不摘的话，界面的「运行中的实例」
// 「输入源」「输入帧率」会一直挂着一个已经不存在的模块——用户会以为没删干净。
void IpcServer::forget_module_instances(const std::string& module_id) {
    for (auto it = ctx_.running_modules.begin(); it != ctx_.running_modules.end();) {
        if (it->module_id != module_id) {
            ++it;
            continue;
        }
        const std::string name = it->display_name;
        std::string remove_error;
        if (ctx_.engine != nullptr && !name.empty() &&
            !ctx_.engine->remove_source(name, remove_error)) {
            // 摘不掉也不要紧：条目已经从 running_modules 里删了，界面不会再显示它，
            // 但源还在合并器里就会继续喂数据，所以这条必须留在日志里。
            FGB_WARN(kTag, "摘除实例 %s 的输入源失败：%s", name.c_str(), remove_error.c_str());
        }
        it = ctx_.running_modules.erase(it);
        ctx_.source_names.erase(
            std::remove(ctx_.source_names.begin(), ctx_.source_names.end(), name),
            ctx_.source_names.end());
    }

    // status.source 是启动时拼好的人话（"native:A+native:B"），删完要重拼，
    // 否则输入帧率卡片还会显示那个刚被删掉的模块名。
    std::string joined;
    for (const std::string& name : ctx_.source_names) {
        if (!joined.empty()) {
            joined += "+";
        }
        joined += name;
    }
    ctx_.source_name = joined;
}

std::string IpcServer::restart_instances(const std::vector<std::string>& names,
                                         std::size_t* restored) {
    std::string first_error;
    std::size_t done = 0;
    for (const std::string& name : names) {
        std::string start_error;
        if (start_instance(name, start_error)) {
            ++done;
        } else if (first_error.empty()) {
            first_error = "实例「" + name + "」恢复失败：" + start_error;
        }
    }
    if (restored != nullptr) {
        *restored = done;
    }
    return first_error;
}

void IpcServer::broadcast_notice(bool ok, const std::string& text) {
    if (ok) {
        FGB_INFO(kTag, "%s", text.c_str());
    } else {
        FGB_WARN(kTag, "%s", text.c_str());
    }
    std::string out;
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("notice");
    writer.key("level").str(ok ? "info" : "error");
    writer.key("message").str(text);
    writer.obj_end();
    server_.broadcast(out);
}

// 异步安装（模块库先下载再落盘）的结果只有这里知道：下载线程不碰模块仓，
// 落盘发生在 poll()。装完必须主动告诉界面，否则用户只能去日志里找。
void IpcServer::finish_module_job(bool native, bool ok, const std::string& error) {
    ModuleJob& job = job_for(native);
    if (!job.active) {
        return;
    }
    const std::string name    = job.name.empty() ? job.module_id : job.name;
    const std::string version = job.version.empty() ? std::string{} : " v" + job.version;
    std::string text;
    if (ok) {
        text = "「" + name + "」" + (job.update ? "已更新到" : "已安装") + version;
    } else {
        text = "「" + name + "」" + (job.update ? "更新失败：" : "安装失败：") +
               (error.empty() ? "未知错误" : error);
    }

    std::size_t restored      = 0;
    const std::string failed  = restart_instances(job.stopped_instances, &restored);
    if (!job.stopped_instances.empty()) {
        if (failed.empty()) {
            text += "；已自动恢复 " + std::to_string(restored) + " 个运行中的实例";
        } else {
            text += "；" + failed;
        }
    }
    job = ModuleJob{};

    // 成功装完要刷新模块列表（新版本号/新模块），失败也要让卡片回到可重试状态
    std::string message;
    build_modules(message);
    server_.broadcast(message);
    build_status(message);
    server_.broadcast(message);
    broadcast_notice(ok && failed.empty(), text);
}

void IpcServer::poll_picker() {
    if (!picker_ || picker_->running.load()) {
        return;
    }
    const auto state = picker_;
    picker_.reset();
    if (state->ready.load() && ctx_.modules != nullptr) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            path = state->path;
        }

        // 手动装包同样可能是覆盖升级：先探出目标模块，它要是正跑着就先停实例，
        // 否则旧 DLL 被锁，用户只会拿到一句"无法替换旧版本"。
        std::string target_id;
        std::string peek_error;
        std::vector<std::string> stopped;
        if (ctx_.modules->peek_package_id(path, target_id, peek_error)) {
            stopped = stop_module_instances(target_id);
        }

        std::string error;
        vrcft::InstallOutcome outcome;
        // replace=true：同一个 ModuleId 的新包就是这个模块的升级包。
        // 手动安装（尤其是原生模块，没有在线注册表）必须能覆盖旧版本，
        // 否则作者改了 Version 重新打包后永远装不进去。
        const bool ok =
            ctx_.modules->install_path(path, error, "zip", true, nullptr, &outcome);
        std::string message;
        if (ok) {
            const std::string name = outcome.name.empty() ? outcome.id : outcome.name;
            if (outcome.upgraded) {
                message = name + " 已升级";
                if (!outcome.previous_version.empty()) {
                    message += " v" + outcome.previous_version;
                }
                if (!outcome.version.empty()) {
                    message += (outcome.previous_version.empty() ? " v" : " → v") + outcome.version;
                }
            } else if (!outcome.version.empty()) {
                message = name + " 已安装 v" + outcome.version;
            } else {
                message = name + " 已安装";
            }
        } else {
            message = "安装失败：" + (error.empty() ? std::string("未知错误") : error);
        }

        std::size_t restored     = 0;
        const std::string failed = restart_instances(stopped, &restored);
        if (!stopped.empty()) {
            if (failed.empty()) {
                message += "；已自动恢复 " + std::to_string(restored) + " 个运行中的实例";
            } else {
                message += "；" + failed;
            }
        }
        after_module_change(ok, ok ? message : (error.empty() ? message : error));
        // 成功和失败都要让用户看见：装完只写日志，用户会以为点了没反应
        broadcast_notice(ok && failed.empty(), message);
    } else {
        std::string message;
        build_modules(message);
        server_.broadcast(message); // 只是把 picking 状态复位
    }
}

void IpcServer::after_module_change(bool ok, const std::string& message) {
    if (ctx_.modules != nullptr) {
        ctx_.modules->rescan();
        if (ok && ctx_.modules->active_id().empty()) {
            for (const vrcft::ModuleInfo& module : ctx_.modules->modules()) {
                if (module.enabled && !module.broken && ctx_.modules->set_active(module.id)) {
                    break;
                }
            }
        }
        ctx_.active_module_id = ctx_.modules->active_id();
    }
    if (ok) {
        FGB_INFO(kTag, "%s", message.c_str());
    } else {
        FGB_WARN(kTag, "%s", message.c_str());
    }
    std::string modules;
    build_modules(modules);
    server_.broadcast(modules);
    std::string status;
    build_status(status);
    server_.broadcast(status);
}

void IpcServer::pump(int timeout_ms) {
    server_.poll(timeout_ms);
    flush_logs();
    poll_picker();

    {
        std::lock_guard<std::mutex> lock(diag_mutex_);
        if (diag_result_pending_) {
            server_.broadcast(diag_result_);
            diag_result_.clear();
            diag_result_pending_ = false;
        }
    }
    tick_auto_update();

    // SteamVR 操作在后台线程：完成的这一刻由主循环统一广播，避免跨线程 send
    if (steamvr_ != nullptr) {
        steamvr::Status result;
        if (steamvr_->take(result)) {
            std::string message;
            build_steamvr(message);
            server_.broadcast(message);
        }
    }

    // 模块库的后台线程与界面是否连着无关：界面断开时下载也要能装完
    if (library_ != nullptr || native_library_ != nullptr) {
        vrcft::LibraryEvents events;
        std::string vrcft_phase  = last_vrcft_phase_;
        std::string native_phase = last_native_phase_;
        std::string vrcft_error;
        std::string native_error;
        if (library_ != nullptr) {
            events = library_->poll();
            const vrcft::LibrarySnapshot snapshot = library_->snapshot();
            vrcft_phase = snapshot.status.phase;
            vrcft_error = snapshot.status.error;
        }
        if (native_library_ != nullptr) {
            const vrcft::NativeLibraryEvents native_events = native_library_->poll();
            events.changed         = events.changed || native_events.changed;
            events.modules_changed = events.modules_changed || native_events.modules_changed;
            const vrcft::NativeLibrarySnapshot snapshot = native_library_->snapshot();
            native_phase = snapshot.status.phase;
            native_error = snapshot.status.error;
        }
        if (events.modules_changed) {
            // 具体的名字/版本由 finish_module_job 组合成人话播出去，这里只做状态重算
            after_module_change(true, "模块库安装完成");
        }
        if (events.changed) {
            std::string message;
            build_library(message);
            server_.broadcast(message);
        }
        // 装完（成功或失败）要把代停的实例拉回来，并给用户一句明确的结果；
        // 这两步都只在相位从"下载/安装中"落到"空闲/失败"的那一拍做一次。
        if (library_ != nullptr && install_phase_finished(last_vrcft_phase_, vrcft_phase)) {
            finish_module_job(false, vrcft_phase == "idle", vrcft_error);
        }
        if (native_library_ != nullptr &&
            install_phase_finished(last_native_phase_, native_phase)) {
            finish_module_job(true, native_phase == "idle", native_error);
        }
        last_vrcft_phase_  = vrcft_phase;
        last_native_phase_ = native_phase;
    }

    if (server_.ready_count() == 0) {
        // 页面全走了：停掉取样，引擎热路径回到零额外开销
        if (preview_wanted_) {
            preview_wanted_ = false;
            if (ctx_.preview != nullptr) {
                ctx_.preview->set_enabled(false);
            }
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (preview_wanted_ && ctx_.preview != nullptr && now - last_preview_ >= kPreviewPeriod) {
        std::string message;
        build_preview(message);
        server_.broadcast(message);
        last_preview_ = now;
    }

    const bool library_busy =
        (library_ != nullptr && library_->busy()) ||
        (native_library_ != nullptr && native_library_->busy());
    if (library_busy && now - last_library_push_ >= std::chrono::milliseconds(200)) {
        std::string message;
        build_library(message);
        server_.broadcast(message);
        last_library_push_ = now;
    }

    // fps 用两次推送之间的帧数增量算：不用给引擎加埋点，也不受 UI 刷新率影响
    const double dt = std::chrono::duration<double>(now - last_status_).count();
    const EngineStats stats = ctx_.engine != nullptr ? ctx_.engine->stats() : EngineStats{};
    if (dt > 0.02) {
        const std::uint64_t delta = stats.frames_in >= last_frames_ ? stats.frames_in - last_frames_ : 0;
        last_fps_ = static_cast<double>(delta) / dt;
    }

    if (now - last_status_ >= kStatusPeriod) {
        std::string message;
        build_status(message);
        server_.broadcast(message);
        last_status_ = now;
        last_frames_ = stats.frames_in;
    }

    if (now - last_params_ >= kParamsPeriod) {
        std::string message;
        build_params(message);
        server_.broadcast(message);
        last_params_ = now;
    }

    if (now - last_gaze_ >= kGazePeriod) {
        std::string message;
        build_gaze(message);
        server_.broadcast(message);
        last_gaze_ = now;
    }
}

} // namespace fgb::web
