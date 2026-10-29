// 在线模块库：registry.vrcft.io 的只读客户端 + 本地缓存 + 后台下载安装。
//
// 与 VRCFT 用的是同一个公开接口（GET https://registry.vrcft.io/modules），
// 所以列表里的模块、版本、评分与 VRCFT 界面一致，不需要自己维护一份清单。
//
// 线程模型：网络/下载在后台线程，注册表与磁盘安装只在引擎线程做（poll()），
// 避免两个线程同时改模块仓。
#pragma once

#include "vrcft/module_registry.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fgb::vrcft {

struct LibraryEntry {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string usage;
    std::string page;
    std::string download_url;
    std::string dll_name;
    std::string file_hash; // MD5（可能为空）
    std::string last_updated;
    int downloads = 0;
    int ratings   = 0;
    double rating = 0.0;
};

struct LibraryStatus {
    // idle / refreshing / downloading / installing / error
    std::string phase = "idle";
    std::string error;
    std::string install_id;
    std::uint64_t received = 0;
    std::uint64_t total    = 0;
    bool stale             = false; // 显示的是缓存（刷新失败/未刷新）
    std::string fetched_at;
};

struct LibrarySnapshot {
    std::vector<LibraryEntry> entries;
    LibraryStatus status;
};

struct LibraryEvents {
    bool changed         = false;
    bool modules_changed = false;
};

// registry 响应是"数组套一层对象"，键值都是标量。这个解析器只覆盖这种形状，
// 不做完整 JSON 校验——但字符串里的花括号/转义必须正确处理，否则描述一多就错位。
bool parse_registry_json(std::string_view text, std::vector<LibraryEntry>& out,
                         std::string& error);

std::filesystem::path default_library_cache_path();

class ModuleLibrary {
public:
    explicit ModuleLibrary(ModuleRegistry* registry,
                           std::filesystem::path cache_path = default_library_cache_path());
    ~ModuleLibrary();

    ModuleLibrary(const ModuleLibrary&)            = delete;
    ModuleLibrary& operator=(const ModuleLibrary&) = delete;

    // 启动时把上次的列表读出来（标记 stale，界面先有东西看）
    void load_cache();

    // 忙（正在刷新/下载）时返回 false，调用方据此给界面明确回执
    bool refresh();
    bool install(std::string entry_id);

    // 引擎线程每轮调用：处理下载完成后的安装，返回"要不要重新广播"
    LibraryEvents poll();

    LibrarySnapshot snapshot() const;
    bool busy() const noexcept { return !done_.load(std::memory_order_acquire); }
    const std::filesystem::path& cache_path() const noexcept { return cache_path_; }
    void stop();

private:
    void join_if_done();
    void start_worker(std::function<void()> work);
    void do_refresh();
    void do_download(LibraryEntry entry);
    void save_cache(const std::string& body) const;

    ModuleRegistry* registry_ = nullptr;
    std::filesystem::path cache_path_;
    std::filesystem::path download_dir_;

    mutable std::mutex mutex_;
    std::vector<LibraryEntry> entries_;
    LibraryStatus status_;
    bool changed_ = true;

    bool pending_install_ready_ = false;
    std::string pending_install_id_;
    std::filesystem::path pending_install_path_;

    std::thread worker_;
    std::atomic<bool> done_{true};
    std::atomic<bool> stop_{false};
};

} // namespace fgb::vrcft
