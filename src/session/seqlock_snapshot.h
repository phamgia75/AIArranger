#ifndef AI_ARRANGER_SESSION_SEQLOCK_SNAPSHOT_H
#define AI_ARRANGER_SESSION_SEQLOCK_SNAPSHOT_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// ── Lock-free snapshot publication (seqlock over atomic words) ────────────────
//
// Replaces std::atomic<T> for a trivially-copyable T that is too large to be
// lock-free (std::atomic<EngineSnapshot> is mutex-backed). The engine thread —
// which a product host may drive from the audio render callback — must never
// take a lock to publish; UI threads poll at 30-60 Hz from anywhere.
//
//  - Readers: wait-free unless a publish is in flight, then retry (bounded by
//    the writer's copy of a few words). Never block the writer.
//  - Writers: serialised by an atomic_flag. tryStore() never waits (the RT
//    tick uses it and simply skips a publish if another writer is mid-copy; the
//    next tick republishes). store() spins for the non-RT lifecycle path.
//  - The payload lives in std::atomic<uint64_t> words, so there is no data race
//    in the C++ memory model, unlike a plain-memcpy seqlock. Ordering uses only
//    release word stores / acquire word loads (no standalone fences), which
//    ThreadSanitizer models exactly:
//      writer: seq=odd (relaxed) -> words (release, so seq=odd is visible to
//              anyone who reads a new word) -> seq=even (release)
//      reader: seq (acquire) -> words (acquire, so the re-check below cannot be
//              hoisted above them) -> seq re-check; retry if odd or changed.

namespace ai_arranger::session {

template <class T>
class SeqlockSnapshot {
    static_assert(std::is_trivially_copyable_v<T>, "snapshot must be trivially copyable");
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "needs lock-free 64-bit atomics");
    static constexpr size_t kWords = (sizeof(T) + sizeof(uint64_t) - 1) / sizeof(uint64_t);

public:
    SeqlockSnapshot() noexcept { write(T{}); }

    SeqlockSnapshot(const SeqlockSnapshot&) = delete;
    SeqlockSnapshot& operator=(const SeqlockSnapshot&) = delete;

    // Publish without ever waiting. Returns false (nothing published) if another
    // writer is mid-publish.
    bool tryStore(const T& v) noexcept {
        if (writer_.test_and_set(std::memory_order_acquire)) return false;
        write(v);
        writer_.clear(std::memory_order_release);
        return true;
    }

    // Publish, spinning while another writer finishes (non-realtime callers).
    void store(const T& v) noexcept {
        while (writer_.test_and_set(std::memory_order_acquire)) {}
        write(v);
        writer_.clear(std::memory_order_release);
    }

    // Consistent copy of the latest published value, from any thread.
    T load() const noexcept {
        uint64_t buf[kWords];
        for (;;) {
            const uint64_t s0 = seq_.load(std::memory_order_acquire);
            if (s0 & 1u) continue;                        // publish in flight
            for (size_t i = 0; i < kWords; ++i)
                buf[i] = words_[i].load(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s0) break;
        }
        T out;
        std::memcpy(&out, buf, sizeof(T));
        return out;
    }

private:
    void write(const T& v) noexcept {
        uint64_t buf[kWords] = {};
        std::memcpy(buf, &v, sizeof(T));
        const uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);      // odd: in flight
        for (size_t i = 0; i < kWords; ++i)
            words_[i].store(buf[i], std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);      // even: stable
    }

    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> words_[kWords] = {};
    std::atomic_flag      writer_ = ATOMIC_FLAG_INIT;
};

} // namespace ai_arranger::session

#endif // AI_ARRANGER_SESSION_SEQLOCK_SNAPSHOT_H
