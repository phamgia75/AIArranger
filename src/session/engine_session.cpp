#include "session/engine_session.h"

namespace ai_arranger::session {

EngineSession::EngineSession() noexcept = default;

bool EngineSession::loadStyle(const uasf::StyleDefinition& style) noexcept {
    if (!player_.loadStyle(style)) return false;
    style_loaded_ = true;

    // Variation A-D -> the style's Main A-D sections (by type, not by index:
    // index k is an Intro/Fill/Ending in most styles). A missing Main falls
    // back to the first Main; a style without Mains keeps indexes 0-3.
    int mains[4] = {-1, -1, -1, -1};
    int firstMain = -1;
    for (size_t i = 0; i < style.sections.size(); ++i) {
        const auto t = style.sections[i].type;
        if (t < uasf::SectionType::Main1 || t > uasf::SectionType::Main4) continue;
        const int k = static_cast<int>(t) - static_cast<int>(uasf::SectionType::Main1);
        if (mains[k] < 0) mains[k] = static_cast<int>(i);
        if (firstMain < 0) firstMain = static_cast<int>(i);
    }
    for (int k = 0; k < 4; ++k)
        if (mains[k] < 0) mains[k] = (firstMain >= 0) ? firstMain : k;
    adapter_.setVariationSections(mains[0], mains[1], mains[2], mains[3]);
    return true;
}

bool EngineSession::boot() noexcept {
    if (booted_) return true;  // idempotent

    clock_.setSampleRate(48000);
    if (!style_loaded_) {
        clock_.setResolution(480);
        clock_.setTempo(120);
        loadStyle(arranger::buildDemoStyle());
    }
    // A style loaded before boot already set the clock's tempo/PPQN/meter
    // (StylePlayer::loadStyle). Overwriting them here ran a 1920-PPQN Genos
    // style on a 480-PPQN clock (4x slow) whenever loadStyle() preceded start().
    clock_.reset();
    booted_ = true;
    return true;
}

void EngineSession::shutdown() noexcept {
    if (!booted_) return;
    // Panic first: it clears the scheduler and then sends a NoteOff per active
    // note + CC 120/123. In the old order stop() scheduled the NoteOffs and the
    // panic's scheduler clear discarded them (Issue #14).
    player_.panic();
    player_.stop();    // nothing left sounding; stops the clock and sequencer
    clock_.stop();
    booted_ = false;
}

void EngineSession::reset() noexcept {
    shutdown();
    boot();
}

} // namespace ai_arranger::session
