#include "engine/game.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace bt {
namespace {

constexpr std::uint16_t kDescriptor = 0x1CEE;
constexpr std::uint16_t kBasePalette = 0x4A12, kShownPalette = 0x4D12;

// Globals the level loader resets (the zeroing block at the top of sub_105B4).
constexpr std::uint16_t kResetGlobals[] = {
    0x000, 0x002, 0x004, 0x312, 0x30E, 0x310, 0x34C, 0x390, 0x316, 0x32C, 0x358, 0x35A, 0x35C, 0x35E, 0x362,
    0x2A82, 0x340, 0x342, 0x344, 0x346, 0x348, 0x34A, 0x31A, 0x1B6E, 0x1B70, 0x1B72, 0x1B74, 0x1B76, 0x1B78,
    0x1B7A, 0x1B7C, 0x320, 0x322, 0x324,
};

}  // namespace

Game::Game(const Archive& archive, const std::filesystem::path& exe_path, int language)
    : archive_(archive), machine_(std::make_unique<Machine>()) {
    machine_->ds = DataSegment(exe_path);
    machine_->scripts = archive.entry(64);
    // seg001 + seg002 from the EXE image (file offset 0xA500), with the text
    // resource loaded at the start of seg001 (sub_10A6B).
    {
        std::ifstream exe(exe_path, std::ios::binary);
        machine_->text.assign(Machine::kTextImageSize, 0);
        exe.seekg(0xA500);
        exe.read(reinterpret_cast<char*>(machine_->text.data()), static_cast<std::streamsize>(Machine::kTextImageSize));
        const Bytes table = archive.entry(1 + std::clamp(language, 0, 2));
        std::copy_n(table.begin(), std::min(table.size(), machine_->text.size()), machine_->text.begin());
    }
    // sub_19046 (startup): unpack the sprite palettes listed at ds:6A78h, in
    // order, into the table at ds:6ABCh that palette slots copy from.
    DataSegment& ds = machine_->ds;
    std::uint16_t dst = 0x6ABC;
    for (std::uint16_t a = 0x6A78; ds.u16(a) != 0xFFFF; a = static_cast<std::uint16_t>(a + 2))
        for (std::uint8_t b : archive.resource(ds.u16(a))) ds.set_u8(dst++, b);
    vm_ = std::make_unique<Vm>(*machine_);
}

Game::~Game() { stop_worker(); }

// Ends the worker (it unwinds from its frame wait) so the state can be
// replaced or the game destroyed.
void Game::stop_worker() {
    if (!worker_.joinable()) return;
    stopping_ = true;
    go_.release();
    worker_.join();
    stopping_ = false;
    saveable_ = false;
    nesting_ = 0;
}

namespace {
constexpr char kSaveMagic[8] = {'B', 'T', 'R', 'S', 'A', 'V', 'E', '1'};
}

void Game::save_state(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    const Machine& m = *machine_;
    auto put = [&](const void* p, std::size_t n) { out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); };
    put(kSaveMagic, sizeof kSaveMagic);
    if (!saveable_ && worker_.joinable()) throw std::runtime_error("can't save in the middle of a transition");
    const std::int32_t header[3] = {level_id_, has_background_ ? 1 : 0, 3};  // [2] = 3: the map, tiles and palette at load follow
    put(header, sizeof header);
    put(m.ds.raw(), DataSegment::kSize);
    put(m.map.data(), m.map.size());
    put(m.frames.data(), m.frames.size());
    put(m.statics.data(), m.statics.size());
    put(m.tiles.data(), m.tiles.size());
    put(m.tile_masks.data(), m.tile_masks.size());
    put(m.tile_anims.data(), m.tile_anims.size());
    put(m.map_at_load.data(), m.map_at_load.size());
    put(m.tiles_at_load.data(), m.tiles_at_load.size());
    put(m.tile_masks_at_load.data(), m.tile_masks_at_load.size());
    put(m.palette_at_load.data(), m.palette_at_load.size());
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

void Game::load_state(const std::filesystem::path& path) {
    stop_worker();
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("no save at " + path.string());
    char magic[sizeof kSaveMagic];
    in.read(magic, sizeof magic);
    if (!in || !std::equal(magic, magic + sizeof magic, kSaveMagic)) throw std::runtime_error(path.string() + " is not a save state");
    std::int32_t header[3];
    in.read(reinterpret_cast<char*>(header), sizeof header);
    auto machine = std::make_unique<Machine>();
    machine->scripts = machine_->scripts;
    machine->text = machine_->text;
    std::vector<std::uint8_t> ds(DataSegment::kSize);
    auto get = [&](void* p, std::size_t n) { in.read(static_cast<char*>(p), static_cast<std::streamsize>(n)); };
    get(ds.data(), ds.size());
    get(machine->map.data(), machine->map.size());
    get(machine->frames.data(), machine->frames.size());
    get(machine->statics.data(), machine->statics.size());
    get(machine->tiles.data(), machine->tiles.size());
    get(machine->tile_masks.data(), machine->tile_masks.size());
    get(machine->tile_anims.data(), machine->tile_anims.size());
    // Older saves: the saved map and tiles stand in for the loaded ones.
    machine->map_at_load = machine->map;
    machine->tiles_at_load = machine->tiles;
    machine->tile_masks_at_load = machine->tile_masks;
    std::copy_n(ds.data() + kBasePalette, 768, machine->palette_at_load.begin());
    if (header[2] >= 1) get(machine->map_at_load.data(), machine->map_at_load.size());
    if (header[2] >= 2) {
        get(machine->tiles_at_load.data(), machine->tiles_at_load.size());
        get(machine->tile_masks_at_load.data(), machine->tile_masks_at_load.size());
    }
    if (header[2] >= 3) get(machine->palette_at_load.data(), machine->palette_at_load.size());
    if (!in) throw std::runtime_error(path.string() + " is truncated");
    for (std::size_t i = 0; i < ds.size(); ++i) machine->ds.set_u8(static_cast<std::uint16_t>(i), ds[i]);
    // Sprite banks: the ids the level loaded are in ds:1152h (slot = index).
    for (int si = 0; si < 0x20; si += 2) {
        const std::uint16_t id = machine->ds.u16(static_cast<std::uint16_t>(0x1152 + si));
        if (id && id != 0xFFFF && id < 353) machine->banks[si / 2] = archive_.resource(id);
    }
    level_id_ = header[0];
    has_background_ = header[1] != 0;
    menu_ = {};
    entry_ = Entry::AfterWait;  // saves are made at the frame wait of a plain main loop pass
    level_ = bt::load_level(archive_, level_id_ - 1);
    *machine_ = std::move(*machine);
}

void Game::start_level(int level_id) {
    const DataSegment& ds = machine_->ds;
    for (std::uint16_t e = 0x543A; ds.u16(e) != 0xFFFF; e = static_cast<std::uint16_t>(e + 10))
        if (ds.u16(e) == level_id) {
            load_level(level_id, ds.u16(static_cast<std::uint16_t>(e + 2)), ds.u16(static_cast<std::uint16_t>(e + 4)));
            return;
        }
    const Level lv = bt::load_level(archive_, level_id - 1);
    load_level(level_id, lv.objects.empty() ? 128 : lv.objects[0].x, lv.objects.empty() ? 112 : lv.objects[0].y);
}

void Game::boot() {
    DataSegment& ds = machine_->ds;
    // sub_10D0D: the settings block (DATA.DAT entry 0) goes to ds:25E6h.
    const Bytes settings = archive_.entry(0);
    for (std::size_t i = 0; i < settings.size(); ++i) ds.set_u8(static_cast<std::uint16_t>(0x25E6 + i), settings[i]);
    // sub_10E05
    for (std::uint16_t a : {std::uint16_t{0x302}, std::uint16_t{0x304}, std::uint16_t{0x31A}, std::uint16_t{0x328}, std::uint16_t{0x32A}, std::uint16_t{0x318}}) ds.set_u16(a, 0);
    ds.set_u16(0x386, 0xFFFF);
    // sub_10D0D takes these from the setup program's config: music and sound on.
    ds.set_u16(0x37E, 1);
    ds.set_u16(0x380, 1);
    const char start[4] = {'S', 'T', 'R', 'T'};
    for (int k = 0; k < 4; ++k) ds.set_u16(static_cast<std::uint16_t>(0x372 + 2 * k), static_cast<std::uint8_t>(start[k]));
    // loc_190B4: the frame rate divisor (2: 30 frames a second).
    ds.set_u16(0x66, 0);
    ds.set_u16(0x68, 2);
    // sub_128E9: Kyle's starting flags and the first level, loaded from the
    // "next level" variables like every level change.
    ds.set_field(0x1552, 0, 0);
    ds.set_field(field::kFlags, 0, 0x27);
    ds.set_u16(0x1CA8, 0x61);
    ds.set_u16(0x1CAA, 0x80);
    ds.set_u16(0x1CAC, 0x70);
    load_level(0x61, 0x80, 0x70);
}

void Game::load_level(int level_id, int x, int y) {
    stop_worker();
    load_level_data(level_id, x, y);
    entry_ = Entry::AfterLoad;
}

void Game::load_level_data(int level_id, int x, int y) {
    DataSegment& ds = machine_->ds;
    // sub_10A15 spawns Kyle with the parameter and low flag byte he had before.
    const std::uint16_t kyle_param = ds.field(0x1552, 0);
    const std::uint16_t kyle_flags = static_cast<std::uint16_t>(ds.field(field::kFlags, 0) & 0xFF);
    for (std::uint16_t a : kResetGlobals) ds.set_u16(a, 0);
    // sub_10F6D: every level starts the random generator (sub_10C9F) from
    // the same seed, and stops any macro (demo) recording or playback.
    for (std::uint16_t a = 0x4E; a < 0x58; a = static_cast<std::uint16_t>(a + 2)) ds.set_u16(a, 0x481E);
    ds.set_u16(0x58, 0x260A);
    ds.set_u16(0x306, 0);
    ds.set_u16(0x308, 0);
    ds.set_u16(0x304, 0);
    ds.set_u16(0x1C8A, 0x1000);  // velocity limits
    ds.set_u16(0x1C8C, 0x1000);
    ds.set_u16(0x326, 0xFFFF);
    for (std::uint16_t a = 0x452; a < 0x452 + 0xDE0; a = static_cast<std::uint16_t>(a + 2)) ds.set_u16(a, 0);  // sub_11F38
    ds.set_u16(0x259E, Machine::kFrameSeg);
    ds.set_u16(0x2594, Machine::kTileAnimSeg);
    ds.set_u16(0x25A0, Machine::kStaticSeg);
    // The map, frame and static segments are not cleared: past the new
    // level's data they keep the previous level's bytes, as on the original.

    level_id_ = level_id;
    ds.set_u16(0x1CA2, ds.u16(0x1CA4));
    ds.set_u16(0x1CA4, static_cast<std::uint16_t>(level_id));

    // sub_106AD: the descriptor lives in the data segment.
    const Bytes desc = archive_.resource(level_id);
    for (std::size_t i = 0; i < desc.size(); ++i) ds.set_u8(static_cast<std::uint16_t>(kDescriptor + i), desc[i]);
    level_ = bt::load_level(archive_, level_id - 1);

    // sub_106D9: foreground, background and metatiles packed in the map segment.
    std::uint16_t off = 0;
    auto put = [&](int id) {
        const Bytes data = archive_.resource(id);
        const std::uint16_t start = off;
        for (std::uint8_t b : data) machine_->map[off++] = b;
        return start;
    };
    ds.set_u16(0x25B0, put(ds.u16(kDescriptor + 0x21)));
    has_background_ = static_cast<std::int16_t>(ds.u16(kDescriptor + 0x2C)) >= 0;
    if (has_background_) ds.set_u16(0x25B2, put(ds.u16(kDescriptor + 0x2C)));
    ds.set_u16(Machine::kMetatileTable, put(ds.u16(kDescriptor + 0x25)));

    // sub_106B5: the tileset and its masks.
    machine_->tiles.fill(0);
    machine_->tile_masks.fill(0);
    machine_->tile_anims.fill(0);
    {
        const Bytes t = archive_.resource(ds.u16(kDescriptor + 0x23));
        std::copy_n(t.begin(), std::min(t.size(), machine_->tiles.size()), machine_->tiles.begin());
        const Bytes m = archive_.resource(ds.u16(kDescriptor + 0x23) + 1);
        std::copy_n(m.begin(), std::min(m.size(), machine_->tile_masks.size()), machine_->tile_masks.begin());
    }

    walk_descriptor();

    // sub_10A48: part sizes for this level.
    const std::uint8_t sizes = ds.u8(kDescriptor + 7);
    ds.set_u16(0x350, ds.u8(static_cast<std::uint16_t>(0x25DA + sizes)));
    ds.set_u16(0x352, ds.u8(static_cast<std::uint16_t>(0x25E0 + sizes)));

    // sub_11142: free every actor slot.
    for (int s = 0x26; s >= 0; s -= 2) {
        ds.set_field(field::kScriptSeg, s, 0);
        ds.set_field(0x1912, s, 0);
        ds.set_field(0x18EA, s, 0xFFFF);
    }

    // loc_15C36 (with the font load): the text layer starts empty.
    for (int i = 0; i < 0x380; ++i) ds.set_u16(static_cast<std::uint16_t>(0x2D08 + 2 * i), 0x20);

    // The shown palette is left alone: it is still black from the fade out
    // (or from start-up) until the fade in after the first two passes.

    // sub_10A15: Kyle is actor type 0 in slot 0.
    ds.set_u16(0x31E, 0);
    ds.set_u16(0x26, static_cast<std::uint16_t>(x));
    ds.set_u16(0x28, static_cast<std::uint16_t>(y));
    ds.set_u16(0x32E, kyle_param);
    ds.set_u16(ds_addr::kCurrentSlot, 0xFFFF);
    // ds:364h/366h (the hot spot) keep what the last spawn left: sub_10A15
    // does not set them, so Kyle's half size can be 0 (as in the intro).
    if (vm_->spawn_actor(0, 0xFFFF, kyle_flags) != 0) throw std::runtime_error("could not spawn Kyle");
    ds.set_u16(0x31E, 2);

    set_camera_bounds();
    place_camera();
    machine_->repainted.fill(0);
    machine_->map_at_load = machine_->map;
    machine_->tiles_at_load = machine_->tiles;
    machine_->tile_masks_at_load = machine_->tile_masks;
    std::copy_n(ds.raw() + kBasePalette, 768, machine_->palette_at_load.begin());
    draw_page();  // sub_10A0C draws the first screen and flips to it
    page_flip();
    hud_init();
    music_action(ds.u8(0x1CF2));  // sub_11C64: the entry action
    spawn_start_records();
    vm_->spawn_visible_records();
    settle_input();  // sub_10A93 twice: nothing counts as newly pressed
    settle_input();
}

// sub_190C1: typing the string at ds:70EEh ("FASTER") toggles the frame
// divisor ds:68h between 2 (30 frames a second) and 1 (60).
void Game::typed_cheat() {
    DataSegment& ds = machine_->ds;
    const std::uint8_t key = ds.u8(0x66);
    if (!key) return;
    const std::uint16_t at = ds.u16(0x70F5);
    if (ds.u8(at) == key) {
        ds.set_u16(0x70F5, static_cast<std::uint16_t>(at + 1));
        if (ds.u8(static_cast<std::uint16_t>(at + 1)) == 0) {
            ds.set_u16(0x68, ds.u16(0x68) ^ 3);
            ds.set_u16(0x70F5, 0x70EE);
        }
    } else {
        ds.set_u16(0x70F5, 0x70EE);
    }
    ds.set_u16(0x66, 0);
}

// sub_10A93: newly pressed bits from the latched input word (no new read).
void Game::settle_input() {
    DataSegment& ds = machine_->ds;
    const std::uint16_t now = ds.u16(0x35A);
    ds.set_u16(0x35C, static_cast<std::uint16_t>((now ^ ds.u16(0x360)) & now));
    ds.set_u16(0x35E, static_cast<std::uint16_t>(ds.u16(0x35E) | ds.u16(0x35C)));
    ds.set_u16(0x360, now);
}

// The music actions of the table at ds:295Ah, run with the header's entry
// action (byte 4, ds:1CF2h) on load and its exit action (ds:1CF4h) when the
// level ends; byte 5 is the song. 0 = play the song (loc_11CC2), 1 = keep,
// 2 = stop (loc_11CF6), 3 = load only (sub_11C77).
void Game::music_action(int action) {
    DataSegment& ds = machine_->ds;
    auto& ev = vm_->events();
    const int song = ds.u8(0x1CF3);
    auto load = [&] {  // sub_11C77
        if (song == ds.u16(0x386)) return;
        ds.set_u16(0x386, static_cast<std::uint16_t>(song));
        ds.set_u16(0x38E, 0);
        ev.push_back({VmEvent::Kind::MusicLoad, song});
    };
    switch (action) {
        case 0:
            load();
            if (ds.u16(0x37E) && (song != ds.u16(0x386) || ds.u16(0x38E) == 0)) {
                ds.set_u16(0x38A, static_cast<std::uint16_t>(song));
                ev.push_back({VmEvent::Kind::MusicStart, song});
                ds.set_u16(0x38E, 1);
            }
            break;
        case 2:
            ev.push_back({VmEvent::Kind::MusicStop, 0});
            ds.set_u16(0x38E, 0);
            break;
        case 3: load(); break;
        default: break;
    }
}

// Walks the descriptor body in the order of sub_105B4's helpers, filling the
// tables the game reads at run time.
void Game::walk_descriptor() {
    DataSegment& ds = machine_->ds;
    auto u16 = [&](std::uint16_t a) { return ds.u16(a); };
    std::uint16_t o = kDescriptor + 0x36;
    while (u16(o) != 0xFFFF) o = static_cast<std::uint16_t>(o + 14);  // sub_1074E: spawn records
    o = static_cast<std::uint16_t>(o + 2);

    // sub_10760: resources unpacked into the palette area at slot * 3; slots
    // from 128 up are reference-counted sprite palettes.
    for (int s = 0; s < 8; ++s) {
        ds.set_u16(static_cast<std::uint16_t>(0x1C7A + 2 * s), 0xFFFF);
        ds.set_u16(static_cast<std::uint16_t>(0x1C6A + 2 * s), 0);
    }
    while (u16(o) != 0xFFFF) {
        const std::uint16_t id = u16(o);
        const std::uint8_t slot = ds.u8(static_cast<std::uint16_t>(o + 2));
        const Bytes data = archive_.resource(id);
        for (std::size_t i = 0; i < data.size(); ++i)
            ds.set_u8(static_cast<std::uint16_t>(kBasePalette + slot * 3 + i), data[i]);
        const int ps = (slot >> 4) - 8;
        if (ps >= 0) {
            ds.set_u16(static_cast<std::uint16_t>(0x1C7A + 2 * ps), id);
            ds.set_u16(static_cast<std::uint16_t>(0x1C6A + 2 * ps), static_cast<std::uint16_t>(u16(static_cast<std::uint16_t>(0x1C6A + 2 * ps)) + 1));
        }
        o = static_cast<std::uint16_t>(o + 3);
    }
    o = static_cast<std::uint16_t>(o + 2);

    // sub_107BC: background tile animation sequences.
    ds.set_u16(0x1B86, u16(o));
    o = static_cast<std::uint16_t>(o + 2);
    for (int si = 0; ds.u8(o) != 0; si += 2) {
        ds.set_u16(static_cast<std::uint16_t>(0x1B98 + si), ds.u8(o));
        ds.set_u16(static_cast<std::uint16_t>(0x1BA8 + si), ds.u8(o));
        ds.set_u16(static_cast<std::uint16_t>(0x1BB8 + si), ds.u8(static_cast<std::uint16_t>(o + 1)));
        ds.set_u16(static_cast<std::uint16_t>(0x1BC8 + si), ds.u8(static_cast<std::uint16_t>(o + 2)));
        o = static_cast<std::uint16_t>(o + 3);
        ds.set_u16(static_cast<std::uint16_t>(0x1BD8 + si), o);
        while (u16(o) != 0xFFFF) o = static_cast<std::uint16_t>(o + 2);
        o = static_cast<std::uint16_t>(o + 2);
    }
    o = static_cast<std::uint16_t>(o + 1);

    // sub_10802: tile animations. Each record: flags (bit 7 = random delay),
    // frame count, current frame, first tile, tile count * 32, frames resource.
    ds.set_u16(0x1BE8, u16(o));
    o = static_cast<std::uint16_t>(o + 2);
    for (std::uint16_t si = 0, at_anim = 0; ds.u8(o) != 0; si = static_cast<std::uint16_t>(si + 2), o = static_cast<std::uint16_t>(o + 8)) {
        std::uint16_t fl = ds.u8(o);
        if (fl & 0x80) fl = static_cast<std::uint16_t>((fl & 0xFF7F) | 0x8000);
        ds.set_u16(static_cast<std::uint16_t>(0x1BEA + si), fl);
        ds.set_u16(static_cast<std::uint16_t>(0x1BFA + si), 1);
        ds.set_u16(static_cast<std::uint16_t>(0x1C0A + si), ds.u8(static_cast<std::uint16_t>(o + 1)));
        ds.set_u16(static_cast<std::uint16_t>(0x1C1A + si), ds.u8(static_cast<std::uint16_t>(o + 2)));
        ds.set_u16(static_cast<std::uint16_t>(0x1C2A + si), static_cast<std::uint16_t>(ds.u8(static_cast<std::uint16_t>(o + 3)) << 6));
        ds.set_u16(static_cast<std::uint16_t>(0x1C3A + si), static_cast<std::uint16_t>(u16(static_cast<std::uint16_t>(o + 4)) >> 5));
        ds.set_u16(static_cast<std::uint16_t>(0x1C4A + si), at_anim);
        ds.set_u16(static_cast<std::uint16_t>(0x1C5A + si), Machine::kTileAnimSeg);
        for (std::uint8_t b : archive_.resource(u16(static_cast<std::uint16_t>(o + 6)))) machine_->tile_anims[at_anim++] = b;
    }
    o = static_cast<std::uint16_t>(o + 1);

    // sub_10876: single-frame sprites preloaded into the static segment.
    std::uint16_t at = 0;
    for (int si = 0x3E; u16(o) != 0xFFFF; si -= 2, o = static_cast<std::uint16_t>(o + 6)) {
        const std::uint16_t id = u16(o);
        ds.set_u16(static_cast<std::uint16_t>(0x11B2 + si), id);
        ds.set_u16(static_cast<std::uint16_t>(0x11F2 + si), at);
        for (std::uint8_t b : archive_.resource(id)) machine_->statics[at++] = b;
    }
    o = static_cast<std::uint16_t>(o + 2);

    // sub_108A5: animated sprite banks (kept in EMS by the original).
    for (int si = 0x1E; u16(o) != 0xFFFF; si -= 2, o = static_cast<std::uint16_t>(o + 5)) {
        const std::uint16_t id = u16(o);
        ds.set_u16(static_cast<std::uint16_t>(0x1152 + si), id);
        ds.set_u16(static_cast<std::uint16_t>(0x1172 + si), static_cast<std::uint16_t>(si / 2));
        ds.set_u16(static_cast<std::uint16_t>(0x1192 + si),
                   static_cast<std::uint16_t>(ds.u8(static_cast<std::uint16_t>(o + 3)) * ds.u8(static_cast<std::uint16_t>(o + 2)) * 64));
        machine_->banks[si / 2] = archive_.resource(id);
    }
    o = static_cast<std::uint16_t>(o + 2);

    // sub_108F3: exit records (10 bytes until 0xFF).
    ds.set_u16(0x1CA6, o);
    while (ds.u8(o) != 0xFF) o = static_cast<std::uint16_t>(o + 10);
    o = static_cast<std::uint16_t>(o + 1);

    // sub_10904: frame buffer slots {offset, size code}; offsets accumulate.
    ds.set_u16(0x1CAE, o);
    std::uint16_t cx = 0;
    while (u16(o) != 0xFFFF) {
        ds.set_u16(o, cx);
        const int w = ds.u8(static_cast<std::uint16_t>(o + 2)), h = ds.u8(static_cast<std::uint16_t>(o + 3));
        cx = static_cast<std::uint16_t>(cx + w * h * 72);
        o = static_cast<std::uint16_t>(o + 4);
    }
}

// sub_10936: map row size and camera limits.
void Game::set_camera_bounds() {
    DataSegment& ds = machine_->ds;
    const int w = ds.u16(kDescriptor + 0x1C), h = ds.u16(kDescriptor + 0x1E);
    ds.set_u16(0x1C96, static_cast<std::uint16_t>(w * 2));
    ds.set_u16(0x1C92, static_cast<std::uint16_t>(w * 2));
    ds.set_u16(0x1C8E, static_cast<std::uint16_t>(std::max(0, w * 16 - 0x100)));
    ds.set_u16(0x1C94, static_cast<std::uint16_t>(h * 2));
    ds.set_u16(0x1C90, static_cast<std::uint16_t>(std::max(0, h * 16 - 0xE0)));
    const int bw = ds.u16(kDescriptor + 0x27), bh = ds.u16(kDescriptor + 0x29);
    ds.set_u16(0x1CA0, static_cast<std::uint16_t>(bw * 2));
    ds.set_u16(0x1C9C, static_cast<std::uint16_t>(bw * 2));
    ds.set_u16(0x1C98, static_cast<std::uint16_t>(std::max(0, bw * 16 - 0x100)));
    ds.set_u16(0x1C9E, static_cast<std::uint16_t>(bh * 2));
    ds.set_u16(0x1C9A, static_cast<std::uint16_t>(std::max(0, bh * 16 - 0xE0)));
}

// sub_10991: the camera shows the 256x224 screen containing Kyle.
void Game::place_camera() {
    DataSegment& ds = machine_->ds;
    auto snap = [](int pos, int max, int size) {
        if (pos < 0) pos = 0;
        if (pos > max) pos = max;
        return pos / size * size;
    };
    ds.set_u16(0x1B4E, static_cast<std::uint16_t>(snap(static_cast<std::int16_t>(ds.field(field::kX, 0)), ds.u16(0x1C8E), 0x100)));
    ds.set_u16(0x1B50, static_cast<std::uint16_t>(snap(static_cast<std::int16_t>(ds.field(field::kY, 0)), ds.u16(0x1C90), 0xE0)));
    // Background camera: the foreground camera times the 8.8 parallax factors
    // in header words 0x32 / 0x34 (integer part 1B52h/1B54h, fraction 1B56h/1B58h).
    const std::uint32_t bx = static_cast<std::uint32_t>(ds.u16(kDescriptor + 0x32)) * ds.u16(0x1B4E);
    const std::uint32_t by = static_cast<std::uint32_t>(ds.u16(kDescriptor + 0x34)) * ds.u16(0x1B50);
    ds.set_u16(0x1B52, static_cast<std::uint16_t>(bx >> 8));
    ds.set_u16(0x1B56, static_cast<std::uint16_t>(bx & 0xFF));
    ds.set_u16(0x1B54, static_cast<std::uint16_t>(by >> 8));
    ds.set_u16(0x1B58, static_cast<std::uint16_t>(by & 0xFF));
    // Level 0x9C (the title text) keeps one screen per language in its background.
    if (ds.u16(0x1CA4) == 0x9C) ds.set_u16(0x1B54, static_cast<std::uint16_t>(0xE0 * ds.u16(0x6A)));
}

// sub_100B6's test: has Kyle left the screen the camera shows?
bool Game::camera_needs_flip() const {
    const DataSegment& ds = machine_->ds;
    const int kx = static_cast<std::int16_t>(ds.field(field::kX, 0)), ky = static_cast<std::int16_t>(ds.field(field::kY, 0));
    const int cx = ds.u16(0x1B4E), cy = ds.u16(0x1B50);
    auto sgn = [](int v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(v)); };
    if (sgn(kx - cx) < 0 ? cx != 0 : (sgn(kx - cx - 0x101) >= 0 && cx < static_cast<std::int16_t>(ds.u16(0x1C8E)))) return true;
    if (sgn(ky - cy) < 0 ? cy != 0 : (sgn(ky - cy - 0xE1) >= 0 && cy < static_cast<std::int16_t>(ds.u16(0x1C90)))) return true;
    return false;
}

// sub_11442: spawn every record flagged 0x800 (present from the start).
void Game::spawn_start_records() {
    DataSegment& ds = machine_->ds;
    ds.set_u16(ds_addr::kCurrentSlot, 0xFFFF);
    for (int r = 0;; ++r) {
        const std::uint16_t rec = static_cast<std::uint16_t>(ds_addr::kSpawnRecords + r * 14);
        if (ds.u16(rec) == 0xFFFF) break;
        if (ds.u16(static_cast<std::uint16_t>(rec + 8)) == 0 || !(ds.u16(static_cast<std::uint16_t>(rec + 10)) & 0x800)) continue;
        ds.set_u16(0x26, ds.u16(rec));
        ds.set_u16(0x28, ds.u16(static_cast<std::uint16_t>(rec + 2)));
        ds.set_u16(0x364, ds.u16(static_cast<std::uint16_t>(rec + 4)));
        ds.set_u16(0x366, ds.u16(static_cast<std::uint16_t>(rec + 6)));
        ds.set_u16(0x32E, ds.u16(static_cast<std::uint16_t>(rec + 12)));
        vm_->spawn_actor(ds.u16(static_cast<std::uint16_t>(rec + 8)), r, ds.u16(static_cast<std::uint16_t>(rec + 10)));
    }
}

// sub_140DC + sub_1407B: HUD state and its sprite parts. Part 0 and part 1
// are used as stamps for the inventory panel; parts 3-8 show Kyle's health
// with 16x16 icons (resource 0x4E) kept in the data segment at ds:57A6h.
void Game::hud_init() {
    DataSegment& ds = machine_->ds;
    ds.set_u16(0x100, 8);
    if (!has_hud()) return;
    ds.set_u16(0x100, 0);
    std::uint16_t next = static_cast<std::uint16_t>(ds.u16(0x1CB0) + 2);
    ds.set_u16(0x1CC4, next >= 0x10 ? 0 : next);
    ds.set_u16(0x1CC2, 3);
    for (int i = 0; i < 8; ++i) ds.set_u16(static_cast<std::uint16_t>(0x60A6 + 2 * i), 3);
    // sub_1402A: the inventory icon bank is resource 0x4C (its slot in ds:1152h).
    int bank = 0x1E;
    while (bank >= 0 && ds.u16(static_cast<std::uint16_t>(0x1152 + bank)) != 0x4C) bank -= 2;
    ds.set_u16(0x57A4, static_cast<std::uint16_t>(bank));
    // sub_1407B
    ds.set_u16(0x752, 0x5C26);
    ds.set_u16(0x852, Machine::kDataSeg);
    ds.set_u16(0x452, 2);
    ds.set_u16(0x952, 0x20);
    std::uint16_t seg = 0, off = 0;
    vm_->alloc_frame_buffer(0x4D, seg, off);
    ds.set_u16(0x754, off);
    ds.set_u16(0x854, seg);
    ds.set_u16(0xF54, 0xF0F0);
    ds.set_u16(0x454, 2);
    ds.set_u16(0x954, 0x20);
    std::uint16_t at = 0x57A6;
    for (std::uint8_t b : archive_.resource(0x4E)) ds.set_u8(at++, b);
    for (int si = 0xA; si >= 0; si -= 2) {
        ds.set_u16(static_cast<std::uint16_t>(0x858 + si), Machine::kDataSeg);
        ds.set_u16(static_cast<std::uint16_t>(0xF58 + si), 0x7070);
        ds.set_u16(static_cast<std::uint16_t>(0x458 + si), 0x300);
        ds.set_u16(static_cast<std::uint16_t>(0x958 + si), 0x10);
    }
}

void Game::hud_tick() {
    DataSegment& ds = machine_->ds;
    if (!has_hud()) return;
    // sub_1411C: [ and ] cycle the selected inventory slot.
    const std::uint16_t pressed = ds.u16(0x35C);
    if ((pressed & 0x3000) && (pressed & 0x3000) != 0x3000) {
        int sel = static_cast<std::int16_t>(ds.u16(0x1CB0));
        sel = (pressed & 0x2000) ? (sel + 2 >= 0x10 ? 0 : sel + 2) : (sel - 2 < 0 ? 0xE : sel - 2);
        ds.set_u16(0x1CB0, static_cast<std::uint16_t>(sel));
        vm_->events().push_back({VmEvent::Kind::PlaySound, 0x83});
    }
    // sub_14196: note inventory changes; skip empty slots when the selected one empties.
    for (int di = 0xE; di >= 0; di -= 2) {
        const std::uint16_t item = ds.u16(static_cast<std::uint16_t>(0x1CB2 + di));
        if (item == ds.u16(static_cast<std::uint16_t>(0x60B6 + di))) continue;
        ds.set_u16(static_cast<std::uint16_t>(0x60B6 + di), item);
        ds.set_u16(static_cast<std::uint16_t>(0x60A6 + di), ds.u16(static_cast<std::uint16_t>(0x60A6 + di)) | 3);
        if (di != ds.u16(0x1CB0)) continue;
        ds.set_u16(0x1CC2, ds.u16(0x1CC2) | 3);
        if (item) continue;
        int k = di;
        do {
            k = k + 2 >= 0x10 ? 0 : k + 2;
        } while (!ds.u16(static_cast<std::uint16_t>(0x1CB2 + k)) && k != ds.u16(0x1CB0));
        ds.set_u16(0x1CB0, static_cast<std::uint16_t>(k));
    }
    // sub_14176
    if (ds.u16(0x1CB0) != ds.u16(0x1CC4)) {  // both slots and the name are redrawn
        const std::uint16_t was = ds.u16(0x1CC4), now = ds.u16(0x1CB0);
        ds.set_u16(0x1CC4, now);
        ds.set_u16(0x1CC2, ds.u16(0x1CC2) | 3);
        for (std::uint16_t slot : {now, was})
            ds.set_u16(static_cast<std::uint16_t>(0x60A6 + slot), ds.u16(static_cast<std::uint16_t>(0x60A6 + slot)) | 3);
    }
    // sub_141DD: health parts at the top right of the playfield, 4 points each.
    const std::uint16_t cx = ds.u16(0x1B4E), cy = ds.u16(0x1B50);
    for (int si = 0xA, x = cx + 0x98; si >= 0; si -= 2, x += 0x10) {
        ds.set_u16(static_cast<std::uint16_t>(0x458 + si), ds.u16(static_cast<std::uint16_t>(0x458 + si)) & 0xFF7F);
        ds.set_u16(static_cast<std::uint16_t>(0x558 + si), static_cast<std::uint16_t>(x));
        ds.set_u16(static_cast<std::uint16_t>(0x658 + si), static_cast<std::uint16_t>(cy + 8));
    }
    int health = static_cast<std::int16_t>(ds.field(0x148A, 0));
    for (int si = 0; health > 0 && si < 0xC; si += 2) {
        const int n = health >= 4 ? 4 : health;
        health -= 4;
        ds.set_u16(static_cast<std::uint16_t>(0x758 + si), static_cast<std::uint16_t>((n - 1) * 0x120 + 0x57A6));
        ds.set_u16(static_cast<std::uint16_t>(0xE58 + si), 0x303);
        ds.set_u16(static_cast<std::uint16_t>(0x458 + si), ds.u16(static_cast<std::uint16_t>(0x458 + si)) | 0x80);
    }
}

void Game::tick() {
    if (!worker_.joinable()) worker_ = std::thread([this] { run_logic(); });
    else go_.release();
    done_.acquire();
    if (error_) {
        std::exception_ptr e = error_;
        error_ = nullptr;
        if (worker_.joinable()) worker_.join();
        std::rethrow_exception(e);
    }
}

// The worker: what the original does after loading a level (start and
// loc_1013A: two main loop iterations, then the fade in), then the main loop.
void Game::run_logic() {
    try {
        if (entry_ == Entry::AfterLoad) {
            ++nesting_;
            iteration();
            iteration();
            fade(true);
            --nesting_;
        }
        if (entry_ == Entry::AfterWait) {  // a save resumes behind the frame wait it was made at
            machine_->ds.set_u16(0x5312, 0);
            frame_tail();
        }
        entry_ = Entry::Loop;
        for (;;) iteration();
    } catch (const Stop&) {
    } catch (...) {
        error_ = std::current_exception();
        done_.release();
    }
}

void Game::wait_frame() {
    DataSegment& ds = machine_->ds;
    if (ds.s16(0x5312) <= 0) return;  // the retrace already came: no wait
    saveable_ = nesting_ == 0;
    retraces_ = ds.s16(0x5312);
    done_.release();
    go_.acquire();
    if (stopping_) throw Stop{};
    ds.set_u16(0x5312, 0);
}

// sub_1008C: the frame wait, then the per-frame work.
void Game::frame() {
    wait_frame();
    frame_tail();
}

// sub_128A1: once a fade has turned uploads on (ds:5315h), a pending palette
// change (ds:4A10h) is copied to the DAC.
void Game::upload_palette() {
    DataSegment& ds = machine_->ds;
    if (!ds.u8(0x5315) || !ds.u16(0x4A10)) return;
    ds.set_u16(0x4A10, 0);
    std::copy_n(ds.raw() + 0x4D12, machine_->dac.size(), machine_->dac.begin());
}

// sub_15BA0 draws the sprites, tiles and text on the page this frame shows.
void Game::draw_page() {
    // Cells under sprite parts and text are repainted from now on (the
    // original marks them dirty when it draws or erases there).
    Machine& m = *machine_;
    const DataSegment& ds = m.ds;
    const int cx = ds.s16(0x1B4E), cy = ds.s16(0x1B50);
    auto mark = [&](int x0, int y0, int x1, int y1) {
        for (int y = std::max(0, y0 >> 3); y <= std::min(27, y1 >> 3); ++y)
            for (int x = std::max(0, x0 >> 3); x <= std::min(31, x1 >> 3); ++x) m.repainted[static_cast<std::size_t>(y * 32 + x)] = 1;
    };
    for (int p = 0; p < 0x100; p += 2) {
        if (!(ds.field(0x452, p) & 0x80)) continue;
        const int size = ds.field(0x952, p);
        const int x = ds.s16(static_cast<std::uint16_t>(0x552 + p)) - cx, y = ds.s16(static_cast<std::uint16_t>(0x652 + p)) - cy;
        if (x + size <= 0 || y + size <= 0 || x >= 256 || y >= 224) continue;
        mark(x, y, x + size - 1, y + size - 1);
    }
    for (int c = 0; c < 32 * 28; ++c)
        if (ds.u16(static_cast<std::uint16_t>(0x2D08 + c * 2)) != 0x20) m.repainted[static_cast<std::size_t>(c)] = 1;
    if (!shown_) shown_ = std::make_unique<Machine>();
    *shown_ = *machine_;
    shown_has_background_ = has_background_;
}

// The rest of sub_1008C after the wait.
void Game::frame_tail() {
    DataSegment& ds = machine_->ds;
    upload_palette();
    cycle_palettes();
    update_shake();
    animate_tiles();
    draw_page();
    track_redraws();
    // sub_142D4 / sub_1423E: the HUD drawer counts down the per-page redraw
    // bits of the 8 inventory slots and of the item name.
    if (ds.u16(0x1CF6) & 1) {
        for (std::uint16_t di = 0; di < 0x10; di = static_cast<std::uint16_t>(di + 2))
            ds.set_u16(static_cast<std::uint16_t>(0x60A6 + di), static_cast<std::uint16_t>(ds.u16(static_cast<std::uint16_t>(0x60A6 + di)) >> 1));
        ds.set_u16(0x1CC2, static_cast<std::uint16_t>(ds.u16(0x1CC2) >> 1));
    }
    page_flip();
}

// sub_158FA (from sub_15BA0): the part drawer's bookkeeping for the page it
// draws. E52h counts pages still to draw a part on (low byte) and to erase
// its old image from (high byte); A52h/C52h are where it was last drawn and
// B52h/D52h where it was the page before. ds:60C6h > 0 redraws every part
// (a screen flip sets it to 3). Freed parts keep their slot until erased.
void Game::track_redraws() {
    DataSegment& ds = machine_->ds;
    const bool all = ds.u8(0x60C6) != 0;
    for (int p = 0xFE; p >= 0; p -= 2) {
        const auto at = [&](std::uint16_t base) { return static_cast<std::uint16_t>(base + p); };
        if (ds.u8(at(0xE53))) {
            ds.set_u8(at(0xE53), static_cast<std::uint8_t>(ds.u8(at(0xE53)) - 1));
            ds.set_u16(at(0xB52), ds.u16(at(0xA52)));
            ds.set_u16(at(0xD52), ds.u16(at(0xC52)));
        }
        if (!(ds.u16(at(0x452)) & 0x80)) continue;
        if (!all) {
            if (!ds.u8(at(0xE52))) continue;
            ds.set_u8(at(0xE52), static_cast<std::uint8_t>(ds.u8(at(0xE52)) - 1));
        }
        ds.set_u16(at(0xA52), ds.u16(at(0x552)));
        ds.set_u16(at(0xC52), ds.u16(at(0x652)));
    }
    // sub_15BA0 only counts the flag down when it is already 0, so after the
    // first frame it stays non-zero and every part is redrawn every frame.
    if (!all) ds.set_u8(0x60C6, 0xFF);
}

// sub_1283B: swap the pages and count the vertical retraces to wait for.
void Game::page_flip() {
    DataSegment& ds = machine_->ds;
    ds.set_u16(0x4A0C, ds.u16(0x4A0C) ^ 2);
    const std::uint16_t a = ds.u16(0x4A08);
    ds.set_u16(0x4A08, ds.u16(0x4A0A));
    ds.set_u16(0x4A0A, a);
    const int wait = ds.s16(0x5312) + ds.s16(0x68);
    ds.set_u16(0x5312, static_cast<std::uint16_t>(wait > 0 ? wait : 1));
}

// sub_10A80 / sub_10A93: latch the input word, newly pressed bits, accumulated presses.
void Game::latch_input() {
    DataSegment& d = machine_->ds;
    const std::uint16_t now = d.u16(0x2A82);
    d.set_u16(0x35A, now);
    const std::uint16_t pressed = static_cast<std::uint16_t>((now ^ d.u16(0x360)) & now);
    d.set_u16(0x35C, pressed);
    d.set_u16(0x35E, static_cast<std::uint16_t>(d.u16(0x35E) | pressed));
    d.set_u16(0x360, now);
}

// sub_10043: one pass of the main loop; it falls through into sub_1008C.
void Game::iteration() {
    DataSegment& d = machine_->ds;
    d.set_u16(0x31A, static_cast<std::uint16_t>(d.u16(0x31A) + 1));
    latch_input();
    // loc_12A12: P opens the "give up?" menu.
    if (!(d.u16(0x1CF6) & 0x20) && !(d.u16(0x318) & 3) && (d.u16(0x35C) & 0x4000)) menu(false);
    typed_cheat();   // sub_190C1
    level_frames();  // sub_1011B
    vm_->run_scheduler();  // sub_1601A
    // sub_1290E: Esc opens "quit to main menu?" (or leaves special scenes).
    if ((d.u16(0x35C) & 2) && !(d.u16(0x318) & 3)) {
        if (d.u16(0x1CF6) & 0x20) {
            if (d.u16(0x1CA4) != 0x6E) {
                d.set_u16(0x20A, 1);
                d.set_u16(0x1CA8, 0x6E);
                d.set_u16(0x318, d.u16(0x318) | 1);
            }
        } else {
            menu(true);
        }
    }
    blink_parts();  // sub_10179
    vm_->integrate_velocity();
    vm_->run_check_pass(+1);
    vm_->apply_anchors();
    vm_->run_check_pass(-1);
    vm_->move_parts();
    vm_->mark_offscreen_actors();
    if (camera_needs_flip()) flip_screen();  // sub_100B6
    hud_tick();  // sub_1411C, sub_14164
    d.set_u16(0x31A, static_cast<std::uint16_t>(d.u16(0x31A) + 1));
    frame();
}

// sub_100B6 (loc_100F5): Kyle left the screen. The new screen is set up and
// a whole nested main loop pass runs before this one finishes.
void Game::flip_screen() {
    ++nesting_;
    place_camera();
    vm_->mark_offscreen_actors();
    vm_->run_scheduler(true);  // sub_15FF1: actors that just left the view (they usually despawn)
    DataSegment& ds = machine_->ds;
    vm_->spawn_visible_records();
    ds.set_u8(0x60C6, 3);  // byte_15FC6: redraw everything on the next pages
    machine_->repainted.fill(0);  // sub_125BB redraws the whole screen
    iteration();
    --nesting_;
}

// sub_10179: while ds:258h counts down, the parts from ds:198Ah (ds:19B2h of
// them) blink every other two frames.
void Game::blink_parts() {
    DataSegment& ds = machine_->ds;
    const int t = ds.s16(0x258) - 1;
    ds.set_u16(0x258, static_cast<std::uint16_t>(t));
    if (t < 0) {
        ds.set_u16(0x258, 0);
        return;
    }
    const bool hide = t & 2;
    std::uint16_t si = ds.u16(0x198A);
    for (int n = ds.u16(0x19B2); n > 0; --n, si = static_cast<std::uint16_t>(si + 2)) {
        const std::uint16_t a = static_cast<std::uint16_t>(si + 0x452);
        ds.set_u16(a, hide ? static_cast<std::uint16_t>(ds.u16(a) | 0x40) : static_cast<std::uint16_t>(ds.u16(a) & ~0x40));
        ds.set_u16(static_cast<std::uint16_t>(si + 0xE52), hide ? 0x200 : 0x202);
    }
}

// sub_1011B: ds:318h bit 0 changes level, bit 2 pauses until a key, bit 1 is
// cleared.
void Game::level_frames() {
    DataSegment& ds = machine_->ds;
    const std::uint16_t f = ds.u16(0x318);
    if (f & 1) {
        change_level();
    } else if (f & 4) {
        ds.set_u16(0x318, static_cast<std::uint16_t>(f & ~4));
        ++nesting_;
        do {
            frame();
            latch_input();
        } while (!(ds.u16(0x35C) & 0xCFC3));
        settle_input();
        ds.set_u16(0x35E, 0);
        --nesting_;
    } else if (f & 2) {
        ds.set_u16(0x318, static_cast<std::uint16_t>(f & ~2));
    }
}

// sub_1266C: the shown palette is the base palette plus the tint (ds:320h-324h).
void Game::retint() { vm_->retint_range(0, 256); }

// sub_1270D / sub_12752: two frames of fading, through tint 46h.
void Game::fade(bool in) {
    DataSegment& ds = machine_->ds;
    ++nesting_;
    if (!in) ds.set_u16(0x326, 0xFFFF);
    const std::array<int, 2> steps = in ? std::array<int, 2>{0x46, 0} : std::array<int, 2>{0, 0x46};
    for (int bx : steps) {
        for (int k = 0; k < 3; ++k) ds.set_u16(static_cast<std::uint16_t>(0x320 + 2 * k), static_cast<std::uint16_t>(bx));
        retint();
        ds.set_u16(0x4A10, 2);
        ds.set_u8(0x5315, 1);
        page_flip();
        wait_frame();
        upload_palette();
    }
    for (int k = 0; k < 3; ++k) ds.set_u16(static_cast<std::uint16_t>(0x320 + 2 * k), 0);
    if (!in) ds.set_u8(0x5315, 0);
    --nesting_;
}

// loc_1296F / loc_12A4C: darken the palette (unless a tint is active), show
// the question, and start with the second answer selected.
void Game::open_menu(bool quit_menu) {
    DataSegment& ds = machine_->ds;
    menu_ = {true, quit_menu, false};
    if (!(ds.u16(0x320) | ds.u16(0x322) | ds.u16(0x324))) {
        for (int k = 0; k < 3; ++k) ds.set_u16(static_cast<std::uint16_t>(0x320 + 2 * k), 4);
        vm_->retint_range(0, 256);
        menu_.darkened = true;
    }
    const std::uint16_t first = quit_menu ? 0x90 : 0x8D;
    for (int k = 0; k < 3; ++k) ds.set_u16(static_cast<std::uint16_t>(0x5781 + 10 * k), static_cast<std::uint16_t>(first + k));
    if (ds.u16(0x380)) vm_->events().push_back({VmEvent::Kind::MidiEffect, ds.u16(0xAC)});
    ds.set_u16(0x1B46, 0);
    ds.set_u16(0x370, 0);
    vm_->show_text_block(0x5780);
}

// sub_12AF5: Left / Right pick an answer, the choice blinks every 12 frames;
// Enter, Esc or Fire closes the menu.
// The menu loop: a frame per step until Enter, Esc or Fire.
void Game::menu(bool quit_menu) {
    DataSegment& ds = machine_->ds;
    ++nesting_;
    open_menu(quit_menu);
    frame();
    // The second answer starts selected; the choice blinks every 12 frames.
    ds.set_u16(0x5A, 0x5794);
    ds.set_u16(0x5C, 0x5794);
    ds.set_u16(0x5E, 0x5780);
    ds.set_u16(0x60, 0xC);
    ds.set_u16(0x64, 0);
    do {
        menu_select();
        frame();
        latch_input();
    } while (!(ds.u16(0x35C) & 0x43));
    vm_->clear_text();
    frame();
    menu_finish();
    --nesting_;
}

void Game::menu_finish() {
    DataSegment& ds = machine_->ds;
    const std::uint16_t pressed = ds.u16(0x35C);
    const bool yes = !(pressed & 2) && ds.u16(0x5C) == 0x578A;
    if (yes && menu_.quit_menu) {
        ds.set_u16(0x20A, 1);
        ds.set_u16(0x1CA8, 0x6E);
        ds.set_u16(0x318, ds.u16(0x318) | 1);
    } else if (yes) {
        vm_->apply_password();
        ds.set_u16(0x318, ds.u16(0x318) | 1);
        ds.set_u16(0x1CF6, ds.u16(0x1CF6) | 0x10);
    }
    if (menu_.darkened) {  // sub_16355
        for (int k = 0; k < 3; ++k) ds.set_u16(static_cast<std::uint16_t>(0x320 + 2 * k), 0);
        retint();
    }
    settle_input();
    ds.set_u16(0x35E, 0);
    menu_.active = false;
}

// sub_12AF5: Left / Right pick an answer, the choice blinks every 12 frames.
void Game::menu_select() {
    DataSegment& ds = machine_->ds;
    const std::uint16_t pressed = ds.u16(0x35C);
    bool redraw = false;
    auto choose = [&](std::uint16_t answer) {
        if (ds.u16(0x5C) == answer) return;
        ds.set_u16(0x5A, answer);
        ds.set_u16(0x5C, answer);
        ds.set_u16(0x5E, 0x5780);
        ds.set_u16(0x60, 0xD);
        vm_->events().push_back({VmEvent::Kind::PlaySound, ds.u16(0x83)});
        redraw = true;
    };
    if (pressed & 0x10) choose(0x578A);
    else if (pressed & 0x20) choose(0x5794);
    if (!redraw) {
        const std::uint16_t t = static_cast<std::uint16_t>(ds.u16(0x60) - 1);
        ds.set_u16(0x60, t);
        if (t != 0) return;
        ds.set_u16(0x60, 0xC);
        const std::uint16_t a = ds.u16(0x5E);
        ds.set_u16(0x5E, ds.u16(0x5A));
        ds.set_u16(0x5A, a);
    }
    vm_->show_text_block(ds.u16(0x5E));
}
// sub_186C4: palette cycles enabled in the mask ds:1B86h. A range rotates one
// colour per step (direction by its ends); a single colour steps through a
// list of 5:5:5 values.
void Game::cycle_palettes() {
    DataSegment& ds = machine_->ds;
    auto rgb = [&](std::uint16_t base, int c) { return static_cast<std::uint16_t>(base + c * 3); };
    auto move3 = [&](std::uint16_t dst, std::uint16_t src) {
        for (int k = 0; k < 3; ++k) ds.set_u8(static_cast<std::uint16_t>(dst + k), ds.u8(static_cast<std::uint16_t>(src + k)));
    };
    for (int si = 0xE; si >= 0; si -= 2) {
        if (!(ds.u16(0x1B86) & ds.u16(static_cast<std::uint16_t>(0x288A + si)))) continue;
        const std::uint16_t cnt = static_cast<std::uint16_t>(ds.u16(static_cast<std::uint16_t>(0x1BA8 + si)) - 1);
        ds.set_u16(static_cast<std::uint16_t>(0x1BA8 + si), cnt);
        if (cnt != 0) continue;
        const int first = ds.u16(static_cast<std::uint16_t>(0x1BB8 + si)), last = ds.u16(static_cast<std::uint16_t>(0x1BC8 + si));
        if (last > first) {  // sub_18703: rotate the range up by one colour
            for (std::uint16_t base : {std::uint16_t{0x4A12}, std::uint16_t{0x4D12}}) {
                std::uint8_t keep[3];
                for (int k = 0; k < 3; ++k) keep[k] = ds.u8(static_cast<std::uint16_t>(rgb(base, last) + k));
                for (int c = last; c > first; --c) move3(rgb(base, c), rgb(base, c - 1));
                for (int k = 0; k < 3; ++k) ds.set_u8(static_cast<std::uint16_t>(rgb(base, first) + k), keep[k]);
            }
        } else if (last < first) {  // sub_18758: rotate down
            for (std::uint16_t base : {std::uint16_t{0x4A12}, std::uint16_t{0x4D12}}) {
                std::uint8_t keep[3];
                for (int k = 0; k < 3; ++k) keep[k] = ds.u8(static_cast<std::uint16_t>(rgb(base, last) + k));
                for (int c = last; c < first; ++c) move3(rgb(base, c), rgb(base, c + 1));
                for (int k = 0; k < 3; ++k) ds.set_u8(static_cast<std::uint16_t>(rgb(base, first) + k), keep[k]);
            }
        } else {  // sub_187AD: rotate the colour through its list, as 5:5:5
            const std::uint16_t c = rgb(0x4A12, last);
            std::uint16_t v = static_cast<std::uint16_t>(((ds.u8(c) & 0x3E) >> 1) | ((ds.u8(static_cast<std::uint16_t>(c + 1)) & 0x3E) << 4) |
                                                         ((ds.u8(static_cast<std::uint16_t>(c + 2)) & 0x3E) << 9));
            for (std::uint16_t d = ds.u16(static_cast<std::uint16_t>(0x1BD8 + si)); ds.u16(d) != 0xFFFF; d = static_cast<std::uint16_t>(d + 2)) {
                const std::uint16_t t = ds.u16(d);
                ds.set_u16(d, v);
                v = t;
            }
            ds.set_u8(c, static_cast<std::uint8_t>((v << 1) & 0x3E));
            ds.set_u8(static_cast<std::uint16_t>(c + 2), static_cast<std::uint8_t>((v >> 9) & 0x3E));
            ds.set_u8(static_cast<std::uint16_t>(c + 1), static_cast<std::uint8_t>((v >> 4) & 0x3E));
            vm_->retint_range(static_cast<std::uint16_t>(last * 3), 1);  // sub_12671 for this one colour (byte offset)
        }
        ds.set_u16(static_cast<std::uint16_t>(0x1BA8 + si), ds.u16(static_cast<std::uint16_t>(0x1B98 + si)));
    }
    ds.set_u16(0x4A10, 2);  // the palette goes to the DAC at the next frame (sub_128A1)
}

// sub_10AC1: screen shake offsets ds:344h (x) and ds:346h (y).
void Game::update_shake() {
    DataSegment& ds = machine_->ds;
    for (int axis = 0; axis < 2; ++axis) {
        const std::uint16_t ticks = static_cast<std::uint16_t>(0x348 + 2 * axis), mask = static_cast<std::uint16_t>(0x340 + 2 * axis);
        const std::uint16_t off = static_cast<std::uint16_t>(0x344 + 2 * axis);
        if (ds.u16(ticks) && (ds.set_u16(ticks, static_cast<std::uint16_t>(ds.u16(ticks) - 1)), ds.u16(mask)))
            ds.set_u16(off, ds.u16(off) ^ ds.u16(mask));
        else
            ds.set_u16(off, 0);
    }
}

// sub_18822: tile animations enabled in ds:1BE8h copy their next frame into
// the tileset (pixels) and the tile masks.
void Game::animate_tiles() {
    DataSegment& ds = machine_->ds;
    for (int bp = 0xE; bp >= 0; bp -= 2) {
        if (!(ds.u16(0x1BE8) & ds.u16(static_cast<std::uint16_t>(0x288A + bp)))) continue;
        const std::uint16_t a = static_cast<std::uint16_t>(0x1BFA + bp);
        const int delay = static_cast<std::int16_t>(static_cast<std::uint16_t>(ds.u16(a) - 1));
        ds.set_u16(a, static_cast<std::uint16_t>(delay));
        if (delay > 0) continue;
        if (delay < 0) {
            std::uint16_t v = ds.u16(static_cast<std::uint16_t>(0x1BEA + bp));
            if (static_cast<std::int16_t>(v) < 0) v = static_cast<std::uint16_t>((vm_->random() & v & 0x7F) + 2);
            ds.set_u16(a, v);
        }
        const std::uint16_t first = ds.u16(static_cast<std::uint16_t>(0x1C2A + bp));
        const int count = ds.u16(static_cast<std::uint16_t>(0x1C3A + bp));
        std::uint16_t src = static_cast<std::uint16_t>(0x48 * count * ds.u16(static_cast<std::uint16_t>(0x1C1A + bp)) + ds.u16(static_cast<std::uint16_t>(0x1C4A + bp)));
        std::uint16_t di = first, mi = static_cast<std::uint16_t>(first >> 3);
        for (int n = 0; n < count * 8; ++n) {
            machine_->tile_masks[mi++ & 0x1FFF] = machine_->tile_anims[src++];
            for (int k = 0; k < 8; ++k) machine_->tiles[di++] = machine_->tile_anims[src++];
        }
        if (ds.u16(a) != 0) {
            int f = ds.u16(static_cast<std::uint16_t>(0x1C1A + bp)) + 1;
            if (f >= ds.u16(static_cast<std::uint16_t>(0x1C0A + bp))) f = 0;
            ds.set_u16(static_cast<std::uint16_t>(0x1C1A + bp), static_cast<std::uint16_t>(f));
        }
    }
}

// sub_1011B: on a level exit every actor's script restarts once (ds:316h),
// then the level in ds:1CA8h loads with Kyle at (ds:1CAAh, ds:1CACh).
// sub_1011B, loc_1013A: fade out, load the next level, run two passes and
// fade in; the interrupted main loop pass then carries on.
void Game::change_level() {
    DataSegment& ds = machine_->ds;
    ++nesting_;
    ds.set_u16(0x318, 0);
    music_action(ds.u8(0x1CF4));  // sub_11C6B: the exit action
    fade(false);
    ds.set_u16(0x316, static_cast<std::uint16_t>(ds.u16(0x316) + 1));
    vm_->run_scheduler();
    // sub_11D00: unless the music is kept, sound effects stop (sub_195C7)
    // and the music counts as not playing; stopping waits ten frames for
    // the music to fade.
    const int exit_action = ds.u8(0x1CF4);
    if (exit_action != 1) {
        if (exit_action == 2)
            for (int k = 0; k < 10; ++k) {
                ds.set_u16(0x5312, 1);
                wait_frame();
            }
        vm_->events().push_back({VmEvent::Kind::StopAllSounds, 0});
        ds.set_u16(0x38E, 0);
    }
    load_level_data(ds.u16(0x1CA8), static_cast<std::int16_t>(ds.u16(0x1CAA)), static_cast<std::int16_t>(ds.u16(0x1CAC)));
    iteration();
    iteration();
    fade(true);
    --nesting_;
}
}  // namespace bt
