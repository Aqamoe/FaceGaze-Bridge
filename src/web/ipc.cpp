#include "web/ipc.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "params/avatar_config.hpp"
#include "params/catalog.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
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
    return {}; // 未知类型一律不发，避免把配置文件、二进制当静态资源漏出去
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

} // namespace

IpcServer::IpcServer(IpcContext context) : ctx_(std::move(context)) {
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

        build_status(message);
        server_.send_text(conn, message);
        build_params(message);
        server_.send_text(conn, message);
        send_backlog(conn);
    };

    server_.on_close = [](std::intptr_t) { FGB_DEBUG(kTag, "UI 已断开"); };

    server_.on_text = [this](std::intptr_t conn, std::string_view text) {
        handle_text(conn, text);
    };

    server_.on_http = [this](const net::ws::HttpHead& head, std::string& body,
                             std::string& content_type) {
        return handle_http(head, body, content_type);
    };
}

IpcServer::~IpcServer() { stop(); }

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

    const params::ParamRouter* router = ctx_.sink != nullptr ? &ctx_.sink->router() : nullptr;
    std::size_t index                 = 0;

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
            const bool bound = router != nullptr && !router->address(index).empty();
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
    writer.key("fps").num(last_fps_, 1);

    if (ctx_.sink != nullptr) {
        const SinkStats sink_stats = ctx_.sink->stats();
        writer.key("messages").integer(static_cast<long long>(sink_stats.messages));
        writer.key("bytes").integer(static_cast<long long>(sink_stats.bytes));
        writer.key("avatar_name").str(ctx_.sink->avatar_name());
        writer.key("avatar_id").str(ctx_.sink->avatar_id());
        writer.key("bound_to_avatar").boolean(ctx_.sink->router().bound_to_avatar());

        const params::ParamRouter& router = ctx_.sink->router();
        writer.key("param_count").integer(static_cast<long long>(router.param_count()));
        writer.key("target_count").integer(static_cast<long long>(router.targets().size()));
        writer.key("binary_count").integer(static_cast<long long>(router.binary_param_count()));
        writer.key("matched").integer(static_cast<long long>(router.matched()));
    } else {
        writer.key("messages").integer(0);
        writer.key("bytes").integer(0);
        writer.key("avatar_name").str("");
        writer.key("avatar_id").str("");
        writer.key("bound_to_avatar").boolean(false);
        writer.key("param_count").integer(0);
        writer.key("target_count").integer(0);
        writer.key("binary_count").integer(0);
        writer.key("matched").integer(0);
    }

    writer.key("params_mode").str(ctx_.params_mode);
    writer.key("eye_mode").str(ctx_.eye_mode);
    writer.key("clients").integer(static_cast<long long>(server_.ready_count()));

    writer.key("module");
    writer.obj_begin();
    writer.key("id").str(ctx_.active_module_id);
    writer.key("state").str(module_state());
    // 模块自己报的错优先：它比"等待初始化超时"这类描述具体
    std::string module_error;
    if (ctx_.vrcft != nullptr) {
        module_error = !ctx_.vrcft->module_error().empty() ? ctx_.vrcft->module_error()
                                                           : ctx_.vrcft->last_error();
    }
    writer.key("error").str(module_error);
    writer.key("restarts").integer(
        static_cast<long long>(ctx_.vrcft != nullptr ? ctx_.vrcft->restart_count() : 0));
    writer.key("alive").boolean(ctx_.vrcft != nullptr && ctx_.vrcft->module_alive());
    writer.obj_end();

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

// 当前模块的运行态。界面上的红点必须配一句人话，
// 所以这里只给四种状态，其余细节靠 status.module.error。
const char* IpcServer::module_state() const noexcept {
    if (ctx_.active_module_id.empty()) {
        return "none";
    }
    if (ctx_.vrcft == nullptr) {
        return "external"; // 模块不归我们管（例如 --module-dll 手工指定）
    }
    // 模块自己报了错就是故障，哪怕进程还活着（它在重试）
    if (!ctx_.vrcft->module_error().empty() && ctx_.vrcft->stats().frames == 0) {
        return "error";
    }
    if (!ctx_.vrcft->last_error().empty()) {
        return "error";
    }
    if (ctx_.vrcft->stats().frames > 0 && ctx_.vrcft->module_alive()) {
        return "active";
    }
    return ctx_.vrcft->module_alive() ? "starting" : "stopped";
}

void IpcServer::build_modules(std::string& out) {
    out.clear();
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("type").str("modules");

    const vrcft::ModuleRegistry* registry = ctx_.modules;
    writer.key("active").str(ctx_.active_module_id);
    writer.key("config").str(registry != nullptr ? registry->config_path().string() : std::string{});
    writer.key("state").str(module_state());

    writer.key("roots").arr_begin();
    if (registry != nullptr) {
        for (const vrcft::ModuleRoot& root : registry->roots()) {
            writer.obj_begin();
            writer.key("path").str(root.path.string());
            writer.key("origin").str(root.origin);
            writer.obj_end();
        }
    }
    writer.arr_end();

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

            const char* state = "installed";
            if (module.broken) {
                state = "broken";
            } else if (module.active) {
                state = module_state();
            }
            writer.key("state").str(state);
            writer.obj_end();
        }
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
    bool ok          = false;
    std::string error;

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
            ok                    = true;
        }
    } else if (has_type && type == "set_module") {
        std::string id;
        json::field_string(text, "id", id); // 空串 = 不加载任何模块
        if (ctx_.modules == nullptr) {
            error = "module registry not available";
        } else if (ctx_.modules->set_active(id)) {
            ctx_.active_module_id = ctx_.modules->active_id();
            ok                    = true;
        } else {
            error = "unknown module";
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
    writer.obj_end();
    server_.send_text(conn, reply);

    if (ok && (type == "set_filter" || type == "scan_modules" || type == "set_module")) {
        // 让操作立刻看到权威值，而不是等下一次 10 Hz 推送
        std::string status;
        build_status(status);
        server_.broadcast(status);
    }
    if (ok && (type == "scan_modules" || type == "set_module")) {
        std::string modules;
        build_modules(modules);
        server_.broadcast(modules);
    }
}

bool IpcServer::handle_http(const net::ws::HttpHead& head, std::string& body,
                            std::string& content_type) {
    content_type = "application/json; charset=utf-8";

    if (head.method != "GET") {
        return false;
    }

    if (head.path == "/api/status") {
        build_metrics(body);
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

    // 前缀校验：即使请求里混进了软链接，也不能读到 ui 目录之外
    const std::wstring root_text   = root.wstring();
    const std::wstring target_text = target.wstring();
    if (target_text.size() < root_text.size() ||
        target_text.compare(0, root_text.size(), root_text) != 0) {
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

void IpcServer::pump(int timeout_ms) {
    server_.poll(timeout_ms);
    flush_logs();

    if (server_.ready_count() == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

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
}

} // namespace fgb::web
