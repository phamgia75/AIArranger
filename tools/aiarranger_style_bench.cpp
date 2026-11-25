// aiarranger-style-bench: Offline Musical Quality Benchmark CLI (Track B Q1).
//
// Usage:
//   aiarranger-style-bench --input <style.sty> [--target-kst <Target.KST>] [--ppqn 384]
//                          [--break-policy <yamaha1bar|withhold>] [--json <out.json>] [--md <out.md>]
//   aiarranger-style-bench --input-dir <dir> [--ppqn 384]
//                          [--break-policy <yamaha1bar|withhold>] [--json <out.json>] [--md <out.md>]
//
// Epistemic boundary:
//   100% offline, deterministic, software-verified only (hardwareVerified: false, L3 = NotRun).
//   Never claims Pa5X hardware compatibility.

#include "exporters/korg/style_quality_bench.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ai_arranger::exporters::korg::bench;

namespace {

int usage() {
    std::cerr << "usage: aiarranger-style-bench (--input <file.sty> | --input-dir <dir>)\n"
                 "                              [--target-kst <Target.KST>] [--ppqn <N>]\n"
                 "                              [--break-policy <yamaha1bar|withhold>]\n"
                 "                              [--json <out.json>] [--md <out.md>]\n";
    return 1;
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        std::cerr << "cannot write " << path << "\n";
        return false;
    }
    return true;
}

std::string batchToJson(const std::vector<BenchResult>& results) {
    using detail::fmtNum;
    std::ostringstream o;
    double sumM1 = 0, sumM2 = 0, sumM3 = 0, sumM4 = 0, sumM5 = 0, sumM6 = 0, sumComp = 0;
    size_t okCount = 0;
    bool allValid = !results.empty();
    for (const auto& r : results) {
        if (!r.ok || !r.structuralValid) allValid = false;
        if (r.ok) {
            ++okCount;
            sumM1 += r.m1.score; sumM2 += r.m2.score; sumM3 += r.m3.score;
            sumM4 += r.m4.score; sumM5 += r.m5.score; sumM6 += r.m6.score;
            sumComp += r.compositeScore;
        }
    }
    const double denom = okCount ? double(okCount) : 1.0;
    o << "{\n";
    o << "  \"schema\": \"" << kSchemaId << "\",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"NotRun\",\n";
    o << "  \"label\": \"" << (allValid ? kValidLabel : kInvalidLabel) << "\",\n";
    o << "  \"weights\": { \"M1\": 0.25, \"M2\": 0.20, \"M3\": 0.15, \"M4\": 0.15, \"M5\": 0.12, \"M6\": 0.13 },\n";
    o << "  \"fixtureCount\": " << results.size() << ",\n";
    o << "  \"allStructuralValid\": " << (allValid ? "true" : "false") << ",\n";
    o << "  \"meanScores\": {\n";
    o << "    \"composite\": " << fmtNum(sumComp / denom, 2) << ",\n";
    o << "    \"M1\": " << fmtNum(sumM1 / denom, 2) << ",\n";
    o << "    \"M2\": " << fmtNum(sumM2 / denom, 2) << ",\n";
    o << "    \"M3\": " << fmtNum(sumM3 / denom, 2) << ",\n";
    o << "    \"M4\": " << fmtNum(sumM4 / denom, 2) << ",\n";
    o << "    \"M5\": " << fmtNum(sumM5 / denom, 2) << ",\n";
    o << "    \"M6\": " << fmtNum(sumM6 / denom, 2) << "\n";
    o << "  },\n";
    o << "  \"fixtures\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        std::string item = toJson(results[i]);
        // Indent by 4 spaces
        std::istringstream iss(item);
        std::string line;
        bool firstLine = true;
        while (std::getline(iss, line)) {
            if (!firstLine) o << "\n";
            firstLine = false;
            o << "    " << line;
        }
        o << (i + 1 < results.size() ? ",\n" : "\n");
    }
    o << "  ]\n";
    o << "}\n";
    return o.str();
}

} // namespace

int main(int argc, char** argv) {
    std::string inputPath, inputDir, jsonOut, mdOut;
    BenchOptions opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (i + 1 >= argc) return usage();
        const std::string v = argv[++i];
        if (a == "--input") inputPath = v;
        else if (a == "--input-dir") inputDir = v;
        else if (a == "--target-kst") opt.targetKstPath = v;
        else if (a == "--json") jsonOut = v;
        else if (a == "--md") mdOut = v;
        else if (a == "--break-policy") {
            if (v == "yamaha1bar") opt.breakYamaha1BarCap = true;
            else if (v == "withhold") opt.breakYamaha1BarCap = false;
            else return usage();
        } else if (a == "--ppqn") {
            try {
                size_t used = 0;
                const long n = std::stol(v, &used);
                if (used != v.size() || n < 0 || n > 0x7FFF) return usage();
                opt.targetPpqn = static_cast<uint16_t>(n);
            } catch (...) {
                return usage();
            }
        } else {
            return usage();
        }
    }

    if (inputPath.empty() == inputDir.empty()) return usage();

    if (!inputPath.empty()) {
        const BenchResult r = evaluateStyleFile(inputPath, opt);
        if (!r.ok) {
            std::cerr << "benchmark failed: " << r.error << "\n";
            return 2;
        }
        const std::string js = toJson(r);
        const std::string md = toMarkdown({r});
        if (!jsonOut.empty() && !writeText(jsonOut, js)) return 1;
        if (!mdOut.empty() && !writeText(mdOut, md)) return 1;
        if (jsonOut.empty() && mdOut.empty()) std::cout << md;
        else std::cout << md;
        return r.structuralValid ? 0 : 2;
    }

    // Batch directory mode: deterministic alphabetical filename order
    std::error_code ec;
    if (!fs::is_directory(inputDir, ec)) {
        std::cerr << "not a directory: " << inputDir << "\n";
        return 1;
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(inputDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == ".sty") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return a.filename().string() < b.filename().string();
    });
    if (files.empty()) {
        std::cerr << "no .sty files found in " << inputDir << "\n";
        return 1;
    }

    std::vector<BenchResult> results;
    results.reserve(files.size());
    bool allOk = true;
    for (const auto& p : files) {
        BenchResult r = evaluateStyleFile(p.string(), opt);
        if (!r.ok || !r.structuralValid) allOk = false;
        results.push_back(std::move(r));
    }

    const std::string js = batchToJson(results);
    const std::string md = toMarkdown(results);
    if (!jsonOut.empty() && !writeText(jsonOut, js)) return 1;
    if (!mdOut.empty() && !writeText(mdOut, md)) return 1;
    std::cout << md;
    return allOk ? 0 : 2;
}
