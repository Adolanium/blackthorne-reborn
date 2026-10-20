#include "data/xmidi.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace bt {
namespace {

std::uint32_t be32(const Bytes& b, std::size_t o) {
    return (static_cast<std::uint32_t>(b.at(o)) << 24) | (b.at(o + 1) << 16) | (b.at(o + 2) << 8) | b.at(o + 3);
}

bool tag_is(const Bytes& b, std::size_t o, std::string_view tag) {
    return o + 4 <= b.size() && std::equal(tag.begin(), tag.end(), b.begin() + static_cast<std::ptrdiff_t>(o));
}

std::uint32_t read_vlq(const Bytes& b, std::size_t& o, std::size_t end) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4 && o < end; ++i) {
        const std::uint8_t c = b[o++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

// EVNT chunk: XMIDI delays are runs of bytes < 0x80 that add up; note-ons
// carry a VLQ duration instead of a matching note-off.
MidiSequence parse_events(const Bytes& b, std::size_t o, std::size_t end) {
    MidiSequence seq;
    std::uint32_t tick = 0;
    while (o < end) {
        const std::uint8_t c = b[o];
        if (c < 0x80) {
            tick += c;
            ++o;
            continue;
        }
        ++o;
        const std::uint8_t type = c & 0xF0;
        if (c == 0xFF) {  // meta event
            const std::uint8_t meta = b.at(o++);
            const std::uint32_t len = read_vlq(b, o, end);
            o += len;
            if (meta == 0x2F) break;  // end of track
            continue;
        }
        if (c == 0xF0 || c == 0xF7) {  // sysex
            o += read_vlq(b, o, end);
            continue;
        }
        if (type == 0x90) {
            const std::uint8_t note = b.at(o++), vel = b.at(o++);
            const std::uint32_t dur = read_vlq(b, o, end);
            seq.events.push_back({tick, c, note, vel});
            seq.events.push_back({tick + dur, static_cast<std::uint8_t>(0x80 | (c & 0x0F)), note, 0});
        } else if (type == 0xC0 || type == 0xD0) {
            seq.events.push_back({tick, c, b.at(o++), 0});
        } else {
            const std::uint8_t d1 = b.at(o++), d2 = b.at(o++);
            if (type == 0xB0 && d1 == 116 && seq.loop_start < 0) seq.loop_start = tick;   // FOR loop
            if (type == 0xB0 && d1 == 117 && seq.loop_end < 0) seq.loop_end = tick;       // NEXT loop
            seq.events.push_back({tick, c, d1, d2});
        }
    }
    std::stable_sort(seq.events.begin(), seq.events.end(),
                     [](const MidiEvent& a, const MidiEvent& b2) { return a.tick < b2.tick; });
    seq.length_ticks = seq.events.empty() ? tick : std::max(tick, seq.events.back().tick);
    return seq;
}

void walk(const Bytes& b, std::size_t o, std::size_t end, std::vector<MidiSequence>& out) {
    while (o + 8 <= end) {
        const std::uint32_t len = be32(b, o + 4);
        const std::size_t body = o + 8;
        if (body + len > b.size()) throw std::runtime_error("XMIDI chunk overruns file");
        if (tag_is(b, o, "FORM") || tag_is(b, o, "CAT ")) {
            walk(b, body + 4, body + len, out);  // skip the form type (XDIR / XMID)
        } else if (tag_is(b, o, "EVNT")) {
            out.push_back(parse_events(b, body, body + len));
        }
        o = body + len + (len & 1);
    }
}

}  // namespace

std::vector<MidiSequence> parse_xmidi(const Bytes& data) {
    if (!tag_is(data, 0, "FORM")) throw std::runtime_error("not an XMIDI file");
    std::vector<MidiSequence> out;
    walk(data, 0, data.size(), out);
    if (out.empty()) throw std::runtime_error("XMIDI file has no EVNT chunk");
    return out;
}

}  // namespace bt
