// 示例原生模块：只提供独立注视（ADR-0030 的“注视 B”）。
//
// 不写 openness/pupil 属性，所以它永远不会接管头像的睁眼度。
// 它用于验证 gaze 通道、100 ms 独立过期，以及“头像用 A、渲染用 B”。
//
// 环境变量 FGB_SAMPLE_GAZE_POINTS_ONLY=1 时只置点位属性：
// 这是“2D-only 模块”的参考写法（PICO Connect 私有通道就是这一类），
// 单测用它验证方向与点位不互相连坐。
#include "fgb_module_api.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {

using Clock = std::chrono::steady_clock;

struct Instance {
    const FgbHostServices* host = nullptr;
    Clock::time_point start{};
    bool points_only = false;
};

bool read_points_only() {
    char buffer[8]{};
    std::size_t length = 0;
    if (getenv_s(&length, buffer, sizeof(buffer), "FGB_SAMPLE_GAZE_POINTS_ONLY") != 0) {
        return false;
    }
    return length > 1 && buffer[0] != '\0' && buffer[0] != '0';
}

int initialize(void** out, const FgbHostServices* host, const FgbInitParams* params) {
    (void)params;
    if (out == nullptr || host == nullptr || host->ctx == nullptr) {
        return 0;
    }
    auto* self       = new Instance();
    self->host       = host;
    self->start      = Clock::now();
    self->points_only = read_points_only();
    *out             = self;
    host->report_status(host->ctx, FGB_STATUS_ACTIVE, "");
    return 1;
}

void update(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    FgbFrameSlot* slot = self->host->acquire_frame_slot(self->host->ctx);
    if (slot == nullptr) {
        return;
    }
    if (slot->struct_size < sizeof(FgbFrameSlot)) {
        return;
    }

    const double t       = std::chrono::duration<double>(Clock::now() - self->start).count();
    const double yaw_deg = 9.0 * std::sin(t * 0.7);
    const double pitch_deg = 5.0 * std::sin(t * 1.1);
    const double yaw     = yaw_deg * 0.017453292519943295;
    const double pitch   = pitch_deg * 0.017453292519943295;
    const double cp      = std::cos(pitch);
    const float dir[3]   = {static_cast<float>(-std::sin(yaw) * cp),
                            static_cast<float>(std::sin(pitch)),
                            static_cast<float>(-std::cos(yaw) * cp)};

    if (self->points_only) {
        // 没有方向模型时只置点位属性：伪造一个 -Z 会让注视/扫视判定永远"注视中"。
        // 同时声明 POINT_VIEWPORT：这里的合成点就是按视口归一化产生的。
        slot->attributes = FGB_ATTR_LEFT_POINT | FGB_ATTR_RIGHT_POINT | FGB_ATTR_POINT_VIEWPORT;
    } else {
        slot->attributes = FGB_ATTR_LEFT_GAZE | FGB_ATTR_RIGHT_GAZE | FGB_ATTR_COMBINED_GAZE |
                           FGB_ATTR_LEFT_POINT | FGB_ATTR_RIGHT_POINT | FGB_ATTR_POINT_VIEWPORT;
        for (int i = 0; i < 3; ++i) {
            slot->left_eye.gaze_dir[i]     = dir[i];
            slot->right_eye.gaze_dir[i]    = dir[i];
            slot->combined_gaze_dir[i]     = dir[i];
        }
        slot->left_eye.gaze_confidence  = 0.9f;
        slot->right_eye.gaze_confidence = 0.9f;
        slot->combined_gaze_confidence  = 0.9f;
    }
    // 暂定 [0,1] 左上原点；这里只产生一个可读的合成点。
    slot->left_eye.gaze_point_2d[0]  = static_cast<float>(0.5 + 0.05 * std::sin(t * 0.7));
    slot->left_eye.gaze_point_2d[1]  = static_cast<float>(0.5 - 0.05 * std::sin(t * 1.1));
    slot->right_eye.gaze_point_2d[0] = slot->left_eye.gaze_point_2d[0];
    slot->right_eye.gaze_point_2d[1] = slot->left_eye.gaze_point_2d[1];

    const std::uint64_t ts = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
    slot->capture_ts_ns = ts;

    self->host->commit_frame_slot(self->host->ctx, slot, 0);
}

void teardown(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    if (self == nullptr) {
        return;
    }
    self->host->report_status(self->host->ctx, FGB_STATUS_STOPPED, "");
    delete self;
}

const FgbModule kModule = {
    sizeof(FgbModule),
    FGB_MODULE_ABI_V2,
    "com.fgbbridge.sample.gaze",
    "Native Gaze Sample",
    FGB_CAP_GAZE,
    &initialize,
    &update,
    &teardown,
};

} // namespace

extern "C" FGB_MODULE_EXPORT const FgbModule* fgb_module_query_v2(uint32_t host_abi_version) {
    if (host_abi_version != FGB_MODULE_ABI_V2) {
        return nullptr;
    }
    return &kModule;
}
