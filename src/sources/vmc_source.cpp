#include "sources/vmc_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"
#include "vmc/vmc_map.hpp"

#include <chrono>
#include <cstring>

namespace fgb {
namespace {

constexpr char kTag[]           = "vmc";
constexpr SourceId kVmcSourceId = 1;

} // namespace

VmcSource::VmcSource(VmcSourceConfig config) : config_(config) {
    pending_.source    = kVmcSourceId;
    pending_.qualityScore = 1.0f;
}

VmcSource::~VmcSource() { stop(); }

bool VmcSource::start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!socket_.open_receiver(config_.port)) {
        FGB_ERROR(kTag, "无法绑定 UDP 端口 %u", static_cast<unsigned>(config_.port));
        return false;
    }
    socket_.set_nonblocking(true);
    socket_.set_recv_buffer(1 << 20);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { receive_loop(); });
    FGB_INFO(kTag, "监听 UDP :%u", static_cast<unsigned>(config_.port));
    return true;
}

void VmcSource::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    socket_.close();
    dirty_.store(false, std::memory_order_release);
}

void VmcSource::receive_loop() noexcept {
    std::byte buffer[4096];

    while (running_.load(std::memory_order_acquire)) {
        // 一次 tick 内尽量把队列排空，避免突发流量堆积
        bool any = false;
        for (int i = 0; i < 32; ++i) {
            std::string from_ip;
            std::uint16_t from_port = 0;
            const int n            = socket_.recv_from(buffer, from_ip, from_port);
            if (n <= 0) {
                if (n < 0) {
                    parse_errors_.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            any = true;
            packets_.fetch_add(1, std::memory_order_relaxed);

            const auto view = std::span<const std::byte>{buffer, static_cast<std::size_t>(n)};
            osc::for_each_message(view, [this](const osc::MessageView& m) { handle_message(m); });
        }

        if (!any) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

void VmcSource::handle_message(const osc::MessageView& message) noexcept {
    const std::string_view address = message.address();

    if (address == "/VMC/Ext/Blend/Val") {
        std::string_view name;
        float value = 0.0f;
        if (message.get_string(0, name) && message.get_float(1, value)) {
            handle_blend_value(name, value);
        }
        return;
    }

    if (address == "/VMC/Ext/Blend/Apply") {
        handle_apply();
        return;
    }

    if (address == "/VMC/Ext/Root/Pos") {
        // (name, px, py, pz, qx, qy, qz, qw) —— VMC 用 Unity 左手系
        std::string_view name;
        float v[7] = {};
        if (message.get_string(0, name)) {
            bool ok = true;
            for (std::size_t i = 0; i < 7; ++i) {
                ok = ok && message.get_float(i + 1, v[i]);
            }
            if (ok) {
                // Unity (x, y, z) → OpenXR (x, y, -z)
                pending_.headPosLocal      = {v[0], v[1], -v[2]};
                pending_.headRotLocal      = {v[3], v[4], -v[5], v[6]};
                pending_.hasHeadPose       = true;
            }
        }
        return;
    }
}

void VmcSource::handle_blend_value(std::string_view name, float value) noexcept {
    // 眨眼单独走 EyeState：它既是表情也是眼追，塞进形状数组会造成两处真相
    if (name == "eyeBlinkLeft") {
        pending_.eyes.left.openness  = clamp01(1.0f - value);
        pending_.eyes.left.valid     = true;
        pending_.eyes.left.opennessConf = 1.0f;
        return;
    }
    if (name == "eyeBlinkRight") {
        pending_.eyes.right.openness    = clamp01(1.0f - value);
        pending_.eyes.right.valid       = true;
        pending_.eyes.right.opennessConf = 1.0f;
        return;
    }

    // 累加而非覆盖：一个输入名可映射到多个肌肉形状（browInnerUp 等双侧形状）
    vmc::apply_blend(pending_, name, value);
}

void VmcSource::handle_apply() noexcept {
    // 声明这一轮实际带了哪几路：VMC 没有眼追，头姿只在收到 /VMC/Ext/Head 时才有。
    // 不声明的话合并层会把它当成"眼追来源"（neutral 的眼睛是 valid 的）。
    pending_.channels = 0;
    for (std::size_t i = 0; i < kShapeCount; ++i) {
        if (pending_.shapeValid[i] != 0) {
            pending_.add_channel(channel::kExpression);
            break;
        }
    }
    if (pending_.hasHeadPose) {
        pending_.add_channel(channel::kHead);
    }

    pending_.captureTsNs = clock::now_ns();
    pending_.sequence    = frames_.load(std::memory_order_relaxed) + 1;
    pending_.sanitize();

    snapshot_.publish(pending_);
    dirty_.store(true, std::memory_order_release);
    frames_.fetch_add(1, std::memory_order_relaxed);

    // 一轮结束，清空形状；眼球状态与头部位姿保留（VMC 未必每帧都发）
    pending_.shapes.fill(0.0f);
    pending_.shapeValid.fill(0);
}

bool VmcSource::poll(FaceFrame& out) {
    if (!dirty_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    return snapshot_.read(out);
}

SourceStats VmcSource::stats() const noexcept {
    SourceStats s;
    s.frames       = frames_.load(std::memory_order_relaxed);
    s.packets      = packets_.load(std::memory_order_relaxed);
    s.parse_errors = parse_errors_.load(std::memory_order_relaxed);
    return s;
}

} // namespace fgb
