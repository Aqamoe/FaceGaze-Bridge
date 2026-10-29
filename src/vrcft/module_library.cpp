#include "vrcft/module_library.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "net/http_client.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_map>

namespace fgb::vrcft {
namespace {

constexpr char kTag[] = "library";
constexpr char kDefaultRegistryUrl[] = "https://registry.vrcft.io/modules";
constexpr std::size_t kMaxLibraryBytes     = 8u * 1024u * 1024u;
constexpr std::size_t kMaxModuleDownload   = 256u * 1024u * 1024u;

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string registry_url() {
    const char* env = std::getenv("FGB_MODULE_REGISTRY");
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

bool valid_download_url(std::string_view url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://127.0.0.1", 0) == 0 ||
           url.rfind("http://localhost", 0) == 0;
}

std::string md5_file(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) {
        return {};
    }

    DWORD hash_length = 0;
    DWORD written     = 0;
    ::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_length),
                        sizeof(hash_length), &written, 0);

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    char buffer[64 * 1024];
    bool failed = !input;
    while (!failed && input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize got = input.gcount();
        if (got > 0) {
            failed = ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer),
                                      static_cast<ULONG>(got), 0) < 0;
        }
    }

    std::string result;
    if (!failed) {
        std::string digest(static_cast<std::size_t>(hash_length), '\0');
        if (::BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()), hash_length, 0) >= 0) {
            static const char* hex = "0123456789abcdef";
            for (const unsigned char byte : digest) {
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0F]);
            }
        }
    }

    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

// ---------- registry JSON ----------

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }

    bool consume(char expected) {
        if (peek() != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    bool string_value(std::string& out) {
        if (!consume('"')) {
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'u': {
                unsigned code = 0;
                if (pos_ + 4 > text_.size()) {
                    return false;
                }
                for (int i = 0; i < 4; ++i) {
                    const char h = text_[pos_++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') {
                        code |= static_cast<unsigned>(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        code |= static_cast<unsigned>(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        code |= static_cast<unsigned>(h - 'A' + 10);
                    } else {
                        return false;
                    }
                }
                if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= text_.size() &&
                    text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    unsigned low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text_[pos_++];
                        low <<= 4;
                        if (h >= '0' && h <= '9') {
                            low |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            low |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            low |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                if (code < 0x80) {
                    out.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else if (code < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
                break;
            }
            default:
                out.push_back(esc); // \" \\ \/ 等
                break;
            }
        }
        return false;
    }

    void scalar(std::string& out) {
        const std::size_t begin = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r' ||
                c == '\n') {
                break;
            }
            ++pos_;
        }
        out.assign(text_.substr(begin, pos_ - begin));
    }

    bool skip_value() {
        skip_ws();
        const char c = peek();
        if (c == '"') {
            std::string ignored;
            return string_value(ignored);
        }
        if (c == '{' || c == '[') {
            int depth = 0;
            while (pos_ < text_.size()) {
                const char cur = text_[pos_];
                if (cur == '"') {
                    std::string ignored;
                    if (!string_value(ignored)) {
                        return false;
                    }
                    continue;
                }
                ++pos_;
                if (cur == '{' || cur == '[') {
                    ++depth;
                } else if (cur == '}' || cur == ']') {
                    --depth;
                    if (depth == 0) {
                        return true;
                    }
                }
            }
            return false;
        }
        std::string ignored;
        scalar(ignored);
        return !ignored.empty();
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;
};

bool parse_object(Reader& reader, std::unordered_map<std::string, std::string>& fields) {
    reader.skip_ws();
    if (!reader.consume('{')) {
        return false;
    }
    for (;;) {
        reader.skip_ws();
        if (reader.consume('}')) {
            return true;
        }
        std::string key;
        if (!reader.string_value(key)) {
            return false;
        }
        reader.skip_ws();
        if (!reader.consume(':')) {
            return false;
        }
        reader.skip_ws();
        std::string value;
        if (reader.peek() == '"') {
            if (!reader.string_value(value)) {
                return false;
            }
        } else if (reader.peek() == '{' || reader.peek() == '[') {
            if (!reader.skip_value()) {
                return false;
            }
        } else {
            reader.scalar(value);
            if (value == "null") {
                value.clear();
            }
        }
        fields[key] = std::move(value);

        reader.skip_ws();
        if (reader.consume(',')) {
            continue;
        }
        if (reader.consume('}')) {
            return true;
        }
        return false;
    }
}

std::string field(const std::unordered_map<std::string, std::string>& fields,
                  const char* key) {
    const auto it = fields.find(key);
    return it == fields.end() ? std::string{} : it->second;
}

int int_field(const std::unordered_map<std::string, std::string>& fields, const char* key) {
    const std::string text = field(fields, key);
    return text.empty() ? 0 : std::atoi(text.c_str());
}

double double_field(const std::unordered_map<std::string, std::string>& fields,
                    const char* key) {
    const std::string text = field(fields, key);
    return text.empty() ? 0.0 : std::atof(text.c_str());
}

bool write_module_metadata(const std::filesystem::path& dir, const LibraryEntry& entry,
                           std::string& error) {
    std::string json;
    json::JsonWriter writer{json};
    writer.obj_begin();
    writer.key("ModuleId").str(entry.id);
    writer.key("ModuleName").str(entry.name);
    writer.key("AuthorName").str(entry.author);
    writer.key("Version").str(entry.version);
    writer.key("ModuleDescription").str(entry.description);
    writer.key("UsageInstructions").str(entry.usage);
    writer.key("ModulePageUrl").str(entry.page);
    writer.key("DownloadUrl").str(entry.download_url);
    writer.key("DllFileName").str(entry.dll_name);
    writer.obj_end();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / "module.json", std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "无法写入模块元数据";
        return false;
    }
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    return true;
}

} // namespace

bool parse_registry_json(std::string_view text, std::vector<LibraryEntry>& out,
                         std::string& error) {
    out.clear();
    error.clear();
    // 有些镜像/本地缓存会带 UTF-8 BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Reader reader{text};
    reader.skip_ws();
    if (!reader.consume('[')) {
        error = "模块库响应不是数组";
        return false;
    }
    for (;;) {
        reader.skip_ws();
        if (reader.consume(']')) {
            return true;
        }
        if (reader.peek() == '{') {
            std::unordered_map<std::string, std::string> fields;
            if (!parse_object(reader, fields)) {
                error = "模块库 JSON 解析失败";
                return false;
            }
            LibraryEntry entry;
            entry.id           = field(fields, "ModuleId");
            entry.name         = field(fields, "ModuleName");
            entry.author       = field(fields, "AuthorName");
            entry.version      = field(fields, "Version");
            entry.description  = field(fields, "ModuleDescription");
            entry.usage        = field(fields, "UsageInstructions");
            entry.page         = field(fields, "ModulePageUrl");
            entry.download_url = field(fields, "DownloadUrl");
            entry.dll_name     = field(fields, "DllFileName");
            entry.file_hash    = field(fields, "FileHash");
            entry.last_updated = field(fields, "LastUpdated");
            entry.downloads    = int_field(fields, "Downloads");
            entry.ratings      = int_field(fields, "Ratings");
            entry.rating       = double_field(fields, "Rating");
            if (!entry.id.empty() && valid_download_url(entry.download_url)) {
                out.push_back(std::move(entry));
            }
        } else {
            reader.skip_value();
        }

        reader.skip_ws();
        if (reader.consume(',')) {
            continue;
        }
        if (reader.consume(']')) {
            return true;
        }
        error = "模块库 JSON 结构不完整";
        return false;
    }
}

std::filesystem::path default_library_cache_path() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "library.json";
}

ModuleLibrary::ModuleLibrary(ModuleRegistry* registry, std::filesystem::path cache_path)
    : registry_(registry), cache_path_(std::move(cache_path)) {
    if (!cache_path_.empty()) {
        download_dir_ = cache_path_.parent_path() / "downloads";
    }
    status_.stale = true;
}

ModuleLibrary::~ModuleLibrary() { stop(); }

void ModuleLibrary::load_cache() {
    if (cache_path_.empty()) {
        return;
    }
    std::ifstream input(cache_path_, std::ios::binary);
    if (!input) {
        return;
    }
    std::string body{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::vector<LibraryEntry> entries;
    std::string error;
    if (!parse_registry_json(body, entries, error)) {
        FGB_WARN(kTag, "模块库缓存损坏：%s", error.c_str());
        return;
    }
    cache_written_unix_ = file_mtime_unix(cache_path_);
    std::lock_guard<std::mutex> lock(mutex_);
    entries_       = std::move(entries);
    status_.stale  = true;
    status_.fetched_at =
        cache_written_unix_ != 0 ? unix_to_local_string(cache_written_unix_) : std::string{};
    FGB_DEBUG(kTag, "模块库缓存：%zu 个模块，更新于 %s", entries_.size(), status_.fetched_at.c_str());
    changed_ = true;
}

void ModuleLibrary::join_finished() {
    if (install_worker_.joinable() && install_done_.load(std::memory_order_acquire)) {
        install_worker_.join();
    }
    if (refresh_worker_.joinable() && refresh_done_.load(std::memory_order_acquire)) {
        refresh_worker_.join();
    }
}

void ModuleLibrary::start_install_worker(std::function<void()> work) {
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

void ModuleLibrary::start_refresh_worker(std::function<void()> work) {
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

void ModuleLibrary::refresh_if_stale(std::chrono::seconds max_age) {
    if (cache_written_unix_ != 0) {
        const std::int64_t age = now_unix() - cache_written_unix_;
        if (age >= 0 && age < static_cast<std::int64_t>(max_age.count())) {
            return;
        }
    }
    refresh();
}

bool ModuleLibrary::refresh() {
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

void ModuleLibrary::do_refresh() {
    std::string body;
    std::string error;
    // registry.vrcft.io 偶尔会超时（实测同一个地址连续请求第三次会卡 20 s），
    // 失败一次就放弃会让界面显得"坏掉了"；短暂重试一次再报错。
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
        FGB_WARN(kTag, "模块库刷新失败：%s", error.c_str());
        return;
    }

    std::vector<LibraryEntry> entries;
    std::string parse_error;
    if (!parse_registry_json(body, entries, parse_error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.refreshing    = false;
        status_.refresh_error = parse_error;
        status_.stale         = true;
        changed_              = true;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_              = std::move(entries);
        status_.refreshing    = false;
        status_.refresh_error.clear();
        status_.stale         = false;
        status_.fetched_at    = now_local_string();
        changed_              = true;
    }
    cache_written_unix_ = now_unix();
    save_cache(body);
    FGB_INFO(kTag, "模块库已刷新：%zu 个模块", entries_.size());
}

bool ModuleLibrary::install(std::string entry_id) {
    if (!install_done_.load(std::memory_order_acquire)) {
        return false;
    }
    LibraryEntry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const LibraryEntry& item) { return item.id == entry_id; });
        if (it == entries_.end()) {
            status_.phase = "error";
            status_.error = "模块库里没有这个条目";
            changed_      = true;
            return false;
        }
        entry = *it;
        status_.phase      = "downloading";
        status_.error.clear();
        status_.install_id = entry.id;
        status_.received   = 0;
        status_.total      = 0;
        changed_           = true;
    }
    start_install_worker([this, entry = std::move(entry)] { do_download(entry); });
    return true;
}

void ModuleLibrary::do_download(LibraryEntry entry) {
    std::error_code ec;
    std::filesystem::create_directories(download_dir_, ec);

    const std::string extension =
        lower(std::filesystem::path{entry.download_url}.extension().string());
    // 只有明确的 .dll 才按单文件处理，其余一律当压缩包（VRCFT 的下载地址有时不带扩展名）
    const bool is_dll = extension == ".dll";
    const std::filesystem::path target =
        download_dir_ / (safe_file_name(entry.id) + (is_dll ? ".dll" : ".zip"));

    std::string error;
    const bool ok = net::http_download(
        entry.download_url, target, error,
        [this](const net::HttpProgress& progress) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.received = progress.received;
            status_.total    = progress.total;
            changed_         = true;
        },
        kMaxModuleDownload);
    if (!ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = "error";
        status_.error = "下载失败：" + error;
        changed_      = true;
        return;
    }

    if (!entry.file_hash.empty()) {
        const std::string actual = md5_file(target);
        if (lower(actual) != lower(entry.file_hash)) {
            std::filesystem::remove(target, ec);
            std::lock_guard<std::mutex> lock(mutex_);
            status_.phase = "error";
            status_.error = "校验失败：下载内容与模块库的 MD5 不一致";
            changed_      = true;
            FGB_WARN(kTag, "模块 %s 校验失败（期望 %s，实际 %s）", entry.name.c_str(),
                     entry.file_hash.c_str(), actual.c_str());
            return;
        }
    }

    // 裸 DLL 的下载：补一个带完整元数据的目录，安装后名字/作者/版本不会丢
    std::filesystem::path package = target;
    if (is_dll) {
        const std::filesystem::path dir =
            download_dir_ / ("pkg-" + safe_file_name(entry.id));
        std::filesystem::remove_all(dir, ec);
        if (!write_module_metadata(dir, entry, error)) {
            std::filesystem::remove(target, ec);
            std::lock_guard<std::mutex> lock(mutex_);
            status_.phase = "error";
            status_.error = error;
            changed_      = true;
            return;
        }
        std::filesystem::copy_file(target, dir / target.filename(), ec);
        if (ec) {
            std::filesystem::remove_all(dir, ec);
            std::filesystem::remove(target, ec);
            std::lock_guard<std::mutex> lock(mutex_);
            status_.phase = "error";
            status_.error = "无法准备模块安装目录";
            changed_      = true;
            return;
        }
        package = dir;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_install_ready_ = true;
    pending_install_id_    = entry.id;
    pending_install_path_  = package;
    status_.phase          = "installing";
    changed_               = true;
}

LibraryEvents ModuleLibrary::poll() {
    join_finished();

    LibraryEvents events;
    std::string install_id;
    std::filesystem::path install_path;
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
        }
    }

    if (!install_path.empty() && registry_ != nullptr) {
        std::string error;
        const bool ok = registry_->install_path(install_path, error, "registry", true);
        std::error_code ec;
        std::filesystem::remove_all(install_path, ec);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.phase = ok ? "idle" : "error";
            status_.error = ok ? std::string{} : error;
            status_.install_id.clear();
            status_.received = 0;
            status_.total    = 0;
            changed_         = true;
        }
        events.changed = true;
        events.modules_changed = ok;
        if (ok) {
            FGB_INFO(kTag, "模块库安装完成：%s", install_id.c_str());
        } else {
            FGB_WARN(kTag, "模块库安装失败：%s", error.c_str());
        }
    }
    return events;
}

LibrarySnapshot ModuleLibrary::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return LibrarySnapshot{entries_, status_};
}

void ModuleLibrary::save_cache(const std::string& body) const {
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

void ModuleLibrary::stop() {
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
