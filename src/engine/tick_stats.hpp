// 引擎 tick 工作耗时的滑动分布。
//
// 只记"做一帧要多久"（合并 + 后处理 + 扇出），不记休眠/暂停造成的间隔——
// 那不是我们的工作量。p50/p95 用最近 5 秒的样本，平均/最大值用累计值，
// 这样既能看当前是否抖动，也能看长跑里有没有偶发尖峰。
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>

namespace fgb {

class TickStats {
public:
    static constexpr std::size_t kSamples = 600; // 5 秒 @ 120 Hz

    struct Snapshot {
        double avg_us = 0.0;
        double p50_us = 0.0;
        double p95_us = 0.0;
        double max_us = 0.0;
        std::uint64_t samples = 0;
    };

    void record(std::uint64_t ns) noexcept {
        samples_[next_] = ns;
        next_           = (next_ + 1) % kSamples;
        if (count_ < kSamples) {
            ++count_;
        }
        total_ns_ += ns;
        ++total_count_;
        if (ns > max_ns_) {
            max_ns_ = ns;
        }
    }

    Snapshot snapshot() const noexcept {
        Snapshot out;
        out.samples = total_count_;
        if (total_count_ == 0) {
            return out;
        }
        out.avg_us  = static_cast<double>(total_ns_) / static_cast<double>(total_count_) / 1000.0;
        out.max_us  = static_cast<double>(max_ns_) / 1000.0;
        if (count_ == 0) {
            return out;
        }

        std::array<std::uint64_t, kSamples> sorted{};
        std::copy(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(count_),
                  sorted.begin());
        const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(count_);
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_ / 2),
                         middle);
        out.p50_us = static_cast<double>(sorted[count_ / 2]) / 1000.0;
        const std::size_t p95_index =
            std::min(count_ - 1, static_cast<std::size_t>(static_cast<double>(count_) * 0.95));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(p95_index),
                         middle);
        out.p95_us = static_cast<double>(sorted[p95_index]) / 1000.0;
        return out;
    }

    // 休眠/长时间停顿后重置窗口：之前的尖峰已经没有参考价值
    void reset() noexcept {
        next_       = 0;
        count_      = 0;
        total_ns_   = 0;
        total_count_ = 0;
        max_ns_     = 0;
    }

private:
    std::array<std::uint64_t, kSamples> samples_{};
    std::size_t next_    = 0;
    std::size_t count_   = 0;
    std::uint64_t total_ns_    = 0;
    std::uint64_t total_count_ = 0;
    std::uint64_t max_ns_      = 0;
};

} // namespace fgb
