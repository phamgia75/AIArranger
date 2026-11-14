// The live Pa5X style-track map and the marker-SMF exporter must route every
// Genos channel the same way, so a style sounds on the same Pa5X track live and
// after import.

#include "midi/pa5x_profile.h"
#include "exporters/korg/genos_to_korg.h"
#include <cstdio>

using namespace ai_arranger;

int main() {
    const auto m = midi::pa5x::genosStyleTrackMap();
    int bad = 0;
    for (uint8_t g = 0; g < 16; ++g) {
        const uint8_t want = exporters::korg::korgChannelFor(g);
        const uint8_t live = m.dst[g];
        if (want != live) { std::fprintf(stderr, "  FAIL: ch%d live %d exporter %d\n", g + 1, live, want); ++bad; }
    }
    if (!m.dropVoiceSelect) { std::fprintf(stderr, "  FAIL: preset keeps Yamaha voice select\n"); ++bad; }
    std::printf(bad ? "FAILED\n" : "  PASS: live map == exporter map for all 16 channels\n");
    return bad ? 1 : 0;
}
