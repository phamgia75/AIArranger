// P1 report export (jobs/report_export.h, parity C17/C18): HTML escaping, CSV quoting and formula-injection guard,
// CSV -> rows round trip, HTML with no external reference, the four report kinds built from their structs / JSON,
// the report job (E6: staged, checked, published; refused inside its source) and the read-only report file loader.
// Synthetic data only (a library folder made here with hostile names).

#include "jobs/report_export.h"
#include "jobs/export_specs.h"
#include <cstdio>
#include <fstream>
#include <unistd.h>

using namespace ai_arranger;
using namespace ai_arranger::jobs;
namespace rp = ai_arranger::jobs::report;

static int failures = 0, passes = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { std::fprintf(stderr, "  FAIL: %s\n", name); failures++; } \
    else { std::printf("  PASS: %s\n", name); passes++; } } while (0)

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static JobState run(const ExportSpec& s) {
    JobState st;
    st.id = 7;
    runExport(s, st, [] { return false; }, [](const JobState&) {});
    return st;
}
static std::vector<std::vector<std::string>> withHeader(const rp::Table& t) {
    std::vector<std::vector<std::string>> w{t.columns};
    w.insert(w.end(), t.rows.begin(), t.rows.end());
    return w;
}
static bool roundTrips(const rp::Report& r) {
    for (const auto& t : r.tables) {
        std::vector<std::vector<std::string>> rows;
        if (!rp::parseCsv(rp::csv(t), &rows).empty() || rows != withHeader(t)) return false;
    }
    return true;
}

static const std::vector<std::string> kHostile = {"<script src=x>alert(1)<b>", "Quote\"d, comma", "line\nbreak", "=SUM(A1:A9)", "+1", "-2",
                                                  "@cmd", "\ttab", "'apos", "a&b"};

int main() {
    std::printf("Test: report export (HTML / CSV)\n");
    const fs::path root = fs::temp_directory_path() / ("aia-report-export-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // ── CSV cells: RFC 4180 quoting and the formula-injection guard ──
    TEST("csv: plain cell unchanged", rp::csvCell("Ballad 1") == "Ballad 1");
    TEST("csv: comma quoted", rp::csvCell("a,b") == "\"a,b\"");
    TEST("csv: quotes doubled inside quotes", rp::csvCell("say \"hi\"") == "\"say \"\"hi\"\"\"");
    TEST("csv: newline quoted", rp::csvCell("a\nb") == "\"a\nb\"" && rp::csvCell("a\r\nb") == "\"a\r\nb\"");
    for (const std::string lead : {"=", "+", "-", "@", "\t", "\r", "'"})
        TEST(("csv: leading " + std::string(lead == "\t" ? "TAB" : lead == "\r" ? "CR" : lead) + " gets a ' prefix").c_str(),
             rp::csvCell(lead + "1").rfind(lead == "\r" ? "\"'" : "'", 0) == 0);
    TEST("csv: =HYPERLINK with comma is prefixed and quoted", rp::csvCell("=HYPERLINK(\"x\",\"y\")") == "\"'=HYPERLINK(\"\"x\"\",\"\"y\"\")\"");
    TEST("csv: a table is CRLF-terminated, header first", rp::csv({"t", "T", {"a", "b"}, {{"1", "2"}}}) == "a,b\r\n1,2\r\n");

    // ── CSV round trip: parsed rows equal the source data ──
    {
        rp::Table t{"t", "T", {"name", "value", "empty"}, {}};
        for (const auto& h : kHostile) t.rows.push_back({h, h + h, ""});
        t.rows.push_back({"", "\"", ",,"});
        t.rows.push_back({"''", "'", "x\r\ny"});
        std::vector<std::vector<std::string>> rows;
        const auto e = rp::parseCsv(rp::csv(t), &rows);
        TEST("csv round trip: hostile cells parse back exactly", e.empty() && rows == withHeader(t));
        std::vector<std::vector<std::string>> x;
        TEST("csv reader: unterminated quote refused", !rp::parseCsv("\"abc\r\n", &x).empty());
        TEST("csv reader: stray quote refused", !rp::parseCsv("ab\"c\r\n", &x).empty());
        TEST("csv reader: bytes after a closing quote refused", !rp::parseCsv("\"a\"b\r\n", &x).empty());
        TEST("csv reader: a lone CR refused", !rp::parseCsv("a\rb\r\n", &x).empty());
    }

    // ── a synthetic library with hostile item and file names ──
    const fs::path lib = root / "Hostile.KST";
    for (const auto& h : kHostile) put(lib / "STYLE/USER01" / (h + ".STG"), "AIARRANGER-SYNTHETIC-NOT-A-KORG-FILE");
    put(lib / "PCM/RAM.PCM", "AIARRANGER-SYNTHETIC-NOT-A-KORG-FILE");
    const auto L = library::load(lib);
    TEST("library: the hostile synthetic KST loads", L.ok && L.items.size() == kHostile.size() + 1);
    const auto inv = rp::fromInventory(L);
    {
        TEST("inventory: tables from the Library struct (items, banks, files, dependencies, skipped)",
             inv.tables.size() == 5 && inv.tables[0].id == "items" && inv.tables[0].rows.size() == L.items.size() &&
             inv.tables[2].rows.size() == L.files.size() && inv.tables[3].rows.size() == L.deps.size());
        // names come from the struct as they are (the model already shows control bytes as \xNN)
        bool names = inv.tables[0].rows.size() == L.items.size();
        for (size_t i = 0; names && i < L.items.size(); ++i) names = inv.tables[0].rows[i][3] == L.items[i].name && inv.tables[0].rows[i][4] == L.items[i].file;
        bool script = false, formula = false;
        for (const auto& row : inv.tables[0].rows) { script |= row[3] == kHostile[0]; formula |= row[3] == kHostile[3]; }
        TEST("inventory: every item name and file is a cell as the struct has it", names && script && formula);
        TEST("inventory: KST slots unknown are empty, never -1", inv.tables[0].rows[0][2].empty());
        TEST("inventory: CSV round trip of every table", roundTrips(inv));
        const auto h = rp::html(inv);
        TEST("html: <script> from a name is escaped", has(h, "&lt;script src=x&gt;alert(1)&lt;b&gt;") && !has(h, "<script"));
        TEST("html: quotes and & escaped", has(h, "Quote&quot;d, comma") && has(h, "a&amp;b") && has(h, "&#39;apos"));
        TEST("html: only its own tags, no external reference", rp::htmlRefusal(h).empty());
        TEST("html: no URL, link, import or font (src=/href= in text are escaped data; tags checked above)", !has(h, "http:") && !has(h, "https:") &&
             !has(h, "<link") && !has(h, "url(") && !has(h, "@import") && !has(h, "@font-face"));
        TEST("html: a CSP forbids every load", has(h, "default-src 'none'"));
        TEST("html: says not verified on a Pa5X", has(h, "Not verified on a Pa5X"));
        TEST("html check: an unescaped <script> is refused", !rp::htmlRefusal("<p class=\"notice\"><script>x</script></p>").empty());
        TEST("html check: an image or link is refused", !rp::htmlRefusal("<img src=\"http://x/y.png\">").empty() &&
             !rp::htmlRefusal("<link href=\"x.css\">").empty());
        TEST("html check: url() or @import in the style is refused", !rp::htmlRefusal("<style>body{background:url(x)}</style>").empty() &&
             !rp::htmlRefusal("<style>@import 'x';</style>").empty());
        TEST("html check: a raw > outside a tag is refused", !rp::htmlRefusal("<td>a>b</td>").empty());
        std::string text = rp::text(inv);
        TEST("viewer text: lists the tables with their counts", has(text, "== Items (" + std::to_string(L.items.size()) + ") ==") && has(text, "Not verified on a Pa5X"));
        rp::Table big{"b", "Big", {"n"}, {}};
        for (int i = 0; i < 5; ++i) big.rows.push_back({std::to_string(i)});
        TEST("viewer text: capped rows say the export has all", has(rp::text({"k", "t", "s", {}, {big}}, 2), "... 3 more rows"));
    }

    // ── validation findings (every finding cites its source) ──
    {
        const auto v = library::validate(lib);
        const auto r = rp::fromValidation(v);
        bool sourced = !r.tables.empty() && !r.tables[0].rows.empty();
        if (sourced) for (const auto& row : r.tables[0].rows) sourced &= row.size() == 5 && !row[4].empty() && !row[1].empty();
        TEST("validation: one row per finding, each with its rule and source", v.ok && sourced && r.tables[0].rows.size() == v.findings.size());
        TEST("validation: CSV round trip, HTML clean", roundTrips(r) && rp::htmlRefusal(rp::html(r)).empty());
        TEST("validation: a folder that is not there is NOT validated (no table)", rp::fromValidation(library::validate(root / "nope")).tables.empty());
    }

    // ── Library Manager plan from plan.json (the existing format, unchanged) ──
    {
        put(root / "B.KST/STYLE/USER02/Waltz.STG", "w");
        const auto B = library::load(root / "B.KST");
        const std::vector<library::Step> plan = {{library::Op::Copy, 'A', 0, "STYLE/USER09", -1, ""}, {library::Op::Rename, 'B', 0, "", -1, "=New, \"name\""}};
        const auto ev = library::evaluate(L, B, plan);
        const auto json = library::planJson(L, B, plan, ev);
        rp::Report r;
        const auto e = rp::fromJsonText(json, "plan.json", &r);
        TEST("plan: plan.json read as a plan report", e.empty() && r.kind == "plan" && r.tables.size() == 1 && r.tables[0].rows.size() == 2);
        if (r.tables.size() == 1 && r.tables[0].rows.size() == 2) {
            const auto& s0 = r.tables[0].rows[0];
            const auto& s1 = r.tables[0].rows[1];
            TEST("plan: step 1 from the JSON fields (op, item, target bank, first free slot)",
                 s0[0] == "1" && s0[1] == "copy" && s0[2] == "A" && s0[6] == L.items[0].name && s0[9] == "STYLE/USER09" && s0[10].empty());
            TEST("plan: step 2 keeps the new name and its summary", s1[1] == "rename" && s1[11] == "=New, \"name\"" && s1[14] == ev.steps[1].summary);
            TEST("plan: conflicts and warnings as lines", s0[15] == [&] { std::string o; for (size_t i = 0; i < ev.steps[0].conflicts.size(); ++i) o += (i ? "\n" : "") + ev.steps[0].conflicts[i]; return o; }());
        }
        TEST("plan: status and apply facts from the JSON", r.facts.size() >= 2 && r.facts[1].second.rfind("export-only: EXPORT-ONLY", 0) == 0);
        TEST("plan: CSV round trip, HTML clean", roundTrips(r) && rp::htmlRefusal(rp::html(r)).empty());
        rp::Report x;
        TEST("json: another JSON document is refused", has(rp::fromJsonText("{\"format\": \"other\"}", "x", &x), "not an AIArranger"));
        TEST("json: invalid JSON is refused", has(rp::fromJsonText("{\"format\": ", "x", &x), "not valid JSON"));
    }

    // ── the report job (E6) and the export report it leaves, read back as a report ──
    {
        const auto shared = std::make_shared<const rp::Report>(inv);
        const auto before = library::load(lib);
        const auto st = run(reportExport(shared, root / "Inventory Report"));
        const fs::path out = root / "Inventory Report";
        TEST("job: done, published, report beside it, nothing staged left", st.phase == Phase::Done && fs::is_regular_file(out / "report.html") &&
             fs::is_regular_file(out / "items.csv") && fs::is_regular_file(out / "PACKAGE.sha256") &&
             fs::is_regular_file(root / "Inventory Report.export.json") && !fs::exists(root / "Inventory Report.INCOMPLETE"));
        TEST("job: L1 passed by the independent check, L2 not applicable, never L3", st.level.l1.state == Check::Passed &&
             has(st.verifyText, "every CSV parses back") && st.level.l2.state == Check::NotRun && st.level.l3.state == Check::NotRun);
        std::vector<std::vector<std::string>> rows;
        TEST("job: the written items.csv parses back to the inventory items", rp::parseCsv(slurp(out / "items.csv"), &rows).empty() && rows == withHeader(inv.tables[0]));
        TEST("job: the written report.html has no external reference", rp::htmlRefusal(slurp(out / "report.html")).empty());
        const auto after = library::load(lib);
        bool same = before.files.size() == after.files.size();
        for (const auto& [rel, f] : before.files) same &= after.files.count(rel) && after.files.at(rel).sha256 == f.sha256;
        TEST("job: the source library is unchanged (read-only)", same);

        rp::Report er;
        const auto e = rp::loadJsonReport(root / "Inventory Report.export.json", &er);
        TEST("export report: NAME.export.json read as a report", e.empty() && er.kind == "export" && er.tables.size() == 3);
        if (er.tables.size() == 3) {
            bool filesMatch = er.tables[0].rows.size() == st.files.size();
            for (size_t i = 0; filesMatch && i < st.files.size(); ++i)
                filesMatch = er.tables[0].rows[i] == std::vector<std::string>{st.files[i].rel, std::to_string(st.files[i].bytes), st.files[i].sha256};
            TEST("export report: files table equals the job's files (path, bytes, sha256)", filesMatch);
            TEST("export report: result and L1 from the JSON", has(rp::text(er), "Result: done") && has(rp::text(er), "Verification L1: passed"));
            TEST("export report: CSV round trip, HTML clean", roundTrips(er) && rp::htmlRefusal(rp::html(er)).empty());
        }

        TEST("job: an existing output is refused", run(reportExport(shared, out)).phase == Phase::Failed);
        const auto inside = run(reportExport(shared, lib / "reports"));
        TEST("job: output inside the source library refused (D18), nothing created", inside.phase == Phase::Failed &&
             inside.stoppedAt == Phase::Destination && !fs::exists(lib / "reports") && !fs::exists(lib / "reports.INCOMPLETE"));
        TEST("job: no report open is refused", run(reportExport(nullptr, root / "None")).phase == Phase::Failed && !fs::exists(root / "None"));

        // the independent check catches a CSV that does not match its table, and a foreign tag in the HTML
        fs::create_directories(root / "check");
        for (const auto& [rel, b] : rp::files(inv)) put(root / "check" / rel, std::string(b.begin(), b.end()));
        TEST("check: an untouched folder passes", rp::checkFolder(root / "check", inv).empty());
        put(root / "check/items.csv", slurp(root / "check/items.csv") + "x,y\r\n");
        TEST("check: a CSV that does not parse back to its table fails", has(rp::checkFolder(root / "check", inv), "items.csv"));
        put(root / "check/items.csv", rp::csv(inv.tables[0]));
        put(root / "check/report.html", slurp(root / "check/report.html") + "<img src=\"https://example.invalid/x.png\">");
        TEST("check: an image tag in the HTML fails", has(rp::checkFolder(root / "check", inv), "report.html"));
    }

    // ── the read-only loader: bounded, regular files only ──
    {
        rp::Report x;
        put(root / "big.export.json", std::string(rp::kMaxJsonBytes + 1, ' '));
        TEST("loader: a file over 10 MiB is refused", has(rp::loadJsonReport(root / "big.export.json", &x), "larger than"));
        fs::create_symlink(root / "Inventory Report.export.json", root / "link.export.json");
        TEST("loader: a symlink is not followed", has(rp::loadJsonReport(root / "link.export.json", &x), "not a regular file"));
        TEST("loader: a missing file is refused", !rp::loadJsonReport(root / "nope.json", &x).empty());
    }

    fs::remove_all(root);
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
