// aiarranger-style end to end: inspect / edit / export-smf / cv-copy exit codes,
// never overwriting an existing file, and no output when an edit is rejected.

#include "engine/uasf/deserializer.h"
#include "engine/uasf/serializer.h"
#include "exporters/korg/korg_style_smf.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace ai_arranger;
namespace fs = std::filesystem;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s (line %d)\n", name, __LINE__); ++failures; } \
    else { std::printf("  PASS: %s\n", name); ++passes; } } while (0)

static int run(const std::string& args) {
    const std::string cmd = std::string("\"") + STYLE_BIN + "\" " + args + (std::getenv("CLI_DEBUG") ? "" : " >/dev/null 2>&1");
    const int st = std::system(cmd.c_str());
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static std::string q(const fs::path& p) { return "\"" + p.string() + "\""; }
static std::vector<uint8_t> readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void writeBytes(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
static uasf::StyleDefinition load(const fs::path& p) {
    uasf::UasfDeserializer d;
    return d.deserializeFromFile(p.string()).style;
}

static uasf::StyleDefinition makeStyle() {
    uasf::StyleDefinition s;
    s.name = "CLI SYNTH";
    s.tempo_bpm = 110;
    s.resolution = 480;
    uasf::SectionDefinition sec;
    sec.type = uasf::SectionType::Main1;
    sec.name = "Main A";
    sec.bars = 1;
    sec.resolution = 480;
    sec.beats_per_bar = 4;
    sec.beat_note = 4;
    uasf::TrackDefinition drum{"Rhythm2", 9, uasf::TrackRole::Drum, true, {}, {}};
    uasf::TrackDefinition bass{"Bass", 10, uasf::TrackRole::Bass, false, {}, {}};
    for (uint64_t b = 0; b < 4; ++b) {
        drum.events.push_back({uasf::MidiEventType::NoteOn, 9, 36, 100, b * 480});
        drum.events.push_back({uasf::MidiEventType::NoteOff, 9, 36, 0, b * 480 + 60});
        bass.events.push_back({uasf::MidiEventType::NoteOn, 10, 40, 90, b * 480 + 7});
        bass.events.push_back({uasf::MidiEventType::NoteOff, 10, 40, 0, b * 480 + 400});
    }
    sec.tracks = {drum, bass};
    s.sections.push_back(sec);
    return s;
}

int main() {
    std::printf("Test: aiarranger-style CLI\n");
    const fs::path dir = fs::temp_directory_path() / ("aiarr_style_cli_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path in = dir / "in.uasf";
    uasf::UasfSerializer ser;
    writeBytes(in, ser.serialize(makeStyle()).data);

    TEST("no args -> usage 1", run("") == 1);
    TEST("inspect -> 0", run("inspect " + q(in)) == 0);
    TEST("inspect missing file -> 1", run("inspect " + q(dir / "nope.uasf")) == 1);

    const fs::path t1 = dir / "t1.uasf";
    TEST("edit transpose -> 0", run("edit " + q(in) + " --out " + q(t1) + " --op transpose --semitones 2") == 0);
    auto s1 = load(t1);
    TEST("bass transposed, drum not", s1.sections[0].tracks[1].events[0].data1 == 42 &&
                                      s1.sections[0].tracks[0].events[0].data1 == 36);
    const auto t1Bytes = readBytes(t1);
    TEST("existing --out refused -> 1", run("edit " + q(in) + " --out " + q(t1) + " --op tempo --bpm 90") == 1);
    TEST("existing file left as it was", readBytes(t1) == t1Bytes);

    const fs::path bad = dir / "bad.uasf";
    TEST("rejected edit -> 2", run("edit " + q(in) + " --out " + q(bad) + " --op tempo --bpm 0") == 2);
    TEST("rejected edit writes nothing", !fs::exists(bad) && !fs::exists(dir / "bad.uasf.tmp"));
    TEST("unknown op -> 2", run("edit " + q(in) + " --out " + q(bad) + " --op frobnicate") == 2);
    TEST("non-numeric value -> 2", run("edit " + q(in) + " --out " + q(bad) + " --op tempo --bpm fast") == 2);

    TEST("single-element op without --section -> 2", run("edit " + q(in) + " --out " + q(bad) + " --op meter --num 3 --den 4") == 2);
    const fs::path blocked = dir / "blocked.uasf";
    writeBytes(dir / "blocked.uasf.tmp", {1, 2, 3});
    TEST("existing temp path refused -> 1", run("edit " + q(in) + " --out " + q(blocked) + " --op tempo --bpm 90") == 1);
    TEST("existing temp file left as it was", readBytes(dir / "blocked.uasf.tmp") == std::vector<uint8_t>({1, 2, 3}));

    // D15 P3: single-file outputs refuse a mounted volume like the package writers (shared output guard)
    const std::string vol = "/Volumes/AIA-VOLTEST-" + std::to_string(::getpid());
    TEST("/Volumes --out refused -> 1", run("export-smf " + q(in) + " --out " + q(vol + "/x.mid")) == 1);
    TEST("/Volumes edit --out refused -> 1", run("edit " + q(in) + " --out " + q(vol + "/x.uasf") + " --op tempo --bpm 90") == 1);
    std::error_code lec;
    if (fs::is_directory("/Volumes")) {
        fs::create_directory_symlink("/Volumes", dir / "vol", lec);
        TEST("symlink into /Volumes refused -> 1",
             !lec && run("export-smf " + q(in) + " --out " + q(dir / "vol" / ("AIA-VOLTEST-" + std::to_string(::getpid())) / "x.mid")) == 1);
    }
    TEST("nothing created on /Volumes", !fs::exists(vol));

    const fs::path qz = dir / "q.uasf";
    TEST("edit quantize -> 0", run("edit " + q(in) + " --out " + q(qz) + " --op quantize --section 0 --track 1 --grid 1/16") == 0);
    TEST("quantized to the grid", load(qz).sections[0].tracks[1].events[0].tick == 0);
    TEST("quantize bad grid -> 2", run("edit " + q(in) + " --out " + q(bad) + " --op quantize --section 0 --grid 1/7") == 2);

    const fs::path mid = dir / "out.mid";
    TEST("export-smf -> 0", run("export-smf " + q(qz) + " --out " + q(mid)) == 0);
    const auto parsed = exporters::korg::parse(readBytes(mid));
    TEST("export is a valid Pa5X style SMF", parsed.ok && parsed.style.elements.size() == 1 &&
                                             parsed.style.elements[0].marker == "v1cv1" &&
                                             exporters::korg::validate(parsed.style).empty());

    const fs::path cv = dir / "cv.mid";
    TEST("cv-copy -> 0", run("cv-copy " + q(mid) + " --out " + q(cv) + " --from v1cv1 --to v1cv4") == 0);
    const auto cvp = exporters::korg::parse(readBytes(cv));
    TEST("cv-copy wrote 2 elements", cvp.ok && cvp.style.elements.size() == 2 && cvp.style.elements[1].marker == "v1cv4");
    TEST("cv-copy to invalid marker -> 2", run("cv-copy " + q(mid) + " --out " + q(dir / "x.mid") + " --from v1cv1 --to v1cv7") == 2);

    // Plain SMF (format 0, 480 PPQN, ch 1): one note at beat 2 into the bass track.
    const std::vector<uint8_t> smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                      'M', 'T', 'r', 'k', 0, 0, 0, 13,
                                      0x83, 0x60, 0x90, 45, 99, 0x83, 0x60, 0x80, 45, 0, 0x00, 0xFF, 0x2F};
    // Track length 13 cuts the End-of-Track length byte: a truncated file is rejected.
    const fs::path smfPath = dir / "plain.mid";
    writeBytes(smfPath, smf);
    const fs::path imp = dir / "imp.uasf";
    TEST("truncated SMF -> 2", run("edit " + q(in) + " --out " + q(imp) + " --op import-smf --section 0 --track 1 --smf " + q(smfPath)) == 2);
    TEST("no file after rejected import", !fs::exists(imp));
    std::vector<uint8_t> good = smf;
    good[21] = 14;
    good.push_back(0x00);
    writeBytes(smfPath, good);
    const fs::path imp2 = dir / "imp2.uasf";
    TEST("import-smf well-formed -> 0", run("edit " + q(in) + " --out " + q(imp2) + " --op import-smf --section 0 --track 1 --smf " + q(smfPath)) == 0);
    const auto si = load(imp2);
    TEST("imported note on the bass channel", si.sections[0].tracks[1].events.size() == 2 &&
                                              si.sections[0].tracks[1].events[0].tick == 480 &&
                                              si.sections[0].tracks[1].events[0].channel == 10 &&
                                              si.sections[0].tracks[1].events[0].data1 == 45);

    fs::remove_all(dir);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
