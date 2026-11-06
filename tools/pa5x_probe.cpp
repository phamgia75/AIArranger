// pa5x-probe: one short, bounded Pa5X hardware test per run (non-interactive,
// so it can be driven step by step over SSH). Runbook: docs/PA5X_FIRST_EXECUTION_LOG.md.
//
//   pa5x-probe listen  [--seconds 20] [--channel 1-16|any] [--split 60] [--mode fingered|onbass|single]
//                      [--source "Pa5X KEYBOARD"]
//       RECEIVE ONLY. Prints every Note On/Off from the Pa5X keyboard port and the
//       chord the engine's detector finds in the keys held below the split.
//
//   pa5x-probe note    --dest "Pa5X SOUND" --channel N --note 60 [--vel 64] [--ms 400] --confirm-send
//   pa5x-probe alloff  --dest "Pa5X SOUND" --confirm-send
//   pa5x-probe control --dest "Pa5X SOUND" --channel N --cmd variation1 --confirm-send
//
// Sending is fail-closed: an exact destination name (one match only) AND
// --confirm-send are required. The flag records the operator's approval; it
// does not replace it. `note` and `alloff` end with All Sound Off on the
// destination; `control` sends one Program Change and nothing else (no silence,
// so a playing style keeps sounding).
#include "engine/chord/fingered_detector.h"
#include "engine/chord/fingered_on_bass_detector.h"
#include "engine/chord/single_finger_detector.h"
#include "engine/midi/coremidi_in.h"
#include "engine/midi/coremidi_out.h"
#include "midi/pa5x_probe.h"
#include "midi/pa5x_profile.h"
#include <CoreMIDI/CoreMIDI.h>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace ai_arranger;
using namespace ai_arranger::midi;
namespace probe = ai_arranger::midi::pa5x::probe;

namespace {

std::map<std::string, std::string> parseFlags(int argc, char** argv, int from) {
    std::map<std::string, std::string> f;
    for (int i = from; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) { f["!bad"] = k; break; }
        if (k == "--confirm-send") { f[k] = "1"; continue; }
        if (i + 1 >= argc) { f["!bad"] = k; break; }
        f[k] = argv[++i];
    }
    return f;
}

int intFlag(const std::map<std::string, std::string>& f, const char* k, int def, int lo, int hi, bool& ok) {
    auto it = f.find(k);
    if (it == f.end()) return def;
    char* end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    if (*end != '\0' || v < lo || v > hi) { std::fprintf(stderr, "%s must be %d..%d\n", k, lo, hi); ok = false; return def; }
    return static_cast<int>(v);
}

int listen(const std::map<std::string, std::string>& f) {
    bool ok = true;
    const int seconds = intFlag(f, "--seconds", 20, 1, 600, ok);
    const int split = intFlag(f, "--split", 60, 1, 128, ok);
    std::optional<uint8_t> channel;   // nullopt = every channel
    if (auto it = f.find("--channel"); it != f.end() && it->second != "any") {
        channel = probe::parseChannel(it->second);
        if (!channel) { std::fprintf(stderr, "--channel must be 1-16 or any\n"); ok = false; }
    }
    const std::string mode = f.count("--mode") ? f.at("--mode") : "fingered";
    chord::FingeredDetector fingered;
    chord::FingeredOnBassDetector onBass;
    chord::SingleFingerDetector single;
    const chord::IChordScanMode* det = mode == "fingered" ? static_cast<const chord::IChordScanMode*>(&fingered)
                                     : mode == "onbass" ? static_cast<const chord::IChordScanMode*>(&onBass)
                                     : mode == "single" ? static_cast<const chord::IChordScanMode*>(&single) : nullptr;
    if (!det) { std::fprintf(stderr, "--mode must be fingered, onbass or single\n"); ok = false; }
    if (!ok) return 2;

    CoreMidiIn in;
    if (!in.initialize("pa5x-probe")) { std::fprintf(stderr, "CoreMIDI unavailable\n"); return 1; }
    const auto sources = in.enumerateSources();
    int pick = -1, matches = 0;
    std::string pickName;
    for (const auto& s : sources) {
        const bool hit = f.count("--source") ? s.name == f.at("--source")
                                             : pa5x::classify(s.name, true) == pa5x::PortMatch::Keyboard;
        if (hit) { pick = s.index; pickName = s.name; ++matches; }
    }
    if (matches != 1) {
        std::fprintf(stderr, "need exactly one matching source, found %d. Sources:\n", matches);
        for (const auto& s : sources) std::fprintf(stderr, "  [%d] %s\n", s.index, s.name.c_str());
        return 3;
    }

    // shortcut: printf + a mutex on the CoreMIDI read thread breaks CoreMidiIn's
    // "cheap, lock-free sink" rule; fine for a human-paced diagnostic, move to a
    // ring buffer + printer thread if this ever feeds timing measurements.
    std::mutex mu;
    probe::HeldNotes held;
    std::atomic<uint64_t> events{0};
    in.setSink([&](const MidiInputMessage& m) {
        if (channel && m.channel != *channel) return;
        std::lock_guard<std::mutex> lk(mu);
        if (!held.apply(m)) return;
        ++events;
        const bool down = m.type == MidiInMsgType::NoteOn && m.data2 > 0;
        std::printf("ch %2d  %-3s %3d %s%d vel %3d", m.channel + 1, down ? "ON" : "off", m.data1,
                    probe::pitchName(m.data1), m.data1 / 12 - 1, m.data2);
        uint8_t notes[128];
        const size_t n = held.below(m.channel, static_cast<uint8_t>(split), notes, sizeof notes);
        if (n == 0) { std::printf("   | below split: none\n"); }
        else {
            const arranger::Chord c = det->detect(notes, n);
            std::printf("   | held below %d:", split);
            for (size_t i = 0; i < n; ++i) std::printf(" %s%d", probe::pitchName(notes[i]), notes[i] / 12 - 1);
            if (c.type == arranger::ChordType::NoChord) std::printf("  -> NoChord\n");
            else std::printf("  -> %s%s (bass %s)\n", probe::pitchName(c.root), probe::chordTypeName(c.type),
                             probe::pitchName(c.bass ? c.bass : c.root));
        }
        std::fflush(stdout);
    });
    if (!in.selectSource(pick)) { std::fprintf(stderr, "could not connect source [%d]\n", pick); return 1; }
    std::printf("listening %d s on [%d] %s (receive only: nothing is sent), split %d, mode %s, channel %s\n",
                seconds, pick, pickName.c_str(), split, det->name(),
                channel ? std::to_string(*channel + 1).c_str() : "any");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    in.setSink(nullptr);   // blocks until the read thread has left the sink
    in.shutdown();
    std::printf("done: %llu note events\n", static_cast<unsigned long long>(events.load()));
    return 0;
}

std::string endpointName(MIDIEndpointRef ep) {
    CFStringRef cf = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf) != noErr || !cf) return {};
    char buf[256] = {0};
    CFStringGetCString(cf, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(cf);
    return buf;
}

// One Program Change with nothing after it. CoreMidiOut always silences the device
// on deselect/shutdown, which would cut a playing style right after a Variation
// command, so this path talks to CoreMIDI directly with the same guard.
int sendProgramChangeOnly(const std::string& dest, uint8_t ch, uint8_t program) {
    if (const char* g = std::getenv("AIARR_MIDI_HW_TX"); g && std::string(g) == "0") {
        std::fprintf(stderr, "refused: AIARR_MIDI_HW_TX=0 is set\n"); return 2;
    }
    MIDIEndpointRef ep = 0;
    int matches = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
        if (endpointName(MIDIGetDestination(i)) == dest) { ep = MIDIGetDestination(i); ++matches; }
    if (matches != 1) { std::fprintf(stderr, "refused: %d destinations named exactly \"%s\"\n", matches, dest.c_str()); return 3; }
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    if (MIDIClientCreate(CFSTR("pa5x-probe"), nullptr, nullptr, &client) != noErr ||
        MIDIOutputPortCreate(client, CFSTR("pa5x-probe out"), &port) != noErr) {
        std::fprintf(stderr, "CoreMIDI unavailable\n"); return 1;
    }
    std::printf("send: Program Change %d on ch %d, nothing else\n", program, ch + 1);
    Byte buf[64];
    auto* list = reinterpret_cast<MIDIPacketList*>(buf);
    MIDIPacket* pkt = MIDIPacketListInit(list);
    const Byte msg[2] = {static_cast<Byte>(0xC0 | (ch & 0x0F)), static_cast<Byte>(program & 0x7F)};
    pkt = MIDIPacketListAdd(list, sizeof buf, pkt, 0, sizeof msg, msg);
    const OSStatus st = pkt ? MIDISend(port, ep, list) : -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // let the driver take it before disposing
    MIDIClientDispose(client);
    std::printf("MIDISend status %d (0 = handed to CoreMIDI)\n", static_cast<int>(st));
    return st == noErr ? 0 : 1;
}

int send(const std::string& cmd, const std::map<std::string, std::string>& f) {
    if (!f.count("--confirm-send")) { std::fprintf(stderr, "refused: sending needs --confirm-send (operator approval)\n"); return 2; }
    if (!f.count("--dest")) { std::fprintf(stderr, "refused: --dest \"<exact destination name>\" is required\n"); return 2; }
    bool ok = true;
    std::optional<uint8_t> ch;
    if (cmd != "alloff") {
        ch = f.count("--channel") ? probe::parseChannel(f.at("--channel")) : std::nullopt;
        if (!ch) { std::fprintf(stderr, "--channel 1-16 is required\n"); ok = false; }
    }
    const int note = intFlag(f, "--note", 60, 0, 127, ok);
    const int vel = intFlag(f, "--vel", 64, 1, 127, ok);
    const int ms = intFlag(f, "--ms", 400, 20, 5000, ok);
    std::optional<pa5x::Command> ctl;
    if (cmd == "control") {
        ctl = f.count("--cmd") ? probe::commandFromName(f.at("--cmd")) : std::nullopt;
        if (!ctl) { std::fprintf(stderr, "--cmd unknown (see --help)\n"); ok = false; }
    }
    if (!ok) return 2;

    if (cmd == "control") {
        const int r = sendProgramChangeOnly(f.at("--dest"), *ch, static_cast<uint8_t>(*ctl));
        if (r == 0) std::printf("done. Observe the Pa5X and record the result.\n");
        return r;
    }

    CoreMidiOut out;
    if (!out.initialize("pa5x-probe")) { std::fprintf(stderr, "CoreMIDI unavailable\n"); return 1; }
    if (!out.hardwareTransmitAllowed()) { std::fprintf(stderr, "refused: AIARR_MIDI_HW_TX=0 is set\n"); return 2; }
    if (!out.selectDestinationByName(f.at("--dest"))) {
        std::fprintf(stderr, "refused: no single destination named exactly \"%s\". Destinations:\n", f.at("--dest").c_str());
        for (const auto& d : out.enumerateDestinations()) std::fprintf(stderr, "  [%d] %s (uid %d)\n", d.index, d.name.c_str(), d.uid);
        return 3;
    }
    const auto waitDispatched = [&](uint64_t n) {
        for (int i = 0; i < 200 && out.dispatchedCount() < n; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    using T = uasf::MidiEventType;
    if (cmd == "note") {
        std::printf("send: Note On ch %d note %d vel %d, %d ms, Note Off, then All Sound Off\n", *ch + 1, note, vel, ms);
        out.send({T::NoteOn, *ch, static_cast<uint8_t>(note), static_cast<uint8_t>(vel), 0});
        waitDispatched(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        out.send({T::NoteOff, *ch, static_cast<uint8_t>(note), 0, 0});
        waitDispatched(2);
    } else {
        std::printf("send: All Sound Off (CC 120) + All Notes Off (CC 123) on all 16 channels\n");
    }
    std::printf("sent bytes so far: %llu\n", static_cast<unsigned long long>(out.sentCount()));
    out.shutdown();   // All Sound Off on the still-selected destination
    std::printf("done (bytes sent: see above; shutdown adds the silence). Observe the Pa5X and record the result.\n");
    return 0;
}

void help() {
    std::puts("usage: pa5x-probe listen|note|alloff|control [flags]   (see the header of tools/pa5x_probe.cpp)\n"
              "control --cmd: variation1-4 fill1-4 break intro1 intro2 ending1-3 playstop1 playstop2");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "--help")) { help(); return argc < 2 ? 2 : 0; }
    const std::string cmd = argv[1];
    const auto f = parseFlags(argc, argv, 2);
    if (f.count("!bad")) { std::fprintf(stderr, "bad argument: %s\n", f.at("!bad").c_str()); return 2; }
    if (cmd == "listen") return listen(f);
    if (cmd == "note" || cmd == "alloff" || cmd == "control") return send(cmd, f);
    help();
    return 2;
}
