// Soak test: runs every level (from its password-table start) with random
// input and reports any engine error. Usage: btsoak <DATA.DAT> [ticks] [seed]

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "data/archive.h"
#include "data/level.h"
#include "engine/game.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: btsoak <DATA.DAT> [ticks] [seed]\n");
        return 1;
    }
    const std::filesystem::path data = argv[1];
    const int ticks = argc > 2 ? std::atoi(argv[2]) : 3000;
    const unsigned seed = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1;
    bt::Archive archive(data);
    int failures = 0;
    for (int d : bt::kLevelDescriptors) {
        const int level = d + 1;
        bt::Game game(archive, data.parent_path() / "BTHORNE.EXE");
        // Start position from the password table, else the level's first record.
        int x = 128, y = 112;
        const auto& ds = game.machine().ds;
        bool found = false;
        for (std::uint16_t e = 0x543A; ds.u16(e) != 0xFFFF; e = static_cast<std::uint16_t>(e + 10))
            if (ds.u16(e) == level) {
                x = ds.u16(static_cast<std::uint16_t>(e + 2));
                y = ds.u16(static_cast<std::uint16_t>(e + 4));
                found = true;
            }
        if (!found) {
            const bt::Level lv = bt::load_level(archive, d);
            if (!lv.objects.empty()) {
                x = lv.objects[0].x;
                y = lv.objects[0].y;
            }
        }
        std::mt19937 rng(seed + static_cast<unsigned>(level));
        int t = 0;
        try {
            game.load_level(level, x, y);
            std::uint16_t held = 0;
            for (; t < ticks; ++t) {
                if (t % 20 == 0) held = static_cast<std::uint16_t>(rng() & 0x07FC);  // movement and actions, no menus
                game.set_input(held);
                game.tick();
                game.vm().events().clear();
            }
            std::printf("level %02X ok: %d ticks, now on level %02X, Kyle (%d,%d)\n", level, ticks, game.level_id(),
                        static_cast<std::int16_t>(ds.field(0x15F2, 0)), static_cast<std::int16_t>(ds.field(0x161A, 0)));
        } catch (const std::exception& e) {
            ++failures;
            std::printf("level %02X FAILED at tick %d (now level %02X): %s\n", level, t, game.level_id(), e.what());
        }
    }
    std::printf("%d level(s) failed\n", failures);
    return failures ? 1 : 0;
}
