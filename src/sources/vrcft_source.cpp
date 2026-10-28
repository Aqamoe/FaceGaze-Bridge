#include "sources/vrcft_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"

#include <chrono>

namespace fgb::vrcft {
namespace {

constexpr char kTag[]             = "vrcft";
constexpr SourceId kVrcftSourceId = 2;

constexpr clock::Ns ms_to_ns(int ms) noexcept {
    return static_cast<clock::Ns>(ms) * 1'000'000ULL;
}

} // namespace

VrcftSource::VrcftSource(VrcftSourceConfig config) : config_(std::move(config)) {
    pending_.source       = kVrcftSourceId;
    pending_.qualityScore = 1.0f;
}

VrcftSource::~VrcftSource() { stop(); }

bool VrcftSource::start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    std::error_code ec;
    if (!std::filesystem::exists(config_.module_process_exe, ec)) {
        last_error_ = "找不到 VRCFaceTracking.ModuleProcess.exe";
        FGB_ERROR(kTag, "%s", last_error_.c_str());
        return false;
    }
    if (!std::filesystem::exists(config_.module_dll, ec)) {
        last_error_ = "找不到模块 DLL";
        FGB_ERROR(kTag, "%s", last_error_.c_str());
        return false;
    }
    if (!server_.open(0)) {
        last_error_ = "无法创建沙箱 UDP 端口";
        FGB_ERROR(kTag, "%s", last_error_.c_str());
        return false;
    }
    if (!config_.record_path.empty()) {
        capture_t0_ = 0;
        if (!recorder_.open(config_.record_path.string())) {
            // 录制失败不该阻断面捕本身，只告警
            FGB_WARN(kTag, "无法创建录制文件 %s，本次不录制", config_.record_path.string().c_str());
        } else {
            FGB_INFO(kTag, "录制到 %s", config_.record_path.string().c_str());
        }
    }

    FGB_INFO(kTag, "沙箱监听 127.0.0.1:%u，模块 %s", static_cast<unsigned>(server_.port()),
             config_.module_dll.filename().string().c_str());

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run_loop(); });
    return true;
}

void VrcftSource::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    host_.kill();
    server_.close();
    if (recorder_.is_open()) {
        FGB_INFO(kTag, "录制结束：%zu 帧 → %s", recorder_.frames(), recorder_.path().c_str());
        recorder_.close();
    }
    dirty_.store(false, std::memory_order_release);
}

void VrcftSource::run_loop() noexcept {
    int attempt = 0;
    while (running_.load(std::memory_order_acquire)) {
        const bool ok = run_session();
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (ok) {
            attempt = 0;
            continue; // 会话正常结束（模块自行退出），立刻重来
        }
        if (++attempt > config_.max_restarts) {
            FGB_ERROR(kTag, "连续 %d 次失败，放弃：%s", attempt - 1, last_error_.c_str());
            break;
        }
        ++restarts_;
        const int backoff_ms = 500 * attempt; // 0.5s / 1s / 1.5s
        FGB_WARN(kTag, "会话失败（%s），%d ms 后重启模块（第 %d 次）", last_error_.c_str(), backoff_ms,
                 attempt);
        clock::sleep_for_ns(ms_to_ns(backoff_ms));
    }
    running_.store(false, std::memory_order_release);
}

bool VrcftSource::wait_packet(PacketType expected, int timeout_ms) noexcept {
    const clock::Ns deadline = clock::now_ns() + ms_to_ns(timeout_ms);
    while (running_.load(std::memory_order_acquire) && clock::now_ns() < deadline) {
        Peer from;
        const int n = server_.receive(rx_, from);
        if (n <= 0) {
            clock::sleep_for_ns(200'000); // 0.2 ms，别空转烧 CPU
            continue;
        }
        ++packets_;
        if (!server_.peer().valid()) {
            server_.remember_peer(from);
        }
        last_len_ = static_cast<std::size_t>(n);

        PacketHeader header{};
        if (!parse_header(std::span<const std::byte>{rx_.data(), last_len_}, header)) {
            continue;
        }
        if (header.type == expected) {
            return true;
        }
        if (header.type == PacketType::EventLog) {
            log_event(std::span<const std::byte>{rx_.data(), last_len_});
        }
    }
    return false;
}

void VrcftSource::log_event(std::span<const std::byte> packet) noexcept {
    EventLogEntry entry{};
    if (!parse_event_log(packet, entry)) {
        return;
    }
    const auto level = entry.level;
    FGB_LOG(level >= 4 ? log::Level::error : (level == 3 ? log::Level::warn : log::Level::debug),
            kTag, "模块: %.*s", static_cast<int>(entry.text.size()), entry.text.data());
}

void VrcftSource::drain_incoming() noexcept {
    for (int i = 0; i < 32; ++i) {
        Peer from;
        const int n = server_.receive(rx_, from);
        if (n <= 0) {
            break;
        }
        ++packets_;
        if (!server_.peer().valid()) {
            server_.remember_peer(from);
        }
        last_len_ = static_cast<std::size_t>(n);
        dispatch(std::span<const std::byte>{rx_.data(), last_len_});
    }
}

void VrcftSource::dispatch(std::span<const std::byte> packet) noexcept {
    PacketHeader header{};
    if (!parse_header(packet, header)) {
        return;
    }
    if (header.type == PacketType::EventLog) {
        log_event(packet);
        return;
    }
    if (header.type != PacketType::ReplyUpdate) {
        return;
    }

    // 录制放在解码之前：存原样字节，回放时才能复现"解码"这一段行为本身
    if (recorder_.is_open() && header.body.size() == capture::kPayloadSize) {
        const std::uint64_t now = clock::now_ns();
        if (capture_t0_ == 0) {
            capture_t0_ = now;
        }
        recorder_.write(now - capture_t0_, header.body.data(), header.body.size());
    }

    UpdateData data{};
    if (!decode_update(header.body, data)) {
        return;
    }
    if (!data.has_any_valid()) {
        return; // 无效帧不覆盖，保持上次已知状态
    }

    ConvertOptions options;
    options.eye_full_scale_deg = config_.eye_full_scale_deg;
    if (apply_to_frame(data, options, pending_)) {
        publish();
    }
}

void VrcftSource::publish() noexcept {
    pending_.captureTsNs = clock::now_ns();
    pending_.sequence    = frames_.load(std::memory_order_relaxed) + 1;
    pending_.sanitize();
    snapshot_.publish(pending_);
    dirty_.store(true, std::memory_order_release);
    frames_.fetch_add(1, std::memory_order_relaxed);
}

bool VrcftSource::run_session() noexcept {
    if (!host_.launch(config_.module_process_exe, config_.module_dll, server_.port())) {
        last_error_ = "无法启动 ModuleProcess";
        return false;
    }
    server_.clear_peer();

    std::byte out[64];

    // ---- 1. 握手 ----
    if (!wait_packet(PacketType::Handshake, config_.handshake_timeout_ms)) {
        last_error_ = "等待握手超时";
        return false;
    }
    HandshakeInfo handshake{};
    if (!parse_handshake(std::span<const std::byte>{rx_.data(), last_len_}, handshake) ||
        !handshake.challenge_ok) {
        last_error_ = "握手挑战字校验失败";
        return false;
    }
    if (!server_.send(std::span<const std::byte>{rx_.data(), last_len_}, server_.peer())) {
        last_error_ = "握手 ACK 发送失败";
        return false;
    }
    FGB_DEBUG(kTag, "握手完成 pid=%d", handshake.pid);

    // ---- 2. 能力查询 ----
    std::size_t n = write_simple(PacketType::EventGetSupported, out);
    if (n == 0 || !server_.send(std::span<const std::byte>{out, n}, server_.peer())) {
        last_error_ = "GetSupported 发送失败";
        return false;
    }
    if (!wait_packet(PacketType::ReplyGetSupported, 5000)) {
        last_error_ = "等待能力查询回复超时";
        return false;
    }
    ReplySupported supported{};
    if (!parse_reply_supported(std::span<const std::byte>{rx_.data(), last_len_}, supported)) {
        last_error_ = "能力查询回复解析失败";
        return false;
    }
    if (!supported.eye && !supported.expression) {
        last_error_ = "模块不支持眼追也不支持表情";
        return false;
    }

    // ---- 3. 初始化 ----
    n = write_flags(PacketType::EventInit, supported.eye, supported.expression, out);
    if (n == 0 || !server_.send(std::span<const std::byte>{out, n}, server_.peer())) {
        last_error_ = "Init 发送失败";
        return false;
    }
    if (!wait_packet(PacketType::ReplyInit, config_.init_timeout_ms)) {
        last_error_ = "等待初始化回复超时";
        return false;
    }
    ReplyInitInfo init{};
    if (!parse_reply_init(std::span<const std::byte>{rx_.data(), last_len_}, init)) {
        last_error_ = "初始化回复解析失败";
        return false;
    }
    FGB_INFO(kTag, "模块已初始化: %.*s（眼追=%d 表情=%d）", static_cast<int>(init.module_name.size()),
             init.module_name.data(), init.eye_success ? 1 : 0, init.expression_success ? 1 : 0);

    // ---- 4. 状态同步（不发这一步，模块 Update 会空转）----
    n = write_status(ModuleState::Active, init.eye_success, init.expression_success, out);
    if (n == 0 || !server_.send(std::span<const std::byte>{out, n}, server_.peer())) {
        last_error_ = "状态同步发送失败";
        return false;
    }

    // ---- 5. 拉取循环 ----
    const clock::Ns period = 1'000'000'000ULL / static_cast<clock::Ns>(config_.tick_hz > 0
                                                                           ? config_.tick_hz
                                                                           : 100);
    clock::Ns next       = clock::now_ns() + period;
    int invalid_run      = 0;
    std::uint64_t frames_at_start = frames_.load(std::memory_order_relaxed);

    while (running_.load(std::memory_order_acquire)) {
        if (!host_.alive()) {
            last_error_ = "模块进程已退出";
            return false;
        }

        n = write_simple(PacketType::EventUpdate, out);
        if (n > 0) {
            server_.send(std::span<const std::byte>{out, n}, server_.peer());
        }

        drain_incoming();

        const std::uint64_t now_frames = frames_.load(std::memory_order_relaxed);
        if (now_frames == frames_at_start) {
            if (++invalid_run >= config_.no_data_threshold) {
                // 连上了但硬件没数据：重启模块有机会拿到干净的 socket
                last_error_ = "模块已连接但无硬件数据";
                return false;
            }
        } else {
            invalid_run      = 0;
            frames_at_start  = now_frames;
        }

        clock::sleep_until_ns(next);
        next += period;
        const clock::Ns now = clock::now_ns();
        if (next < now) {
            next = now + period;
        }
    }

    // ---- 6. 优雅卸载 ----
    n = write_simple(PacketType::EventTeardown, out);
    if (n > 0) {
        server_.send(std::span<const std::byte>{out, n}, server_.peer());
    }
    host_.wait_exit(config_.teardown_wait_ms);
    host_.kill();
    return true;
}

bool VrcftSource::poll(FaceFrame& out) {
    if (!dirty_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    return snapshot_.read(out);
}

SourceStats VrcftSource::stats() const noexcept {
    SourceStats s;
    s.frames  = frames_.load(std::memory_order_relaxed);
    s.packets = packets_.load(std::memory_order_relaxed);
    return s;
}

} // namespace fgb::vrcft
