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
    }

    // SteamVR 卡片只在发布布局（app.vrmanifest 与 exe 同目录）下可用；
    // 命令行开发实例没有 exe_dir 时直接不挂服务，界面显示"不可用"。
    if (!ctx_.exe_dir.empty()) {
        steamvr_ = std::make_unique<steamvr::Service>(ctx_.exe_dir);
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
    if (update_thread_.joinable()) {
        update_thread_.join();
    }
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
        writer.obj_end();
    }
    writer.arr_end();

    writer.key("eye").obj_begin();
    writer.key("active").boolean(profile.has_eye);
    writer.key("gain_x").num(static_cast<double>(profile.eye_gain_x), 3);
    writer.key("gain_y").num(static_cast<double>(profile.eye_gain_y), 3);
    writer.key("center_x").num(static_cast<double>(profile.eye_center_x), 3);
    writer.key("center_y").num(static_cast<double>(profile.eye_center_y), 3);
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

void IpcServer::start_update_check(std::string url) {
    if (update_running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (update_thread_.joinable()) {
        update_thread_.join();
    }

    update_thread_ = std::thread([this, url = std::move(url)] {
        update::ReleaseInfo release;
        std::string error;
        const bool ok = update::check_latest(url, release, error);
        const bool available =
            ok && update::compare_versions(release.tag, FGB_VERSION) > 0;

        std::string message;
        JsonWriter writer{message};
        writer.obj_begin();
        writer.key("type").str("update");
        writer.key("ok").boolean(ok);
        writer.key("current").str(FGB_VERSION);
        writer.key("tag").str(release.tag);
        writer.key("latest").str(release.tag);
        writer.key("name").str(release.name);
        writer.key("url").str(release.url);
        writer.key("available").boolean(available);
        if (!error.empty()) {
            writer.key("error").str(error);
        }
        writer.obj_end();

        {
            std::lock_guard<std::mutex> lock(update_mutex_);
            update_result_         = std::move(message);
            update_result_pending_ = true;
        }
        update_running_.store(false, std::memory_order_release);
        FGB_INFO(kTag, "更新检查：%s", ok ? (available ? "有新版本" : "已是最新") : error.c_str());
    });
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
            bool found = false;
            for (const RunningModule& running : ctx_.running_modules) {
                if (running.module_id != instance.module_id ||
                    running.host_process != instance.host_process) {
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
            writer.key("page").str(module.page_url);
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
            if (json::field_number(text, "lo", value)) {
                range.lo = clamp01(static_cast<float>(value));
            }
            if (json::field_number(text, "hi", value)) {
                range.hi = clamp01(static_cast<float>(value));
            }
            if (range.lo > range.hi) {
                std::swap(range.lo, range.hi); // 拖过头就交换，而不是让用户猜为什么没反应
            }
            bool active = range.active;
            if (json::field_bool(text, "active", active)) {
                range.active = active;
                // 刚启用的形状如果区间是"没调过"的状态（跨度近 0），给一个能用的默认：
                // 否则用户勾了启用、拖滑杆，输出却一点没变
                if (active && range.hi - range.lo < 0.02f) {
                    range.lo = 0.0f;
                    range.hi = 1.0f;
                }
            }
            ctx_.calib->set_profile(profile); // 实时生效：预览与实际输出同时变
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
                    if (range.hi - range.lo >= 0.02f) {
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
        json::field_bool(text, "host_process", host_process);
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (id.empty() || name.empty()) {
            error = "missing module id or instance name";
        } else if (ctx_.modules->add_instance(name, id, host_process)) {
            modules_touched = true;
            modules_message = "启动实例已添加（重启引擎后生效）";
            ack_message     = "实例 " + name + " 已添加";
            ok              = true;
        } else {
            error = "无法添加实例（名字重复或模块不可用）";
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
                error = "unknown instance";
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
                error = "unknown instance";
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
        const bool running =
            std::any_of(ctx_.running_modules.begin(), ctx_.running_modules.end(),
                        [&id](const RunningModule& module) { return module.module_id == id; });
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (running) {
            // DLL 正被模块进程加载，直接删会失败。标记为"停机后删除"，
            // 引擎停止（模块进程退出）后由 cmd_run 执行，然后自动重启。
            pending_delete_id_ = id;
            if (ctx_.restart_engine != nullptr) {
                ctx_.restart_engine->store(true, std::memory_order_release);
            }
            if (ctx_.quit != nullptr) {
                ctx_.quit->store(true, std::memory_order_release);
            }
            ack_message      = "模块正在运行：停止引擎、删除文件后会自动重启";
            engine_restarting = true;
            ok                = true;
        } else {
            std::string remove_error;
            const bool removed = ctx_.modules->remove_module(id, remove_error);
            ctx_.active_module_id = ctx_.modules->active_id();
            modules_touched       = true;
            modules_message       = removed ? "模块已删除" : remove_error;
            ok                    = removed;
            if (!removed) {
                error = remove_error;
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
        if (library_ == nullptr) {
            error = "library not available";
        } else if (id.empty()) {
            error = "missing module id";
        } else if (library_->install(id)) {
            library_touched = true;
            ok              = true;
        } else {
            error = "模块库正在刷新或下载，稍后再试";
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
        if (url.empty()) {
            url = update::kDefaultUpdateUrl;
        }
        if (update_running_.load(std::memory_order_acquire)) {
            error = "更新检查正在进行";
        } else {
            start_update_check(url);
            ok          = true;
            ack_message = "正在检查更新…";
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

    if (modules_touched) {
        after_module_change(ok, ok ? modules_message : error);
    } else if (modules_dirty_only && ok) {
        std::string message;
        build_modules(message);
        server_.broadcast(message);
    }
    if (library_touched && ok) {
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

std::string IpcServer::take_pending_delete() {
    std::string id = std::move(pending_delete_id_);
    pending_delete_id_.clear();
    return id;
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
        std::string error;
        vrcft::InstallOutcome outcome;
        // replace=true：同一个 ModuleId 的新包就是这个模块的升级包。
        // 手动安装（尤其是原生模块，没有在线注册表）必须能覆盖旧版本，
        // 否则作者改了 Version 重新打包后永远装不进去。
        const bool ok =
            ctx_.modules->install_path(path, error, "zip", true, nullptr, &outcome);
        std::string message = "模块已安装";
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
        }
        after_module_change(ok, ok ? message : error);
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
    {
        std::lock_guard<std::mutex> lock(update_mutex_);
        if (update_result_pending_) {
            server_.broadcast(update_result_);
            update_result_.clear();
            update_result_pending_ = false;
        }
    }

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
    if (library_ != nullptr) {
        const vrcft::LibraryEvents events = library_->poll();
        if (events.modules_changed) {
            after_module_change(true, "模块库安装完成");
        }
        if (events.changed) {
            std::string message;
            build_library(message);
            server_.broadcast(message);
        }
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

    if (library_ != nullptr && library_->busy() &&
        now - last_library_push_ >= std::chrono::milliseconds(200)) {
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
