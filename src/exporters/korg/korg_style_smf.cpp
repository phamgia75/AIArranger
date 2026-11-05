#include "exporters/korg/korg_style_smf.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace ai_arranger::exporters::korg {

namespace {
constexpr uint32_t kMaxVlq = 0x0FFFFFFF;

bool isPow2Den(uint8_t d) { return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32; }

uint8_t log2Den(uint8_t d) {
    uint8_t n = 0;
    while (d > 1) { d >>= 1; ++n; }
    return n;
}

void vlq(std::vector<uint8_t>& o, uint32_t v) {
    uint8_t buf[4];
    int n = 0;
    buf[n++] = v & 0x7F;
    while ((v >>= 7) != 0) buf[n++] = 0x80 | (v & 0x7F);
    while (n) o.push_back(buf[--n]);
}

void be32(std::vector<uint8_t>& o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(static_cast<uint8_t>(v >> s));
}
} // namespace

bool isValidMarker(std::string_view m) noexcept {
    // "EnCVn", lowercase only: E in {v,i,f,e}; element 1-4 for variations, 1-2
    // otherwise; "cv"; chord variation 1-6 for variations, 1-2 otherwise.
    if (m.size() != 5 || m[2] != 'c' || m[3] != 'v') return false;
    const bool var = m[0] == 'v';
    if (!var && m[0] != 'i' && m[0] != 'f' && m[0] != 'e') return false;
    const char maxEl = var ? '4' : '2', maxCv = var ? '6' : '2';
    return m[1] >= '1' && m[1] <= maxEl && m[4] >= '1' && m[4] <= maxCv;
}

std::vector<std::string> validate(const Style& s) {
    std::vector<std::string> err;
    if (s.ppqn == 0 || s.ppqn > 0x7FFF) err.push_back("PPQN " + std::to_string(s.ppqn) + " out of range 1..32767");
    if (s.mpqn > 0xFFFFFF) err.push_back("tempo out of range");
    if (s.elements.empty()) err.push_back("no style elements");

    std::set<std::string> seen;
    uint64_t total = 0;
    for (const auto& e : s.elements) {
        const std::string at = "element '" + e.marker + "': ";
        if (!isValidMarker(e.marker)) err.push_back(at + "invalid marker name (Korg EnCVn, lowercase)");
        if (!seen.insert(e.marker).second) err.push_back(at + "duplicate marker");
        if (e.num == 0 || !isPow2Den(e.den)) {
            err.push_back(at + "missing or bad time signature");
        } else if (s.ppqn) {
            const uint64_t barNum = uint64_t{s.ppqn} * 4 * e.num;
            if (barNum % e.den) err.push_back(at + "bar is not a whole number of ticks");
            else if (e.length == 0 || e.length % (barNum / e.den))
                err.push_back(at + "length is not a whole number of bars");
        }
        total += e.length;

        uint32_t prev = 0;
        std::map<uint16_t, int> open;   // (channel << 8 | note) -> sounding count
        std::set<std::string> seenMsg;  // each distinct event error once per element
        auto bad = [&](const std::string& m) { if (seenMsg.insert(m).second) err.push_back(at + m); };
        for (const auto& ev : e.events) {
            const uint8_t hi = ev.status & 0xF0, ch = ev.status & 0x0F;
            if (hi < 0x80 || hi == 0xF0) { bad("non channel-voice event"); continue; }
            if (ev.d1 > 0x7F || ev.d2 > 0x7F) bad("data byte above 127");
            if (ev.tick < prev) bad("events not sorted by tick");
            prev = ev.tick;
            if (ev.tick >= e.length) bad("event at or after the element end (Pa appends a measure)");
            if (ch < kBassCh) bad("event on MIDI channel " + std::to_string(ch + 1) + " (Korg imports 9-16 only)");
            if ((hi == 0xC0 || (hi == 0xB0 && (ev.d1 == 0 || ev.d1 == 32))) && ev.tick != 0)
                bad("bank select / program change not at tick 0");
            const bool on = hi == 0x90 && ev.d2 > 0;
            const bool off = hi == 0x80 || (hi == 0x90 && ev.d2 == 0);
            const uint16_t key = static_cast<uint16_t>(ch << 8 | ev.d1);
            if (on && isChordDriven(ch) && ev.d1 <= kChordEventNoteMax)
                bad("note " + std::to_string(ev.d1) + " on a chord-driven track would import as a Chord event");
            if (on) ++open[key];
            else if (off) {
                auto it = open.find(key);
                if (it == open.end() || it->second == 0) bad("NoteOff without a sounding note");
                else --it->second;
            }
        }
        for (const auto& [k, n] : open)
            if (n) { bad("note still sounding at the element end"); break; }
    }
    if (total > kMaxVlq) err.push_back("style longer than an SMF can address");
    return err;
}

WriteResult write(const Style& s) {
    WriteResult r;
    const auto errs = validate(s);
    if (!errs.empty()) { r.error = "invalid style: " + errs.front(); return r; }

    std::vector<uint8_t> trk;
    uint32_t now = 0;
    auto at = [&](uint32_t abs) { vlq(trk, abs - now); now = abs; };

    if (s.mpqn) {
        at(0);
        trk.insert(trk.end(), {0xFF, 0x51, 0x03, static_cast<uint8_t>(s.mpqn >> 16),
                               static_cast<uint8_t>(s.mpqn >> 8), static_cast<uint8_t>(s.mpqn)});
    }
    uint32_t base = 0;   // validate() bounds the sum by kMaxVlq
    for (const auto& e : s.elements) {
        at(base);
        trk.insert(trk.end(), {0xFF, 0x06, static_cast<uint8_t>(e.marker.size())});
        trk.insert(trk.end(), e.marker.begin(), e.marker.end());
        at(base);
        trk.insert(trk.end(), {0xFF, 0x58, 0x04, e.num, log2Den(e.den), 24, 8});
        for (const auto& ev : e.events) {
            at(base + ev.tick);
            trk.push_back(ev.status);   // no running status: simpler, still valid SMF
            trk.push_back(ev.d1);
            const uint8_t hi = ev.status & 0xF0;
            if (hi != 0xC0 && hi != 0xD0) trk.push_back(ev.d2);
        }
        base += e.length;
    }
    at(base);
    trk.insert(trk.end(), {0xFF, 0x2F, 0x00});
    if (trk.size() > 0xFFFFFFFFull) { r.error = "track too large"; return r; }

    r.bytes = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1,
               static_cast<uint8_t>(s.ppqn >> 8), static_cast<uint8_t>(s.ppqn)};
    r.bytes.insert(r.bytes.end(), {'M', 'T', 'r', 'k'});
    be32(r.bytes, static_cast<uint32_t>(trk.size()));
    r.bytes.insert(r.bytes.end(), trk.begin(), trk.end());
    r.ok = true;
    return r;
}

ParseResult parse(const std::vector<uint8_t>& b) {
    ParseResult r;
    auto fail = [&](std::string m) { r.ok = false; r.error = std::move(m); return r; };
    auto rd32 = [&](size_t p) {
        return uint32_t{b[p]} << 24 | uint32_t{b[p + 1]} << 16 | uint32_t{b[p + 2]} << 8 | b[p + 3];
    };

    if (b.size() < 14 || std::string(b.begin(), b.begin() + 4) != "MThd") return fail("not an SMF (no MThd)");
    const uint32_t hlen = rd32(4);
    if (hlen < 6 || hlen > b.size() - 8) return fail("bad MThd length");
    const uint16_t format = static_cast<uint16_t>(b[8] << 8 | b[9]);
    const uint16_t ntrks = static_cast<uint16_t>(b[10] << 8 | b[11]);
    const uint16_t division = static_cast<uint16_t>(b[12] << 8 | b[13]);
    if (format != 0) return fail("SMF format " + std::to_string(format) + " (Korg imports format 0 only)");
    if (ntrks != 1) return fail("format 0 SMF must have exactly one track");
    if (division & 0x8000) return fail("SMPTE time division not supported");
    r.style.ppqn = division;

    struct Marker { uint64_t tick; std::string name; };
    struct Abs { uint64_t tick; Event ev; };
    std::vector<Marker> markers;
    std::map<uint64_t, std::pair<uint8_t, uint8_t>> timeSigs;
    std::vector<Abs> events;
    bool haveTrack = false, haveEot = false;
    uint64_t eot = 0;
    bool tempoSeen = false;
    uint32_t sysex = 0;

    size_t p = 8 + hlen;
    while (p < b.size()) {
        if (b.size() - p < 8) return fail("truncated chunk header");
        const std::string id(b.begin() + p, b.begin() + p + 4);
        const uint32_t len = rd32(p + 4);
        p += 8;
        if (len > b.size() - p) return fail("chunk '" + id + "' length runs past end of file");
        if (id != "MTrk") {
            r.notes.push_back("unknown chunk '" + id + "' skipped");
            p += len;
            continue;
        }
        if (haveTrack) return fail("more than one MTrk");
        haveTrack = true;

        const size_t end = p + len;
        uint64_t abs = 0;
        uint8_t running = 0;
        auto readVlq = [&](uint32_t& v) {
            v = 0;
            for (int i = 0; i < 4; ++i) {
                if (p >= end) return false;
                const uint8_t c = b[p++];
                v = v << 7 | (c & 0x7F);
                if (!(c & 0x80)) return true;
            }
            return false;   // longer than 4 bytes
        };
        while (p < end && !haveEot) {
            uint32_t delta;
            if (!readVlq(delta)) return fail("bad delta time");
            abs += delta;
            if (abs > kMaxVlq) return fail("track longer than an SMF can address");
            if (p >= end) return fail("truncated event");
            uint8_t st = b[p];
            if (st & 0x80) ++p;
            else if (running) st = running;
            else return fail("running status with no previous status");

            if (st == 0xFF) {
                running = 0;
                if (p >= end) return fail("truncated meta event");
                const uint8_t type = b[p++];
                uint32_t mlen;
                if (!readVlq(mlen) || mlen > end - p) return fail("bad meta event length");
                const uint8_t* d = &b[p];
                if (type == 0x06) markers.push_back({abs, std::string(d, d + mlen)});
                else if (type == 0x58 && mlen >= 4) timeSigs[abs] = {d[0], d[1] < 8 ? static_cast<uint8_t>(1u << d[1]) : uint8_t{0}};
                else if (type == 0x51 && mlen == 3 && !tempoSeen) {
                    r.style.mpqn = uint32_t{d[0]} << 16 | uint32_t{d[1]} << 8 | d[2];
                    tempoSeen = true;
                } else if (type == 0x2F) { haveEot = true; eot = abs; }
                p += mlen;
            } else if (st == 0xF0 || st == 0xF7) {
                running = 0;
                uint32_t slen;
                if (!readVlq(slen) || slen > end - p) return fail("bad SysEx length");
                p += slen;
                ++sysex;
            } else if (st >= 0xF1) {
                return fail("system message in SMF track");
            } else {
                running = st;
                const uint8_t hi = st & 0xF0;
                const size_t n = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
                if (end - p < n) return fail("truncated channel event");
                Event ev{0, st, b[p], n == 2 ? b[p + 1] : uint8_t{0}};
                if (ev.d1 > 0x7F || ev.d2 > 0x7F) return fail("data byte above 127");
                p += n;
                events.push_back({abs, ev});
            }
        }
        if (p < end) r.notes.push_back("bytes after End of Track ignored");
        p = end;
    }
    if (!haveTrack) return fail("no MTrk chunk");
    if (!haveEot) return fail("missing End of Track");
    if (sysex) r.notes.push_back(std::to_string(sysex) + " SysEx event(s) skipped");

    std::stable_sort(markers.begin(), markers.end(),
                     [](const Marker& a, const Marker& x) { return a.tick < x.tick; });
    size_t ei = 0, stray = 0;
    for (size_t i = 0; i < markers.size(); ++i) {
        const uint64_t start = markers[i].tick;
        const uint64_t stop = i + 1 < markers.size() ? markers[i + 1].tick : eot;
        Element el;
        el.marker = markers[i].name;
        el.length = static_cast<uint32_t>(stop > start ? stop - start : 0);
        auto ts = timeSigs.find(start);
        if (ts != timeSigs.end()) { el.num = ts->second.first; el.den = ts->second.second; }
        else { el.num = 0; el.den = 0; }
        for (; ei < events.size() && events[ei].tick < stop; ++ei) {
            if (events[ei].tick < start) { ++stray; continue; }
            Event ev = events[ei].ev;
            ev.tick = static_cast<uint32_t>(events[ei].tick - start);
            el.events.push_back(ev);
        }
        r.style.elements.push_back(std::move(el));
    }
    stray += events.size() - ei;
    if (stray) r.notes.push_back(std::to_string(stray) + " channel event(s) outside any marker ignored");
    r.ok = true;
    return r;
}

namespace {
struct CmpNote { uint8_t ch, pitch, vel; uint64_t on, off; };

// Notes of one element at resolution q: FIFO pairing per (channel, pitch);
// a note still sounding at the end is closed there, a stray NoteOff ignored.
std::vector<CmpNote> notesAt(const Element& e, uint16_t from, uint16_t q) {
    auto sc = [&](uint64_t t) { return (t * q + from / 2) / from; };
    std::vector<CmpNote> out;
    std::map<uint16_t, std::vector<size_t>> open;
    for (const auto& ev : e.events) {
        const uint8_t hi = ev.status & 0xF0, ch = ev.status & 0x0F;
        const uint16_t k = static_cast<uint16_t>(ch << 8 | ev.d1);
        // Pa5X exports Chord events as notes C-1..B-1 on chord-driven tracks
        // (New Features p.46): they are chords, not style notes.
        if (isChordDriven(ch) && ev.d1 <= kChordEventNoteMax) continue;
        if (hi == 0x90 && ev.d2) {
            open[k].push_back(out.size());
            out.push_back({ch, ev.d1, ev.d2, sc(ev.tick), UINT64_MAX});
        } else if ((hi == 0x80 || hi == 0x90) && !open[k].empty()) {
            out[open[k].front()].off = sc(ev.tick);
            open[k].erase(open[k].begin());
        }
    }
    for (auto& n : out) if (n.off == UINT64_MAX) n.off = sc(e.length);
    std::stable_sort(out.begin(), out.end(), [](const CmpNote& x, const CmpNote& y) {
        return std::tie(x.ch, x.pitch, x.on) < std::tie(y.ch, y.pitch, y.on);
    });
    return out;
}

bool near1(uint64_t x, uint64_t y) { return x > y ? x - y <= 1 : y - x <= 1; }
} // namespace

CompareResult compareStyles(const Style& a, const Style& b) {
    CompareResult r;
    if (!a.ppqn || !b.ppqn) { r.differences.push_back("PPQN 0"); return r; }
    const uint16_t q = std::min(a.ppqn, b.ppqn);
    if (a.ppqn != b.ppqn)
        r.info.push_back("PPQN differs (" + std::to_string(a.ppqn) + " vs " + std::to_string(b.ppqn) +
                         "); compared at " + std::to_string(q) + " with 1-tick tolerance");
    if (a.mpqn != b.mpqn)
        r.info.push_back("tempo event differs (" + std::to_string(a.mpqn) + " vs " + std::to_string(b.mpqn) + " us/q)");

    for (const auto& ea : a.elements) {
        const Element* eb = nullptr;
        for (const auto& x : b.elements) if (x.marker == ea.marker) { eb = &x; break; }
        const std::string at = ea.marker + ": ";
        if (!eb) { r.differences.push_back(at + "missing from the Pa5X export"); continue; }
        if (ea.num != eb->num || ea.den != eb->den)
            r.differences.push_back(at + "meter " + std::to_string(ea.num) + "/" + std::to_string(ea.den) + " vs " +
                                    std::to_string(eb->num) + "/" + std::to_string(eb->den));
        const uint64_t la = (uint64_t{ea.length} * q + a.ppqn / 2) / a.ppqn;
        const uint64_t lb = (uint64_t{eb->length} * q + b.ppqn / 2) / b.ppqn;
        if (la != lb) r.differences.push_back(at + "length " + std::to_string(la) + " vs " + std::to_string(lb) + " ticks");
        const auto na = notesAt(ea, a.ppqn, q), nb = notesAt(*eb, b.ppqn, q);
        if (na.size() != nb.size()) {
            r.differences.push_back(at + std::to_string(na.size()) + " notes vs " + std::to_string(nb.size()));
            continue;
        }
        size_t bad = 0;
        std::string first;
        for (size_t i = 0; i < na.size(); ++i) {
            const auto &x = na[i], &y = nb[i];
            if (x.ch == y.ch && x.pitch == y.pitch && x.vel == y.vel && near1(x.on, y.on) && near1(x.off, y.off)) continue;
            if (!bad++)
                first = "ch " + std::to_string(x.ch + 1) + " note " + std::to_string(x.pitch) + " @" +
                        std::to_string(x.on) + " v" + std::to_string(x.vel) + " vs ch " + std::to_string(y.ch + 1) +
                        " note " + std::to_string(y.pitch) + " @" + std::to_string(y.on) + " v" + std::to_string(y.vel);
        }
        if (bad) r.differences.push_back(at + std::to_string(bad) + " notes differ (first: " + first + ")");
    }
    for (const auto& eb : b.elements)
        if (std::none_of(a.elements.begin(), a.elements.end(), [&](const Element& x) { return x.marker == eb.marker; }))
            r.info.push_back("element only in the Pa5X export: '" + eb.marker + "' (" + std::to_string(eb.events.size()) + " events)");
    return r;
}

} // namespace ai_arranger::exporters::korg
