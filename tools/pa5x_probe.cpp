// pa5x-probe: one short, bounded Pa5X hardware test per run (non-interactive,
// so it can be driven step by step over SSH). Runbook: docs/PA5X_FIRST_EXECUTION_LOG.md.
//
//   pa5x-probe listen  [--seconds 20] [--channel 1-16|any] [--split 60] [--mode fingered|onbass|single]
//                      [--source "Pa5X KEYBOARD"]
//       RECEIVE ONLY. Prints every Note On/Off from the Pa5X keyboard port and the
//       chord the engine's detector finds in the keys held below the split.
//
//   pa5x-probe listen --raw [--seconds 20] [--source "Pa5X KEYBOARD"]
//       RECEIVE ONLY, below the parser: every non-realtime message as hex, and a
//       count of each realtime byte (clock, Active Sensing...). Shows whether the
//       port delivers anything at all.
//
//   pa5x-probe note    --dest "Pa5X SOUND" --channel N --note 60 [--vel 64] [--ms 400] --confirm-send
//   pa5x-probe alloff  --dest "Pa5X SOUND" --confirm-send
//   pa5x-probe control --dest "Pa5X SOUND" --channel N --cmd variation1 --confirm-send
//   pa5x-probe clock   --dest "Pa5X SOUND" [--bpm 120] [--beats 8] --confirm-send
//       MIDI Start, 24 clocks per beat for --beats beats, Stop. Realtime bytes only:
//       no notes, no silence messages. Needs Pa5X Clock Source = External (USB).
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
#include <mach/mach_time.h>
#include <csignal>
#include <cstdlib>
#include <array>
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
        if (k == "--confirm-send" || k == "--raw") { f[k] = "1"; continue; }
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

std::string endpointName(MIDIEndpointRef ep);

int listenRaw(const std::map<std::string, std::string>& f) {
    bool ok = true;
    const int seconds = intFlag(f, "--seconds", 20, 1, 600, ok);
    if (!ok) return 2;
    // The endpoint list can read empty until this process has a CoreMIDI client.
    MIDIClientRef client = 0;
    if (MIDIClientCreate(CFSTR("pa5x-probe raw"), nullptr, nullptr, &client) != noErr) {
        std::fprintf(stderr, "CoreMIDI unavailable\n"); return 1;
    }
    MIDIEndpointRef src = 0;
    std::string name;
    int matches = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        const std::string n = endpointName(MIDIGetSource(i));
        const bool hit = f.count("--source") ? n == f.at("--source") : pa5x::classify(n, true) == pa5x::PortMatch::Keyboard;
        if (hit) { src = MIDIGetSource(i); name = n; ++matches; }
    }
    if (matches != 1) {
        std::fprintf(stderr, "need exactly one matching source, found %d\n", matches);
        MIDIClientDispose(client); return 3;
    }
    MIDIPortRef port = 0;
    struct State {
        std::mutex mu;
        std::array<uint64_t, 8> realtime{};   // 0xF8..0xFF
        uint64_t other = 0;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    } state;
    State* st = &state;   // blocks capture by const copy; share through a pointer
    if (MIDIInputPortCreateWithBlock(client, CFSTR("pa5x-probe raw in"), &port,
            ^(const MIDIPacketList* list, void*) {
                // shortcut: printing on the CoreMIDI thread, same as the parsed listen.
                std::lock_guard<std::mutex> lk(st->mu);
                const MIDIPacket* p = &list->packet[0];
                for (UInt32 k = 0; k < list->numPackets; ++k, p = MIDIPacketNext(p)) {
                    std::string hex;
                    for (UInt16 b = 0; b < p->length; ++b) {
                        const Byte x = p->data[b];
                        if (x >= 0xF8) { ++st->realtime[x - 0xF8]; continue; }
                        char h[4]; std::snprintf(h, sizeof h, "%02X ", x); hex += h;
                    }
                    if (!hex.empty()) {
                        ++st->other;
                        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st->t0).count();
                        std::printf("+%9.1f ms  %s\n", ms, hex.c_str());
                        std::fflush(stdout);
                    }
                }
            }) != noErr ||
        MIDIPortConnectSource(port, src, nullptr) != noErr) {
        std::fprintf(stderr, "CoreMIDI unavailable\n"); return 1;
    }
    std::printf("raw listening %d s on %s (receive only: nothing is sent)\n", seconds, name.c_str());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    MIDIPortDisconnectSource(port, src);
    MIDIClientDispose(client);   // no more callbacks after this returns
    std::lock_guard<std::mutex> lk(state.mu);
    static const char* rt[] = {"F8 clock", "F9", "FA start", "FB continue", "FC stop", "FD", "FE active sensing", "FF reset"};
    std::printf("done: %llu non-realtime packets;", static_cast<unsigned long long>(state.other));
    for (int i = 0; i < 8; ++i) if (state.realtime[i]) std::printf(" %s x%llu;", rt[i], static_cast<unsigned long long>(state.realtime[i]));
    std::printf("\n");
    return 0;
}

int listen(const std::map<std::string, std::string>& f) {
    if (f.count("--raw")) return listenRaw(f);
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
    std::printf("done: %llu note events, %llu messages parsed in total (realtime bytes are not counted here; use --raw)\n",
                static_cast<unsigned long long>(events.load()), static_cast<unsigned long long>(in.receivedCount()));
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
    // The endpoint list can read empty until this process has a CoreMIDI client.
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    if (MIDIClientCreate(CFSTR("pa5x-probe"), nullptr, nullptr, &client) != noErr ||
        MIDIOutputPortCreate(client, CFSTR("pa5x-probe out"), &port) != noErr) {
        std::fprintf(stderr, "CoreMIDI unavailable\n"); return 1;
    }
    MIDIEndpointRef ep = 0;
    int matches = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
        if (endpointName(MIDIGetDestination(i)) == dest) { ep = MIDIGetDestination(i); ++matches; }
    if (matches != 1) {
        std::fprintf(stderr, "refused: %d destinations named exactly \"%s\"\n", matches, dest.c_str());
        MIDIClientDispose(client); return 3;
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

// Start / clocks / Stop with CoreMIDI timestamps, through the same guards as
// sendProgramChangeOnly (env switch, one exact destination match).
int sendClock(const std::string& dest, int bpm, int beats) {
    if (const char* g = std::getenv("AIARR_MIDI_HW_TX"); g && std::string(g) == "0") {
        std::fprintf(stderr, "refused: AIARR_MIDI_HW_TX=0 is set\n"); return 2;
    }
    const auto plan = probe::clockPlan(bpm, beats);
    if (plan.empty()) { std::fprintf(stderr, "--bpm 20-300 and --beats 1-256\n"); return 2; }
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    if (MIDIClientCreate(CFSTR("pa5x-probe"), nullptr, nullptr, &client) != noErr ||
        MIDIOutputPortCreate(client, CFSTR("pa5x-probe out"), &port) != noErr) {
        std::fprintf(stderr, "CoreMIDI unavailable\n"); return 1;
    }
    MIDIEndpointRef ep = 0;
    int matches = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
        if (endpointName(MIDIGetDestination(i)) == dest) { ep = MIDIGetDestination(i); ++matches; }
    if (matches != 1) {
        std::fprintf(stderr, "refused: %d destinations named exactly \"%s\"\n", matches, dest.c_str());
        MIDIClientDispose(client); return 3;
    }
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const uint64_t t0 = mach_absolute_time() + 100000000ull * tb.denom / tb.numer;   // start 100 ms from now
    std::printf("send: Start, %d clocks (%d beats at %d BPM), Stop: %zu realtime bytes, nothing else\n",
                beats * 24, beats, bpm, plan.size());
    std::vector<Byte> buf(plan.size() * 16 + 64);
    auto* list = reinterpret_cast<MIDIPacketList*>(buf.data());
    MIDIPacket* pkt = MIDIPacketListInit(list);
    for (const auto& b : plan) {
        const Byte msg = b.status;
        pkt = MIDIPacketListAdd(list, buf.size(), pkt, t0 + b.ns * tb.denom / tb.numer, 1, &msg);
        if (!pkt) { std::fprintf(stderr, "packet list overflow\n"); MIDIClientDispose(client); return 1; }
    }
    const OSStatus st = MIDISend(port, ep, list);
    // Keep the client alive until every scheduled byte is due: disposing it may drop pending packets.
    std::this_thread::sleep_for(std::chrono::nanoseconds(100000000ull + plan.back().ns + 300000000ull));
    MIDIClientDispose(client);
    std::printf("MIDISend status %d (0 = handed to CoreMIDI)\n", static_cast<int>(st));
    return st == noErr ? 0 : 1;
}

int send(const std::string& cmd, const std::map<std::string, std::string>& f) {
    if (!f.count("--confirm-send")) { std::fprintf(stderr, "refused: sending needs --confirm-send (operator approval)\n"); return 2; }
    if (!f.count("--dest")) { std::fprintf(stderr, "refused: --dest \"<exact destination name>\" is required\n"); return 2; }
    bool ok = true;
    std::optional<uint8_t> ch;
    if (cmd == "clock") {
        const int bpm = intFlag(f, "--bpm", 120, 20, 300, ok);
        const int beats = intFlag(f, "--beats", 8, 1, 256, ok);
        if (!ok) return 2;
        const int r = sendClock(f.at("--dest"), bpm, beats);
        if (r == 0) std::printf("done. Observe the Pa5X tempo display and record the result.\n");
        return r;
    }
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
        // The hold is bounded (--ms <= 5000): a dropped SSH session (SIGHUP/SIGPIPE) or Ctrl-C must
        // not kill us between Note On and the Note Off + All Sound Off below. SIGKILL: run `alloff`.
        for (int sig : {SIGHUP, SIGINT, SIGTERM, SIGPIPE}) std::signal(sig, SIG_IGN);
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
    std::puts("usage: pa5x-probe listen|note|alloff|control|clock [flags]   (see the header of tools/pa5x_probe.cpp)\n"
              "control --cmd: variation1-4 fill1-4 break intro1 intro2 ending1-3 playstop1 playstop2");
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // events must reach a pipe/log live, not only at exit
    if (argc < 2 || !std::strcmp(argv[1], "--help")) { help(); return argc < 2 ? 2 : 0; }
    const std::string cmd = argv[1];
    const auto f = parseFlags(argc, argv, 2);
    if (f.count("!bad")) { std::fprintf(stderr, "bad argument: %s\n", f.at("!bad").c_str()); return 2; }
    if (cmd == "listen") return listen(f);
    if (cmd == "note" || cmd == "alloff" || cmd == "control" || cmd == "clock") return send(cmd, f);
    help();
    return 2;
}
