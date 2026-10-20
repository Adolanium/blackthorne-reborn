// Records our engine's data segment after every tick, in the same format as
// tools/emu/btemu.py records the original, for tools/emu/compare.py.
// Usage: bttrace <DATA.DAT> <ticks> <out.bin> [--input "from-to:hexbits,..."]
//                [--poke "frame:hexaddr=hexvalue,..."] [--map]
// --map also writes the map segment per tick to <out.bin>.map.
// Input for tick k is the schedule's bits at k - 1, matching btemu.py, which
// writes the word for the next tick at each frame boundary.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "data/archive.h"
#include "engine/game.h"

namespace {

struct Span {
    int from, to;
    std::uint16_t bits;
};

std::vector<Span> parse_input(const std::string& spec) {
    std::vector<Span> out;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        const std::string part = spec.substr(pos, end - pos);
        int a = 0, b = 0;
        unsigned bits = 0;
        if (std::sscanf(part.c_str(), "%d-%d:%x", &a, &b, &bits) == 3) out.push_back({a, b, static_cast<std::uint16_t>(bits)});
        pos = end + 1;
    }
    return out;
}

struct Poke {
    int frame;
    unsigned addr, value;
};

std::vector<Poke> parse_pokes(const std::string& spec) {
    std::vector<Poke> out;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        Poke p{};
        if (std::sscanf(spec.substr(pos, end - pos).c_str(), "%d:%x=%x", &p.frame, &p.addr, &p.value) == 3) out.push_back(p);
        pos = end + 1;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: bttrace <DATA.DAT> <ticks> <out.bin> [--input spec]\n");
        return 1;
    }
    const std::filesystem::path data = argv[1];
    const int ticks = std::atoi(argv[2]);
    std::string spec, poke_spec;
    for (int i = 4; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--input") spec = argv[i + 1];
        if (std::string(argv[i]) == "--poke") poke_spec = argv[i + 1];
    }
    bool with_map = false;
    for (int i = 4; i < argc; ++i)
        if (std::string(argv[i]) == "--map") with_map = true;
    const auto schedule = parse_input(spec);
    const auto pokes = parse_pokes(poke_spec);

    bt::Archive archive(data);
    bt::Game game(archive, data.parent_path() / "BTHORNE.EXE");
    game.boot();
    std::ofstream out(argv[3], std::ios::binary);
    std::ofstream map_out;
    if (with_map) map_out.open(std::string(argv[3]) + ".map", std::ios::binary);
    for (int t = 1; t <= ticks; ++t) {
        std::uint16_t bits = 0;
        for (const Span& s : schedule)
            if (s.from <= t - 1 && t - 1 <= s.to) bits |= s.bits;
        game.set_input(bits);
        for (const Poke& p : pokes)
            if (p.frame == t - 1) game.machine().ds.set_u16(static_cast<std::uint16_t>(p.addr), static_cast<std::uint16_t>(p.value));
        game.tick();
        game.vm().events().clear();
        out.write(reinterpret_cast<const char*>(game.machine().ds.raw()), bt::DataSegment::kSize);
        if (with_map) map_out.write(reinterpret_cast<const char*>(game.machine().map.data()), static_cast<std::streamsize>(game.machine().map.size()));
    }
    std::printf("ticks recorded: %d, level %02X\n", ticks, game.level_id());
    return 0;
}
