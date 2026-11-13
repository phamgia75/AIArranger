#include "style_edit/style_editor.h"

#include "engine/uasf/serializer.h"
#include "exporters/korg/genos_to_korg.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <deque>
#include <map>
#include <set>
#include <tuple>

namespace ai_arranger::style_edit {

using uasf::MidiEvent;
using uasf::MidiEventType;
using uasf::SectionDefinition;
using uasf::StyleDefinition;
using uasf::TrackDefinition;
using uasf::KorgSound;

namespace {

bool isOn(const MidiEvent& e) { return e.type == MidiEventType::NoteOn && e.data2 > 0; }
bool isOff(const MidiEvent& e) {
    return e.type == MidiEventType::NoteOff || (e.type == MidiEventType::NoteOn && e.data2 == 0);
}

// partner[i] = index of the other end of event i's note, or -1 (FIFO per channel+pitch,
// in array order). An orphan NoteOn / NoteOff keeps -1.
std::vector<long> partners(const std::vector<MidiEvent>& ev) {
    std::vector<long> p(ev.size(), -1);
    std::map<int, std::deque<size_t>> open;
    for (size_t i = 0; i < ev.size(); ++i) {
        const int key = ev[i].channel << 8 | ev[i].data1;
        if (isOn(ev[i])) open[key].push_back(i);
        else if (isOff(ev[i]) && !open[key].empty()) {
            const size_t on = open[key].front();
            open[key].pop_front();
            p[on] = static_cast<long>(i);
            p[i] = static_cast<long>(on);
        }
    }
    return p;
}

int rank(const MidiEvent& e) { return isOff(e) ? 0 : isOn(e) ? 2 : 1; }

// After an op changed ticks in place (array order = the original pairing) or
// appended whole notes: give every note at least one tick, end notes at the
// element end at the latest, check bounds, then sort (NoteOff before other
// events before NoteOn at one tick, so a re-struck pitch pairs FIFO).
std::string finishTrack(std::vector<MidiEvent>& ev, uint64_t len) {
    const auto p = partners(ev);
    for (size_t i = 0; i < ev.size(); ++i)
        if (isOn(ev[i]) && p[i] >= 0 && ev[p[i]].tick <= ev[i].tick) ev[p[i]].tick = ev[i].tick + 1;
    for (auto& e : ev) {
        const auto type = static_cast<uint8_t>(e.type);
        if (type < 0x80 || type >= 0xF0) return "not a channel voice event";
        if (e.data1 > 127 || e.data2 > 127) return "data byte above 127";
        if (isOff(e)) e.tick = std::min(e.tick, len);
        else if (e.tick >= len)
            return "event at tick " + std::to_string(e.tick) + " is at or past the element end (" +
                   std::to_string(len) + ")";
    }
    std::stable_sort(ev.begin(), ev.end(), [](const MidiEvent& a, const MidiEvent& b) {
        return std::make_tuple(a.tick, rank(a)) < std::make_tuple(b.tick, rank(b));
    });
    return {};
}

bool inRange(const Selection& s, uint64_t t) { return t >= s.from && t < s.to; }

bool isDrumTrack(const TrackDefinition& t) {
    return t.is_drum || t.role == uasf::TrackRole::Drum || t.role == uasf::TrackRole::Percussion;
}

// Call fn on every selected track; stops at the first error.
std::string forEachTrack(StyleDefinition& s, const Selection& sel,
                         const std::function<std::string(SectionDefinition&, TrackDefinition&)>& fn) {
    if (sel.from >= sel.to) return "empty tick range";
    if (sel.section && *sel.section >= s.sections.size()) return "no section " + std::to_string(*sel.section);
    for (size_t si = 0; si < s.sections.size(); ++si) {
        if (sel.section && si != *sel.section) continue;
        auto& sec = s.sections[si];
        if (sel.track && *sel.track >= sec.tracks.size())
            return "section " + std::to_string(si) + " has no track " + std::to_string(*sel.track);
        for (size_t ti = 0; ti < sec.tracks.size(); ++ti) {
            if (sel.track && ti != *sel.track) continue;
            if (auto e = fn(sec, sec.tracks[ti]); !e.empty())
                return "section " + std::to_string(si) + " track " + std::to_string(ti) + ": " + e;
        }
    }
    return {};
}

std::string trackAt(StyleDefinition& s, size_t section, size_t track, TrackDefinition*& out) {
    if (section >= s.sections.size()) return "no section " + std::to_string(section);
    if (track >= s.sections[section].tracks.size())
        return "section " + std::to_string(section) + " has no track " + std::to_string(track);
    out = &s.sections[section].tracks[track];
    return {};
}

// Selected indices plus the other end of every selected note.
std::string withPartners(const std::vector<MidiEvent>& ev, const std::vector<size_t>& idx, std::set<size_t>& out) {
    const auto p = partners(ev);
    for (size_t i : idx) {
        if (i >= ev.size()) return "no event " + std::to_string(i);
        out.insert(i);
        if (p[i] >= 0) out.insert(static_cast<size_t>(p[i]));
    }
    return {};
}

// Refusal when an event of the element lies at or past its end (NoteOffs may sit on it).
std::string pastEnd(const SectionDefinition& sec) {
    const uint64_t len = sectionLengthTicks(sec);
    if (len == 0) return {};   // a 0-tick bar: the UASF validator rejects it
    for (const auto& t : sec.tracks)
        for (const auto& e : t.events)
            if (isOff(e) ? e.tick > len : e.tick >= len)
                return "track '" + t.name + "' has events past the new element end (" + std::to_string(len) +
                       " ticks); delete them first";
    return {};
}

std::string squash(const std::string& s) {   // lower case, no spaces: "Variation 1" -> "variation1"
    std::string o;
    for (char c : s)
        if (c != ' ') o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

uasf::ValidationIssue firstError(const std::vector<uasf::ValidationIssue>& issues) {
    for (const auto& i : issues)
        if (i.severity == uasf::ValidationIssue::ERROR) return i;
    return {};
}

} // namespace

uint32_t gridTicks(uint32_t resolution, const std::string& grid) {
    static const std::map<std::string, uint32_t> kDiv = {{"1/4", 4}, {"1/8", 8}, {"1/16", 16}, {"1/32", 32}};
    const bool triplet = !grid.empty() && grid.back() == 't';
    const auto it = kDiv.find(triplet ? grid.substr(0, grid.size() - 1) : grid);
    if (it == kDiv.end() || resolution == 0) return 0;
    const uint64_t num = uint64_t{resolution} * 4 * (triplet ? 2 : 3);
    const uint64_t den = uint64_t{it->second} * 3;
    return num % den ? 0 : static_cast<uint32_t>(num / den);
}

uint64_t sectionLengthTicks(const SectionDefinition& s) {
    if (s.beat_note == 0) return 0;
    return uint64_t{s.bars} * s.resolution * 4 * s.beats_per_bar / s.beat_note;
}

const char* sectionTypeName(uasf::SectionType t) noexcept {
    static const char* n[] = {"Intro 1", "Intro 2", "Intro 3", "Variation 1", "Variation 2", "Variation 3", "Variation 4",
                              "Fill 1", "Fill 2", "Fill 3", "Fill 4", "Ending 1", "Ending 2", "Ending 3", "Break"};
    const auto i = static_cast<size_t>(t);
    return i < std::size(n) ? n[i] : "?";
}

std::optional<uasf::SectionType> sectionTypeFromName(const std::string& name) {
    std::string k = squash(name);
    if (k.rfind("main", 0) == 0) k = "variation" + k.substr(4);
    for (int i = 0; i <= static_cast<int>(uasf::SectionType::Break); ++i)
        if (squash(sectionTypeName(static_cast<uasf::SectionType>(i))) == k) return static_cast<uasf::SectionType>(i);
    return std::nullopt;
}

const char* trackRoleName(uasf::TrackRole r) noexcept {
    static const char* n[] = {"accompaniment", "chord", "bass", "drum", "percussion",
                              "phrase1", "phrase2", "pad", "melody", "articulation"};
    const auto i = static_cast<size_t>(r);
    return i < std::size(n) ? n[i] : "?";
}

std::optional<uasf::TrackRole> trackRoleFromName(const std::string& name) {
    for (int i = 0; i <= static_cast<int>(uasf::TrackRole::Articulation); ++i)
        if (squash(name) == trackRoleName(static_cast<uasf::TrackRole>(i))) return static_cast<uasf::TrackRole>(i);
    return std::nullopt;
}

std::string saveError(const StyleDefinition& s) {
    if (s.sections.size() > uasf::format::kMaxSections)
        return "cannot be kept as UASF: " + std::to_string(s.sections.size()) + " elements, UASF holds at most " +
               std::to_string(uasf::format::kMaxSections);
    for (size_t i = 0; i < s.sections.size(); ++i)
        if (s.sections[i].tracks.size() > uasf::format::kMaxTracksPerSection)
            return "cannot be kept as UASF: element " + std::to_string(i) + " has " + std::to_string(s.sections[i].tracks.size()) +
                   " tracks, UASF holds at most " + std::to_string(uasf::format::kMaxTracksPerSection);
    const auto w = uasf::UasfSerializer().serialize(s);
    if (!w.success) return "cannot be kept as UASF: " + w.error;
    if (w.data.size() > uasf::format::kMaxFileSize)   // the reader refuses it: a save would never reopen
        return "cannot be kept as UASF: " + std::to_string((w.data.size() + (1u << 20) - 1) >> 20) + " MiB, a UASF file opens only up to " +
               std::to_string(uasf::format::kMaxFileSize >> 20) + " MiB";
    return {};
}

StyleEditor::StyleEditor(StyleDefinition style, size_t undoLimit)
    : style_(std::move(style)), history_(undoLimit) {}

EditResult StyleEditor::apply(const std::string& label, const Mutator& m) {
    EditResult r;
    StyleDefinition next = style_;
    if (auto e = m(next, r); !e.empty()) { r.error = e; return r; }
    uasf::UasfValidator v;
    auto res = v.validate(next);
    r.issues = std::move(res.issues);
    if (!res.valid) {
        const auto e = firstError(r.issues);
        r.error = "UASF validator: " + e.message + (e.location.empty() ? "" : " (" + e.location + ")");
        return r;
    }
    if (auto e = saveError(next); !e.empty()) { r.error = std::move(e); return r; }   // e.g. a 33rd element
    history_.record(label, std::move(style_));
    style_ = std::move(next);
    r.ok = true;
    return r;
}

EditResult StyleEditor::quantize(const Selection& sel, uint32_t grid, uint32_t strength) {
    if (grid == 0) return {false, "grid must be > 0 ticks"};
    if (strength > 100) return {false, "strength must be 0..100"};
    return apply("Quantize", [&](StyleDefinition& s, EditResult& r) {
        return forEachTrack(s, sel, [&](SectionDefinition& sec, TrackDefinition& t) {
            const uint64_t len = sectionLengthTicks(sec);
            auto& ev = t.events;
            const auto p = partners(ev);
            std::vector<int64_t> delta(ev.size(), 0);
            for (size_t i = 0; i < ev.size(); ++i) {
                if (!isOn(ev[i]) || !inRange(sel, ev[i].tick)) continue;
                const uint64_t tk = ev[i].tick;
                uint64_t q = (tk + grid / 2) / grid * grid;
                if (q >= len) q = tk / grid * grid;
                const int64_t d = (static_cast<int64_t>(q) - static_cast<int64_t>(tk)) * strength / 100;
                delta[i] = d;
                if (p[i] >= 0) delta[p[i]] = d;
            }
            bool touched = false;
            for (size_t i = 0; i < ev.size(); ++i) {
                if (!delta[i]) continue;
                ev[i].tick = static_cast<uint64_t>(static_cast<int64_t>(ev[i].tick) + delta[i]);
                if (isOn(ev[i])) ++r.changed;
                touched = true;
            }
            return touched ? finishTrack(ev, len) : std::string();   // untouched tracks stay as they are
        });
    });
}

EditResult StyleEditor::transpose(const Selection& sel, int semis) {
    if (semis < -127 || semis > 127) return {false, "transpose out of range"};
    return apply("Transpose", [&](StyleDefinition& s, EditResult& r) {
        return forEachTrack(s, sel, [&](SectionDefinition& sec, TrackDefinition& t) -> std::string {
            if (isDrumTrack(t)) { ++r.skipped_tracks; return {}; }
            auto& ev = t.events;
            const auto p = partners(ev);
            std::vector<bool> hit(ev.size(), false);
            for (size_t i = 0; i < ev.size(); ++i) {
                const bool poly = ev[i].type == MidiEventType::PolyPressure;
                if (!(isOn(ev[i]) || poly) || !inRange(sel, ev[i].tick)) continue;
                hit[i] = true;
                if (p[i] >= 0) hit[p[i]] = true;
            }
            for (size_t i = 0; i < ev.size(); ++i) {
                if (!hit[i]) continue;
                const int n = ev[i].data1 + semis;
                if (n < 0 || n > 127) return "note " + std::to_string(ev[i].data1) + " would leave 0..127";
                ev[i].data1 = static_cast<uint8_t>(n);
                if (isOn(ev[i])) ++r.changed;
            }
            const bool touched = std::find(hit.begin(), hit.end(), true) != hit.end();
            return touched ? finishTrack(ev, sectionLengthTicks(sec)) : std::string();
        });
    });
}

EditResult StyleEditor::velocity(const Selection& sel, VelocityMode mode, int value) {
    if (mode == VelocityMode::ScalePercent && (value < 0 || value > 1000)) return {false, "scale must be 0..1000%"};
    return apply("Velocity", [&](StyleDefinition& s, EditResult& r) {
        return forEachTrack(s, sel, [&](SectionDefinition&, TrackDefinition& t) {
            for (auto& e : t.events) {
                if (!isOn(e) || !inRange(sel, e.tick)) continue;
                int v = mode == VelocityMode::Set      ? value
                        : mode == VelocityMode::Offset ? e.data2 + value
                                                       : (e.data2 * value + 50) / 100;
                v = std::clamp(v, 1, 127);
                if (v != e.data2) { e.data2 = static_cast<uint8_t>(v); ++r.changed; }
            }
            return std::string();
        });
    });
}

EditResult StyleEditor::setTempo(uint32_t bpm) {
    if (bpm < uasf::kMinTempoBpm || bpm > uasf::kMaxTempoBpm)
        return {false, "tempo must be " + std::to_string(uasf::kMinTempoBpm) + ".." +
                           std::to_string(uasf::kMaxTempoBpm) + " BPM"};
    return apply("Tempo", [&](StyleDefinition& s, EditResult& r) {
        r.changed = s.tempo_bpm != bpm;
        s.tempo_bpm = bpm;
        return std::string();
    });
}

EditResult StyleEditor::setMeter(size_t section, uint32_t num, uint32_t den) {
    if (num > 255) return {false, "numerator must be 1..255"};
    if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32)
        return {false, "denominator must be 1, 2, 4, 8, 16 or 32"};
    return apply("Meter", [&](StyleDefinition& s, EditResult& r) -> std::string {
        if (section >= s.sections.size()) return "no section " + std::to_string(section);
        auto& sec = s.sections[section];
        sec.beats_per_bar = num;
        sec.beat_note = den;
        r.changed = 1;
        return pastEnd(sec);
    });
}

EditResult StyleEditor::insertEvent(size_t section, size_t track, MidiEvent ev, uint64_t noteLength) {
    const uint8_t type = static_cast<uint8_t>(ev.type);
    if (type < 0x80 || type >= 0xF0) return {false, "only channel voice events can be inserted"};
    if (isOff(ev)) return {false, "insert a note as a NoteOn with a length"};
    if (isOn(ev) && noteLength == 0) return {false, "a NoteOn needs a length > 0"};
    return apply("Insert event", [&](StyleDefinition& s, EditResult& r) -> std::string {
        TrackDefinition* t = nullptr;
        if (auto e = trackAt(s, section, track, t); !e.empty()) return e;
        const uint64_t len = sectionLengthTicks(s.sections[section]);
        ev.channel = t->midi_channel;
        if (ev.type == MidiEventType::ProgramChange || ev.type == MidiEventType::ChannelPressure) ev.data2 = 0;
        if (ev.tick >= len) return "tick " + std::to_string(ev.tick) + " is at or past the element end";
        t->events.push_back(ev);
        if (isOn(ev)) t->events.push_back({MidiEventType::NoteOff, ev.channel, ev.data1, 0, ev.tick + noteLength});
        r.changed = 1;
        return finishTrack(t->events, len);
    });
}

EditResult StyleEditor::deleteEvents(size_t section, size_t track, const std::vector<size_t>& indices) {
    return apply("Delete events", [&](StyleDefinition& s, EditResult& r) -> std::string {
        TrackDefinition* t = nullptr;
        if (auto e = trackAt(s, section, track, t); !e.empty()) return e;
        std::set<size_t> del;
        if (auto e = withPartners(t->events, indices, del); !e.empty()) return e;
        for (auto it = del.rbegin(); it != del.rend(); ++it) t->events.erase(t->events.begin() + *it);
        r.changed = del.size();
        return {};
    });
}

EditResult StyleEditor::moveEvents(size_t section, size_t track, const std::vector<size_t>& indices, int64_t delta) {
    return apply("Move events", [&](StyleDefinition& s, EditResult& r) -> std::string {
        TrackDefinition* t = nullptr;
        if (auto e = trackAt(s, section, track, t); !e.empty()) return e;
        std::set<size_t> mv;
        if (auto e = withPartners(t->events, indices, mv); !e.empty()) return e;
        for (size_t i : mv) {
            const int64_t nt = static_cast<int64_t>(t->events[i].tick) + delta;
            if (nt < 0) return "event " + std::to_string(i) + " would move before the element start";
            t->events[i].tick = static_cast<uint64_t>(nt);
        }
        r.changed = mv.size();
        return finishTrack(t->events, sectionLengthTicks(s.sections[section]));
    });
}

std::optional<Clip> StyleEditor::copy(size_t section, size_t track, uint64_t from, uint64_t to,
                                      std::string* error) const {
    auto fail = [&](std::string m) -> std::optional<Clip> { if (error) *error = std::move(m); return std::nullopt; };
    if (from >= to) return fail("empty tick range");
    if (section >= style_.sections.size()) return fail("no section " + std::to_string(section));
    const auto& sec = style_.sections[section];
    if (track >= sec.tracks.size()) return fail("section " + std::to_string(section) + " has no track " + std::to_string(track));
    const auto& ev = sec.tracks[track].events;
    const auto p = partners(ev);
    Clip c;
    c.resolution = sec.resolution;
    for (size_t i = 0; i < ev.size(); ++i) {
        const bool orphanOff = isOff(ev[i]) && p[i] < 0;
        bool take = !isOff(ev[i]) && ev[i].tick >= from && ev[i].tick < to;
        if (isOff(ev[i]) && p[i] >= 0) take = ev[p[i]].tick >= from && ev[p[i]].tick < to;   // follows its NoteOn
        if (!take || orphanOff) continue;
        MidiEvent e = ev[i];
        e.tick -= from;
        c.events.push_back(e);
    }
    return c;
}

EditResult StyleEditor::paste(size_t section, size_t track, uint64_t at, const Clip& clip) {
    if (clip.resolution == 0) return {false, "clip has no resolution"};
    return apply("Paste", [&](StyleDefinition& s, EditResult& r) -> std::string {
        TrackDefinition* t = nullptr;
        if (auto e = trackAt(s, section, track, t); !e.empty()) return e;
        const auto& sec = s.sections[section];
        for (auto e : clip.events) {
            e.tick = at + (e.tick * sec.resolution + clip.resolution / 2) / clip.resolution;
            e.channel = t->midi_channel;
            t->events.push_back(e);
        }
        r.changed = clip.events.size();
        return finishTrack(t->events, sectionLengthTicks(sec));
    });
}

EditResult StyleEditor::replaceTrackEvents(size_t section, size_t track, std::vector<MidiEvent> events,
                                           const std::string& label) {
    return apply(label, [&](StyleDefinition& s, EditResult& r) -> std::string {
        TrackDefinition* t = nullptr;
        if (auto e = trackAt(s, section, track, t); !e.empty()) return e;
        const uint64_t len = sectionLengthTicks(s.sections[section]);
        for (auto& e : events) e.channel = t->midi_channel;
        // A note left sounding would hang on the instrument: close it at the end.
        const auto p = partners(events);
        const size_t n = events.size();
        for (size_t i = 0; i < n; ++i)
            if (isOn(events[i]) && p[i] < 0) events.push_back({MidiEventType::NoteOff, t->midi_channel, events[i].data1, 0, len});
        t->events = std::move(events);
        r.changed = t->events.size();
        return finishTrack(t->events, len);
    });
}

// ── Elements and track roles ──────────────────────────────────────────

EditResult StyleEditor::copySection(size_t from, uasf::SectionType to) {
    return apply(std::string("Copy to ") + sectionTypeName(to), [&](StyleDefinition& s, EditResult& r) -> std::string {
        if (from >= s.sections.size()) return "no section " + std::to_string(from);
        if (s.sections[from].type == to) return "source and destination are the same element";
        SectionDefinition copy = s.sections[from];
        copy.type = to;
        auto it = std::find_if(s.sections.begin(), s.sections.end(), [&](const auto& x) { return x.type == to; });
        if (it != s.sections.end()) {
            copy.name = it->name;
            copy.keep_name = it->keep_name;   // the target's name and whether it is stored stay together
            *it = std::move(copy);
        } else {
            copy.name = sectionTypeName(to);
            copy.keep_name = true;
            s.sections.push_back(std::move(copy));
        }
        // Pa Copy copies the element's sounds too: the target's own ones are replaced by the source's.
        auto& es = s.korg_export.element_sounds;
        const auto fromType = s.sections[from].type;
        for (auto it = es.begin(); it != es.end();) it = it->first.first == to ? es.erase(it) : std::next(it);
        std::vector<std::pair<uint8_t, uasf::KorgSound>> copied;
        for (const auto& [key, snd] : es) if (key.first == fromType) copied.emplace_back(key.second, snd);
        for (const auto& [ch, snd] : copied) es[{to, ch}] = snd;
        for (const auto& t : s.sections[from].tracks) r.changed += t.events.size();
        return {};
    });
}

EditResult StyleEditor::clearSection(size_t section) {
    return apply("Clear element", [&](StyleDefinition& s, EditResult& r) -> std::string {
        if (section >= s.sections.size()) return "no section " + std::to_string(section);
        for (auto& t : s.sections[section].tracks) { r.changed += t.events.size(); t.events.clear(); }
        return {};
    });
}

EditResult StyleEditor::renameSection(size_t section, const std::string& name) {
    if (name.empty() || name.size() > 64) return {false, "name must be 1..64 characters"};
    return apply("Rename element", [&](StyleDefinition& s, EditResult& r) -> std::string {
        if (section >= s.sections.size()) return "no section " + std::to_string(section);
        s.sections[section].name = name;
        s.sections[section].keep_name = true;
        r.changed = 1;
        return {};
    });
}

EditResult StyleEditor::setSectionBars(size_t section, uint32_t bars) {
    if (bars < 1 || bars > 256) return {false, "length must be 1..256 bars"};
    return apply("Element length", [&](StyleDefinition& s, EditResult& r) -> std::string {
        if (section >= s.sections.size()) return "no section " + std::to_string(section);
        s.sections[section].bars = bars;
        r.changed = 1;
        return pastEnd(s.sections[section]);
    });
}

EditResult StyleEditor::setChannelRole(uint8_t channel, uasf::TrackRole role) {
    return apply("Track role", [&](StyleDefinition& s, EditResult& r) -> std::string {
        for (auto& sec : s.sections)
            for (auto& t : sec.tracks)
                if (t.midi_channel == channel) {
                    t.role = role;
                    t.is_drum = role == uasf::TrackRole::Drum || role == uasf::TrackRole::Percussion;
                    ++r.changed;
                }
        return r.changed ? "" : "no track on channel " + std::to_string(channel + 1);
    });
}

EditResult StyleEditor::setPart(uint8_t src, uint8_t part) {
    if (src > 15 || part > 15) return {false, "channel must be 1-16"};
    return apply("Pa5X part", [&](StyleDefinition& s, EditResult& r) -> std::string {
        s.korg_export.route[src] = part;
        r.changed = 1;
        return {};
    });
}

EditResult StyleEditor::setSound(uint8_t kch, std::optional<uasf::KorgSound> snd, std::optional<uasf::SectionType> element) {
    if (kch < 8 || kch > 15) return {false, "not a Korg style track (channels 9-16)"};
    if (snd && (snd->msb > 127 || snd->lsb > 127 || snd->pc > 127)) return {false, "MSB, LSB and PC must be 0-127"};
    return apply(element ? "Pa5X element sound" : "Pa5X sound", [&](StyleDefinition& s, EditResult& r) -> std::string {
        auto& k = s.korg_export;
        if (element && std::none_of(s.sections.begin(), s.sections.end(), [&](const auto& x) { return x.type == *element; }))
            return "no element " + std::string(sectionTypeName(*element));
        if (element) { if (snd) k.element_sounds[{*element, kch}] = *snd; else k.element_sounds.erase({*element, kch}); }
        else {   // style-wide = in every element: it replaces this track's element sounds (Codex r4 #3)
            if (snd) k.sounds[kch] = *snd;
            else k.sounds.erase(kch);
            for (auto it = k.element_sounds.begin(); it != k.element_sounds.end();) it = it->first.second == kch ? k.element_sounds.erase(it) : std::next(it);
        }
        r.changed = 1;
        return {};
    });
}

// ── Korg Chord Variations ─────────────────────────────────────────────

KorgStyleEditor::KorgStyleEditor(exporters::korg::Style style, size_t undoLimit)
    : style_(std::move(style)), history_(undoLimit) {}

EditResult KorgStyleEditor::copyChordVariation(const std::string& from, const std::string& to) {
    EditResult r;
    auto& els = style_.elements;
    auto find = [&](const std::string& m) {
        return std::find_if(els.begin(), els.end(), [&](const auto& e) { return e.marker == m; });
    };
    const auto src = find(from);
    if (src == els.end()) { r.error = "no element '" + from + "'"; return r; }
    if (from == to) { r.error = "source and destination are the same"; return r; }
    exporters::korg::Style next = style_;
    exporters::korg::Element copy = *src;
    copy.marker = to;
    const auto idx = static_cast<size_t>(find(to) - els.begin());
    if (idx < next.elements.size()) next.elements[idx] = std::move(copy);
    else next.elements.push_back(std::move(copy));
    if (const auto errs = exporters::korg::validate(next); !errs.empty()) { r.error = "Korg validator: " + errs.front(); return r; }
    r.changed = src->events.size();
    history_.record("Copy " + from + " to " + to, std::move(style_));
    style_ = std::move(next);
    r.ok = true;
    return r;
}

// ── SMF ───────────────────────────────────────────────────────────────

SmfFile readSmf(const std::vector<uint8_t>& b) {
    SmfFile f;
    auto fail = [&](std::string m) { f.ok = false; f.error = std::move(m); f.tracks.clear(); return f; };
    auto rd32 = [&](size_t p) {
        return uint32_t{b[p]} << 24 | uint32_t{b[p + 1]} << 16 | uint32_t{b[p + 2]} << 8 | b[p + 3];
    };
    if (b.size() < 14 || std::string(b.begin(), b.begin() + 4) != "MThd") return fail("not an SMF (no MThd)");
    const uint32_t hlen = rd32(4);
    if (hlen < 6 || hlen > b.size() - 8) return fail("bad MThd length");
    const uint16_t format = static_cast<uint16_t>(b[8] << 8 | b[9]);
    const uint16_t division = static_cast<uint16_t>(b[12] << 8 | b[13]);
    if (format > 1) return fail("SMF format " + std::to_string(format) + " not supported (0 or 1)");
    if (division == 0 || (division & 0x8000)) return fail("SMPTE or zero time division not supported");
    f.ppqn = division;

    size_t p = 8 + hlen;
    while (p < b.size()) {
        if (b.size() - p < 8) return fail("truncated chunk header");
        const bool isTrk = std::string(b.begin() + p, b.begin() + p + 4) == "MTrk";
        const uint32_t len = rd32(p + 4);
        p += 8;
        if (len > b.size() - p) return fail("chunk length runs past end of file");
        const size_t end = p + len;
        if (!isTrk) { p = end; continue; }
        auto& out = f.tracks.emplace_back();
        uint64_t abs = 0;
        uint8_t running = 0;
        auto vlq = [&](uint32_t& v) {
            v = 0;
            for (int i = 0; i < 4 && p < end; ++i) {
                const uint8_t c = b[p++];
                v = v << 7 | (c & 0x7F);
                if (!(c & 0x80)) return true;
            }
            return false;
        };
        while (p < end) {
            uint32_t d;
            if (!vlq(d)) return fail("bad delta time");
            abs += d;
            if (p >= end) return fail("truncated event");
            uint8_t st = b[p];
            if (st & 0x80) ++p;
            else if (running) st = running;
            else return fail("running status with no previous status");
            if (st == 0xFF) {
                running = 0;
                if (p >= end) return fail("truncated meta event");
                const uint8_t type = b[p++];
                uint32_t ml;
                if (!vlq(ml) || ml > end - p) return fail("bad meta event length");
                p += ml;
                if (type == 0x2F) break;
            } else if (st == 0xF0 || st == 0xF7) {
                running = 0;
                uint32_t sl;
                if (!vlq(sl) || sl > end - p) return fail("bad SysEx length");
                p += sl;
            } else if (st > 0xF0) {
                return fail("system message in SMF track");
            } else {
                running = st;
                const size_t n = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
                if (end - p < n) return fail("truncated channel event");
                SmfEvent e{abs, st, b[p], n == 2 ? b[p + 1] : uint8_t{0}};
                if (e.d1 > 0x7F || e.d2 > 0x7F) return fail("data byte above 127");
                p += n;
                out.push_back(e);
            }
        }
        p = end;
    }
    if (f.tracks.empty()) return fail("no MTrk chunk");
    f.ok = true;
    return f;
}

std::vector<MidiEvent> smfTrackToUasf(const SmfFile& smf, size_t smfTrack, std::optional<uint8_t> channel,
                                      uint32_t resolution, uint64_t lengthTicks, size_t* dropped) {
    std::vector<MidiEvent> out;
    size_t drop = 0;
    if (smfTrack < smf.tracks.size() && smf.ppqn) {
        for (const auto& e : smf.tracks[smfTrack]) {
            const uint8_t ch = e.status & 0x0F;
            if (channel && ch != *channel) continue;
            const uint64_t t = (e.tick * resolution + smf.ppqn / 2) / smf.ppqn;
            const MidiEvent m{static_cast<MidiEventType>(e.status & 0xF0), ch, e.d1, e.d2, t};
            // NoteOffs past the end are kept: the editor ends those notes at the element end.
            if (t >= lengthTicks && !isOff(m)) { ++drop; continue; }
            out.push_back(m);
        }
    }
    // NoteOffs whose NoteOn was dropped above would be orphans: drop them too.
    const auto p = partners(out);
    size_t w = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (isOff(out[i]) && p[i] < 0 && out[i].tick >= lengthTicks) { ++drop; continue; }
        out[w++] = out[i];
    }
    out.resize(w);
    if (dropped) *dropped = drop;
    return out;
}

SmfExport exportPa5xSmf(const StyleDefinition& style, uint16_t ppqn) {
    namespace k = exporters::korg;
    SmfExport x;
    k::MapOptions opt;
    opt.ppqn = ppqn;
    opt.route = style.korg_export.route;
    for (const auto& [ch, snd] : style.korg_export.sounds) opt.sounds[ch] = {snd.msb, snd.lsb, snd.pc};
    for (const auto& [key, snd] : style.korg_export.element_sounds) opt.element_sounds[key] = {snd.msb, snd.lsb, snd.pc};
    const auto mapped = k::mapToKorg(style, opt);
    if (!mapped.ok) { x.error = "conversion failed: " + mapped.error; return x; }
    x.report = k::formatReport(mapped.report, mapped.style);
    const auto w = k::write(mapped.style);
    if (!w.ok) { x.error = "write failed: " + w.error; return x; }
    const auto back = k::parse(w.bytes);
    if (!back.ok) { x.error = "re-read failed: " + back.error; return x; }
    if (const auto errs = k::validate(back.style); !errs.empty()) { x.error = "validator: " + errs.front(); return x; }
    x.korg = mapped.style;
    x.bytes = w.bytes;
    x.ok = true;
    return x;
}

KorgSmfImport importKorgSmf(const std::vector<uint8_t>& bytes, const std::string& name, SoundLoss policy) {
    namespace k = exporters::korg;
    KorgSmfImport out;
    const auto p = k::parse(bytes);
    if (!p.ok) { out.error = "not a Korg Style SMF: " + p.error; return out; }
    if (const auto errs = k::validate(p.style); !errs.empty()) { out.error = "Korg validator: " + errs.front(); return out; }
    out.notes = p.notes;
    const auto& ks = p.style;
    // UASF resolution is 1..1920: a finer file is rescaled (exact rational, per absolute tick).
    const uint32_t res = std::min<uint32_t>(ks.ppqn, 1920);
    auto conv = [&](uint64_t t) { return (t * res + ks.ppqn / 2) / ks.ppqn; };
    if (res != ks.ppqn) out.notes.push_back("PPQN " + std::to_string(ks.ppqn) + " rescaled to 1920");
    StyleDefinition& s = out.style;
    s.name = name;
    s.format_version = "1.0";
    s.resolution = res;
    s.tempo_bpm = ks.mpqn ? (60'000'000u + ks.mpqn / 2) / ks.mpqn : 120;
    if (!ks.mpqn) out.notes.push_back("no tempo in the file: 120 BPM set");
    // Korg track channel -> the source channel the exporter maps onto it (korgChannelFor inverse).
    auto sourceCh = [](uint8_t kch) -> int {
        for (int g = 0; g < 16; ++g)
            if (k::korgChannelFor(static_cast<uint8_t>(g)) == kch) return g;
        return -1;
    };
    auto& kx = s.korg_export;
    for (const auto& el : ks.elements) {
        std::optional<uasf::SectionType> type;
        for (int i = 0; i <= static_cast<int>(uasf::SectionType::Break); ++i)
            if (const char* m = k::korgMarkerFor(static_cast<uasf::SectionType>(i)); m && el.marker == m)
                type = static_cast<uasf::SectionType>(i);
        if (!type || std::any_of(s.sections.begin(), s.sections.end(), [&](const auto& x) { return x.type == *type; })) {
            out.notes.push_back("element " + el.marker + " not imported (UASF keeps one chord variation per element)");
            continue;
        }
        SectionDefinition sec;
        sec.type = *type;
        sec.name = sectionTypeName(*type);
        sec.resolution = res;
        sec.beats_per_bar = el.num;
        sec.beat_note = el.den;
        const uint64_t bar = uint64_t{ks.ppqn} * 4 * el.num / el.den;
        sec.bars = bar ? static_cast<uint32_t>(el.length / bar) : 0;
        std::map<uint8_t, TrackDefinition> tracks;
        size_t chordNotes = 0, dropped = 0;
        // MIDI running bank state per Korg channel in this element: the sound a device selects is the CC0/CC32 value
        // current when a Program Change arrives. The first such complete selection is the element's sound.
        struct Bank { int msb = -1, lsb = -1; std::optional<KorgSound> got; size_t events = 0; };
        std::map<uint8_t, Bank> bank;
        for (const auto& e : el.events) {
            const uint8_t kch = e.status & 0x0F, hi = e.status & 0xF0;
            const int g = sourceCh(kch);
            if (g < 0) { ++dropped; continue; }
            if (hi == 0xC0 || (hi == 0xB0 && (e.d1 == 0 || e.d1 == 32))) {
                auto& b = bank[kch];
                ++b.events;
                if (hi == 0xB0) (e.d1 == 0 ? b.msb : b.lsb) = e.d2;
                else if (!b.got && b.msb >= 0 && b.lsb >= 0) b.got = KorgSound{uint8_t(b.msb), uint8_t(b.lsb), e.d1};
                continue;
            }
            auto [it, fresh] = tracks.try_emplace(kch);
            TrackDefinition& t = it->second;
            if (fresh) {
                t.name = k::korgTrackName(kch);
                t.midi_channel = static_cast<uint8_t>(g);
                t.is_drum = kch == k::kDrumCh || kch == k::kPercCh;
                t.role = kch == k::kDrumCh ? uasf::TrackRole::Drum : kch == k::kPercCh ? uasf::TrackRole::Percussion
                         : kch == k::kBassCh ? uasf::TrackRole::Bass : uasf::TrackRole::Accompaniment;
            }
            if (hi == 0x90 && e.d2 && k::isChordDriven(kch) && e.d1 <= k::kChordEventNoteMax) ++chordNotes;
            t.events.push_back({static_cast<MidiEventType>(hi), static_cast<uint8_t>(g), e.d1, e.d2, conv(e.tick)});
        }
        for (auto& [kch, t] : tracks) sec.tracks.push_back(std::move(t));
        if (chordNotes)
            out.notes.push_back(el.marker + ": " + std::to_string(chordNotes) +
                                " notes C-1..B-1 on chord-driven tracks (Pa5X Chord events) kept as notes; the exporter drops them");
        if (dropped) out.notes.push_back(el.marker + ": " + std::to_string(dropped) + " events outside the 8 Korg style tracks not imported");
        // Each element keeps its own bank/program (the exporter writes it at that element's start).
        // Exact = one CC0, one CC32 and a Program Change after both; anything else is a loss.
        for (const auto& [kch, b] : bank) {
            const std::string trk = std::string("Korg ") + k::korgTrackName(kch) + ", " + el.marker + ": ";
            if (!b.got) { out.losses.push_back(trk + "no Program Change after both CC0 and CC32 (incomplete selection), no sound kept"); continue; }
            kx.element_sounds[{*type, kch}] = *b.got;
            if (b.events != 3)
                out.losses.push_back(trk + std::to_string(b.events) + " bank/program events, kept the first complete selection " +
                                     std::to_string(b.got->msb) + "/" + std::to_string(b.got->lsb) + "/" + std::to_string(b.got->pc) +
                                     " (bank state at its Program Change); the rest not kept");
        }
        s.sections.push_back(std::move(sec));
    }
    if (s.sections.empty()) { out.error = "no element maps to a UASF section"; return out; }
    if (!out.losses.empty() && policy == SoundLoss::Refuse) {
        out.error = "bank/program cannot be kept exactly (" + out.losses.front() + (out.losses.size() > 1 ? ", +" + std::to_string(out.losses.size() - 1) + " more" : "") +
                    "): open it with \"keep the first sound\" to accept the loss";
        return out;
    }
    for (const auto& l : out.losses) out.notes.push_back("LOSS: " + l);
    // A track with one sound in every element: style-wide (same export, and a style-wide edit then applies to it).
    for (uint8_t kch = 8; kch <= 15; ++kch) {
        std::optional<KorgSound> one;
        size_t n = 0;
        bool same = true;
        for (const auto& [key, snd] : kx.element_sounds)
            if (key.second == kch) { ++n; same = same && (!one || *one == snd); one = snd; }
        if (!one || !same || n != s.sections.size()) continue;
        kx.sounds[kch] = *one;
        for (const auto& sec : s.sections) kx.element_sounds.erase({sec.type, kch});
    }
    uasf::UasfValidator v;
    for (const auto& i : v.validate(s).issues)
        if (i.severity == uasf::ValidationIssue::ERROR) { out.error = "UASF validator: " + i.message; return out; }
    if (auto e = saveError(s); !e.empty()) { out.error = std::move(e); return out; }
    out.ok = true;
    return out;
}

} // namespace ai_arranger::style_edit
