// ARKit LiveLink 输入（UE Apple ARKit Face Support 的 UDP 协议，默认 11111）。
//
// 线格式（大端，Epic 的 AppleARKitLiveLinkSource.cpp）：
//   u8    version（实测 6）
//   i32   device_id 字节数 + UTF-8
//   i32   subject 字节数 + UTF-8
//   i32   frame_number / f32 sub_frame / i32 fps 分子 / i32 分母
//   u8    curve_count（通常 61）
//   f32[] curve_count 个
//
// 61 条曲线的顺序：0..51 = Apple ARKit 52 blendshape（官方顺序），
// 52..54 = HeadYaw/HeadPitch/HeadRoll，55..57 = 左眼 Yaw/Pitch/Roll，
// 58..60 = 右眼 Yaw/Pitch/Roll。没有头部位移。
//
// 未在真机验收（手头没有 iPhone）。头姿与眼姿的单位/符号没有 Epic 一手文档：
// 社区实现（PyLiveLinkFace）记录为 -1..1 归一化，这里按 ±90°（头）/ ±30°（眼）
// 解释；符号按 UE（左手，X 前 Y 右 Z 上）→ OpenXR（右手，-Z 前）推导。
// 若实机发现方向相反/幅度不对，只需改这里的两个常量与符号（ADR-0029）。
#pragma once

#include "core/cont/seqlock.hpp"
#include "net/udp_socket.hpp"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <thread>

namespace fgb {

struct LiveLinkSourceConfig {
    std::uint16_t port = 11111;
};

struct LiveLinkFrameInfo {
    std::uint8_t version = 0;
    std::uint8_t curve_count = 0;
    std::string subject;
};

// 解析一个数据报。容错：
//   - 允许 4 字节大端长度前缀（社区回放工具会加，LiveLink Face 本身不加）
//   - curve_count 超过 61 截断，少于 61 时只使用实际条数
// 失败返回 false，out 内容无意义。
bool parse_livelink_frame(std::span<const std::byte> packet, FaceFrame& out,
                          LiveLinkFrameInfo* info = nullptr) noexcept;

// 头姿归一化幅度（-1..1 → 度）。未实机校准前集中在这里，方便一处改。
inline constexpr float kLiveLinkHeadMaxDeg = 90.0f;
inline constexpr float kLiveLinkEyeMaxDeg  = 30.0f;

class LiveLinkSource final : public ISource {
public:
    explicit LiveLinkSource(LiveLinkSourceConfig config);
    ~LiveLinkSource() override;

    const char* name() const noexcept override { return "livelink"; }
    bool start() override;
    void stop() noexcept override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;

private:
    void receive_loop() noexcept;
    void publish(FaceFrame& frame, const LiveLinkFrameInfo& info) noexcept;

    LiveLinkSourceConfig config_;
    net::UdpSocket socket_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> dirty_{false};

    SeqLock<FaceFrame> snapshot_;
    std::string last_subject_;

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> parse_errors_{0};
};

} // namespace fgb
