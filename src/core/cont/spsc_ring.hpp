// 单生产者单消费者无锁环。用于 source 线程 → 合并线程的帧传递。
//
// 设计约束：
//  - 容量必须是 2 的幂，用位与代替取模
//  - head/tail 各自独占一条 cache line，避免伪共享
//  - 用 64 位单调计数判断空满，不做回绕处理（2^64 帧 ≈ 宇宙寿命）
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fgb {

template <typename T, std::size_t N>
class SpscRing {
    static_assert(N >= 2, "容量至少 2");
    static_assert((N & (N - 1)) == 0, "容量必须是 2 的幂");

public:
    static constexpr std::size_t capacity() noexcept { return N; }

    // 仅生产者线程调用
    bool try_push(const T& value) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= N) {
            return false; // 满
        }
        buf_[static_cast<std::size_t>(head & kMask)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 仅消费者线程调用
    bool try_pop(T& out) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // 空
        }
        out = buf_[static_cast<std::size_t>(tail & kMask)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // 任一线程可读，仅用于监控，不做同步保证
    std::size_t size_approx() const noexcept {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(head - tail);
    }

    bool empty() const noexcept { return size_approx() == 0; }

    std::uint64_t pushed_total() const noexcept { return head_.load(std::memory_order_relaxed); }

private:
    static constexpr std::uint64_t kMask = N - 1;

    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    alignas(64) std::array<T, N> buf_{};
};

} // namespace fgb
