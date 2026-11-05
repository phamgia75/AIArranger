// Gate 9 — Korg playback CLI.
//
// Manual driver for the Musical Review: loads a UASF style (or the built-in
// demo), runs it through the StylePlayer, and sends the resulting MIDI to an
// external arranger (Korg PA / Yamaha Genos, etc.) over CoreMIDI.
//
// This is a hands-on tool, not an automated test — run it on a machine with a
// real MIDI destination connected.
//
//   korg-playback                            # built-in demo style, NO destination
//   korg-playback [style.uasf] --dest "NAME"  # select the destination named exactly NAME
//
// Fail-closed: nothing is sent until a destination is chosen, either with --dest
// (exact name) or interactively with [ / ]. A connected device is never picked
// on its own.
//
// Live keys (single keypress, no Enter):
//   space   start / stop          1..6  switch to section index (0-based +1)
//   i       intro                 f     queue fill
//   e       queue ending          p     PANIC (all notes off)
//   c       cycle chord           [ ]   prev / next destination
//   l       list destinations     ?     help
//   q       quit

#include "engine/arranger/style_player.h"
#include "engine/realtime/clock.h"
#include "engine/midi/scheduler.h"
#include "engine/midi/panic.h"
#include "engine/midi/coremidi_out.h"
#include "engine/midi/note_balance.h"
#include "engine/uasf/deserializer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include <mach/mach_time.h>

using namespace ai_arranger;
using namespace ai_arranger::arranger;
using namespace ai_arranger::realtime;
using namespace ai_arranger::midi;
using namespace ai_arranger::uasf;

namespace {

uint64_t nowNs() {
    static mach_timebase_info_data_t tb = []{ mach_timebase_info_data_t i{}; mach_timebase_info(&i); return i; }();
    return mach_absolute_time() * tb.numer / tb.denom;
}

const char* stateName(SequencerState s) {
    switch (s) {
        case SequencerState::Stopped:      return "Stopped";
        case SequencerState::PlayingIntro: return "Intro";
        case SequencerState::PlayingMain:  return "Main";
        case SequencerState::PlayingFill:  return "Fill";
        case SequencerState::PlayingEnding:return "Ending";
        case SequencerState::Panic:        return "Panic";
    }
    return "?";
}

void printHelp() {
    std::printf(
        "\nKeys: [space]start/stop  [1-6]section  [i]intro  [f]fill  [e]ending\n"
        "      [c]chord  [p]PANIC  [ [ / ] ]dest-  [l]list  [?]help  [q]quit\n\n");
}

struct RawTerminal {
    termios saved{};
    bool ok{false};
    RawTerminal() {
        if (tcgetattr(STDIN_FILENO, &saved) == 0) {
            termios raw = saved;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;   // non-blocking read
            raw.c_cc[VTIME] = 1;  // 0.1s timeout
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
            ok = true;
        }
    }
    ~RawTerminal() { if (ok) tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
};

const Chord kChordCycle[] = {
    chords::CMaj, chords::GMaj, chords::AMin, chords::FMaj, chords::DMin, chords::EMin,
};
constexpr int kChordCount = sizeof(kChordCycle) / sizeof(kChordCycle[0]);

} // namespace

int main(int argc, char** argv) {
    // ── Load a style ─────────────────────────────────────────────────
    const char* stylePath = nullptr;
    const char* destName = nullptr;
    for (int i = 1; i < argc; ++i) {
        const bool isDest = std::string(argv[i]) == "--dest";
        if (isDest && i + 1 < argc) destName = argv[++i];
        else if (!isDest && !stylePath) stylePath = argv[i];
        else { std::fprintf(stderr, "usage: korg-playback [style.uasf] [--dest NAME]\n"); return 1; }
    }
    StyleDefinition style;
    if (stylePath) {
        UasfDeserializer de;
        auto res = de.deserializeFromFile(stylePath);
        if (!res.success) {
            std::fprintf(stderr, "Failed to load '%s': %s\n", stylePath, res.error.c_str());
            return 1;
        }
        style = res.style;
        std::printf("Loaded style: %s (%zu sections)\n", style.name.c_str(), style.sections.size());
    } else {
        style = buildDemoStyle();
        std::printf("Loaded built-in demo style (%zu sections)\n", style.sections.size());
    }
    if (style.resolution == 0) style.resolution = 480;
    if (style.tempo_bpm == 0)  style.tempo_bpm = 120;

    for (size_t i = 0; i < style.sections.size(); ++i) {
        std::printf("  [%zu] %-10s bars=%u\n", i, style.sections[i].name.c_str(), style.sections[i].bars);
    }

    // ── Engine wiring ────────────────────────────────────────────────
    RealtimeClock clock;
    MidiScheduler scheduler;
    PanicHandler  panic;
    StylePlayer   player(clock, scheduler, panic);
    CoreMidiOut   out;
    NoteBalance   balance;

    // Latency stats from the dispatch tap (schedule -> dispatch).
    std::atomic<uint64_t> sched_ns{0};
    std::atomic<uint64_t> lat_sum{0};
    std::atomic<uint64_t> lat_max{0};
    std::atomic<uint64_t> lat_n{0};
    out.setDispatchTap([&](const MidiEvent& e) {
        balance.observe(e);
        uint64_t s = sched_ns.load(std::memory_order_acquire);
        if (s) {
            uint64_t d = nowNs() - s;
            lat_sum.fetch_add(d, std::memory_order_relaxed);
            lat_n.fetch_add(1, std::memory_order_relaxed);
            uint64_t m = lat_max.load(std::memory_order_relaxed);
            while (d > m && !lat_max.compare_exchange_weak(m, d)) {}
        }
    });

    if (!out.initialize("AI Arranger — Korg Playback")) {
        std::fprintf(stderr, "CoreMIDI init failed\n");
        return 1;
    }

    // Route scheduler output to CoreMIDI (records send time for latency).
    scheduler.setOutputCallback([&](const MidiEvent& e) {
        sched_ns.store(nowNs(), std::memory_order_release);
        out.send(e);
    });

    auto dests = out.enumerateDestinations();
    std::printf("\nMIDI destinations (%zu):\n", dests.size());
    for (auto& d : dests) std::printf("  [%d] %s\n", d.index, d.name.c_str());
    if (destName) {
        // Controlled error, never a fallback: the name must match exactly one live
        // destination, and that exact endpoint is selected (hot-plug safe).
        if (!out.selectDestinationByName(destName)) {
            std::fprintf(stderr, "--dest \"%s\": no unique destination with that exact name\n", destName);
            out.shutdown();
            return 2;
        }
        std::printf("Selected: %s\n", destName);
    } else {
        std::printf("No destination selected: playback is silent until you pick one with [ / ].\n");
    }

    player.loadStyle(style);
    clock.setSampleRate(48000);
    clock.setTempo(style.tempo_bpm);
    clock.setResolution(style.resolution);

    printHelp();

    // ── Playback thread (wall-clock driven virtual sample clock) ─────
    std::atomic<bool> alive{true};
    std::thread pump([&]{
        const uint32_t sr = 48000;
        uint64_t last = nowNs();
        while (alive.load(std::memory_order_acquire)) {
            uint64_t t = nowNs();
            if (clock.isRunning()) {
                double dtSec = (t - last) / 1e9;
                uint32_t samples = (uint32_t)(dtSec * sr);
                if (samples > 0) {
                    clock.advance(samples);
                    player.tick();
                    scheduler.advanceTo(clock.getPosition());
                    last = t;
                }
            } else {
                last = t;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // ── Key loop ─────────────────────────────────────────────────────
    RawTerminal raw;
    int chordIdx = 0;
    bool running = true;
    while (running) {
        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) continue;

        switch (c) {
            case ' ':
                if (player.isPlaying()) { player.stop(); std::printf("[stop]\n"); }
                else { player.start(0); std::printf("[start]\n"); }
                break;
            case 'i': player.switchSection(0); std::printf("[intro]\n"); break;
            case 'f': player.fill();   std::printf("[fill]\n"); break;
            case 'e': player.ending(); std::printf("[ending]\n"); break;
            case 'p': player.panic();  std::printf("[PANIC]\n"); break;
            case 'c': {
                chordIdx = (chordIdx + 1) % kChordCount;
                Chord ch = kChordCycle[chordIdx];
                player.setChord(ch);
                std::printf("[chord] type=%d root=%d\n", (int)ch.type, ch.root);
                break;
            }
            case 'l': {
                auto ds = out.enumerateDestinations();
                std::printf("Destinations (%zu):\n", ds.size());
                for (auto& d : ds) std::printf("  [%d]%s %s\n", d.index,
                    d.index == out.selectedDestination() ? "*" : " ", d.name.c_str());
                break;
            }
            case '[': case ']': {
                auto ds = out.enumerateDestinations();
                if (!ds.empty()) {
                    const int n = (int)ds.size();
                    int sel = out.selectedDestination();
                    if (sel < 0) sel = (c == ']') ? 0 : n - 1;   // first explicit pick
                    else sel = (c == ']') ? (sel + 1) % n : (sel - 1 + n) % n;
                    // Select the device shown in this list by identity, not by an
                    // index a hot-plug may have shifted since enumeration.
                    if (out.selectDestinationByUid(ds[sel].uid))
                        std::printf("[dest] -> %s\n", ds[sel].name.c_str());
                    else
                        std::printf("[dest] %s is gone; nothing selected\n", ds[sel].name.c_str());
                }
                break;
            }
            case '?': printHelp(); break;
            case 'q': running = false; break;
            default:
                if (c >= '1' && c <= '6') {
                    int idx = c - '1';
                    if (idx < (int)style.sections.size()) {
                        player.switchSection(idx);
                        std::printf("[section %d] %s\n", idx, style.sections[idx].name.c_str());
                    }
                }
                break;
        }
        std::printf("  state=%s section=%d active-notes=%d stuck=%d sent=%llu dropped=%llu\n",
                    stateName(player.getState()), player.getCurrentSection(),
                    balance.activeCount(), balance.stuckNoteCount(),
                    (unsigned long long)out.sentCount(), (unsigned long long)out.droppedCount());
    }

    // ── Shutdown ─────────────────────────────────────────────────────
    player.panic();
    scheduler.advanceTo(INT64_MAX);
    alive.store(false, std::memory_order_release);
    if (pump.joinable()) pump.join();
    out.shutdown();

    uint64_t n = lat_n.load();
    if (n) {
        std::printf("\n[latency] schedule->dispatch avg=%.1fus max=%.1fus over %llu events\n",
                    (lat_sum.load() / (double)n) / 1000.0, lat_max.load() / 1000.0,
                    (unsigned long long)n);
    }
    std::printf("Final balance: stuck=%d  (NoteOn=%d NoteOff=%d orphans=%d)\n",
                balance.stuckNoteCount(), balance.totalNoteOn(),
                balance.totalNoteOff(), balance.orphanOffCount());
    std::printf("Bye.\n");
    return 0;
}
