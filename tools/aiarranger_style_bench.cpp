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
    const CorpusSummary cs = computeCorpusSummary(results);
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << kSchemaId << "\",\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"NotRun\",\n";
    o << "  \"label\": \"" << (cs.allStructuralValid ? kValidLabel : kInvalidLabel) << "\",\n";
    o << "  \"weights\": { \"M1\": 0.25, \"M2\": 0.20, \"M3\": 0.15, \"M4\": 0.15, \"M5\": 0.12, \"M6\": 0.13 },\n";
    o << "  \"fixtureCount\": " << cs.fixtureCount << ",\n";
    o << "  \"allStructuralValid\": " << (cs.allStructuralValid ? "true" : "false") << ",\n";
    o << "  \"meanScores\": {\n";
    o << "    \"composite\": " << fmtNum(cs.meanComposite, 2) << ",\n";
    o << "    \"M1\": " << fmtNum(cs.meanDimensions[0], 2) << ",\n";
    o << "    \"M2\": " << fmtNum(cs.meanDimensions[1], 2) << ",\n";
    o << "    \"M3\": " << fmtNum(cs.meanDimensions[2], 2) << ",\n";
    o << "    \"M4\": " << fmtNum(cs.meanDimensions[3], 2) << ",\n";
    o << "    \"M5\": " << fmtNum(cs.meanDimensions[4], 2) << ",\n";
    o << "    \"M6\": " << fmtNum(cs.meanDimensions[5], 2) << "\n";
    o << "  },\n";
    o << "  \"corpusSummary\": {\n";
    o << "    \"fixtureCount\": " << cs.fixtureCount << ",\n";
    o << "    \"gradeCounts\": { \"pass\": " << cs.passCount
      << ", \"warn\": " << cs.warnCount
      << ", \"fail\": " << cs.failCount << " },\n";
    o << "    \"meanComposite\": " << fmtNum(cs.meanComposite, 2) << ",\n";
    o << "    \"minComposite\": " << fmtNum(cs.minComposite, 2) << ",\n";
    o << "    \"minDimensionScores\": { \"M1\": " << fmtNum(cs.minDimensionScores[0], 2)
      << ", \"M2\": " << fmtNum(cs.minDimensionScores[1], 2)
      << ", \"M3\": " << fmtNum(cs.minDimensionScores[2], 2)
      << ", \"M4\": " << fmtNum(cs.minDimensionScores[3], 2)
      << ", \"M5\": " << fmtNum(cs.minDimensionScores[4], 2)
      << ", \"M6\": " << fmtNum(cs.minDimensionScores[5], 2) << " },\n";
    o << "    \"activeDimensionMeans\": { \"M1\": " << fmtNum(cs.activeDimensionMeans[0], 2)
      << ", \"M2\": " << fmtNum(cs.activeDimensionMeans[1], 2)
      << ", \"M3\": " << fmtNum(cs.activeDimensionMeans[2], 2)
      << ", \"M4\": " << fmtNum(cs.activeDimensionMeans[3], 2)
      << ", \"M5\": " << fmtNum(cs.activeDimensionMeans[4], 2)
      << ", \"M6\": " << fmtNum(cs.activeDimensionMeans[5], 2) << " },\n";
    o << "    \"activeDimensionCounts\": { \"M1\": " << cs.activeDimensionCounts[0]
      << ", \"M2\": " << cs.activeDimensionCounts[1]
      << ", \"M3\": " << cs.activeDimensionCounts[2]
      << ", \"M4\": " << cs.activeDimensionCounts[3]
      << ", \"M5\": " << cs.activeDimensionCounts[4]
      << ", \"M6\": " << cs.activeDimensionCounts[5] << " },\n";
    o << "    \"activeComposite\": " << fmtNum(cs.activeComposite, 2) << ",\n";
    o << "    \"hardInvariantViolations\": " << cs.hardInvariantViolations << ",\n";
    o << "    \"corpusGrade\": \"" << cs.corpusGrade << "\"\n";
    o << "  },\n";
    o << "  \"fixtures\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        std::string item = toJson(results[i]);
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
        std::cout << md;
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
