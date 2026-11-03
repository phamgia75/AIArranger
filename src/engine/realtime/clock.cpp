#include "engine/realtime/clock.h"
#include <cmath>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace ai_arranger::realtime {

namespace {
// Host-time -> ns ratio. On Apple the host time is mach_absolute_time() ticks
// (CoreAudio's time base). Elsewhere the host time fed to setMachTime() is
// already nanoseconds, so the ratio is 1/1. Kept out of the header so the
// portable core's public surface includes no Darwin header.
struct HostTimebase { uint32_t numer; uint32_t denom; };
const HostTimebase sTimebaseInfo = []() {
#if defined(__APPLE__)
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    if (info.denom == 0) return HostTimebase{1, 1};
    return HostTimebase{info.numer, info.denom};
#else
    return HostTimebase{1, 1};
#endif
}();
} // namespace

RealtimeClock::RealtimeClock() {
    recalcDerived();
}

void RealtimeClock::setSampleRate(uint32_t sampleRate) noexcept {
    sample_rate_.store(sampleRate, std::memory_order_relaxed);
    config_generation_.fetch_add(1, std::memory_order_release);
    recalcDerived();
}

void RealtimeClock::setTempo(uint32_t bpm) noexcept {
    tempo_bpm_.store(bpm, std::memory_order_relaxed);
    config_generation_.fetch_add(1, std::memory_order_release);
    recalcDerived();
}

void RealtimeClock::setResolution(uint32_t ticksPerQuarter) noexcept {
    resolution_.store(ticksPerQuarter, std::memory_order_relaxed);
    config_generation_.fetch_add(1, std::memory_order_release);
    recalcDerived();
}

void RealtimeClock::setTimeSignature(uint32_t beatsPerBar, uint32_t beatNote) noexcept {
    beats_per_bar_.store(beatsPerBar, std::memory_order_relaxed);
    beat_note_.store(beatNote, std::memory_order_relaxed);
    config_generation_.fetch_add(1, std::memory_order_release);
    recalcDerived();
}

void RealtimeClock::reset() noexcept {
    sample_position_.store(0, std::memory_order_release);
    last_mach_time_.store(0, std::memory_order_release);
    cache_generation_.store(0, std::memory_order_release);
}

void RealtimeClock::advance(uint32_t numSamples) noexcept {
    if (!running_.load(std::memory_order_acquire)) return;
    sample_position_.fetch_add(numSamples, std::memory_order_acq_rel);
    cache_generation_.store(0, std::memory_order_release); // invalidate
}

void RealtimeClock::setMachTime(uint64_t machTime) noexcept {
    last_mach_time_.store(machTime, std::memory_order_release);
}

uint64_t RealtimeClock::getMachTime() const noexcept {
    return last_mach_time_.load(std::memory_order_acquire);
}

uint64_t RealtimeClock::getMachTimeNs() const noexcept {
    uint64_t mt = last_mach_time_.load(std::memory_order_acquire);
    return mt * sTimebaseInfo.numer / sTimebaseInfo.denom;
}

void RealtimeClock::stop() noexcept {
    running_.store(false, std::memory_order_release);
}

void RealtimeClock::start() noexcept {
    running_.store(true, std::memory_order_release);
}

int64_t RealtimeClock::getPosition() const noexcept {
    ensureCache();
    double tps = ticks_per_sample_.load(std::memory_order_acquire);
    if (tps <= 0.0) return 0;
    int64_t samples = sample_position_.load(std::memory_order_acquire);
    return static_cast<int64_t>(static_cast<double>(samples) * tps);
}

int64_t RealtimeClock::getSamplePosition() const noexcept {
    return sample_position_.load(std::memory_order_acquire);
}

uint32_t RealtimeClock::getTempo() const noexcept {
    return tempo_bpm_.load(std::memory_order_acquire);
}

double RealtimeClock::getTicksPerSample() const noexcept {
    return ticks_per_sample_.load(std::memory_order_acquire);
}

bool RealtimeClock::isRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

uint32_t RealtimeClock::getSampleRate() const noexcept {
    return sample_rate_.load(std::memory_order_acquire);
}

uint32_t RealtimeClock::getBeatsPerBar() const noexcept {
    return beats_per_bar_.load(std::memory_order_acquire);
}

int32_t RealtimeClock::getCurrentBar() const noexcept {
    return getCurrentBarFromTick(getPosition());
}

int32_t RealtimeClock::getCurrentBeat() const noexcept {
    return getCurrentBeatFromTick(getPosition());
}

int32_t RealtimeClock::getCurrentBarFromTick(int64_t tick) const noexcept {
    auto tpb = ticksPerBar();
    if (tpb <= 0) return 0;
    return static_cast<int32_t>(tick / tpb) + 1;
}

int32_t RealtimeClock::getCurrentBeatFromTick(int64_t tick) const noexcept {
    auto tpb = ticksPerBar();
    auto tpbt = ticksPerBeat();
    if (tpb <= 0 || tpbt <= 0) return 0;
    return static_cast<int32_t>((tick % tpb) / tpbt) + 1;
}

bool RealtimeClock::isFirstBeatOfBar() const noexcept {
    int64_t pos = getPosition();
    return (pos % ticksPerBar()) < ticksPerBeat();
}

int64_t RealtimeClock::getNextBarTick() const noexcept {
    int64_t pos = getPosition();
    auto tpb = ticksPerBar();
    if (tpb <= 0) return 0;
    return ((pos / tpb) + 1) * tpb;
}

int64_t RealtimeClock::ticksPerBar() const noexcept {
    ensureCache();
    return ticks_per_bar_cache_.load(std::memory_order_acquire);
}

int64_t RealtimeClock::ticksPerBeat() const noexcept {
    ensureCache();
    return ticks_per_beat_cache_.load(std::memory_order_acquire);
}

void RealtimeClock::recalcDerived() noexcept {
    uint32_t sr = sample_rate_.load(std::memory_order_relaxed);
    uint32_t bpm = tempo_bpm_.load(std::memory_order_relaxed);
    uint32_t res = resolution_.load(std::memory_order_relaxed);
    uint32_t bpb = beats_per_bar_.load(std::memory_order_relaxed);
    uint32_t bpn = beat_note_.load(std::memory_order_relaxed);

    if (sr == 0 || bpm == 0 || res == 0) {
        ticks_per_sample_.store(0.0, std::memory_order_release);
        ticks_per_bar_cache_.store(0, std::memory_order_release);
        ticks_per_beat_cache_.store(0, std::memory_order_release);
        samples_per_beat_.store(0, std::memory_order_release);
        return;
    }

    // ticks per sample = resolution / (sampleRate * 60 / bpm)
    double tps = static_cast<double>(res) / (static_cast<double>(sr) * 60.0 / static_cast<double>(bpm));
    ticks_per_sample_.store(tps, std::memory_order_release);

    // Beat = the time-signature denominator note (quarter = res ticks): 6/8
    // bars are 6 eighths = 3 quarters. 4/4 is unchanged (res, 4 x res).
    int64_t ticksPerBeatVal = (bpn > 0) ? static_cast<int64_t>(res) * 4 / bpn : res;
    int64_t ticksPerBarVal = ticksPerBeatVal * bpb;

    ticks_per_bar_cache_.store(ticksPerBarVal, std::memory_order_release);
    ticks_per_beat_cache_.store(ticksPerBeatVal, std::memory_order_release);

    int64_t spb = static_cast<int64_t>(static_cast<double>(sr) * 60.0 * static_cast<double>(bpn) / (static_cast<double>(bpm) * 4.0));
    samples_per_beat_.store(spb, std::memory_order_release);
}

void RealtimeClock::ensureCache() const noexcept {
    uint64_t gen = config_generation_.load(std::memory_order_acquire);
    uint64_t cached = cache_generation_.load(std::memory_order_acquire);
    if (gen != cached) {
        // Recalculate cached values
        const_cast<RealtimeClock*>(this)->recalcDerived();
        cache_generation_.store(gen, std::memory_order_release);
    }
}

} // namespace ai_arranger::realtime
