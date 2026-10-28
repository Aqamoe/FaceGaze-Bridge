#include "params/calibration.hpp"

#include "base/log.hpp"
#include "capture/capture_file.hpp"
#include "config/ini.hpp"
#include "core/model/math.hpp"
#include "vrcft/update_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fgb::params {
namespace {

constexpr char kTag[] = "calib";

// hi - lo 小于这个值说明这个形状在这两次录制里几乎没动，不该拿它做映射
constexpr float kMinSpan = 0.05f;

float percentile(std::vector<float>& values, float p) {
    if (values.empty()) {
        return 0.0f;
    }
    const std::size_t idx = static_cast<std::size_t>(p * static_cast<float>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(idx), values.end());
    return values[idx];
}

// 采集一份抓包里每个形状的取值与视线角度
struct Samples {
    std::array<std::vector<float>, kShapeCount> shapes;
    std::vector<float> gaze_x;
    std::vector<float> gaze_y;
    std::size_t frames = 0;
};

bool collect(const std::string& path, Samples& out, std::string& error) {
    capture::Reader reader;
    if (!reader.open(path)) {
        error = "打开失败：" + path + "（" + reader.error() + "）";
        return false;
    }

    FaceFrame frame;
    while (reader.next()) {
        vrcft::UpdateData data{};
        if (!vrcft::decode_update(
                std::span<const std::byte>{reader.payload(), reader.payload_size()}, data)) {
            continue;
        }
        if (!data.has_any_valid()) {
            continue;
        }
        const vrcft::ConvertOptions options;
        vrcft::apply_to_frame(data, options, frame);

        for (std::size_t i = 0; i < kShapeCount; ++i) {
            out.shapes[i].push_back(frame.shapes[i]);
        }
        out.gaze_x.push_back(0.5f * (frame.eyes.left.gazePitchYawDeg.y +
                                    frame.eyes.right.gazePitchYawDeg.y));
        out.gaze_y.push_back(0.5f * (frame.eyes.left.gazePitchYawDeg.x +
                                    frame.eyes.right.gazePitchYawDeg.x));
        ++out.frames;
    }

    if (out.frames == 0) {
        error = "抓包里没有有效帧：" + path;
        return false;
    }
    return true;
}

} // namespace

std::size_t CalibrationProfile::active_shape_count() const noexcept {
    std::size_t n = 0;
    for (const ShapeRange& r : shapes) {
        if (r.active) {
            ++n;
        }
    }
    return n;
}

bool derive_profile(const std::string& neutral_capture, const std::string& express_capture,
                    CalibrationProfile& out, std::string& error) {
    Samples neutral;
    Samples express;
    if (!collect(neutral_capture, neutral, error)) {
        return false;
    }
    if (!collect(express_capture, express, error)) {
        return false;
    }

    out = CalibrationProfile{};
    std::size_t active = 0;

    for (std::size_t i = 0; i < kShapeCount; ++i) {
        auto& neutral_values = neutral.shapes[i];
        auto& express_values = express.shapes[i];
        if (neutral_values.empty() || express_values.empty()) {
            continue;
        }

        const float lo = clamp01(percentile(neutral_values, 0.5f));
        const float hi = clamp01(percentile(express_values, 0.95f));
        if (hi - lo < kMinSpan) {
            continue; // 这个形状两次录制没差别，保持原样
        }
        ShapeRange range;
        range.lo     = lo;
        range.hi     = hi;
        range.active = true;
        out.shapes[i] = range;
        ++active;
    }

    if (!neutral.gaze_x.empty()) {
        double sum_x = 0.0;
        double sum_y = 0.0;
        for (std::size_t i = 0; i < neutral.gaze_x.size(); ++i) {
            sum_x += neutral.gaze_x[i];
            sum_y += neutral.gaze_y[i];
        }
        out.eye_center_x = static_cast<float>(sum_x / static_cast<double>(neutral.gaze_x.size()));
        out.eye_center_y = static_cast<float>(sum_y / static_cast<double>(neutral.gaze_y.size()));
        out.has_eye      = true;
    }

    FGB_INFO(kTag, "推导完成：%zu / %zu 个形状参与校准（中性 %zu 帧，夸张 %zu 帧）", active,
             kShapeCount, neutral.frames, express.frames);
    return true;
}

bool save_profile(const std::string& path, const CalibrationProfile& profile) {
    config::IniWriter writer;
    if (!writer.open(path)) {
        return false;
    }
    writer.comment("FaceGaze Bridge 校准档案");
    writer.comment("lo/hi 是原始数据的映射区间：lo 对应表情为 0，hi 对应表情为 1");

    writer.section("eye");
    writer.kv_bool("active", profile.has_eye);
    writer.kv("gain_x", static_cast<double>(profile.eye_gain_x), 4);
    writer.kv("gain_y", static_cast<double>(profile.eye_gain_y), 4);
    writer.kv("center_x", static_cast<double>(profile.eye_center_x), 4);
    writer.kv("center_y", static_cast<double>(profile.eye_center_y), 4);

    writer.section("shapes");
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const ShapeRange& range = profile.shapes[i];
        if (!range.active) {
            continue;
        }
        char line[64];
        std::snprintf(line, sizeof(line), "%.4f %.4f", static_cast<double>(range.lo),
                      static_cast<double>(range.hi));
        writer.kv(shape_name(static_cast<Shape>(i)),
                  std::string_view{line, std::strlen(line)});
    }

    writer.close();
    return true;
}

bool load_profile(const std::string& path, CalibrationProfile& profile) {
    bool saw_any = false;
    const bool ok = config::ini_load(path, [&](const config::IniEntry& entry) {
        if (entry.section == "eye") {
            if (entry.key == "active") {
                profile.has_eye = config::parse_bool(entry.value, false);
            } else if (entry.key == "gain_x") {
                profile.eye_gain_x = config::parse_float(entry.value, 1.0f);
            } else if (entry.key == "gain_y") {
                profile.eye_gain_y = config::parse_float(entry.value, 1.0f);
            } else if (entry.key == "center_x") {
                profile.eye_center_x = config::parse_float(entry.value, 0.0f);
            } else if (entry.key == "center_y") {
                profile.eye_center_y = config::parse_float(entry.value, 0.0f);
            }
            saw_any = true;
            return;
        }
        if (entry.section != "shapes") {
            return;
        }
        Shape shape{};
        if (!shape_from_name(entry.key, shape)) {
            return; // 名字对不上就跳过：可能是旧版本档案
        }
        const std::size_t space = entry.value.find(' ');
        if (space == std::string::npos) {
            return;
        }
        ShapeRange range;
        range.lo     = config::parse_float(entry.value.substr(0, space), 0.0f);
        range.hi     = config::parse_float(entry.value.substr(space + 1), 1.0f);
        range.active = range.hi - range.lo >= kMinSpan;
        profile.shapes[index_of(shape)] = range;
        saw_any = true;
    });
    return ok && saw_any;
}

CalibrationApplier::CalibrationApplier(CalibrationProfile profile) : profile_(profile) {}

void CalibrationApplier::process(FaceFrame& frame) noexcept {
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const ShapeRange& range = profile_.shapes[i];
        if (!range.active) {
            continue;
        }
        const float span = range.hi - range.lo;
        if (span < 1e-4f) {
            continue;
        }
        frame.shapes[i] = clamp01((frame.shapes[i] - range.lo) / span);
    }

    if (profile_.has_eye) {
        // 注意这里单位是"度"，不能套 clamp_sym（那是给 ±1 归一化值用的）
        for (EyeState* eye : {&frame.eyes.left, &frame.eyes.right}) {
            const float yaw   = (eye->gazePitchYawDeg.y - profile_.eye_center_x) * profile_.eye_gain_x;
            const float pitch = (eye->gazePitchYawDeg.x - profile_.eye_center_y) * profile_.eye_gain_y;
            eye->gazePitchYawDeg.y = std::clamp(yaw, -60.0f, 60.0f);
            eye->gazePitchYawDeg.x = std::clamp(pitch, -60.0f, 60.0f);
        }
    }
}

} // namespace fgb::params
