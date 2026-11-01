#include "sinks/vrss_gaze_sink.hpp"

#include "base/log.hpp"
#include "vrss/paths.hpp"

#include <cmath>
#include <span>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fgb {
namespace {

constexpr std::uint64_t kFreshLimitNs = 50'000'000; // 50ms：超过就不该再当"当前注视"

std::uint64_t eye_timestamp(std::uint64_t eye_ts, const GazeState& gaze) noexcept {
    return eye_ts != 0 ? eye_ts : gaze.capture_ts_ns;
}

bool fresh(std::uint64_t ts, std::uint64_t now_ns) noexcept {
    if (ts == 0) {
        return true; // 来源没有时间戳，不能因此丢数据
    }
    if (ts > now_ns) {
        return true; // 轻微时钟偏差按有效处理
    }
    return now_ns - ts <= kFreshLimitNs;
}

std::uint64_t qpc_now(std::uint64_t fallback_ns) noexcept {
#ifdef _WIN32
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter)) {
        return static_cast<std::uint64_t>(counter.QuadPart);
    }
#endif
    return fallback_ns;
}

float clamp_ndc(float v) noexcept {
    if (!std::isfinite(v)) {
        return 0.0f;
    }
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace

const char* vrss_no_data_code(VrssNoData reason) noexcept {
    switch (reason) {
    case VrssNoData::None: return "ok";
    case VrssNoData::Disabled: return "disabled";
    case VrssNoData::NoGaze: return "waiting-gaze";
    case VrssNoData::StaleGaze: return "stale-gaze";
    case VrssNoData::SourceSpace: return "source-space";
    case VrssNoData::NoGeometry: return "no-geometry";
    }
    return "waiting-gaze";
}

const char* vrss_no_data_message(VrssNoData reason) noexcept {
    switch (reason) {
    case VrssNoData::None: return "注视包正在发送";
    case VrssNoData::Disabled: return "VRSS 注视输出已关闭";
    case VrssNoData::NoGaze: return "注视源当前没有数据";
    case VrssNoData::StaleGaze: return "注视数据已过期（超过 50 ms）";
    case VrssNoData::SourceSpace:
        return "注视源只给原始的逐眼 2D 点，没有声明视口坐标（ADR-0033），"
               "FGB 不会把它当屏幕位置发给 VRSS；可在设置里打开「使用未标定的原始 2D 点」";
    case VrssNoData::NoGeometry:
        return "只有方向型注视，但没有可用的头显几何（geometry.txt）";
    }
    return "注视源当前没有数据";
}

VrssGazeSink::VrssGazeSink(VrssSinkConfig config) : config_(std::move(config)) {}

std::string VrssGazeSink::target() const {
    return config_.host + ":" + std::to_string(config_.port);
}

bool VrssGazeSink::start() {
    if (config_.port == 0 || config_.host.empty()) {
        FGB_ERROR("vrss", "VRSS 输出没有目标地址");
        return false;
    }
    if (config_.rate_hz < 1 || config_.rate_hz > 120) {
        FGB_ERROR("vrss", "VRSS 帧率必须在 1..120 Hz，当前 %d", config_.rate_hz);
        return false;
    }
    if (!(std::isfinite(config_.x_gain) && std::isfinite(config_.y_gain) &&
          config_.x_gain >= 0.1f && config_.x_gain <= 2.0f &&
          config_.y_gain >= 0.1f && config_.y_gain <= 2.0f) ||
        !(std::isfinite(config_.x_offset) && std::isfinite(config_.y_offset)) ||
        std::abs(config_.x_offset) > 1.0f || std::abs(config_.y_offset) > 1.0f ||
        !std::isfinite(config_.stereo_offset) || std::abs(config_.stereo_offset) > 0.5f) {
        FGB_ERROR("vrss", "VRSS 校准参数越界");
        return false;
    }
    if (!socket_.open_sender()) {
        FGB_ERROR("vrss", "无法创建 VRSS UDP socket");
        return false;
    }
    interval_ns_ = 1'000'000'000ull / static_cast<std::uint64_t>(config_.rate_hz);
    sequence_    = 0;
    throttle_.reset();
    started_ = true;
    FGB_INFO("vrss", "VRSS 注视输出 → %s（%d Hz，%s，%s）", target().c_str(), config_.rate_hz,
             config_.geometry.valid() ? "2D 点 + 几何方向" : "仅 2D 点",
             config_.enabled ? "已启用" : "未启用");
    return true;
}

void VrssGazeSink::set_config(const VrssSinkConfig& config) noexcept {
    config_ = config;
    if (config_.rate_hz >= 1 && config_.rate_hz <= 120) {
        interval_ns_ = 1'000'000'000ull / static_cast<std::uint64_t>(config_.rate_hz);
    }
}

void VrssGazeSink::stop() noexcept {
    started_ = false;
    socket_.close();
}

void VrssGazeSink::point_to_ndc(float px, float py, float& x, float& y) noexcept {
    x = 2.0f * px - 1.0f;
    // 视口 [0,1] 是左上原点、+y 向下；NVIDIA 的 NDC 是中心 0,0、+Y 向上。
    y = 1.0f - 2.0f * py;
}

void VrssGazeSink::source_point_to_ndc(float px, float py, float& x, float& y) noexcept {
    x = 2.0f * px - 1.0f;
    // "来源空间"的原始点 y 原点是**下方**（实测：抬头时数值变小，而视口坐标应变大），
    // 和 ADR-0033 的视口约定相反，所以这里不再取 1-2y。
    y = 2.0f * py - 1.0f;
}

bool VrssGazeSink::direction_to_ndc(Vec3 dir_local, const vrss::EyeGeometry& eye, float& x,
                                    float& y) noexcept {
    const float len = length(dir_local);
    if (!std::isfinite(len) || len <= 1e-6f || !eye.valid()) {
        return false;
    }
    const Vec3 dir = dir_local * (1.0f / len);
    const float* r = eye.eye_to_head.data();
    // v = R^T * dir：把 HMD 局部方向转到眼科坐标。
    const Vec3 v{r[0] * dir.x + r[3] * dir.y + r[6] * dir.z,
                 r[1] * dir.x + r[4] * dir.y + r[7] * dir.z,
                 r[2] * dir.x + r[5] * dir.y + r[8] * dir.z};
    if (!(v.z < -1e-6f)) {
        return false; // 视线不在眼前方
    }
    const float* p = eye.projection.data();
    const float nx = v.x / -v.z;
    const float ny = v.y / -v.z;
    x = 2.0f * (nx - p[0]) / (p[1] - p[0]) - 1.0f;
    y = 2.0f * ny / (p[3] - p[2]) + (p[2] + p[3]) / (p[3] - p[2]);
    return std::isfinite(x) && std::isfinite(y);
}

bool VrssGazeSink::build_ndc(const GazeState& gaze, std::uint64_t now_ns,
                             const VrssSinkConfig& config, std::array<float, 4>& ndc) noexcept {
    float left_x = 0.0f, left_y = 0.0f, right_x = 0.0f, right_y = 0.0f;
    bool left_ok = false, right_ok = false;

    const std::uint64_t left_ts  = eye_timestamp(gaze.left_capture_ts_ns, gaze);
    const std::uint64_t right_ts = eye_timestamp(gaze.right_capture_ts_ns, gaze);

    // 2D 视口点优先：它是 VRSS 最直接的输入，不需要 FOV/IPD 换算。
    if (gaze.point_viewport && gaze.left_point_valid && fresh(left_ts, now_ns)) {
        point_to_ndc(gaze.left_point_2d.x, gaze.left_point_2d.y, left_x, left_y);
        left_ok = true;
    } else if (gaze.left_valid && fresh(left_ts, now_ns) && config.geometry.eyes[0].valid()) {
        left_ok = direction_to_ndc(gaze.left_dir_local, config.geometry.eyes[0], left_x, left_y);
    }
    if (gaze.point_viewport && gaze.right_point_valid && fresh(right_ts, now_ns)) {
        point_to_ndc(gaze.right_point_2d.x, gaze.right_point_2d.y, right_x, right_y);
        right_ok = true;
    } else if (gaze.right_valid && fresh(right_ts, now_ns) && config.geometry.eyes[1].valid()) {
        right_ok = direction_to_ndc(gaze.right_dir_local, config.geometry.eyes[1], right_x, right_y);
    }

    // provider 只接受 validMask=3：单眼失效时镜像另一眼并短暂保持，不发半有效包。
    if (!left_ok && !right_ok && config.allow_source_points && !gaze.point_viewport &&
        fresh(left_ts, now_ns) && fresh(right_ts, now_ns)) {
        // 未标定的原始点：两眼差的是水平视差（实测 dy 恒为 0、dx 0.10..0.30），
        // 真正注视的是中点；两眼各发自己的值反而会把高质量区按视差推开。
        if (gaze.left_point_valid && gaze.right_point_valid) {
            source_point_to_ndc(0.5f * (gaze.left_point_2d.x + gaze.right_point_2d.x),
                                0.5f * (gaze.left_point_2d.y + gaze.right_point_2d.y), left_x,
                                left_y);
            right_x = left_x;
            right_y = left_y;
            left_ok = right_ok = true;
        } else if (gaze.left_point_valid) {
            source_point_to_ndc(gaze.left_point_2d.x, gaze.left_point_2d.y, left_x, left_y);
            left_ok = true;
        } else if (gaze.right_point_valid) {
            source_point_to_ndc(gaze.right_point_2d.x, gaze.right_point_2d.y, right_x, right_y);
            right_ok = true;
        }
    }
    if (left_ok && !right_ok) {
        right_x = left_x;
        right_y = left_y;
    } else if (!left_ok && right_ok) {
        left_x = right_x;
        left_y = right_y;
    } else if (!left_ok && !right_ok) {
        return false;
    }

    if (config.flip_y) {
        left_y  = -left_y;
        right_y = -right_y;
    }
    // 已经过注视标定（space == calibrated）的位置不再套 gain/offset：
    // 那几条滑块是"标定之前"临时用的，两套一起用等于校准两次，位置会整体偏掉
    // （ADR-0042 §6：sink 不得自行校准）。stereo_offset 是显示侧补偿，仍然生效。
    const bool manual_adjust = gaze.space != GazeSpace::calibrated;
    const float x_gain      = manual_adjust ? config.x_gain : 1.0f;
    const float y_gain      = manual_adjust ? config.y_gain : 1.0f;
    const float x_offset    = manual_adjust ? config.x_offset : 0.0f;
    const float y_offset    = manual_adjust ? config.y_offset : 0.0f;

    const float left_ndc_x  = left_x * x_gain + x_offset + config.stereo_offset;
    const float right_ndc_x = right_x * x_gain + x_offset - config.stereo_offset;
    ndc[0] = clamp_ndc(left_ndc_x);
    ndc[1] = clamp_ndc(left_y * y_gain + y_offset);
    ndc[2] = clamp_ndc(right_ndc_x);
    ndc[3] = clamp_ndc(right_y * y_gain + y_offset);
    return true;
}

VrssSinkStatus VrssGazeSink::status() const noexcept {
    VrssSinkStatus out = status_;
    out.enabled    = config_.enabled;
    out.messages   = stats_.messages;
    out.errors     = stats_.errors;
    out.suppressed = stats_.suppressed;
    return out;
}

VrssNoData VrssGazeSink::diagnose(const GazeState& gaze, const VrssSinkConfig& config,
                                  std::uint64_t now_ns) noexcept {
    const std::uint64_t left_ts  = eye_timestamp(gaze.left_capture_ts_ns, gaze);
    const std::uint64_t right_ts = eye_timestamp(gaze.right_capture_ts_ns, gaze);

    const bool has_point = gaze.left_point_valid || gaze.right_point_valid || gaze.point2d_valid;
    const bool has_dir   = gaze.left_valid || gaze.right_valid;
    if (!has_point && !has_dir) {
        return VrssNoData::NoGaze;
    }
    // 有点但没声明视口空间：这就是 ADR-0033 里说的"来源原始坐标"，
    // 两眼 x 能差 0.3，当屏幕位置用会让高质量区整体偏掉。
    if (!gaze.point_viewport && !has_dir && !config.allow_source_points) {
        return VrssNoData::SourceSpace;
    }
    if (!fresh(left_ts, now_ns) || !fresh(right_ts, now_ns)) {
        return VrssNoData::StaleGaze;
    }
    if (has_dir && !config.geometry.valid()) {
        return VrssNoData::NoGeometry;
    }
    return VrssNoData::NoGaze;
}

void VrssGazeSink::submit(const FaceFrame& frame, const GazeState& gaze,
                          std::uint64_t now_ns) {
    (void)frame;
    if (!started_ || !config_.enabled) {
        status_.reason = VrssNoData::Disabled;
        return;
    }
    if (!throttle_.due(now_ns, interval_ns_)) {
        // 节流不改 reason：刚发过，状态应该是 ok，不是"没数据"。
        ++stats_.suppressed;
        return;
    }
    // 方向型来源要先有几何。引擎启动时几何可能还没导出（用户刚点一键启用），
    // 这里最多每 5 秒补读一次；2D 点路径完全不依赖它。
    if (!config_.geometry.valid() &&
        (next_geometry_try_ns_ == 0 || now_ns >= next_geometry_try_ns_)) {
        next_geometry_try_ns_ = now_ns + 5'000'000'000ull;
        std::string error;
        if (vrss::load_geometry(vrss::run_dir() / "geometry.txt", config_.geometry, error)) {
            FGB_INFO("vrss", "已补读 HMD 几何，方向型注视可用");
        }
    }

    std::array<float, 4> ndc{};
    if (!build_ndc(gaze, now_ns, config_, ndc)) {
        ++status_.no_data;
        status_.reason = diagnose(gaze, config_, now_ns);
        return;
    }

    VrssGazePacketV1 packet{};
    packet.sequence = ++sequence_;
    packet.qpc      = qpc_now(now_ns);
    packet.left_x   = ndc[0];
    packet.left_y   = ndc[1];
    packet.right_x  = ndc[2];
    packet.right_y  = ndc[3];

    const auto bytes = std::as_bytes(std::span<const VrssGazePacketV1>{&packet, 1});
    if (socket_.send_to(bytes, config_.host, config_.port)) {
        ++stats_.messages;
        stats_.bytes += bytes.size();
        status_.reason  = VrssNoData::None;
        status_.ndc     = ndc;
        status_.ndc_ts_ns = now_ns;
    } else {
        ++stats_.errors;
        status_.reason = VrssNoData::None; // 发不出去是另一类故障，靠 errors 计数看
    }
}

} // namespace fgb
