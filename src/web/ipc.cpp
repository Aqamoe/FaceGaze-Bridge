#include "web/ipc.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "params/avatar_config.hpp"
#include "params/catalog.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#endif

#include <algorithm>
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
            writer.key("head").boolean(report.used_head);
            writer.key("has_expression").boolean(report.provides_expression);
            writer.key("has_eyes").boolean(report.provides_eyes);
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
    // 选了启动模块但没在跑（或者改了启停）：界面提示用户点"重启引擎"
    writer.key("restart_needed")
        .boolean(!ctx_.active_module_id.empty() && !selected_running);

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
    if (ok && type == "set_filter") {
        std::string status;
        build_status(status);
        server_.broadcast(status);
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
        const bool ok = ctx_.modules->install_path(path, error, "zip");
        after_module_change(ok, ok ? "模块已安装" : error);
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
        return;
    }

    const auto now = std::chrono::steady_clock::now();

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
}

} // namespace fgb::web
