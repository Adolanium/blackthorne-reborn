// btexport: writes every level's map layers as PNGs, the material for
// repainting them (docs/VISUALS.md). The output is derived from the game
// data, so it must stay out of git (export it to a git-ignored folder).
//
// Usage: btexport <DATA.DAT> <out dir> [level ids in hex, comma separated]
//
// For each level, <out>/levels/<ID>/ gets:
//   fg.png, bg.png              the foreground / background layer, 1 pixel per
//                               game pixel, transparent where no tile is drawn
//   fg_front.png, bg_front.png  the same layers' "in front" tiles only (tile
//                               bit 3: drawn over sprites), to split a repaint
//   preview.png                 background under foreground, for reference
//   level.json                  sizes, parallax and the base palette

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "data/archive.h"
#include "engine/game.h"
#include "render/png.h"
#include "render/renderer.h"

namespace {

// Every level the game reaches: the intro and title scenes, the 21 password
// levels and the ending.
const std::vector<int> kAllLevels = {0x61, 0x67, 0x6E, 0x75, 0x7B, 0x84, 0x85, 0x9C, 0xB3, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5,
                                     0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xEC, 0xED, 0xEE,
                                     0xEF, 0xF0, 0x15B, 0x15D, 0x15E, 0x15F, 0x160};

const char* kReadme = R"(Blackthorne Reborn: level layers for repainting
================================================

These images come from the original game data. Do not commit them or
anything made from them to a public repository.

Each levels/<ID>/ folder has:
  fg.png, bg.png        the foreground and background layers at 1x (one pixel
                        per game pixel), transparent where nothing is drawn
  fg_front.png,         the parts of those layers that the game draws IN FRONT
  bg_front.png          of the characters (columns, foliage, bars)
  preview.png           background under foreground, for reference
  level.json            sizes and palette

Making an art pack
------------------
Repaint fg.png and bg.png at exactly 4 times their size, keeping every
element where it is, so that walls, ledges, ladders and doors line up with
the game's collision. Keep transparency where the 1x image is transparent:
the background shows through, and the foreground only covers what it
covered before. Save the results as fg.png / bg.png (4x) in
  <pack>/levels/<ID>/
and put a pack.json next to levels/ with {"scale": 4}. Missing files fall
back to the original art, so a pack can be partial.
)";

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: btexport <DATA.DAT> <out dir> [level ids in hex, comma separated]\n");
        return 1;
    }
    const std::filesystem::path data = argv[1], out = argv[2];
    std::vector<int> levels = kAllLevels;
    if (argc > 3) {
        levels.clear();
        for (const char* p = argv[3]; *p;) {
            char* end = nullptr;
            levels.push_back(static_cast<int>(std::strtol(p, &end, 16)));
            p = *end == ',' ? end + 1 : end;
            if (end == p && *p) break;
        }
    }
    bt::Archive archive(data);
    const bt::Renderer renderer(archive);
    std::filesystem::create_directories(out / "levels");
    std::ofstream(out / "README.txt") << kReadme;
    int exported = 0;
    for (int level : levels) {
        char id[8];
        std::snprintf(id, sizeof id, "%X", level);
        try {
            bt::Game game(archive, data.parent_path() / "BTHORNE.EXE");
            game.start_level(level);
            const bt::Machine& m = game.machine();
            const bt::DataSegment& ds = m.ds;
            // The level's own palette (ds:4A12h), not the shown one: right after
            // a load the screen is still black from the fade.
            const bt::Scene scene{&m, game.has_background(), ds.raw() + 0x4A12};
            const std::filesystem::path dir = out / "levels" / id;
            std::filesystem::create_directories(dir);
            bt::Image fg, fg_front, bg, bg_front;
            renderer.render_layer(scene, false, -1, fg);
            renderer.render_layer(scene, false, 8, fg_front);
            bt::write_png(dir / "fg.png", fg);
            bt::write_png(dir / "fg_front.png", fg_front);
            bt::Image preview = fg;
            if (game.has_background()) {
                renderer.render_layer(scene, true, -1, bg);
                renderer.render_layer(scene, true, 8, bg_front);
                bt::write_png(dir / "bg.png", bg);
                bt::write_png(dir / "bg_front.png", bg_front);
                // The preview puts the background's top left under the foreground's.
                for (int y = 0; y < preview.height; ++y)
                    for (int x = 0; x < preview.width; ++x) {
                        std::uint32_t& p = preview.pixels[static_cast<std::size_t>(y) * preview.width + x];
                        if (p >> 24) continue;
                        if (x < bg.width && y < bg.height) p = bg.pixels[static_cast<std::size_t>(y) * bg.width + x];
                    }
            }
            for (std::uint32_t& p : preview.pixels)
                if (!(p >> 24)) p = 0xFF000000u;
            bt::write_png(dir / "preview.png", preview);
            std::ofstream json(dir / "level.json");
            json << "{\n  \"id\": \"" << id << "\",\n  \"foreground\": {\"width\": " << fg.width << ", \"height\": " << fg.height
                 << "},\n  \"background\": " << (game.has_background() ? "{\"width\": " + std::to_string(bg.width) + ", \"height\": " + std::to_string(bg.height) + "}" : std::string("null"))
                 << ",\n  \"parallax\": [" << ds.u16(0x1CEE + 0x32) << ", " << ds.u16(0x1CEE + 0x34) << "],\n  \"palette\": \"";
            for (int i = 0; i < 768; ++i) {
                char hex[3];
                std::snprintf(hex, sizeof hex, "%02x", ds.u8(static_cast<std::uint16_t>(0x4A12 + i)) * 255 / 63);
                json << hex;
            }
            json << "\"\n}\n";
            std::printf("level %s: %dx%d%s\n", id, fg.width, fg.height, game.has_background() ? " + background" : "");
            ++exported;
        } catch (const std::exception& e) {
            std::printf("level %s: skipped (%s)\n", id, e.what());
        }
    }
    std::printf("%d level(s) exported to %s\n", exported, out.string().c_str());
    return 0;
}
