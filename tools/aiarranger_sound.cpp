// aiarranger-sound demo --out DIR (roadmap S4): builds the original demo organ and writes the
// USB-ready package folder (media/sound/package_writer.h: .sf2, tx/ TXPROG, wav/, REPORT.txt).
// Refuses to write into an existing DIR. Exit 0 = written, 1 = usage/IO/validation error.
#include "media/sound/additive_source.h"
#include "media/sound/package_writer.h"
#include <cstdio>
#include <string>

using namespace ai_arranger::media::sound;

int main(int argc, char** argv) {
    if (argc != 4 || std::string(argv[1]) != "demo" || std::string(argv[2]) != "--out") {
        std::fprintf(stderr, "usage: aiarranger-sound demo --out DIR   (DIR must not exist)\n");
        return 1;
    }
    double worst = 0;
    const Instrument in = buildDemoOrgan(&worst);
    char notes[96];
    std::snprintf(notes, sizeof notes, "Source: original additive synthesis, looped; worst pitch error %.4f cents\n", worst);
    std::string err;
    const std::string rep = writePackage(in, argv[3], notes, &err);
    if (rep.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::fputs(rep.c_str(), stdout);
    return 0;
}
