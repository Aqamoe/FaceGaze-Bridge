// SteamVR 启动项（OpenVR IVRApplications）的薄封装。
//
// 【为什么不用配置文件/注册表】
// SteamVR 的 steamvr.vrsettings 与 steamapps.vrmanifest 没有公开格式，版本更新会覆盖，
// 写坏了还会被当成"这软件改坏了 SteamVR"。官方路径只有 IVRApplications：
//   AddApplicationManifest + SetApplicationAutoLaunch（ADR-0020）。
//
// 【为什么要 Background 类型】
// VRApplication_Background 的语义是"跟随 SteamVR"：SteamVR 没起来时不反向拉起，
// 退出后也不负责保活。代价是必须 SteamVR 正在运行时才能注册/查询，界面要说清楚。
//
// 【为什么 OpenVR 调用不放在外壳】
// 外壳只负责窗口与引擎生命周期（ADR-0016/0022）。命令行、探针、托盘都共享
// 引擎的控制面，注册逻辑放在引擎侧，只有一份。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fgb::steamvr {

inline constexpr const char* kAppKey = "com.facegazebridge.desktop";

enum class State {
    Unknown,      // 还没检测过
    NotInstalled, // 找不到 SteamVR 运行库
    NotRunning,   // 运行库在，但 OpenVR Background 初始化失败（通常是 SteamVR 没开）
    Off,          // 可注册，自启动未开
    On,           // 自启动已开
    Error,        // 注册/读回失败，detail 里是原因
};

struct Status {
    State state = State::Unknown;
    bool runtime_found     = false;
    bool steamvr_running   = false;
    bool registered        = false; // IsApplicationInstalled
    bool autolaunch        = false; // GetApplicationAutoLaunch
    bool ok                = false;
    int error              = 0;
    std::string reason;     // 稳定标识，界面据此翻译（见 i18n settings.steamvr.reason.*）
    std::string detail;     // 原始错误/说明，排障用
    std::string runtime_dir;
    std::string manifest_path;
    std::string binary_path;
    std::string app_key = kAppKey;
    std::string action; // refresh / enable / disable / remove
};

const char* state_name(State state) noexcept;

// 从 openvrpaths.vrpath 里取 "runtime" 数组的第一项。纯函数，单测覆盖。
bool parse_openvrpaths_runtime(std::string_view text, std::string& runtime_dir);

// 从 Steam 的 config/appconfig.json 里取 "manifest_paths"（已登记的 manifest 列表）。
// 重复安装到新目录后，旧 manifest 仍占着 app_key，要靠这份列表把它找出来摘掉。
// 纯函数，单测覆盖。
std::vector<std::string> parse_manifest_paths(std::string_view text);

// 随包 manifest 的内容。binary_path_windows 相对 manifest 所在目录解析。
std::string build_manifest_json(std::string_view exe_name = "FaceGaze Bridge.exe",
                                std::string_view arguments = "--tray",
                                std::string_view app_key  = kAppKey);

std::filesystem::path manifest_path_for(const std::filesystem::path& exe_dir);
std::filesystem::path dll_path_for(const std::filesystem::path& runtime_dir);

// 依次尝试 openvrpaths.vrpath、Steam 注册表、常见安装目录；返回第一个含
// bin/win64/openvr_api.dll 的目录，找不到返回空。
std::filesystem::path find_runtime_dir();

// VR_Init 的错误码 → 状态。只做纯映射，单测不依赖 SteamVR。
State classify_init_error(int error) noexcept;

// VR_Init / IVRApplications 的函数指针表。定义在 .cpp，避免 openvr.h 污染公开头文件。
struct ApiImpl;

class Manager {
public:
    explicit Manager(std::filesystem::path exe_dir);
    ~Manager();

    Manager(const Manager&)            = delete;
    Manager& operator=(const Manager&) = delete;

    // 尝试 Background 初始化并读回注册状态。失败也返回可用状态，不抛异常。
    Status status(std::string_view action = "refresh");
    Status set_autolaunch(bool enabled);
    Status remove_registration();

private:
    Status probe(std::string_view action);
    bool load_openvr(std::string& error);

    std::filesystem::path exe_dir_;
    void* module_   = nullptr; // HMODULE，进程生命周期内只加载一次
    ApiImpl* api_   = nullptr;
};

// 后台服务：OpenVR 初始化可能阻塞数百毫秒到数秒，不能放在引擎 120 Hz 循环里。
// 命令入队，结果由主循环 take() 后广播；同一时刻只做一件事。
class Service {
public:
    enum class Op { Refresh, Enable, Disable, Remove };

    explicit Service(std::filesystem::path exe_dir);
    ~Service();

    Service(const Service&)            = delete;
    Service& operator=(const Service&) = delete;

    void request(Op op);
    bool take(Status& out);     // 取一次已完成的结果（取走后清空）
    Status snapshot() const;    // 最近一次完成的状态，HTTP 只读接口用
    bool busy() const;

private:
    void run();

    Manager manager_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Op> queue_;
    Status snapshot_{};
    Status result_{};
    bool has_result_ = false;
    bool busy_       = false;
    bool stop_       = false;
    std::thread worker_;
};

} // namespace fgb::steamvr
