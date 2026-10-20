// Soak test: runs every level (from its password-table start) with random
// input and reports any engine error. Usage: btsoak <DATA.DAT> [ticks] [seed]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
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
        const auto& ds = game.machine().ds;
        std::mt19937 rng(seed + static_cast<unsigned>(level));
        int t = 0;
        try {
            game.start_level(level);
            std::uint16_t held = 0;
            for (; t < ticks; ++t) {
                if (t % 20 == 0) held = static_cast<std::uint16_t>(rng() & 0x07FC);  // movement and actions, no menus
                game.set_input(held);
                game.tick();
                game.vm().events().clear();
            }
            // Save-state round trip: a fresh game loaded from the save must
            // stay in lockstep with this one.
            const auto save = std::filesystem::temp_directory_path() / "btsoak.bts";
            game.save_state(save);
            bt::Game copy(archive, data.parent_path() / "BTHORNE.EXE");
            copy.load_state(save);
            for (int k = 0; k < 300; ++k, ++t) {
                if (t % 20 == 0) held = static_cast<std::uint16_t>(rng() & 0x07FC);
                for (bt::Game* g : {&game, &copy}) {
                    g->set_input(held);
                    g->tick();
                    g->vm().events().clear();
                }
            }
            if (std::memcmp(game.machine().ds.raw(), copy.machine().ds.raw(), bt::DataSegment::kSize) != 0)
                throw std::runtime_error("save state diverged after loading");
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
