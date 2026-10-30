#include "config/outputs.hpp"

#include "config/ini.hpp"

#include <algorithm>
#include <cstdlib>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fgb::config {
namespace {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool parse_uint16_value(std::string_view text, std::uint16_t& out) noexcept {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const std::string buffer{text};
    char* end = nullptr;
    const unsigned long value = std::strtoul(buffer.c_str(), &end, 10);
    if (end == buffer.c_str() || *end != '\0' || value == 0 || value > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_int_value(std::string_view text, int min, int max, int& out) noexcept {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const std::string buffer{text};
    char* end = nullptr;
    const long value = std::strtol(buffer.c_str(), &end, 10);
    if (end == buffer.c_str() || *end != '\0' || value < min || value > max) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool is_path_separator(char c) noexcept {
    return c == '/' || c == '\\';
}

bool is_reserved_name_char(char c) noexcept {
    // Windows 文件名禁用字符；冒号也禁，避免 "C:" 之类看起来像盘符
    switch (c) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '|':
    case '?':
    case '*':
        return true;
    default:
        return false;
    }
}

std::string profile_stem(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F || is_path_separator(c) || is_reserved_name_char(c)) {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    if (out.empty() || out == "." || out == "..") {
        out = "profile";
    }
    return out;
}

// 把一条 [section] key=value 应用到输出设置；失败时给出可显示的原因
bool apply_output_entry(const IniEntry& entry, OutputOptions& out, std::string& bad_error) {
    if (entry.section == "output") {
        if (entry.key == "enabled") {
            out.vrchat_enabled = parse_bool(entry.value, out.vrchat_enabled);
        } else if (entry.key == "vrchat") {
            if (!parse_target(entry.value, out.vrchat_host, out.vrchat_port)) {
                bad_error = "output.vrchat 需要 host:port";
                return false;
            }
        } else if (entry.key == "eye_mode") {
            out.eye_mode = entry.value;
        } else if (entry.key == "params_mode") {
            out.params_mode = entry.value;
        }
        return true;
    }
    if (entry.section == "vmc") {
        if (entry.key == "enabled") {
            out.vmc_enabled = parse_bool(entry.value, out.vmc_enabled);
        } else if (entry.key == "target") {
            if (!parse_target(entry.value, out.vmc_host, out.vmc_port)) {
                bad_error = "vmc.target 需要 host:port";
                return false;
            }
        }
        return true;
    }
    if (entry.section == "shm") {
        if (entry.key == "enabled") {
            out.shm_enabled = parse_bool(entry.value, out.shm_enabled);
        } else if (entry.key == "name") {
            out.shm_name = entry.value;
        }
        return true;
    }
    if (entry.section == "ws") {
        if (entry.key == "enabled") {
            out.ws_enabled = parse_bool(entry.value, out.ws_enabled);
        } else if (entry.key == "port") {
            if (!parse_uint16_value(entry.value, out.ws_port)) {
                bad_error = "ws.port 必须是 1..65535";
                return false;
            }
        } else if (entry.key == "rate") {
            if (!parse_int_value(entry.value, 0, 1000, out.ws_rate_hz)) {
                bad_error = "ws.rate 必须在 0..1000";
                return false;
            }
        } else if (entry.key == "all_shapes") {
            out.ws_all_shapes = parse_bool(entry.value, out.ws_all_shapes);
        }
        return true;
    }
    if (entry.section == "udp") {
        if (entry.key == "enabled") {
            out.udp_enabled = parse_bool(entry.value, out.udp_enabled);
        } else if (entry.key == "target") {
            if (!parse_target(entry.value, out.udp_host, out.udp_port)) {
                bad_error = "udp.target 需要 host:port";
                return false;
            }
        } else if (entry.key == "rate") {
            if (!parse_int_value(entry.value, 0, 1000, out.udp_rate_hz)) {
                bad_error = "udp.rate 必须在 0..1000";
                return false;
            }
        }
    }
    return true;
}

void write_output_sections(IniWriter& writer, const OutputOptions& options) {
    writer.section("output");
    writer.kv_bool("enabled", options.vrchat_enabled);
    writer.kv("vrchat", format_target(options.vrchat_host, options.vrchat_port));
    writer.kv("eye_mode", options.eye_mode);
    writer.kv("params_mode", options.params_mode);

    writer.section("vmc");
    writer.kv_bool("enabled", options.vmc_enabled);
    writer.kv("target", format_target(options.vmc_host, options.vmc_port));

    writer.section("shm");
    writer.kv_bool("enabled", options.shm_enabled);
    writer.kv("name", options.shm_name);

    writer.section("ws");
    writer.kv_bool("enabled", options.ws_enabled);
    writer.kv("port", std::to_string(options.ws_port));
    writer.kv("rate", std::to_string(options.ws_rate_hz));
    writer.kv_bool("all_shapes", options.ws_all_shapes);

    writer.section("udp");
    writer.kv_bool("enabled", options.udp_enabled);
    writer.kv("target", format_target(options.udp_host, options.udp_port));
    writer.kv("rate", std::to_string(options.udp_rate_hz));
}

// .tmp → 原子替换；调用方负责先把内容写完
bool commit_temp(const std::filesystem::path& path, const std::filesystem::path& temp,
                 std::string& error) {
    std::error_code ec;
#ifdef _WIN32
    if (::MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING) == FALSE) {
        std::filesystem::remove(temp, ec);
        error = "替换输出设置失败（错误码 " + std::to_string(::GetLastError()) + "）";
        return false;
    }
#else
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "替换输出设置失败：" + ec.message();
        return false;
    }
#endif
    return true;
}

// 找同名预设；没有就返回空路径。只认文件里写的展示名，不靠文件名猜。
std::filesystem::path find_profile(const std::filesystem::path& dir, std::string_view name) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".ini") {
            continue;
        }
        OutputOptions options;
        std::string profile_name;
        std::string error;
        if (load_output_profile(entry.path(), options, profile_name, error) && profile_name == name) {
            return entry.path();
        }
    }
    return {};
}

} // namespace

std::filesystem::path default_outputs_path() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "outputs.ini";
}

bool parse_target(std::string_view text, std::string& host, std::uint16_t& port) noexcept {
    text = trim(text);
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    host.assign(text.substr(0, colon));
    return parse_uint16_value(text.substr(colon + 1), port);
}

std::string format_target(std::string_view host, std::uint16_t port) {
    std::string out{host};
    out.push_back(':');
    out += std::to_string(port);
    return out;
}

bool validate_outputs(const OutputOptions& options, std::string& error) {
    error.clear();
    if (options.vrchat_host.empty() || options.vrchat_port == 0) {
        error = "VRChat 目标需要 host:port";
        return false;
    }
    if (options.eye_mode != "parameters" && options.eye_mode != "native" &&
        options.eye_mode != "both") {
        error = "眼追模式必须是 parameters / native / both";
        return false;
    }
    if (options.params_mode != "v2" && options.params_mode != "v1" &&
        options.params_mode != "both") {
        error = "参数集必须是 v2 / v1 / both";
        return false;
    }
    if (options.vmc_enabled && (options.vmc_host.empty() || options.vmc_port == 0)) {
        error = "VMC 目标需要 host:port";
        return false;
    }
    if (options.shm_enabled && options.shm_name.empty()) {
        error = "共享内存需要映射名";
        return false;
    }
    if (options.ws_enabled) {
        if (options.ws_port == 0) {
            error = "WebSocket 端口不能为 0";
            return false;
        }
        if (options.ws_rate_hz < 0 || options.ws_rate_hz > 1000) {
            error = "WebSocket 帧率必须在 0..1000";
            return false;
        }
    }
    if (options.udp_enabled) {
        if (options.udp_host.empty() || options.udp_port == 0) {
            error = "UDP 目标需要 host:port";
            return false;
        }
        if (options.udp_rate_hz < 0 || options.udp_rate_hz > 1000) {
            error = "UDP 帧率必须在 0..1000";
            return false;
        }
    }
    return true;
}

bool load_outputs(const std::filesystem::path& path, OutputOptions& out, std::string& error) {
    out = OutputOptions{};
    error.clear();
    if (path.empty()) {
        error = "输出设置路径为空";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "输出设置文件不存在";
        return false;
    }

    bool bad = false;
    std::string bad_error;
    if (!ini_load(path, [&](const IniEntry& entry) {
            if (!apply_output_entry(entry, out, bad_error)) {
                bad = true;
            }
        })) {
        out   = OutputOptions{};
        error = "无法读取输出设置";
        return false;
    }

    if (bad) {
        out   = OutputOptions{};
        error = bad_error;
        return false;
    }
    if (!validate_outputs(out, error)) {
        out = OutputOptions{};
        return false;
    }
    return true;
}

bool save_outputs(const std::filesystem::path& path, const OutputOptions& options,
                  std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "输出设置路径为空";
        return false;
    }
    if (!validate_outputs(options, error)) {
        return false;
    }

    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::filesystem::path temp = path;
    temp += ".tmp";

    IniWriter writer;
    if (!writer.open(temp)) {
        error = "无法写入输出设置";
        return false;
    }
    writer.comment("FaceGaze Bridge 输出设置；改完需要重启引擎生效");
    write_output_sections(writer, options);
    writer.close();

    // 保存前留一份 .bak：手改坏了还能找回上一版
    if (std::filesystem::is_regular_file(path, ec)) {
        std::filesystem::copy_file(path, path.string() + ".bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

    return commit_temp(path, temp, error);
}

std::filesystem::path default_profiles_dir() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "outputs-profiles";
}

std::filesystem::path path_from_utf8(std::string_view text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {}; // 非法 UTF-8：交给调用方按"路径不可用"处理，不猜编码
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                          length);
    return std::filesystem::path{wide};
#else
    return std::filesystem::path{std::string{text}};
#endif
}

bool validate_profile_name(std::string_view name, std::string& error) {
    error.clear();
    if (name.empty()) {
        error = "预设名称不能为空";
        return false;
    }
    if (name.size() > 60) {
        error = "预设名称太长（最多 60 字节）";
        return false;
    }
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '\t' || name.back() == '\t') {
        error = "预设名称首尾不能有空白";
        return false;
    }
    if (name == "." || name == "..") {
        error = "预设名称不能是 . 或 ..";
        return false;
    }
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            error = "预设名称不能包含控制字符";
            return false;
        }
        if (is_path_separator(c) || is_reserved_name_char(c)) {
            error = "预设名称不能包含 \\ / : * ? \" < > |";
            return false;
        }
    }
    return true;
}

bool load_output_profile(const std::filesystem::path& file, OutputOptions& out, std::string& name,
                         std::string& error) {
    out  = OutputOptions{};
    name.clear();
    error.clear();

    bool bad = false;
    std::string bad_error;
    if (!ini_load(file, [&](const IniEntry& entry) {
            if (entry.section == "profile" && entry.key == "name") {
                name = entry.value;
                return;
            }
            if (!apply_output_entry(entry, out, bad_error)) {
                bad = true;
            }
        })) {
        out   = OutputOptions{};
        error = "无法读取输出预设";
        return false;
    }
    if (bad) {
        out   = OutputOptions{};
        error = bad_error;
        return false;
    }
    if (name.empty()) {
        name = file.stem().string(); // 手写的预设没有 [profile]：用文件名兜底
    }
    if (!validate_outputs(out, error)) {
        out = OutputOptions{};
        return false;
    }
    return true;
}

bool list_output_profiles(const std::filesystem::path& dir, std::vector<OutputProfile>& out,
                          std::string& error) {
    out.clear();
    error.clear();
    if (dir.empty()) {
        error = "预设目录不可用";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return true; // 还没建过预设
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            error = "读取预设目录失败：" + ec.message();
            return false;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".ini") {
            continue;
        }
        OutputOptions options;
        std::string name;
        std::string load_error;
        if (!load_output_profile(entry.path(), options, name, load_error)) {
            continue; // 坏掉的预设不显示，也不阻止其它预设
        }
        out.push_back(OutputProfile{name, entry.path()});
    }
    std::sort(out.begin(), out.end(), [](const OutputProfile& a, const OutputProfile& b) {
        return a.name < b.name;
    });
    return true;
}

bool save_output_profile(const std::filesystem::path& dir, std::string_view name,
                         const OutputOptions& options, std::string& error) {
    error.clear();
    if (!validate_profile_name(name, error) || !validate_outputs(options, error)) {
        return false;
    }
    if (dir.empty()) {
        error = "预设目录不可用";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "无法创建预设目录：" + ec.message();
        return false;
    }

    std::filesystem::path target = find_profile(dir, name);
    if (target.empty()) {
        // 文件名清洗后可能撞车（"a/b" 与 "a_b"）：逐个试到空位，展示名以文件内为准
        const std::string stem = profile_stem(name);
        for (int suffix = 0; suffix < 1000 && target.empty(); ++suffix) {
            std::string file_name = stem;
            if (suffix > 0) {
                file_name += "-" + std::to_string(suffix);
            }
            file_name += ".ini";
            const std::filesystem::path candidate = dir / path_from_utf8(file_name);
            if (!std::filesystem::exists(candidate, ec)) {
                target = std::move(candidate);
            }
        }
        if (target.empty()) {
            error = "无法为预设分配文件名";
            return false;
        }
    }

    std::filesystem::path temp = target;
    temp += ".tmp";

    IniWriter writer;
    if (!writer.open(temp)) {
        error = "无法写入预设文件";
        return false;
    }
    writer.comment("FaceGaze Bridge 输出预设；在界面输出页选择后应用");
    writer.section("profile");
    writer.kv("name", name);
    write_output_sections(writer, options);
    writer.close();
    return commit_temp(target, temp, error);
}

bool delete_output_profile(const std::filesystem::path& dir, std::string_view name,
                           std::string& error) {
    error.clear();
    if (dir.empty()) {
        error = "预设目录不可用";
        return false;
    }
    const std::filesystem::path target = find_profile(dir, name);
    if (target.empty()) {
        error = "预设不存在";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::remove(target, ec) || ec) {
        error = "删除预设失败：" + ec.message();
        return false;
    }
    return true;
}

} // namespace fgb::config
