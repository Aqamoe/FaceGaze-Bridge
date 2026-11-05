// 日志刷屏夹具：模拟"每帧报同一句话"的坏模块。
// 真机上的 Steam Link 模块就是这样：100 行/秒刷同一个未识别地址，外加一个
// 永远算错的"没有数据"看门狗，一晚上写出 71 MB 的 engine.log（ADR-0046）。
// 这里用它验证宿主的去重闸门真的接上了。
#include "fgb_module_api.h"

#include <cstdio>

namespace {

struct Instance {
    const FgbHostServices* host = nullptr;
    unsigned counter            = 0;
};

int initialize(void** out, const FgbHostServices* host, const FgbInitParams* params) {
    (void)params;
    if (out == nullptr || host == nullptr || host->ctx == nullptr) {
        return 0;
    }
    auto* self = new Instance();
    self->host = host;
    *out       = self;
    host->report_status(host->ctx, FGB_STATUS_ACTIVE, "");
    return 1;
}

void update(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    if (self == nullptr || self->host == nullptr) {
        return;
    }
    // 每帧 8 条日志，内容只差一个递增的计数
    for (int i = 0; i < 8; ++i) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "LOGSPAM 上报了 %u 条无法识别的 OSC 地址（已忽略）：/sl/xrfb/facew/x",
                      ++self->counter);
        self->host->log(self->host->ctx, FGB_LOG_WARN, text);
    }
    // 每帧一条"永远没数据"的错误，数字还每次都变
    char error[160];
    std::snprintf(error, sizeof(error), "LOGSPAM 已 %u 秒没有数据，检查来源应用是否在发送",
                  1844674407u + self->counter);
    self->host->report_status(self->host->ctx, FGB_STATUS_ERROR, error);
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
    "com.fgbbridge.test.logspam",
    "Log Spam Test Module",
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
