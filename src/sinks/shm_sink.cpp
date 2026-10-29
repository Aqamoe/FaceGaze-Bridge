#include "sinks/shm_sink.hpp"

#include "base/log.hpp"

#include "fgb_shared_frame.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <cstring>
#include <string_view>

namespace fgb {
namespace {
constexpr char kTag[] = "shm-out";
}

ShmSink::ShmSink(ShmSinkConfig config) : config_(std::move(config)) {
    mapping_name_ = "Local\\FGB_Shm_" + sanitize(config_.name.empty() ? "default" : config_.name);
}

ShmSink::~ShmSink() { stop(); }

std::string ShmSink::sanitize(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') ||
                        (uc >= 'A' && uc <= 'Z') || c == '.' || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    return out.empty() ? "default" : out;
}

bool ShmSink::start() {
#ifdef _WIN32
    if (view_ != nullptr) {
        return true;
    }
    const std::wstring wide(mapping_name_.begin(), mapping_name_.end());
    HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          sizeof(FgbSharedBlockV1), wide.c_str());
    if (mapping == nullptr) {
        FGB_ERROR(kTag, "创建共享内存失败：%s（错误码 %lu）", mapping_name_.c_str(),
                  static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    void* view = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                 sizeof(FgbSharedBlockV1));
    if (view == nullptr) {
        ::CloseHandle(mapping);
        FGB_ERROR(kTag, "映射共享内存失败：%s", mapping_name_.c_str());
        return false;
    }
    mapping_ = mapping;
    view_    = view;

    auto* block = static_cast<FgbSharedBlockV1*>(view_);
    std::memset(block, 0, sizeof(*block));
    block->magic       = FGB_SHM_MAGIC;
    block->version     = FGB_SHM_VERSION;
    block->struct_size = sizeof(FgbSharedBlockV1);
    block->slot_count  = FGB_SHM_SLOTS;
    std::atomic_thread_fence(std::memory_order_release);

    FGB_INFO(kTag, "共享内存输出：%s（%zu 字节，%u 槽）", mapping_name_.c_str(),
             sizeof(FgbSharedBlockV1), static_cast<unsigned>(FGB_SHM_SLOTS));
    return true;
#else
    return false;
#endif
}

void ShmSink::stop() noexcept {
#ifdef _WIN32
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
#else
    view_    = nullptr;
    mapping_ = nullptr;
#endif
}

void ShmSink::write_frame(const FaceFrame& frame, const GazeState& gaze,
                          std::uint64_t now_ns) {
#ifdef _WIN32
    auto* block = static_cast<FgbSharedBlockV1*>(view_);
    std::atomic_ref<std::uint64_t> sequence(block->sequence);
    std::atomic_ref<std::uint32_t> active(block->active_slot);

    const std::uint32_t slot = 1u - active.load(std::memory_order_acquire);
    FgbSharedFrameV1& out    = block->slots[slot];

    sequence.fetch_add(1, std::memory_order_relaxed); // 奇数：写入中

    out.sequence      = ++write_sequence_;
    out.capture_ts_ns = frame.captureTsNs;
    out.publish_ts_ns = frame.publishTsNs != 0 ? frame.publishTsNs : now_ns;
    out.channels      = frame.channels;
    out.attributes    = 0;
    if (frame.eyes.left.valid) {
        out.attributes |= FGB_SHM_ATTR_LEFT_VALID;
    }
    if (frame.eyes.right.valid) {
        out.attributes |= FGB_SHM_ATTR_RIGHT_VALID;
    }
    if (frame.hasHeadPose) {
        out.attributes |= FGB_SHM_ATTR_HAS_HEAD;
    }
    if (gaze.fixation) {
        out.attributes |= FGB_SHM_ATTR_FIXATION;
    }
    out.shape_count = static_cast<std::uint32_t>(kShapeCount);
    out.reserved    = 0;

    out.left_openness             = frame.eyes.left.openness;
    out.left_openness_confidence  = frame.eyes.left.opennessConf;
    out.left_gaze_dir[0]          = frame.eyes.left.gazeDirLocal.x;
    out.left_gaze_dir[1]          = frame.eyes.left.gazeDirLocal.y;
    out.left_gaze_dir[2]          = frame.eyes.left.gazeDirLocal.z;
    out.left_gaze_confidence      = frame.eyes.left.gazeConf;
    out.right_openness            = frame.eyes.right.openness;
    out.right_openness_confidence = frame.eyes.right.opennessConf;
    out.right_gaze_dir[0]         = frame.eyes.right.gazeDirLocal.x;
    out.right_gaze_dir[1]         = frame.eyes.right.gazeDirLocal.y;
    out.right_gaze_dir[2]         = frame.eyes.right.gazeDirLocal.z;
    out.right_gaze_confidence     = frame.eyes.right.gazeConf;
    out.vergence_distance_m       = frame.eyes.vergenceDistanceM;

    out.gaze_dir[0]     = gaze.dir_local.x;
    out.gaze_dir[1]     = gaze.dir_local.y;
    out.gaze_dir[2]     = gaze.dir_local.z;
    out.gaze_pitch_deg  = gaze.pitch_deg;
    out.gaze_yaw_deg    = gaze.yaw_deg;
    out.gaze_confidence = gaze.confidence;

    out.head_rotation[0] = frame.headRotLocal.x;
    out.head_rotation[1] = frame.headRotLocal.y;
    out.head_rotation[2] = frame.headRotLocal.z;
    out.head_rotation[3] = frame.headRotLocal.w;
    out.head_position[0] = frame.headPosLocal.x;
    out.head_position[1] = frame.headPosLocal.y;
    out.head_position[2] = frame.headPosLocal.z;

    std::memcpy(out.shapes, frame.shapes.data(), sizeof(out.shapes));
    std::memcpy(out.shape_valid, frame.shapeValid.data(), sizeof(out.shape_valid));

    sequence.fetch_add(1, std::memory_order_release); // 偶数：写完
    active.store(slot, std::memory_order_release);
    stats_.messages += 1;
    stats_.bytes += sizeof(FgbSharedFrameV1);
#else
    (void)frame;
    (void)gaze;
    (void)now_ns;
#endif
}

void ShmSink::submit(const FaceFrame& frame, const GazeState& gaze,
                     std::uint64_t now_ns) {
    if (view_ == nullptr) {
        return;
    }
    if (config_.min_interval_ns != 0 && last_submit_ns_ != 0 &&
        now_ns - last_submit_ns_ < config_.min_interval_ns) {
        stats_.suppressed += 1;
        return;
    }
    last_submit_ns_ = now_ns;
    write_frame(frame, gaze, now_ns);
}

} // namespace fgb
