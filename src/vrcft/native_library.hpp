// 原生模块在线库：fgb-registry（registry schema v2）的只读客户端 + 缓存 + 校验安装。
//
// 与 VRCFT 的老注册表（vrcft/module_library.hpp）是两套接口，不要试图合并：
//   - 老格式是"扁平数组 + 全标量"，能力列表得拆字符串，没有版本历史/通道/校验值
//   - 新格式是 schemaVersion 2，版本条目带 sha256 与 size，宿主必须校验后才解压
// 两边的刷新、下载、错误文案互相独立；界面上也只是同一个页面的两个标签。
//
// 线程模型与老库一致：网络/下载在后台线程，写入模块仓只在引擎线程（poll()），
// 避免两个线程同时改模块仓。
#pragma once

#include "vrcft/module_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fgb::vrcft {

// modules[].latest 与 modules[].versions[] 是同构的版本条目。
struct NativeVersion {
    std::string version;
    std::string channel = "stable";
    std::string published_at;
    std::string file_name;
    std::string download_url;
    std::string sha256;
    std::uint64_t size = 0;
    int abi_version    = 0; // 包内 fgb-native.json 的 AbiVersion，加载前判断
    int downloads      = 0;
    std::string notes;
    std::string release_url;
};

// i18n 只覆盖 summary / description / usage（schema 定义的三个字段）。
struct NativeLocalizedText {
    std::string summary;
    std::string description;
    std::string usage;
};

struct NativeEntry {
    std::string id;
    std::string name;
    std::string author;
    std::string author_url;
    std::string summary;
    std::string description;
    std::string usage;
    std::string homepage;
    std::string repo;
    std::string license;
    std::string icon;
    std::vector<std::string> screenshots;
    std::vector<std::string> tags;
    std::vector<std::string> capabilities; // expression / eyes / gaze / head
    std::string default_role;
    std::string channel = "stable";
    int requires_abi    = 0;
    std::string min_host_version;
    std::vector<std::string> requires_os;
    std::vector<std::string> requires_arch;
    bool featured       = false;
    bool verified       = false;
    bool archived       = false;
    int stats_downloads = 0;
    int stats_stars     = 0;
    std::string published_at;
    std::string updated_at;
    NativeVersion latest;
    std::vector<NativeVersion> versions;
    std::vector<std::pair<std::string, NativeLocalizedText>> i18n;

    // 兼容性由宿主算，不写进缓存正文：宿主升级后同一份缓存要能重新判定。
    bool compatible = true;
    std::string incompatible_code;   // abi / host_version / os / arch
    std::string incompatible_reason; // 人话原因（日志与排障用，界面按 code 本地化）
};

struct NativeRegistry {
    std::string title;
    std::string description;
    std::string homepage;
    std::string generated_at;
    std::string generator;
    std::vector<NativeEntry> modules;
};

struct HostEnvironment {
    int abi = 0;
    std::string host_version;
    std::string os;
    std::string arch;
};

// 本机环境：ABI 2、当前程序版本（MinHostVersion 用它比）、windows / x64
HostEnvironment host_environment();

// schemaVersion 必须是 2（不认识的版本整体拒绝，别猜着解析）；
// 缺 id / name / latest 或下载地址不是 https 的条目会被丢弃，而不是让整份清单失败。
bool parse_native_registry(std::string_view text, NativeRegistry& out, std::string& error);

void evaluate_compatibility(NativeEntry& entry, const HostEnvironment& env);

// version 为空时取 latest；找不到返回 nullptr。
const NativeVersion* select_version(const NativeEntry& entry, std::string_view version);

// 已解压的包里核对 fgb-native.json：ModuleId 必须等于条目 id，
// AbiVersion 必须等于条目声明（包和清单不一致就是"挂羊头卖狗肉"）。
bool verify_native_package(const std::filesystem::path& root, const NativeEntry& entry,
                           ModuleInfo& info, std::string& error);

// 单个 DLL 资产没有包内清单，但模块仓必须靠 fgb-native.json 认出"这是原生模块"，
// 否则会按 VRCFT 模块加载。用索引元数据补一份（能力位与角色提示一起写）。
bool write_native_package_manifest(const std::filesystem::path& dir, const NativeEntry& entry,
                                   const NativeVersion& version, const std::string& dll_name,
                                   std::string& error);

std::filesystem::path default_native_library_cache_path();

struct NativeLibraryStatus {
    // phase 只描述"下载/安装"；刷新是独立任务，两者可以同时进行
    std::string phase = "idle"; // idle / downloading / installing / error
    std::string error;
    std::string install_id;
    std::string install_version;
    std::uint64_t received = 0;
    std::uint64_t total    = 0;
    bool stale             = false;
    std::string fetched_at;
    bool refreshing        = false;
    std::string refresh_error;
};

struct NativeLibrarySnapshot {
    NativeRegistry registry;
    NativeLibraryStatus status;
};

struct NativeLibraryEvents {
    bool changed         = false;
    bool modules_changed = false;
};

class NativeLibrary {
public:
    explicit NativeLibrary(ModuleRegistry* registry,
                           std::filesystem::path cache_path = default_native_library_cache_path());
    ~NativeLibrary();

    NativeLibrary(const NativeLibrary&)            = delete;
    NativeLibrary& operator=(const NativeLibrary&) = delete;

    void load_cache();
    void refresh_if_stale(std::chrono::seconds max_age);

    // 已有同类任务时返回 false，调用方给界面明确回执。
    bool refresh();
    // version 为空 = 条目 latest。安装失败原因写在 status().error。
    bool install(std::string entry_id, std::string version = {});

    // 引擎线程每轮调用：处理校验完成后的落盘，返回"要不要重新广播"
    NativeLibraryEvents poll();

    NativeLibrarySnapshot snapshot() const;
    bool busy() const noexcept { return !install_done_.load(std::memory_order_acquire); }
    const std::filesystem::path& cache_path() const noexcept { return cache_path_; }
    void stop();

private:
    void join_finished();
    void start_install_worker(std::function<void()> work);
    void start_refresh_worker(std::function<void()> work);
    void do_refresh();
    void do_download(NativeEntry entry, NativeVersion version);
    void save_cache(const std::string& body) const;
    void set_error(std::string message);

    ModuleRegistry* registry_ = nullptr;
    std::filesystem::path cache_path_;
    std::filesystem::path download_dir_;

    mutable std::mutex mutex_;
    NativeRegistry registry_data_;
    NativeLibraryStatus status_;
    bool changed_ = true;

    bool pending_install_ready_ = false;
    std::string pending_install_id_;
    std::filesystem::path pending_install_path_;
    std::filesystem::path pending_install_archive_;
    NativeEntry pending_install_metadata_;

    std::int64_t cache_written_unix_ = 0;

    std::thread install_worker_;
    std::atomic<bool> install_done_{true};
    std::thread refresh_worker_;
    std::atomic<bool> refresh_done_{true};
    std::atomic<bool> stop_{false};
};

} // namespace fgb::vrcft
