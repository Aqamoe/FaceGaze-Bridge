#include "params/gaze_profile.hpp"

#include "base/log.hpp"
#include "config/ini.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <cstdlib> // _wgetenv
#endif

namespace fgb::params {
namespace {

constexpr char kTag[] = "gaze-profile";

bool finite_or(float value, float fallback, float& out) noexcept {
    if (!std::isfinite(value)) {
        out = fallback;
        return false;
    }
    out = value;
    return true;
}

// 一行六个数的仿射参数；数量不对就返回 false，由调用方决定怎么降级。
bool parse_floats(const std::string& text, float* out, std::size_t count) noexcept {
    std::istringstream input{text};
    for (std::size_t i = 0; i < count; ++i) {
        if (!(input >> out[i])) {
            return false;
        }
    }
    float extra = 0.0f;
    return !(input >> extra); // 多出来的数说明格式不对
}

} // namespace

std::filesystem::path default_gaze_profile_path() {
#ifdef _WIN32
    // 宽字符环境变量：中文用户名下窄字符路径会乱码，档案就写到别处去了
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local != nullptr && local[0] != L'\0') {
        return std::filesystem::path{local} / L"FaceGazeBridge" / L"gaze.ini";
    }
#endif
    return std::filesystem::path{"gaze.ini"};
}

bool load_gaze_profile(const std::string& path, GazeProfile& profile, std::string& error) {
    profile = GazeProfile{};
    error.clear();

    bool saw_any = false;
    const bool opened = config::ini_load(path, [&](const config::IniEntry& entry) {
        saw_any = true;
        if (entry.section.empty()) {
            if (entry.key == "version") {
                profile.version = static_cast<std::uint32_t>(
                    config::parse_float(entry.value, 0.0f));
            } else if (entry.key == "source_kind") {
                profile.source_kind = gaze_source_kind_from_name(entry.value);
            } else if (entry.key == "created_utc") {
                profile.created_utc = entry.value;
            } else if (entry.key == "hmd_identity") {
                profile.hmd_identity = entry.value;
            } else if (entry.key == "residual_p95") {
                finite_or(config::parse_float(entry.value, -1.0f), -1.0f, profile.residual_p95);
            } else if (entry.key == "eye_trim") {
                // "active gain_x gain_y center_x center_y"；缺这条 = 旧档案，判"未知"
                float v[5] = {};
                if (parse_floats(entry.value, v, 5)) {
                    bool finite = true;
                    for (float value : v) {
                        finite = finite && std::isfinite(value);
                    }
                    if (finite) {
                        profile.trim.active    = v[0] >= 0.5f;
                        profile.trim.gain_x    = v[1];
                        profile.trim.gain_y    = v[2];
                        profile.trim.center_x  = v[3];
                        profile.trim.center_y  = v[4];
                        profile.have_trim      = true;
                    }
                }
            } else if (entry.key == "input_bounds") {
                // "lminx lmaxx lminy lmaxy rminx rmaxx rminy rmaxy"
                float v[8] = {};
                if (parse_floats(entry.value, v, 8)) {
                    bool finite = true;
                    for (float value : v) {
                        finite = finite && std::isfinite(value);
                    }
                    if (finite) {
                        for (std::size_t eye = 0; eye < 2; ++eye) {
                            for (std::size_t axis = 0; axis < 4; ++axis) {
                                profile.input_bounds[eye][axis] = v[eye * 4 + axis];
                            }
                        }
                        profile.have_bounds = true;
                    }
                }
            }
            return;
        }
        if (entry.section != "left" && entry.section != "right") {
            return; // 未知段忽略：旧版本/手改不炸
        }
        GazeEyeMap& eye = profile.eyes[entry.section == "left" ? 0 : 1];
        if (entry.key == "active") {
            eye.active = config::parse_bool(entry.value, false);
        } else if (entry.key == "map") {
            // "a b c d e f"
            float v[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
            if (!parse_floats(entry.value, v, 6)) {
                FGB_WARN(kTag, "gaze.ini 的 [%s] map 不是 6 个数，整段忽略",
                         entry.section.c_str());
                eye = GazeEyeMap{};
                return;
            }
            for (float& value : v) {
                if (!std::isfinite(value)) {
                    eye = GazeEyeMap{};
                    return;
                }
            }
            eye.a = v[0];
            eye.b = v[1];
            eye.c = v[2];
            eye.d = v[3];
            eye.e = v[4];
            eye.f = v[5];
        }
    });

    if (!opened) {
        error = "读不到注视标定档案：" + path;
        return false;
    }
    if (!saw_any) {
        error = "注视标定档案是空的：" + path;
        return false;
    }
    if (!profile.ready()) {
        error = "注视标定档案缺少 version / source_kind / map，按未标定处理";
        return false;
    }
    return true;
}

bool save_gaze_profile(const std::string& path, const GazeProfile& profile, std::string& error) {
    namespace fs = std::filesystem;
    error.clear();

    const fs::path target{path};
    const fs::path temp = target.string() + ".tmp";
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "创建注视标定目录失败：" + ec.message();
            return false;
        }
    }

    config::IniWriter writer;
    if (!writer.open(temp)) {
        error = "写不了注视标定临时文件：" + temp.string();
        return false;
    }
    writer.comment("注视标定档案（ADR-0042）。换头显或重新佩戴后必须重标。");
    writer.kv("version", static_cast<double>(profile.version), 0);
    writer.kv("source_kind", gaze_source_kind_name(profile.source_kind));
    writer.kv("created_utc", profile.created_utc);
    writer.kv("hmd_identity", profile.hmd_identity);
    writer.kv("residual_p95", static_cast<double>(profile.residual_p95), 5);
    if (profile.have_bounds) {
        char bounds[160];
        std::snprintf(bounds, sizeof(bounds), "%.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                      static_cast<double>(profile.input_bounds[0][0]),
                      static_cast<double>(profile.input_bounds[0][1]),
                      static_cast<double>(profile.input_bounds[0][2]),
                      static_cast<double>(profile.input_bounds[0][3]),
                      static_cast<double>(profile.input_bounds[1][0]),
                      static_cast<double>(profile.input_bounds[1][1]),
                      static_cast<double>(profile.input_bounds[1][2]),
                      static_cast<double>(profile.input_bounds[1][3]));
        writer.kv("input_bounds", std::string_view{bounds});
    }
    if (profile.have_trim) {
        // 仿射是建在这四个参数之上的：写下来才能发现"标定完又动了粗调"
        char trim[96];
        std::snprintf(trim, sizeof(trim), "%d %.4f %.4f %.4f %.4f", profile.trim.active ? 1 : 0,
                      static_cast<double>(profile.trim.gain_x),
                      static_cast<double>(profile.trim.gain_y),
                      static_cast<double>(profile.trim.center_x),
                      static_cast<double>(profile.trim.center_y));
        writer.comment("标定时生效的角度粗调（calibration.ini [eye]）：改动后必须重标");
        writer.kv("eye_trim", std::string_view{trim});
    }

    for (int i = 0; i < 2; ++i) {
        const GazeEyeMap& eye = profile.eyes[static_cast<std::size_t>(i)];
        writer.section(i == 0 ? "left" : "right");
        writer.kv_bool("active", eye.active);
        char line[160];
        std::snprintf(line, sizeof(line), "%.6f %.6f %.6f %.6f %.6f %.6f",
                      static_cast<double>(eye.a), static_cast<double>(eye.b),
                      static_cast<double>(eye.c), static_cast<double>(eye.d),
                      static_cast<double>(eye.e), static_cast<double>(eye.f));
        writer.kv("map", std::string_view{line});
    }
    writer.close();

    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        error = "替换注视标定档案失败";
        return false;
    }
    return true;
}

} // namespace fgb::params
