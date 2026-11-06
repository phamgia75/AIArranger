// CoreMidiIn hot-plug recovery: the selected source is remembered by CoreMIDI
// unique ID. pollReconnect() drops it when it disappears and re-attaches it when
// a source with the same unique ID comes back (a Pa5X USB replug), and never
// attaches a different device. Uses process-owned virtual sources only.

#include "engine/midi/coremidi_in.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace ai_arranger;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } \
} while(0)

static int indexOf(const midi::CoreMidiIn& in, const std::string& name) {
    for (const auto& s : in.enumerateSources()) if (s.name == name) return s.index;
    return -1;
}

static MIDIEndpointRef makeSource(MIDIClientRef c, CFStringRef name, SInt32 uid) {
    MIDIEndpointRef ep = 0;
    if (MIDISourceCreate(c, name, &ep) != noErr) return 0;
    if (MIDIObjectSetIntegerProperty(ep, kMIDIPropertyUniqueID, uid) != noErr) {
        MIDIEndpointDispose(ep);
        return 0;
    }
    return ep;
}

static void sendNote(MIDIEndpointRef src, uint8_t note) {
    Byte buf[64];
    auto* pl = reinterpret_cast<MIDIPacketList*>(buf);
    MIDIPacket* p = MIDIPacketListInit(pl);
    const Byte on[3] = {0x90, note, 100};
    MIDIPacketListAdd(pl, sizeof(buf), p, 0, 3, on);
    MIDIReceived(src, pl);
}

static bool waitFor(const std::atomic<int>& v, int want) {
    for (int i = 0; i < 200 && v.load() != want; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return v.load() == want;
}

int main() {
    setenv("AIARR_MIDI_HW_TX", "0", 1);
    std::printf("Test: CoreMidiIn reconnects the selected source by unique ID\n");

    midi::CoreMidiIn in;
    if (!in.initialize("AIA reconnect test")) {
        std::printf("SKIP: CoreMIDI client unavailable (environment, #21)\n");
        return 77;
    }
    MIDIClientRef dev = 0;
    if (MIDIClientCreate(CFSTR("AIA fake device"), nullptr, nullptr, &dev) != noErr) {
        std::printf("SKIP: cannot create a second CoreMIDI client\n");
        return 77;
    }
    const SInt32 uid = 0x5A5A0000 | (static_cast<SInt32>(std::rand()) & 0xFFFF);
    const std::string name = "AIA Pa5X KEYBOARD " + std::to_string(uid);
    CFStringRef cfName = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);

    MIDIEndpointRef src = makeSource(dev, cfName, uid);
    if (src == 0) {
        std::printf("SKIP: cannot create a virtual source with a fixed unique ID\n");
        return 77;
    }

    std::atomic<int> notes{0};
    in.setSink([&](const midi::MidiInputMessage& m) {
        if (m.type == midi::MidiInMsgType::NoteOn) notes.fetch_add(1);
    });

    TEST("nothing selected: pollReconnect never auto-connects", !in.pollReconnect() && !in.hasLiveSource());
    TEST("select the fake device", in.selectSource(indexOf(in, name)));
    TEST("selection remembers the unique ID", in.selectedUniqueId() == uid);
    sendNote(src, 60);
    TEST("note received while connected", waitFor(notes, 1));
    TEST("pollReconnect with the device present is a no-op", in.pollReconnect() && in.reconnectCount() == 0);

    // Unplug.
    MIDIEndpointDispose(src);
    TEST("device gone: pollReconnect reports no live source", !in.pollReconnect() && !in.hasLiveSource());
    TEST("device gone: the unique ID is kept for recovery", in.selectedUniqueId() == uid);

    // A different device appears: must not be attached.
    const std::string other = name + " other";
    CFStringRef cfOther = CFStringCreateWithCString(nullptr, other.c_str(), kCFStringEncodingUTF8);
    MIDIEndpointRef decoy = makeSource(dev, cfOther, uid + 1);
    TEST("a different device is never attached", decoy != 0 && !in.pollReconnect() && !in.hasLiveSource());

    // Replug: same unique ID comes back as a new endpoint object.
    src = makeSource(dev, cfName, uid);
    TEST("device back: pollReconnect re-attaches it", src != 0 && in.pollReconnect() && in.hasLiveSource());
    TEST("reconnect counted once", in.reconnectCount() == 1);
    TEST("selected index follows the device", in.selectedSource() == indexOf(in, name));
    sendNote(src, 62);
    TEST("note received after reconnect", waitFor(notes, 2));
    if (decoy) { sendNote(decoy, 64); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    TEST("decoy traffic not received", notes.load() == 2);

    TEST("explicit disconnect clears the reconnect target",
         in.selectSource(-1) && in.selectedUniqueId() == 0 && !in.pollReconnect());
    TEST("failed selection leaves nothing selected",
         !in.selectSource(100000) && in.selectedUniqueId() == 0 && in.selectedSource() == -1);

    in.setSink(nullptr);
    in.shutdown();
    if (decoy) MIDIEndpointDispose(decoy);
    if (src) MIDIEndpointDispose(src);
    MIDIClientDispose(dev);
    CFRelease(cfName); CFRelease(cfOther);

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
