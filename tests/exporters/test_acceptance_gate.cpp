// Unit tests for Phase 3 P3-M7 Proposed Production Acceptance Gate (acceptance_gate.h).
// Covers all 5 grades (A, B, C, D, U), strict offline-only epistemic boundary,
// strict fail-closed JSON listening record ingestion, JSON control-char escaping,
// and NaN/Inf/bounds safety.

#include "exporters/korg/acceptance_gate.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace ai_arranger::exporters::korg;
using namespace ai_arranger::exporters::korg::quality;

static int passes = 0;
static int failures = 0;

#define TEST(name, expr)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "  FAIL: %s\n", (name));                      \
            ++failures;                                                        \
        } else {                                                               \
            std::printf("  PASS: %s\n", (name));                               \
            ++passes;                                                          \
        }                                                                      \
    } while (0)

static OfflineAcceptanceEvidence makeCleanOffline() {
    OfflineAcceptanceEvidence ev{};
    ev.style_name = "SYNTH_POP_SFF2.sty";
    ev.has_fatal_conversion_error = false;
    ev.hard_invariant_violations = 0;
    ev.unresolved_critical_sounds = 0;
    ev.unresolved_total_sounds = 0;
    ev.has_analyzer_summary = true;
    ev.overall_musical_score = 94.25;
    return ev;
}

static HumanListeningRecord makeVerifiedListening(int rating, uint32_t crit = 0, uint32_t minor = 0) {
    HumanListeningRecord rec{};
    rec.present = true;
    rec.evaluator_id = "musician-01";
    rec.overall_rating_1_to_5 = rating;
    rec.critical_defect_count = crit;
    rec.minor_defect_count = minor;
    rec.hardware_verified = true;
    rec.hardware_model = "Korg Pa5X";
    rec.reference_type = "Genos_Direct_AB";
    rec.required_sections_verified = true;
    rec.required_chords_verified = true;
    return rec;
}

int main() {
    std::printf("Test: Phase 3 P3-M7 Proposed Acceptance Gate (Grade A/B/C/D/U)\n");

    // 1. Offline PASS alone MUST ALWAYS yield Grade U (NEVER Grade A or B)
    {
        const auto ev = makeCleanOffline();
        const auto rep = evaluateAcceptanceGate(ev);
        TEST("offline PASS alone yields Grade U (Unverified)",
             rep.grade == AcceptanceGrade::U &&
             rep.grade_code == "U" &&
             rep.grade_label == "Unverified" &&
             rep.criteria_status == "PROPOSED_ACCEPTANCE_CRITERIA" &&
             !rep.hardware_verified &&
             rep.l3_status == "NOT_TESTED" &&
             rep.independent_from_frozen_q1);

        // Even with a 5/5 listening record, hardware_verified=false MUST stay Grade U!
        auto unhw = makeVerifiedListening(5, 0, 0);
        unhw.hardware_verified = false;
        const auto rep2 = evaluateAcceptanceGate(ev, unhw);
        TEST("listening 5/5 with hardwareVerified=false still yields Grade U (never Grade A)",
             rep2.grade == AcceptanceGrade::U && !rep2.hardware_verified && rep2.l3_status == "NOT_TESTED");
    }

    // 2. Grade A (Performance Ready): requires hardware_verified + rating >= 4 + 0 defects + 0 unresolved
    {
        const auto ev = makeCleanOffline();
        const auto rep4 = evaluateAcceptanceGate(ev, makeVerifiedListening(4, 0, 0));
        TEST("hardware-verified rating 4/5 with 0 defects & 0 unresolved yields Grade A",
             rep4.grade == AcceptanceGrade::A &&
             rep4.grade_code == "A" &&
             rep4.grade_label == "Performance Ready" &&
             rep4.hardware_verified &&
             rep4.l3_status == "VERIFIED");

        const auto rep5 = evaluateAcceptanceGate(ev, makeVerifiedListening(5, 0, 0));
        TEST("hardware-verified rating 5/5 with 0 defects yields Grade A",
             rep5.grade == AcceptanceGrade::A && rep5.hardware_verified);
    }

    // 3. Grade B (Usable With Minor Refinement)
    {
        const auto ev = makeCleanOffline();
        const auto rep3 = evaluateAcceptanceGate(ev, makeVerifiedListening(3, 0, 0));
        TEST("hardware-verified rating 3/5 (no major correction) yields Grade B",
             rep3.grade == AcceptanceGrade::B && rep3.grade_code == "B");

        const auto repMinor = evaluateAcceptanceGate(ev, makeVerifiedListening(4, 0, 2));
        TEST("hardware-verified rating 4/5 with minor_defect_count=2 yields Grade B",
             repMinor.grade == AcceptanceGrade::B);

        auto evNonCrit = makeCleanOffline();
        evNonCrit.unresolved_total_sounds = 1;
        evNonCrit.unresolved_critical_sounds = 0;
        const auto repNonCrit = evaluateAcceptanceGate(evNonCrit, makeVerifiedListening(5, 0, 0));
        TEST("hardware-verified rating 5/5 with 1 non-critical unresolved sound yields Grade B",
             repNonCrit.grade == AcceptanceGrade::B);

        auto missingChords = makeVerifiedListening(5, 0, 0);
        missingChords.required_chords_verified = false;
        const auto repChords = evaluateAcceptanceGate(ev, missingChords);
        TEST("hardware-verified rating 5/5 with required_chords_verified=false yields Grade B",
             repChords.grade == AcceptanceGrade::B);
    }

    // 4. Grade C (Manual Correction Required)
    {
        auto evUnresCrit = makeCleanOffline();
        evUnresCrit.unresolved_critical_sounds = 1;
        evUnresCrit.unresolved_total_sounds = 2;
        const auto repUnresOffline = evaluateAcceptanceGate(evUnresCrit);
        TEST("unresolved critical sound (offline) yields Grade C",
             repUnresOffline.grade == AcceptanceGrade::C && !repUnresOffline.hardware_verified);

        const auto repUnresHw = evaluateAcceptanceGate(evUnresCrit, makeVerifiedListening(5, 0, 0));
        TEST("unresolved critical sound blocks Grade A even with 5/5 listening -> Grade C",
             repUnresHw.grade == AcceptanceGrade::C);

        const auto ev = makeCleanOffline();
        const auto repR2 = evaluateAcceptanceGate(ev, makeVerifiedListening(2, 0, 0));
        TEST("listening rating 2/5 yields Grade C",
             repR2.grade == AcceptanceGrade::C);

        auto r3Major = makeVerifiedListening(3, 0, 1);
        r3Major.major_correction_needed = true;
        const auto repR3Major = evaluateAcceptanceGate(ev, r3Major);
        TEST("listening rating 3/5 with majorCorrectionNeeded=true yields Grade C",
             repR3Major.grade == AcceptanceGrade::C);
    }

    // 5. Grade D (Rejected)
    {
        auto evFatal = makeCleanOffline();
        evFatal.has_fatal_conversion_error = true;
        evFatal.fatal_error_message = "corrupt SMF header";
        TEST("fatal conversion error yields Grade D",
             evaluateAcceptanceGate(evFatal).grade == AcceptanceGrade::D);

        auto evHard = makeCleanOffline();
        evHard.hard_invariant_violations = 1;
        TEST("hard_invariant_violations > 0 yields Grade D (even with 5/5 HW listening)",
             evaluateAcceptanceGate(evHard, makeVerifiedListening(5, 0, 0)).grade == AcceptanceGrade::D);

        const auto ev = makeCleanOffline();
        TEST("listening rating 1/5 yields Grade D",
             evaluateAcceptanceGate(ev, makeVerifiedListening(1, 0, 0)).grade == AcceptanceGrade::D);

        TEST("listening critical_defect_count > 0 yields Grade D even if rating == 4",
             evaluateAcceptanceGate(ev, makeVerifiedListening(4, 1, 0)).grade == AcceptanceGrade::D);
    }

    // 6. Strict HumanListeningRecord JSON parser (valid + fail-closed malformed/out-of-range)
    {
        const std::string validJson = R"({
            "schema": "aiarranger-human-listening-record/1",
            "evaluatorId": "musician-01",
            "overallRating": 4,
            "perDimensionRatings": [5, 4, 4, 4, 5, null, 4, 4, 5, 4, 4, 5],
            "criticalDefectCount": 0,
            "minorDefectCount": 0,
            "hardwareVerified": true,
            "hardwareModel": "Korg Pa5X 76",
            "referenceType": "Genos_Direct_AB",
            "notes": "Clean guitar strums & \"tight\" bass\\drum groove\nline2"
        })";
        const auto p = parseHumanListeningRecordJson(validJson);
        TEST("valid listening JSON parses cleanly with all fields",
             p.ok && p.record.present && p.record.evaluator_id == "musician-01" &&
             p.record.overall_rating_1_to_5 == 4 && p.record.hardware_verified &&
             p.record.hardware_model == "Korg Pa5X 76" &&
             p.record.per_dimension_present[0] && p.record.per_dimension_ratings[0] == 5 &&
             !p.record.per_dimension_present[5] && p.record.per_dimension_ratings[5] == 0);

        const auto pObjDims = parseHumanListeningRecordJson(R"({
            "evaluator_id": "musician-02",
            "overall_rating_1_to_5": 3,
            "per_dimension_ratings": {"D01": 4, "D12_mix_balance_effects": 3},
            "hardware_verified": false
        })");
        TEST("snake_case aliases & object per_dimension_ratings parse cleanly",
             pObjDims.ok && pObjDims.record.overall_rating_1_to_5 == 3 &&
             pObjDims.record.per_dimension_present[0] && pObjDims.record.per_dimension_ratings[0] == 4 &&
             pObjDims.record.per_dimension_present[11] && pObjDims.record.per_dimension_ratings[11] == 3);

        TEST("rejects empty JSON", !parseHumanListeningRecordJson("").ok);
        TEST("rejects empty object {}", !parseHumanListeningRecordJson("{}").ok);
        TEST("rejects missing overallRating",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1"})").ok);
        TEST("rejects out-of-range overallRating 0",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":0})").ok);
        TEST("rejects out-of-range overallRating 6",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":6})").ok);
        TEST("rejects floating-point overallRating 4.5",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4.5})").ok);
        TEST("rejects negative criticalDefectCount",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4,"criticalDefectCount":-1})").ok);
        TEST("rejects hardwareVerified=true without hardwareModel",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4,"hardwareVerified":true})").ok);
        TEST("rejects wrong perDimensionRatings array length (11)",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4,"perDimensionRatings":[4,4,4,4,4,4,4,4,4,4,4]})").ok);
        TEST("rejects out-of-range perDimensionRatings value (6)",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4,"perDimensionRatings":[6,4,4,4,4,4,4,4,4,4,4,4]})").ok);
        TEST("rejects unknown field typo (fail-closed)",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4,"overalRating":5})").ok);
        TEST("rejects duplicate key",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4,"overall_rating_1_to_5":5})").ok);
        TEST("rejects trailing garbage",
             !parseHumanListeningRecordJson(R"({"evaluatorId":"m1","overallRating":4} trailing)").ok);
        TEST("rejects wrong schema",
             !parseHumanListeningRecordJson(R"({"schema":"wrong/99","evaluatorId":"m1","overallRating":4})").ok);

        const std::string tmpl = formatListeningRecordTemplateJson("TEST.sty", "Candidate B", "deadbeef");
        TEST("generated LISTENING_RECORD_TEMPLATE.json is rejected until templatePlaceholder=false",
             !parseHumanListeningRecordJson(tmpl).ok);
    }

    // 7. JSON control-char escaping, Markdown formatting, and NaN/Inf/empty bounds safety
    {
        OfflineAcceptanceEvidence ev{};
        ev.style_name = "Bad\"Name\\With\tCtrl\x01\x1f.sty";
        ev.has_analyzer_summary = true;
        ev.overall_musical_score = std::numeric_limits<double>::quiet_NaN();
        ev.dimension_scores[0] = std::numeric_limits<double>::infinity();
        ev.dimension_scores[1] = -42.0;

        HumanListeningRecord rec = makeVerifiedListening(4, 0, 0);
        rec.notes = std::string("Quote:\" Backslash:\\ Tab:\t Ctrl:") + static_cast<char>(0x07) + " NL:\nEnd";

        const auto rep = evaluateAcceptanceGate(ev, rec);
        const std::string js = formatAcceptanceReportJson(rep);
        const std::string md = formatAcceptanceReportMd(rep);

        TEST("JSON escaping handles quotes, backslashes, tabs, newlines, and control chars < 0x20",
             js.find("\"criteriaStatus\": \"PROPOSED_ACCEPTANCE_CRITERIA\"") != std::string::npos &&
             js.find("\\\"Name\\\\With\\tCtrl\\u0001\\u001f.sty") != std::string::npos &&
             js.find("\\u0007") != std::string::npos &&
             js.find("NaN") == std::string::npos &&
             js.find("inf") == std::string::npos &&
             js.find("INF") == std::string::npos &&
             js.find("\"overallMusicalScore\": 0.00") != std::string::npos);

        TEST("Markdown report renders PROPOSED_ACCEPTANCE_CRITERIA and grade safely",
             md.find("PROPOSED_ACCEPTANCE_CRITERIA") != std::string::npos &&
             md.find("Grade A (Performance Ready)") != std::string::npos &&
             md.find("hardwareVerified: true") != std::string::npos);
    }

    // 8. buildOfflineAcceptanceEvidence classifies critical (drum/bass/acc1) vs non-critical (acc2..5/perc)
    {
        ConversionManifest m{};
        m.ok = true;
        TrackSoundResolution tDrum{}; tDrum.track_key = "drum"; tDrum.status = "EXACT";
        TrackSoundResolution tAcc1{}; tAcc1.track_key = "acc1"; tAcc1.status = "UNRESOLVED_LOW_CONFIDENCE";
        TrackSoundResolution tAcc4{}; tAcc4.track_key = "acc4"; tAcc4.status = "UNSUPPORTED";
        m.track_resolutions = {tDrum, tAcc1, tAcc4};

        const auto ev = buildOfflineAcceptanceEvidence("EXP15_LIKE.sty", m, nullptr, false, 0);
        TEST("buildOfflineAcceptanceEvidence counts acc1 as critical (1) and total unresolved (2)",
             ev.unresolved_critical_sounds == 1u &&
             ev.unresolved_total_sounds == 2u &&
             evaluateAcceptanceGate(ev).grade == AcceptanceGrade::C);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
