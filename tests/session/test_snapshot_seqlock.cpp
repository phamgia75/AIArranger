// Snapshot publication: lock-free for the engine thread, never torn for readers.
//
// The Gate 4 contract lets a host drive tick() from the audio render callback,
// and tick() publishes the UI snapshot. std::atomic<EngineSnapshot> is
// mutex-backed at this size, i.e. a lock on the realtime path. SeqlockSnapshot
// replaces it; this test pins its properties (run under TSan in CI):
//   - every read is internally consistent (all fields from one publish),
//   - reads are monotonic for a monotonic writer,
//   - tryStore never waits and fails only while another writer is mid-publish,
//   - the facade's snapshot still round-trips lifecycle/tempo state.

#include "session/seqlock_snapshot.h"
#include "session/engine_snapshot.h"
#include "session/live_engine_facade.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ai_arranger;
using session::EngineSnapshot;
using session::SeqlockSnapshot;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

static EngineSnapshot make(uint64_t k) {
    EngineSnapshot s;
    s.playing          = (k & 1u) != 0;
    s.chordRoot        = static_cast<uint8_t>(k % 128);
    s.section          = static_cast<int32_t>(k % 7);
    s.tempoBpm         = static_cast<uint32_t>(40 + k % 200);
    s.positionTicks    = static_cast<int64_t>(k);
    s.receivedMessages = k * 3;
    s.dispatchedNotes  = ~k;
    return s;
}
static bool consistent(const EngineSnapshot& s) {
    const uint64_t k = static_cast<uint64_t>(s.positionTicks);
    const EngineSnapshot w = make(k);
    return s.playing == w.playing && s.chordRoot == w.chordRoot && s.section == w.section &&
           s.tempoBpm == w.tempoBpm && s.receivedMessages == w.receivedMessages &&
           s.dispatchedNotes == w.dispatchedNotes;
}

int main() {
    std::printf("Test: seqlock snapshot publication\n");

    {
        SeqlockSnapshot<EngineSnapshot> snap;
        const EngineSnapshot d = snap.load();
        TEST("default value readable before any publish", d.tempoBpm == 120 && d.positionTicks == 0);
        snap.store(make(42));
        TEST("store/load round-trips", consistent(snap.load()) && snap.load().positionTicks == 42);
        TEST("tryStore succeeds uncontended", snap.tryStore(make(43)) && snap.load().positionTicks == 43);
    }

    // One RT-style writer (tryStore) + one lifecycle writer (store) + 3 readers.
    {
        SeqlockSnapshot<EngineSnapshot> snap;
        snap.store(make(0));
        std::atomic<bool> done{false};
        std::atomic<uint64_t> torn{0}, backwards{0}, reads{0}, skipped{0};

        std::thread rt([&] {
            for (uint64_t k = 1; k <= 400000; ++k)
                if (!snap.tryStore(make(k * 2))) skipped.fetch_add(1, std::memory_order_relaxed);
            done.store(true, std::memory_order_release);
        });
        std::thread lifecycle([&] {
            while (!done.load(std::memory_order_acquire)) {
                const auto cur = snap.load();
                snap.store(cur);   // republish an already-consistent value
            }
        });
        std::vector<std::thread> readers;
        for (int r = 0; r < 3; ++r) {
            readers.emplace_back([&] {
                int64_t last = -1;
                while (!done.load(std::memory_order_acquire)) {
                    const auto s = snap.load();
                    if (!consistent(s)) torn.fetch_add(1, std::memory_order_relaxed);
                    if (s.positionTicks < last) backwards.fetch_add(1, std::memory_order_relaxed);
                    // The lifecycle writer can republish an older value it just
                    // read, so only the RT-only phase below asserts monotonicity.
                    last = s.positionTicks;
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        rt.join(); lifecycle.join();
        for (auto& t : readers) t.join();
        TEST("concurrent: no torn snapshot under 2 writers + 3 readers", torn.load() == 0);
        TEST("concurrent: readers made progress", reads.load() > 0);
        std::printf("    (reads=%llu, rt publishes skipped while lifecycle wrote=%llu)\n",
                    static_cast<unsigned long long>(reads.load()),
                    static_cast<unsigned long long>(skipped.load()));
    }

    // Single writer: readers must observe a monotonic sequence.
    {
        SeqlockSnapshot<EngineSnapshot> snap;
        snap.store(make(0));   // readers may run before the first tryStore
        std::atomic<bool> done{false};
        std::atomic<uint64_t> torn{0}, backwards{0};
        std::thread w([&] {
            for (uint64_t k = 1; k <= 400000; ++k) snap.tryStore(make(k));
            done.store(true, std::memory_order_release);
        });
        std::thread r([&] {
            int64_t last = -1;
            while (!done.load(std::memory_order_acquire)) {
                const auto s = snap.load();
                if (!consistent(s)) torn.fetch_add(1);
                if (s.positionTicks < last) backwards.fetch_add(1);
                last = s.positionTicks;
            }
        });
        w.join(); r.join();
        TEST("single writer: never torn", torn.load() == 0);
        TEST("single writer: reads are monotonic", backwards.load() == 0);
        TEST("single writer: final value visible", snap.load().positionTicks == 400000);
    }

    // Facade still publishes lifecycle + tempo through the seqlock.
    {
        session::LiveEngineFacade fac(nullptr, nullptr);
        fac.start();
        fac.setTempo(97);
        fac.tick(48);
        const auto s = fac.snapshot();
        TEST("facade: snapshot shows Running + tempo 97",
             s.lifecycleState == static_cast<int32_t>(session::LifecycleState::Running) &&
             s.tempoBpm == 97);
        fac.stop();
        TEST("facade: stop publishes Stopped",
             fac.snapshot().lifecycleState == static_cast<int32_t>(session::LifecycleState::Stopped));
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
