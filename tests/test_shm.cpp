// 共享内存输出：写入后按消费者视角读回，验证帧内容与 seqlock 状态。
#include "sinks/shm_sink.hpp"

#include "fgb_shared_frame.h"
#include "test_framework.hpp"

#include <windows.h>

#include <string>

using namespace fgb;

FGB_TEST(shm_sink_writes_readable_frame) {
    ShmSinkConfig config;
    config.name = "fgb-test-" + std::to_string(::GetTickCount64());
    ShmSink sink{config};
    FGB_CHECK(sink.start());

    FaceFrame frame;
    frame.captureTsNs = 111;
    frame.publishTsNs = 222;
    frame.sequence    = 7;
    frame.add_channel(channel::kExpression);
    frame.add_channel(channel::kEyes);
    frame.add_channel(channel::kHead);
    frame.set_shape(Shape::JawOpen, 0.75f);
    frame.eyes.left.valid    = true;
    frame.eyes.left.openness = 0.4f;
    frame.hasHeadPose        = true;
    frame.headRotLocal       = Quat{0.0f, 0.1f, 0.0f, 0.99f};

    GazeState gaze;
    gaze.valid      = true;
    gaze.pitch_deg  = 5.0f;
    gaze.yaw_deg    = -3.0f;
    gaze.confidence = 1.0f;

    sink.submit(frame, gaze, 333);
    FGB_CHECK(sink.stats().messages == 1);

    const std::wstring name(sink.mapping_name().begin(), sink.mapping_name().end());
    HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    FGB_CHECK(mapping != nullptr);
    if (mapping != nullptr) {
        auto* block = static_cast<const FgbSharedBlockV1*>(
            ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(FgbSharedBlockV1)));
        FGB_CHECK(block != nullptr);
        if (block != nullptr) {
            FGB_CHECK(block->magic == FGB_SHM_MAGIC);
            FGB_CHECK(block->version == FGB_SHM_VERSION);
            FGB_CHECK(block->struct_size == sizeof(FgbSharedBlockV1));
            FGB_CHECK(block->slot_count == FGB_SHM_SLOTS);
            FGB_CHECK((block->sequence & 1u) == 0);
            const std::uint32_t slot = block->active_slot;
            FGB_CHECK(slot < FGB_SHM_SLOTS);
            const FgbSharedFrameV1& out = block->slots[slot];
            FGB_CHECK(out.channels == (channel::kExpression | channel::kEyes | channel::kHead));
            FGB_CHECK((out.attributes & FGB_SHM_ATTR_LEFT_VALID) != 0);
            FGB_CHECK((out.attributes & FGB_SHM_ATTR_HAS_HEAD) != 0);
            FGB_CHECK(out.capture_ts_ns == 111);
            FGB_CHECK(out.publish_ts_ns == 222);
            FGB_CHECK(out.left_openness > 0.39f && out.left_openness < 0.41f);
            FGB_CHECK(out.gaze_yaw_deg < -2.9f && out.gaze_yaw_deg > -3.1f);
            const std::size_t jaw = index_of(Shape::JawOpen);
            FGB_CHECK(jaw < FGB_SHM_MAX_SHAPES);
            FGB_CHECK(out.shapes[jaw] > 0.74f && out.shapes[jaw] < 0.76f);
            FGB_CHECK(out.shape_valid[jaw] != 0);
            ::UnmapViewOfFile(block);
        }
        ::CloseHandle(mapping);
    }

    sink.stop();
    HANDLE after = ::OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    FGB_CHECK(after == nullptr); // stop 后映射应关闭
    if (after != nullptr) {
        ::CloseHandle(after);
    }
}
