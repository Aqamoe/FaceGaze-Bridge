// 状态上报夹具：先 ACTIVE，跑十来帧后报一次 ERROR，再过一阵回到 ACTIVE。
// 用来验证"模块报错后自愈"这条路径——宿主模式下错误经共享内存回传，
// 曾经出现"报了错就永远清不掉"的问题（界面一直显示故障，重启实例也救不回来）。
#include "fgb_module_api.h"

#include <cstddef>
#include <cstdint>

namespace {

constexpr int kErrorAfterFrames  = 10; // 第 10 帧开始报错
constexpr int kActiveAfterFrames = 30; // 第 30 帧恢复
constexpr char kErrorMessage[]    = "gaze sample stale (>100 ms)";

struct Instance {
    const FgbHostServices* host = nullptr;
    int frames                  = 0;
    bool reporting_error        = false;
};

int initialize(void** out, const FgbHostServices* host, const FgbInitParams* params) {
    (void)params;
    if (out == nullptr || host == nullptr || host->ctx == nullptr) {
        return 0;
    }
    auto* self  = new Instance();
    self->host  = host;
    *out        = self;
    host->report_status(host->ctx, FGB_STATUS_ACTIVE, "");
    return 1;
}

void update(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    ++self->frames;

    // 语义：只有"没有新样本"时才报错，恢复后必须再报一次 ACTIVE
    if (!self->reporting_error && self->frames >= kErrorAfterFrames) {
        self->reporting_error = true;
        self->host->report_status(self->host->ctx, FGB_STATUS_ERROR, kErrorMessage);
    } else if (self->reporting_error && self->frames >= kActiveAfterFrames) {
        self->reporting_error = false;
        self->host->report_status(self->host->ctx, FGB_STATUS_ACTIVE, "");
    }

    FgbFrameSlot* slot = self->host->acquire_frame_slot(self->host->ctx);
    if (slot == nullptr || slot->struct_size < sizeof(FgbFrameSlot)) {
        return;
    }
    slot->attributes   = FGB_ATTR_EXPRESSION;
    slot->shapes[0]    = 0.5f;
    slot->shape_valid[0] = 1;
    slot->capture_ts_ns  = 0; // 宿主补时间戳
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
    "com.fgbbridge.test.statuscycle",
    "Status Cycle Test Module",
    FGB_CAP_EXPRESSION,
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
