// VRChat OSC 输出。
//
// 默认走 avatar 参数（含眼追），与 VRCFT 行为一致——这是绝大多数模型唯一认的方式。
// 原生 /tracking/eye/* 是显式可切换的模式，不是默认。
#pragma once

#include "net/udp_socket.hpp"
#include "osc/encoder.hpp"
#include "sinks/sink.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace fgb {

// EyeLeftX, EyeLeftY, EyeRightX, EyeRightY, EyeLidLeft, EyeLidRight
inline constexpr std::size_t kEyeParamCount = 6;

enum class EyeOutputMode {
    parameters, // 眼追写 avatar 参数（默认）
    native,     // 只发 /tracking/eye/*
    both,
};

const char* to_string(EyeOutputMode mode) noexcept;
bool parse_eye_output_mode(std::string_view text, EyeOutputMode& out) noexcept;

struct VrchatSinkConfig {
    std::string host    = "127.0.0.1";
    std::uint16_t port  = 9000;
    EyeOutputMode eye_mode = EyeOutputMode::parameters;

    std::uint64_t min_interval_ns = 11'000'000;  // ~90 Hz
    std::uint64_t heartbeat_ns    = 1'000'000'000;
    std::size_t max_datagram      = 1200;        // 避免 IP 分片
    float shape_epsilon           = 0.008f;
    float gaze_epsilon            = 0.002f;
    float eye_full_scale_deg      = 30.0f;       // 视线满量程角度
};

class VrchatSink final : public ISink {
public:
    explicit VrchatSink(VrchatSinkConfig config);
    ~VrchatSink() override = default;

    const char* name() const noexcept override { return "vrchat"; }
    bool start() override;
    void stop() noexcept override;
    void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) override;
    SinkStats stats() const noexcept override { return stats_; }

    // 眼追参数派生。纯函数，单独可测——这是最容易做错又最难肉眼发现的部分。
    // out 顺序：EyeLeftX, EyeLeftY, EyeRightX, EyeRightY, EyeLidLeft, EyeLidRight
    static void compute_eye_params(const EyePair& eyes, float full_scale_deg, float out[kEyeParamCount]) noexcept;

private:
    void emit_eye_parameters(osc::BundleWriter& writer, const FaceFrame& frame, const GazeState& gaze,
                             bool force) noexcept;
    void emit_eye_native(osc::BundleWriter& writer, const FaceFrame& frame, const GazeState& gaze,
                         bool force) noexcept;
    void emit_shapes(osc::BundleWriter& writer, const FaceFrame& frame, bool force) noexcept;
    void flush(osc::BundleWriter& writer) noexcept;
    void reset_dedup() noexcept;

    static constexpr std::size_t kScratchSize = 2048;

    VrchatSinkConfig config_;
    net::UdpSocket socket_;
    SinkStats stats_{};

    std::array<float, kShapeCount> last_shapes_{};
    std::array<std::uint8_t, kShapeCount> shape_sent_{};

    std::array<float, kEyeParamCount> last_eye_{};
    std::array<std::uint8_t, kEyeParamCount> eye_sent_{};

    float last_native_pitch_yaw_[4]{};
    float last_native_closed_   = 0.0f;
    bool native_sent_           = false;

    std::uint64_t last_send_ns_      = 0;
    std::uint64_t last_heartbeat_ns_ = 0;
    bool started_                    = false;
};

} // namespace fgb
