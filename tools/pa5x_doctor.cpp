// pa5x-doctor: read-only Pa5X USB-MIDI diagnostics.
//
// Lists every CoreMIDI source and destination with its unique ID and online
// state, and identifies the Pa5X ports by the profile in midi/pa5x_profile.h.
// It creates no ports and sends nothing.
//
// Exit codes: 0 both Pa5X ports found and online, 3 Pa5X not (fully) present,
// 1 CoreMIDI unavailable.

#include "midi/pa5x_profile.h"
#include <CoreMIDI/CoreMIDI.h>
#include <cstdio>
#include <string>

using namespace ai_arranger::midi;

namespace {
std::string nameOf(MIDIEndpointRef ep) {
    CFStringRef cf = nullptr;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf) != noErr || !cf) return {};
    char buf[256] = {0};
    CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf);
    return buf;
}
SInt32 intProp(MIDIEndpointRef ep, CFStringRef key) {
    SInt32 v = 0;
    MIDIObjectGetIntegerProperty(ep, key, &v);
    return v;
}
const char* label(pa5x::PortMatch m) {
    switch (m) {
        case pa5x::PortMatch::Sound:     return "Pa5X SOUND (engine -> Pa5X)";
        case pa5x::PortMatch::Keyboard:  return "Pa5X KEYBOARD (Pa5X -> engine)";
        case pa5x::PortMatch::Candidate: return "Pa5X candidate (name not in profile)";
        default:                         return "";
    }
}

// Prints one direction; returns how many profile-exact, online ports it found.
int list(const char* title, bool sources) {
    const ItemCount n = sources ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    std::printf("%s (%lu):\n", title, static_cast<unsigned long>(n));
    int found = 0;
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef ep = sources ? MIDIGetSource(i) : MIDIGetDestination(i);
        const std::string name = nameOf(ep);
        const bool offline = intProp(ep, kMIDIPropertyOffline) != 0;
        const auto m = pa5x::classify(name, sources);
        std::printf("  [%lu] %-28s uid=%-11d %s %s\n", static_cast<unsigned long>(i), name.c_str(),
                    static_cast<int>(intProp(ep, kMIDIPropertyUniqueID)),
                    offline ? "OFFLINE" : "online ", label(m));
        if (!offline && (m == pa5x::PortMatch::Sound || m == pa5x::PortMatch::Keyboard)) ++found;
    }
    return found;
}
} // namespace

int main() {
    MIDIClientRef client = 0;   // forces CoreMIDI to load its device list
    if (MIDIClientCreate(CFSTR("pa5x-doctor"), nullptr, nullptr, &client) != noErr) {
        std::fprintf(stderr, "CoreMIDI unavailable (MIDIClientCreate failed)\n");
        return 1;
    }
    std::printf("pa5x-doctor (read-only: no ports created, nothing sent)\n\n");
    const int src = list("Sources", true);
    const int dst = list("Destinations", false);
    MIDIClientDispose(client);

    std::printf("\n");
    if (src == 1 && dst == 1) {
        std::printf("RESULT: Pa5X found (KEYBOARD + SOUND online). Select the SOUND port by uid.\n");
        return 0;
    }
    std::printf("RESULT: Pa5X NOT ready (keyboard ports: %d, sound ports: %d; expected 1 and 1).\n"
                "Check the USB cable, the Pa5X power, and the KORG USB-MIDI driver.\n", src, dst);
    return 3;
}
