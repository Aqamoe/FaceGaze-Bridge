// 进程级资源指标：CPU 占用、工作集、句柄数。
//
// 引擎本身是单线程主循环 + 少量后台线程，CPU% 可能低于 100；休眠/唤醒后
// 第一次采样会因为时间跳变而失真，所以第一帧固定返回 0，调用方按"未就绪"处理。
#pragma once

#include <cstdint>

namespace fgb {

struct ProcessStats {
    double cpu_percent   = 0.0;
    double working_set_mb = 0.0;
    std::uint32_t handles = 0;
};

class ProcessSampler {
public:
    ProcessStats sample() noexcept;

private:
    bool has_previous_        = false;
    std::uint64_t last_cpu_ns_  = 0;
    std::uint64_t last_wall_ns_ = 0;
};

} // namespace fgb
