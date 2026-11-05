// MidiOutputManager re-resolves the remembered device by identity (unique ID)
// when the provider reports one: a different device that merely shares the
// display name is never adopted on refresh / hot-plug. Headless (fake provider).

#include "midi/midi_output_manager.h"
#include "fake_midi_output_provider.h"
#include <cstdio>

using namespace ai_arranger::midi;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

int main() {
    std::printf("Test: MIDI output manager device identity\n");
    FakeMidiOutputProvider p;
    MidiOutputManager mgr(p);

    p.setDevices({{0, "IAC Bus 1", 7}, {1, "Pa5X SOUND", 111}});
    mgr.refresh();
    TEST("select Pa5X SOUND (uid 111)", mgr.selectDevice(1) && mgr.state() == OutputState::Connected);

    // The device goes away and a DIFFERENT device with the same name appears.
    p.setDevices({{0, "IAC Bus 1", 7}});
    mgr.refresh();
    p.setDevices({{0, "IAC Bus 1", 7}, {1, "Pa5X SOUND", 222}});
    mgr.notifyHotPlug();
    TEST("same name, different uid: not adopted (DISCONNECTED)", mgr.state() == OutputState::Disconnected);
    TEST("same name, different uid: provider has no destination", p.selected() == -1 && !p.hasLiveDestination());
    mgr.refresh();
    TEST("refresh also refuses the impostor", mgr.state() == OutputState::Disconnected && p.selected() == -1);

    // The original device (uid 111) comes back: reconnects, even at a new index.
    p.setDevices({{0, "Pa5X SOUND", 111}, {1, "IAC Bus 1", 7}, {2, "Pa5X SOUND", 222}});
    mgr.notifyHotPlug();
    TEST("original uid back: CONNECTED at its new index",
         mgr.state() == OutputState::Connected && mgr.selectedIndex() == 0 && p.selected() == 0);

    // Snapshot race: the UI shows the list, then a hot-plug shifts it before the
    // pick. Picking index 1 ("Pa5X SOUND" uid 111 when shown) must select uid 111
    // or nothing — never the device that now sits at index 1.
    {
        FakeMidiOutputProvider r;
        MidiOutputManager m(r);
        r.setDevices({{0, "IAC Bus 1", 7}, {1, "Pa5X SOUND", 111}});
        m.refresh();                                            // list shown to the user
        r.devices = {{0, "IAC Bus 1", 7}, {1, "Other Synth", 333}};   // shifted underneath
        TEST("stale pick of a vanished device: rejected, nothing selected",
             !m.selectDevice(1) && r.selected() == -1 && !r.hasLiveDestination());
        r.devices = {{0, "Pa5X SOUND", 111}, {1, "IAC Bus 1", 7}};
        TEST("stale index, device moved: the device the user saw is selected",
             m.selectDevice(1) && r.selected() == 0 && r.devices[r.selected()].uid == 111);
    }

    // Providers without identity (uid 0) keep the by-name behaviour.
    FakeMidiOutputProvider q;
    MidiOutputManager m2(q);
    q.setDevices({{0, "Synth"}});
    m2.refresh();
    m2.selectDevice(0);
    q.setDevices({});
    m2.refresh();
    q.setDevices({{0, "Synth"}});
    m2.notifyHotPlug();
    TEST("no identity available: reconnect by name as before", m2.state() == OutputState::Connected);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
