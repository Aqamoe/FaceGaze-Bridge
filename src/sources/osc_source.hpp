// 通用 OSC 输入：按用户映射表把任意 OSC 地址翻译成统一形状 / 眼睛目标。
//
// 和 VMC 源的区别：VMC 的名字表是协议规定的（eyeBlinkLeft → EyeLid 等），
// 这里没有协议，映射完全由用户写。好处是任何能发 OSC 的软件都能接进来；
// 代价是必须有人先配表，所以映射表单独存盘、可手改、可热重启。
//
// 值语义：
//   out = raw * scale + offset
//   invert：0..1 目标（形状 / EyeLid）= 1 - out；视线轴（Eye*X/Y）= -out
//   EyeLid* 按 v2 语义，1 = 闭眼；Eye*X/Y 是 -1..1，+X 指向佩戴者右侧、+Y 向上
//
// 同一条地址每次到达都覆盖该映射的当前值；不同地址映射到同一个形状时相加
// （左右脸各发一半这类场景）。没有 Apply 包，所以收到任意一条消息就发布一帧，
// 之后值保持，直到发送端停下（合并层按 stale 超时处理）。
#pragma once

#include "config/osc_map.hpp"
#include "core/cont/seqlock.hpp"
#include "net/udp_socket.hpp"
#include "osc/decoder.hpp"
#include "sources/osc_target.hpp"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fgb {

struct OscSourceConfig {
    std::uint16_t port = 9103;
    bool any_interface = false; // 默认只绑回环；手机 OSC 应用需要 --osc-in-lan
    std::vector<config::OscMapping> mappings;
};

// 映射规则（单测与界面预览共用）：out = raw * scale + offset；
// invert 对 0..1 目标取 1-out，对视线轴取反。
float apply_osc_value(float raw, const config::OscMapping& mapping, OscTargetKind kind) noexcept;

class OscSource final : public ISource {
public:
    explicit OscSource(OscSourceConfig config);
    ~OscSource() override;

    const char* name() const noexcept override { return "osc"; }
    bool start() override;
    void stop() noexcept override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;

private:
    struct Entry {
        config::OscMapping mapping;
        OscTarget target;
        bool seen   = false;
        float value = 0.0f;
    };

    void receive_loop() noexcept;
    void handle_message(const osc::MessageView& message) noexcept;
    void publish() noexcept;

    OscSourceConfig config_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;

    net::UdpSocket socket_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> dirty_{false};

    SeqLock<FaceFrame> snapshot_;

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> packets_{0};
    std::atomic<std::uint64_t> parse_errors_{0};
};

} // namespace fgb
