// NVIDIA VRSS 的注视输入：每眼归一化位置（[-1,1]，中心 0,0）。
//
// 这条链路不是"注视点渲染"，而是可变速率超采样（Variable Rate Super Sampling）：
// 驱动按注视位置在中央做更高倍率超采样，用于抗锯齿。provider 每 10ms 轮询，
// 超过 150ms 没有新包就丢弃，所以这里必须按固定频率持续发送，不能做按变化过滤。
#pragma once

#include "core/model/frame.hpp"
#include "core/model/gaze.hpp"
#include "net/udp_socket.hpp"
#include "sinks/sink.hpp"
#include "sinks/throttle.hpp"
#include "vrss/geometry.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace fgb {

struct VrssSinkConfig {
    std::string host              = "127.0.0.1";
    std::uint16_t port            = 5556;
    int rate_hz                   = 90; // 30..120
    float x_gain                  = 1.0f;
    float y_gain                  = 1.0f;
    // 中心偏移，归一化单位：正 x = 右、正 y = 上。最终 ndc = point * gain + offset。
    float x_offset                = 0.0f;
    float y_offset                = 0.0f;
    // 双眼对称补偿：正值 = 内收（左眼往右、右眼往左），用来抵消驱动侧自己加出来的视差。
    // 实测：给两眼发完全相同的 NDC，头显里右眼镜区偏右、左眼镜区偏左。
    float stereo_offset           = 0.0f;
    bool flip_y                   = false; // 标准路线 +Y 向上；方向仍反时手动翻一次
    bool enabled                  = true;  // 可在运行时开关，不必重启引擎
    // 实验：允许用"来源空间"的原始逐眼 2D 点（ADR-0033 里明确不能当屏幕位置的那类）。
    // 打开后按两眼中点算一个位置给两眼，绕开两眼约 0.3 的视差；必须真机四方向确认。
    bool allow_source_points      = false;
    vrss::HmdGeometry geometry{};
};

#pragma pack(push, 1)
// 与上游 common-vrss-provider/GazeInput.h 的 UdpGazeV1 逐字节一致。
struct VrssGazePacketV1 {
    std::uint32_t magic      = 0x3147564e; // "NVG1"
    std::uint32_t version    = 1;
    std::uint64_t sequence   = 0;
    std::uint64_t qpc        = 0;
    float left_x             = 0.0f;
    float left_y             = 0.0f;
    float right_x            = 0.0f;
    float right_y            = 0.0f;
    std::uint32_t valid_mask = 3;
};
#pragma pack(pop)
static_assert(sizeof(VrssGazePacketV1) == 44, "VRSS 注视包必须与上游 44 字节契约一致");

// 为什么这一帧没发出去。没有这个，"一包都没发"和"发了但驱动不用"在页面上一模一样。
enum class VrssNoData : std::uint8_t {
    None = 0,     // 正常：这一帧发出去了
    Disabled,     // 开关关着
    NoGaze,       // 这一帧根本没有注视
    StaleGaze,    // 有注视，但超过 50 ms
    SourceSpace,  // 只有"来源空间"的 2D 点（没声明视口坐标，见 ADR-0033）
    NoGeometry,   // 只有方向型注视，但没有头显几何
};

// 机器可读原因码（IPC/日志）
const char* vrss_no_data_code(VrssNoData reason) noexcept;
// 给人看的一句话（页面直接显示）
const char* vrss_no_data_message(VrssNoData reason) noexcept;

struct VrssSinkStatus {
    bool enabled         = false;
    VrssNoData reason     = VrssNoData::NoGaze;
    std::uint64_t messages   = 0; // 已发出的包
    std::uint64_t errors     = 0; // 发送失败的包
    std::uint64_t suppressed = 0; // 被帧率节流丢掉
    std::uint64_t no_data    = 0; // 有 tick 但没有可发的注视
    std::array<float, 4> ndc{};   // 最近一次发出去的 NDC（页面自检用）
    std::uint64_t ndc_ts_ns  = 0;
};

class VrssGazeSink final : public ISink {
public:
    explicit VrssGazeSink(VrssSinkConfig config);

    const char* name() const noexcept override { return "vrss"; }
    bool start() override;
    void stop() noexcept override;
    void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) override;
    SinkStats stats() const noexcept override { return stats_; }

    // 和 submit 同一线程（引擎主循环）写入，IPC 主循环读取。
    VrssSinkStatus status() const noexcept;
    // 这一帧为什么发不出去；build_ndc 返回 false 时用来解释原因。
    static VrssNoData diagnose(const GazeState& gaze, const VrssSinkConfig& config,
                               std::uint64_t now_ns) noexcept;

    const VrssSinkConfig& config() const noexcept { return config_; }
    // IPC 命令在引擎主循环里调用，和 submit 同一线程；不需要额外加锁。
    void set_config(const VrssSinkConfig& config) noexcept;
    std::string target() const;

    // 供单测：把注视状态换算成左右眼 NDC。返回 false 表示这一帧不该发送。
    static bool build_ndc(const GazeState& gaze, std::uint64_t now_ns,
                          const VrssSinkConfig& config, std::array<float, 4>& ndc) noexcept;
    // 视口坐标 [0,1]（左上原点）→ NDC。
    static void point_to_ndc(float px, float py, float& x, float& y) noexcept;
    // 来源空间原始点 [0,1]（**左下原点**，实测该来源 +y 向上）→ NDC。
    static void source_point_to_ndc(float px, float py, float& x, float& y) noexcept;
    // HMD 局部方向 → 逐眼视口 NDC（OpenVR 投影，与上游 ProjectHeadTangent 同式）。
    static bool direction_to_ndc(Vec3 dir_local, const vrss::EyeGeometry& eye,
                                 float& x, float& y) noexcept;

private:
    VrssSinkConfig config_;
    net::UdpSocket socket_;
    SinkThrottle throttle_;
    SinkStats stats_{};
    VrssSinkStatus status_{};
    std::uint64_t sequence_    = 0;
    std::uint64_t interval_ns_ = 0;
    std::uint64_t next_geometry_try_ns_ = 0;
    bool started_              = false;
};

} // namespace fgb
