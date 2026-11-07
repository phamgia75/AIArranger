#include "midi/midi_input_parser.h"

namespace ai_arranger::midi {

size_t parseMidiInput(const uint8_t* bytes, size_t len,
                      MidiInputMessage* out, size_t maxOut) noexcept {
    if (!out || maxOut == 0) return 0;
    size_t n = 0;
    forEachMidiInput(bytes, len, [&](const MidiInputMessage& m) { if (n < maxOut) out[n++] = m; });
    return n;
}

} // namespace ai_arranger::midi
