// 示例原生模块（ABI v2）：只提供睁眼度/瞳孔，不提供注视方向。
//
// v1 的 FGB_CAP_EYE 是整眼原子能力，这个模块在 v1 下无法存在。它用来验证
// FgbFrameSlotV2 的属性掩码：宿主能把 openness 与另一个模块的 gaze 拼成头像眼睛。
#include "fgb_module_api.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace {

using Clock = std::chrono::steady_clock;

struct Instance {
    const FgbHostServicesV2* host = nullptr;
    Clock::time_point start{};
};

void set_openness(FgbEyeAttributesV2& eye, float openness, std::uint64_t ts) {
    eye.openness            = openness;
    eye.openness_confidence = 1.0f;
    eye.pupil_diameter_mm   = 3.0f + 0.5f * openness;
    eye.pupil_dilation      = 0.0f;
    eye.openness_ts_ns      = ts;
    eye.pupil_ts_ns         = ts;
    eye.valid = 0; // 宿主以帧的 attributes 为准
}

int initialize(void** out, const FgbHostServicesV2* host, const FgbInitParamsV2* params) {
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
    FgbFrameSlotV2* slot = self->host->acquire_frame_slot(self->host->ctx);
    if (slot == nullptr || slot->struct_size < sizeof(FgbFrameSlotV2)) {
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

const FgbModuleV2 kModule = {
    sizeof(FgbModuleV2),
    FGB_MODULE_ABI_V2,
    "com.fgbbridge.sample.openness",
    "Native Openness Sample",
    FGB_CAP_EYE_ATTRIBUTES,
    &initialize,
    &update,
    &teardown,
};

} // namespace

extern "C" FGB_MODULE_EXPORT const FgbModuleV2* fgb_module_query_v2(uint32_t host_abi_version) {
    if (host_abi_version != FGB_MODULE_ABI_V2) {
        return nullptr;
    }
    return &kModule;
}