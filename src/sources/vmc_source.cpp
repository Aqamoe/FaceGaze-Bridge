#include "sources/vmc_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"

#include <chrono>
#include <cstring>

namespace fgb {
namespace {

constexpr char kTag[]           = "vmc";
constexpr SourceId kVmcSourceId = 1;

struct BlendMapping {
    const char* name; // ARKit / PerfectSync 常见拼写
    Shape shape;
    float scale; // 双向形状用 +1 / -1 两条
};

// 只做直接映射。组合映射（如 mouthSmile → CornerPull + CornerSlant）留到 M3，
// 那时候需要和 VRCFT 的行为逐条对齐，不是拍脑袋能定的。
constexpr BlendMapping kMappings[] = {
    {"browDownLeft", Shape::BrowDownLeft, 1.0f},
    {"browDownRight", Shape::BrowDownRight, 1.0f},
    {"browInnerUp", Shape::BrowInnerUp, 1.0f},
    {"browOuterUpLeft", Shape::BrowOuterUpLeft, 1.0f},
    {"browOuterUpRight", Shape::BrowOuterUpRight, 1.0f},
    {"cheekPuff", Shape::CheekPuffSuck, 1.0f},
    {"cheekSquintLeft", Shape::CheekSquintLeft, 1.0f},
    {"cheekSquintRight", Shape::CheekSquintRight, 1.0f},
    {"eyeSquintLeft", Shape::EyeSquintLeft, 1.0f},
    {"eyeSquintRight", Shape::EyeSquintRight, 1.0f},
    {"jawForward", Shape::JawZ, 1.0f},
    {"jawLeft", Shape::JawX, -1.0f},
    {"jawOpen", Shape::JawOpen, 1.0f},
    {"jawRight", Shape::JawX, 1.0f},
    {"mouthClose", Shape::MouthClosed, 1.0f},
    {"mouthDimpleLeft", Shape::MouthDimpleLeft, 1.0f},
    {"mouthDimpleRight", Shape::MouthDimpleRight, 1.0f},
    {"mouthFrownLeft", Shape::MouthFrownLeft, 1.0f},
    {"mouthFrownRight", Shape::MouthFrownRight, 1.0f},
    {"mouthFunnel", Shape::LipFunnel, 1.0f},
    // ARKit 的 mouthLeft/Right 是嘴角横移，UE v2 没有对应基础形状（MouthX 属于派生层）。
    // M1 先用 MouthLowerX 近似，等 M3 实现派生层时再改。
    {"mouthLeft", Shape::MouthLowerX, -1.0f},
    {"mouthLowerDownLeft", Shape::MouthLowerDownLeft, 1.0f},
    {"mouthLowerDownRight", Shape::MouthLowerDownRight, 1.0f},
    {"mouthPressLeft", Shape::MouthPressLeft, 1.0f},
    {"mouthPressRight", Shape::MouthPressRight, 1.0f},
    {"mouthPucker", Shape::LipPucker, 1.0f},
    {"mouthRight", Shape::MouthLowerX, 1.0f},
    {"mouthShrugLower", Shape::MouthRaiserLower, 1.0f},
    {"mouthShrugUpper", Shape::MouthRaiserUpper, 1.0f},
    {"mouthSmileLeft", Shape::MouthCornerPullLeft, 1.0f},
    {"mouthSmileRight", Shape::MouthCornerPullRight, 1.0f},
    {"mouthStretchLeft", Shape::MouthStretchLeft, 1.0f},
    {"mouthStretchRight", Shape::MouthStretchRight, 1.0f},
    {"mouthUpperUpLeft", Shape::MouthUpperUpLeft, 1.0f},
    {"mouthUpperUpRight", Shape::MouthUpperUpRight, 1.0f},
    {"noseSneerLeft", Shape::NoseSneerLeft, 1.0f},
    {"noseSneerRight", Shape::NoseSneerRight, 1.0f},
    {"tongueOut", Shape::TongueOut, 1.0f},
};

bool lookup_blend(std::string_view name, Shape& shape, float& scale) noexcept {
    for (const auto& entry : kMappings) {
        if (name == entry.name) {
            shape = entry.shape;
            scale = entry.scale;
            return true;
        }
    }
    return false;
}

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

    Shape shape{};
    float scale = 1.0f;
    if (!lookup_blend(name, shape, scale)) {
        return;
    }
    const std::size_t idx = index_of(shape);
    // 累加而非覆盖：双向形状（JawX / MouthX）由两条 entry 相加得到
    pending_.shapes[idx] += value * scale;
    pending_.shapeValid[idx] = 1;
}

void VmcSource::handle_apply() noexcept {
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
