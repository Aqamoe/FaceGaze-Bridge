// VMC 输出：把统一帧翻译成 VMC 协议发给别的程序（VSeeFace / Warudo / Unity 场景）。
//
// 为什么值得做：FGB 的价值不该只有"喂 VRChat"。把脸追结果当上游发出去，
// 用户可以让同一个表情同时驱动 VRChat 和直播里的 Unity 角色，或者接到自己的工具链上。
//
// 与输入侧的对称关系：
//   输入 /VMC/Ext/Blend/Val (名字, 值)  → 名字映射成多个肌肉形状（sources/vmc_source）
//   输出 肌肉形状 → 名字（取映射到它的最大值）→ /VMC/Ext/Blend/Val + /VMC/Ext/Blend/Apply
// 名字表是同一份（vmc/vmc_map），避免"能收不能发"或者对方不认名字。
//
// 帧结构与 VrcchatSink 一样：独立节流（默认 60 Hz）、死区、心跳全量重发。
#pragma once

#include "net/udp_socket.hpp"
#include "sinks/sink.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fgb {

struct VmcSinkConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 39539; // VMC 接收端（VSeeFace 等默认监听这里）

    std::uint64_t min_interval_ns = 16'666'667; // ~60 Hz
    std::uint64_t heartbeat_ns    = 1'000'000'000;
    std::size_t max_datagram      = 1200; // 避免 IP 分片
    float epsilon                 = 0.005f;

    bool send_head_pose = true; // 只有来源带 Head 通道时才发
    bool send_eyes      = true; // 用 eyeBlinkLeft/Right 表达睁闭眼
};

class VmcSink final : public ISink {
public:
    explicit VmcSink(VmcSinkConfig config);
    ~VmcSink() override = default;

    const char* name() const noexcept override { return "vmc-out"; }
    bool start() override;
    void stop() noexcept override;
    void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) override;
    SinkStats stats() const noexcept override { return stats_; }

    const VmcSinkConfig& config() const noexcept { return config_; }
    std::string target() const;

private:
    struct Slot {
        std::string_view name;          // 指向 vmc::blend_entries() 里的常量字符串
        std::vector<Shape> shapes;      // 同一个名字可能对应多个肌肉形状
        float last      = 0.0f;
        bool has_last   = false;
    };

    void build_slots();
    void mark_all_dirty() noexcept;

    static constexpr std::size_t kScratchSize = 2048;
    static constexpr std::size_t kHeadPoseOverhead = 96;

    VmcSinkConfig config_;
    net::UdpSocket socket_;
    SinkStats stats_{};
    std::vector<Slot> slots_;

    std::byte scratch_[kScratchSize]{};

    float last_blink_[2]{-1.0f, -1.0f};
    float last_head_[7]{};
    bool has_last_head_ = false;

    std::uint64_t last_send_ns_ = 0;
    std::uint64_t last_beat_ns_ = 0;
    bool started_               = false;
};

} // namespace fgb
