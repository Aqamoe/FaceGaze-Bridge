#include "core/cont/seqlock.hpp"
#include "core/cont/spsc_ring.hpp"

#include "test_framework.hpp"

#include <cstdint>

using namespace fgb;

namespace {
struct Sample {
    std::uint64_t seq = 0;
    float value       = 0.0f;
};
} // namespace

FGB_TEST(ring_fifo_order) {
    SpscRing<int, 8> ring;
    FGB_CHECK(ring.empty());

    for (int i = 0; i < 5; ++i) {
        FGB_CHECK(ring.try_push(i));
    }
    FGB_CHECK(ring.size_approx() == 5);

    for (int i = 0; i < 5; ++i) {
        int out = -1;
        FGB_CHECK(ring.try_pop(out));
        FGB_CHECK(out == i);
    }
    FGB_CHECK(ring.empty());

    int out = 0;
    FGB_CHECK(!ring.try_pop(out));
}

FGB_TEST(ring_capacity_limit) {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        FGB_CHECK(ring.try_push(i));
    }
    FGB_CHECK(!ring.try_push(99)); // 满

    int out = 0;
    FGB_CHECK(ring.try_pop(out));
    FGB_CHECK(ring.try_push(4)); // 腾出一格后可写
}

FGB_TEST(ring_wraparound) {
    SpscRing<int, 4> ring;
    int expected = 0;
    for (int round = 0; round < 100; ++round) {
        FGB_CHECK(ring.try_push(expected++));
        FGB_CHECK(ring.try_push(expected++));
        int a = 0;
        int b = 0;
        FGB_CHECK(ring.try_pop(a));
        FGB_CHECK(ring.try_pop(b));
        FGB_CHECK(a == expected - 2);
        FGB_CHECK(b == expected - 1);
    }
}

FGB_TEST(seqlock_roundtrip) {
    SeqLock<Sample> lock;
    Sample in;
    in.seq   = 7;
    in.value = 1.25f;
    lock.publish(in);

    Sample out;
    FGB_CHECK(lock.read(out));
    FGB_CHECK(out.seq == 7);
    FGB_CHECK_NEAR(out.value, 1.25, 1e-6);

    FGB_CHECK(lock.version() % 2 == 0);
}

FGB_TEST(seqlock_repeated_publish_keeps_latest) {
    SeqLock<Sample> lock;
    for (std::uint64_t i = 1; i <= 50; ++i) {
        Sample s;
        s.seq   = i;
        s.value = static_cast<float>(i) * 0.5f;
        lock.publish(s);
    }
    Sample out;
    FGB_CHECK(lock.read(out));
    FGB_CHECK(out.seq == 50);
    FGB_CHECK_NEAR(out.value, 25.0, 1e-5);
}
