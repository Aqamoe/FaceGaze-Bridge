// 性能指标与休眠恢复的单测。
//
// tick 耗时是给用户/排障看的，不需要精确到 ns，但分位数不能用错：
// p50/p95 应该落在样本范围内，且 p95 >= p50。
#include "base/clock.hpp"
#include "base/process_stats.hpp"
#include "engine/engine.hpp"
#include "engine/tick_stats.hpp"

#include "test_framework.hpp"

using namespace fgb;

FGB_TEST(tick_stats_percentiles_are_sane) {
    TickStats stats;
    for (int i = 0; i < 100; ++i) {
        stats.record(static_cast<std::uint64_t>(i + 1) * 1000ULL); // 1..100 µs
    }
    const TickStats::Snapshot snapshot = stats.snapshot();
    FGB_CHECK(snapshot.samples == 100);
    FGB_CHECK_NEAR(snapshot.avg_us, 50.5, 0.1);
    FGB_CHECK(snapshot.p50_us >= 49.0 && snapshot.p50_us <= 52.0);
    FGB_CHECK(snapshot.p95_us >= 94.0 && snapshot.p95_us <= 96.0);
    FGB_CHECK_NEAR(snapshot.max_us, 100.0, 0.1);
    FGB_CHECK(snapshot.p95_us >= snapshot.p50_us);
}

FGB_TEST(tick_stats_ring_keeps_recent_window) {
    TickStats stats;
    for (int i = 0; i < 700; ++i) {
        stats.record(10'000);
    }
    stats.record(1'000'000); // 只有最新样本是 1 ms
    const TickStats::Snapshot snapshot = stats.snapshot();
    FGB_CHECK(snapshot.samples == 701);
    // 600 样本窗口里只有 1 个 1 ms，p50 仍是 10 µs
    FGB_CHECK_NEAR(snapshot.p50_us, 10.0, 0.01);
    FGB_CHECK_NEAR(snapshot.max_us, 1000.0, 0.01);
}

FGB_TEST(engine_reports_resume_after_long_gap) {
    Engine engine;
    engine.tick_once(1'000'000);
    engine.tick_once(1'000'000 + 8'333'333);
    FGB_CHECK(engine.stats().resumes == 0);

    engine.tick_once(1'000'000 + 8'333'333 + 2'000'000'000ULL);
    FGB_CHECK(engine.stats().resumes == 1);
    // 恢复后耗时窗口重新开始：只有这一 tick 的样本
    FGB_CHECK(engine.tick_stats().samples == 1);
}

FGB_TEST(process_sampler_returns_resources) {
    ProcessSampler sampler;
    const ProcessStats first = sampler.sample();
    FGB_CHECK(first.working_set_mb > 0.0);
    FGB_CHECK(first.handles > 0);
    FGB_CHECK_NEAR(first.cpu_percent, 0.0, 0.001); // 第一帧没有基线，固定 0

    clock::sleep_for_ns(20'000'000);
    const ProcessStats second = sampler.sample();
    FGB_CHECK(second.working_set_mb > 0.0);
    FGB_CHECK(second.handles > 0);
    FGB_CHECK(second.cpu_percent >= 0.0);

    // 空转 200 ms 必须能被采样到，否则指标在真机上永远是 0
    const std::uint64_t busy_start = clock::now_ns();
    volatile std::uint64_t sink  = 0;
    while (clock::now_ns() - busy_start < 200'000'000ULL) {
        sink = sink + 1;
    }
    const ProcessStats busy = sampler.sample();
    FGB_CHECK(busy.cpu_percent > 1.0);
}
