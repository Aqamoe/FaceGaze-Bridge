// UI ↔ 引擎的控制面 IPC。
//
// 形态：`ws://127.0.0.1:<port>/ws`，JSON 文本帧。
//   - 引擎 → UI：hello / catalog（参数名表，只在连接建立时发一次）/ status / params / log
//   - UI → 引擎：set_filter / reload_avatar / set_log_level / quit，逐条回 ack
//
// 为什么是 WebSocket 而不是让页面轮询 HTTP：
//   1. 推送，不轮询。参数热力图要 10 Hz 刷新，轮询会把请求-响应延迟抖到画面上
//   2. 一条连接既下行又上行，滑杆改动随手就能回传，不用为每个动作设计一个 URL
//   3. 参数名表只发一次，后续每帧只发数值数组（365 个数字 ≈ 2.4 KB），轮询做不到这个
//
// 同端口仍然保留 `/api/status`、`/api/params` 两个只读 HTTP 接口：curl 能验，
// 自动化测试不依赖浏览器；页面的静态资源也由这里发（WebView2 直接用 http 源加载，
// 省掉 https 页面连 ws:// 回环时的 mixed-content 麻烦）。
#pragma once

#include "core/process/one_euro.hpp"
#include "engine/engine.hpp"
#include "net/ws_server.hpp"
#include "sinks/vrchat_sink.hpp"
#include "sources/vrcft_source.hpp"
#include "vrcft/module_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>

namespace fgb::web {

struct IpcContext {
    Engine* engine     = nullptr;
    VrchatSink* sink   = nullptr; // 可能为空（没配 OSC 输出）
    FaceFilter* filter = nullptr; // 可能为空（未启用滤波）

    std::string source_name;
    std::string params_mode;
    std::string eye_mode;
    std::filesystem::path ui_dir; // 静态页面根目录，空表示不发页面

    // 模块管理：非 vrcft 输入时为空。列表是"磁盘上有什么"，运行态在 vrcft 那边。
    vrcft::ModuleRegistry* modules    = nullptr;
    const vrcft::VrcftSource* vrcft   = nullptr;
    std::string active_module_id;

    std::atomic<bool>* quit = nullptr;
};

class IpcServer {
public:
    explicit IpcServer(IpcContext context);
    ~IpcServer();

    IpcServer(const IpcServer&)            = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    bool start(std::uint16_t port, std::string& error);
    void stop();

    std::uint16_t port() const noexcept { return server_.port(); }
    std::size_t client_count() const noexcept { return server_.ready_count(); }

    // 引擎主循环里每帧调用。内部自带节流：状态与参数各 10 Hz 推送。
    void pump(int timeout_ms);

private:
    struct LogLine {
        int level = 2;
        std::string tag;
        std::string message;
    };

    void build_hello(std::string& out) const;
    void build_catalog(std::string& out);
    void build_status(std::string& out);
    void build_params(std::string& out);
    void build_modules(std::string& out);
    void build_metrics(std::string& out); // status 的公共部分，HTTP 接口复用
    const char* module_state() const noexcept;

    void send_backlog(std::intptr_t conn);
    void handle_text(std::intptr_t conn, std::string_view text);
    bool handle_http(const net::ws::HttpHead& head, std::string& body,
                     std::string& content_type);
    bool serve_static(std::string_view path, std::string& body, std::string& content_type) const;
    void flush_logs();
    void refresh_catalog();

    IpcContext ctx_;
    net::WsServer server_;
    std::string catalog_json_;
    bool catalog_dirty_ = true;

    std::chrono::steady_clock::time_point last_status_{};
    std::chrono::steady_clock::time_point last_params_{};
    std::uint64_t last_frames_  = 0; // fps 用两次推送之间的增量算
    double last_fps_            = 0.0;

    std::deque<LogLine> logs_;        // 待推送（UI 没连上时暂存，有上限）
    std::deque<LogLine> recent_logs_; // 最近若干行，新连接建立时补发
    std::mutex log_mutex_;
};

} // namespace fgb::web
