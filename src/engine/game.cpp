#include "engine/game.h"

#include <algorithm>
#include <stdexcept>

namespace bt {
namespace {

constexpr std::uint16_t kDescriptor = 0x1CEE;
constexpr std::uint16_t kBasePalette = 0x4A12, kShownPalette = 0x4D12;

// Globals the level loader resets (the zeroing block at the top of sub_105B4).
constexpr std::uint16_t kResetGlobals[] = {
    0x000, 0x002, 0x004, 0x312, 0x30E, 0x310, 0x34C, 0x390, 0x316, 0x32C, 0x358, 0x35A, 0x35C, 0x35E, 0x362,
    0x1A82, 0x340, 0x342, 0x344, 0x346, 0x348, 0x34A, 0x31A, 0x1B6E, 0x1B70, 0x1B72, 0x1B74, 0x1B76, 0x1B78,
    0x1B7A, 0x1B7C, 0x320, 0x322, 0x324,
};

}  // namespace

Game::Game(const Archive& archive, const std::filesystem::path& exe_path, int language)
    : archive_(archive), machine_(std::make_unique<Machine>()) {
    machine_->ds = DataSegment(exe_path);
    machine_->scripts = archive.entry(64);
    machine_->text = archive.entry(1 + std::clamp(language, 0, 2));
    // sub_19046 (startup): unpack the sprite palettes listed at ds:6A78h, in
    // order, into the table at ds:6ABCh that palette slots copy from.
    DataSegment& ds = machine_->ds;
    std::uint16_t dst = 0x6ABC;
    for (std::uint16_t a = 0x6A78; ds.u16(a) != 0xFFFF; a = static_cast<std::uint16_t>(a + 2))
        for (std::uint8_t b : archive.resource(ds.u16(a))) ds.set_u8(dst++, b);
    vm_ = std::make_unique<Vm>(*machine_);
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
    // sub_128E9: Kyle's starting flags and the first level.
    ds.set_field(0x1552, 0, 0);
    ds.set_field(field::kFlags, 0, 0x27);
    load_level(0x61, 0x80, 0x70);
}

void Game::load_level(int level_id, int x, int y) {
    DataSegment& ds = machine_->ds;
    // sub_10A15 spawns Kyle with the parameter and low flag byte he had before.
    const std::uint16_t kyle_param = ds.field(0x1552, 0);
    const std::uint16_t kyle_flags = static_cast<std::uint16_t>(ds.field(field::kFlags, 0) & 0xFF);
    for (std::uint16_t a : kResetGlobals) ds.set_u16(a, 0);
    ds.set_u16(0x1C8A, 0x1000);  // velocity limits
    ds.set_u16(0x1C8C, 0x1000);
    ds.set_u16(0x326, 0xFFFF);
    for (std::uint16_t a = 0x452; a < 0x452 + 0xDE0; a = static_cast<std::uint16_t>(a + 2)) ds.set_u16(a, 0);  // sub_11F38
    ds.set_u16(0x259E, Machine::kFrameSeg);
    ds.set_u16(0x25A0, Machine::kStaticSeg);
    machine_->frames.fill(0);
    machine_->statics.fill(0);
    machine_->map.fill(0);

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

    // The shown palette starts as the untinted base palette.
    for (int i = 0; i < 768; ++i) ds.set_u8(static_cast<std::uint16_t>(kShownPalette + i), ds.u8(static_cast<std::uint16_t>(kBasePalette + i)));

    // sub_10A15: Kyle is actor type 0 in slot 0.
    ds.set_u16(0x31E, 0);
    ds.set_u16(0x26, static_cast<std::uint16_t>(x));
    ds.set_u16(0x28, static_cast<std::uint16_t>(y));
    ds.set_u16(0x32E, kyle_param);
    ds.set_u16(ds_addr::kCurrentSlot, 0xFFFF);
    ds.set_u16(0x364, 0xFFFF);
    if (vm_->spawn_actor(0, 0xFFFF, kyle_flags) != 0) throw std::runtime_error("could not spawn Kyle");
    ds.set_u16(0x31E, 2);

    set_camera_bounds();
    place_camera();
    hud_init();
    music_action();
    spawn_start_records();
    vm_->spawn_visible_records();
}

// sub_11C64: header byte 4 picks what happens to the music; byte 5 is the song.
// 0 = play the song (sub_11CC2), 1 = keep, 2 = stop (sub_11CF6), 3 = load only.
void Game::music_action() {
    DataSegment& ds = machine_->ds;
    auto& ev = vm_->events();
    const int action = ds.u8(0x1CF2), song = ds.u8(0x1CF3);
    auto load = [&] {  // sub_11C77
        if (song == ds.u16(0x386)) return;
        ds.set_u16(0x386, static_cast<std::uint16_t>(song));
        ds.set_u16(0x38E, 0);
        ev.push_back({VmEvent::Kind::MusicLoad, song});
    };
    switch (action) {
        case 0:
            load();
            if (ds.u16(0x37E) && (song != ds.u16(0x38A) || ds.u16(0x38E) == 0)) {
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
        ds.set_u16(static_cast<std::uint16_t>(0x1C5A + si), 0);
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
    if (ds.u16(0x1CB0) != ds.u16(0x1CC4)) {
        ds.set_u16(0x1CC4, ds.u16(0x1CB0));
        ds.set_u16(0x1CC2, ds.u16(0x1CC2) | 3);
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

void Game::main_loop_actors() {
    vm_->run_scheduler();
    vm_->integrate_velocity();
    vm_->run_check_pass(+1);
    vm_->apply_anchors();
    vm_->run_check_pass(-1);
    vm_->move_parts();
    vm_->mark_offscreen_actors();
}

void Game::tick() {
    // sub_10A80 / sub_10A93: latch the input word, newly pressed bits, accumulated presses.
    DataSegment& d = machine_->ds;
    const std::uint16_t now = d.u16(0x2A82);
    d.set_u16(0x35A, now);
    const std::uint16_t pressed = static_cast<std::uint16_t>((now ^ d.u16(0x360)) & now);
    d.set_u16(0x35C, pressed);
    d.set_u16(0x35E, static_cast<std::uint16_t>(d.u16(0x35E) | pressed));
    d.set_u16(0x360, now);
    main_loop_actors();
    if (camera_needs_flip()) {  // sub_100B6
        place_camera();
        vm_->mark_offscreen_actors();
        // sub_15FF1: run actors that just left the view (they usually despawn).
        DataSegment& ds = machine_->ds;
        for (int s = 0; s < ds.s16(ds_addr::kActorEnd); s += 2)
            if (ds.field(field::kFlags, s) & 0x200) vm_->schedule_actor(s);
        vm_->spawn_visible_records();
        main_loop_actors();
    }
    hud_tick();  // sub_1411C, sub_14164 close the main loop
    // The main loop falls through into the frame routine sub_1008C.
    cycle_palettes();
    update_shake();
    animate_tiles();
    if (machine_->ds.u16(0x318) & 1) change_level();
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
            vm_->retint_range(c, 1);
        }
        ds.set_u16(static_cast<std::uint16_t>(0x1BA8 + si), ds.u16(static_cast<std::uint16_t>(0x1B98 + si)));
    }
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
void Game::change_level() {
    DataSegment& ds = machine_->ds;
    ds.set_u16(0x318, 0);
    ds.set_u16(0x316, static_cast<std::uint16_t>(ds.u16(0x316) + 1));
    vm_->run_scheduler();
    load_level(ds.u16(0x1CA8), static_cast<std::int16_t>(ds.u16(0x1CAA)), static_cast<std::int16_t>(ds.u16(0x1CAC)));
    main_loop_actors();
    main_loop_actors();
}

}  // namespace bt
