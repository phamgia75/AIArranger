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
    using detail::jsonEscape;
    const CorpusSummary cs = computeCorpusSummary(results);
    auto optAgg = [](uint32_t cnt, double v) -> std::string {
        return cnt > 0 ? fmtNum(v, 2) : "null";
    };
    static constexpr const char* kKeys[6] = {"M1", "M2", "M3", "M4", "M5", "M6"};
    uint32_t totalNaExclusions = 0;
    for (uint32_t c : cs.naExcludedCounts) totalNaExclusions += c;

    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"" << kSchemaId << "\",\n";
    o << "  \"statusTaxonomy\": [\"PASS\", \"FAIL\", \"UNSUPPORTED\", \"N/A\", \"NOT_TESTED\"],\n";
    o << "  \"hardwareVerified\": false,\n";
    o << "  \"l3Status\": \"" << benchStatusToString(cs.l3Status) << "\",\n";
    o << "  \"label\": \"" << (cs.allStructuralValid ? kValidLabel : kInvalidLabel) << "\",\n";
    o << "  \"weights\": { \"M1\": 0.25, \"M2\": 0.20, \"M3\": 0.15, \"M4\": 0.15, \"M5\": 0.12, \"M6\": 0.13 },\n";
    o << "  \"fixtureCount\": " << cs.fixtureCount << ",\n";
    o << "  \"allStructuralValid\": " << (cs.allStructuralValid ? "true" : "false") << ",\n";
    o << "  \"meanScores\": {\n";
    o << "    \"composite\": " << fmtNum(cs.meanComposite, 2) << ",\n";
    for (size_t i = 0; i < 6; ++i) {
        o << "    \"" << kKeys[i] << "\": " << optAgg(cs.evaluatedDimensionCounts[i], cs.meanDimensions[i])
          << (i < 5 ? ",\n" : "\n");
    }
    o << "  },\n";
    o << "  \"corpusSummary\": {\n";
    o << "    \"fixtureCount\": " << cs.fixtureCount << ",\n";
    o << "    \"statusTaxonomy\": [\"PASS\", \"FAIL\", \"UNSUPPORTED\", \"N/A\", \"NOT_TESTED\"],\n";
    o << "    \"l3Status\": \"" << benchStatusToString(cs.l3Status) << "\",\n";
    o << "    \"targetKstSummary\": { \"pass\": " << cs.targetKstPassCount
      << ", \"fail\": " << cs.targetKstFailCount
      << ", \"notTested\": " << cs.targetKstNotTestedCount << " },\n";
    o << "    \"gradeCounts\": { \"pass\": " << cs.passCount
      << ", \"warn\": " << cs.warnCount
      << ", \"fail\": " << cs.failCount << " },\n";
    o << "    \"coverage\": {\n";
    o << "      \"fixtureCount\": " << cs.fixtureCount << ",\n";
    for (size_t i = 0; i < 6; ++i) {
        o << "      \"" << kKeys[i] << "\": { \"evaluated\": " << cs.evaluatedDimensionCounts[i]
          << ", \"na\": " << cs.naExcludedCounts[i]
          << ", \"activeTorture\": " << cs.activeDimensionCounts[i]
          << ", \"pass\": " << cs.dimPassCounts[i]
          << ", \"fail\": " << cs.dimFailCounts[i] << " }"
          << (i < 5 ? ",\n" : "\n");
    }
    o << "    },\n";
    o << "    \"naExclusions\": { \"total\": " << totalNaExclusions
      << ", \"M1\": " << cs.naExcludedCounts[0]
      << ", \"M2\": " << cs.naExcludedCounts[1]
      << ", \"M3\": " << cs.naExcludedCounts[2]
      << ", \"M4\": " << cs.naExcludedCounts[3]
      << ", \"M5\": " << cs.naExcludedCounts[4]
      << ", \"M6\": " << cs.naExcludedCounts[5] << " },\n";
    o << "    \"meanComposite\": " << fmtNum(cs.meanComposite, 2) << ",\n";
    o << "    \"minComposite\": " << fmtNum(cs.minComposite, 2) << ",\n";
    o << "    \"evaluatedDimensionMeans\": { \"M1\": " << optAgg(cs.evaluatedDimensionCounts[0], cs.meanDimensions[0])
      << ", \"M2\": " << optAgg(cs.evaluatedDimensionCounts[1], cs.meanDimensions[1])
      << ", \"M3\": " << optAgg(cs.evaluatedDimensionCounts[2], cs.meanDimensions[2])
      << ", \"M4\": " << optAgg(cs.evaluatedDimensionCounts[3], cs.meanDimensions[3])
      << ", \"M5\": " << optAgg(cs.evaluatedDimensionCounts[4], cs.meanDimensions[4])
      << ", \"M6\": " << optAgg(cs.evaluatedDimensionCounts[5], cs.meanDimensions[5]) << " },\n";
    o << "    \"minDimensionScores\": { \"M1\": " << optAgg(cs.evaluatedDimensionCounts[0], cs.minDimensionScores[0])
      << ", \"M2\": " << optAgg(cs.evaluatedDimensionCounts[1], cs.minDimensionScores[1])
      << ", \"M3\": " << optAgg(cs.evaluatedDimensionCounts[2], cs.minDimensionScores[2])
      << ", \"M4\": " << optAgg(cs.evaluatedDimensionCounts[3], cs.minDimensionScores[3])
      << ", \"M5\": " << optAgg(cs.evaluatedDimensionCounts[4], cs.minDimensionScores[4])
      << ", \"M6\": " << optAgg(cs.evaluatedDimensionCounts[5], cs.minDimensionScores[5]) << " },\n";
    o << "    \"activeDimensionMeans\": { \"M1\": " << optAgg(cs.activeDimensionCounts[0], cs.activeDimensionMeans[0])
      << ", \"M2\": " << optAgg(cs.activeDimensionCounts[1], cs.activeDimensionMeans[1])
      << ", \"M3\": " << optAgg(cs.activeDimensionCounts[2], cs.activeDimensionMeans[2])
      << ", \"M4\": " << optAgg(cs.activeDimensionCounts[3], cs.activeDimensionMeans[3])
      << ", \"M5\": " << optAgg(cs.activeDimensionCounts[4], cs.activeDimensionMeans[4])
      << ", \"M6\": " << optAgg(cs.activeDimensionCounts[5], cs.activeDimensionMeans[5]) << " },\n";
    o << "    \"activeDimensionCounts\": { \"M1\": " << cs.activeDimensionCounts[0]
      << ", \"M2\": " << cs.activeDimensionCounts[1]
      << ", \"M3\": " << cs.activeDimensionCounts[2]
      << ", \"M4\": " << cs.activeDimensionCounts[3]
      << ", \"M5\": " << cs.activeDimensionCounts[4]
      << ", \"M6\": " << cs.activeDimensionCounts[5] << " },\n";
    o << "    \"activeComposite\": " << fmtNum(cs.activeComposite, 2) << ",\n";
    o << "    \"hardInvariantViolations\": " << cs.hardInvariantViolations << ",\n";
    o << "    \"unsupportedFeatures\": {\n";
    o << "      \"totalCount\": " << cs.unsupportedTotalOccurrences << ",\n";
    o << "      \"fixturesWithUnsupported\": " << cs.fixturesWithUnsupported << ",\n";
    o << "      \"uniqueTags\": [";
    {
        size_t idx = 0;
        for (const auto& [feat, cnt] : cs.unsupportedByFeature) {
            (void)cnt;
            if (idx++ > 0) o << ", ";
            o << "\"" << jsonEscape(feat) << "\"";
        }
    }
    o << "],\n";
    o << "      \"byFeature\": {";
    if (!cs.unsupportedByFeature.empty()) {
        o << "\n";
        size_t idx = 0;
        for (const auto& [feat, cnt] : cs.unsupportedByFeature) {
            o << "        \"" << jsonEscape(feat) << "\": " << cnt
              << (++idx < cs.unsupportedByFeature.size() ? ",\n" : "\n      ");
        }
    }
    o << "}\n";
    o << "    },\n";
    o << "    \"notTestedItems\": [";
    for (size_t i = 0; i < cs.notTestedItems.size(); ++i) {
        if (i > 0) o << ", ";
        o << "\"" << jsonEscape(cs.notTestedItems[i]) << "\"";
    }
    o << "],\n";
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
