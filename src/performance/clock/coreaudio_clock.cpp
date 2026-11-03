#include "performance/clock/coreaudio_clock.h"
#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <ctime>
#endif

namespace ai_arranger::performance {

namespace {
struct HostTimebase { uint32_t numer; uint32_t denom; };

// Cached host timebase (host ticks -> nanoseconds). Non-Apple hosts read
// CLOCK_MONOTONIC in nanoseconds already, so the ratio is 1/1 there.
const HostTimebase& timebase() {
    static const HostTimebase tb = [] {
#if defined(__APPLE__)
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        if (t.denom == 0) return HostTimebase{1, 1};
        return HostTimebase{t.numer, t.denom};
#else
        return HostTimebase{1, 1};
#endif
    }();
    return tb;
}

uint64_t hostNow() noexcept {
#if defined(__APPLE__)
    return mach_absolute_time();
#else
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
#endif
}
} // namespace

void CoreAudioClock::start() noexcept {
    last_host_.store(hostNow(), std::memory_order_release);
    running_.store(true, std::memory_order_release);
}

uint64_t CoreAudioClock::pollElapsedSamples() noexcept {
    if (!running_.load(std::memory_order_acquire)) return 0;
    const uint64_t now = hostNow();
    const uint64_t prev = last_host_.exchange(now, std::memory_order_acq_rel);
    if (now <= prev) return 0;
    const auto& tb = timebase();
    // host ticks -> nanoseconds -> samples (true elapsed; no fixed-step drift).
    const long double ns =
        static_cast<long double>(now - prev) * tb.numer / tb.denom;
    const long double samples = ns * sample_rate_ / 1'000'000'000.0L;
    return static_cast<uint64_t>(samples);
}

} // namespace ai_arranger::performance
