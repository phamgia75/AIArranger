// Output routing: logical Style parts -> physical channels (midi/output_routing.h), the Pa5X
// preset profile, the facade path (Note On / Note Off / cleanup on one physical channel, no
// change while playing) and the optional session field. Offline: fake provider only.

#include "midi/output_routing.h"
#include "midi/pa5x_profile.h"
#include "session/live_engine_facade.h"
#include "session/session_persistence.h"
#include "engine/arranger/style_player.h"
#include "engine/chord/fingered_detector.h"
#include "fake_midi_input_source.h"
#include "fake_midi_output_provider.h"
#include <cstdio>
#include <map>
#include <set>

using namespace ai_arranger;
using namespace ai_arranger::midi;
using uasf::MidiEventType;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool sameMap(const control::OutputChannelMap& a, const control::OutputChannelMap& b) {
    for (int i = 0; i < 16; ++i) if (a.dst[i] != b.dst[i]) return false;
    return a.dropVoiceSelect == b.dropVoiceSelect;
}

// Per (physical channel, note): Note On count minus Note Off count, over the captured output.
static std::map<std::pair<int, int>, int> balance(const std::vector<uasf::MidiEvent>& sent) {
    std::map<std::pair<int, int>, int> b;
    for (const auto& e : sent) {
        if (e.type == MidiEventType::NoteOn && e.data2 > 0) ++b[{e.channel, e.data1}];
        else if (e.type == MidiEventType::NoteOff || e.type == MidiEventType::NoteOn) --b[{e.channel, e.data1}];
    }
    return b;
}
static bool balanced(const std::vector<uasf::MidiEvent>& sent) {
    for (const auto& [k, v] : balance(sent)) if (v != 0) return false;
    return true;
}

struct FacadeRig {
    FakeMidiInputSource fin; FakeMidiOutputProvider fout;
    chord::FingeredDetector det;
    session::LiveEngineFacade fac{&fin, &fout};
    FacadeRig() {
        fin.devices = {{0, "keyboard (fake)"}};
        fout.setDevices({{0, "Pa5X SOUND (fake provider)"}});
        fac.setChordScanMode(&det);
        fac.start();
        fac.selectMidiOutput(0);
        fac.tick(48);
    }
    void play(int ticks) { for (int i = 0; i < ticks; ++i) fac.tick(480); }
};

int main() {
    std::printf("Test: output routing (parts -> channels)\n");
    const RoutingProfile preset = pa5x::aiArrangerPresetRouting();
    const PartLayout demoLayout = partLayoutFromRoles(arranger::buildDemoStyle());

    // ── The new layer reproduces both existing, tested Pa5X maps exactly ──
    TEST("SFF layout + Pa5X preset == genosStyleTrackMap()",
         sameMap(buildChannelMap(sffPartLayout(), preset, true), pa5x::genosStyleTrackMap()));
    TEST("demo role layout + Pa5X preset == demoStyleTrackMap()",
         sameMap(buildChannelMap(demoLayout, preset, true), pa5x::demoStyleTrackMap()));
    TEST("demo layout: drums ch10 -> Drum, bass ch2 -> Bass, chord ch3 -> Acc1, nothing else",
         demoLayout.part_of_channel[9] == (uint8_t)StylePart::Drum && demoLayout.part_of_channel[1] == (uint8_t)StylePart::Bass &&
         demoLayout.part_of_channel[2] == (uint8_t)StylePart::Acc1 && [&] {
             int n = 0; for (auto p : demoLayout.part_of_channel) n += p != kNoPart; return n == 3; }());

    // ── Documented vs device-confirmed ──
    bool noneConfirmed = true, allPhoto = true;
    for (size_t i = 0; i < kStyleParts; ++i) {
        noneConfirmed &= preset.evidence[i] != RouteEvidence::DeviceConfirmed;
        allPhoto &= preset.evidence[i] == RouteEvidence::OwnerPhoto;
    }
    TEST("Pa5X preset: no part is marked device-confirmed (R2-R4 not run); all from the owner's photos",
         noneConfirmed && allPhoto);

    // ── Host choice: preset only for the exact "Pa5X SOUND" destination ──
    TEST("outputMapFor(\"Pa5X SOUND\") == preset map",
         sameMap(pa5x::outputMapFor("Pa5X SOUND", demoLayout), pa5x::demoStyleTrackMap()));
    TEST("outputMapFor: FLOW 8, IAC, a Pa5X candidate name, the KEYBOARD port and \"\" stay identity",
         sameMap(pa5x::outputMapFor("FLOW 8", demoLayout), control::OutputChannelMap{}) &&
         sameMap(pa5x::outputMapFor("IAC Bus 1", demoLayout), control::OutputChannelMap{}) &&
         sameMap(pa5x::outputMapFor("Pa5X MIDI OUT", demoLayout), control::OutputChannelMap{}) &&
         sameMap(pa5x::outputMapFor("Pa5X KEYBOARD", demoLayout), control::OutputChannelMap{}) &&
         sameMap(pa5x::outputMapFor("", demoLayout), control::OutputChannelMap{}));

    // ── Host state: the map follows the chosen destination, not a later enumeration ──
    {
        pa5x::HostOutputRouting h{"Pa5X SOUND", demoLayout};
        h.layout = sffPartLayout();   // a style loaded while the Pa5X is unplugged
        TEST("host: Pa5X chosen, SFF style loaded later -> Genos preset map (no re-enumeration)",
             sameMap(h.map(), pa5x::genosStyleTrackMap()));
        h.destination = "";
        TEST("host: no destination -> identity", sameMap(h.map(), control::OutputChannelMap{}));
    }

    // ── Role layout edge cases ──
    {
        uasf::StyleDefinition st;
        uasf::SectionDefinition sec;
        auto track = [](uint8_t ch, uasf::TrackRole r) { uasf::TrackDefinition t; t.midi_channel = ch; t.role = r; return t; };
        sec.tracks = {track(0, uasf::TrackRole::Bass), track(1, uasf::TrackRole::Bass), track(9, uasf::TrackRole::Drum)};
        for (uint8_t ch = 2; ch < 8; ++ch) sec.tracks.push_back(track(ch, uasf::TrackRole::Pad));   // 6 accompaniment tracks
        sec.tracks.push_back(track(20, uasf::TrackRole::Melody));                                    // invalid channel
        st.sections.push_back(sec);
        const PartLayout l = partLayoutFromRoles(st);
        TEST("second Bass channel gets no part (one owner per part)", l.part_of_channel[0] == (uint8_t)StylePart::Bass &&
             l.part_of_channel[1] == kNoPart);
        TEST("accompaniment in channel order Acc1..Acc5, the 6th is dropped",
             l.part_of_channel[2] == (uint8_t)StylePart::Acc1 && l.part_of_channel[6] == (uint8_t)StylePart::Acc5 &&
             l.part_of_channel[7] == kNoPart);
        const auto m = buildChannelMap(l, preset, false);
        TEST("unmapped channels drop; voice select kept when not asked", m.dst[1] == control::OutputChannelMap::kDrop &&
             m.dst[7] == control::OutputChannelMap::kDrop && m.dst[0] == 8 && !m.dropVoiceSelect);
    }

    // ── Text form ──
    {
        const std::string t = formatRoutingProfile(preset);
        TEST("format: 1-based channels in part order",
             t == "drum=10,perc=11,bass=9,acc1=12,acc2=13,acc3=14,acc4=15,acc5=16");
        const auto back = parseRoutingProfile(t);
        TEST("parse(format(p)) keeps every channel; evidence becomes UserSet", back && back->channel == preset.channel &&
             back->evidence[0] == RouteEvidence::UserSet);
        const auto partial = parseRoutingProfile("bass=2,drum=off");
        TEST("partial text: named parts set, others unassigned", partial && partial->channel[2] == 1 &&
             partial->channel[0] == kUnassigned && partial->channel[3] == kUnassigned);
        TEST("refused: empty, unknown part, repeated part, 0, 17, 07, x, missing '='",
             !parseRoutingProfile("") && !parseRoutingProfile("lead=1") && !parseRoutingProfile("bass=1,bass=2") &&
             !parseRoutingProfile("bass=0") && !parseRoutingProfile("bass=17") && !parseRoutingProfile("bass=07") &&
             !parseRoutingProfile("bass=x") && !parseRoutingProfile("bass"));
        TEST("refused: two parts on one physical channel", !parseRoutingProfile("bass=9,acc1=9"));
    }

    // ── Facade: the preset applied while stopped; ownership through Stop and Panic ──
    {
        FacadeRig r;
        TEST("preset map accepted while stopped", r.fac.setOutputChannelMap(buildChannelMap(demoLayout, preset, true)));
        r.fout.sent.clear();
        r.fac.transportStart();
        r.play(37);   // stop in the middle of bar 1 (notes held)
        const size_t beforeStop = r.fout.sent.size();
        r.fac.transportStop();
        r.fac.tick(0);
        std::set<int> chans;
        for (const auto& e : r.fout.sent) chans.insert(e.channel);
        TEST("demo output only on physical ch 9, 10, 12", !chans.empty() && [&] {
            for (int c : chans) if (c != 8 && c != 9 && c != 11) return false; return true; }());
        TEST("Stop came with notes held", beforeStop > 0 && !balanced({r.fout.sent.begin(), r.fout.sent.begin() + (long)beforeStop}));
        TEST("after Stop every (channel, note) is balanced: Note Off on the Note On's channel", balanced(r.fout.sent));

        r.fout.sent.clear();
        r.fac.transportStart();
        r.play(37);
        r.fac.panic();
        r.fac.tick(0);
        TEST("after Panic every (channel, note) is balanced", balanced(r.fout.sent));
        bool panicOnlyMapped = true;
        for (const auto& e : r.fout.sent) panicOnlyMapped &= e.channel == 8 || e.channel == 9 || e.channel == 11;
        TEST("Panic output stays on the mapped channels", panicOnlyMapped);
    }

    // ── Facade: a routing change during playback is refused; no note changes channel ──
    {
        FacadeRig r;
        r.fac.setOutputChannelMap(buildChannelMap(demoLayout, preset, true));
        r.fout.sent.clear();
        r.fac.transportStart();
        r.play(37);
        auto other = preset; other.channel[(size_t)StylePart::Bass] = 4;   // bass -> ch 5
        TEST("change refused while playing; map unchanged",
             !r.fac.setOutputChannelMap(buildChannelMap(demoLayout, other, true)) && r.fac.outputChannelMap().dst[1] == 8);
        r.play(37);
        r.fac.transportStop();
        r.fac.tick(0);
        bool noCh5 = true;
        for (const auto& e : r.fout.sent) noCh5 &= e.channel != 4;
        TEST("nothing reached ch 5 while playing; balanced after Stop", noCh5 && balanced(r.fout.sent));
        TEST("change accepted after Stop", r.fac.setOutputChannelMap(buildChannelMap(demoLayout, other, true)));
        r.fout.sent.clear();
        r.fac.transportStart();
        r.play(400);
        r.fac.transportStop();
        r.fac.tick(0);
        bool bassOn5 = false, bassOn9 = false;
        for (const auto& e : r.fout.sent) { bassOn5 |= e.channel == 4; bassOn9 |= e.channel == 8; }
        TEST("next run: bass on ch 5 only, balanced", bassOn5 && !bassOn9 && balanced(r.fout.sent));
        r.fac.stop();
    }

    // ── Session field: optional, validated, old files unchanged ──
    {
        using session::SessionPersistence;
        session::PerformerSession s;
        s.output_routing = formatRoutingProfile(preset);
        session::PerformerSession back;
        TEST("session round-trip keeps output_routing",
             SessionPersistence::deserialize(SessionPersistence::serialize(s), back) && back.output_routing == s.output_routing);
        const std::string v1 =   // a file written before the field existed
            "{\n  \"version\": 1,\n  \"style_name\": \"x\",\n  \"variation\": 2,\n  \"tempo_bpm\": 98,\n"
            "  \"split_point\": 55,\n  \"manual_bass\": 1,\n  \"sync_armed\": 0,\n  \"chord_scan_mode\": 1,\n"
            "  \"groove_profile\": 3,\n  \"midi_output_name\": \"Pa5X SOUND\",\n  \"ui_layout\": 1,\n  \"theme\": 2\n}\n";
        session::PerformerSession old;
        TEST("old file without the field loads; every value kept; routing empty",
             SessionPersistence::deserialize(v1, old) && old.output_routing.empty() && old.tempo_bpm == 98 &&
             old.split_point == 55 && old.theme == 2 && old.midi_output_name == "Pa5X SOUND");
        for (const char* tricky : {"output_routing", "tempo_bpm", "theme"}) {
            session::PerformerSession t;
            t.style_name = tricky;            // a key name as the style name (review of 27ca014)
            t.tempo_bpm = 97;
            session::PerformerSession u;
            const bool ok = SessionPersistence::deserialize(SessionPersistence::serialize(t), u) &&
                            u.style_name == t.style_name && u.tempo_bpm == 97 && u.output_routing.empty();
            if (!ok) std::fprintf(stderr, "    style name %s\n", tricky);
            TEST("a style named like a key loads; that key is read from its property, not the value", ok);
        }
        {   // a quoted key inside a value is escaped (\"), so it is never taken as the property.
            // (readStr does not decode escapes, so such a style name does not round-trip; pre-existing.)
            session::PerformerSession t;
            t.style_name = "a\"output_routing\": \"bass=1";
            t.tempo_bpm = 97;
            session::PerformerSession u;
            TEST("escaped \"output_routing\": inside a value is not read as the routing",
                 SessionPersistence::deserialize(SessionPersistence::serialize(t), u) && u.output_routing.empty() && u.tempo_bpm == 97);
        }
        {   // the old (v1) layout with that style name: loads, routing empty
            std::string v1b = v1;
            v1b.replace(v1b.find("\"x\""), 3, "\"output_routing\"");
            session::PerformerSession o;
            TEST("old file with style_name \"output_routing\" loads", SessionPersistence::deserialize(v1b, o) &&
                 o.style_name == "output_routing" && o.output_routing.empty() && o.tempo_bpm == 98);
        }
        s.output_routing = "bass=9,acc1=9";
        TEST("invalid routing refused by isValid (save would fail)", !SessionPersistence::isValid(s));
    }

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
