// SFF1 reader — SMF (MTrk) event decoding correctness.
//
// Real Genos styles carry 33-80 SysEx events (F0 <len> <payload>) in their MTrk.
// The reader used to consume only the F0 status byte and then read the SysEx
// payload as delta-times, stretching every later event (e.g. CLASSIC_6_8's last
// event landed at tick 3,536,709 instead of 230,400) — and, because `break` in
// its switch only left the switch, it also emitted the End-of-Track meta (FF)
// and truncated messages as channel events.
//
// This test pins (a) exact decoding of synthetic tracks and (b) event-for-event
// equality with a strict, independent SMF decoder on every real corpus style.

#include "importers/sff1/sff1_reader.h"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

using namespace ai_arranger::importers::sff1;

static int failures = 0;
static int passes = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

using Ev = std::tuple<uint32_t, uint8_t, uint8_t, uint8_t>;  // tick, status, d1, d2

static std::vector<uint8_t> smf(const std::vector<uint8_t>& trk) {
    std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                              'M', 'T', 'r', 'k'};
    const uint32_t n = static_cast<uint32_t>(trk.size());
    b.push_back(n >> 24); b.push_back(n >> 16); b.push_back(n >> 8); b.push_back(n);
    b.insert(b.end(), trk.begin(), trk.end());
    return b;
}

static std::vector<Ev> engineEvents(const ParseResult& r) {
    std::vector<Ev> out;
    for (const auto& s : r.sections)
        for (const auto& t : s.tracks)
            for (const auto& e : t.events) out.emplace_back(e.tick, e.status, e.data1, e.data2);
    return out;
}

// Strict reference decoder (SMF 1.0): VLQ deltas, running status for channel
// messages only, F0/F7 <vlq len> payload skipped, FF <type> <vlq len> skipped,
// FF 2F ends the track.
static bool vlq(const std::vector<uint8_t>& d, size_t& p, uint32_t& v) {
    v = 0;
    for (int i = 0; i < 4; ++i) {
        if (p >= d.size()) return false;
        const uint8_t b = d[p++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return true;
    }
    return false;
}
static std::vector<Ev> referenceEvents(const std::vector<uint8_t>& file) {
    std::vector<Ev> out;
    size_t p = 0;
    while (p + 8 <= file.size()) {
        const std::string id(reinterpret_cast<const char*>(&file[p]), 4);
        const uint32_t len = (uint32_t(file[p + 4]) << 24) | (uint32_t(file[p + 5]) << 16) |
                             (uint32_t(file[p + 6]) << 8) | file[p + 7];
        if (id != "MTrk") { p += 8 + len; continue; }
        std::vector<uint8_t> t(file.begin() + p + 8, file.begin() + p + 8 + len);
        size_t q = 0; uint8_t rs = 0; uint32_t tick = 0, dt = 0, l = 0;
        while (q < t.size() && vlq(t, q, dt)) {
            tick += dt;
            if (q >= t.size()) break;
            uint8_t s = t[q];
            if (s & 0x80) ++q; else s = rs;
            if (s == 0xFF) {
                const uint8_t type = t[q++];
                if (!vlq(t, q, l)) break;
                q += l;
                if (type == 0x2F) break;
            } else if (s == 0xF0 || s == 0xF7) {
                if (!vlq(t, q, l)) break;
                q += l;
            } else if (s & 0x80) {
                rs = s;
                const bool one = (s & 0xF0) == 0xC0 || (s & 0xF0) == 0xD0;
                if (q + (one ? 1 : 2) > t.size()) break;
                out.emplace_back(tick, s, t[q], one ? 0 : t[q + 1]);
                q += one ? 1 : 2;
            } else {
                break;
            }
        }
        break;  // SFF1: one MTrk
    }
    return out;
}

int main() {
    std::printf("Test: SFF1 SMF event decoding (SysEx / meta / EoT / truncation)\n");
    Sff1Reader reader;

    // ── 1. SysEx payload is skipped, not read as delta-times ────────────
    {
        const auto buf = smf({
            0x00, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7,   // GM On SysEx @0
            0x60, 0x90, 60, 100,                               // NoteOn  @96
            0x60, 0x80, 60, 0,                                 // NoteOff @192
            0x00, 0xFF, 0x2F, 0x00});                          // End of Track
        const auto ev = engineEvents(reader.parseBuffer(buf, "sysex.sty"));
        const std::vector<Ev> want = {{96, 0x90, 60, 100}, {192, 0x80, 60, 0}};
        TEST("sysex: payload skipped, note ticks exact (96, 192)", ev == want);
    }

    // ── 2. Meta events and End-of-Track are not emitted as events ───────
    {
        const auto buf = smf({
            0x00, 0xFF, 0x06, 0x04, 'S', 'I', 'n', 't',        // Marker "SInt"
            0x10, 0xB0, 7, 100,                                // CC7 @16
            0x00, 0xFF, 0x2F, 0x00,                            // End of Track @16
            0x00, 0x90, 64, 90});                              // after EoT: ignored
        const auto ev = engineEvents(reader.parseBuffer(buf, "meta.sty"));
        const std::vector<Ev> want = {{16, 0xB0, 7, 100}};
        TEST("meta/EoT: only the channel event, nothing after End of Track", ev == want);
    }

    // ── 3. Running status survives; SysEx/meta cancel it (SMF 1.0) ──────
    {
        const auto buf = smf({
            0x00, 0x90, 60, 100,
            0x01, 62, 100,                                     // running status NoteOn
            0x01, 0xC0, 5,                                     // Program change
            0x00, 0xFF, 0x2F, 0x00});
        const auto ev = engineEvents(reader.parseBuffer(buf, "running.sty"));
        const std::vector<Ev> want = {{0, 0x90, 60, 100}, {1, 0x90, 62, 100}, {2, 0xC0, 5, 0}};
        TEST("running status: NoteOn reused, 1-byte PC decoded", ev == want);
    }

    // ── 4. A truncated trailing message is dropped, not zero-filled ─────
    {
        const auto buf = smf({0x00, 0x90, 60, 100, 0x00, 0x90, 61});  // missing velocity
        const auto ev = engineEvents(reader.parseBuffer(buf, "trunc.sty"));
        const std::vector<Ev> want = {{0, 0x90, 60, 100}};
        TEST("truncation: partial NoteOn not emitted", ev == want);
    }

    // ── 5. Every real corpus style decodes identically to the reference ─
    {
        const char* files[] = {
            "BALLAD_FOLK_SC_GENOS_.S718---4c7ad352-3441-418f-9ad6-41fd41c38619.sty",
            "CLASSIC_6_8_SC_GENOS.S718---36428220-aac8-423d-9a87-79f1594df699.sty",
            "C_WHISPER_SC_GENOS.S718---da1df5af-1207-4d84-bf3d-99b5d051b92b.sty",
            "POP_ACOUSTIC_2_SC_GENOS.S718---7ba40ed3-527f-49ce-a22c-2414d5de2ec5.sty",
        };
        int identical = 0, loaded = 0;
        for (const char* name : files) {
            const std::string path = std::string(CORPUS_DIR) + "/" + name;
            std::ifstream in(path, std::ios::binary);
            if (!in) { std::fprintf(stderr, "  missing fixture: %s\n", path.c_str()); continue; }
            ++loaded;
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
            const auto ref = referenceEvents(bytes);
            const auto eng = engineEvents(reader.parseFile(path));
            if (!ref.empty() && eng == ref) ++identical;
            else std::fprintf(stderr, "  %s: engine %zu events, reference %zu\n",
                              name, eng.size(), ref.size());
        }
        TEST("corpus: all 4 required style fixtures present", loaded == 4);
        TEST("corpus: every style decodes event-for-event like the reference", identical == 4);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
