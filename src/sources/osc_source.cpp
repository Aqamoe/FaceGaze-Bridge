#include "sources/osc_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"

#include <chrono>
#include <cmath>
#include <utility>

namespace fgb {
namespace {

constexpr char kTag[]            = "osc";
constexpr SourceId kOscSourceId  = 4;
constexpr float kGazeAxisMaxDeg  = 30.0f;

} // namespace

float apply_osc_value(float raw, const config::OscMapping& mapping, OscTargetKind kind) noexcept {
    float value = raw * mapping.scale + mapping.offset;
    if (mapping.invert) {
        value = osc_target_is_signed(kind) ? -value : 1.0f - value;
    }
    return value;
}

OscSource::OscSource(OscSourceConfig config) : config_(std::move(config)) {
    entries_.reserve(config_.mappings.size());
    for (config::OscMapping& mapping : config_.mappings) {
        OscTarget target;
        if (!parse_osc_target(mapping.target, target)) {
            FGB_WARN(kTag, "映射 %s 的目标「%s」不认识，已跳过", mapping.address.c_str(),
                     mapping.target.c_str());
            continue;
        }
        const std::size_t index = entries_.size();
        index_.emplace(mapping.address, index);
        Entry entry;
        entry.mapping = std::move(mapping);
        entry.target  = target;
        entries_.push_back(std::move(entry));
    }
}

OscSource::~OscSource() { stop(); }

bool OscSource::start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!socket_.open_receiver(config_.port, false, config_.any_interface)) {
        FGB_ERROR(kTag, "无法绑定 UDP 端口 %u", static_cast<unsigned>(config_.port));
        return false;
    }
    socket_.set_nonblocking(true);
    socket_.set_recv_buffer(1 << 20);

    if (entries_.empty()) {
        FGB_WARN(kTag, "映射表是空的：收到 OSC 也不会有输出（编辑 osc_map.ini 后重启）");
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { receive_loop(); });
    FGB_INFO(kTag, "监听 UDP %s:%u，%zu 条映射", config_.any_interface ? "0.0.0.0" : "127.0.0.1",
             static_cast<unsigned>(config_.port), entries_.size());
    return true;
}

void OscSource::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    socket_.close();
    dirty_.store(false, std::memory_order_release);
}

void OscSource::receive_loop() noexcept {
    std::byte buffer[4096];

    while (running_.load(std::memory_order_acquire)) {
        bool any_message = false;
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
            packets_.fetch_add(1, std::memory_order_relaxed);

            const auto view = std::span<const std::byte>{buffer, static_cast<std::size_t>(n)};
            osc::for_each_message(view, [this](const osc::MessageView& m) {
                handle_message(m);
            });
            any_message = true;
        }

        if (any_message) {
            publish();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

void OscSource::handle_message(const osc::MessageView& message) noexcept {
    const auto it = index_.find(std::string{message.address()});
    if (it == index_.end()) {
        return;
    }
    Entry& entry = entries_[it->second];

    float raw = 0.0f;
    if (!message.get_float(0, raw)) {
        std::int32_t as_int = 0;
        bool as_bool       = false;
        if (message.get_int(0, as_int)) {
            raw = static_cast<float>(as_int);
        } else if (message.get_bool(0, as_bool)) {
            raw = as_bool ? 1.0f : 0.0f;
        } else {
            return; // 不是数值消息（字符串等），忽略
        }
    }

    float value = apply_osc_value(raw, entry.mapping, entry.target.kind);
    if (!std::isfinite(value)) {
        return;
    }
    entry.value = value;
    entry.seen  = true;
}

void OscSource::publish() noexcept {
    FaceFrame frame;
    frame.source     = kOscSourceId;
    frame.qualityScore = 1.0f;

    float gaze_x[2] = {0.0f, 0.0f};
    float gaze_y[2] = {0.0f, 0.0f};
    bool gaze_seen[2] = {false, false};
    bool lid_seen[2]  = {false, false};
    bool eye_seen[2]  = {false, false};
    bool any_expression = false;

    for (const Entry& entry : entries_) {
        if (!entry.seen) {
            continue;
        }
        const float value = entry.value;
        switch (entry.target.kind) {
        case OscTargetKind::Shape: {
            const std::size_t index = index_of(entry.target.shape);
            frame.shapes[index] += value;
            frame.shapeValid[index] = 1;
            any_expression          = true;
            break;
        }
        case OscTargetKind::EyeOpenLeft:
            frame.eyes.left.openness     = 1.0f - value;
            frame.eyes.left.opennessConf = 1.0f;
            lid_seen[0]                  = true;
            eye_seen[0]                  = true;
            break;
        case OscTargetKind::EyeOpenRight:
            frame.eyes.right.openness     = 1.0f - value;
            frame.eyes.right.opennessConf = 1.0f;
            lid_seen[1]                   = true;
            eye_seen[1]                   = true;
            break;
        case OscTargetKind::EyeGazeXLeft:
            gaze_x[0]  = value;
            gaze_seen[0] = true;
            eye_seen[0]  = true;
            break;
        case OscTargetKind::EyeGazeYLeft:
            gaze_y[0]  = value;
            gaze_seen[0] = true;
            eye_seen[0]  = true;
            break;
        case OscTargetKind::EyeGazeXRight:
            gaze_x[1]  = value;
            gaze_seen[1] = true;
            eye_seen[1]  = true;
            break;
        case OscTargetKind::EyeGazeYRight:
            gaze_y[1]  = value;
            gaze_seen[1] = true;
            eye_seen[1]  = true;
            break;
        }
    }

    for (int eye = 0; eye < 2; ++eye) {
        EyeState& state = eye == 0 ? frame.eyes.left : frame.eyes.right;
        if (!eye_seen[eye]) {
            continue;
        }
        state.valid = true;
        state.opennessConf = 1.0f;
        if (!lid_seen[eye]) {
            state.openness = 1.0f;
        }
        state.openness = clamp01(state.openness);
        if (gaze_seen[eye]) {
            const float yaw_deg   = clamp_sym(gaze_x[eye]) * kGazeAxisMaxDeg;
            const float pitch_deg = clamp_sym(gaze_y[eye]) * kGazeAxisMaxDeg;
            state.gazePitchYawDeg = {pitch_deg, yaw_deg};
            state.gazeDirLocal    = pitch_yaw_deg_to_dir(pitch_deg, yaw_deg);
            state.gazeConf        = 1.0f;
        }
    }

    if (any_expression) {
        frame.add_channel(channel::kExpression);
    }
    if (eye_seen[0] || eye_seen[1]) {
        frame.add_channel(channel::kEyes);
    }

    frame.captureTsNs = clock::now_ns();
    frame.sequence    = frames_.load(std::memory_order_relaxed) + 1;
    frame.sanitize();

    snapshot_.publish(frame);
    dirty_.store(true, std::memory_order_release);
    frames_.fetch_add(1, std::memory_order_relaxed);
}

bool OscSource::poll(FaceFrame& out) {
    if (!dirty_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    return snapshot_.read(out);
}

SourceStats OscSource::stats() const noexcept {
    SourceStats s;
    s.frames       = frames_.load(std::memory_order_relaxed);
    s.packets      = packets_.load(std::memory_order_relaxed);
    s.parse_errors = parse_errors_.load(std::memory_order_relaxed);
    return s;
}

} // namespace fgb
