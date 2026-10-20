#pragma once

#include <cstdint>
#include <vector>

#include "data/archive.h"

namespace bt {

// One MIDI channel message, timed in XMIDI ticks (120 per second).
struct MidiEvent {
    std::uint32_t tick;
    std::uint8_t status;   // 0x80-0xEF; note-offs are synthesised from XMIDI durations
    std::uint8_t data1;
    std::uint8_t data2;
};

struct MidiSequence {
    std::vector<MidiEvent> events;  // sorted by tick
    std::uint32_t length_ticks = 0;
    // Loop points from Miles controllers 116/117 (FOR/NEXT), if present.
    std::int64_t loop_start = -1;
    std::int64_t loop_end = -1;
};

constexpr int kXmidiTicksPerSecond = 120;

// Parses an IFF XMIDI file (FORM XDIR / CAT XMID / FORM XMID / EVNT) and
// returns the sequences it contains. Throws on malformed data.
std::vector<MidiSequence> parse_xmidi(const Bytes& data);

}  // namespace bt
