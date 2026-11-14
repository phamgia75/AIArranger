// W3: builders for Pa5X Root-tree objects (Pad, Keyboard Set, Style). EXPERIMENTAL, NOT HARDWARE-VERIFIED.
//
// Every node, element and attribute written here was OBSERVED in owner Pa5X KST working copies, in the versions the
// Pa5X itself wrote in 2024 (PadClip v0 r4, KeyboardSetClip v0 r4 session form, STAGE v1 r2 "SongStyle1" form).
// Write rule (structure survey 2026-11-14, counts in docs/research/pa-studio/PA5X_NATIVE_KST_W3_W5.md):
//   - a node/element is written only where it was present in every observed file of that version and role;
//   - an element is written only when every observed attribute of it held one constant value in every file, and then
//     exactly with those constants (format constants such as EQSetting trim="99");
//   - values that vary between owner files (tempo, chord tables, loop lengths, track routes, ...) are NOT invented: the
//     element is left out and listed as UNKNOWN in kUnknown below;
//   - link values are node names of the tree being built; role/scene/track numbers are the per-name constants observed
//     in every file; a TimeSignature element present in every file of its role is written as 4/4 in the observed
//     encoding (numerator 4, denominator 2 = log2, as in SMF meta 0x58; 4/2 is the most frequent observed pair).
// Korg SysEx parameter blocks (F0 42 ...), present in every observed Pad/KBD Set/Style MIDI leaf, are NOT generated
// (their layout is UNKNOWN): leaves carry only standard SMF events. Whether the Pa5X loads such objects is the first
// hardware question (W6).
#ifndef AI_ARRANGER_MEDIA_PANATIVE_KST_BUILDERS_H
#define AI_ARRANGER_MEDIA_PANATIVE_KST_BUILDERS_H

#include "media/panative/root_tree.h"
#include <algorithm>
#include <array>
#include <tuple>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ai_arranger::media::panative::build {

using roottree::Node;

struct SoundSelect { uint8_t msb = 121, lsb = 0, program = 0; };   // CC0, CC32, Program Change (channel 1, as observed)
struct Note { uint32_t tick = 0, length = 96; uint8_t key = 60, velocity = 100; };

constexpr uint16_t kPpq = 384;   // OBSERVED division 0x0180 in every Pa5X MIDI leaf of the surveyed versions

// Elements present in every observed file that these builders leave out because their values vary (not invented).
inline const std::vector<std::string>& unknownOmitted() {
    static const std::vector<std::string> u = {
        "every MIDI leaf: Korg SysEx parameter blocks (F0 42 7F/3F ...), layout UNKNOWN",
        "every MIDI leaf: the controller events after the Program Change (numbers/values vary)",
        "PadClip / track PadClip PARAMS: Sync, Loop, Pattern, CHORD_TABLE (values vary)",
        "KeyboardSetClip PARAMS: ControlFunctions, KeyboardProcessing, MidiEffect, Track1-4Params (values vary)",
        "PlainClip PARAMS: Loop (length varies), MidiEffect (Upper parts vary)",
        "StyleClip PARAMS: Sync, Initial, Tempo, Bass/Drum/Perc/Acc1-5Params, ChordFollow (values vary), LimitKeyRange (acc5 in 176/177)",
        "KbdSet1-4 SceneDescription PARAMS: TimeSignature (174-176/177 only)",
        "StageDescription PARAMS: Tempo, Signature, Track9-16Params (values vary)",
        "Style: chord-recognition behaviour, tempo and loop lengths therefore come only from the SMF data",
    };
    return u;
}

namespace detail {
inline void be32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s += char(v >> (8 * i)); }
inline void vlq(std::string& s, uint32_t v) {
    char b[5]; int n = 0;
    b[n++] = char(v & 0x7F);
    while (v >>= 7) b[n++] = char(0x80 | (v & 0x7F));
    while (n) s += b[--n];
}
struct Ev { uint32_t tick; int order; std::string bytes; };
inline std::string track(std::vector<Ev> evs, uint32_t endTick) {
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });
    std::string t;
    uint32_t now = 0;
    for (const auto& e : evs) { vlq(t, e.tick - now); now = e.tick; t += e.bytes; }
    vlq(t, endTick > now ? endTick - now : 0);
    t += std::string("\xFF\x2F\x00", 3);
    std::string out = "MTrk";
    be32(out, uint32_t(t.size()));
    return out + t;
}
inline std::string smf(uint16_t format, const std::vector<std::string>& tracks) {
    std::string s = "MThd";
    be32(s, 6);
    s += char(format >> 8); s += char(format); s += char(tracks.size() >> 8); s += char(tracks.size());
    s += char(kPpq >> 8); s += char(kPpq & 0xFF);
    for (const auto& t : tracks) s += t;
    return s;
}
// Header events in the observed order: time signature (0x58), [tempo (0x51)], key signature (0x59).
inline std::vector<Ev> head(bool tempo, uint32_t bpm) {
    std::vector<Ev> e = {{0, 0, std::string("\xFF\x58\x04\x04\x02\x18\x08", 7)}};   // 4/4
    if (tempo) {
        const uint32_t us = 60000000u / (bpm ? bpm : 120);
        e.push_back({0, 1, std::string("\xFF\x51\x03", 3) + char(us >> 16) + char(us >> 8) + char(us)});
    }
    e.push_back({0, 2, std::string("\xFF\x59\x02\x00\x00", 5)});                    // C major
    return e;
}
inline void select(std::vector<Ev>& e, const SoundSelect& s) {
    e.push_back({0, 3, {char(0xB0), 0, char(s.msb & 0x7F)}});
    e.push_back({0, 4, {char(0xB0), 32, char(s.lsb & 0x7F)}});
    e.push_back({0, 5, {char(0xC0), char(s.program & 0x7F)}});
}
inline uint32_t endOf(const std::vector<Note>& notes, uint32_t bars) {
    uint32_t end = bars * 4u * kPpq;
    for (const auto& n : notes) end = std::max(end, n.tick + n.length);
    return end;
}
inline std::string notesTrack(const std::vector<Note>& notes, uint32_t end) {
    std::vector<Ev> e;
    for (const auto& n : notes) {
        e.push_back({n.tick, 1, {char(0x90), char(n.key & 0x7F), char(n.velocity ? n.velocity & 0x7F : 1)}});
        e.push_back({n.tick + n.length, 0, {char(0x80), char(n.key & 0x7F), 64}});
    }
    return track(e, end);
}

// XML: declaration as observed (7 809/7 809 and 71 273/71 273 leaves), elements written by the builders below.
struct X {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<X> kids;
};
inline std::string esc(const std::string& v) {
    std::string o;
    for (char c : v) o += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : std::string(1, c);
    return o;
}
inline void emit(const X& x, std::string& o, int ind) {
    o += std::string(size_t(ind) * 2, ' ') + "<" + x.tag;
    for (const auto& [k, v] : x.attrs) o += " " + k + "=\"" + esc(v) + "\"";
    if (x.kids.empty()) { o += " />\n"; return; }
    o += ">\n";
    for (const auto& k : x.kids) emit(k, o, ind + 1);
    o += std::string(size_t(ind) * 2, ' ') + "</" + x.tag + ">\n";
}
inline std::string xml(const X& root) {
    std::string o = "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n";
    emit(root, o, 0);
    return o;
}
inline Node leaf(const std::string& name, std::string bytes) { Node n; n.kind = Node::Leaf; n.name = name; n.payload = std::move(bytes); return n; }
inline Node branch(const std::string& name, std::vector<Node> kids) { Node n; n.kind = Node::Branch; n.name = name; n.children = std::move(kids); return n; }
inline X clip(const char* type, const char* release, std::vector<std::pair<std::string, std::string>> res, std::vector<X> params) {
    return {"CLIP", {{"type", type}, {"version", "0"}, {"release", release}}, {{"RESOURCE", std::move(res), {}}, {"PARAMS", {}, std::move(params)}}};
}
// Constant-valued PARAMS children, each observed n/n with exactly these values in its role.
inline X eq() { return {"EQSetting", {{"bypass", "0"}, {"dryOff", "0"}, {"highGain", "0"}, {"lowGain", "0"}, {"midFreq", "0"}, {"midGain", "0"}, {"trim", "99"}}, {}}; }
inline X ts1() { return {"TimeStretching", {{"denominator", "1"}, {"numerator", "1"}}, {}}; }
inline X tuning0() { return {"OctaveTuning", {{"tracks", "0"}}, {}}; }
inline X timeSig44() { return {"TimeSignature", {{"beatsubdivisions", "1"}, {"denominator", "2"}, {"numerator", "4"}}, {}}; }
}  // namespace detail

// A PadClip as a .pdc tree (also the track clip of a Style element). Main.mid = SMF format 1 (observed 438/438 for
// PadClip v0 r4; format 0 never), track 1: header events + sound select, track 2: the notes; all on channel 1.
inline Node padClipTree(const SoundSelect& sound, const std::vector<Note>& notes, uint32_t bars = 1, uint32_t bpm = 120) {
    using namespace detail;
    const uint32_t end = endOf(notes, bars);
    auto h = head(true, bpm);
    select(h, sound);
    Node r;
    r.kind = Node::Root; r.name = "\x01";
    // PadClip PARAMS children constant in 438/438: TimeStretching, Start, MidiEffect, EQSetting, OctaveTuning (observed order).
    const X c = clip("PadClip", "4", {{"name", "Main.mid"}},
                     {ts1(), {"Start", {{"fineSlide", "0"}}, {}},
                      {"MidiEffect", {{"attenuation", "5"}, {"deltatime", "5"}, {"interval", "0"}, {"repetitions", "1"}, {"type", "0"}, {"wetonly", "no"}}, {}},
                      eq(), tuning0()});
    r.children = {leaf("Main.mid", smf(1, {track(h, end), notesTrack(notes, end)})), leaf("ClipDescription.xml", xml(c))};
    return r;
}

// A KeyboardSetClip v0 r4 (session form) as a .ksc tree: four PlainClip parts Lower/Upper1-3 + MFX.mid.
inline Node keyboardSetTree(const std::array<SoundSelect, 4>& parts) {
    using namespace detail;
    static const char* names[4] = {"Lower", "Upper1", "Upper2", "Upper3"};
    std::vector<Node> scn;
    X scene{"SCENE", {}, {}};
    for (int i = 0; i < 4; ++i) {
        auto e = head(false, 0);   // the form without tempo (865/1053)
        select(e, parts[size_t(i)]);
        // PlainClip PARAMS constant in 1053/1053 for every part: Sync, Start, EQSetting, Pitch, ReTrigger.
        const X c = clip("PlainClip", "4", {{"name", "Main.mid"}},
                         {{"Sync", {{"start", "4"}, {"startrec", "4"}, {"sync", "2"}}, {}}, {"Start", {{"fineSlide", "0"}, {"measure", "0"}}, {}}, eq(), {"Pitch", {{"transposition", "0"}}, {}}, {"ReTrigger", {{"mode", "3"}}, {}}});
        scn.push_back(branch(std::string(names[i]) + ".plc", {leaf("Main.mid", smf(0, {track(e, 0)})), leaf("ClipDescription.xml", xml(c))}));
        scene.kids.push_back({"RESOURCE", {{"name", std::string(names[i]) + ".plc"}, {"track", std::to_string(i + 1)}, {"type", "PlainClip"}}, {}});
    }
    scene.kids.push_back({"PARAMS", {}, {timeSig44()}});
    scn.push_back(leaf("SceneDescription.xml", xml(scene)));
    const X session{"SESSION", {{"maxscenes", "1"}, {"maxtracks", "4"}}, {{"RESOURCE", {{"name", "RTTracks.scn"}, {"scenenum", "1"}}, {}}, {"PARAMS", {}, {}}}};
    const X c = clip("KeyboardSetClip", "4", {{"name", "Main.ssn"}, {"complement", "MFX.mid"}},
                     {{"Sync", {{"start", "4"}, {"startrec", "4"}}, {}}});
    Node r;
    r.kind = Node::Root; r.name = "\x01";
    r.children = {branch("Main.ssn", {branch("RTTracks.scn", std::move(scn)), leaf("SessionDescription.xml", xml(session))}),
                  leaf("MFX.mid", smf(0, {track(head(false, 0), 0)})), leaf("ClipDescription.xml", xml(c))};
    return r;
}

struct StylePad { std::string name; SoundSelect sound; std::vector<Note> notes; };   // name without ".pdc"
struct StyleSpec {
    std::string name;                                // the Style name (user text, becomes <name>.stc)
    SoundSelect drum{120, 0, 0};                     // Drum track (track 2) sound
    std::vector<Note> drumPattern;                   // one-bar pattern, used in every element
    std::array<std::string, 4> kbdSetNames;          // embedded KBD Sets (scenes KbdSet1-4)
    std::array<std::array<SoundSelect, 4>, 4> kbdSets;
    std::array<StylePad, 4> pads;                    // PadSet1 (tracks 17-20)
    uint32_t bpm = 120;
};

// Observed per-element constants: scene name -> (SessionDescription role, scenenum), 177/177 SongStyle1-form Styles.
inline const std::vector<std::tuple<const char*, int, int>>& styleElements() {
    static const std::vector<std::tuple<const char*, int, int>> e = {
        {"Intro1", 4, 1}, {"Intro2", 5, 2}, {"CountIn", 11, 3}, {"Var1", 0, 4}, {"Var2", 1, 5}, {"Var3", 2, 6}, {"Var4", 3, 7},
        {"Fill1", 6, 8}, {"Fill2", 7, 9}, {"Fill3", 13, 10}, {"Fill4", 14, 11}, {"Break", 10, 12}, {"End1", 8, 13}, {"End2", 9, 14}, {"End3", 12, 15}};
    return e;
}

// A Style (STAGE v1 r2, SongStyle1 form, as in 177/443 observed v1 r2 Styles) as a .stg tree:
// Main.ssn -> SongStyle1.scn (the style core) + PadSet1.scn (4 Pads) + KbdSet1-4.scn (one embedded KBD Set each).
// Only the Drum track is written, in all 15 elements.
inline Node styleTree(const StyleSpec& s) {
    using namespace detail;
    std::vector<Node> els;
    X sess{"SESSION", {{"maxscenes", "15"}, {"maxtracks", "10"}}, {}};
    for (const auto& [el, role, num] : styleElements()) {
        X scene{"SCENE", {}, {{"RESOURCE", {{"name", "Drum.pdc"}, {"track", "2"}, {"type", "PadClip"}}, {}}, {"PARAMS", {}, {timeSig44(), tuning0()}}}};
        els.push_back(branch(std::string(el) + ".scn", {branch("Drum.pdc", padClipTree(s.drum, s.drumPattern, 1, s.bpm).children),
                                                         leaf("SceneDescription.xml", xml(scene))}));
        sess.kids.push_back({"RESOURCE", {{"name", std::string(el) + ".scn"}, {"role", std::to_string(role)}, {"scenenum", std::to_string(num)}}, {}});
    }
    X roles{"PARAMS", {}, {}};
    for (int t = 1; t <= 10; ++t) roles.kids.push_back({"TrackRole", {{"role", std::to_string(t - 1)}, {"track", std::to_string(t)}}, {}});
    sess.kids.push_back(roles);
    els.push_back(leaf("SessionDescription.xml", xml(sess)));
    const X stc = clip("StyleClip", "7", {{"name", "Main.ssn"}, {"complement", "MFX.mid"}}, {});   // no PARAMS child constant in 177/177
    const std::string stcName = s.name + ".stc";
    X core{"SCENE", {}, {{"RESOURCE", {{"name", stcName}, {"numoftracks", "10"}, {"t1", "9"}, {"t2", "10"}, {"t3", "11"}, {"t4", "12"}, {"t5", "13"},
                                       {"t6", "14"}, {"t7", "15"}, {"t8", "16"}, {"t9", "27"}, {"t10", "25"}, {"type", "StyleClip"}}, {}},
                        {"PARAMS", {}, {timeSig44(), tuning0()}}}};
    std::vector<Node> scenes;
    scenes.push_back(branch("SongStyle1.scn", {branch(stcName, {branch("Main.ssn", std::move(els)), leaf("MFX.mid", smf(0, {track(head(false, 0), 0)})),
                                                                leaf("ClipDescription.xml", xml(stc))}),
                                                leaf("SceneDescription.xml", xml(core))}));
    X padScene{"SCENE", {}, {}};
    std::vector<Node> pads;
    for (int i = 0; i < 4; ++i) {
        const auto& p = s.pads[size_t(i)];
        pads.push_back(branch(p.name + ".pdc", padClipTree(p.sound, p.notes, 1, s.bpm).children));
        padScene.kids.push_back({"RESOURCE", {{"name", p.name + ".pdc"}, {"track", std::to_string(17 + i)}, {"type", "PadClip"}}, {}});
    }
    padScene.kids.push_back({"PARAMS", {}, {timeSig44(), tuning0()}});
    pads.push_back(leaf("SceneDescription.xml", xml(padScene)));
    scenes.push_back(branch("PadSet1.scn", std::move(pads)));
    X main{"SESSION", {{"maxscenes", "71"}, {"maxtracks", "28"}}, {{"RESOURCE", {{"name", "SongStyle1.scn"}, {"scenenum", "1"}}, {}}}};
    main.kids.push_back({"RESOURCE", {{"name", "PadSet1.scn"}, {"scenenum", "15"}}, {}});
    for (int i = 0; i < 4; ++i) {
        const std::string scn = "KbdSet" + std::to_string(i + 1) + ".scn", ksc = s.kbdSetNames[size_t(i)] + ".ksc";
        const X sc{"SCENE", {}, {{"RESOURCE", {{"name", ksc}, {"track", "21"}, {"type", "KeyboardSetClip"}}, {}}, {"PARAMS", {}, {tuning0()}}}};   // TimeSignature in 174/177 only
        scenes.push_back(branch(scn, {branch(ksc, keyboardSetTree(s.kbdSets[size_t(i)]).children), leaf("SceneDescription.xml", xml(sc))}));
        main.kids.push_back({"RESOURCE", {{"name", scn}, {"scenenum", std::to_string(25 + i)}}, {}});
    }
    main.kids.push_back({"PARAMS", {}, {{"NextClips", {}, {}}, {"NextScenes", {}, {}}}});
    scenes.push_back(leaf("SessionDescription.xml", xml(main)));
    X stparams{"PARAMS", {}, {{"Setupscene", {{"scene", "1"}}, {}}}};
    for (int t = 1; t <= 8; ++t) stparams.kids.push_back({"Track" + std::to_string(t) + "Params", {{"fxGroup", "0"}, {"mute", "0"}}, {}});
    for (int p = 1; p <= 4; ++p) stparams.kids.push_back({"Pad" + std::to_string(p) + "Params", {{"mute", "0"}, {"trackroute", "3"}}, {}});
    const X stage{"STAGE", {{"type", "1"}, {"version", "1"}, {"release", "2"}}, {{"RESOURCE", {{"mainsessionname", "Main.ssn"}}, {}}, stparams}};
    Node r;
    r.kind = Node::Root; r.name = "\x01";
    r.children = {branch("Main.ssn", std::move(scenes)), leaf("StageDescription.xml", xml(stage))};
    return r;
}

}  // namespace ai_arranger::media::panative::build

#endif
