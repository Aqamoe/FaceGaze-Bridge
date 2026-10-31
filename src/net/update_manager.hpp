// 自动更新：检查 → 静默下载 → 校验 → 由用户确认后安装（[ADR-0036]）。
//
// 为什么单独一层而不是塞进 IpcServer：状态机要能被界面之外的探针断言，
// 也要能在没有界面的托盘形态下自己跑完下载。IPC 只负责"广播快照 + 转发命令"。
//
// 边界：
//   - 只对 Inno 装出来的副本自动安装（installed_copy），其余只提示；
//   - 下载必须过校验（资产 digest → SHA256SUMS.txt），拿不到哈希就不下载；
//   - 安装器由这里启动，必须 CREATE_BREAKAWAY_FROM_JOB（见 ADR-0036 §4）。
#pragma once

#include "net/update_check.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace fgb::update {

enum class Stage {
    idle,        // 没有在跑，也没有已下载的包
    checking,    // 正在请求 GitHub
    available,   // 有新版本，但不能/还没下载（非安装版、缺校验和、用户忽略过）
    downloading, // 正在下载
    ready,       // 下载并校验完成，等用户点"安装"
    installing,  // 已启动安装器，进程即将退出
    failed,      // 上一次检查/下载失败（error 里是原因）
};

const char* stage_name(Stage stage) noexcept;

struct Staged {
    bool valid = false;
    std::string version;
    std::string path; // UTF-8 绝对路径
    std::string sha256;
    std::uint64_t size = 0;
};

struct Snapshot {
    Stage stage = Stage::idle;
    std::string current;
    std::string tag;
    std::string latest;
    std::string name;
    std::string page_url;
    std::string error;
    bool available = false;
    bool eligible  = false; // 当前目录是安装器装的，可以自动安装
    bool skipped   = false; // 用户忽略过这一版
    std::uint64_t received = 0;
    std::uint64_t total    = 0;
    Staged staged;
};

// 落盘的节奏控制与暂存记录。
// "下一次该查的时间"直接存绝对时间：比存"上次查的时间 + 推算"少一层出错空间
// （系统时钟被改、夏令时都不影响）。
// 暂存记录也放这里：用户下载完直接关程序，下次启动不必重下 100 MB。
struct Preferences {
    std::int64_t next_check_at = 0;
    std::string skipped_version;
    bool auto_check = true;
    std::string staged_version;
    std::string staged_path; // UTF-8
    std::string staged_sha256;
    std::int64_t staged_size = 0;
};

bool load_preferences(const std::filesystem::path& path, Preferences& out);
bool save_preferences(const std::filesystem::path& path, const Preferences& prefs);

// 成功 24 小时后再查（加 0..4 小时抖动），失败 6 小时后退避。
std::int64_t next_check_time(std::int64_t now, bool succeeded, std::int64_t jitter_seconds);

// 当前 exe 目录是不是安装器登记的那个（HKCU 卸载项 InstallLocation）。
// 不是的话自动安装会把绿色版搬进 %LOCALAPPDATA%\Programs，这必须避免。
bool installed_copy(const std::filesystem::path& exe_dir,
                    std::filesystem::path* install_dir = nullptr);

// %LOCALAPPDATA%\FaceGazeBridge；取不到环境变量时返回空路径（此时不启用更新）
std::filesystem::path default_data_dir();

class Manager {
public:
    // assume_installed: 只给探针用（--update-assume-installed）。开发目录不是安装器装的，
    // 不放开这个开关就测不到"下载 + 校验"这条链路；普通运行永远按注册表判断。
    Manager(std::filesystem::path data_dir, std::filesystem::path exe_dir,
            std::string current_version, bool assume_installed = false);
    ~Manager();

    Manager(const Manager&)            = delete;
    Manager& operator=(const Manager&) = delete;

    // manual=true 绕过节流，也不受 skipped_version 抑制。
    // 非 manual 时若还没到 next_check_at 就直接返回（不改变状态）。
    void start_check(std::string url, bool manual);

    // 校验 staged 文件并启动安装器；成功返回 true，之后进程应立即退出（退出码 4）。
    bool start_install(std::string& error);

    // 忽略当前最新版：同版本不再自动提示
    void skip_current_version();

    bool busy() const noexcept;
    bool take(Snapshot& out);
    Snapshot snapshot() const;

private:
    void worker(std::string url, bool manual);
    bool fetch_checksum(const ReleaseInfo& info, const ReleaseAsset& asset, std::string& hex,
                        std::string& error);
    bool download_verified(const ReleaseAsset& asset, const std::filesystem::path& destination,
                           const std::string& expected, std::string& error);
    void publish(Snapshot next);
    void publish_error(Stage stage, std::string error);

    std::filesystem::path data_dir_;
    std::filesystem::path exe_dir_;
    std::filesystem::path prefs_path_;
    std::string current_version_;
    bool eligible_ = false;
    bool assume_installed_ = false; // 探针开关，见构造函数注释

    mutable std::mutex mutex_;
    Snapshot snap_;
    bool changed_ = false;

    Preferences prefs_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    // 析构时置位：下载中途退出不该让引擎等完整包下完（100 MB 可能要几分钟）
    std::atomic<bool> abort_{false};
};

} // namespace fgb::update
