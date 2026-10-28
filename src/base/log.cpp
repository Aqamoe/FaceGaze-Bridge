#include "base/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace fgb::log {
namespace {

std::atomic<Level> g_level{Level::info};
std::mutex g_mutex;
std::mutex g_callback_mutex;
Callback g_callback;
// 回调里再打日志会无限递归；用 thread_local 挡一层
thread_local bool t_in_callback = false;

constexpr const char* level_name(Level l) noexcept {
    switch (l) {
    case Level::trace:
        return "TRACE";
    case Level::debug:
        return "DEBUG";
    case Level::info:
        return "INFO ";
    case Level::warn:
        return "WARN ";
    case Level::error:
        return "ERROR";
    default:
        return "?????";
    }
}

} // namespace

void set_level(Level l) noexcept { g_level.store(l, std::memory_order_relaxed); }

Level level() noexcept { return g_level.load(std::memory_order_relaxed); }

bool enabled(Level l) noexcept {
    const Level cur = level();
    return cur != Level::off && static_cast<int>(l) >= static_cast<int>(cur);
}

void write(Level l, std::string_view tag, const char* fmt, ...) noexcept {
    if (!enabled(l)) {
        return;
    }

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    // 单次 fprintf 保证一行不被其他线程拆开
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::fprintf(stderr, "[%02d:%02d:%02d.%03d] %s %.*s: %s\n", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, static_cast<int>(ms), level_name(l),
                     static_cast<int>(tag.size()), tag.data(), msg);
    }

    // 回调在锁外调用：订阅者可能自己加锁，跟 fprintf 抢同一把锁会拖慢日志
    if (t_in_callback) {
        return;
    }
    try {
        Callback callback;
        {
            std::lock_guard<std::mutex> lock(g_callback_mutex);
            callback = g_callback;
        }
        if (callback) {
            t_in_callback = true;
            callback(l, tag, std::string_view{msg});
            t_in_callback = false;
        }
    } catch (...) {
        t_in_callback = false;
    }
}

void set_callback(Callback callback) {
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    g_callback = std::move(callback);
}

} // namespace fgb::log
