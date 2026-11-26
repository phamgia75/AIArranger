#include "engine/arranger/chord_input.h"
#include <cstring>

namespace ai_arranger::arranger {

ChordInput::ChordInput() {
    chord_data_.store(packChord(chords::CMaj), std::memory_order_release);
}

void ChordInput::setChord(Chord chord) noexcept {
    chord_data_.store(packChord(chord), std::memory_order_release);
}

Chord ChordInput::getChord() const noexcept {
    return unpackChord(chord_data_.load(std::memory_order_acquire));
}

void ChordInput::setProgression(const Chord* chords, size_t count) noexcept {
    progression_ = const_cast<Chord*>(chords);
    progression_count_ = count;
    progression_index_ = 0;
}

Chord ChordInput::nextProgressionChord() noexcept {
    if (!progression_ || progression_count_ == 0) return chords::NoCh;
    Chord c = progression_[progression_index_];
    progression_index_ = (progression_index_ + 1) % progression_count_;
    setChord(c);
    return c;
}

uint32_t ChordInput::packChord(Chord c) noexcept {
    const uint8_t octByte = static_cast<uint8_t>(static_cast<int8_t>(c.octave));
    return (static_cast<uint32_t>(c.type) & 0xFFu) |
           ((static_cast<uint32_t>(c.root) & 0xFFu) << 8) |
           ((static_cast<uint32_t>(c.bass) & 0xFFu) << 16) |
           ((static_cast<uint32_t>(octByte) & 0xFFu) << 24);
}

Chord ChordInput::unpackChord(uint32_t data) noexcept {
    Chord c{};
    c.type   = static_cast<ChordType>(data & 0xFFu);
    c.root   = static_cast<uint8_t>((data >> 8) & 0xFFu);
    c.bass   = static_cast<uint8_t>((data >> 16) & 0xFFu);
    c.octave = static_cast<int16_t>(static_cast<int8_t>((data >> 24) & 0xFFu));
    return c;
}

} // namespace ai_arranger::arranger
