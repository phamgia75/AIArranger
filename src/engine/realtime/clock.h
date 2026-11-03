#ifndef AI_ARRANGER_REALTIME_CLOCK_H
#define AI_ARRANGER_REALTIME_CLOCK_H

#include <cstdint>
#include <atomic>

namespace ai_arranger::realtime {

/**
 * Sample-position-based realtime clock.
 *
 * Uses mach_absolute_time() for high-precision timing reference,
 * driven by CoreAudio render callback sample positions.
 *
 * Architecture rules (ADR-013):
 * - No malloc in realtime path
 * - No mutex in realtime path
 * - No file IO in realtime path
 * - No ObjC/Swift/ARC
 * - mach_absolute_time(), not std::chrono, in callback path
 */

class RealtimeClock {
public:
    static constexpr uint32_t kDefaultSampleRate = 48000;
    static constexpr uint32_t kDefaultResolution = 480; // ticks per quarter note

    RealtimeClock();

    // ── Configuration (non-realtime) ────────────────────────────────
    void setSampleRate(uint32_t sampleRate) noexcept;
    void setTempo(uint32_t bpm) noexcept;
    void setResolution(uint32_t ticksPerQuarter) noexcept;
    void setTimeSignature(uint32_t beatsPerBar, uint32_t beatNote) noexcept;

    // ── Realtime-safe ───────────────────────────────────────────────
    void reset() noexcept;
    void advance(uint32_t numSamples) noexcept;
    void stop() noexcept;
    void start() noexcept;

    // ── mach_absolute_time reference (call from CoreAudio callback) ─
    void setMachTime(uint64_t machTime) noexcept;
    uint64_t getMachTime() const noexcept;
    uint64_t getMachTimeNs() const noexcept;

    // ── Read-only queries (realtime-safe) ───────────────────────────
    int64_t  getPosition() const noexcept;
    int64_t  getSamplePosition() const noexcept;
    uint32_t getTempo() const noexcept;
    double   getTicksPerSample() const noexcept;
    bool     isRunning() const noexcept;
    uint32_t getSampleRate() const noexcept;
    uint32_t getBeatsPerBar() const noexcept;

    // ── Bar/beat helpers (realtime-safe) ────────────────────────────
    int32_t getCurrentBar() const noexcept;
    int32_t getCurrentBeat() const noexcept;
    int32_t getCurrentBarFromTick(int64_t tick) const noexcept;
    int32_t getCurrentBeatFromTick(int64_t tick) const noexcept;

    // ── Section boundary helpers ────────────────────────────────────
    bool isFirstBeatOfBar() const noexcept;
    int64_t getNextBarTick() const noexcept;
    int64_t ticksPerBar() const noexcept;
    int64_t ticksPerBeat() const noexcept;

private:
    // Lock-free state (atomic for cross-thread visibility)
    std::atomic<uint32_t> sample_rate_{kDefaultSampleRate};
    std::atomic<uint32_t> tempo_bpm_{120};
    std::atomic<uint32_t> resolution_{kDefaultResolution};
    std::atomic<uint32_t> beats_per_bar_{4};
    std::atomic<uint32_t> beat_note_{4};
    std::atomic<int64_t>  sample_position_{0};
    std::atomic<bool>     running_{false};

    // mach_absolute_time reference
    std::atomic<uint64_t> last_mach_time_{0};

    // Derived (recalc on config change)
    mutable std::atomic<double>  ticks_per_sample_{0.0};   // mutable for cached get
    mutable std::atomic<int64_t> ticks_per_bar_cache_{0};
    mutable std::atomic<int64_t> ticks_per_beat_cache_{0};
    mutable std::atomic<int64_t> samples_per_beat_{0};
    std::atomic<uint64_t>        config_generation_{0};
    mutable std::atomic<uint64_t> cache_generation_{0};

    void recalcDerived() noexcept;
    void ensureCache() const noexcept;
};

} // namespace ai_arranger::realtime
#endif // AI_ARRANGER_REALTIME_CLOCK_H
