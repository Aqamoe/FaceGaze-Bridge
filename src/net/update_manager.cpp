#include "net/update_manager.hpp"

#include "base/hash.hpp"
#include "base/log.hpp"
#include "config/ini.hpp"
#include "net/http_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fgb::update {
namespace {

constexpr const char* kTag = "update";

// Inno 的卸载项名固定是 {AppId}_is1；AppId 来自 packaging/facegazebridge.iss，
// 改那里的 GUID 必须同步改这里，否则"是否安装版"永远判成否。
#ifdef _WIN32
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{7B2F3C64-9E0A-4C1B-A0D8-4F6D2B7E5A91}_is1";
#endif

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::wstring utf8_to_wide(std::string_view text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          length);
    return wide;
#else
    return std::wstring(text.begin(), text.end());
#endif
}

std::string wide_to_utf8(std::wstring_view text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (length <= 0) {
        return {};
    }
    std::string narrow(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                          length, nullptr, nullptr);
    return narrow;
#else
    return std::string(text.begin(), text.end());
#endif
}

#ifdef _WIN32
std::wstring normalize_dir(const std::filesystem::path& dir) {
    std::wstring text = dir.lexically_normal().wstring();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) {
        text.pop_back();
    }
    return text;
}

// 安装器把 RELAUNCH 当"装完把程序拉起来"的开关；不给这个参数时（手动静默安装）
// 保持原来的行为，免得 CI 里装完弹一个窗口。
bool launch_installer(const std::filesystem::path& setup, std::string& error) {
    std::wstring command = L"\"" + setup.wstring() +
                           L"\" /SILENT /CLOSEAPPLICATIONS /NORESTART /SUPPRESSMSGBOXES"
                           L" /RELAUNCH=1";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    // 引擎与外壳同属一个 KILL_ON_JOB_CLOSE 的 Job（ADR-0022）：不脱开的话，
    // 外壳一退出安装器就被连带杀掉，表现为"点了安装没反应"（ADR-0036 §4）。
    constexpr DWORD kBreakaway = CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP;
    if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, kBreakaway, nullptr,
                         nullptr, &startup, &process) == 0) {
        const DWORD code = ::GetLastError();
        error = "无法启动安装器（错误码 " + std::to_string(code) + "）";
        FGB_WARN(kTag, "启动安装器失败：%lu", static_cast<unsigned long>(code));
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    FGB_INFO(kTag, "已启动安装器（脱离外壳 Job）");
    return true;
}
#endif

std::string_view trim_tag(std::string_view tag) noexcept {
    while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) {
        tag.remove_prefix(1);
    }
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) {
        tag.remove_prefix(1);
    }
    return tag;
}

} // namespace

const char* stage_name(Stage stage) noexcept {
    switch (stage) {
    case Stage::idle: return "idle";
    case Stage::checking: return "checking";
    case Stage::available: return "available";
    case Stage::downloading: return "downloading";
    case Stage::ready: return "ready";
    case Stage::installing: return "installing";
    case Stage::failed: return "failed";
    }
    return "idle";
}

bool load_preferences(const std::filesystem::path& path, Preferences& out) {
    out = Preferences{};
    bool opened = false;
    const bool loaded = config::ini_load(path, [&](const config::IniEntry& entry) {
        opened = true;
        if (entry.key == "next_check_at") {
            out.next_check_at = std::strtoll(entry.value.c_str(), nullptr, 10);
        } else if (entry.key == "skipped_version") {
            out.skipped_version = entry.value;
        } else if (entry.key == "auto_check") {
            out.auto_check = config::parse_bool(entry.value, true);
        } else if (entry.key == "staged_version") {
            out.staged_version = entry.value;
        } else if (entry.key == "staged_path") {
            out.staged_path = entry.value;
        } else if (entry.key == "staged_sha256") {
            out.staged_sha256 = entry.value;
        } else if (entry.key == "staged_size") {
            out.staged_size = std::strtoll(entry.value.c_str(), nullptr, 10);
        }
    });
    return loaded && opened;
}

bool save_preferences(const std::filesystem::path& path, const Preferences& prefs) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    config::IniWriter writer;
    if (!writer.open(path)) {
        return false;
    }
    writer.comment("FaceGaze Bridge 更新状态（界面/引擎自动维护，手改一般没有意义）");
    writer.section("update");
    writer.kv("next_check_at", std::to_string(prefs.next_check_at));
    writer.kv("skipped_version", prefs.skipped_version);
    writer.kv_bool("auto_check", prefs.auto_check);
    writer.kv("staged_version", prefs.staged_version);
    writer.kv("staged_path", prefs.staged_path);
    writer.kv("staged_sha256", prefs.staged_sha256);
    writer.kv("staged_size", std::to_string(prefs.staged_size));
    writer.close();
    return true;
}

std::int64_t next_check_time(std::int64_t now, bool succeeded, std::int64_t jitter_seconds) {
    if (!succeeded) {
        // 失败退避 6 小时：网络问题通常不是一分钟能好的，但也不能等到明天
        return now + 6 * 3600;
    }
    // 成功 24 小时 + 抖动：发版当天不要把所有用户在同一分钟叫醒
    constexpr std::int64_t kDay = 24 * 3600;
    const std::int64_t jitter  = std::clamp<std::int64_t>(jitter_seconds, 0, 4 * 3600);
    return now + kDay + jitter;
}

bool installed_copy(const std::filesystem::path& exe_dir, std::filesystem::path* install_dir) {
    if (install_dir != nullptr) {
        *install_dir = std::filesystem::path{};
    }
#ifdef _WIN32
    if (exe_dir.empty()) {
        return false;
    }
    wchar_t buffer[1024]{};
    DWORD bytes = static_cast<DWORD>(sizeof(buffer));
    const LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation",
                                          RRF_RT_REG_SZ, nullptr, buffer, &bytes);
    if (status != ERROR_SUCCESS || buffer[0] == L'\0') {
        return false;
    }
    const std::filesystem::path registered{buffer};
    if (install_dir != nullptr) {
        *install_dir = registered;
    }
    const std::wstring left  = normalize_dir(exe_dir);
    const std::wstring right = normalize_dir(registered);
    if (left.empty() || right.empty()) {
        return false;
    }
    return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    (void)exe_dir;
    return false;
#endif
}

std::filesystem::path default_data_dir() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{std::string{local}} / "FaceGazeBridge";
}

Manager::Manager(std::filesystem::path data_dir, std::filesystem::path exe_dir,
                 std::string current_version, bool assume_installed)
    : data_dir_(std::move(data_dir)),
      exe_dir_(std::move(exe_dir)),
      prefs_path_(data_dir_ / "update.ini"),
      current_version_(std::move(current_version)) {
    eligible_ = assume_installed || installed_copy(exe_dir_);
    assume_installed_ = assume_installed;

    snap_.current  = current_version_;
    snap_.eligible = eligible_;
    if (!prefs_path_.empty()) {
        Preferences stored;
        if (load_preferences(prefs_path_, stored)) {
            prefs_ = stored;
            // 上次已经下完的包：只要文件还在、大小对得上，就直接恢复成 ready，
            // 不必重下（用户很可能是"下载完关了程序，第二天再装"）
            if (!prefs_.staged_path.empty() && prefs_.staged_size > 0) {
                const std::filesystem::path staged{utf8_to_wide(prefs_.staged_path)};
                std::error_code ec;
                const auto size = std::filesystem::file_size(staged, ec);
                if (!ec && static_cast<std::int64_t>(size) == prefs_.staged_size) {
                    snap_.staged.valid   = true;
                    snap_.staged.version = prefs_.staged_version;
                    snap_.staged.path    = prefs_.staged_path;
                    snap_.staged.sha256  = prefs_.staged_sha256;
                    snap_.staged.size    = static_cast<std::uint64_t>(prefs_.staged_size);
                    snap_.stage          = Stage::ready;
                    snap_.available      = true;
                    snap_.tag            = "v" + prefs_.staged_version;
                    snap_.latest         = snap_.tag;
                }
            }
        }
    }
}

Manager::~Manager() {
    abort_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool Manager::busy() const noexcept {
    return running_.load(std::memory_order_acquire);
}

Snapshot Manager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

bool Manager::take(Snapshot& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!changed_) {
        return false;
    }
    out     = snap_;
    changed_ = false;
    return true;
}

void Manager::publish(Snapshot next) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_    = std::move(next);
    changed_ = true;
}

void Manager::publish_error(Stage stage, std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_.stage = stage;
    snap_.error = std::move(error);
    changed_    = true;
}

void Manager::start_check(std::string url, bool manual) {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return; // 已经有一次检查/下载在跑，直接忽略
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (!manual) {
        std::int64_t next = 0;
        bool auto_check   = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            next       = prefs_.next_check_at;
            auto_check = prefs_.auto_check;
        }
        if (!auto_check || (next > 0 && now_seconds() < next)) {
            running_.store(false, std::memory_order_release);
            return;
        }
    }
    thread_ = std::thread([this, url = std::move(url), manual] { worker(std::move(url), manual); });
}

void Manager::skip_current_version() {
    std::string version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version = trim_tag(snap_.tag);
        if (version.empty()) {
            return;
        }
        prefs_.skipped_version = std::string{version};
        snap_.skipped          = true;
        if (snap_.stage == Stage::available || snap_.stage == Stage::ready) {
            // 已下载的包不再提示；文件留着，下次手动检查还能直接装
            snap_.stage = Stage::available;
        }
        changed_ = true;
        save_preferences(prefs_path_, prefs_);
    }
    FGB_INFO(kTag, "用户忽略了版本 %s", version.c_str());
}

bool Manager::fetch_checksum(const ReleaseInfo& info, const ReleaseAsset& asset, std::string& hex,
                             std::string& error) {
    hex = digest_hex(asset.digest);
    if (!hex.empty()) {
        return true;
    }
    const ReleaseAsset* sums = find_sums_asset(info);
    if (sums == nullptr) {
        error = "发布里没有校验和，已停止自动下载（请用下载页手动安装）";
        return false;
    }
    std::string text;
    if (!net::http_get(sums->url, text, error, 256u * 1024u)) {
        error = "下载校验和失败：" + error;
        return false;
    }
    if (!checksum_for(text, asset.name, hex)) {
        error = "校验和文件里没有这个安装包，已停止自动下载";
        return false;
    }
    return true;
}

bool Manager::download_verified(const ReleaseAsset& asset, const std::filesystem::path& destination,
                                const std::string& expected, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);

    // 已经下过且哈希对得上就复用：关掉程序再打开不该重下 100 MB
    if (std::filesystem::exists(destination, ec) && !ec) {
        const auto size = std::filesystem::file_size(destination, ec);
        if (!ec && (asset.size == 0 || static_cast<std::uint64_t>(size) == asset.size) &&
            hash::sha256_file(destination) == expected) {
            return true;
        }
        std::filesystem::remove(destination, ec);
    }

    constexpr std::size_t kSetupLimit = 512u * 1024u * 1024u;
    auto last_publish = std::chrono::steady_clock::now();
    const auto progress = [this, &asset, &last_publish](const net::HttpProgress& update) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_publish < std::chrono::milliseconds(200)) {
            return;
        }
        last_publish = now;
        std::lock_guard<std::mutex> lock(mutex_);
        if (snap_.stage != Stage::downloading) {
            return;
        }
        snap_.received = update.received;
        snap_.total    = update.total != 0 ? update.total : asset.size;
        changed_       = true;
    };

    if (!net::http_download(asset.url, destination, error, progress, kSetupLimit, &abort_)) {
        return false;
    }

    const auto size = std::filesystem::file_size(destination, ec);
    if (ec) {
        error = "安装包读写失败";
        return false;
    }
    if (asset.size != 0 && static_cast<std::uint64_t>(size) != asset.size) {
        error = "安装包大小与发布信息不符";
        std::filesystem::remove(destination, ec);
        return false;
    }
    const std::string actual = hash::sha256_file(destination);
    if (actual != expected) {
        error = "安装包 SHA-256 校验失败";
        std::filesystem::remove(destination, ec);
        return false;
    }
    return true;
}

void Manager::worker(std::string url, bool manual) {
    Snapshot next = snapshot();
    next.error.clear();
    next.stage = Stage::checking;
    publish(next);

    ReleaseInfo info;
    std::string error;
    const bool ok = check_latest(url, info, error);
    const std::int64_t now = now_seconds();

    // 抖动按数据目录散开：同一台机器每次都落在同一个偏移，不会"每次升级都撞同一分钟"
    const std::int64_t jitter =
        static_cast<std::int64_t>(std::hash<std::string>{}(data_dir_.string()) % 14400U);

    if (!ok) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prefs_.next_check_at = next_check_time(now, false, 0);
            save_preferences(prefs_path_, prefs_);
        }
        publish_error(Stage::failed, error);
        FGB_WARN(kTag, "更新检查失败：%s", error.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    const bool newer = compare_versions(info.tag, current_version_) > 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prefs_.next_check_at = next_check_time(now, true, jitter);
        save_preferences(prefs_path_, prefs_);
    }

    next             = snapshot();
    next.current     = current_version_;
    next.tag         = info.tag;
    next.latest      = info.tag;
    next.name        = info.name;
    next.page_url    = info.url;
    next.available   = newer;
    next.error.clear();
    next.received    = 0;
    next.total       = 0;

    if (!newer) {
        next.stage  = Stage::idle;
        next.staged = Staged{};
        publish(next);
        FGB_INFO(kTag, "更新检查：已是最新（%s）", info.tag.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    bool skipped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        skipped = !manual && !prefs_.skipped_version.empty() &&
                  compare_versions(prefs_.skipped_version, info.tag) == 0;
    }
    next.skipped = skipped;

    const ReleaseAsset* asset = find_installer_asset(info);
    if (asset == nullptr) {
        next.stage = Stage::available;
        next.error = "发布里没有安装包，请用下载页手动安装";
        publish(next);
        FGB_WARN(kTag, "更新检查：%s 没有 Setup 资产", info.tag.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }
    if (skipped) {
        next.stage = Stage::available;
        publish(next);
        FGB_INFO(kTag, "更新检查：%s 已被用户忽略", info.tag.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }
    if (!eligible_) {
        next.stage = Stage::available;
        next.error = "当前程序不是安装器装的，请用下载页手动安装";
        publish(next);
        FGB_INFO(kTag, "更新检查：有新版本 %s，但当前副本不能自动安装", info.tag.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    std::string expected;
    if (!fetch_checksum(info, *asset, expected, error)) {
        next.stage = Stage::available;
        next.error = error;
        publish(next);
        FGB_WARN(kTag, "更新检查：%s", error.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    next.stage    = Stage::downloading;
    next.total    = asset->size;
    next.received = 0;
    publish(next);
    FGB_INFO(kTag, "开始下载更新包：%s", asset->name.c_str());

    const std::filesystem::path destination =
        data_dir_ / "updates" / utf8_to_wide(asset->name);
    if (!download_verified(*asset, destination, expected, error)) {
        publish_error(Stage::failed, error);
        FGB_WARN(kTag, "下载更新包失败：%s", error.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    next               = snapshot();
    next.staged.valid  = true;
    next.staged.version = std::string{trim_tag(info.tag)};
    next.staged.path   = wide_to_utf8(destination.wstring());
    next.staged.sha256 = expected;
    next.staged.size   = static_cast<std::uint64_t>(std::filesystem::file_size(destination));
    next.stage         = Stage::ready;
    next.received      = next.staged.size;
    next.total         = next.staged.size;
    next.error.clear();
    publish(next);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prefs_.staged_version = next.staged.version;
        prefs_.staged_path    = next.staged.path;
        prefs_.staged_sha256  = next.staged.sha256;
        prefs_.staged_size    = static_cast<std::int64_t>(next.staged.size);
        save_preferences(prefs_path_, prefs_);
    }
    FGB_INFO(kTag, "更新包已就绪：%s（%llu 字节）", next.staged.version.c_str(),
             static_cast<unsigned long long>(next.staged.size));
    running_.store(false, std::memory_order_release);
}

bool Manager::start_install(std::string& error) {
    const Snapshot current = snapshot();
    if (current.stage == Stage::installing) {
        error = "正在安装，请稍候";
        return false;
    }
    if (!current.staged.valid) {
        error = "还没有下载好的安装包";
        return false;
    }
#ifdef _WIN32
    std::filesystem::path registered;
    if (!assume_installed_ && !installed_copy(exe_dir_, &registered)) {
        error = "当前程序不是安装器装的，请用下载页手动安装";
        return false;
    }
    const std::filesystem::path setup{utf8_to_wide(current.staged.path)};
    std::error_code ec;
    if (!std::filesystem::exists(setup, ec) || ec) {
        error = "安装包已不存在，请重新下载";
        return false;
    }
    const auto size = std::filesystem::file_size(setup, ec);
    if (ec || (current.staged.size != 0 &&
               static_cast<std::uint64_t>(size) != current.staged.size)) {
        error = "安装包不完整，请重新下载";
        return false;
    }
    if (!launch_installer(setup, error)) {
        return false;
    }
    Snapshot next = current;
    next.stage    = Stage::installing;
    next.error.clear();
    publish(next);
    return true;
#else
    (void)exe_dir_;
    error = "当前平台不支持自动安装";
    return false;
#endif
}

} // namespace fgb::update
