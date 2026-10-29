// WebSocket JSON 输出：给网页/脚本/研究工具的可读数据流。
//
// 与共享内存/UDP 二进制的取舍：
//   共享内存  零拷贝，但要写 C/C# 读取端
//   UDP 二进制 最低延迟，但字段是固定偏移
//   WS JSON   浏览器/Node/Python 直接连，代价是文本编码和 ~30 Hz 的节流
//
// 只绑 127.0.0.1，无鉴权——它是本机数据口，不是公网服务。
// 协议格式见 docs/07-streaming-outputs.md（与 docs/02-features.md §3.3 对齐）。
#pragma once

#include "net/ws_server.hpp"
#include "sinks/sink.hpp"
#include "sinks/throttle.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace fgb {

struct WsJsonConfig {
    std::uint16_t port = 0; // 0 = 未启用（CLI 必须显式给 --ws-out）
    std::uint64_t min_interval_ns = 33'333'333; // ~30 Hz
    bool all_shapes       = false; // false = 只发 > shape_epsilon 的有效形状
    float shape_epsilon   = 0.01f;
    std::string source_name = "fgb";
};

class WsJsonSink final : public ISink {
public:
    explicit WsJsonSink(WsJsonConfig config);
    ~WsJsonSink() override = default;

    const char* name() const noexcept override { return "ws-out"; }
    bool start() override;
    void stop() noexcept override;
    void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) override;
    SinkStats stats() const noexcept override { return stats_; }

    std::uint16_t port() const noexcept { return server_.port(); }
    std::size_t clients() const noexcept { return server_.ready_count(); }
    // 端口被占时 start() 仍返回 true（不拖垮引擎），但这里为 false
    bool listening() const noexcept { return started_.load(); }

    // 供测试/探针对照：生成一帧 JSON 文本
    static std::string build_json(const FaceFrame& frame, const GazeState& gaze,
                                  std::uint64_t now_ns, std::uint64_t seq,
                                  std::string_view source, bool all_shapes, float epsilon);

private:
    WsJsonConfig config_;
    net::WsServer server_;
    SinkStats stats_{};
    SinkThrottle throttle_;
    std::uint64_t seq_ = 0;
    std::atomic<bool> started_{false};
};

} // namespace fgb
