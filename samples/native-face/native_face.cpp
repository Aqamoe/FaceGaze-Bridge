// 示例原生模块：只提供表情（ADR-0030 的“面捕 A”）。
//
// 与 native-sine 的区别：它没有眼睛/头姿通道。用来验证“面捕 A + 注视 B”
// 的组合不会让 A 被误判成眼追来源，也不会让 B 的注视接管头像眼睛。
#include "fgb_module_api.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace {

using Clock = std::chrono::steady_clock;

struct Instance {
    const FgbHostServicesV1* host = nullptr;
    Clock::time_point start{};
    std::int32_t jaw = -1;
    std::int32_t brow_left = -1;
    std::int32_t brow_right = -1;
};

std::int32_t shape(Instance* self, const char* name) {
    return self->host->shape_index(self->host->ctx, name);
}

int initialize(void** out, const FgbHostServicesV1* host, const FgbInitParamsV1* params) {
    (void)params;
    if (out == nullptr || host == nullptr || host->ctx == nullptr) {
        return 0;
    }
    auto* self         = new Instance();
    self->host         = host;
    self->start        = Clock::now();
    self->jaw          = shape(self, "JawOpen");
    self->brow_left    = shape(self, "BrowInnerUpLeft");
    self->brow_right   = shape(self, "BrowInnerUpRight");
    *out               = self;
    host->report_status(host->ctx, FGB_STATUS_ACTIVE, "");
    return 1;
}

void update(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    const double t = std::chrono::duration<double>(Clock::now() - self->start).count();

    FgbFrameSlotV1* slot = self->host->acquire_frame_slot(self->host->ctx);
    if (slot == nullptr) {
        return;
    }

    const float jaw = static_cast<float>(0.5 + 0.5 * std::sin(t * 1.4));
    if (self->jaw >= 0) {
        slot->shapes[self->jaw]      = jaw;
        slot->shape_valid[self->jaw] = 1;
    }
    if (self->brow_left >= 0) {
        slot->shapes[self->brow_left]      = static_cast<float>(0.2 + 0.2 * std::sin(t));
        slot->shape_valid[self->brow_left] = 1;
    }
    if (self->brow_right >= 0) {
        slot->shapes[self->brow_right]      = static_cast<float>(0.2 + 0.2 * std::sin(t + 0.6));
        slot->shape_valid[self->brow_right] = 1;
    }

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

const FgbModuleV1 kModule = {
    sizeof(FgbModuleV1),
    FGB_MODULE_ABI_V1,
    "com.fgbbridge.sample.face",
    "Native Face Sample",
    FGB_CAP_EXPRESSION,
    &initialize,
    &update,
    &teardown,
};

} // namespace

extern "C" FGB_MODULE_EXPORT const FgbModuleV1* fgb_module_query(uint32_t host_abi_version) {
    if (host_abi_version != FGB_MODULE_ABI_V1) {
        return nullptr;
    }
    return &kModule;
}
