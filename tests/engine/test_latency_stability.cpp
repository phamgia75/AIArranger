// Gate 9 — Latency + MIDI stability harness.
//
// Measures schedule->dispatch latency through the CoreMidiOut lock-free queue
// + dispatch thread (the same path that feeds external hardware), and verifies
// the transport stays balanced across a simulated disconnect / reconnect.
//
// Runs headless (no real MIDI hardware needed): the dispatch tap observes every
// event before the endpoint check, so latency and balance are measurable on CI.

#include "engine/midi/coremidi_out.h"
#include "engine/midi/note_balance.h"
#include "engine/uasf/types.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <mach/mach_time.h>
#include <unistd.h>

using namespace ai_arranger::midi;
using namespace ai_arranger::uasf;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  ❌ FAIL: %s\n", name); failures++; } \
    else { std::printf("  ✅ PASS: %s\n", name); passes++; } \
} while(0)

// Same check as TEST; on failure also prints the measured values, so an
// intermittent failure identifies its phase and numbers (thresholds unchanged).
#define TESTV(name, expr, fmt, ...) do { \
    if (!(expr)) { std::fprintf(stderr, "  ❌ FAIL: %s [" fmt "]\n", name, __VA_ARGS__); failures++; } \
    else { std::printf("  ✅ PASS: %s\n", name); passes++; } \
} while(0)

static MidiEvent mk(MidiEventType t, uint8_t ch, uint8_t d1, uint8_t d2) {
    MidiEvent e; e.type = t; e.channel = ch; e.data1 = d1; e.data2 = d2; e.tick = 0; return e;
}

static uint64_t nowNs() {
    static mach_timebase_info_data_t tb = []{ mach_timebase_info_data_t i{}; mach_timebase_info(&i); return i; }();
    return mach_absolute_time() * tb.numer / tb.denom;
}

int main() {
    setenv("AIARR_MIDI_HW_TX", "0", 1);   // never drive real hardware, even outside CTest
    std::printf("Test: Latency + MIDI Stability\n");

    // ── 1. schedule -> dispatch latency (p50 / p99) ──────────────────
    {
        constexpr int N = 500;
        std::vector<uint64_t> sendNs(N, 0);
        std::vector<uint64_t> latencyNs;
        latencyNs.reserve(N);

        CoreMidiOut out;
        std::atomic<int> consumed{0};
        out.setDispatchTap([&](const MidiEvent&) {
            int i = consumed.fetch_add(1, std::memory_order_acq_rel);
            if (i < N) latencyNs.push_back(nowNs() - sendNs[i]);
        });

        TEST("CoreMidiOut init for latency run", out.initialize("AIA Latency"));
        out.selectDestination(-1); // headless: measure queue->dispatch only

        for (int i = 0; i < N; ++i) {
            sendNs[i] = nowNs();
            out.send(mk(MidiEventType::NoteOn, 0, 60, 100));
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        for (int w = 0; w < 1000 && consumed.load() < N; ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

        TESTV("All events dispatched (no loss)", out.dispatchedCount() >= (uint64_t)N,
              "dispatched=%llu of %d, tap=%d", (unsigned long long)out.dispatchedCount(), N, consumed.load());
        // Join the dispatch thread before reading what its tap wrote: `consumed`
        // is bumped before push_back, so a wait on it alone races the last write.
        out.shutdown();

        std::sort(latencyNs.begin(), latencyNs.end());
        uint64_t p50 = latencyNs.empty() ? 0 : latencyNs[latencyNs.size() * 50 / 100];
        uint64_t p99 = latencyNs.empty() ? 0 : latencyNs[latencyNs.size() * 99 / 100];
        uint64_t mx  = latencyNs.empty() ? 0 : latencyNs.back();
        std::printf("  [latency] n=%zu  p50=%.1fus  p99=%.1fus  max=%.1fus\n",
                    latencyNs.size(), p50 / 1000.0, p99 / 1000.0, mx / 1000.0);

        TESTV("Latency samples collected", latencyNs.size() >= (size_t)(N * 9 / 10), "samples=%zu of %d", latencyNs.size(), N);
        // Loose ceiling: catches gross scheduling failure, tolerant of CI jitter.
        TESTV("p99 schedule->dispatch latency < 50ms", p99 < 50ull * 1000 * 1000, "p99=%.1fus max=%.1fus", p99 / 1000.0, mx / 1000.0);
    }

    // ── 2. Stability across simulated disconnect / reconnect ─────────
    {
        CoreMidiOut out;
        NoteBalance balance;
        out.setDispatchTap([&](const MidiEvent& e) { balance.observe(e); });

        TEST("CoreMidiOut init for stability run", out.initialize("AIA Stability"));
        // "Replug" target = a virtual destination this test owns, found by
        // endpoint identity: a real attached device is never selected (#safety).
        MIDIClientRef sinkClient = 0;
        MIDIEndpointRef sinkEp = 0;
        int sinkIdx = -1;
        const std::string sinkName = "AIA STABILITY SINK " + std::to_string(getpid());
        CFStringRef cf = CFStringCreateWithCString(nullptr, sinkName.c_str(), kCFStringEncodingUTF8);
        if (MIDIClientCreate(CFSTR("AIA Stability sink"), nullptr, nullptr, &sinkClient) == noErr &&
            MIDIDestinationCreate(sinkClient, cf, [](const MIDIPacketList*, void*, void*) {}, nullptr, &sinkEp) == noErr) {
            for (int w = 0; w < 200 && sinkIdx < 0; ++w) {
                for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
                    if (MIDIGetDestination(i) == sinkEp) sinkIdx = static_cast<int>(i);
                if (sinkIdx < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        CFRelease(cf);

        // Stream balanced NoteOn/NoteOff pairs while toggling the destination
        // (simulating a device dropping out and coming back). The dispatch path
        // must deliver every event in order regardless, so balance is preserved.
        constexpr int PAIRS = 200;
        for (int i = 0; i < PAIRS; ++i) {
            if (i == 50) out.selectDestination(-1);                 // "unplug"
            if (i == 120) out.selectDestinationByName(sinkName);      // "replug" (own unique sink or none)
            uint8_t note = 36 + (i % 24);
            out.send(mk(MidiEventType::NoteOn,  0, note, 100));
            out.send(mk(MidiEventType::NoteOff, 0, note, 0));
        }
        uint64_t expected = (uint64_t)PAIRS * 2;
        for (int w = 0; w < 1000 && out.dispatchedCount() < expected; ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

        TESTV("Every event dispatched across reconnect", out.dispatchedCount() >= expected,
              "dispatched=%llu of %llu, sinkIdx=%d", (unsigned long long)out.dispatchedCount(), (unsigned long long)expected, sinkIdx);
        out.shutdown();   // join the dispatch thread before reading the tap's NoteBalance
        if (sinkEp) MIDIEndpointDispose(sinkEp);
        if (sinkClient) MIDIClientDispose(sinkClient);
        TESTV("Balanced on/off totals across reconnect",
              balance.totalNoteOn() == PAIRS && balance.totalNoteOff() == PAIRS,
              "on=%llu off=%llu of %d", (unsigned long long)balance.totalNoteOn(),
              (unsigned long long)balance.totalNoteOff(), PAIRS);
        TEST("No stuck notes after disconnect/reconnect", balance.stuckNoteCount() == 0);
        TEST("No orphan note-offs across reconnect", balance.orphanOffCount() == 0);
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
