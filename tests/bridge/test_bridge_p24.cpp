// P2.4 C ABI additions (contract 1.1.0): UASF style load from bytes, chord mode,
// host MIDI port selection, MIDI input injection -> chord detection, and
// draining the engine's output. Headless; no CoreMIDI.

#include "aiarranger/aiarranger_engine.h"
#include "engine/uasf/serializer.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

// One Main A bar: a bass C2 on every beat (channel 10, so it follows the chord).
static std::vector<uint8_t> styleBytes() {
    uasf::StyleDefinition s;
    s.name = "P24";
    s.tempo_bpm = 120;
    s.resolution = 480;
    uasf::SectionDefinition sec;
    sec.type = uasf::SectionType::Main1; sec.bars = 1; sec.resolution = 480;
    sec.beats_per_bar = 4; sec.beat_note = 4;
    uasf::TrackDefinition t;
    t.name = "Bass"; t.midi_channel = 10; t.role = uasf::TrackRole::Bass;
    for (uint64_t b = 0; b < 4; ++b) {
        t.events.push_back({uasf::MidiEventType::NoteOn, 10, 36, 100, b * 480});
        t.events.push_back({uasf::MidiEventType::NoteOff, 10, 36, 0, b * 480 + 240});
    }
    sec.tracks.push_back(t);
    s.sections.push_back(sec);
    return uasf::UasfSerializer().serialize(s).data;
}

static std::vector<AiArrMidiMessage> drain(AiArrEngine* e) {
    std::vector<AiArrMidiMessage> all;
    AiArrMidiMessage buf[64];   // >= 32: room for the overflow silence block
    uint32_t n = 0;
    do {
        aiarr_engine_read_midi_out(e, buf, 64, &n);
        all.insert(all.end(), buf, buf + n);
    } while (n > 0);
    return all;
}

int main() {
    std::printf("Test: C ABI P2.4 additions\n");
    TEST("contract version 1.1.0", aiarr_contract_version() == 0x010100u);

    // ── NULL / argument checks ──
    uint32_t n = 99;
    AiArrMidiMessage m[4];
    TEST("NULL handles", aiarr_engine_load_style_uasf(nullptr, nullptr, 0) == AIARR_ERR_NULL_ARGUMENT &&
                         aiarr_engine_set_chord_mode(nullptr, 0) == AIARR_ERR_NULL_ARGUMENT &&
                         aiarr_engine_select_midi_input(nullptr, 0) == AIARR_ERR_NULL_ARGUMENT &&
                         aiarr_engine_select_midi_output(nullptr, 0) == AIARR_ERR_NULL_ARGUMENT &&
                         aiarr_engine_send_midi_in(nullptr, nullptr, 0) == AIARR_ERR_NULL_ARGUMENT &&
                         aiarr_engine_read_midi_out(nullptr, m, 4, &n) == AIARR_ERR_NULL_ARGUMENT);

    AiArrEngine* e = aiarr_engine_create();
    TEST("NULL count / NULL out with capacity", aiarr_engine_read_midi_out(e, m, 4, nullptr) == AIARR_ERR_NULL_ARGUMENT &&
                                                aiarr_engine_read_midi_out(e, nullptr, 4, &n) == AIARR_ERR_NULL_ARGUMENT);
    TEST("empty read: count 0", aiarr_engine_read_midi_out(e, m, 4, &n) == AIARR_OK && n == 0);

    // ── Style load ──
    const std::vector<uint8_t> bytes = styleBytes();
    TEST("garbage UASF -> UNSUPPORTED", aiarr_engine_load_style_uasf(e, bytes.data(), 8) == AIARR_ERR_UNSUPPORTED);
    TEST("valid UASF loads", aiarr_engine_load_style_uasf(e, bytes.data(), static_cast<uint32_t>(bytes.size())) == AIARR_OK);

    // ── Ports ──
    TEST("input index 1 -> DEVICE_UNAVAILABLE", aiarr_engine_select_midi_input(e, 1) == AIARR_ERR_DEVICE_UNAVAILABLE);
    TEST("output index 5 -> DEVICE_UNAVAILABLE", aiarr_engine_select_midi_output(e, 5) == AIARR_ERR_DEVICE_UNAVAILABLE);
    const uint8_t noteOnC[] = {0x90, 48, 100};
    TEST("inject before selecting -> DEVICE_UNAVAILABLE",
         aiarr_engine_send_midi_in(e, noteOnC, 3) == AIARR_ERR_DEVICE_UNAVAILABLE);
    TEST("chord mode out of range -> UNSUPPORTED", aiarr_engine_set_chord_mode(e, 3) == AIARR_ERR_UNSUPPORTED);

    TEST("start", aiarr_engine_start(e) == AIARR_OK);
    TEST("select host ports", aiarr_engine_select_midi_input(e, 0) == AIARR_OK &&
                              aiarr_engine_select_midi_output(e, 0) == AIARR_OK);
    AiArrSnapshot s{};
    aiarr_engine_tick(e, 0);
    aiarr_engine_snapshot(e, &s);
    TEST("snapshot: in and out live", s.midiInLive == 1 && s.midiOutLive == 1);
    TEST("load while not playing still OK after start",
         aiarr_engine_load_style_uasf(e, bytes.data(), static_cast<uint32_t>(bytes.size())) == AIARR_OK);

    // ── Chord input: F major (F A C below the split), running status ──
    const uint8_t fMajor[] = {0x90, 41, 100, 45, 100, 48, 100};
    TEST("inject F A C (running status)", aiarr_engine_send_midi_in(e, fMajor, sizeof fMajor) == AIARR_OK);
    std::vector<uint8_t> big(257, 0xF8);
    TEST("more than 256 bytes -> UNSUPPORTED", aiarr_engine_send_midi_in(e, big.data(), 257) == AIARR_ERR_UNSUPPORTED);
    for (int i = 0; i < 4; ++i) aiarr_engine_tick(e, 256);
    aiarr_engine_snapshot(e, &s);
    TEST("chord detected: F major", s.chordRoot % 12 == 5 && s.chordType == 0);

    // ── Output: play, the bass follows F (C2 -> F2 = 41 or F1 = 29) ──
    drain(e);
    TEST("transport start", aiarr_engine_transport_start(e) == AIARR_OK);
    for (int i = 0; i < 200; ++i) aiarr_engine_tick(e, 480);   // 2 s = 1 bar at 120
    const auto out = drain(e);
    bool bassF = false, wellFormed = !out.empty();
    for (const auto& x : out) {
        wellFormed &= x.length >= 1 && x.length <= 3 && (x.status & 0x80);
        if ((x.status & 0xF0) == 0x90 && x.data2 > 0 && (x.status & 0x0F) == 10) bassF |= x.data1 % 12 == 5;
    }
    TEST("output drained, well-formed short messages", wellFormed);
    TEST("regression: a chord held before Start is played (bass on F, not C)", bassF);

    // ── Chord mode switch: Single Finger, one key C = C major ──
    TEST("chord mode Single Finger", aiarr_engine_set_chord_mode(e, AIARR_CHORD_MODE_SINGLE_FINGER) == AIARR_OK);
    const uint8_t release[] = {0x80, 41, 0, 45, 0, 48, 0};
    const uint8_t oneKey[] = {0x90, 43, 100};   // G alone
    aiarr_engine_send_midi_in(e, release, sizeof release);
    aiarr_engine_send_midi_in(e, oneKey, sizeof oneKey);
    for (int i = 0; i < 4; ++i) aiarr_engine_tick(e, 256);
    aiarr_engine_snapshot(e, &s);
    TEST("single finger: one key G = G major", s.chordRoot % 12 == 7 && s.chordType == 0);
    TEST("load while playing -> INVALID_STATE",
         aiarr_engine_load_style_uasf(e, bytes.data(), static_cast<uint32_t>(bytes.size())) == AIARR_ERR_INVALID_STATE);

    // ── Disconnect output: refused while playing (Codex L2 #1), allowed stopped ──
    TEST("deselect output while playing -> INVALID_STATE (NoteOffs would be lost)",
         aiarr_engine_select_midi_output(e, -1) == AIARR_ERR_INVALID_STATE);
    TEST("re-selecting the same port while playing is OK", aiarr_engine_select_midi_output(e, 0) == AIARR_OK);
    aiarr_engine_transport_stop(e);
    for (int i = 0; i < 4; ++i) aiarr_engine_tick(e, 256);
    TEST("deselect output when stopped", aiarr_engine_select_midi_output(e, -1) == AIARR_OK);
    aiarr_engine_transport_start(e);
    drain(e);
    for (int i = 0; i < 100; ++i) aiarr_engine_tick(e, 480);
    TEST("disconnected output sends nothing", drain(e).empty());

    // ── Drain respects capacity ──
    aiarr_engine_transport_stop(e);
    for (int i = 0; i < 4; ++i) aiarr_engine_tick(e, 256);
    aiarr_engine_select_midi_output(e, 0);
    aiarr_engine_transport_start(e);
    for (int i = 0; i < 100; ++i) aiarr_engine_tick(e, 480);
    AiArrMidiMessage one[1];
    TEST("capacity 1 -> at most 1", aiarr_engine_read_midi_out(e, one, 1, &n) == AIARR_OK && n <= 1);
    TEST("capacity 0 with NULL out is OK", aiarr_engine_read_midi_out(e, nullptr, 0, &n) == AIARR_OK && n == 0);

    aiarr_engine_stop(e);
    aiarr_engine_destroy(e);

    // ── Overflow (Codex L2 #2): the host stops draining past the 1024 buffer and
    //    then stops; once drained, silence on all channels follows the backlog. ──
    {
        AiArrEngine* o = aiarr_engine_create();
        aiarr_engine_load_style_uasf(o, bytes.data(), static_cast<uint32_t>(bytes.size()));
        aiarr_engine_start(o);
        aiarr_engine_select_midi_output(o, 0);
        aiarr_engine_transport_start(o);
        for (int i = 0; i < 40000; ++i) aiarr_engine_tick(o, 480);   // ~400 s, > 1024 messages
        aiarr_engine_stop(o);
        const auto all = drain(o);
        bool block = all.size() == 1024 + 32;   // full buffer, then the silence block
        for (size_t i = 0; block && i < 32; ++i) {
            const auto& x = all[1024 + i];
            block &= x.status == (0xB0 | (i / 2)) && x.data1 == (i % 2 ? 123 : 120) && x.data2 == 0 && x.length == 3;
        }
        TEST("overflow: backlog capped at the buffer, then CC120+CC123 on all 16 channels", block);
        uint32_t k = 9;
        AiArrMidiMessage b[64];
        TEST("silence sent once", aiarr_engine_read_midi_out(o, b, 64, &k) == AIARR_OK && k == 0);
        aiarr_engine_destroy(o);
    }
    // ── Threads as the contract allows: inject (host MIDI thread), tick (owner),
    //    drain (host output thread) run concurrently. Meaningful under TSan. ──
    {
        AiArrEngine* t = aiarr_engine_create();
        aiarr_engine_load_style_uasf(t, bytes.data(), static_cast<uint32_t>(bytes.size()));
        aiarr_engine_start(t);
        aiarr_engine_select_midi_input(t, 0);
        aiarr_engine_select_midi_output(t, 0);
        aiarr_engine_transport_start(t);
        std::atomic<bool> done{false};
        std::atomic<uint64_t> drained{0};
        std::thread injector([&] {
            for (int i = 0; i < 2000; ++i) {
                const uint8_t on[] = {0x90, static_cast<uint8_t>(41 + i % 7), 90};
                const uint8_t off[] = {0x80, static_cast<uint8_t>(41 + i % 7), 0};
                aiarr_engine_send_midi_in(t, on, 3);
                aiarr_engine_send_midi_in(t, off, 3);
            }
        });
        std::thread drainer([&] {
            AiArrMidiMessage b[32];
            uint32_t k = 0;
            while (!done.load()) {
                aiarr_engine_read_midi_out(t, b, 32, &k);
                drained += k;
            }
        });
        for (int i = 0; i < 3000; ++i) aiarr_engine_tick(t, 64);
        injector.join();
        aiarr_engine_stop(t);
        done = true;
        drainer.join();
        AiArrSnapshot ts{};
        TEST("concurrent inject/tick/drain: engine alive, output drained",
             aiarr_engine_snapshot(t, &ts) == AIARR_OK && drained.load() > 0);
        aiarr_engine_destroy(t);
    }
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
