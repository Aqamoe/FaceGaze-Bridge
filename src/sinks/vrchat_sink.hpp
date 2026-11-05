// VRChat OSC 输出。
//
// 默认走 avatar 参数（含眼追），与 VRCFT 行为一致——这是绝大多数模型唯一认的方式。
// 原生 /tracking/eye/* 是显式可切换的模式，不是默认。
//
// 从 Avatars SDK 3.10.6 / 客户端 2026.4.1 起，VRChat 还有第三条通道：原生面捕
// `/tracking/face/v1`（102 字节 blob，见 sinks/vrchat_face.hpp）。它由头像自己在
// Avatar Descriptor 里声明，**不经过 avatar 参数表**——所以老模型上发它等于白发，
// 而新模型上不发它则完全不表情。两者互斥，默认由 `auto` 按 OSCQuery 的
// `/avatar/avatarfullfacetracking` 自动选。
//
// 参数值不在这里算：catalog 给出逻辑名与取值，地址由 ParamRouter 从头像配置解析。
// Sink 只负责节流、分包、类型标签。
//
// 【地址为什么不能写死】
// VRCFT 文档写 `v2/EyeX`，真实头像里叫 `FT/v2/EyeX`，还有 `EchoFT/v2/...` 这类作者前缀。
// 写死任何一个都只能覆盖一部分头像，所以地址一律取自头像声明的参数表；
// 拿不到配置时才回退到固定前缀（并明确告知）。
#pragma once

#include "net/udp_socket.hpp"
#include "osc/encoder.hpp"
#include "params/avatar_config.hpp"
#include "params/catalog.hpp"
#include "params/oscquery.hpp"
#include "sinks/sink.hpp"
#include "sinks/throttle.hpp"
#include "sinks/vrchat_face.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fgb {

enum class EyeOutputMode {
    parameters, // 眼追写 avatar 参数（默认）
    native,     // 只发 /tracking/eye/*
    both,
};

const char* to_string(EyeOutputMode mode) noexcept;
bool parse_eye_output_mode(std::string_view text, EyeOutputMode& out) noexcept;

// 发哪一代参数：v2（默认）/ v1 legacy / 两者都发
enum class ParamsMode {
    v2,
    v1,
    both,
};

const char* to_string(ParamsMode mode) noexcept;
bool parse_params_mode(std::string_view text, ParamsMode& out) noexcept;

// 表情走哪条通道：原生 102 字节 blob（新模型）还是 avatar 参数（老模型）
enum class FaceOutputMode {
    auto_,      // 按头像是否启用原生面捕自动选（默认）
    native,     // 强制 /tracking/face/v1，不发参数
    parameters, // 强制 avatar 参数，不发 blob
};

const char* to_string(FaceOutputMode mode) noexcept;
bool parse_face_output_mode(std::string_view text, FaceOutputMode& out) noexcept;

struct VrchatSinkConfig {
    std::string host       = "127.0.0.1";
    std::uint16_t port     = 9000;
    // 关掉只停 OSC 发送；头像参数发现与路由照常，参数页不会跟着空掉
    bool enabled           = true;
    EyeOutputMode eye_mode = EyeOutputMode::parameters;
    ParamsMode params_mode = ParamsMode::v2;
    FaceOutputMode face_mode = FaceOutputMode::auto_;

    std::uint64_t min_interval_ns = 11'000'000; // ~90 Hz
    std::uint64_t heartbeat_ns    = 1'000'000'000;
    std::size_t max_datagram      = 1200; // 避免 IP 分片
    float shape_epsilon           = 0.008f;
    float gaze_epsilon            = 0.002f;
    float eye_full_scale_deg      = 30.0f;

    // 地址发现
    bool discover_avatar         = true;      // 读 OSCQuery / VRChat 写的头像参数表
    std::string avatar_config;                // 显式指定某个配置文件的路径
    std::string fallback_prefix   = "FT/v2/"; // 无配置时 v2 参数的地址前缀

    // OSCQuery：mDNS 找端点，换头像时轮询 /avatar/change 热重绑
    bool use_oscquery       = true;
    std::string oscquery_url; // 非空则跳过 mDNS，直接用这个基地址（探针/测试）
    int oscquery_timeout_ms = 500;
    int avatar_poll_ms      = 2000; // <= 0 关闭后台轮询
};

// 控制面只读取这份快照，不碰引擎线程正在用的 router
struct RouterStatus {
    bool bound               = false;
    std::size_t matched      = 0;
    std::size_t param_count  = 0;
    std::size_t target_count = 0;
    std::size_t binary_count = 0;
    std::string avatar_name;
    std::string avatar_id;
    std::string avatar_source; // OSCQuery / 配置文件 / 本地配置 / 固定前缀

    // 表情实际走哪条通道，以及是不是"真的知道"（本机头像 JSON 里没有原生面捕标记）
    bool face_native       = false;
    bool face_native_known = false;
};

class VrchatSink final : public ISink {
public:
    explicit VrchatSink(VrchatSinkConfig config);
    ~VrchatSink() override;

    const char* name() const noexcept override { return "vrchat"; }
    bool start() override;
    void stop() noexcept override;
    void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) override;
    SinkStats stats() const noexcept override { return stats_; }

    // 头像切换后重新解析地址。只置标志，实际重绑在引擎线程下一次 submit 完成。
    void reload_avatar();

    // 运行时开关 OSC 发送（控制面 set_outputs 用）。头像发现不受影响，恢复后下一帧继续发。
    void set_enabled(bool enabled) noexcept;

    // 供控制面显示"现在绑的是哪个头像、发了多少参数"
    RouterStatus status() const;
    // 每个逻辑参数是否有落地目标（1/0），顺序同 catalog。
    // 【别再用"主地址是否为空"判断绑定】二进制形式的参数（头像只声明了 `<名>1/2/4`）
    // 没有主地址却在发，用地址判空会把 PupilDilation、JawX 这类参数标成"未发送"。
    std::vector<std::uint8_t> bound_flags() const;

private:
    struct PendingAvatar {
        params::AvatarConfig config;
        std::string source;
    };

    void resolve_addresses();
    void apply_pending_avatar();
    void start_watcher();
    void watch_avatars();
    void poll_avatar_once(bool force);
    void poll_avatar_from_log(bool force);
    void publish_avatar(params::AvatarConfig config, std::string source);
    void bind_avatar_locked(const params::AvatarConfig& config, std::string_view source);
    void use_fallback();

    void map_target_groups();
    void emit_params(osc::BundleWriter& writer, const params::EvalContext& ctx,
                     bool force) noexcept;
    void emit_value(osc::BundleWriter& writer, std::size_t slot, std::string_view address,
                    params::ParamKind kind, float value, bool force) noexcept;
    void emit_eye_native(osc::BundleWriter& writer, const FaceFrame& frame, bool force) noexcept;
    void emit_face_native(osc::BundleWriter& writer, const FaceFrame& frame, bool force) noexcept;

    // 当前生效的表情通道。要求持有 router_mutex_（读头像标志）或在绑定过程中调用
    bool wants_face_native() const noexcept;
    void flush(osc::BundleWriter& writer) noexcept;
    void reset_dedup() noexcept;

    static constexpr std::size_t kScratchSize = 2048;

    VrchatSinkConfig config_;
    net::UdpSocket socket_;
    std::atomic<bool> enabled_{true};
    SinkStats stats_{};
    params::ParamRouter router_;
    std::string avatar_name_;
    std::string avatar_id_;
    std::string avatar_source_;

    // router_ 在引擎线程被 submit 读、在头像切换时被重绑；控制面走快照
    mutable std::shared_mutex router_mutex_;

    // 槽位索引 = router 目标列表下标（一个参数可能对应多条目标：float + 若干二进制位）
    std::vector<float> last_values_;
    std::vector<std::uint8_t> sent_;
    std::vector<std::uint8_t> target_group_; // 每条目标属于哪个 catalog 组，用于 v2/v1 过滤

    float last_native_pitch_yaw_[4]{};
    float last_native_closed_ = 0.0f;
    bool native_sent_         = false;

    // 原生面捕：头像有没有勾 Use All Tracking Keys（由 OSCQuery 给出），以及上一包的负载。
    // 只在 router_mutex_ 保护下读写，submit 走 shared_lock。
    bool avatar_face_native_       = false;
    bool avatar_face_native_known_ = false;
    bool face_native_sent_         = false;
    std::array<std::uint8_t, kNativeFaceBytes> last_face_bytes_{};

    SinkThrottle throttle_;
    std::uint64_t last_heartbeat_ns_ = 0;
    bool started_                    = false;

    // 后台头像 watcher：只做网络/文件 I/O 和发布新配置，不直接碰 router_
    std::thread watcher_;
    std::mutex watcher_mutex_;
    std::condition_variable watcher_wake_;
    std::atomic<bool> watcher_stop_{false};
    std::atomic<bool> reload_requested_{false};

    std::mutex pending_mutex_;
    PendingAvatar pending_;
    std::atomic<std::uint64_t> pending_seq_{0};
    std::uint64_t applied_seq_ = 0;

    // 当前绑定是不是 OSCQuery 来的。watcher 用它判断"同一个头像 ID 也要不要重拉"：
    // 启动时 OSCQuery 失败、先绑了本地 JSON，之后 OSCQuery 恢复时 ID 可能没变。
    std::atomic<bool> bound_via_oscquery_{false};

    std::string oscquery_base_url_;
    std::chrono::steady_clock::time_point next_discovery_{};
    std::string last_seen_avatar_id_;
};

} // namespace fgb
