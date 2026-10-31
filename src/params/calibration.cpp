#include "params/calibration.hpp"

#include "base/log.hpp"
#include "capture/capture_file.hpp"
#include "config/ini.hpp"
#include "core/model/math.hpp"
#include "vrcft/update_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fgb::params {
namespace {

constexpr char kTag[] = "calib";

// 曲线 id 的字符串形式：档案要能手改，"custom:1" 比数字 9 好读得多。
// 认不出来的一律退回线性——手改档案写错一个字母，不该让整个校准失效。
constexpr const char* kCurveNames[] = {"linear", "ease_in", "ease_out", "smooth", "step"};
static_assert(sizeof(kCurveNames) / sizeof(kCurveNames[0]) ==
                  static_cast<std::size_t>(CurveKind::count),
              "曲线名字表与 CurveKind 必须一一对应");

// 按空白切 token：形状行现在是 `lo hi curve [param]`，旧档案只有前两段
std::vector<std::string_view> split_tokens(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t') {
            ++i;
        }
        if (i > start) {
            out.push_back(text.substr(start, i - start));
        }
    }
    return out;
}

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

// 曲线 id 的字符串形式：档案要能手改，"custom:1" 比数字 9 好读得多。
// 认不出来的一律退回线性——手改档案写错一个字母，不该让整个校准失效。
std::string curve_to_text(std::uint8_t curve) {
    if (curve >= kCurveCustomBase) {
        return "custom:" + std::to_string(curve - kCurveCustomBase);
    }
    if (curve < static_cast<std::uint8_t>(CurveKind::count)) {
        return kCurveNames[curve];
    }
    return "linear";
}

std::uint8_t curve_from_text(std::string_view text) {
    if (text.rfind("custom:", 0) == 0) {
        const int slot = std::atoi(std::string{text.substr(7)}.c_str());
        if (slot >= 0 && slot < static_cast<int>(kMaxCustomCurves)) {
            return static_cast<std::uint8_t>(kCurveCustomBase + slot);
        }
        return 0;
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(CurveKind::count); ++i) {
        if (text == kCurveNames[i]) {
            return static_cast<std::uint8_t>(i);
        }
    }
    return 0;
}

namespace {

// 单调三次 Hermite（Fritsch–Carlson）的切线。
//
// 面捕的输出必须连续：折线在控制点处的折角放大到脸上就是"一顿一顿"；而普通三次样条
// 会过冲，输出可能冲出相邻控制点的范围甚至局部反向，表现为表情抽搐。这个算法两样都避开，
// 代价只是每帧多算几十次乘加（控制点 ≤8）。
void curve_tangents(const CurveShape& curve, float* slope, float* tangent) noexcept {
    const int n = static_cast<int>(curve.count);
    for (int i = 0; i < n - 1; ++i) {
        const float dx = curve.points[i + 1].x - curve.points[i].x;
        slope[i] = dx > 1e-6f ? (curve.points[i + 1].y - curve.points[i].y) / dx : 0.0f;
    }
    if (n == 2) {
        tangent[0] = slope[0];
        tangent[1] = slope[0];
        return;
    }
    tangent[0]         = slope[0];
    tangent[n - 1]     = slope[n - 2];
    for (int i = 1; i < n - 1; ++i) {
        tangent[i] = 0.5f * (slope[i - 1] + slope[i]);
    }
    // 保形修正：切线不能超过相邻割线的 3 倍，否则这一段会鼓出去（过冲）
    for (int i = 0; i < n - 1; ++i) {
        if (std::fabs(slope[i]) < 1e-6f) {
            tangent[i]     = 0.0f;
            tangent[i + 1] = 0.0f;
            continue;
        }
        const float a = tangent[i] / slope[i];
        const float b = tangent[i + 1] / slope[i];
        const float s = a * a + b * b;
        if (s > 9.0f) {
            const float tau = 3.0f / std::sqrt(s);
            tangent[i]      = tau * a * slope[i];
            tangent[i + 1]  = tau * b * slope[i];
        }
    }
}

// 一段 Hermite：u ∈ [0,1]，dx 是这一段的 x 跨度
float hermite_at(float y0, float y1, float m0, float m1, float dx, float u) noexcept {
    const float u2 = u * u;
    const float u3 = u2 * u;
    return (2.0f * u3 - 3.0f * u2 + 1.0f) * y0 + (u3 - 2.0f * u2 + u) * dx * m0 +
           (-2.0f * u3 + 3.0f * u2) * y1 + (u3 - u2) * dx * m1;
}

} // namespace

// 阶跃的过渡带宽：纯硬阶跃在 120 Hz 上是一帧从 0 跳到 1，看起来像抽搐。
// 0.02 的窄带只影响阈值附近，用户感知不到"变软"，但输出不会打嗝。
constexpr float kStepBand = 0.02f;

float apply_curve(const CurveShape* custom, std::size_t custom_count, std::uint8_t curve,
                  float param, float t) noexcept {
    t = clamp01(t);
    if (curve >= kCurveCustomBase) {
        const std::size_t slot = static_cast<std::size_t>(curve - kCurveCustomBase);
        if (custom == nullptr || slot >= custom_count) {
            return t; // 指向不存在的槽：按线性走，别把整个形状吞掉
        }
        const CurveShape& shape = custom[slot];
        if (shape.count < 2) {
            return t;
        }
        if (t <= shape.points[0].x) {
            return shape.points[0].y;
        }
        float slope[kMaxCurvePoints];
        float tangent[kMaxCurvePoints];
        curve_tangents(shape, slope, tangent);
        for (std::size_t i = 1; i < shape.count; ++i) {
            if (t <= shape.points[i].x) {
                const float dx = shape.points[i].x - shape.points[i - 1].x;
                if (dx <= 1e-6f) {
                    return shape.points[i].y;
                }
                const float u = (t - shape.points[i - 1].x) / dx;
                return clamp01(hermite_at(shape.points[i - 1].y, shape.points[i].y,
                                          tangent[i - 1], tangent[i], dx, u));
            }
        }
        return shape.points[shape.count - 1].y;
    }

    switch (static_cast<CurveKind>(curve)) {
    case CurveKind::ease_in:
        return t * t;
    case CurveKind::ease_out:
        return t * (2.0f - t);
    case CurveKind::smooth:
        return t * t * (3.0f - 2.0f * t);
    case CurveKind::step: {
        const float threshold = clamp01(param);
        if (t <= threshold - kStepBand) {
            return 0.0f;
        }
        if (t >= threshold + kStepBand) {
            return 1.0f;
        }
        return (t - (threshold - kStepBand)) / (2.0f * kStepBand);
    }
    case CurveKind::linear:
    default:
        return t;
    }
}

void sanitize_curve(CurveShape& curve) noexcept {
    if (curve.count < 2) {
        curve.count    = 2;
        curve.points[0] = CurvePoint{0.0f, 0.0f};
        curve.points[1] = CurvePoint{1.0f, 1.0f};
        return;
    }
    std::size_t n = std::min<std::size_t>(curve.count, kMaxCurvePoints);

    // 先夹 y，再排序：排序之后再锚定首尾，保证端点一定是 (0,0)/(1,1)——
    // 端点自由移动会让"最高只能到 0.8"这种状态出现，而用户没有任何线索能解释它。
    for (std::size_t i = 0; i < n; ++i) {
        curve.points[i].x = clamp01(curve.points[i].x);
        curve.points[i].y = clamp01(curve.points[i].y);
    }
    std::sort(curve.points.begin(), curve.points.begin() + static_cast<std::ptrdiff_t>(n),
              [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    // 相邻 x 至少留 1e-3，否则求值时会出现垂直段和除零
    for (std::size_t i = 1; i < n; ++i) {
        if (curve.points[i].x - curve.points[i - 1].x < 1e-3f) {
            curve.points[i].x = curve.points[i - 1].x + 1e-3f;
        }
    }
    if (curve.points[n - 1].x > 1.0f) {
        // 挤到边界外就整体往回压，别截断成重复点
        const float shift = curve.points[n - 1].x - 1.0f;
        for (std::size_t i = 0; i < n; ++i) {
            curve.points[i].x = clamp01(curve.points[i].x - shift);
        }
    }
    curve.points[0]       = CurvePoint{0.0f, 0.0f};
    curve.points[n - 1]   = CurvePoint{1.0f, 1.0f};
    curve.count           = static_cast<std::uint8_t>(n);
}

std::size_t CalibrationProfile::active_shape_count() const noexcept {
    std::size_t n = 0;
    for (const ShapeRange& r : shapes) {
        if (r.active) {
            ++n;
        }
    }
    return n;
}

bool operator==(const CalibrationProfile& a, const CalibrationProfile& b) noexcept {
    if (a.has_eye != b.has_eye) {
        return false;
    }
    // 眼睑参数与视线无关，单独比：只调它不该被 has_eye 的开关掩盖
    if (a.eyelid_gain != b.eyelid_gain || a.eyelid_lo != b.eyelid_lo) {
        return false;
    }
    if (a.has_eye && (a.eye_gain_x != b.eye_gain_x || a.eye_gain_y != b.eye_gain_y ||
                      a.eye_center_x != b.eye_center_x || a.eye_center_y != b.eye_center_y)) {
        return false;
    }
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const ShapeRange& x = a.shapes[i];
        const ShapeRange& y = b.shapes[i];
        if (x.active != y.active) {
            return false;
        }
        // 未激活的形状数值没有意义，界面也不显示：比较它只会让"改了又改回去"
        // 被误判成未保存（例如拖动滑杆后取消勾选）。
        if (x.active && (x.lo != y.lo || x.hi != y.hi || x.curve != y.curve ||
                         x.curve_param != y.curve_param)) {
            return false;
        }
    }
    // 自定义曲线表也要比：改曲线点不影响任何 ShapeRange 字段，漏了它就会出现
    // "拖完曲线点，界面仍显示已保存"。
    for (std::size_t i = 0; i < kMaxCustomCurves; ++i) {
        const CurveShape& x = a.custom_curves[i];
        const CurveShape& y = b.custom_curves[i];
        if (x.count != y.count) {
            return false;
        }
        for (std::size_t p = 0; p < x.count; ++p) {
            if (x.points[p].x != y.points[p].x || x.points[p].y != y.points[p].y) {
                return false;
            }
        }
    }
    return true;
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
        if (hi - lo < kMinDeriveSpan) {
            continue; // 这个形状两次录制没差别，保持原样
        }
        ShapeRange range;
        range.lo     = lo;
        range.hi     = hi;
        range.active = true;
        // 推导只给出端点；曲线留给用户在界面上按手感挑，因为"中段怎么分配"是主观的，
        // 抓包数据里没有这个信息
        range.curve  = static_cast<std::uint8_t>(CurveKind::linear);
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
    writer.comment("第三个字段是映射曲线：linear / ease_in / ease_out / smooth / step / custom:N");
    writer.comment("step 的阈值写在第四个字段；自定义曲线的控制点在 [curves] 段");

    writer.section("eye");
    writer.kv_bool("active", profile.has_eye);
    writer.kv("gain_x", static_cast<double>(profile.eye_gain_x), 4);
    writer.kv("gain_y", static_cast<double>(profile.eye_gain_y), 4);
    writer.kv("center_x", static_cast<double>(profile.eye_center_x), 4);
    writer.kv("center_y", static_cast<double>(profile.eye_center_y), 4);
    // 与视线各存各的：只调眼睑不该被迫打开视线校准
    writer.kv("eyelid_gain", static_cast<double>(profile.eyelid_gain), 4);
    writer.kv("eyelid_lo", static_cast<double>(profile.eyelid_lo), 4);

    writer.section("shapes");
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const ShapeRange& range = profile.shapes[i];
        if (!range.active) {
            continue;
        }
        char line[96];
        if (range.curve == static_cast<std::uint8_t>(CurveKind::step)) {
            std::snprintf(line, sizeof(line), "%.4f %.4f %s %.4f", static_cast<double>(range.lo),
                          static_cast<double>(range.hi), curve_to_text(range.curve).c_str(),
                          static_cast<double>(range.curve_param));
        } else {
            std::snprintf(line, sizeof(line), "%.4f %.4f %s", static_cast<double>(range.lo),
                          static_cast<double>(range.hi), curve_to_text(range.curve).c_str());
        }
        writer.kv(shape_name(static_cast<Shape>(i)),
                  std::string_view{line, std::strlen(line)});
    }

    // 只写被引用的槽：没人用的空槽写进去，用户手改档案时反而会以为它生效了
    writer.section("curves");
    for (std::size_t slot = 0; slot < kMaxCustomCurves; ++slot) {
        const CurveShape& curve = profile.custom_curves[slot];
        if (curve.count < 2) {
            continue;
        }
        std::string line;
        for (std::size_t p = 0; p < curve.count; ++p) {
            char point[32];
            std::snprintf(point, sizeof(point), "%s%.3f,%.3f", p == 0 ? "" : " ",
                          static_cast<double>(curve.points[p].x),
                          static_cast<double>(curve.points[p].y));
            line += point;
        }
        writer.kv(std::to_string(slot), std::string_view{line});
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
            } else if (entry.key == "eyelid_gain") {
                // 夹到界面能拖的范围：手改档案写成 10 只会让所有眼睛永远闭合
                profile.eyelid_gain = std::clamp(config::parse_float(entry.value, 1.0f), 0.2f, 4.0f);
            } else if (entry.key == "eyelid_lo") {
                // 基线超过 0.5 就等于"眼睛大部分时间都是闭的"，一定是填错了
                profile.eyelid_lo = std::clamp(config::parse_float(entry.value, 0.0f), 0.0f, 0.5f);
            }
            saw_any = true;
            return;
        }
        if (entry.section != "shapes") {
            if (entry.section == "curves") {
                // 槽号就是键；对不上就跳过（旧档案没有这一段，走默认）
                const int slot = std::atoi(entry.key.c_str());
                if (slot < 0 || slot >= static_cast<int>(kMaxCustomCurves)) {
                    return;
                }
                CurveShape curve;
                for (const std::string_view token : split_tokens(entry.value)) {
                    if (curve.count >= kMaxCurvePoints) {
                        break;
                    }
                    const std::size_t comma = token.find(',');
                    if (comma == std::string_view::npos) {
                        continue;
                    }
                    CurvePoint point;
                    point.x = config::parse_float(token.substr(0, comma), 0.0f);
                    point.y = config::parse_float(token.substr(comma + 1), 0.0f);
                    curve.points[curve.count++] = point;
                }
                sanitize_curve(curve);
                profile.custom_curves[static_cast<std::size_t>(slot)] = curve;
            }
            return;
        }
        Shape shape{};
        if (!shape_from_name(entry.key, shape)) {
            return; // 名字对不上就跳过：可能是旧版本档案
        }
        const std::vector<std::string_view> tokens = split_tokens(entry.value);
        if (tokens.size() < 2) {
            return;
        }
        ShapeRange range;
        range.lo = config::parse_float(tokens[0], 0.0f);
        range.hi = config::parse_float(tokens[1], 1.0f);
        // 旧档案只有两段：第三段缺省就是线性，天然向后兼容
        if (tokens.size() >= 3) {
            range.curve = curve_from_text(tokens[2]);
        }
        if (tokens.size() >= 4) {
            range.curve_param = config::parse_float(tokens[3], 0.5f);
        }
        if (range.lo > range.hi) {
            std::swap(range.lo, range.hi);
        }
        range.active = range.hi - range.lo >= kMinSpan;
        profile.shapes[index_of(shape)] = range;
        saw_any = true;
    });
    return ok && saw_any;
}

std::filesystem::path default_calibration_path() {
#ifdef _WIN32
    // 用宽字符环境变量：中文用户名下窄字符路径会变成乱码，档案就写到别处去了
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local != nullptr && local[0] != L'\0') {
        return std::filesystem::path{local} / L"FaceGazeBridge" / L"calibration.ini";
    }
#endif
    return std::filesystem::path{"calibration.ini"};
}

bool save_profile_atomic(const std::string& path, const CalibrationProfile& profile,
                         std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;

    const fs::path target{path};
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec); // 目录不在就建，不能因为目录缺失保存失败
        if (ec) {
            error = "创建目录失败：" + target.parent_path().string();
            return false;
        }
    }

    fs::path temp = target;
    temp += ".tmp";
    if (!save_profile(temp.string(), profile)) {
        error = "写入临时档案失败：" + temp.string();
        return false;
    }

    // 旧档案留一份 .bak：用户拖错了一条 lo/hi 之后，这是唯一能拿回来的东西
    if (fs::exists(target, ec)) {
        fs::path backup = target;
        backup += ".bak";
        fs::copy_file(target, backup, fs::copy_options::overwrite_existing, ec);
        ec.clear(); // 备份失败不该挡住保存：正式档案才是本体
    }

#ifdef _WIN32
    if (::MoveFileExW(temp.wstring().c_str(), target.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "替换档案失败（目标文件是否只读/被占用）：" + path;
        return false;
    }
#else
    fs::rename(temp, target, ec);
    if (ec) {
        error = "替换档案失败：" + path;
        return false;
    }
#endif
    return true;
}

CalibrationApplier::CalibrationApplier(CalibrationProfile profile) {
    profile_snapshot_.publish(profile);
    engine_copy_ = std::move(profile);
}

void CalibrationApplier::set_profile(const CalibrationProfile& profile) noexcept {
    // 写者只有一个（控制面线程），读者只有引擎线程：seqlock 的前提成立
    profile_snapshot_.publish(profile);
}

CalibrationProfile CalibrationApplier::profile() const noexcept {
    CalibrationProfile out;
    if (!profile_snapshot_.read(out)) {
        // 极端竞争下返回全透传：界面读到的"没在改"比读到半截档案安全
        return CalibrationProfile{};
    }
    return out;
}

void CalibrationApplier::process(FaceFrame& frame) noexcept {
    // 快照读失败说明控制线程正在替换档案（一次写入只有几百纳秒）。
    // 沿用上一帧的拷贝比"这一帧跳过校准"平滑得多：跳过会让输出突跳一下。
    CalibrationProfile fresh;
    if (profile_snapshot_.read(fresh)) {
        engine_copy_ = fresh;
    }
    const CalibrationProfile& profile = engine_copy_;

    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const ShapeRange& range = profile.shapes[i];
        if (!range.active) {
            continue;
        }
        const float span = range.hi - range.lo;
        if (span < 1e-4f) {
            continue;
        }
        const float t = clamp01((frame.shapes[i] - range.lo) / span);
        frame.shapes[i] = apply_curve(profile.custom_curves.data(), profile.custom_curves.size(),
                                      range.curve, range.curve_param, t);
    }

    // 眼睑开合：先扣掉模块在闭眼时报的底噪，按剩余范围归一化，再放大。
    // 只减不归一化的话，lo=0.1 时增益要从 1.6 起算，用户没法把"1.4"和观感对上；
    // 而归一化之后 gain=1.0 仍然等于"原样透传"，参数语义保持一致。
    // openness 不属于 88 个形状，所以这条不受 range.active 影响。
    if (profile.eyelid_gain != 1.0f || profile.eyelid_lo > 0.0f) {
        const float lo   = std::clamp(profile.eyelid_lo, 0.0f, 0.5f);
        const float span = 1.0f - lo;
        for (EyeState* eye : {&frame.eyes.left, &frame.eyes.right}) {
            const float base =
                span > 1e-3f ? (eye->openness - lo) / span : eye->openness - lo;
            eye->openness = clamp01(base * profile.eyelid_gain);
        }
    }

    if (profile.has_eye) {
        // 注意这里单位是"度"，不能套 clamp_sym（那是给 ±1 归一化值用的）
        for (EyeState* eye : {&frame.eyes.left, &frame.eyes.right}) {
            const float yaw   = (eye->gazePitchYawDeg.y - profile.eye_center_x) * profile.eye_gain_x;
            const float pitch = (eye->gazePitchYawDeg.x - profile.eye_center_y) * profile.eye_gain_y;
            eye->gazePitchYawDeg.y = std::clamp(yaw, -60.0f, 60.0f);
            eye->gazePitchYawDeg.x = std::clamp(pitch, -60.0f, 60.0f);
        }
    }
}

} // namespace fgb::params
