#include "config/osc_map.hpp"

#include "config/ini.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <unordered_map>

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

bool parse_float_value(std::string_view text, float& out) noexcept {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const std::string buffer{text};
    char* end = nullptr;
    const float value = std::strtof(buffer.c_str(), &end);
    if (end == buffer.c_str() || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parse_uint16_value(std::string_view text, std::uint16_t& out) noexcept {
    text = trim(text);
    if (text.empty()) {
        return false;
    }
    const std::string buffer{text};
    char* end              = nullptr;
    const unsigned long v  = std::strtoul(buffer.c_str(), &end, 10);
    if (end == buffer.c_str() || *end != '\0' || v == 0 || v > 65535) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

bool parse_bool_value(std::string_view text, bool& out) noexcept {
    text = trim(text);
    if (text == "1" || text == "true" || text == "on" || text == "yes") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "off" || text == "no") {
        out = false;
        return true;
    }
    return false;
}

// 目标名只允许字母数字下划线：形状名就是标识符，出现别的字符说明写错了
bool valid_target_syntax(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    for (const char c : target) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool commit_temp(const std::filesystem::path& path, const std::filesystem::path& temp,
                 std::string& error) {
    std::error_code ec;
#ifdef _WIN32
    if (::MoveFileExW(temp.wstring().c_str(), path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING) == FALSE) {
        std::filesystem::remove(temp, ec);
        error = "替换 OSC 映射表失败（错误码 " + std::to_string(::GetLastError()) + "）";
        return false;
    }
#else
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "替换 OSC 映射表失败：" + ec.message();
        return false;
    }
#endif
    return true;
}

} // namespace

std::filesystem::path default_osc_map_path() {
#ifdef _WIN32
    // 宽字符环境变量：中文用户名下窄字符路径会乱码
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local != nullptr && local[0] != L'\0') {
        return std::filesystem::path{local} / L"FaceGazeBridge" / L"osc_map.ini";
    }
#endif
    return std::filesystem::path{"osc_map.ini"};
}

bool parse_osc_mapping(std::string_view address, std::string_view spec, OscMapping& out,
                       std::string& error) {
    error.clear();
    address = trim(address);
    if (address.empty() || address.front() != '/') {
        error = "OSC 地址必须以 / 开头";
        return false;
    }

    OscMapping mapping;
    mapping.address = std::string{address};

    bool have_target = false;
    std::size_t pos  = 0;
    while (pos <= spec.size()) {
        const std::size_t comma = spec.find(',', pos);
        std::string_view token  = trim(spec.substr(pos, comma == std::string_view::npos
                                                            ? std::string_view::npos
                                                            : comma - pos));
        if (!token.empty()) {
            const std::size_t eq = token.find('=');
            if (eq == std::string_view::npos) {
                if (token == "invert") {
                    mapping.invert = true;
                } else if (!have_target) {
                    if (!valid_target_syntax(token)) {
                        error = "目标名只能包含字母、数字和下划线";
                        return false;
                    }
                    mapping.target = std::string{token};
                    have_target    = true;
                } else {
                    error = "无法识别的字段: " + std::string{token};
                    return false;
                }
            } else {
                const std::string_view key   = trim(token.substr(0, eq));
                const std::string_view value = trim(token.substr(eq + 1));
                if (key == "scale") {
                    if (!parse_float_value(value, mapping.scale)) {
                        error = "scale 不是合法数字";
                        return false;
                    }
                } else if (key == "offset") {
                    if (!parse_float_value(value, mapping.offset)) {
                        error = "offset 不是合法数字";
                        return false;
                    }
                } else if (key == "invert") {
                    if (!parse_bool_value(value, mapping.invert)) {
                        error = "invert 只能是 0/1/true/false";
                        return false;
                    }
                } else {
                    error = "未知字段: " + std::string{key};
                    return false;
                }
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }

    if (!have_target) {
        error = "缺少目标（例如 JawOpen 或 EyeLidLeft）";
        return false;
    }

    out = std::move(mapping);
    return true;
}

std::string format_osc_mapping(const OscMapping& mapping) {
    std::string text = mapping.target;
    char buffer[64];
    if (mapping.scale != 1.0f) {
        std::snprintf(buffer, sizeof(buffer), ", scale=%g", static_cast<double>(mapping.scale));
        text += buffer;
    }
    if (mapping.offset != 0.0f) {
        std::snprintf(buffer, sizeof(buffer), ", offset=%g", static_cast<double>(mapping.offset));
        text += buffer;
    }
    if (mapping.invert) {
        text += ", invert";
    }
    return text;
}

bool load_osc_map(const std::filesystem::path& path, OscMapOptions& out, std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "OSC 映射表路径为空";
        return false;
    }

    OscMapOptions loaded;
    std::unordered_map<std::string, std::size_t> index_of_address;
    std::string parse_error;

    const bool opened = ini_load(path, [&](const IniEntry& entry) {
        if (!parse_error.empty()) {
            return;
        }
        if (entry.section == "input") {
            if (entry.key == "port" && !parse_uint16_value(entry.value, loaded.port)) {
                parse_error = "port 必须是 1..65535";
            }
            return;
        }
        if (entry.section != "map") {
            return; // 未知节忽略，给未来留扩展位
        }

        OscMapping mapping;
        if (!parse_osc_mapping(entry.key, entry.value, mapping, parse_error)) {
            parse_error = entry.key + ": " + parse_error;
            return;
        }
        const auto it = index_of_address.find(mapping.address);
        if (it != index_of_address.end()) {
            loaded.mappings[it->second] = std::move(mapping); // 重复地址后者覆盖
        } else {
            index_of_address.emplace(mapping.address, loaded.mappings.size());
            loaded.mappings.push_back(std::move(mapping));
        }
    });

    if (!opened) {
        error = "无法读取 OSC 映射表: " + path.string();
        return false;
    }
    if (!parse_error.empty()) {
        error = parse_error;
        return false;
    }

    out = std::move(loaded);
    return true;
}

bool save_osc_map(const std::filesystem::path& path, const OscMapOptions& options,
                  std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "OSC 映射表路径为空";
        return false;
    }
    if (options.port == 0) {
        error = "OSC 端口不能为 0";
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
        error = "无法写入 OSC 映射表";
        return false;
    }
    writer.comment("FaceGaze Bridge 通用 OSC 输入映射；改完需要重启引擎生效");
    writer.comment("值语法: Target[, scale=1.0][, offset=0.0][, invert]");
    writer.comment("Target 可以是统一形状名（JawOpen）或眼睛目标（EyeLidLeft / EyeLeftX …）");
    writer.section("input");
    writer.kv("port", static_cast<double>(options.port), 0);
    writer.section("map");
    for (const OscMapping& mapping : options.mappings) {
        writer.kv(mapping.address, format_osc_mapping(mapping));
    }
    writer.close();

    if (std::filesystem::is_regular_file(path, ec)) {
        std::filesystem::copy_file(path, path.string() + ".bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

    return commit_temp(path, temp, error);
}

} // namespace fgb::config
