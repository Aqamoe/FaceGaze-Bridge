#include "sinks/vmc_sink.hpp"

#include "base/log.hpp"
#include "osc/encoder.hpp"
#include "vmc/vmc_map.hpp"

#include <algorithm>
#include <cmath>

namespace fgb {
namespace {

constexpr char kTag[] = "vmc-out";

// VMC 的约定：先逐条写 Blend/Val，最后一次 Apply 提交整帧。
// 中间允许分包（一个 bundle 放不下就再开一个），接收端按顺序累加，Apply 之前不提交。
constexpr char kValAddress[]      = "/VMC/Ext/Blend/Val";
constexpr char kApplyAddress[]    = "/VMC/Ext/Blend/Apply";
constexpr char kHeadAddress[]     = "/VMC/Ext/Root/Pos";
constexpr char kBlinkNames[2][16] = {"eyeBlinkLeft", "eyeBlinkRight"};

} // namespace

VmcSink::VmcSink(VmcSinkConfig config) : config_(std::move(config)) {
    if (config_.max_datagram < 256) {
        config_.max_datagram = 256;
    }
}

std::string VmcSink::target() const {
    return config_.host + ":" + std::to_string(config_.port);
}

void VmcSink::build_slots() {
    slots_.clear();
    for (const vmc::BlendEntry& entry : vmc::blend_entries()) {
        auto it = std::find_if(slots_.begin(), slots_.end(),
                               [&](const Slot& slot) { return slot.name == entry.name; });
        if (it == slots_.end()) {
            Slot slot;
            slot.name = entry.name;
            slot.shapes.push_back(entry.shape);
            slots_.push_back(std::move(slot));
        } else {
            it->shapes.push_back(entry.shape);
        }
    }
}

void VmcSink::mark_all_dirty() noexcept {
    for (Slot& slot : slots_) {
        slot.has_last = false;
    }
    last_blink_[0] = -1.0f;
    last_blink_[1] = -1.0f;
    has_last_head_ = false;
}

bool VmcSink::start() {
    if (started_) {
        return true;
    }
    if (!socket_.open_sender()) {
        FGB_ERROR(kTag, "无法创建 UDP 发送 socket");
        return false;
    }
    build_slots();
    mark_all_dirty();
    started_ = true;
    FGB_INFO(kTag, "VMC 输出 → %s（%zu 个 blendshape 名，%s）", target().c_str(), slots_.size(),
             config_.send_head_pose ? "含头姿" : "不含头姿");
    return true;
}

void VmcSink::stop() noexcept {
    socket_.close();
    started_ = false;
}

void VmcSink::submit(const FaceFrame& frame, const GazeState&, std::uint64_t now_ns) {
    if (!started_) {
        return;
    }
    if (!throttle_.due(now_ns, config_.min_interval_ns)) {
        ++stats_.suppressed;
        return;
    }

    // 心跳：即使这一帧什么都没变，也按心跳间隔全量重发一次。
    // 接收端（尤其是 Unity 场景）可能在我们启动之后才起来，只发变化等于永远不同步。
    const bool force = (last_beat_ns_ == 0) || (now_ns - last_beat_ns_ >= config_.heartbeat_ns);

    auto writer = osc::BundleWriter{std::span<std::byte>{scratch_, kScratchSize}};
    if (!writer.begin_bundle()) {
        ++stats_.errors;
        return;
    }

    std::size_t written = 0; // 当前包里写了几条消息
    std::size_t total   = 0; // 之前已经发出去的包里共几条

    auto send_chunk = [&]() -> bool {
        if (written == 0) {
            return true;
        }
        if (!socket_.send_to(writer.data(), config_.host, config_.port)) {
            ++stats_.errors;
            mark_all_dirty(); // 这一包没出去，下一帧必须全量重发
            return false;
        }
        stats_.bytes += writer.size();
        total += written;
        written = 0;
        writer.reset();
        return writer.begin_bundle();
    };

    auto room_for = [&](std::size_t bytes) {
        return writer.size() + bytes + 4 <= config_.max_datagram;
    };

    bool ok = true;

    // 同一个名字映射到多个肌肉形状时取最大值：输入侧一个名字会同时推这几个形状，
    // 输出时取最大才能保证"往返一圈至少不比原来弱"。
    for (Slot& slot : slots_) {
        float value = 0.0f;
        for (const Shape shape : slot.shapes) {
            value = std::max(value, frame.shapes[index_of(shape)]);
        }
        if (!force && slot.has_last && std::fabs(value - slot.last) < config_.epsilon) {
            continue;
        }
        if (!room_for(64) && !send_chunk()) {
            ok = false;
            break;
        }
        if (writer.begin_message(kValAddress, ",sf") && writer.add_string(slot.name) &&
            writer.add_float(value) && writer.end_message()) {
            slot.last     = value;
            slot.has_last = true;
            ++written;
        } else {
            ++stats_.errors;
            ok = false;
            break;
        }
    }

    if (ok && config_.send_eyes && frame.has_channel(channel::kEyes)) {
        const float blink[2] = {1.0f - frame.eyes.left.openness,
                                1.0f - frame.eyes.right.openness};
        for (int eye = 0; eye < 2 && ok; ++eye) {
            const float value = std::clamp(blink[eye], 0.0f, 1.0f);
            if (!force && last_blink_[eye] >= 0.0f &&
                std::fabs(value - last_blink_[eye]) < config_.epsilon) {
                continue;
            }
            if (!room_for(64) && !send_chunk()) {
                ok = false;
                break;
            }
            if (writer.begin_message(kValAddress, ",sf") &&
                writer.add_string(kBlinkNames[eye]) && writer.add_float(value) &&
                writer.end_message()) {
                last_blink_[eye] = value;
                ++written;
            } else {
                ++stats_.errors;
                ok = false;
            }
        }
    }

    if (ok && config_.send_head_pose && frame.has_channel(channel::kHead)) {
        // Unity 左手系：OpenXR 的 z 取反，四元数同理（与输入侧的换算互逆）
        const float head[7] = {frame.headPosLocal.x, frame.headPosLocal.y, -frame.headPosLocal.z,
                               frame.headRotLocal.x, frame.headRotLocal.y, -frame.headRotLocal.z,
                               frame.headRotLocal.w};
        bool changed = force || !has_last_head_;
        for (int i = 0; i < 7 && !changed; ++i) {
            changed = std::fabs(head[i] - last_head_[i]) > 1e-4f;
        }
        if (changed) {
            if (!room_for(kHeadPoseOverhead) && !send_chunk()) {
                ok = false;
            } else if (writer.begin_message(kHeadAddress, ",sffffff") &&
                       writer.add_string("HMD") && writer.add_float(head[0]) &&
                       writer.add_float(head[1]) && writer.add_float(head[2]) &&
                       writer.add_float(head[3]) && writer.add_float(head[4]) &&
                       writer.add_float(head[5]) && writer.add_float(head[6]) &&
                       writer.end_message()) {
                std::copy(std::begin(head), std::end(head), std::begin(last_head_));
                has_last_head_ = true;
                ++written;
            } else {
                ++stats_.errors;
                ok = false;
            }
        }
    }

    if (ok && written > 0) {
        // Apply 必须在最后一包里，否则接收端会拿着一半数据提交
        if (!room_for(32) && !send_chunk()) {
            ok = false;
        } else if (writer.begin_message(kApplyAddress, ",") && writer.end_message()) {
            ++written;
        } else {
            ++stats_.errors;
            ok = false;
        }
    }

    if (!ok || written == 0) {
        return; // 出错（已计数）或这一帧没有任何变化
    }

    if (!socket_.send_to(writer.data(), config_.host, config_.port)) {
        ++stats_.errors;
        mark_all_dirty();
        return;
    }
    stats_.bytes += writer.size();
    stats_.messages += total + written;

    if (force) {
        last_beat_ns_ = now_ns;
    }
}

} // namespace fgb
