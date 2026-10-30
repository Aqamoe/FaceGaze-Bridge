#include "sources/livelink_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"
#include "osc/message.hpp"
#include "vmc/vmc_map.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

namespace fgb {
namespace {

constexpr char kTag[]                = "livelink";
constexpr SourceId kLiveLinkSourceId = 5;
constexpr std::uint8_t kMaxCurves    = 61;
constexpr std::uint8_t kArkitCount   = 52;
constexpr std::size_t kMaxStringLen  = 1024;

// Apple ARKit 的 52 个 blendshape，顺序即 LiveLink 曲线 0..51 的顺序。
// 名字转成 lowerCamel 后与 VMC 名字表共用一套映射（vmc/vmc_map.cpp）。
constexpr const char* kArkitNames[kArkitCount] = {
    "eyeBlinkLeft",      "eyeLookDownLeft",   "eyeLookInLeft",     "eyeLookOutLeft",
    "eyeLookUpLeft",     "eyeSquintLeft",     "eyeWideLeft",       "eyeBlinkRight",
    "eyeLookDownRight",  "eyeLookInRight",    "eyeLookOutRight",   "eyeLookUpRight",
    "eyeSquintRight",    "eyeWideRight",      "jawForward",        "jawLeft",
    "jawRight",          "jawOpen",           "mouthClose",        "mouthFunnel",
    "mouthPucker",       "mouthLeft",         "mouthRight",        "mouthSmileLeft",
    "mouthSmileRight",   "mouthFrownLeft",    "mouthFrownRight",   "mouthDimpleLeft",
    "mouthDimpleRight",  "mouthStretchLeft",  "mouthStretchRight", "mouthRollLower",
    "mouthRollUpper",    "mouthShrugLower",   "mouthShrugUpper",   "mouthPressLeft",
    "mouthPressRight",   "mouthLowerDownLeft", "mouthLowerDownRight", "mouthUpperUpLeft",
    "mouthUpperUpRight", "browDownLeft",      "browDownRight",     "browInnerUp",
    "browOuterUpLeft",   "browOuterUpRight",  "cheekPuff",         "cheekSquintLeft",
    "cheekSquintRight",  "noseSneerLeft",     "noseSneerRight",    "tongueOut",
};

bool read_string(std::span<const std::byte> data, std::size_t& pos, std::string* out) noexcept {
    if (pos + 4 > data.size()) {
        return false;
    }
    const std::int32_t length = static_cast<std::int32_t>(osc::read_be32(data.data() + pos));
    pos += 4;
    if (length < 0 || static_cast<std::size_t>(length) > kMaxStringLen ||
        pos + static_cast<std::size_t>(length) > data.size()) {
        return false;
    }
    if (out != nullptr) {
        out->assign(reinterpret_cast<const char*>(data.data() + pos),
                    static_cast<std::size_t>(length));
    }
    pos += static_cast<std::size_t>(length);
    return true;
}

void set_eye_gaze(EyeState& eye, float x, float y) noexcept {
    const float yaw_deg   = clamp_sym(x) * kLiveLinkEyeMaxDeg;
    const float pitch_deg = clamp_sym(y) * kLiveLinkEyeMaxDeg;
    eye.gazePitchYawDeg   = {pitch_deg, yaw_deg};
    eye.gazeDirLocal      = pitch_yaw_deg_to_dir(pitch_deg, yaw_deg);
    eye.gazeConf          = 1.0f;
}

} // namespace

bool parse_livelink_frame(std::span<const std::byte> packet, FaceFrame& out,
                          LiveLinkFrameInfo* info) noexcept {
    if (packet.size() < 8) {
        return false;
    }

    // 少数回放/转发工具会在前面加 4 字节大端长度；LiveLink Face 本身不加。
    // 版本号是第一个字节（实测 6），不可能为 0，所以能用它区分。
    std::span<const std::byte> data = packet;
    const auto version_of = [](std::span<const std::byte> d) {
        return std::to_integer<std::uint8_t>(d[0]);
    };
    if (version_of(data) == 0 || version_of(data) > 16) {
        const std::uint64_t length = osc::read_be32(data.data());
        if (length + 4 != data.size()) {
            return false;
        }
        data = data.subspan(4);
        if (data.size() < 8) {
            return false;
        }
    }

    std::size_t pos = 0;
    const std::uint8_t version = std::to_integer<std::uint8_t>(data[pos++]);

    std::string subject;
    if (!read_string(data, pos, nullptr) ||
        !read_string(data, pos, info != nullptr ? &subject : nullptr)) {
        return false;
    }
    if (pos + 16 > data.size()) { // frame_number / sub_frame / fps 分子 / 分母
        return false;
    }
    pos += 16;

    if (pos + 1 > data.size()) {
        return false;
    }
    const std::uint8_t count = std::to_integer<std::uint8_t>(data[pos++]);
    if (count == 0 || count > kMaxCurves) {
        return false;
    }
    if (pos + static_cast<std::size_t>(count) * 4 > data.size()) {
        return false;
    }

    float curves[kMaxCurves]{};
    for (std::uint8_t i = 0; i < count; ++i) {
        const std::uint32_t bits = osc::read_be32(data.data() + pos);
        std::memcpy(&curves[i], &bits, sizeof(float));
        pos += 4;
    }

    FaceFrame frame;
    frame.source       = kLiveLinkSourceId;
    frame.qualityScore = 1.0f;

    const std::uint8_t shape_count = count < kArkitCount ? count : kArkitCount;
    for (std::uint8_t i = 0; i < shape_count; ++i) {
        // blink / look* 不在 vmc 名字表里，下面单独处理；这里对它们是无操作
        vmc::apply_blend(frame, kArkitNames[i], curves[i]);
    }
    frame.add_channel(channel::kExpression);

    if (shape_count > 0) {
        frame.eyes.left.openness     = clamp01(1.0f - curves[0]);
        frame.eyes.left.opennessConf = 1.0f;
        frame.eyes.left.valid        = true;
    }
    if (shape_count > 7) {
        frame.eyes.right.openness     = clamp01(1.0f - curves[7]);
        frame.eyes.right.opennessConf = 1.0f;
        frame.eyes.right.valid        = true;
    }
    frame.add_channel(channel::kEyes);
    // 眼姿用 eyeLook* 而不用曲线 55..60：前者语义在 ARKit 文档里是明确的
    //（in/out/up/down），后者的单位和符号没有一手文档。
    if (shape_count > 4) {
        const float x = curves[2] - curves[3]; // 向左眼内 = 佩戴者右侧
        const float y = curves[4] - curves[1];
        set_eye_gaze(frame.eyes.left, x, y);
    }
    if (shape_count > 11) {
        const float x = curves[10] - curves[9]; // 向右眼外 = 佩戴者右侧
        const float y = curves[11] - curves[8];
        set_eye_gaze(frame.eyes.right, x, y);
    }

    if (count >= 55) {
        // 归一化 -1..1（PyLiveLinkFace 的记录），头姿按 ±90° 解释；
        // UE 左手（X 前 Y 右 Z 上）→ OpenXR：pitch 保持，yaw/roll 取反。
        const float yaw   = curves[52] * kLiveLinkHeadMaxDeg;
        const float pitch = curves[53] * kLiveLinkHeadMaxDeg;
        const float roll  = curves[54] * kLiveLinkHeadMaxDeg;
        if (std::fabs(yaw) + std::fabs(pitch) + std::fabs(roll) > 0.01f) {
            frame.hasHeadPose  = true;
            frame.headRotLocal = quat_from_euler_deg(pitch, -yaw, -roll);
            frame.headPosLocal = {};
            frame.add_channel(channel::kHead);
        }
    }

    frame.captureTsNs = clock::now_ns();
    frame.sanitize();

    if (info != nullptr) {
        info->version     = version;
        info->curve_count = count;
        info->subject     = std::move(subject);
    }
    out = frame;
    return true;
}

LiveLinkSource::LiveLinkSource(LiveLinkSourceConfig config) : config_(config) {}

LiveLinkSource::~LiveLinkSource() { stop(); }

bool LiveLinkSource::start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    // iPhone 在局域网另一头，必须绑所有网卡（VMC/OSC 默认只绑回环）
    if (!socket_.open_receiver(config_.port, false, true)) {
        FGB_ERROR(kTag, "无法绑定 UDP 端口 %u", static_cast<unsigned>(config_.port));
        return false;
    }
    socket_.set_nonblocking(true);
    socket_.set_recv_buffer(1 << 20);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { receive_loop(); });
    FGB_INFO(kTag, "监听 UDP 0.0.0.0:%u（Live Link Face）", static_cast<unsigned>(config_.port));
    return true;
}

void LiveLinkSource::stop() noexcept {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    socket_.close();
    dirty_.store(false, std::memory_order_release);
}

void LiveLinkSource::receive_loop() noexcept {
    std::byte buffer[2048];

    while (running_.load(std::memory_order_acquire)) {
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

            FaceFrame frame;
            LiveLinkFrameInfo info;
            const auto view = std::span<const std::byte>{buffer, static_cast<std::size_t>(n)};
            if (parse_livelink_frame(view, frame, &info)) {
                publish(frame, info);
            } else {
                parse_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!any) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
}

void LiveLinkSource::publish(FaceFrame& frame, const LiveLinkFrameInfo& info) noexcept {
    if (info.subject != last_subject_) {
        last_subject_ = info.subject;
        FGB_INFO(kTag, "收到数据: subject=%s version=%u curves=%u", last_subject_.c_str(),
                 static_cast<unsigned>(info.version), static_cast<unsigned>(info.curve_count));
    }
    frame.captureTsNs = clock::now_ns();
    frame.sequence    = frames_.load(std::memory_order_relaxed) + 1;

    snapshot_.publish(frame);
    dirty_.store(true, std::memory_order_release);
    frames_.fetch_add(1, std::memory_order_relaxed);
}

bool LiveLinkSource::poll(FaceFrame& out) {
    if (!dirty_.exchange(false, std::memory_order_acq_rel)) {
        return false;
    }
    return snapshot_.read(out);
}

SourceStats LiveLinkSource::stats() const noexcept {
    SourceStats s;
    s.frames       = frames_.load(std::memory_order_relaxed);
    s.packets      = packets_.load(std::memory_order_relaxed);
    s.parse_errors = parse_errors_.load(std::memory_order_relaxed);
    return s;
}

} // namespace fgb
