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
    spawn_start_records();
    vm_->spawn_visible_records();
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

    // sub_10802: background objects (not loaded yet; their graphics go to a
    // separate segment the renderer will use later).
    ds.set_u16(0x1BE8, u16(o));
    o = static_cast<std::uint16_t>(o + 2);
    while (ds.u8(o) != 0) o = static_cast<std::uint16_t>(o + 8);
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
    if (machine_->ds.u16(0x318) & 1) change_level();
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
