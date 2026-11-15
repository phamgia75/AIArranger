// The committed Pa5X starter sound map parses with the real --sound-map parser,
// resolves all 8 Style tracks, and keeps drum/percussion kits on bank 120/0
// (manual appendix) and melodic sounds on bank 121.

#include "exporters/korg/sound_map.h"
#include <cstdio>
#include <fstream>

using namespace ai_arranger::exporters::korg;

int main() {
    const std::string path = std::string(REPO_DIR) + "/docs/exporters/sound-maps/pa5x-starter.soundmap";
    std::ifstream f(path);
    const auto r = parseSoundMap(f, "pa5x-starter.soundmap");
    int bad = 0;
    auto check = [&](const char* name, bool ok) {
        std::printf("  %s: %s\n", ok ? "PASS" : "FAIL", name);
        bad += ok ? 0 : 1;
    };
    check("file opens and parses", f.is_open() && r.ok);
    check("all 8 Style tracks resolved, none left open", r.sounds.size() == 8 && r.unresolved.empty());
    const auto drum = r.sounds.find(kDrumCh), perc = r.sounds.find(kPercCh);
    check("drum and perc kits on bank 120/0", drum != r.sounds.end() && perc != r.sounds.end() &&
          drum->second.msb == 120 && drum->second.lsb == 0 && perc->second.msb == 120 && perc->second.lsb == 0);
    bool melodic = true;
    for (const auto& [ch, s] : r.sounds)
        if (ch != kDrumCh && ch != kPercCh) melodic &= s.msb == 121;
    check("melodic tracks on bank 121", melodic);
    const auto acc4 = r.sounds.find(static_cast<uint8_t>(kAcc1Ch + 3));
    check("Austrian Grand = 121/20/0 (manual p.1572)", acc4 != r.sounds.end() &&
          acc4->second.lsb == 20 && acc4->second.pc == 0);
    return bad ? 1 : 0;
}
