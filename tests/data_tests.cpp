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
#include "data/xmidi.h"
#include "engine/data_segment.h"
#include "engine/game.h"

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

void test_music(const bt::Archive& a) {
    int tracks = 0, notes = 0;
    for (int i = 0; i < 352; ++i) {
        const bt::Bytes d = a.entry(i);
        if (d.size() < 12 || d[0] != 'F' || d[1] != 'O' || d[2] != 'R' || d[3] != 'M') continue;
        const auto seqs = bt::parse_xmidi(d);
        for (const auto& s : seqs) {
            ++tracks;
            for (const auto& e : s.events) notes += (e.status & 0xF0) == 0x90;
            check(s.length_ticks > 0, "track " + std::to_string(i) + " has a length");
        }
    }
    std::printf("music: %d tracks, %d notes\n", tracks, notes);
    check(tracks >= 20, "at least 20 XMIDI tracks");
}

void test_data_segment(const std::filesystem::path& exe) {
    const bt::DataSegment ds(exe);
    check(ds.u16(0x28CA + 2) == 0x28, "field table entry 1 is 0x28");
    check(ds.u8(0x70EE) == 'F' && ds.u8(0x70F3) == 'R', "FASTER cheat string at ds:70EEh");
    check(ds.u8(0x60CC) == 0x00 && ds.u8(0x60CD) == 0xFF, "terrain set table at ds:60CCh");
    check(bt::DataSegment::field_base(14) == 0x15F2, "field 14 is x");
}

void test_game(const bt::Archive& a, const std::filesystem::path& exe) {
    bt::Game game(a, exe);
    try {
        game.load_level(0xC0, 56, 108);
        const auto& ds = game.machine().ds;
        check(ds.field(bt::field::kScriptSeg, 0) != 0, "Kyle is alive after level load");
        int live = 0;
        for (int s = 0; s < 0x28; s += 2) live += ds.field(bt::field::kScriptSeg, s) != 0;
        std::printf("level 1: %d live actors after load\n", live);
        for (int t = 0; t < 300; ++t) game.tick();
        for (int sl = 0; sl < 0x28; sl += 2) {
            if (!ds.field(bt::field::kScriptSeg, sl)) continue;
            const int p0 = ds.field(0x198A, sl), n = ds.field(0x19B2, sl);
            std::printf("  slot %2d type %02X at (%d,%d) parts %d first %02X colour %04X flags %04X frame %d\n", sl / 2,
                        ds.field(bt::field::kType, sl), ds.field(bt::field::kX, sl), ds.field(bt::field::kY, sl), n, p0,
                        n ? ds.field(0xF52, p0) : 0, ds.field(bt::field::kFlags, sl), static_cast<std::int16_t>(ds.field(0x1782, sl)));
        }
        std::printf("level 1: Kyle at (%d,%d) after 300 ticks\n", ds.field(bt::field::kX, 0), ds.field(bt::field::kY, 0));
    } catch (const std::exception& e) {
        check(false, std::string("level 1 runs 300 ticks: ") + e.what());
    }
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
    test_music(archive);
    test_data_segment(path.parent_path() / "BTHORNE.EXE");
    test_game(archive, path.parent_path() / "BTHORNE.EXE");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
