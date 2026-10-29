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

#include "base/process_stats.hpp"
#include "config/outputs.hpp"
#include "core/process/one_euro.hpp"
#include "engine/engine.hpp"
#include "net/ws_server.hpp"
#include "openvr/steamvr.hpp"
#include "params/calibration.hpp"
#include "params/preview_tap.hpp"
#include "sinks/shm_sink.hpp"
#include "sinks/udp_binary_sink.hpp"
#include "sinks/vmc_sink.hpp"
#include "sinks/vrchat_sink.hpp"
#include "sinks/ws_json_sink.hpp"
#include "sources/vrcft_source.hpp"
#include "vrcft/module_library.hpp"
#include "vrcft/module_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fgb::web {

// 一个正在跑的模块实例。多实例（两个 vrcft 各加载一个模块，分别供眼追/表情）时，
// 界面上必须能按实例看状态——只显示"第一个"会让另一路的问题被藏起来。
struct RunningModule {
    std::string display_name; // "vrcft[1]"，与 --eye-source 里用的名字一致
    std::string module_id;    // 注册表 id（GUID）
    std::string module_name;  // 展示名（VRCFTPicoModule）
    const ISource* source = nullptr;
};

struct IpcContext {
    Engine* engine     = nullptr;
    VrchatSink* sink   = nullptr; // 可能为空（没配 OSC 输出）
    VmcSink* vmc_out   = nullptr; // 可能为空（没开 --vmc-out）
    ShmSink* shm_out   = nullptr; // 可能为空（没开 --shm）
    WsJsonSink* ws_out = nullptr; // 可能为空（没开 --ws-out）
    UdpBinarySink* udp_out = nullptr; // 可能为空（没开 --udp-out）
    FaceFilter* filter = nullptr; // 可能为空（未启用滤波）

    // 校准：tap 固定挂在校准之前；applier 即使没有档案也会挂一个空的（全透传），
    // 否则用户打开校准页时没有可写的 profile，只能重启引擎（ADR-0021 §热更新）。
    params::PreviewTap* preview         = nullptr;
    params::CalibrationApplier* calib   = nullptr;
    std::filesystem::path calib_path;   // 保存目标：--calibration 显式档案优先，否则默认档案
    bool calib_loaded = false;          // 启动时是否真的读到了档案（界面文案用）

    std::string source_name;
    std::string params_mode;
    std::string eye_mode;

    // 输出页保存的目标：启动时加载/应用，set_outputs 写回文件；下次启动生效
    config::OutputOptions outputs;
    std::filesystem::path outputs_path;
    // 命名输出预设的目录；空表示界面上的预设功能不可用
    std::filesystem::path outputs_profiles_dir;

    std::filesystem::path ui_dir;    // 静态页面根目录，空表示不发页面
    std::filesystem::path exe_dir;   // 产品 exe 所在目录；app.vrmanifest 在它旁边

    // 模块管理：非 vrcft 输入时为空。列表是"磁盘上有什么"，运行态在每个实例那边。
    vrcft::ModuleRegistry* modules = nullptr;
    // restart_engine 命令：置位后引擎以退出码 3 结束，桌面入口据此重新拉起
    std::atomic<bool>* restart_engine = nullptr;
    std::vector<RunningModule> running_modules;
    std::string active_module_id; // 注册表里"下次启动加载"的那个，与运行态不是一回事

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

    // 删除正在运行的模块：引擎停下、DLL 解锁之后再执行，返回要删除的 id
    std::string take_pending_delete();

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
    void build_gaze(std::string& out);
    void build_modules(std::string& out);
    void build_library(std::string& out);
    void build_metrics(std::string& out); // status 的公共部分，HTTP 接口复用
    void build_calibration(std::string& out);
    void build_preview(std::string& out);
    void build_steamvr(std::string& out) const;
    void build_output_profiles(std::string& out) const;
    void broadcast_output_profiles();
    void start_diagnostics_export(const std::filesystem::path& output = {});
    void start_update_check(std::string url);
    // 校准档案变了就广播：多个页面/探针连上来时各自显示一致的状态
    void broadcast_calibration();
    // per-instance 运行态；state/error 都来自对应的 VrcftSource
    const char* module_state(const RunningModule& module) const noexcept;

    void send_backlog(std::intptr_t conn);
    void handle_text(std::intptr_t conn, std::string_view text);
    bool handle_http(const net::ws::HttpHead& head, std::string& body,
                     std::string& content_type);
    bool serve_static(std::string_view path, std::string& body, std::string& content_type) const;
    void flush_logs();
    void refresh_catalog();
    void poll_picker();
    void start_picker();
    void after_module_change(bool ok, const std::string& message);

    IpcContext ctx_;
    net::WsServer server_;
    std::string catalog_json_;
    bool catalog_dirty_ = true;

    ProcessSampler process_sampler_;
    ProcessStats last_process_;
    std::chrono::steady_clock::time_point last_process_sample_{};
    std::chrono::steady_clock::time_point last_status_{};
    std::chrono::steady_clock::time_point last_params_{};
    std::chrono::steady_clock::time_point last_gaze_{};
    std::chrono::steady_clock::time_point last_preview_{};
    std::uint64_t last_frames_  = 0; // fps 用两次推送之间的增量算
    double last_fps_            = 0.0;
    // 只有校准页打开时才推 preview；页面走了要立刻停，别让引擎白拷 88 个值
    bool preview_wanted_ = false;
    // 上次保存/加载的档案：和内存里的比一下就知道有没有未保存改动
    params::CalibrationProfile saved_profile_{};
    bool calib_loaded_ = false;

    struct PickerState {
        std::atomic<bool> running{true};
        std::atomic<bool> ready{false};
        std::mutex mutex;
        std::string path;
    };

    std::deque<LogLine> logs_;        // 待推送（UI 没连上时暂存，有上限）
    std::deque<LogLine> recent_logs_; // 最近若干行，新连接建立时补发
    std::mutex log_mutex_;
    // 诊断包在后台打包，结果交给 pump() 在引擎线程广播（WsServer 不是线程安全的）
    std::thread diag_thread_;
    std::atomic<bool> diag_running_{false};
    std::mutex diag_mutex_;
    std::string diag_result_;
    bool diag_result_pending_ = false;

    std::thread update_thread_;
    std::atomic<bool> update_running_{false};
    std::mutex update_mutex_;
    std::string update_result_;
    bool update_result_pending_ = false;

    std::unique_ptr<vrcft::ModuleLibrary> library_;
    // OpenVR 初始化会阻塞，放在后台线程；命令结果由 pump() 广播
    std::unique_ptr<steamvr::Service> steamvr_;
    std::string pending_delete_id_;
    std::shared_ptr<PickerState> picker_;
    std::chrono::steady_clock::time_point last_library_push_{};
};

} // namespace fgb::web
