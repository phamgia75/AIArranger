#ifndef AI_ARRANGER_SFF1_MAPPER_H
#define AI_ARRANGER_SFF1_MAPPER_H

#include "importers/sff1/sff1_types.h"
#include "engine/uasf/types.h"

namespace ai_arranger::importers::sff1 {

struct SffToUasfResult {
    bool success;
    std::string error;
    uasf::StyleDefinition style;
    std::vector<std::string> warnings;
    std::vector<std::string> unmapped_features;
    // CASM-derived section structure (roles + channels, no MIDI events):
    // metadata view of ParseResult::casm_sections. The playable sections (with
    // events) are in `style.sections`, split at the SMF section markers.
    std::vector<uasf::SectionDefinition> casm_sections;
};

class Sff1ToUasfMapper {
public:
    SffToUasfResult map(const ParseResult& parseResult) noexcept;

    // Infer a UASF track role from a CASM track config. Name is the
    // primary signal; the NTT bass-note flag and source channel refine it.
    // Exposed for testing.
    uasf::TrackRole mapCasmTrackRole(const CasmTrackConfig& cfg) noexcept;

private:
    uasf::SectionType mapSectionType(SffSectionType sffType) noexcept;
    uasf::SectionType mapCasmSectionType(const std::string& name) noexcept;
    uasf::TrackRole mapTrackRole(SffTrackRole sffRole) noexcept;
    uasf::MidiEvent mapMidiEvent(const SffMidiEvent& sffEv) noexcept;
};

} // namespace ai_arranger::importers::sff1
#endif // AI_ARRANGER_SFF1_MAPPER_H
