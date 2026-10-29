#include "base/process_stats.hpp"

#include "base/clock.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace fgb {
namespace {
#ifdef _WIN32
std::uint64_t filetime_ns(const FILETIME& time) noexcept {
    ULARGE_INTEGER value;
    value.LowPart  = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart * 100ULL; // FILETIME 单位 100 ns
}
#endif
} // namespace

ProcessStats ProcessSampler::sample() noexcept {
    ProcessStats stats;
#ifdef _WIN32
    const HANDLE process = ::GetCurrentProcess();

    PROCESS_MEMORY_COUNTERS memory{};
    if (::GetProcessMemoryInfo(process, &memory, sizeof(memory)) != FALSE) {
        stats.working_set_mb = static_cast<double>(memory.WorkingSetSize) / (1024.0 * 1024.0);
    }
    DWORD handles = 0;
    if (::GetProcessHandleCount(process, &handles) != FALSE) {
        stats.handles = handles;
    }

    FILETIME creation{}, exit{}, kernel{}, user{};
    if (::GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE) {
        const std::uint64_t cpu_ns = filetime_ns(kernel) + filetime_ns(user);
        const std::uint64_t wall_ns = clock::now_ns();
        if (has_previous_ && wall_ns > last_wall_ns_) {
            const double cpu_delta  = static_cast<double>(cpu_ns - last_cpu_ns_);
            const double wall_delta = static_cast<double>(wall_ns - last_wall_ns_);
            stats.cpu_percent       = 100.0 * cpu_delta / wall_delta;
            if (stats.cpu_percent < 0.0) {
                stats.cpu_percent = 0.0;
            }
        }
        last_cpu_ns_  = cpu_ns;
        last_wall_ns_ = wall_ns;
        has_previous_ = true;
    }
#endif
    return stats;
}

} // namespace fgb
