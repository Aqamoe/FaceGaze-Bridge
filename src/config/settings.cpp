#include "config/settings.hpp"

#include "config/ini.hpp"

#include <algorithm>
#include <cstdlib>
#include <string_view>

namespace fgb::config {
namespace {

constexpr char kFilterSection[]    = "filter";
constexpr char kLogSection[]       = "log";
constexpr char kGazeCalibSection[] = "gaze_calib";

// 与 ipc 的 set_filter 钳制一致：界面限制是 UI 的事，落盘这一层要能挡住手改。
constexpr float kMinCutoffLo = 0.01f;
constexpr float kMinCutoffHi = 20.0f;
constexpr float kBetaLo      = 0.0f;
constexpr float kBetaHi      = 5.0f;

bool commit_temp(const std::filesystem::path& path, const std::filesystem::path& temp,
                 std::string& error) {
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        error = "替换设置文件失败";
        return false;
    }
    return true;
}

std::string trimmed(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

} // namespace

bool valid_log_level(const std::string& level) noexcept {
    return level == "trace" || level == "debug" || level == "info" || level == "warn" ||
           level == "error" || level == "off";
}

std::filesystem::path default_settings_path() {
#ifdef _WIN32
    // 与 gaze.ini / outputs.ini 同一个目录；用宽字符环境变量避开中文用户名的乱码
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local != nullptr && local[0] != L'\0') {
        return std::filesystem::path{local} / L"FaceGazeBridge" / L"settings.ini";
    }
#endif
    return std::filesystem::path{"settings.ini"};
}

bool load_settings(const std::filesystem::path& path, EngineSettings& out, std::string& error) {
    out   = EngineSettings{};
    error.clear();
    if (path.empty()) {
        error = "设置文件路径为空";
        return false;
    }

    bool saw_any = false;
    const bool opened = ini_load(path, [&](const IniEntry& entry) {
        saw_any = true;
        const std::string value = trimmed(entry.value);
        if (entry.section == kFilterSection) {
            if (entry.key == "enabled") {
                out.filter_enabled = parse_bool(value, out.filter_enabled);
            } else if (entry.key == "min_cutoff") {
                out.filter_min_cutoff = std::clamp(parse_float(value, out.filter_min_cutoff),
                                                   kMinCutoffLo, kMinCutoffHi);
            } else if (entry.key == "beta") {
                out.filter_beta = std::clamp(parse_float(value, out.filter_beta), kBetaLo, kBetaHi);
            }
        } else if (entry.section == kLogSection) {
            if (entry.key == "level" && valid_log_level(value)) {
                out.log_level = value;
            }
        } else if (entry.section == kGazeCalibSection) {
            if (entry.key == "span" && (value == "wide" || value == "narrow")) {
                out.gaze_calib_span = value;
            }
        }
        // 未知节/键直接忽略：旧版本读新文件不该整份失败
    });

    if (!opened) {
        error = "读不到设置文件：" + path.string();
        return false;
    }
    if (!saw_any) {
        error = "设置文件是空的：" + path.string();
        return false;
    }
    return true;
}

bool save_settings(const std::filesystem::path& path, const EngineSettings& settings,
                   std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "设置文件路径为空";
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
        error = "无法写入设置文件";
        return false;
    }
    writer.comment("FaceGaze Bridge 运行设置；界面改动会自动写到这里，下次启动生效");
    writer.section(kFilterSection);
    writer.kv_bool("enabled", settings.filter_enabled);
    writer.kv("min_cutoff", static_cast<double>(settings.filter_min_cutoff), 4);
    writer.kv("beta", static_cast<double>(settings.filter_beta), 4);
    // 没设置过的项不写空段：手改文件时"空的 [log] 段"只会让人怀疑是不是坏了
    if (!settings.log_level.empty()) {
        writer.section(kLogSection);
        writer.kv("level", std::string_view{settings.log_level});
    }
    if (!settings.gaze_calib_span.empty()) {
        writer.section(kGazeCalibSection);
        writer.kv("span", std::string_view{settings.gaze_calib_span});
    }
    writer.close();

    if (std::filesystem::is_regular_file(path, ec)) {
        std::filesystem::copy_file(path, path.string() + ".bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
    return commit_temp(path, temp, error);
}

} // namespace fgb::config
