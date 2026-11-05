// Korg Style SMF (separated by markers): marker grammar, writer determinism,
// independent parse(write(x)) == x, validator rules, malformed-input parsing.

#include "exporters/korg/korg_style_smf.h"

#include <cstdio>

using namespace ai_arranger::exporters::korg;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static bool hasError(const Style& s, const char* needle) {
    for (const auto& e : validate(s)) if (e.find(needle) != std::string::npos) return true;
    return false;
}

static Style sample() {
    Style s;
    s.ppqn = 480;
    s.mpqn = 600000;   // 100 BPM
    Element v1{"v1cv1", 4, 4, 1920, {}};
    v1.events = {{0, 0xB0 | kAcc1Ch, 0, 8}, {0, 0xB0 | kAcc1Ch, 32, 1}, {0, 0xC0 | kAcc1Ch, 5, 0},
                 {0, 0x90 | kDrumCh, 36, 100}, {0, 0x90 | kBassCh, 36, 90},
                 {240, 0x80 | kDrumCh, 36, 0}, {480, 0xE0 | kAcc1Ch, 0, 64},
                 {1919, 0x80 | kBassCh, 36, 0}};
    Element e1{"e1cv1", 6, 8, 1440, {}};
    e1.events = {{0, 0x90 | kAcc1Ch, 60, 80}, {1439, 0x80 | kAcc1Ch, 60, 0}};
    s.elements = {v1, e1};
    return s;
}

static bool same(const Style& a, const Style& b) {
    if (a.ppqn != b.ppqn || a.mpqn != b.mpqn || a.elements.size() != b.elements.size()) return false;
    for (size_t i = 0; i < a.elements.size(); ++i) {
        const auto &x = a.elements[i], &y = b.elements[i];
        if (x.marker != y.marker || x.num != y.num || x.den != y.den || x.length != y.length ||
            x.events.size() != y.events.size()) return false;
        for (size_t j = 0; j < x.events.size(); ++j) {
            const auto &p = x.events[j], &q = y.events[j];
            const bool oneByte = (p.status & 0xF0) == 0xC0 || (p.status & 0xF0) == 0xD0;
            if (p.tick != q.tick || p.status != q.status || p.d1 != q.d1 || (!oneByte && p.d2 != q.d2)) return false;
        }
    }
    return true;
}

int main() {
    std::printf("Test: Korg Style SMF format\n");

    // Marker grammar: the Korg tutorial's own valid / invalid examples.
    TEST("marker i1cv2 valid", isValidMarker("i1cv2"));
    TEST("marker v4cv3 valid", isValidMarker("v4cv3"));
    TEST("marker v4cv6 valid", isValidMarker("v4cv6"));
    TEST("marker e2cv2 valid", isValidMarker("e2cv2"));
    TEST("marker V1cv2 rejected", !isValidMarker("V1cv2"));
    TEST("marker v1CV2 rejected", !isValidMarker("v1CV2"));
    TEST("marker 'intro i1cv2' rejected", !isValidMarker("intro i1cv2"));
    TEST("marker 'v1cv1 chorus' rejected", !isValidMarker("v1cv1 chorus"));
    TEST("marker v5cv1 rejected (variations 1-4)", !isValidMarker("v5cv1"));
    TEST("marker i3cv1 rejected (others 1-2)", !isValidMarker("i3cv1"));
    TEST("marker f1cv3 rejected (others cv 1-2)", !isValidMarker("f1cv3"));
    TEST("marker b1cv1 rejected (no documented break code)", !isValidMarker("b1cv1"));

    const Style s = sample();
    TEST("sample validates", validate(s).empty());

    const auto w1 = write(s), w2 = write(s);
    TEST("write ok", w1.ok);
    TEST("write is byte-identical across runs", w1.bytes == w2.bytes);
    TEST("header: MThd format 0, 1 track, PPQN 480",
         w1.bytes.size() > 14 && w1.bytes[8] == 0 && w1.bytes[9] == 0 && w1.bytes[11] == 1 &&
         w1.bytes[12] == 0x01 && w1.bytes[13] == 0xE0);

    const auto back = parse(w1.bytes);
    TEST("parse ok", back.ok);
    TEST("parse(write(x)) == x", same(back.style, s));
    TEST("re-parsed style validates", validate(back.style).empty());

    // Validator rules.
    { Style b = s; b.elements[0].marker = "Main A"; TEST("rejects non-Korg marker", hasError(b, "invalid marker")); }
    { Style b = s; b.elements[1].marker = "v1cv1"; TEST("rejects duplicate marker", hasError(b, "duplicate")); }
    { Style b = s; b.elements[0].length = 1000; TEST("rejects partial bar", hasError(b, "whole number of bars")); }
    { Style b = s; b.elements[0].den = 3; TEST("rejects bad time signature", hasError(b, "time signature")); }
    { Style b = s; b.elements[1].events.insert(b.elements[1].events.begin(), {0, 0x90 | kAcc1Ch, 5, 80});
      b.elements[1].events.push_back({1439, 0x80 | kAcc1Ch, 5, 0});
      TEST("rejects C-1..B-1 note on chord-driven track", hasError(b, "Chord event")); }
    { Style b = s; b.elements[1].events = {{0, 0x90 | kDrumCh, 5, 80}, {10, 0x80 | kDrumCh, 5, 0}};
      TEST("accepts low note on drum track", validate(b).empty()); }
    { Style b = s; b.elements[0].events.push_back({1919, 0xC0 | kAcc1Ch, 3, 0});
      TEST("rejects program change after tick 0", hasError(b, "tick 0")); }
    { Style b = s; b.elements[1].events.pop_back(); TEST("rejects hanging note", hasError(b, "still sounding")); }
    { Style b = s; b.elements[1].events.push_back({1440, 0xB0 | kAcc1Ch, 7, 100});
      TEST("rejects event at element end", hasError(b, "element end")); }
    { Style b = s; b.elements[1].events[0].status = 0x90 | 3; b.elements[1].events[1].status = 0x80 | 3;
      TEST("rejects channel below 9", hasError(b, "channel 4")); }
    { Style b = s; std::swap(b.elements[0].events[3], b.elements[0].events[5]);
      TEST("rejects unsorted events", hasError(b, "sorted")); }
    { Style b = s; b.ppqn = 0; TEST("rejects PPQN 0", hasError(b, "PPQN")); TEST("write refuses invalid style", !write(b).ok); }
    { Style b; b.ppqn = 480; TEST("rejects empty style", hasError(b, "no style elements")); }
    { Style b; b.ppqn = 1;   // 4-tick bars: 0x0FFFFFFF ticks overflow with two max elements
      b.elements = {{"v1cv1", 4, 4, 0x0FFFFFFC, {}}, {"v2cv1", 4, 4, 0x0FFFFFFC, {}}};
      TEST("rejects style longer than SMF can address", hasError(b, "longer")); }

    // Malformed input.
    auto bytes = w1.bytes;
    TEST("parse rejects empty", !parse({}).ok);
    TEST("parse rejects truncated header", !parse({bytes.begin(), bytes.begin() + 10}).ok);
    TEST("parse rejects truncated track", !parse({bytes.begin(), bytes.end() - 5}).ok);
    { auto b = bytes; b[9] = 1; TEST("parse rejects format 1", !parse(b).ok); }
    { auto b = bytes; b[12] = 0xE7; TEST("parse rejects SMPTE division", !parse(b).ok); }
    { auto b = bytes; b[18] = 0x7F; TEST("parse rejects MTrk length past EOF", !parse(b).ok); }
    { auto b = bytes; b.erase(b.end() - 3, b.end()); b[21] -= 3;   // drop End of Track, keep sizes consistent
      TEST("parse rejects missing End of Track", !parse(b).ok); }
    { auto b = bytes;   // unknown chunk appended after the track: skipped, reported
      b.insert(b.end(), {'X', 'Y', 'Z', 'W', 0, 0, 0, 2, 1, 2});
      const auto p = parse(b);
      TEST("parse skips unknown chunk", p.ok && same(p.style, s) && !p.notes.empty()); }
    { std::vector<uint8_t> b(bytes.begin(), bytes.begin() + 22);   // MThd + MTrk header
      b.insert(b.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF, 0x2F, 0x00});   // 5-byte delta
      const uint32_t n = static_cast<uint32_t>(b.size() - 22);
      b[18] = 0; b[19] = 0; b[20] = 0; b[21] = static_cast<uint8_t>(n);
      TEST("parse rejects over-long delta time", !parse(b).ok); }
    { // Running status and a SysEx are legal SMF the Pa5X itself exports.
      std::vector<uint8_t> trk = {0x00, 0xFF, 0x06, 5, 'v', '1', 'c', 'v', '1',
                                  0x00, 0xFF, 0x58, 4, 4, 2, 24, 8,
                                  0x00, 0xF0, 0x03, 0x42, 0x30, 0xF7,
                                  0x00, 0x99, 36, 100, 0x60, 36, 0,   // running status NoteOn v0
                                  0x8E, 0x20, 0xFF, 0x2F, 0x00};      // +1824 -> tick 1920
      std::vector<uint8_t> b = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                'M', 'T', 'r', 'k', 0, 0, 0, static_cast<uint8_t>(trk.size())};
      b.insert(b.end(), trk.begin(), trk.end());
      const auto p = parse(b);
      TEST("parse handles running status + SysEx", p.ok && p.style.elements.size() == 1 &&
           p.style.elements[0].events.size() == 2 && p.style.elements[0].length == 1920 &&
           validate(p.style).empty()); }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
