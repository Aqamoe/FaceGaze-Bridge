// 单写多读的快照发布（seqlock）。
//
// 为什么不用 mutex：Sink 线程若被 UI 线程抢锁 1 ms，就会造成一次输出抖动。
// 对观感来说，抖动比延迟更致命。seqlock 让写者永不阻塞、读者最坏重试。
//
// 注意：data_ 是普通成员，读写并发在 C++ 内存模型下严格说是数据竞争。
// 这是 seqlock 的固有代价（业界一致做法，见 Boehm 的 "Can seqlocks get along
// with programming language memory models?"）。这里用 fence 保证可见性与顺序，
// 并保证读到的数据要么完整、要么被校验和丢弃。
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace fgb {

template <typename T>
class SeqLock {
public:
    // 仅写者线程调用
    void publish(const T& value) noexcept {
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed); // 奇数 = 写入中
        std::atomic_thread_fence(std::memory_order_release);
        data_ = value;
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_relaxed); // 偶数 = 稳定
    }

    // 任意读者线程调用。返回 false 表示竞争过于激烈，本帧放弃。
    bool read(T& out) const noexcept {
        for (int attempt = 0; attempt < 16; ++attempt) {
            const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) {
                std::this_thread::yield();
                continue;
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            out = data_;
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::uint64_t s2 = seq_.load(std::memory_order_relaxed);
            if (s1 == s2) {
                return true;
            }
        }
        return false;
    }

    std::uint64_t version() const noexcept { return seq_.load(std::memory_order_acquire); }

private:
    alignas(64) mutable std::atomic<std::uint64_t> seq_{0};
    T data_{};
};

} // namespace fgb
