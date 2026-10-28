#include "sinks/vrchat_sink.hpp"

#include "base/log.hpp"

#include <cmath>
#include <cstring>

namespace fgb {
namespace {

constexpr char kTag[] = "vrchat";

constexpr char kParamPrefix[]    = "/avatar/parameters/FT/v2/";
constexpr std::size_t kParamPrefixLen = sizeof(kParamPrefix) - 1;

constexpr char kAddrEyeLeftX[]  = "/avatar/parameters/FT/v2/EyeLeftX";
constexpr char kAddrEyeLeftY[]  = "/avatar/parameters/FT/v2/EyeLeftY";
constexpr char kAddrEyeRightX[] = "/avatar/parameters/FT/v2/EyeRightX";
constexpr char kAddrEyeRightY[] = "/avatar/parameters/FT/v2/EyeRightY";
constexpr char kAddrEyeLidL[]   = "/avatar/parameters/FT/v2/EyeLidLeft";
constexpr char kAddrEyeLidR[]   = "/avatar/parameters/FT/v2/EyeLidRight";

constexpr char kAddrNativePitchYaw[] = "/tracking/eye/LeftRightPitchYaw";
constexpr char kAddrNativeClosed[]   = "/tracking/eye/EyesClosedAmount";

// 一条 float 参数消息的经验长度：地址 + 对齐 + 类型标签 + 参数 + bundle 长度前缀
constexpr std::size_t kEstimatedMessageSize = 72;

std::string_view make_shape_address(Shape s, char (&buf)[72]) noexcept {
    const std::string_view name = shape_name(s);
    const std::size_t total     = kParamPrefixLen + name.size();
    if (name.empty() || total >= sizeof(buf)) {
        return {};
    }
    std::memcpy(buf, kParamPrefix, kParamPrefixLen);
    std::memcpy(buf + kParamPrefixLen, name.data(), name.size());
    return {buf, total};
}

} // namespace

const char* to_string(EyeOutputMode mode) noexcept {
    switch (mode) {
    case EyeOutputMode::parameters:
        return "parameters";
    case EyeOutputMode::native:
        return "native";
    case EyeOutputMode::both:
        return "both";
    }
    return "parameters";
}

bool parse_eye_output_mode(std::string_view text, EyeOutputMode& out) noexcept {
    if (text == "parameters") {
        out = EyeOutputMode::parameters;
        return true;
    }
    if (text == "native") {
        out = EyeOutputMode::native;
        return true;
    }
    if (text == "both") {
        out = EyeOutputMode::both;
        return true;
    }
    return false;
}

VrchatSink::VrchatSink(VrchatSinkConfig config) : config_(std::move(config)) { reset_dedup(); }

void VrchatSink::reset_dedup() noexcept {
    last_shapes_.fill(0.0f);
    shape_sent_.fill(0);
    last_eye_.fill(0.0f);
    eye_sent_.fill(0);
    for (float& v : last_native_pitch_yaw_) {
        v = 0.0f;
    }
    last_native_closed_   = 0.0f;
    native_sent_          = false;
    last_send_ns_         = 0;
    last_heartbeat_ns_    = 0;
}

bool VrchatSink::start() {
    if (!socket_.open_sender()) {
        FGB_ERROR(kTag, "无法创建 UDP socket");
        return false;
    }
    if (!socket_.set_broadcast(false)) {
        FGB_WARN(kTag, "关闭广播失败（通常无害）");
    }
    reset_dedup();
    started_ = true;
    FGB_INFO(kTag, "输出到 %s:%u，眼追模式 %s", config_.host.c_str(),
             static_cast<unsigned>(config_.port), to_string(config_.eye_mode));
    return true;
}

void VrchatSink::stop() noexcept {
    started_ = false;
    socket_.close();
}

void VrchatSink::compute_eye_params(const EyePair& eyes, float full_scale_deg,
                                    float out[kEyeParamCount]) noexcept {
    const float scale = full_scale_deg > 1e-3f ? full_scale_deg : 30.0f;
    auto norm         = [scale](float deg) { return clamp_sym(deg / scale); };

    out[0] = norm(eyes.left.gazePitchYawDeg.y);  // X ← yaw
    out[1] = norm(eyes.left.gazePitchYawDeg.x);  // Y ← pitch
    out[2] = norm(eyes.right.gazePitchYawDeg.y);
    out[3] = norm(eyes.right.gazePitchYawDeg.x);
    // v2 的 EyeLid 1.0 = 闭眼。注意 v1 是反的，别搞混。
    out[4] = clamp01(1.0f - eyes.left.openness);
    out[5] = clamp01(1.0f - eyes.right.openness);
}

void VrchatSink::flush(osc::BundleWriter& writer) noexcept {
    if (writer.empty()) {
        return;
    }
    const auto data = writer.data();
    if (socket_.send_to(data, config_.host, config_.port)) {
        ++stats_.messages;
        stats_.bytes += data.size();
    } else {
        ++stats_.errors;
    }
    writer.reset();
    writer.begin_bundle();
}

void VrchatSink::emit_eye_parameters(osc::BundleWriter& writer, const FaceFrame& frame,
                                     const GazeState& /*gaze*/, bool force) noexcept {
    float values[kEyeParamCount];
    compute_eye_params(frame.eyes, config_.eye_full_scale_deg, values);

    constexpr const char* kAddresses[kEyeParamCount] = {
        kAddrEyeLeftX, kAddrEyeLeftY, kAddrEyeRightX, kAddrEyeRightY, kAddrEyeLidL, kAddrEyeLidR,
    };

    for (std::size_t i = 0; i < kEyeParamCount; ++i) {
        if (!force && eye_sent_[i] != 0 && std::fabs(values[i] - last_eye_[i]) < config_.gaze_epsilon) {
            ++stats_.suppressed;
            continue;
        }
        if (writer.size() + kEstimatedMessageSize > config_.max_datagram) {
            flush(writer);
        }
        const float one[1] = {values[i]};
        if (writer.write_floats(kAddresses[i], one)) {
            last_eye_[i] = values[i];
            eye_sent_[i] = 1;
        }
    }
}

void VrchatSink::emit_eye_native(osc::BundleWriter& writer, const FaceFrame& frame,
                                 const GazeState& /*gaze*/, bool force) noexcept {
    // VRChat 约定：pitch 正值 = 向下。我们内部正值 = 向上，这里取负。
    const float vals[4] = {
        -frame.eyes.left.gazePitchYawDeg.x,
        frame.eyes.left.gazePitchYawDeg.y,
        -frame.eyes.right.gazePitchYawDeg.x,
        frame.eyes.right.gazePitchYawDeg.y,
    };
    const float closed = clamp01(0.5f * ((1.0f - frame.eyes.left.openness) +
                                         (1.0f - frame.eyes.right.openness)));

    bool changed = !native_sent_ || force;
    for (std::size_t i = 0; i < 4; ++i) {
        if (std::fabs(vals[i] - last_native_pitch_yaw_[i]) > config_.gaze_epsilon) {
            changed = true;
        }
    }
    if (std::fabs(closed - last_native_closed_) > config_.gaze_epsilon) {
        changed = true;
    }
    if (!changed) {
        ++stats_.suppressed;
        return;
    }

    if (writer.size() + kEstimatedMessageSize * 2 > config_.max_datagram) {
        flush(writer);
    }
    if (writer.write_floats(kAddrNativePitchYaw, vals)) {
        const float one[1] = {closed};
        if (writer.write_floats(kAddrNativeClosed, one)) {
            for (std::size_t i = 0; i < 4; ++i) {
                last_native_pitch_yaw_[i] = vals[i];
            }
            last_native_closed_ = closed;
            native_sent_        = true;
        }
    }
}

void VrchatSink::emit_shapes(osc::BundleWriter& writer, const FaceFrame& frame, bool force) noexcept {
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        const float v = frame.shapes[i];
        if (!force && shape_sent_[i] != 0 && std::fabs(v - last_shapes_[i]) < config_.shape_epsilon) {
            ++stats_.suppressed;
            continue;
        }

        char addr_buf[72];
        const std::string_view addr = make_shape_address(static_cast<Shape>(i), addr_buf);
        if (addr.empty()) {
            continue;
        }

        if (writer.size() + kEstimatedMessageSize > config_.max_datagram) {
            flush(writer);
        }

        const float one[1] = {v};
        if (writer.write_floats(addr, one)) {
            last_shapes_[i] = v;
            shape_sent_[i]  = 1;
        }
    }
}

void VrchatSink::submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) {
    if (!started_) {
        return;
    }
    if (last_send_ns_ != 0 && now_ns - last_send_ns_ < config_.min_interval_ns) {
        return;
    }

    const bool heartbeat =
        last_heartbeat_ns_ == 0 || (now_ns - last_heartbeat_ns_) >= config_.heartbeat_ns;

    std::byte buffer[kScratchSize];
    osc::BundleWriter writer{buffer};
    if (!writer.begin_bundle()) {
        ++stats_.errors;
        return;
    }

    const bool want_params =
        config_.eye_mode == EyeOutputMode::parameters || config_.eye_mode == EyeOutputMode::both;
    const bool want_native =
        config_.eye_mode == EyeOutputMode::native || config_.eye_mode == EyeOutputMode::both;

    if (want_params) {
        emit_eye_parameters(writer, frame, gaze, heartbeat);
    }
    if (want_native) {
        emit_eye_native(writer, frame, gaze, heartbeat);
    }
    emit_shapes(writer, frame, heartbeat);

    flush(writer);

    last_send_ns_ = now_ns;
    if (heartbeat) {
        last_heartbeat_ns_ = now_ns;
    }
}

} // namespace fgb
