#include "vrcft/module_registry.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "config/ini.hpp"
#include "fgb_module_api.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fgb::vrcft {
namespace {

constexpr char kTag[] = "modules";

// std::error_code::message() 在中文 Windows 上是 ACP（GBK）窄串：拼进 UTF-8
// 的错误文本会变成乱码，界面 toast 与日志都读不出来。统一转成 UTF-8。
std::string system_message(const std::error_code& ec) {
#ifdef _WIN32
    const std::string narrow = ec.message();
    if (narrow.empty()) {
        return narrow;
    }
    const int wide_length = ::MultiByteToWideChar(CP_ACP, 0, narrow.data(),
                                                  static_cast<int>(narrow.size()), nullptr, 0);
    if (wide_length <= 0) {
        return narrow;
    }
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, narrow.data(), static_cast<int>(narrow.size()),
                          wide.data(), wide_length);
    const int utf8_length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length,
                                                  nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) {
        return narrow;
    }
    std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_length, utf8.data(), utf8_length,
                          nullptr, nullptr);
    return utf8;
#else
    return ec.message();
#endif
}

#ifndef FGB_VERSION
#define FGB_VERSION "0.0.0"
#endif

// 只比较 x.y.z 的数字段；预发布后缀/Microsoft 式四段版本也能接受（多出来的段按数字比）。
bool version_greater(std::string_view a, std::string_view b) {
    std::size_t ia = 0;
    std::size_t ib = 0;
    while (ia < a.size() || ib < b.size()) {
        auto next = [](std::string_view s, std::size_t& i) {
            while (i < s.size() && (s[i] < '0' || s[i] > '9')) {
                ++i;
            }
            int value = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                value = value * 10 + (s[i] - '0');
                ++i;
            }
            return value;
        };
        const int va = next(a, ia);
        const int vb = next(b, ib);
        if (va != vb) {
            return va > vb;
        }
        if (ia >= a.size() && ib >= b.size()) {
            break;
        }
    }
    return false;
}

bool valid_instance_name(std::string_view name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '=' || c == '[' || c == ']' || c == '#' || c == ';' ||
            c == '\r' || c == '\n') {
            return false;
        }
    }
    return true;
}

std::uint32_t parse_capabilities(const std::vector<std::string>& names) {
    std::uint32_t bits = 0;
    for (const std::string& raw : names) {
        std::string name = raw;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (name == "expression") {
            bits |= FGB_CAP_EXPRESSION;
        } else if (name == "eyes" || name == "eye" || name == "eye_attributes" ||
                   name == "eye-attributes") {
            // ABI v1 的 "eye"（整眼原子）已并入属性级眼睛，清单里两种写法都接受
            bits |= FGB_CAP_EYES;
        } else if (name == "gaze") {
            bits |= FGB_CAP_GAZE;
        } else if (name == "head") {
            bits |= FGB_CAP_HEAD;
        }
    }
    return bits;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    // VRCFT 的 json 有时候带 BOM（.NET 的 UTF8 写入习惯），不去掉会让首键匹配不上
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::filesystem::path first_dll(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::path best;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (lower(entry.path().extension().string()) != ".dll") {
            continue;
        }
        // 多个 dll 时优先挑名字最长的（依赖库通常叫 *Core.dll 之类）
        if (best.empty() ||
            entry.path().filename().string().size() > best.filename().string().size()) {
            best = entry.path();
        }
    }
    return best;
}

// 图标：清单可用 ModuleIcon 指定，否则认模块根目录下常见的 icon.*。
// 只接受图片扩展名、体积有上限，并且解析后的真实路径必须仍在模块目录内
// （软链接/junction 也不能把任意文件当图标吐给界面）。
constexpr std::uintmax_t kMaxIconBytes = 1024 * 1024;

bool icon_extension_ok(const std::filesystem::path& path) {
    const std::string extension = lower(path.extension().string());
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
           extension == ".svg" || extension == ".ico" || extension == ".webp";
}

std::filesystem::path resolve_module_icon(const std::filesystem::path& directory,
                                          const std::string& declared) {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(directory, ec);
    if (ec) {
        return {};
    }
    const std::wstring root_text = root.wstring();
    const auto usable = [&root_text](const std::filesystem::path& candidate) {
        std::error_code inner;
        const std::filesystem::path target = std::filesystem::weakly_canonical(candidate, inner);
        if (inner) {
            return std::filesystem::path{};
        }
        const std::wstring target_text = target.wstring();
        if (target_text.size() <= root_text.size() ||
            target_text.compare(0, root_text.size(), root_text) != 0) {
            return std::filesystem::path{};
        }
        const wchar_t next = target_text[root_text.size()];
        if (next != L'\\' && next != L'/') {
            return std::filesystem::path{};
        }
        if (!std::filesystem::is_regular_file(target, inner) || !icon_extension_ok(target)) {
            return std::filesystem::path{};
        }
        const std::uintmax_t size = std::filesystem::file_size(target, inner);
        if (inner || size == 0 || size > kMaxIconBytes) {
            return std::filesystem::path{};
        }
        return target;
    };

    if (!declared.empty()) {
        const std::filesystem::path relative{declared};
        if (relative.is_absolute() || relative.has_root_name() ||
            relative.has_root_directory()) {
            return {};
        }
        for (const auto& part : relative) {
            if (part.empty() || part == "..") {
                return {};
            }
        }
        return usable(directory / relative);
    }
    for (const char* name : {"icon.png", "icon.jpg", "icon.svg", "icon.ico", "icon.webp"}) {
        std::filesystem::path hit = usable(directory / name);
        if (!hit.empty()) {
            return hit;
        }
    }
    return {};
}

std::uintmax_t directory_size(const std::filesystem::path& directory) {
    std::error_code ec;
    std::uintmax_t total = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        total += entry.file_size(ec);
    }
    return total;
}

std::string now_local_string() {
#ifdef _WIN32
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                  static_cast<unsigned>(now.wMinute));
    return buffer;
#else
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
#endif
}

// 文件夹名：模块 id 里允许 GUID/域名风格的名字，但不允许路径分隔符与 INI 元字符
bool valid_module_id(std::string_view id) {
    if (id.empty() || id.size() > 120) {
        return false;
    }
    for (const char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || c == '[' || c == ']' || c == '=') {
            return false;
        }
    }
    return true;
}

std::string folder_name_for(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (const char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') ||
                        (uc >= 'A' && uc <= 'Z') || c == '.' || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    if (out.empty() || out == "." || out == "..") {
        out = "module";
    }
    return out;
}

std::string state_key(std::string_view id) { return lower(id); }

std::wstring widen(const std::filesystem::path& path) { return path.wstring(); }

std::wstring quote(const std::wstring& text) { return L"\"" + text + L"\""; }

void remove_zone_identifier(const std::filesystem::path& file) {
#ifdef _WIN32
    // 下载来的 DLL 会带 "来自 Internet" 标记，.NET 的 Assembly.LoadFrom 会拦；
    // 数据流删除失败不影响安装，只是可能被运行时拒绝，所以忽略返回值。
    const std::wstring stream = file.wstring() + L":Zone.Identifier";
    ::DeleteFileW(stream.c_str());
#else
    (void)file;
#endif
}

bool looks_like_module_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    if (std::filesystem::exists(dir / "module.json", ec)) {
        return true;
    }
    return !first_dll(dir).empty();
}

// zip 解出来的布局不统一：可能 module.json 在根，也可能套一层目录。
// 深度 2 以内找第一个"像模块"的目录，找不到就用根目录。
std::filesystem::path find_module_root(const std::filesystem::path& extracted) {
    if (looks_like_module_dir(extracted)) {
        return extracted;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(extracted, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_directory() && looks_like_module_dir(entry.path())) {
            return entry.path();
        }
    }
    return extracted;
}

} // namespace

namespace {

std::uint16_t peek16(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t peek32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

// PNG 的块长度与 IHDR 宽高都是大端（ICO/PE 才是小端）
std::uint32_t peek32_be(const unsigned char* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

#ifdef _WIN32
BOOL CALLBACK first_icon_group(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) {
    if (name == nullptr || !IS_INTRESOURCE(name)) {
        return TRUE;
    }
    *reinterpret_cast<int*>(param) = static_cast<int>(reinterpret_cast<INT_PTR>(name));
    return FALSE;
}

void poke16(std::string& out, std::size_t at, std::uint16_t value) {
    out[at]     = static_cast<char>(value & 0xFF);
    out[at + 1] = static_cast<char>((value >> 8) & 0xFF);
}

void poke32(std::string& out, std::size_t at, std::uint32_t value) {
    out[at]     = static_cast<char>(value & 0xFF);
    out[at + 1] = static_cast<char>((value >> 8) & 0xFF);
    out[at + 2] = static_cast<char>((value >> 16) & 0xFF);
    out[at + 3] = static_cast<char>((value >> 24) & 0xFF);
}
#endif

} // namespace

bool find_embedded_png(std::string_view data, std::string& out) {
    out.clear();
    constexpr unsigned char kMagic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t best_start = 0;
    std::size_t best_size  = 0;
    for (std::size_t at = 0; at + 24 <= data.size(); ++at) {
        if (std::memcmp(bytes + at, kMagic, sizeof(kMagic)) != 0) {
            continue;
        }
        if (std::memcmp(bytes + at + 12, "IHDR", 4) != 0) {
            continue;
        }
        const std::uint32_t width  = peek32_be(bytes + at + 16);
        const std::uint32_t height = peek32_be(bytes + at + 20);
        // 图标应当是接近方形的小图；跳过截图、背景图之类的资源
        if (width < 16 || height < 16 || width > 512 || height > 512 ||
            width * 2 < height || height * 2 < width) {
            continue;
        }
        std::size_t pos      = at + 8;
        bool complete        = false;
        while (pos + 12 <= data.size()) {
            const std::uint32_t length = peek32_be(bytes + pos);
            if (length > 8u * 1024u * 1024u || pos + 12u + length > data.size()) {
                break;
            }
            const bool iend = std::memcmp(bytes + pos + 4, "IEND", 4) == 0;
            pos += 12u + length;
            if (iend) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            continue;
        }
        const std::size_t size = pos - at;
        if (size > 1024u * 1024u) {
            continue;
        }
        if (size > best_size) {
            best_size  = size;
            best_start = at;
        }
    }
    if (best_size == 0) {
        return false;
    }
    out.assign(data.substr(best_start, best_size));
    return true;
}

bool read_dll_icon(const std::filesystem::path& dll, std::string& out,
                   std::string& content_type) {
    out.clear();
    content_type.clear();
#ifdef _WIN32
    if (dll.empty()) {
        return false;
    }
    // 只读资源，不跑模块代码；.NET 程序集把 ApplicationIcon 编成 RT_GROUP_ICON。
    HMODULE module = ::LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (module == nullptr) {
        return false;
    }
    bool ok = false;
    int group_id = 0;
    ::EnumResourceNamesW(module, RT_GROUP_ICON, first_icon_group,
                         reinterpret_cast<LONG_PTR>(&group_id));
    if (group_id != 0) {
        HRSRC group_resource = ::FindResourceW(module, MAKEINTRESOURCEW(group_id), RT_GROUP_ICON);
        const DWORD group_size = group_resource != nullptr
                                     ? ::SizeofResource(module, group_resource)
                                     : 0;
        HGLOBAL group_handle =
            group_resource != nullptr ? ::LoadResource(module, group_resource) : nullptr;
        const auto* group =
            group_handle != nullptr ? static_cast<const unsigned char*>(::LockResource(group_handle))
                                    : nullptr;
        if (group != nullptr && group_size >= 6) {
            const std::uint16_t count = peek16(group + 4);
            if (count > 0 && count <= 64 && group_size >= 6u + 14u * count) {
                // ICO 头 + 目录项；每个 RT_ICON 原样跟在后面（可能是 DIB，也可能是 PNG）
                std::string ico(6u + 16u * count, '\0');
                poke16(ico, 0, 0);
                poke16(ico, 2, 1);
                poke16(ico, 4, count);
                std::size_t offset = ico.size();
                bool complete = true;
                for (std::uint16_t i = 0; i < count && complete; ++i) {
                    const unsigned char* entry = group + 6u + 14u * i;
                    const std::uint16_t image_id = peek16(entry + 12);
                    HRSRC image_resource =
                        ::FindResourceW(module, MAKEINTRESOURCEW(image_id), RT_ICON);
                    HGLOBAL image_handle =
                        image_resource != nullptr ? ::LoadResource(module, image_resource)
                                                   : nullptr;
                    const DWORD image_size =
                        image_resource != nullptr ? ::SizeofResource(module, image_resource) : 0;
                    const auto* image =
                        image_handle != nullptr
                            ? static_cast<const unsigned char*>(::LockResource(image_handle))
                            : nullptr;
                    if (image == nullptr || image_size == 0 || offset + image_size > 1024u * 1024u) {
                        complete = false;
                        break;
                    }
                    const std::size_t at = 6u + 16u * i;
                    ico[at]     = static_cast<char>(entry[0]); // width
                    ico[at + 1] = static_cast<char>(entry[1]); // height
                    ico[at + 2] = static_cast<char>(entry[2]); // palette size
                    ico[at + 3] = 0;
                    poke16(ico, at + 4, peek16(entry + 4)); // planes
                    poke16(ico, at + 6, peek16(entry + 6)); // bit count
                    poke32(ico, at + 8, image_size);
                    poke32(ico, at + 12, static_cast<std::uint32_t>(offset));
                    ico.append(reinterpret_cast<const char*>(image), image_size);
                    offset += image_size;
                }
                if (complete) {
                    out = std::move(ico);
                    ok = true;
                }
            }
        }
    }
    ::FreeLibrary(module);
    if (ok) {
        content_type = "image/x-icon";
        return true;
    }
    // 没有 Win32 图标资源：.NET 程序集常把 logo 当 PNG 托管资源直接嵌在 PE 里
    std::error_code size_ec;
    const std::uintmax_t file_size = std::filesystem::file_size(dll, size_ec);
    if (!size_ec && file_size > 0 && file_size <= 64u * 1024u * 1024u) {
        std::ifstream input(dll, std::ios::binary);
        if (input) {
            std::string data{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
            if (find_embedded_png(data, out)) {
                content_type = "image/png";
                return true;
            }
        }
    }
    return false;
#else
    (void)dll;
    (void)content_type;
    return false;
#endif
}

bool parse_module_metadata(std::string_view text, ModuleInfo& out) {
    bool any = false;
    std::string value;

    if (fgb::json::field_string(text, "ModuleId", value)) {
        out.id = value;
        any    = true;
    }
    if (fgb::json::field_string(text, "ModuleName", value)) {
        out.name = value;
        any      = true;
    }
    if (fgb::json::field_string(text, "AuthorName", value)) {
        out.author = value;
    }
    if (fgb::json::field_string(text, "Version", value)) {
        out.version = value;
    }
    if (fgb::json::field_string(text, "ModuleDescription", value)) {
        out.description = value;
    }
    if (fgb::json::field_string(text, "UsageInstructions", value)) {
        out.usage = value;
    }
    if (fgb::json::field_string(text, "ModulePageUrl", value)) {
        out.page_url = value;
    }
    if (fgb::json::field_string(text, "ModuleIcon", value)) {
        out.icon_name = value;
    }
    if (fgb::json::field_string(text, "DllFileName", value) && !value.empty()) {
        out.dll = out.directory / value;
        any     = true;
    }

    // ADR-0030 §8 的可选字段：旧清单没有也照常工作。
    std::vector<std::string> capabilities;
    if (fgb::json::field_string_array(text, "Capabilities", capabilities)) {
        out.declared_capabilities = parse_capabilities(capabilities);
    }
    if (fgb::json::field_string(text, "RoleHint", value)) {
        out.role_hint = value;
    }
    if (fgb::json::field_string(text, "MinHostVersion", value)) {
        out.min_host_version = value;
        out.host_compatible  = !version_greater(value, FGB_VERSION);
    }
    // ABI v2 是唯一 ABI（ADR-0034）：清单写明更低/更高的 ABI 版本时提前在列表里
    // 标成不兼容，不必等到 LoadLibrary 才失败。
    double abi = 0.0;
    if (fgb::json::field_number(text, "AbiVersion", abi)) {
        out.abi_version = static_cast<int>(abi);
        if (out.abi_version != static_cast<int>(FGB_MODULE_ABI_V2)) {
            out.host_compatible = false;
        }
    }

    out.metadata_ok = any;
    return any;
}

const ModuleInfo* find_module(const std::vector<ModuleInfo>& modules, std::string_view id) {
    if (id.empty()) {
        return nullptr;
    }
    const std::string needle = lower(id);
    for (const ModuleInfo& module : modules) {
        if (lower(module.id) == needle || lower(module.name) == needle) {
            return &module;
        }
    }
    return nullptr;
}

std::filesystem::path vrcft_module_root() {
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr || appdata[0] == '\0') {
        return {};
    }
    return std::filesystem::path{appdata} / "VRCFaceTracking" / "CustomLibs";
}

std::filesystem::path default_store_dir() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "modules";
}

std::filesystem::path default_state_path() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "modules.ini";
}

std::optional<VrcftRuntime> find_vrcft_runtime() {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::path, std::string>> candidates;

#ifdef _WIN32
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        candidates.emplace_back(
            std::filesystem::path{buffer}.parent_path() / L"runtime" / L"vrcft", "bundled");
    }
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && local[0] != '\0') {
        candidates.emplace_back(std::filesystem::path{local} / "FaceGazeBridge" / "runtime" /
                                    "vrcft",
                                "local");
    }
#endif
    candidates.emplace_back(
        std::filesystem::path{"C:\\Program Files (x86)\\Steam\\steamapps\\common\\VRCFaceTracking"},
        "vrcft");
    candidates.emplace_back(
        std::filesystem::path{"C:\\Program Files\\Steam\\steamapps\\common\\VRCFaceTracking"},
        "vrcft");

    for (const auto& [dir, origin] : candidates) {
        const std::filesystem::path exe = dir / "VRCFaceTracking.ModuleProcess.exe";
        if (!dir.empty() && std::filesystem::exists(exe, ec)) {
            return VrcftRuntime{dir, exe, origin};
        }
    }
    return std::nullopt;
}

ModuleRegistry::ModuleRegistry(std::filesystem::path state_path, std::filesystem::path store_dir)
    : state_path_(std::move(state_path)), store_dir_(std::move(store_dir)) {
    load_state();
}

void ModuleRegistry::add_import_root(std::filesystem::path path, std::string origin) {
    if (path.empty()) {
        return;
    }
    import_roots_.emplace_back(std::move(path), std::move(origin));
}

void ModuleRegistry::load_state() {
    states_.clear();
    instances_.clear();
    roles_      = ModuleRoles{};
    active_id_.clear();
    next_order_ = 1;
    if (state_path_.empty()) {
        return;
    }
    std::unordered_map<std::string, bool> host_flags;
    config::ini_load(state_path_.string(), [this, &host_flags](const config::IniEntry& entry) {
        if (entry.section == "state" && entry.key == "active") {
            active_id_ = entry.value;
            return;
        }
        if (entry.section == "instances") {
            const std::string name = entry.key;
            const std::string id   = entry.value;
            if (!valid_instance_name(name) || id.empty()) {
                return;
            }
            const bool duplicate = std::any_of(
                instances_.begin(), instances_.end(),
                [&name](const ModuleInstance& instance) {
                    return lower(instance.name) == lower(name);
                });
            if (!duplicate) {
                instances_.push_back(ModuleInstance{name, id, false});
            }
            return;
        }
        if (entry.section == "instance_host") {
            if (valid_instance_name(entry.key)) {
                host_flags[entry.key] = config::parse_bool(entry.value, false);
            }
            return;
        }
        if (entry.section == "roles") {
            const std::string key = lower(entry.key);
            if (key == "expression") {
                roles_.expression = entry.value;
            } else if (key == "eyes" || key == "eye") {
                roles_.eyes = entry.value;
            } else if (key == "gaze") {
                roles_.gaze = entry.value;
            } else if (key == "head") {
                roles_.head = entry.value;
            }
            return;
        }
        constexpr std::string_view prefix = "module.";
        if (entry.section.rfind(prefix, 0) != 0) {
            return;
        }
        const std::string id = entry.section.substr(prefix.size());
        State& state         = states_[state_key(id)];
        if (entry.key == "enabled") {
            state.enabled = config::parse_bool(entry.value, true);
        } else if (entry.key == "source") {
            state.source = entry.value;
        } else if (entry.key == "installed") {
            state.installed_at = entry.value;
        } else if (entry.key == "order") {
            state.order = static_cast<std::uint64_t>(std::strtoull(entry.value.c_str(), nullptr, 10));
        }
    });
    for (ModuleInstance& instance : instances_) {
        const auto it = host_flags.find(instance.name);
        if (it != host_flags.end()) {
            instance.host_process = it->second;
        }
    }
    for (const auto& [id, state] : states_) {
        (void)id;
        if (state.order >= next_order_) {
            next_order_ = state.order + 1;
        }
    }
}

void ModuleRegistry::save_state() const {
    if (state_path_.empty()) {
        return;
    }
    std::error_code ec;
    if (!state_path_.parent_path().empty()) {
        std::filesystem::create_directories(state_path_.parent_path(), ec);
    }

    config::IniWriter writer;
    if (!writer.open(state_path_.string())) {
        FGB_WARN(kTag, "模块状态写不进去：%s", state_path_.string().c_str());
        return;
    }
    writer.comment("FaceGaze Bridge 模块状态（自动生成，可手改）");
    writer.section("state");
    writer.kv("active", active_id_);
    if (!instances_.empty()) {
        writer.section("instances");
        bool any_host = false;
        for (const ModuleInstance& instance : instances_) {
            writer.kv(instance.name, instance.module_id);
            any_host = any_host || instance.host_process;
        }
        if (any_host) {
            writer.section("instance_host");
            for (const ModuleInstance& instance : instances_) {
                writer.kv_bool(instance.name, instance.host_process);
            }
        }
    }
    if (!roles_.expression.empty() || !roles_.eyes.empty() || !roles_.gaze.empty() ||
        !roles_.head.empty()) {
        writer.section("roles");
        writer.kv("expression", roles_.expression);
        writer.kv("eyes", roles_.eyes);
        writer.kv("gaze", roles_.gaze);
        writer.kv("head", roles_.head);
    }
    for (const ModuleInfo& module : modules_) {
        const auto it = states_.find(state_key(module.id));
        if (it == states_.end()) {
            continue;
        }
        const std::string section = "module." + module.id;
        writer.section(section);
        writer.kv_bool("enabled", it->second.enabled);
        writer.kv("source", it->second.source);
        writer.kv("installed", it->second.installed_at);
        writer.kv("order", std::to_string(it->second.order));
    }
    writer.close();
}

void ModuleRegistry::apply_state() {
    bool active_found = false;
    for (ModuleInfo& module : modules_) {
        const auto it = states_.find(state_key(module.id));
        if (it != states_.end()) {
            module.enabled      = it->second.enabled;
            module.source       = it->second.source;
            module.installed_at = it->second.installed_at;
        } else {
            module.enabled = true;
        }
        module.active = !active_id_.empty() && lower(module.id) == lower(active_id_);
        if (module.active && !module.enabled) {
            active_id_.clear();
            module.active = false;
        }
        if (module.active) {
            active_found = true;
        }
    }
    if (!active_id_.empty() && !active_found) {
        active_id_.clear();
    }
}

ModuleInfo ModuleRegistry::scan_directory(const std::filesystem::path& dir,
                                          const std::string& origin) const {
    ModuleInfo info;
    info.directory = dir;
    info.id        = dir.filename().string();
    info.name      = info.id;
    info.source    = origin;

    // 原生模块用 fgb-native.json（字段与 module.json 相同）声明身份，
    // 这样列出模块不需要先 LoadLibrary。
    std::error_code native_ec;
    const bool is_native = std::filesystem::exists(dir / "fgb-native.json", native_ec);
    info.kind = is_native ? ModuleKind::native : ModuleKind::vrcft;
    const std::string metadata = read_file(dir / (is_native ? "fgb-native.json" : "module.json"));
    if (!metadata.empty()) {
        parse_module_metadata(metadata, info);
        info.kind = is_native ? ModuleKind::native : ModuleKind::vrcft;
    }
    if (info.id.empty()) {
        info.id = dir.filename().string();
    }
    if (info.name.empty()) {
        info.name = info.id;
    }
    std::error_code ec;
    if (info.dll.empty() || !std::filesystem::exists(info.dll, ec)) {
        info.dll = first_dll(dir);
    }
    if (!info.metadata_ok && !info.dll.empty()) {
        // 没有 module.json：用 DLL 名当 id/显示名，别用临时目录名
        info.id   = info.dll.stem().string();
        info.name = info.id;
    }
    info.broken = info.dll.empty();
    info.icon   = resolve_module_icon(dir, info.icon_name);
    info.bytes  = directory_size(dir);
    return info;
}

ModuleInfo ModuleRegistry::scan_dll(const std::filesystem::path& dll,
                                    const std::string& origin) const {
    ModuleInfo info;
    info.directory = dll.parent_path();
    info.dll       = dll;
    info.id        = dll.stem().string();
    info.name      = info.id;
    info.source    = origin;
    info.loose     = true;
    info.bytes     = std::filesystem::file_size(dll);
    return info;
}

void ModuleRegistry::scan_store() {
    modules_.clear();
    if (store_dir_.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(store_dir_, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(store_dir_, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }
        if (entry.path().filename().string().rfind(".tmp-", 0) == 0) {
            continue; // 残留的安装临时目录
        }
        ModuleInfo info  = scan_directory(entry.path(), "store");
        const auto state = states_.find(state_key(info.id));
        info.install_order = state != states_.end() ? state->second.order : 0;
        modules_.push_back(std::move(info));
    }
    // 安装顺序升序：老模块在前，新安装的追加在列表下面；同一顺序按名字稳定排序
    std::sort(modules_.begin(), modules_.end(), [](const ModuleInfo& a, const ModuleInfo& b) {
        if (a.install_order != b.install_order) {
            return a.install_order < b.install_order;
        }
        return lower(a.name) < lower(b.name);
    });
}

void ModuleRegistry::scan_import_roots() {
    importable_.clear();
    for (const auto& [root, origin] : import_roots_) {
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            ModuleInfo info;
            if (entry.is_directory()) {
                info = scan_directory(entry.path(), origin);
            } else if (entry.is_regular_file() &&
                       lower(entry.path().extension().string()) == ".dll") {
                info = scan_dll(entry.path(), origin);
            } else {
                continue;
            }
            if (find_module(modules_, info.id) != nullptr) {
                continue; // 已经导入过了
            }
            importable_.push_back(ImportCandidate{std::move(info), origin});
        }
    }

    std::sort(importable_.begin(), importable_.end(),
              [](const ImportCandidate& a, const ImportCandidate& b) {
                  return lower(a.info.name) < lower(b.info.name);
              });
}

void ModuleRegistry::rescan() {
    scan_store();
    apply_state();
    scan_import_roots();
    FGB_DEBUG(kTag, "模块仓 %zu 个，可导入 %zu 个", modules_.size(), importable_.size());
}

const ModuleInfo* ModuleRegistry::find(std::string_view id) const {
    return find_module(modules_, id);
}

const ImportCandidate* ModuleRegistry::find_importable(std::string_view id) const {
    if (id.empty()) {
        return nullptr;
    }
    const std::string needle = lower(id);
    for (const ImportCandidate& candidate : importable_) {
        if (lower(candidate.info.id) == needle || lower(candidate.info.name) == needle) {
            return &candidate;
        }
    }
    return nullptr;
}

void ModuleRegistry::mark_active(std::string_view id) {
    active_id_.clear();
    if (id.empty()) {
        apply_state();
        return;
    }
    const ModuleInfo* module = find(id);
    active_id_               = module != nullptr ? module->id : std::string{id};
    apply_state();
}

bool ModuleRegistry::set_active(std::string_view id) {
    if (!id.empty()) {
        const ModuleInfo* module = find(id);
        if (module == nullptr || !module->enabled) {
            return false;
        }
        active_id_ = module->id;
    } else {
        active_id_.clear();
    }
    apply_state();
    save_state();
    return true;
}

bool ModuleRegistry::add_instance(std::string_view name, std::string_view module_id,
                                  bool host_process) {
    if (!valid_instance_name(name)) {
        return false;
    }
    const ModuleInfo* module = find(module_id);
    if (module == nullptr || module->broken) {
        return false;
    }
    const bool duplicate = std::any_of(
        instances_.begin(), instances_.end(), [&name](const ModuleInstance& instance) {
            return lower(instance.name) == lower(name);
        });
    if (duplicate) {
        return false;
    }
    instances_.push_back(ModuleInstance{std::string{name}, module->id, host_process});
    save_state();
    return true;
}

bool ModuleRegistry::remove_instance(std::string_view name) {
    const std::string needle = lower(name);
    const auto it = std::find_if(instances_.begin(), instances_.end(),
                                 [&needle](const ModuleInstance& instance) {
                                     return lower(instance.name) == needle;
                                 });
    if (it == instances_.end()) {
        return false;
    }
    instances_.erase(it);
    save_state();
    return true;
}

bool ModuleRegistry::set_instance_host(std::string_view name, bool host_process) {
    const std::string needle = lower(name);
    const auto it = std::find_if(instances_.begin(), instances_.end(),
                                 [&needle](const ModuleInstance& instance) {
                                     return lower(instance.name) == needle;
                                 });
    if (it == instances_.end()) {
        return false;
    }
    it->host_process = host_process;
    save_state();
    return true;
}

bool ModuleRegistry::set_roles(const ModuleRoles& roles) {
    auto normalize = [this](std::string value) {
        if (value.empty()) {
            return std::string{};
        }
        const ModuleInfo* module = find(value);
        return module != nullptr ? module->id : value;
    };
    roles_.expression = normalize(roles.expression);
    roles_.eyes       = normalize(roles.eyes);
    roles_.gaze       = normalize(roles.gaze);
    roles_.head       = normalize(roles.head);
    save_state();
    return true;
}

bool ModuleRegistry::set_enabled(std::string_view id, bool enabled) {
    const ModuleInfo* module = find(id);
    if (module == nullptr) {
        return false;
    }
    State& state  = states_[state_key(module->id)];
    state.enabled = enabled;
    if (!enabled && lower(active_id_) == lower(module->id)) {
        active_id_.clear();
    }
    apply_state();
    save_state();
    return true;
}

bool ModuleRegistry::import_module(std::string_view id, std::string& error) {
    error.clear();
    const ImportCandidate* candidate = find_importable(id);
    if (candidate == nullptr) {
        error = "没有找到这个待导入模块";
        return false;
    }

    bool ok = false;
    const std::string import_name = candidate->info.name;
    if (candidate->info.loose) {
        ok = install_loose_dll(candidate->info.dll, error, "import", false, nullptr, nullptr);
    } else {
        ok = install_directory(candidate->info.directory, error, "import", false, nullptr, nullptr);
    }
    if (ok) {
        FGB_INFO(kTag, "已导入模块：%s", import_name.c_str());
    }
    return ok;
}

bool ModuleRegistry::extract_zip(const std::filesystem::path& archive,
                                 const std::filesystem::path& dest, std::string& error) const {
#ifdef _WIN32
    wchar_t system_dir[MAX_PATH]{};
    const UINT length = ::GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = "取不到系统目录，无法解压";
        return false;
    }
    const std::filesystem::path tar =
        std::filesystem::path{system_dir} / L"tar.exe";
    std::error_code ec;
    if (!std::filesystem::exists(tar, ec)) {
        error = "系统缺少 tar.exe（需要 Windows 10 1803 及以上）";
        return false;
    }
    std::filesystem::create_directories(dest, ec);

    std::wstring command = quote(tar.wstring()) + L" -xf " + quote(widen(archive)) + L" -C " +
                           quote(widen(dest));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL ok = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (ok == 0) {
        error = "无法启动解压程序";
        return false;
    }
    ::CloseHandle(process.hThread);
    const DWORD wait = ::WaitForSingleObject(process.hProcess, 120'000);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) {
        ::TerminateProcess(process.hProcess, 1);
        ::CloseHandle(process.hProcess);
        error = "解压超时";
        return false;
    }
    ::GetExitCodeProcess(process.hProcess, &code);
    ::CloseHandle(process.hProcess);
    if (code != 0) {
        error = "压缩包解压失败（文件损坏或格式不支持）";
        return false;
    }

    // 解压后再校验一遍：越界路径/符号链接/超大解压都拒收（zip slip / zip bomb）
    constexpr std::uintmax_t kMaxExtractedBytes = 512ull * 1024ull * 1024ull;
    constexpr std::size_t kMaxExtractedFiles    = 4000;
    std::uintmax_t extracted_bytes = 0;
    std::size_t extracted_files    = 0;
    std::error_code root_ec;
    const std::filesystem::path root = std::filesystem::weakly_canonical(dest, root_ec);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dest, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec)) {
            extracted_bytes += entry.file_size(ec);
            ++extracted_files;
            if (extracted_bytes > kMaxExtractedBytes || extracted_files > kMaxExtractedFiles) {
                error = "压缩包解压后过大，已拒绝安装";
                return false;
            }
        }
        const auto canonical = std::filesystem::weakly_canonical(entry.path(), ec);
        if (ec) {
            error = "压缩包里有无法解析的路径";
            return false;
        }
        const std::filesystem::path relative = std::filesystem::relative(canonical, root, ec);
        if (ec || (!relative.empty() && *relative.begin() == "..")) {
            error = "压缩包包含越界路径：" + entry.path().string();
            return false;
        }
        if (entry.is_symlink(ec)) {
            error = "压缩包包含符号链接，已拒绝安装";
            return false;
        }
    }
    return true;
#else
    (void)archive;
    (void)dest;
    error = "当前平台不支持 zip 安装";
    return false;
#endif
}

bool ModuleRegistry::install_loose_dll(const std::filesystem::path& dll, std::string& error,
                                       const std::string& source, bool replace,
                                       const ModuleMetadataOverride* metadata,
                                       InstallOutcome* outcome) {
    error.clear();
    std::error_code ec;
    const std::filesystem::path temp = store_dir_ / (".tmp-dll-" + std::to_string(
                                                          std::chrono::steady_clock::now()
                                                              .time_since_epoch()
                                                              .count()));
    std::filesystem::create_directories(temp, ec);
    std::filesystem::copy_file(dll, temp / dll.filename(), ec);
    if (ec) {
        error = "复制 DLL 失败：" + system_message(ec);
        std::filesystem::remove_all(temp, ec);
        return false;
    }
    const bool ok = install_directory(temp, error, source, replace, metadata, outcome);
    std::filesystem::remove_all(temp, ec);
    return ok;
}

bool ModuleRegistry::install_directory(const std::filesystem::path& source_dir,
                                       std::string& error, const std::string& source,
                                       bool replace, const ModuleMetadataOverride* metadata,
                                       InstallOutcome* outcome) {
    error.clear();
    if (outcome != nullptr) {
        *outcome = InstallOutcome{};
    }
    std::error_code ec;

    ModuleInfo info = scan_directory(source_dir, "install");
    // 扫描根可能套了一层目录（zip 常见），往上找真正的模块根
    if (info.broken) {
        const std::filesystem::path nested = find_module_root(source_dir);
        if (nested != source_dir) {
            info = scan_directory(nested, "install");
        }
    }
    if (info.broken) {
        error = "没有找到模块 DLL";
        return false;
    }

    // 包里没有 module.json（例如 ALVR 只发一个 DLL）：用模块库条目的元数据补齐，
    // 保证安装后的 ID/名称/版本能和库条目对应。
    if (!info.metadata_ok && metadata != nullptr) {
        if (!metadata->id.empty()) {
            info.id = metadata->id;
        }
        if (!metadata->name.empty()) {
            info.name = metadata->name;
        }
        info.author      = metadata->author;
        info.version     = metadata->version;
        info.description = metadata->description;
        info.usage       = metadata->usage;
        info.page_url    = metadata->page;
    }

    std::string id = info.id;
    if (id.empty()) {
        id = info.dll.stem().string();
    }
    if (!valid_module_id(id)) {
        error = "模块 ID 含有非法字符，拒绝安装";
        return false;
    }
    const std::string folder = folder_name_for(id);
    const std::filesystem::path dest = store_dir_ / folder;
    const bool exists = std::filesystem::exists(dest, ec);

    // 同 ID 已经在仓里：先读旧版本，升级成功后回报"1.0.0 → 1.1.0"，
    // 失败时也能把"当前装的是哪个版本"写进错误里。
    ModuleInfo previous;
    if (exists) {
        previous = scan_directory(dest, "store");
    }
    if (exists && !replace) {
        const std::string installed =
            previous.version.empty() ? std::string{} : "，已安装 v" + previous.version;
        error = "已安装相同 ID 的模块（" + id + installed + "）";
        return false;
    }

    // 先复制到临时目录，完整成功后再替换：更新失败不能把旧版本弄丢
    std::filesystem::create_directories(store_dir_, ec);
    const std::string stamp =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path staging = store_dir_ / (".tmp-copy-" + folder + "-" + stamp);
    std::filesystem::copy(source_dir, staging,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        error = "复制模块文件失败：" + system_message(ec);
        std::filesystem::remove_all(staging, ec);
        return false;
    }

    remove_zone_identifier(staging / info.dll.filename());
    if (!std::filesystem::exists(staging / "module.json", ec) && !info.metadata_ok) {
        // 裸 DLL 安装：补一份最小 module.json，之后扫描/库里才有名字和版本
        std::string json;
        json::JsonWriter writer{json};
        writer.obj_begin();
        writer.key("ModuleId").str(id);
        writer.key("ModuleName").str(info.name);
        writer.key("AuthorName").str(info.author);
        writer.key("Version").str(info.version.empty() ? "local" : info.version);
        writer.key("ModuleDescription").str(info.description);
        writer.key("UsageInstructions").str(info.usage);
        writer.key("ModulePageUrl").str(info.page_url);
        writer.key("DllFileName").str(info.dll.filename().string());
        if (metadata != nullptr && !metadata->download_url.empty()) {
            writer.key("DownloadUrl").str(metadata->download_url);
        }
        writer.obj_end();
        std::ofstream out(staging / "module.json", std::ios::binary);
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
    }

    // 升级时保留模块自己的配置：包内没带 config.ini 就沿用旧目录里那份，
    // 否则每升一次版本用户调好的参数就被清空（ADR-0011：config.ini 归模块所有）。
    if (exists) {
        std::error_code config_ec;
        if (!std::filesystem::exists(staging / "config.ini", config_ec) &&
            std::filesystem::exists(dest / "config.ini", config_ec)) {
            std::filesystem::copy_file(dest / "config.ini", staging / "config.ini",
                                       std::filesystem::copy_options::overwrite_existing,
                                       config_ec);
            if (config_ec) {
                FGB_WARN(kTag, "升级 %s 时未能保留 config.ini：%s", id.c_str(),
                         system_message(config_ec).c_str());
            }
        }
    }

    // 替换旧版本用"先改名再落位"而不是 remove_all：DLL 被正在运行的模块占用时
    // rename 会立刻失败，旧目录原样保留；直接删有可能删掉一半文件再失败，
    // 模块就变成"坏"的，用户只能手动清目录。
    std::filesystem::path backup;
    if (exists) {
        backup = store_dir_ / (".tmp-old-" + folder + "-" + stamp);
        std::filesystem::rename(dest, backup, ec);
        if (ec) {
            // 用错误码而不是 ec.message()：后者是 ANSI 文本，混进 UTF-8 日志会乱码
            error = "无法替换旧版本（模块可能正在运行）：请先在「模块」页停止该模块实例"
                    "或重启引擎后重试（错误码 " +
                    std::to_string(ec.value()) + "）";
            std::filesystem::remove_all(staging, ec);
            return false;
        }
    }
    std::filesystem::rename(staging, dest, ec);
    if (ec) {
        error = "安装目录落位失败：" + system_message(ec);
        std::filesystem::remove_all(staging, ec);
        if (!backup.empty()) {
            // 落位失败要回滚，不能让用户既丢了旧版本又没装上新版本
            std::error_code rollback_ec;
            std::filesystem::rename(backup, dest, rollback_ec);
        }
        return false;
    }
    if (!backup.empty()) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(backup, cleanup_ec);
        if (cleanup_ec) {
            FGB_WARN(kTag, "旧版本目录未能删除：%s", backup.string().c_str());
        }
    }

    // 更新时保留原来的启用状态；新装默认启用
    const auto existing = states_.find(state_key(id));
    State& state        = states_[state_key(id)];
    if (existing == states_.end()) {
        state.enabled = true;
        state.order   = next_order_++;
    }
    state.source       = source.empty() ? "install" : source;
    state.installed_at = now_local_string();

    rescan();
    if (active_id_.empty() && modules_.size() == 1) {
        active_id_ = modules_.front().id;
        apply_state();
    }
    save_state();
    if (outcome != nullptr) {
        outcome->id               = id;
        outcome->name             = info.name;
        outcome->version          = info.version;
        outcome->previous_version = previous.version;
        outcome->upgraded         = exists;
    }
    if (exists) {
        FGB_INFO(kTag, "升级模块：%s（%s，%s → %s）", info.name.c_str(), id.c_str(),
                 previous.version.empty() ? "?" : previous.version.c_str(),
                 info.version.empty() ? "?" : info.version.c_str());
    } else {
        FGB_INFO(kTag, "安装模块：%s（%s，%s）", info.name.c_str(), id.c_str(), state.source.c_str());
    }
    return true;
}

bool ModuleRegistry::install_path(const std::filesystem::path& path, std::string& error,
                                  const std::string& source, bool replace,
                                  const ModuleMetadataOverride* metadata,
                                  InstallOutcome* outcome) {
    error.clear();
    if (outcome != nullptr) {
        *outcome = InstallOutcome{};
    }
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        error = "文件不存在";
        return false;
    }
    if (std::filesystem::is_directory(path, ec)) {
        return install_directory(path, error, source.empty() ? "dir" : source, replace, metadata,
                                 outcome);
    }

    const std::string extension = lower(path.extension().string());
    if (extension == ".dll") {
        return install_loose_dll(path, error, source.empty() ? "dll" : source, replace, metadata,
                                 outcome);
    }
    if (extension != ".zip") {
        error = "只支持 .zip 安装包或 .dll";
        return false;
    }

    const std::filesystem::path temp =
        store_dir_ / (".tmp-zip-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp, ec);
    bool ok = extract_zip(path, temp, error);
    if (ok) {
        const std::filesystem::path root = find_module_root(temp);
        ok = install_directory(root, error, source.empty() ? "zip" : source, replace, metadata,
                               outcome);
    }
    std::filesystem::remove_all(temp, ec);
    return ok;
}

bool ModuleRegistry::remove_module(std::string_view id, std::string& error) {
    error.clear();
    const ModuleInfo* module = find(id);
    if (module == nullptr) {
        error = "模块未安装";
        return false;
    }
    const std::filesystem::path dir = module->directory;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    if (ec) {
        error = "删除失败（可能正在被引擎/模块进程占用）：" + system_message(ec);
        return false;
    }
    states_.erase(state_key(module->id));
    if (lower(active_id_) == lower(module->id)) {
        active_id_.clear();
    }
    const std::string removed = lower(module->id);
    instances_.erase(std::remove_if(instances_.begin(), instances_.end(),
                                    [&removed](const ModuleInstance& instance) {
                                        return lower(instance.module_id) == removed;
                                    }),
                     instances_.end());
    for (std::string* role : {&roles_.expression, &roles_.eyes, &roles_.gaze, &roles_.head}) {
        if (lower(*role) == removed) {
            role->clear();
        }
    }
    rescan();
    save_state();
    FGB_INFO(kTag, "已删除模块：%s", std::string(id).c_str());
    return true;
}

const ModuleInfo* ModuleRegistry::pick_active(std::string_view preferred) const {
    if (!preferred.empty()) {
        return find(preferred);
    }
    if (!active_id_.empty()) {
        const ModuleInfo* active = find(active_id_);
        if (active != nullptr && active->enabled && !active->broken) {
            return active;
        }
    }
    const ModuleInfo* only = nullptr;
    std::size_t enabled_count = 0;
    for (const ModuleInfo& module : modules_) {
        if (!module.enabled || module.broken) {
            continue;
        }
        only = &module;
        ++enabled_count;
        if (enabled_count > 1) {
            return nullptr;
        }
    }
    return enabled_count == 1 ? only : nullptr;
}

} // namespace fgb::vrcft
