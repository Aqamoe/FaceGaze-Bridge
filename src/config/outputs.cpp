#include "config/outputs.hpp"

#include "config/ini.hpp"

#include <cstdlib>
#include <system_error>

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
    if (!ini_load(path.string(), [&](const IniEntry& entry) {
            if (entry.section == "output") {
                if (entry.key == "vrchat") {
                    if (!parse_target(entry.value, out.vrchat_host, out.vrchat_port)) {
                        bad       = true;
                        bad_error = "output.vrchat 需要 host:port";
                    }
                } else if (entry.key == "eye_mode") {
                    out.eye_mode = entry.value;
                } else if (entry.key == "params_mode") {
                    out.params_mode = entry.value;
                }
                return;
            }
            if (entry.section == "vmc") {
                if (entry.key == "enabled") {
                    out.vmc_enabled = parse_bool(entry.value, out.vmc_enabled);
                } else if (entry.key == "target") {
                    if (!parse_target(entry.value, out.vmc_host, out.vmc_port)) {
                        bad       = true;
                        bad_error = "vmc.target 需要 host:port";
                    }
                }
                return;
            }
            if (entry.section == "shm") {
                if (entry.key == "enabled") {
                    out.shm_enabled = parse_bool(entry.value, out.shm_enabled);
                } else if (entry.key == "name") {
                    out.shm_name = entry.value;
                }
                return;
            }
            if (entry.section == "ws") {
                if (entry.key == "enabled") {
                    out.ws_enabled = parse_bool(entry.value, out.ws_enabled);
                } else if (entry.key == "port") {
                    if (!parse_uint16_value(entry.value, out.ws_port)) {
                        bad       = true;
                        bad_error = "ws.port 必须是 1..65535";
                    }
                } else if (entry.key == "rate") {
                    if (!parse_int_value(entry.value, 0, 1000, out.ws_rate_hz)) {
                        bad       = true;
                        bad_error = "ws.rate 必须在 0..1000";
                    }
                } else if (entry.key == "all_shapes") {
                    out.ws_all_shapes = parse_bool(entry.value, out.ws_all_shapes);
                }
                return;
            }
            if (entry.section == "udp") {
                if (entry.key == "enabled") {
                    out.udp_enabled = parse_bool(entry.value, out.udp_enabled);
                } else if (entry.key == "target") {
                    if (!parse_target(entry.value, out.udp_host, out.udp_port)) {
                        bad       = true;
                        bad_error = "udp.target 需要 host:port";
                    }
                } else if (entry.key == "rate") {
                    if (!parse_int_value(entry.value, 0, 1000, out.udp_rate_hz)) {
                        bad       = true;
                        bad_error = "udp.rate 必须在 0..1000";
                    }
                }
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
    if (!writer.open(temp.string())) {
        error = "无法写入输出设置";
        return false;
    }
    writer.comment("FaceGaze Bridge 输出设置；改完需要重启引擎生效");
    writer.section("output");
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
    writer.close();

    // 保存前留一份 .bak：手改坏了还能找回上一版
    if (std::filesystem::is_regular_file(path, ec)) {
        std::filesystem::copy_file(path, path.string() + ".bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

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

} // namespace fgb::config
