// Regression tests for the format library against the original DATA.DAT.
// Usage: bttests [path to DATA.DAT]. Exits 0 (skipped) if the file is missing.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "data/actors.h"
#include "data/archive.h"
#include "data/level.h"
#include "data/sprites.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void test_archive(const bt::Archive& a) {
    check(a.count() == 353, "archive has 353 entries");
    // Entry 352 is an end marker; every other entry decodes to its stated size.
    for (int i = 0; i < 352; ++i) check(!a.entry(i).empty(), "entry " + std::to_string(i) + " decodes");
    check(a.entry(64).size() == 57404, "entry 64 (raw actor table) has 57404 bytes");
    check(a.entry(179).size() == 768, "entry 179 is a 768-byte palette");
}

void test_levels(const bt::Archive& a) {
    for (int d : bt::kLevelDescriptors) {
        const bt::Level lv = bt::load_level(a, d);
        const std::string name = "level " + std::to_string(d);
        check(lv.foreground.width > 0 && lv.foreground.height > 0, name + " has a foreground layer");
        check(lv.background.width == lv.foreground.width && lv.background.height == lv.foreground.height,
              name + " layers have equal size");
        check(lv.tiles.size() >= 873, name + " tileset loaded");
        check(!lv.metatiles.empty(), name + " metatiles loaded");
        check(!lv.objects.empty(), name + " has object spawns");
        for (const auto& o : lv.objects) check(o.type < bt::kActorTypeCount, name + " object type in range");
        for (const auto& b : lv.sprite_banks) {
            const bt::Bytes data = a.entry(b.entry);
            check(data.size() % static_cast<std::size_t>(b.width * b.height) == 0,
                  name + " bank " + std::to_string(b.entry) + " divides into frames");
            check(!bt::decode_sprite_bank(data, b.width, b.height).empty(), name + " bank decodes");
        }
    }
    const bt::Level first = bt::load_level(a, 191);
    check(first.foreground.width == 112 && first.foreground.height == 14, "level 1 is 112x14 metatiles");
    check(first.objects.size() == 16, "level 1 has 16 object spawns");
    check(first.objects[0].type == 0x11 && first.objects[0].x == 128, "level 1 starts with the 0x11 start marker");
    check(first.foreground.collision(0, 0) == 1, "level 1 top-left cell is solid");
}

void test_actor_types(const bt::Archive& a) {
    const auto types = bt::load_actor_types(a);
    check(types.size() == bt::kActorTypeCount, "86 actor types");
    check(types[0].frame_w == 32 && types[0].frame_h == 48, "type 0 uses 32x48 frames");
    check(types[0].parts == 3, "type 0 has 3 parts");
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path path = argc > 1 ? argv[1]
        : "C:/Program Files (x86)/Blizzard Classics/Blackthorne/BTHORNE/DATA.DAT";
    if (!std::filesystem::exists(path)) {
        std::printf("SKIP: %s not found\n", path.string().c_str());
        return 0;
    }
    const bt::Archive archive(path);
    test_archive(archive);
    test_levels(archive);
    test_actor_types(archive);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
