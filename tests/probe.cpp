// Debug probe: runs the engine with scripted input and prints an actor's state
// every tick. Usage:
//   btprobe <DATA.DAT> <level hex> <x> <y> <ticks> [slot] [from-to:hexbits ...]

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "data/archive.h"
#include "engine/game.h"

int main(int argc, char** argv) {
    if (argc < 6) {
        std::printf("usage: btprobe <DATA.DAT> <level hex> <x> <y> <ticks> [slot] [from-to:hexbits ...]\n");
        return 1;
    }
    const std::filesystem::path data = argv[1];
    const int level = static_cast<int>(std::strtol(argv[2], nullptr, 16));
    const int x = std::atoi(argv[3]), y = std::atoi(argv[4]), ticks = std::atoi(argv[5]);
    const int slot = argc > 6 ? std::atoi(argv[6]) * 2 : 0;
    struct Range { int from, to; unsigned bits; };
    std::vector<Range> input;
    for (int i = 7; i < argc; ++i) {
        char* p = nullptr;
        const long from = std::strtol(argv[i], &p, 10);
        const long to = std::strtol(p + 1, &p, 10);
        input.push_back({static_cast<int>(from), static_cast<int>(to), static_cast<unsigned>(std::strtol(p + 1, nullptr, 16))});
    }
    bt::Archive archive(data);
    bt::Game game(archive, data.parent_path() / "BTHORNE.EXE");
    game.load_level(level, x, y);
    const auto& ds = game.machine().ds;
    for (int t = 0; t < ticks; ++t) {
        unsigned bits = 0;
        for (const auto& r : input)
            if (t >= r.from && t <= r.to) bits |= r.bits;
        game.set_input(static_cast<std::uint16_t>(bits));
        game.tick();
        const int p0 = ds.field(0x198A, slot);
        std::printf("t%-4d in %04X  pos (%4d,%4d) box [%d..%d]x[%d..%d] frame %3d flags %04X col %04X/%04X ip %04X vel %d,%d hp %d"
                    "  pal ids %04X %04X refs %d %d\n",
                    t, bits, static_cast<std::int16_t>(ds.field(0x15F2, slot)), static_cast<std::int16_t>(ds.field(0x161A, slot)),
                    static_cast<std::int16_t>(ds.field(0x1412, slot)), static_cast<std::int16_t>(ds.field(0x143A, slot)),
                    static_cast<std::int16_t>(ds.field(0x13C2, slot)), static_cast<std::int16_t>(ds.field(0x13EA, slot)),
                    static_cast<std::int16_t>(ds.field(0x1782, slot)), ds.field(0x1462, slot), ds.field(0xF52, p0),
                    ds.field(0xF52, p0 + 2), ds.field(0x1232, slot), static_cast<std::int16_t>(ds.field(0x1502, slot)),
                    static_cast<std::int16_t>(ds.field(0x152A, slot)), static_cast<std::int16_t>(ds.field(0x148A, slot)),
                    ds.u16(0x1C7A), ds.u16(0x1C7C), ds.u16(0x1C6A), ds.u16(0x1C6C));
        game.vm().events().clear();
    }
    return 0;
}
