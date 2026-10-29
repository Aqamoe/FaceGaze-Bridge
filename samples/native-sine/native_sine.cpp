// 示例原生模块：不需要任何硬件，按正弦波产生表情/眼追/头姿。
//
// 它的用途有三个：
//   1. ABI 的唯一可执行参考实现
//   2. CI / 本机无硬件时验证原生模块链路
//   3. 模块作者可以从这里抄骨架（初始化、按名取形状下标、提交帧）
#include "fgb_module_api.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;

struct Instance {
    const FgbHostServicesV1* host = nullptr;
    Clock::time_point start{};
    std::int32_t jaw = -1;
    std::int32_t squint_left = -1;
    std::int32_t squint_right = -1;
    std::int32_t brow_left = -1;
    std::int32_t brow_right = -1;
    std::uint64_t frames = 0;
};

int32_t shape(Instance* self, const char* name) {
    return self->host->shape_index(self->host->ctx, name);
}

int initialize(void** out, const FgbHostServicesV1* host, const FgbInitParamsV1* params) {
    if (out == nullptr || host == nullptr || host->ctx == nullptr) {
        return 0;
    }
    auto* self          = new Instance();
    self->host          = host;
    self->start         = Clock::now();
    self->jaw           = shape(self, "JawOpen");
    self->squint_left   = shape(self, "EyeSquintLeft");
    self->squint_right  = shape(self, "EyeSquintRight");
    self->brow_left     = shape(self, "BrowInnerUpLeft");
    self->brow_right    = shape(self, "BrowInnerUpRight");
    *out                = self;

    host->log(host->ctx, FGB_LOG_INFO, "sine sample initialized");
    if (params != nullptr && params->module_dir != nullptr) {
        host->log(host->ctx, FGB_LOG_DEBUG, params->module_dir);
    }
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

    const float jaw_open = static_cast<float>(0.5 + 0.5 * std::sin(t * 1.5));
    if (self->jaw >= 0) {
        slot->shapes[self->jaw]     = jaw_open;
        slot->shape_valid[self->jaw] = 1;
    }
    if (self->squint_left >= 0) {
        slot->shapes[self->squint_left] =
            static_cast<float>(0.2 + 0.2 * std::sin(t * 1.2));
        slot->shape_valid[self->squint_left] = 1;
    }
    if (self->squint_right >= 0) {
        slot->shapes[self->squint_right] =
            static_cast<float>(0.2 + 0.2 * std::sin(t * 1.2 + 0.7));
        slot->shape_valid[self->squint_right] = 1;
    }
    if (self->brow_left >= 0) {
        slot->shapes[self->brow_left] = static_cast<float>(0.1 + 0.1 * std::sin(t));
        slot->shape_valid[self->brow_left] = 1;
    }
    if (self->brow_right >= 0) {
        slot->shapes[self->brow_right] = static_cast<float>(0.1 + 0.1 * std::sin(t + 1.0));
        slot->shape_valid[self->brow_right] = 1;
    }

    const float pitch_deg = static_cast<float>(6.0 * std::sin(t * 0.8));
    const float yaw_deg   = static_cast<float>(9.0 * std::sin(t * 0.6));
    const float pitch     = pitch_deg * 0.017453292519943295f;
    const float yaw       = yaw_deg * 0.017453292519943295f;
    const float cp        = std::cos(pitch);
    const float dir[3]    = {-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};

    slot->left_eye.valid                 = 1;
    slot->left_eye.openness              = static_cast<float>(0.5 + 0.4 * std::sin(t * 1.1));
    slot->left_eye.openness_confidence   = 1.0f;
    slot->left_eye.gaze_dir[0]           = dir[0];
    slot->left_eye.gaze_dir[1]           = dir[1];
    slot->left_eye.gaze_dir[2]           = dir[2];
    slot->left_eye.gaze_confidence       = 1.0f;

    slot->right_eye.valid                = 1;
    slot->right_eye.openness             = static_cast<float>(0.5 + 0.4 * std::sin(t * 1.1 + 0.7));
    slot->right_eye.openness_confidence  = 1.0f;
    slot->right_eye.gaze_dir[0]          = dir[0];
    slot->right_eye.gaze_dir[1]          = dir[1];
    slot->right_eye.gaze_dir[2]          = dir[2];
    slot->right_eye.gaze_confidence      = 1.0f;

    const double head_yaw_rad = 0.15 * std::sin(t * 0.4);
    slot->has_head_pose       = 1;
    slot->head_rotation[0]    = 0.0f;
    slot->head_rotation[1]    = static_cast<float>(std::sin(head_yaw_rad / 2.0));
    slot->head_rotation[2]    = 0.0f;
    slot->head_rotation[3]    = static_cast<float>(std::cos(head_yaw_rad / 2.0));

    ++self->frames;
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
    "com.fgbbridge.sample.sine",
    "Sine Sample Module",
    FGB_CAP_EYE | FGB_CAP_EXPRESSION | FGB_CAP_HEAD,
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
