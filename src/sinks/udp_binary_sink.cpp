#include "sinks/udp_binary_sink.hpp"

#include "base/log.hpp"

#include <cstring>
#include <span>
#include <utility>

namespace fgb {
namespace {
constexpr char kTag[] = "udp-out";
static_assert(kShapeCount <= FGB_UDP_MAX_SHAPES, "UDP 帧的形状容量小于当前形状数");
}

UdpBinarySink::UdpBinarySink(UdpBinaryConfig config) : config_(std::move(config)) {}

std::string UdpBinarySink::target() const {
    return config_.host + ":" + std::to_string(config_.port);
}

bool UdpBinarySink::start() {
    if (config_.port == 0) {
        FGB_ERROR(kTag, "UDP 二进制输出没有目标端口");
        return false;
    }
    if (!socket_.open_sender()) {
        FGB_ERROR(kTag, "无法创建 UDP socket");
        return false;
    }
    started_ = true;
    FGB_INFO(kTag, "UDP 二进制输出 → %s（%u 字节/帧，%s）", target().c_str(),
             static_cast<unsigned>(FGB_UDP_FRAME_SIZE),
             config_.min_interval_ns == 0 ? "每 tick" : "按间隔节流");
    return true;
}

void UdpBinarySink::stop() noexcept {
    started_ = false;
    socket_.close();
}

UdpBinarySink::Datagram UdpBinarySink::serialize(const FaceFrame& frame, const GazeState& gaze,
                                                 std::uint64_t now_ns) noexcept {
    FgbUdpFrameV1 wire{};
    wire.magic           = FGB_UDP_MAGIC;
    wire.version         = FGB_UDP_VERSION;
    wire.header_size     = 8;
    wire.sequence        = frame.sequence;
    wire.capture_ts_ns   = frame.captureTsNs;
    wire.publish_ts_ns   = now_ns;
    wire.channels        = frame.channels;
    wire.shape_count     = static_cast<std::uint32_t>(kShapeCount);
    wire.vergence_distance_m =
        frame.eyes.hasVergence ? frame.eyes.vergenceDistanceM : gaze.vergence_m;

    std::uint32_t attributes = 0;
    if (frame.eyes.left.valid) {
        attributes |= FGB_UDP_ATTR_LEFT_VALID;
    }
    if (frame.eyes.right.valid) {
        attributes |= FGB_UDP_ATTR_RIGHT_VALID;
    }
    if (frame.hasHeadPose) {
        attributes |= FGB_UDP_ATTR_HAS_HEAD;
    }
    if (gaze.fixation) {
        attributes |= FGB_UDP_ATTR_FIXATION;
    }
    if (gaze.valid) {
        attributes |= FGB_UDP_ATTR_GAZE_VALID;
    }
    wire.attributes = attributes;

    const EyeState& left  = frame.eyes.left;
    const EyeState& right = frame.eyes.right;
    wire.left_openness            = left.openness;
    wire.left_openness_confidence = left.opennessConf;
    wire.left_gaze_dir[0]         = left.gazeDirLocal.x;
    wire.left_gaze_dir[1]         = left.gazeDirLocal.y;
    wire.left_gaze_dir[2]         = left.gazeDirLocal.z;
    wire.left_gaze_confidence     = left.gazeConf;

    wire.right_openness            = right.openness;
    wire.right_openness_confidence = right.opennessConf;
    wire.right_gaze_dir[0]         = right.gazeDirLocal.x;
    wire.right_gaze_dir[1]         = right.gazeDirLocal.y;
    wire.right_gaze_dir[2]         = right.gazeDirLocal.z;
    wire.right_gaze_confidence     = right.gazeConf;

    wire.gaze_dir[0]      = gaze.dir_local.x;
    wire.gaze_dir[1]      = gaze.dir_local.y;
    wire.gaze_dir[2]      = gaze.dir_local.z;
    wire.gaze_pitch_deg   = gaze.pitch_deg;
    wire.gaze_yaw_deg     = gaze.yaw_deg;
    wire.gaze_confidence  = gaze.confidence;

    wire.gaze_valid = 0;
    if (gaze.left_valid) {
        wire.gaze_valid |= FGB_UDP_GAZE_LEFT_VALID;
    }
    if (gaze.right_valid) {
        wire.gaze_valid |= FGB_UDP_GAZE_RIGHT_VALID;
    }
    if (gaze.valid) {
        wire.gaze_valid |= FGB_UDP_GAZE_COMBINED_VALID;
    }
    if (gaze.point_viewport) {
        wire.gaze_valid |= FGB_UDP_GAZE_POINT_VIEWPORT;
    }
    if (gaze.point2d_valid) {
        wire.gaze_valid |= FGB_UDP_GAZE_POINT_2D_VALID;
    }
    wire.gaze_source            = gaze.source;
    wire.gaze_source_mask       = gaze.source_mask;
    wire.gaze_reserved          = 0;
    wire.gaze_capture_ts_ns     = gaze.capture_ts_ns;
    wire.gaze_left_dir[0]       = gaze.left_dir_local.x;
    wire.gaze_left_dir[1]       = gaze.left_dir_local.y;
    wire.gaze_left_dir[2]       = gaze.left_dir_local.z;
    wire.gaze_right_dir[0]      = gaze.right_dir_local.x;
    wire.gaze_right_dir[1]      = gaze.right_dir_local.y;
    wire.gaze_right_dir[2]      = gaze.right_dir_local.z;
    wire.gaze_left_point_2d[0]  = gaze.left_point_2d.x;
    wire.gaze_left_point_2d[1]  = gaze.left_point_2d.y;
    wire.gaze_right_point_2d[0] = gaze.right_point_2d.x;
    wire.gaze_right_point_2d[1] = gaze.right_point_2d.y;

    if (frame.hasHeadPose) {
        wire.head_rotation[0] = frame.headRotLocal.x;
        wire.head_rotation[1] = frame.headRotLocal.y;
        wire.head_rotation[2] = frame.headRotLocal.z;
        wire.head_rotation[3] = frame.headRotLocal.w;
        wire.head_position[0] = frame.headPosLocal.x;
        wire.head_position[1] = frame.headPosLocal.y;
        wire.head_position[2] = frame.headPosLocal.z;
    } else {
        wire.head_rotation[3] = 1.0f; // 单位四元数，避免消费者归一化时除零
    }

    std::memcpy(wire.shapes, frame.shapes.data(), sizeof(wire.shapes));
    std::memcpy(wire.shape_valid, frame.shapeValid.data(), sizeof(wire.shape_valid));

    Datagram out;
    static_assert(sizeof(out) == sizeof(wire), "线上帧大小与头文件不一致");
    std::memcpy(out.data(), &wire, sizeof(wire));
    return out;
}

void UdpBinarySink::submit(const FaceFrame& frame, const GazeState& gaze,
                           std::uint64_t now_ns) {
    if (!started_) {
        return;
    }
    if (!throttle_.due(now_ns, config_.min_interval_ns)) {
        ++stats_.suppressed;
        return;
    }

    const Datagram datagram = serialize(frame, gaze, now_ns);
    if (socket_.send_to(std::span<const std::byte>{datagram}, config_.host, config_.port)) {
        ++stats_.messages;
        stats_.bytes += datagram.size();
    } else {
        ++stats_.errors;
    }
}

} // namespace fgb
