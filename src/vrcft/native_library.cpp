#include "vrcft/native_library.hpp"

#include "base/hash.hpp"
#include "base/json.hpp"
#include "base/json_value.hpp"
#include "base/log.hpp"
#include "fgb_module_api.h"
#include "net/http_client.hpp"
#include "net/update_check.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef FGB_VERSION
#define FGB_VERSION "0.0.0"
#endif

namespace fgb::vrcft {
namespace {

constexpr char kTag[] = "native-lib";
constexpr char kDefaultRegistryUrl[] = "https://aqamoe.github.io/fgb-registry/registry.json";
constexpr std::size_t kMaxLibraryBytes   = 8u * 1024u * 1024u;
constexpr std::size_t kMaxModuleDownload = 256u * 1024u * 1024u;
constexpr int kSchemaVersion             = 2;

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string registry_url() {
    const char* env = std::getenv("FGB_NATIVE_REGISTRY");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return kDefaultRegistryUrl;
}

std::string now_local_string() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

std::int64_t file_mtime_unix(const std::filesystem::path& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        return 0;
    }
    ULARGE_INTEGER stamp{};
    stamp.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t kEpochDiff = 116444736000000000ULL; // 1601 → 1970（100ns）
    if (stamp.QuadPart < kEpochDiff) {
        return 0;
    }
    return static_cast<std::int64_t>((stamp.QuadPart - kEpochDiff) / 10000000ULL);
#else
    (void)path;
    return 0;
#endif
}

std::int64_t now_unix() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string unix_to_local_string(std::int64_t unix_seconds) {
    const std::time_t stamp = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    localtime_s(&local, &stamp);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

std::string safe_file_name(std::string_view id) {
    std::string out;
    for (const char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') ||
                        (uc >= 'A' && uc <= 'Z') || c == '.' || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    return out.empty() ? "module" : out;
}

// GitHub Pages / jsDelivr 都是 https；本机地址留给探针与本地联调。
bool valid_download_url(std::string_view url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://127.0.0.1", 0) == 0 ||
           url.rfind("http://localhost", 0) == 0;
}

bool valid_sha256(std::string_view text) {
    if (text.size() != 64) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

bool list_contains(const std::vector<std::string>& items, std::string_view wanted) {
    return std::any_of(items.begin(), items.end(), [wanted](const std::string& item) {
        return lower(item) == lower(wanted);
    });
}

NativeVersion parse_version(const json::Value& node) {
    NativeVersion out;
    out.version      = node.string_at("version");
    out.channel      = node.string_at("channel", "stable");
    out.published_at = node.string_at("publishedAt");
    out.file_name    = node.string_at("fileName");
    out.download_url = node.string_at("downloadUrl");
    out.sha256       = lower(node.string_at("sha256"));
    out.size         = static_cast<std::uint64_t>(node.number_at("size", 0.0));
    out.abi_version  = static_cast<int>(node.number_at("abiVersion", 0.0));
    out.downloads    = static_cast<int>(node.number_at("downloads", 0.0));
    out.notes        = node.string_at("notes");
    out.release_url  = node.string_at("releaseUrl");
    return out;
}

NativeEntry parse_entry(const json::Value& node) {
    NativeEntry out;
    out.id          = node.string_at("id");
    out.name        = node.string_at("name");
    out.summary     = node.string_at("summary");
    out.description = node.string_at("description");
    out.usage       = node.string_at("usage");
    out.homepage    = node.string_at("homepage");
    out.repo        = node.string_at("repo");
    out.license     = node.string_at("license");
    out.icon        = node.string_at("icon");
    out.screenshots = node.string_array_at("screenshots");
    out.tags        = node.string_array_at("tags");
    out.capabilities = node.string_array_at("capabilities");
    out.default_role = node.string_at("defaultRole");
    out.channel      = node.string_at("channel", "stable");
    out.featured     = node.bool_at("featured", false);
    out.verified     = node.bool_at("verified", false);
    out.archived     = node.bool_at("archived", false);
    out.published_at = node.string_at("publishedAt");
    out.updated_at   = node.string_at("updatedAt");

    if (const json::Value* author = node.find("author"); author != nullptr) {
        out.author      = author->string_at("name");
        out.author_url  = author->string_at("url");
    }
    if (const json::Value* requirement = node.find("requires"); requirement != nullptr) {
        out.requires_abi     = static_cast<int>(requirement->number_at("abi", 0.0));
        out.min_host_version = requirement->string_at("minHostVersion");
        out.requires_os      = requirement->string_array_at("os");
        out.requires_arch    = requirement->string_array_at("arch");
    }
    if (const json::Value* stats = node.find("stats"); stats != nullptr) {
        out.stats_downloads = static_cast<int>(stats->number_at("downloads", 0.0));
        out.stats_stars     = static_cast<int>(stats->number_at("stars", 0.0));
    }
    if (const json::Value* latest = node.find("latest"); latest != nullptr) {
        out.latest = parse_version(*latest);
    }
    if (const json::Value* versions = node.find("versions"); versions != nullptr &&
        versions->is_array()) {
        for (const json::Value& item : versions->array) {
            if (item.is_object()) {
                out.versions.push_back(parse_version(item));
            }
        }
    }
    if (const json::Value* i18n = node.find("i18n"); i18n != nullptr && i18n->is_object()) {
        for (const auto& [lang, text] : i18n->object) {
            if (!text.is_object()) {
                continue;
            }
            NativeLocalizedText localized;
            localized.summary     = text.string_at("summary");
            localized.description = text.string_at("description");
            localized.usage       = text.string_at("usage");
            out.i18n.emplace_back(lang, std::move(localized));
        }
    }
    return out;
}

// 包里可能套一层目录（zip 常见）：往下找两级就够，再深就是打包方式有问题。
std::filesystem::path find_manifest(const std::filesystem::path& root) {
    std::error_code ec;
    const std::filesystem::path direct = root / "fgb-native.json";
    if (std::filesystem::exists(direct, ec)) {
        return direct;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }
        const std::filesystem::path nested = entry.path() / "fgb-native.json";
        if (std::filesystem::exists(nested, ec)) {
            return nested;
        }
    }
    return {};
}

} // namespace

bool write_native_package_manifest(const std::filesystem::path& dir, const NativeEntry& entry,
                                   const NativeVersion& version, const std::string& dll_name,
                                   std::string& error) {
    error.clear();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string json;
    json::JsonWriter writer{json};
    writer.obj_begin();
    writer.key("ModuleId").str(entry.id);
    writer.key("ModuleName").str(entry.name);
    writer.key("AuthorName").str(entry.author);
    writer.key("Version").str(version.version);
    writer.key("ModuleDescription").str(entry.description);
    writer.key("UsageInstructions").str(entry.usage);
    writer.key("ModulePageUrl").str(entry.homepage);
    writer.key("DllFileName").str(dll_name);
    writer.key("AbiVersion")
        .integer(version.abi_version != 0 ? version.abi_version : entry.requires_abi);
    writer.key("Capabilities").arr_begin();
    for (const std::string& capability : entry.capabilities) {
        writer.str(capability);
    }
    writer.arr_end();
    if (!entry.default_role.empty()) {
        writer.key("RoleHint").str(entry.default_role);
    }
    writer.obj_end();

    std::ofstream out(dir / "fgb-native.json", std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "无法写入模块清单";
        return false;
    }
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    return static_cast<bool>(out);
}

HostEnvironment host_environment() {
    HostEnvironment env;
    env.abi          = static_cast<int>(FGB_MODULE_ABI_V2);
    env.host_version = FGB_VERSION;
#if defined(_M_X64) || defined(__x86_64__)
    env.arch = "x64";
#elif defined(_M_ARM64)
    env.arch = "arm64";
#else
    env.arch = "x86";
#endif
    env.os = "windows";
    return env;
}

bool parse_native_registry(std::string_view text, NativeRegistry& out, std::string& error) {
    out = NativeRegistry{};
    error.clear();

    json::Value root;
    if (!json::parse(text, root, error)) {
        return false;
    }
    if (!root.is_object()) {
        error = "模块库响应不是对象";
        return false;
    }
    const json::Value* schema = root.find("schemaVersion");
    const int version = schema != nullptr && schema->is_number()
                            ? static_cast<int>(schema->number)
                            : 0;
    if (version != kSchemaVersion) {
        error = "模块库结构版本是 " + std::to_string(version) + "，本程序只认识 " +
                std::to_string(kSchemaVersion) + "，请升级 FaceGaze Bridge";
        return false;
    }

    out.generated_at = root.string_at("generatedAt");
    out.generator    = root.string_at("generator");
    if (const json::Value* registry = root.find("registry"); registry != nullptr) {
        out.title       = registry->string_at("title");
        out.description = registry->string_at("description");
        out.homepage    = registry->string_at("homepage");
    }

    const json::Value* modules = root.find("modules");
    if (modules == nullptr || !modules->is_array()) {
        error = "模块库缺少 modules 数组";
        return false;
    }

    const HostEnvironment env = host_environment();
    std::size_t skipped        = 0;
    for (const json::Value& node : modules->array) {
        if (!node.is_object()) {
            ++skipped;
            continue;
        }
        NativeEntry entry = parse_entry(node);
        // 浏览器地址（http 页面）不能带 .dll：能装的东西必须有问题可查的来源
        if (entry.id.empty() || entry.name.empty() || entry.latest.version.empty() ||
            !valid_download_url(entry.latest.download_url)) {
            ++skipped;
            continue;
        }
        evaluate_compatibility(entry, env);
        out.modules.push_back(std::move(entry));
    }
    if (skipped != 0) {
        FGB_WARN(kTag, "模块库有 %zu 个条目缺少 id/name/latest 或下载地址不可用，已跳过", skipped);
    }
    return true;
}

void evaluate_compatibility(NativeEntry& entry, const HostEnvironment& env) {
    entry.compatible = true;
    entry.incompatible_code.clear();
    entry.incompatible_reason.clear();

    const int abi = entry.requires_abi != 0 ? entry.requires_abi : entry.latest.abi_version;
    if (abi != 0 && env.abi != 0 && abi != env.abi) {
        entry.compatible = false;
        entry.incompatible_code = "abi";
        entry.incompatible_reason =
            "需要模块 ABI " + std::to_string(abi) + "，本程序是 " + std::to_string(env.abi);
    } else if (!entry.requires_os.empty() && !list_contains(entry.requires_os, env.os)) {
        entry.compatible        = false;
        entry.incompatible_code = "os";
        entry.incompatible_reason = "只支持这些系统：" + entry.requires_os.front();
    } else if (!entry.requires_arch.empty() && !list_contains(entry.requires_arch, env.arch)) {
        entry.compatible        = false;
        entry.incompatible_code = "arch";
        entry.incompatible_reason = "只支持这些架构：" + entry.requires_arch.front();
    } else if (!entry.min_host_version.empty() &&
               update::compare_versions(env.host_version, entry.min_host_version) < 0) {
        entry.compatible        = false;
        entry.incompatible_code = "host_version";
        entry.incompatible_reason =
            "需要宿主 " + entry.min_host_version + " 或更新，当前是 " + env.host_version;
    }
}

const NativeVersion* select_version(const NativeEntry& entry, std::string_view version) {
    if (version.empty()) {
        return entry.latest.version.empty() ? nullptr : &entry.latest;
    }
    for (const NativeVersion& item : entry.versions) {
        if (item.version == version) {
            return &item;
        }
    }
    // latest 可能不在 versions 里（清单生成失败时），单独再比一次
    if (entry.latest.version == version) {
        return &entry.latest;
    }
    return nullptr;
}

bool verify_native_package(const std::filesystem::path& root, const NativeEntry& entry,
                           ModuleInfo& info, std::string& error) {
    error.clear();
    info = ModuleInfo{};
    if (root.empty()) {
        error = "安装包目录为空";
        return false;
    }
    const std::filesystem::path manifest = find_manifest(root);
    if (manifest.empty()) {
        error = "安装包里没有 fgb-native.json，无法核对模块身份";
        return false;
    }
    std::ifstream input(manifest, std::ios::binary);
    if (!input) {
        error = "读不到安装包里的 fgb-native.json";
        return false;
    }
    const std::string body{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    // 目录要先给：清单里的 DllFileName 是相对模块根解析的
    info.directory = manifest.parent_path();
    if (!parse_module_metadata(body, info)) {
        error = "安装包里的 fgb-native.json 无法解析";
        return false;
    }
    if (lower(info.id) != lower(entry.id)) {
        error = "包内 ModuleId（" + info.id + "）与模块库条目（" + entry.id + "）不一致，已拒绝安装";
        return false;
    }
    if (info.abi_version == 0) {
        error = "包内 fgb-native.json 没有声明 AbiVersion，无法确认 ABI 兼容";
        return false;
    }
    if (entry.requires_abi != 0 && info.abi_version != entry.requires_abi) {
        error = "包内 AbiVersion（" + std::to_string(info.abi_version) + "）与模块库声明（" +
                std::to_string(entry.requires_abi) + "）不一致，已拒绝安装";
        return false;
    }
    std::error_code ec;
    if (info.dll.empty() || !std::filesystem::exists(info.dll, ec)) {
        error = "安装包里没有清单声明的模块 DLL";
        return false;
    }
    return true;
}

std::filesystem::path default_native_library_cache_path() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "native-library.json";
}

NativeLibrary::NativeLibrary(ModuleRegistry* registry, std::filesystem::path cache_path)
    : registry_(registry), cache_path_(std::move(cache_path)) {
    if (!cache_path_.empty()) {
        download_dir_ = cache_path_.parent_path() / "downloads";
    }
    status_.stale = true;
}

NativeLibrary::~NativeLibrary() { stop(); }

void NativeLibrary::load_cache() {
    if (cache_path_.empty()) {
        return;
    }
    std::ifstream input(cache_path_, std::ios::binary);
    if (!input) {
        return;
    }
    const std::string body{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    NativeRegistry registry;
    std::string error;
    if (!parse_native_registry(body, registry, error)) {
        FGB_WARN(kTag, "原生模块库缓存损坏：%s", error.c_str());
        return;
    }
    cache_written_unix_ = file_mtime_unix(cache_path_);
    std::lock_guard<std::mutex> lock(mutex_);
    registry_data_     = std::move(registry);
    status_.stale      = true;
    status_.fetched_at = cache_written_unix_ != 0 ? unix_to_local_string(cache_written_unix_)
                                                  : std::string{};
    FGB_DEBUG(kTag, "原生模块库缓存：%zu 个模块，更新于 %s", registry_data_.modules.size(),
              status_.fetched_at.c_str());
    changed_ = true;
}

void NativeLibrary::join_finished() {
    if (install_worker_.joinable() && install_done_.load(std::memory_order_acquire)) {
        install_worker_.join();
    }
    if (refresh_worker_.joinable() && refresh_done_.load(std::memory_order_acquire)) {
        refresh_worker_.join();
    }
}

void NativeLibrary::start_install_worker(std::function<void()> work) {
    join_finished();
    if (!install_done_.load(std::memory_order_acquire)) {
        return;
    }
    install_done_.store(false, std::memory_order_release);
    install_worker_ = std::thread([this, work = std::move(work)] {
        work();
        install_done_.store(true, std::memory_order_release);
    });
}

void NativeLibrary::start_refresh_worker(std::function<void()> work) {
    join_finished();
    if (!refresh_done_.load(std::memory_order_acquire)) {
        return;
    }
    refresh_done_.store(false, std::memory_order_release);
    refresh_worker_ = std::thread([this, work = std::move(work)] {
        work();
        refresh_done_.store(true, std::memory_order_release);
    });
}

void NativeLibrary::refresh_if_stale(std::chrono::seconds max_age) {
    if (cache_written_unix_ != 0) {
        const std::int64_t age = now_unix() - cache_written_unix_;
        if (age >= 0 && age < static_cast<std::int64_t>(max_age.count())) {
            return;
        }
    }
    refresh();
}

bool NativeLibrary::refresh() {
    if (!refresh_done_.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.refreshing = true;
        status_.refresh_error.clear();
        changed_ = true;
    }
    start_refresh_worker([this] { do_refresh(); });
    return true;
}

void NativeLibrary::do_refresh() {
    std::string body;
    std::string error;
    // GitHub Pages 与 jsDelivr 都会偶发抽风（尤其国内线路），失败一次先重试
    bool fetched = false;
    for (int attempt = 0; attempt < 2 && !fetched; ++attempt) {
        fetched = net::http_get(registry_url(), body, error, kMaxLibraryBytes);
        if (!fetched && attempt == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
    }
    if (!fetched) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.refreshing    = false;
        status_.refresh_error = error;
        status_.stale         = true;
        changed_              = true;
        FGB_WARN(kTag, "原生模块库刷新失败：%s", error.c_str());
        return;
    }

    NativeRegistry registry;
    std::string parse_error;
    if (!parse_native_registry(body, registry, parse_error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.refreshing    = false;
        status_.refresh_error = parse_error;
        status_.stale         = true;
        changed_              = true;
        return;
    }

    const std::size_t count = registry.modules.size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registry_data_        = std::move(registry);
        status_.refreshing    = false;
        status_.refresh_error.clear();
        status_.stale         = false;
        status_.fetched_at    = now_local_string();
        changed_              = true;
    }
    cache_written_unix_ = now_unix();
    save_cache(body);
    FGB_INFO(kTag, "原生模块库已刷新：%zu 个模块", count);
}

bool NativeLibrary::install(std::string entry_id, std::string version) {
    if (!install_done_.load(std::memory_order_acquire)) {
        return false;
    }
    NativeEntry entry;
    NativeVersion selected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(registry_data_.modules.begin(), registry_data_.modules.end(),
                                     [&](const NativeEntry& item) {
                                         return lower(item.id) == lower(entry_id);
                                     });
        if (it == registry_data_.modules.end()) {
            status_.phase = "error";
            status_.error = "模块库里没有这个条目";
            changed_      = true;
            return false;
        }
        const NativeVersion* picked = select_version(*it, version);
        if (picked == nullptr) {
            status_.phase = "error";
            status_.error = "模块库里没有这个版本";
            changed_      = true;
            return false;
        }
        if (!it->compatible) {
            status_.phase = "error";
            status_.error = it->incompatible_reason.empty() ? "这个模块与当前版本不兼容"
                                                            : it->incompatible_reason;
            changed_      = true;
            return false;
        }
        entry              = *it;
        selected           = *picked;
        status_.phase      = "downloading";
        status_.error.clear();
        status_.install_id      = entry.id;
        status_.install_version = selected.version;
        status_.received        = 0;
        status_.total           = selected.size;
        changed_                = true;
    }
    start_install_worker(
        [this, entry = std::move(entry), selected] { do_download(entry, selected); });
    return true;
}

void NativeLibrary::set_error(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.phase = "error";
    status_.error = std::move(message);
    changed_      = true;
}

void NativeLibrary::do_download(NativeEntry entry, NativeVersion version) {
    std::error_code ec;
    std::filesystem::create_directories(download_dir_, ec);

    const std::string extension =
        lower(std::filesystem::path{version.download_url}.extension().string());
    // 单个 DLL 也是合法资产；其余一律按 zip 处理（GitHub 资产可能不带扩展名）
    const bool is_dll = extension == ".dll";
    const std::string stem = safe_file_name(entry.id) + "-" + safe_file_name(version.version);
    const std::filesystem::path archive = download_dir_ / (stem + (is_dll ? ".dll" : ".zip"));

    std::string error;
    const bool downloaded = net::http_download(
        version.download_url, archive, error,
        [this](const net::HttpProgress& progress) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.received = progress.received;
            if (progress.total != 0) {
                status_.total = progress.total;
            }
            changed_ = true;
        },
        kMaxModuleDownload);
    if (!downloaded) {
        set_error("下载失败：" + error);
        return;
    }

    const std::uint64_t actual_size =
        static_cast<std::uint64_t>(std::filesystem::file_size(archive, ec));
    if (version.size != 0 && actual_size != version.size) {
        std::filesystem::remove(archive, ec);
        set_error("校验失败：下载大小与模块库不一致（期望 " + std::to_string(version.size) +
                  " 字节，实际 " + std::to_string(actual_size) + " 字节）");
        return;
    }
    if (!valid_sha256(version.sha256)) {
        std::filesystem::remove(archive, ec);
        set_error("模块库条目缺少有效的 sha256，拒绝安装");
        return;
    }
    const std::string actual_hash = hash::sha256_file(archive);
    if (lower(actual_hash) != version.sha256) {
        std::filesystem::remove(archive, ec);
        set_error("校验失败：下载包的 SHA-256 与模块库不一致");
        FGB_WARN(kTag, "模块 %s 校验失败（期望 %s，实际 %s）", entry.name.c_str(),
                 version.sha256.c_str(), actual_hash.c_str());
        return;
    }

    std::filesystem::path package = archive;
    if (!is_dll) {
        const std::filesystem::path dir = download_dir_ / ("pkg-" + stem);
        std::filesystem::remove_all(dir, ec);
        if (registry_ == nullptr || !registry_->extract_archive(archive, dir, error)) {
            if (registry_ == nullptr) {
                error = "模块仓不可用，无法解压安装包";
            }
            std::filesystem::remove_all(dir, ec);
            std::filesystem::remove(archive, ec);
            set_error("解压失败：" + error);
            return;
        }
        ModuleInfo info;
        if (!verify_native_package(dir, entry, info, error)) {
            std::filesystem::remove_all(dir, ec);
            std::filesystem::remove(archive, ec);
            set_error(error);
            return;
        }
        package = dir;
    } else {
        // 裸 DLL 没有包内清单：身份只能靠库条目兜底。必须补一份 fgb-native.json，
        // 否则模块仓会按 module.json 规则把它当成 VRCFT 模块，启动时走错加载路径。
        const std::filesystem::path dir = download_dir_ / ("pkg-" + stem);
        std::filesystem::remove_all(dir, ec);
        bool prepared =
            write_native_package_manifest(dir, entry, version, archive.filename().string(), error);
        if (prepared) {
            ec.clear();
            std::filesystem::copy_file(archive, dir / archive.filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            prepared = !ec;
            if (!prepared) {
                error = "无法准备模块安装目录";
            }
        }
        if (!prepared) {
            std::filesystem::remove_all(dir, ec);
            std::filesystem::remove(archive, ec);
            set_error(error);
            return;
        }
        FGB_WARN(kTag, "模块 %s 是单个 DLL，无法核对包内清单，仅按模块库元数据安装",
                 entry.id.c_str());
        package = dir;
    }

    // 落盘时用"这次装的版本"而不是条目的 latest：用户可能手动选了旧版本回滚
    entry.latest = version;

    std::lock_guard<std::mutex> lock(mutex_);
    pending_install_ready_    = true;
    pending_install_id_       = entry.id;
    pending_install_path_     = package;
    pending_install_metadata_ = entry;
    pending_install_archive_  = archive;
    status_.phase             = "installing";
    changed_                  = true;
}

NativeLibraryEvents NativeLibrary::poll() {
    join_finished();

    NativeLibraryEvents events;
    std::string install_id;
    std::filesystem::path install_path;
    std::filesystem::path archive;
    NativeEntry metadata;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (changed_) {
            events.changed = true;
            changed_       = false;
        }
        if (pending_install_ready_) {
            pending_install_ready_ = false;
            install_id             = pending_install_id_;
            install_path           = pending_install_path_;
            archive                = pending_install_archive_;
            metadata               = pending_install_metadata_;
        }
    }

    if (!install_path.empty() && registry_ != nullptr) {
        // 裸 DLL 走 install_path 会自动补一份清单；zip 里已经有 fgb-native.json，
        // metadata 只兜底 ID/名字/版本一致（不影响包内清单的权威性）。
        ModuleMetadataOverride override;
        override.id           = metadata.id;
        override.name         = metadata.name;
        override.author       = metadata.author;
        override.version      = metadata.latest.version;
        override.description  = metadata.description;
        override.usage        = metadata.usage;
        override.page         = metadata.homepage;
        override.download_url = metadata.latest.download_url;

        std::string error;
        InstallOutcome outcome;
        const bool ok = registry_->install_path(install_path, error, "registry", true, &override,
                                                &outcome);
        std::error_code ec;
        std::filesystem::remove_all(install_path, ec);
        if (!archive.empty()) {
            std::filesystem::remove(archive, ec);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.phase = ok ? "idle" : "error";
            status_.error = ok ? std::string{} : error;
            status_.install_id.clear();
            status_.install_version.clear();
            status_.received = 0;
            status_.total    = 0;
            changed_         = true;
        }
        events.changed         = true;
        events.modules_changed = ok;
        if (ok) {
            FGB_INFO(kTag, "原生模块安装完成：%s%s", install_id.c_str(),
                     outcome.upgraded ? "（覆盖升级）" : "");
        } else {
            FGB_WARN(kTag, "原生模块安装失败：%s", error.c_str());
        }
    }
    return events;
}

NativeLibrarySnapshot NativeLibrary::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return NativeLibrarySnapshot{registry_data_, status_};
}

void NativeLibrary::save_cache(const std::string& body) const {
    if (cache_path_.empty()) {
        return;
    }
    std::error_code ec;
    if (!cache_path_.parent_path().empty()) {
        std::filesystem::create_directories(cache_path_.parent_path(), ec);
    }
    std::filesystem::path temp = cache_path_;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    std::filesystem::remove(cache_path_, ec);
    std::filesystem::rename(temp, cache_path_, ec);
}

void NativeLibrary::stop() {
    stop_.store(true, std::memory_order_release);
    if (install_worker_.joinable()) {
        install_worker_.join();
    }
    if (refresh_worker_.joinable()) {
        refresh_worker_.join();
    }
    install_done_.store(true, std::memory_order_release);
    refresh_done_.store(true, std::memory_order_release);
}

} // namespace fgb::vrcft
