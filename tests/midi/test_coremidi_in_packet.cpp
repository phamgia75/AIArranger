// CoreMidiIn::readProc fed a MIDIPacketList built in memory (MIDIPacketListInit/Add need no MIDI
// server or device, so this runs on CI where the virtual-source test skips, #21). Regression for the
// fixed 64-message buffer: a packet carrying more messages lost the rest and receivedCount undercounted.
#include "engine/midi/coremidi_in.h"
#include <cstdio>
#include <vector>

using namespace ai_arranger::midi;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

int main() {
    std::printf("Test: CoreMidiIn read callback, in-memory packets\n");
    CoreMidiIn in;   // not initialized: no CoreMIDI client is created
    std::vector<MidiInputMessage> got;
    in.setSink([&](const MidiInputMessage& m) { got.push_back(m); });

    alignas(MIDIPacketList) Byte buf[4096];
    auto* list = reinterpret_cast<MIDIPacketList*>(buf);
    MIDIPacket* pkt = MIDIPacketListInit(list);
    std::vector<Byte> big;   // 70 Note Offs on ch 4 in ONE packet, with an FE in the middle
    for (int k = 0; k < 70; ++k) {
        if (k == 35) big.push_back(0xFE);
        big.push_back(0x83); big.push_back(static_cast<Byte>(30 + k)); big.push_back(0x40);
    }
    pkt = MIDIPacketListAdd(list, sizeof buf, pkt, 0, big.size(), big.data());
    const Byte small[] = {0x93, 0x24, 0x15};   // a second packet: C2 Note On
    pkt = MIDIPacketListAdd(list, sizeof buf, pkt, 1, sizeof small, small);   // other timestamp: Add would merge equal ones
    TEST("packet list built (2 packets)", pkt != nullptr && list->numPackets == 2);

    CoreMidiIn::readProc(list, &in, nullptr);
    bool order = got.size() == 71;
    for (size_t k = 0; order && k < 70; ++k)
        order = got[k].type == MidiInMsgType::NoteOff && got[k].channel == 3 && got[k].data1 == 30 + k;
    TEST("all 70 messages of the large packet delivered, in order", order);
    TEST("second packet delivered after it", got.size() == 71 && got[70].type == MidiInMsgType::NoteOn && got[70].data1 == 0x24);
    TEST("receivedCount counts every delivered message", in.receivedCount() == 71);

    in.setSink(nullptr);   // quiesced: further packets reach no sink
    CoreMidiIn::readProc(list, &in, nullptr);
    TEST("no delivery after the sink is cleared", got.size() == 71 && in.receivedCount() == 71);

    std::printf("\nResults: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
