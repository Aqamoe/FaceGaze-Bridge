// 示例原生模块：只提供睁眼度/瞳孔，不提供注视方向。
//
// 用它验证属性掩码：宿主能把 openness 与另一个模块的 gaze 方向拼成头像眼睛
// （ADR-0031 的阶段 A/B）。在 v1 的整眼原子能力下这种模块无法存在。
#include "fgb_module_api.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using Clock = std::chrono::steady_clock;

struct Instance {
    const FgbHostServices* host = nullptr;
    Clock::time_point start{};
};

void set_openness(FgbEyeAttributes& eye, float openness, std::uint64_t ts) {
    eye.openness            = openness;
    eye.openness_confidence = 1.0f;
    eye.pupil_diameter_mm   = 3.0f + 0.5f * openness;
    eye.pupil_dilation      = 0.0f;
    eye.openness_ts_ns      = ts;
    eye.pupil_ts_ns         = ts;
}

int initialize(void** out, const FgbHostServices* host, const FgbInitParams* params) {
    (void)params;
    if (out == nullptr || host == nullptr || host->ctx == nullptr) {
        return 0;
    }
    auto* self  = new Instance();
    self->host  = host;
    self->start = Clock::now();
    *out        = self;
    host->report_status(host->ctx, FGB_STATUS_ACTIVE, "");
    return 1;
}

void update(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    FgbFrameSlot* slot = self->host->acquire_frame_slot(self->host->ctx);
    if (slot == nullptr || slot->struct_size < sizeof(FgbFrameSlot)) {
        return;
    }

    const double t = std::chrono::duration<double>(Clock::now() - self->start).count();
    const std::uint64_t ts = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());

    slot->attributes = FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS | FGB_ATTR_LEFT_PUPIL |
                       FGB_ATTR_RIGHT_PUPIL;
    set_openness(slot->left_eye, static_cast<float>(0.55 + 0.35 * std::sin(t * 1.1)), ts);
    set_openness(slot->right_eye, static_cast<float>(0.55 + 0.35 * std::sin(t * 1.1 + 0.8)), ts);
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
    "com.fgbbridge.sample.openness",
    "Native Openness Sample",
    FGB_CAP_EYES,
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
