// Actor lifetime (spawn, despawn, spawn records), event checks, collision
// response, actor scans and the remaining actor opcodes.
// Semantics: docs/vm/ops_00-3F.md, ops_80-BF.md, ops_C0-FF.md.

#include <algorithm>
#include <stdexcept>

#include "engine/vm.h"

namespace bt {
namespace {

namespace g {
constexpr std::uint16_t kTmp10 = 0x10, kTmp12 = 0x12, kTmp14 = 0x14, kTmp16 = 0x16, kTmp18 = 0x18;
constexpr std::uint16_t kScratchX = 0x26, kScratchY = 0x28;
constexpr std::uint16_t kSchedSlot = 0x2C;     // slot the scheduler is running
constexpr std::uint16_t kFirstSpawnSlot = 0x31E;
constexpr std::uint16_t kSpawnParam = 0x32E;   // also the part-pool selector during a spawn
constexpr std::uint16_t kQueueLen = 0x330, kQueue = 0x332;
constexpr std::uint16_t kCheckOrdinal = 0x33C, kPass = 0x33E;
constexpr std::uint16_t kHotX = 0x364, kHotY = 0x366;
constexpr std::uint16_t kSpawnBlocked = 0x316;
constexpr std::uint16_t kCameraX = 0x1B4E, kCameraY = 0x1B50;
constexpr std::uint16_t kSmallPart = 0x350, kLargePart = 0x352;
constexpr std::uint16_t kStaticIds = 0x11B2, kStaticOffsets = 0x11F2;
constexpr std::uint16_t kFrameSlots = 0x1CAE;  // pointer to {offset, size code} pairs
constexpr std::uint16_t kFrameSeg = 0x259E, kStaticSeg = 0x25A0;
constexpr std::uint16_t kPalIds = 0x1C7A, kPalRefs = 0x1C6A;
constexpr std::uint16_t kPalTable = 0x6A78, kPalData = 0x6ABC;
constexpr std::uint16_t kBasePalette = 0x4A12, kShownPalette = 0x4D12;
constexpr std::uint16_t kTint = 0x320, kTintMode = 0x326;
constexpr std::uint16_t kFirstPart = 0x34, kPartCount = 0x36;
constexpr std::uint16_t kHitList = 0x1A02;
constexpr std::uint16_t kScanX = 0x44, kScanY = 0x46, kScanSel = 0x48, kScanMask = 0x4A, kScanDir = 0x4C;
constexpr std::uint16_t kScanSlot = 0x5A, kScanMatch = 0x5C, kScanEdge = 0x5E, kScanStart = 0x60;
}  // namespace g

namespace f {
constexpr std::uint16_t kSubX = 0x1282, kSubY = 0x12AA, kPrevX = 0x12D2, kPrevY = 0x12FA;
constexpr std::uint16_t kEventBits = 0x1322, kMoveMode = 0x134A;
constexpr std::uint16_t kBoxW = 0x1372, kBoxH = 0x139A, kBoxKind = 0x148A;
constexpr std::uint16_t kClassId = 0x14B2, kClassMask = 0x14DA;
constexpr std::uint16_t kVelX = 0x1502, kVelY = 0x152A, kParam = 0x1552, kTimer = 0x15CA;
constexpr std::uint16_t kParent = 0x166A, kChild = 0x1692;
constexpr std::uint16_t kFrameBuffer = 0x16BA, kFrameSeg = 0x18C2, kFrame = 0x1782;
constexpr std::uint16_t kF170A = 0x170A, kF1732 = 0x1732, kF16E2 = 0x16E2, kF175A = 0x175A;
constexpr std::uint16_t kMoveX = 0x17AA, kMoveY = 0x17D2;
constexpr std::uint16_t kAnimIp = 0x18EA, kAnimSeg = 0x1912;
constexpr std::uint16_t kFirstPart = 0x198A, kPartCount = 0x19B2;
}  // namespace f

namespace part {
constexpr std::uint16_t kFlags = 0x452, kX = 0x552, kA52 = 0xA52, kB52 = 0xB52, kSize = 0x952;
constexpr std::uint16_t kRedraw = 0xE52, kColour = 0xF52;
}  // namespace part

constexpr std::uint16_t kFlipV = 0x80;
constexpr int kMaxSlot = 0x28;

}  // namespace

void Vm::register_actor_ops() {
    table_[0x0E] = &Vm::op_despawn_self;
    table_[0x10] = &Vm::op_spawn;
    table_[0x17] = &Vm::op_event_overlap<true, false>;   // class id, Kyle only
    table_[0x1A] = &Vm::op_event_overlap<true, true>;    // class mask, Kyle only
    table_[0x2F] = &Vm::op_event_overlap<false, false>;  // class id, any actor
    table_[0x30] = &Vm::op_event_overlap<false, true>;   // class mask, any actor
    table_[0x2D] = &Vm::op_event_terrain<0>;
    table_[0x2E] = &Vm::op_event_terrain<1>;
    table_[0x33] = &Vm::op_event_terrain<2>;
    table_[0xC0] = &Vm::op_if_onscreen<false, false>;
    table_[0xC1] = &Vm::op_if_onscreen<true, false>;
    table_[0xC2] = &Vm::op_if_onscreen<false, true>;
    table_[0xC3] = &Vm::op_if_onscreen<true, true>;
    table_[0xCC] = &Vm::op_scan<false>;
    table_[0xF8] = &Vm::op_scan<true>;
    table_[0xCD] = &Vm::op_indexed_store;
    table_[0xCE] = &Vm::op_indexed_load;
    table_[0xE3] = &Vm::op_spawn_relative;
    table_[0xE5] = &Vm::op_spawn_record;
    table_[0xEF] = &Vm::op_find_overlap<true>;
    table_[0xF0] = &Vm::op_find_overlap<false>;
    table_[0xF1] = &Vm::op_palette;
    table_[0xF2] = &Vm::op_forget_record;
}

// ---- spawning -------------------------------------------------------------

// sub_11F9E: frame buffer for a type's bank field. 0xFFFF: static segment at
// offset 0; 0xFFFE: a free slot of matching size from the level list; else a
// preloaded static sprite by resource id.
bool Vm::alloc_frame_buffer(std::uint16_t bank, std::uint16_t& seg, std::uint16_t& off) {
    if (bank == 0xFFFE) {
        const std::uint16_t want = ds_.u16(g::kTmp10);
        for (std::uint16_t e = ds_.u16(g::kFrameSlots); ds_.u16(e) != 0xFFFF; e = static_cast<std::uint16_t>(e + 4)) {
            if (ds_.u16(static_cast<std::uint16_t>(e + 2)) == want) {
                seg = ds_.u16(g::kFrameSeg);
                off = ds_.u16(e);
                ds_.set_u16(static_cast<std::uint16_t>(e + 2), 0x8000);
                return true;
            }
        }
        return false;
    }
    seg = ds_.u16(g::kStaticSeg);
    off = 0;
    if (bank == 0xFFFF) return true;
    for (int di = 0x3E; di >= 0; di -= 2) {
        if (ds_.u16(static_cast<std::uint16_t>(g::kStaticIds + di)) == bank) {
            off = ds_.u16(static_cast<std::uint16_t>(g::kStaticOffsets + di));
            break;
        }
    }
    return true;
}

// sub_116A8 / loc_11652: find `count` consecutive free sprite parts.
bool Vm::alloc_parts(int count, int& first) {
    auto is_free = [&](int p, int base) {
        return !(ds_.u16(static_cast<std::uint16_t>(base + p)) & 0x80) &&
               ds_.u16(static_cast<std::uint16_t>(base + 0xA00 + p)) == 0;
    };
    const int mode = ds_.u16(g::kSpawnParam);
    if (mode >= 2) {  // top-down from 0x100 to 0x34 (arrays read at 450h/E50h)
        for (int di = 0x100; di >= 0x34;) {
            if (!is_free(di, 0x450)) { di -= 2; continue; }
            const int top = di, stop = di - 2 * count;
            int k = di;
            bool ok = true;
            while (true) {
                k -= 2;
                if (k == stop) break;
                if (k == 0x34) return false;
                if (!is_free(k, 0x450)) { ok = false; break; }
            }
            if (ok) {
                first = stop;
                (void)top;
                return true;
            }
            di = k;
        }
        return false;
    }
    const int start = mode == 1 ? 0x12 : 0x32;
    const int end = 0xE0;
    for (int di = start; di < end;) {
        if (!is_free(di, 0x452)) { di += 2; continue; }
        const int run_end = di + 2 * count;
        int k = di;
        bool ok = true;
        while (true) {
            k += 2;
            if (k == run_end) break;
            if (k == end) return false;
            if (!is_free(k, 0x452)) { ok = false; break; }
        }
        if (ok) {
            first = di;
            return true;
        }
        di = k;
    }
    return false;
}

void Vm::set_part_size(int p, bool large) {
    ds_.set_field(part::kSize, p, ds_.u16(large ? g::kLargePart : g::kSmallPart));
    const std::uint16_t fl = ds_.field(part::kFlags, p);
    ds_.set_field(part::kFlags, p, large ? (fl | 2) : (fl & ~2));
    ds_.set_field(part::kRedraw, p, 0x202);
}

// sub_1115D + loc_117B7: create an actor of `type` at (ds:26h, ds:28h).
// Returns the slot or -1.
int Vm::spawn_actor(int type, int record, std::uint16_t flags) {
    ds_.set_u16(g::kTmp12, static_cast<std::uint16_t>(type));
    ds_.set_u16(g::kTmp14, static_cast<std::uint16_t>(record));
    ds_.set_u16(g::kTmp16, flags);
    if (ds_.u16(g::kSpawnBlocked) != 0) return -1;
    int s = ds_.u16(g::kFirstSpawnSlot);
    while (s < kMaxSlot && ds_.field(field::kScriptSeg, s) != 0) s += 2;
    if (s >= kMaxSlot) return -1;

    const std::size_t rec = static_cast<std::size_t>(type) * 16;
    auto b8 = [&](std::size_t o) { return static_cast<std::uint16_t>(scripts_.at(rec + o)); };
    auto b16 = [&](std::size_t o) { return static_cast<std::uint16_t>(scripts_.at(rec + o) | (scripts_.at(rec + o + 1) << 8)); };

    ds_.set_field(field::kType, s, static_cast<std::uint16_t>(type));
    ds_.set_field(field::kSpawnIndex, s, static_cast<std::uint16_t>(record));
    ds_.set_field(field::kFlags, s, flags & 0xDFFF);
    ds_.set_field(f::kParam, s, ds_.u16(g::kSpawnParam));
    const std::uint8_t parts_byte = scripts_.at(rec + 2);
    ds_.set_u16(g::kSpawnParam, (parts_byte & 0x80) ? 1 : (parts_byte & 0x40) ? 2 : 0);
    ds_.set_field(f::kPartCount, s, parts_byte & 0x3F);
    ds_.set_field(field::kScriptIp, s, static_cast<std::uint16_t>(b16(3) + 3));
    const std::uint16_t seg = page_segment(ds_.u16(ds_addr::kScriptBaseSeg), static_cast<std::uint8_t>(b16(5)));
    ds_.set_field(field::kScriptSeg, s, seg);
    ds_.set_field(f::kAnimSeg, s, seg);
    ds_.set_field(f::kBoxKind, s, b8(7));
    ds_.set_field(f::kBoxW, s, b8(8));
    ds_.set_field(f::kBoxH, s, b8(9));
    ds_.set_u16(g::kTmp10, b16(10));
    std::uint16_t fseg = 0, foff = 0;
    if (!alloc_frame_buffer(b16(0), fseg, foff)) {
        ds_.set_field(field::kScriptSeg, s, 0);
        return -1;
    }
    ds_.set_field(f::kFrameBuffer, s, foff);
    ds_.set_field(f::kFrameSeg, s, fseg);
    ds_.set_field(f::kClassId, s, b16(12));
    ds_.set_field(f::kClassMask, s, b16(14));

    const std::uint16_t x = ds_.u16(g::kScratchX), y = ds_.u16(g::kScratchY);
    ds_.set_field(field::kX, s, x);
    ds_.set_field(f::kPrevX, s, x);
    ds_.set_field(field::kY, s, y);
    ds_.set_field(f::kPrevY, s, y);
    ds_.set_field(f::kParent, s, ds_.u16(ds_addr::kCurrentSlot));
    for (std::uint16_t fld : {f::kSubX, f::kSubY, f::kTimer, f::kVelX, f::kVelY, f::kMoveX, f::kMoveY, field::kClass,
                              f::kF170A, f::kF1732, f::kF16E2, f::kF175A})
        ds_.set_field(fld, s, 0);
    for (std::uint16_t fld : {f::kFrame, f::kChild, f::kAnimIp, f::kMoveMode}) ds_.set_field(fld, s, 0xFFFF);
    const std::uint16_t w = ds_.field(f::kBoxW, s), h = ds_.field(f::kBoxH, s);
    ds_.set_field(field::kLeft, s, static_cast<std::uint16_t>(x - (w >> 1)));
    ds_.set_field(field::kRight, s, static_cast<std::uint16_t>(x - (w >> 1) + w - 1));
    ds_.set_field(field::kTop, s, static_cast<std::uint16_t>(y - (h >> 1)));
    ds_.set_field(field::kBottom, s, static_cast<std::uint16_t>(y - (h >> 1) + h - 1));
    std::uint16_t hot_x = ds_.u16(g::kHotX);
    if (static_cast<std::int16_t>(hot_x) < 0) {
        ds_.set_u16(g::kHotY, static_cast<std::uint16_t>(h >> 1));
        hot_x = static_cast<std::uint16_t>(w >> 1);
    }
    ds_.set_field(field::kHalfWidth, s, hot_x);
    ds_.set_field(field::kHalfHeight, s, ds_.u16(g::kHotY));

    const int count = ds_.field(f::kPartCount, s);
    if (count) {
        int first = 0;
        if (!alloc_parts(count, first)) {
            ds_.set_field(field::kScriptSeg, s, 0);
            return -1;
        }
        ds_.set_field(f::kFirstPart, s, static_cast<std::uint16_t>(first));
        // sub_11713: attributes and colour base from the spawn flags.
        const std::uint16_t fl = ds_.field(field::kFlags, s);
        const std::uint16_t attr = static_cast<std::uint16_t>(((fl & 0x30) << 4) | 0x80);
        const std::uint8_t col = static_cast<std::uint8_t>((fl & 0x0E) << 3);
        for (int i = 0, p = first; i < count; ++i, p += 2) {
            ds_.set_field(part::kFlags, p, attr);
            ds_.set_field(part::kColour, p, static_cast<std::uint16_t>(col | (col << 8)));
            ds_.set_field(part::kRedraw, p, 0x202);
        }
        for (int i = 0, p = first; i < count; ++i, p += 2) set_part_size(p, fl & 1);  // sub_1174A
    }
    // sub_11795: apply the requested flips as real mirror operations.
    if (ds_.field(field::kFlags, s) & flag::kFacing) {
        ds_.set_field(field::kFlags, s, ds_.field(field::kFlags, s) ^ flag::kFacing);
        flip(s, false);
    }
    if (ds_.field(field::kFlags, s) & kFlipV) {
        ds_.set_field(field::kFlags, s, ds_.field(field::kFlags, s) ^ kFlipV);
        flip(s, true);
    }
    // sub_11771: park new parts off screen until the animation positions them.
    for (int i = 0, p = ds_.field(f::kFirstPart, s); i < count; ++i, p += 2) {
        ds_.set_field(part::kX, p, 0x4000);
        ds_.set_field(part::kA52, p, 0x4000);
        ds_.set_field(part::kB52, p, 0x4000);
    }
    if (s >= ds_.s16(ds_addr::kActorEnd)) ds_.set_u16(ds_addr::kActorEnd, static_cast<std::uint16_t>(s + 2));
    return s;
}

// Shared tail of 0x10 / E3 / E5 (loc_1706D): record the child and queue it
// to run this tick if the scheduler has already passed its slot.
void Vm::finish_spawn(int slot) {
    ds_.set_field(f::kChild, self(), static_cast<std::uint16_t>(slot < 0 ? 0 : slot));
    if (slot >= 0 && slot <= static_cast<std::int16_t>(ds_.u16(g::kSchedSlot))) {
        const std::uint16_t n = ds_.u16(g::kQueueLen);
        ds_.set_u8(static_cast<std::uint16_t>(g::kQueue + n), static_cast<std::uint8_t>(slot));
        ds_.set_u16(g::kQueueLen, static_cast<std::uint16_t>(n + 1));
    }
}

// 0x10 (sub_1705B): spawn `type` at (opA1, opB1) with param opA2 and flags.
Vm::Flow Vm::op_spawn() {
    const std::uint8_t m1 = fetch8();
    ds_.set_u16(g::kScratchX, read_operand(m1 & 7));
    ds_.set_u16(g::kScratchY, read_operand((m1 >> 3) & 7));
    const std::uint8_t m2 = fetch8();
    ds_.set_u16(g::kSpawnParam, read_operand(m2 & 7));
    const std::uint16_t extra = static_cast<std::uint16_t>(read_operand((m2 >> 3) & 7) & 0x801);
    ds_.set_u16(g::kTmp10, extra);
    const std::uint16_t flags = static_cast<std::uint16_t>((ds_.field(field::kFlags, self()) & 0xFE) | extra);
    const std::uint8_t type = fetch8();
    ds_.set_u16(g::kHotX, 0xFFFF);
    finish_spawn(spawn_actor(type, 0xFFFF, flags));
    return Flow::Continue;
}

// E3 (sub_17022): spawn relative to self, mirrored by facing.
Vm::Flow Vm::op_spawn_relative() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t dx = read_operand(m1 & 7), dy = read_operand((m1 >> 3) & 7);
    const int s = self();
    const std::uint16_t x = ds_.field(field::kX, s);
    ds_.set_u16(g::kScratchY, static_cast<std::uint16_t>(ds_.field(field::kY, s) + dy));
    ds_.set_u16(g::kScratchX, (ds_.field(field::kFlags, s) & flag::kFacing) ? static_cast<std::uint16_t>(x - dx)
                                                                              : static_cast<std::uint16_t>(x + dx));
    const std::uint8_t m2 = fetch8();
    ds_.set_u16(g::kSpawnParam, read_operand(m2 & 7));
    const std::uint16_t extra = static_cast<std::uint16_t>(read_operand((m2 >> 3) & 7) & 0x801);
    const std::uint16_t flags = static_cast<std::uint16_t>((ds_.field(field::kFlags, s) & 0xFE) | extra);
    const std::uint8_t type = fetch8();
    ds_.set_u16(g::kHotX, 0xFFFF);
    finish_spawn(spawn_actor(type, 0xFFFF, flags));
    return Flow::Continue;
}

// E5 (sub_16F8B): spawn from spawn record r unless one of its actors lives.
Vm::Flow Vm::op_spawn_record() {
    const std::uint8_t mode = fetch8();
    const int r = read_operand(mode & 7) & 0x7F;
    for (int s = ds_.u16(g::kFirstSpawnSlot); s < ds_.s16(ds_addr::kActorEnd); s += 2) {
        if (ds_.field(field::kScriptSeg, s) && ds_.field(field::kSpawnIndex, s) == r) {
            ds_.set_field(f::kChild, self(), static_cast<std::uint16_t>(s));
            return Flow::Continue;
        }
    }
    spawn_from_record(r);
    return Flow::Continue;
}

int Vm::spawn_from_record(int r) {
    const std::uint16_t rec = static_cast<std::uint16_t>(ds_addr::kSpawnRecords + r * 14);
    ds_.set_u16(g::kScratchX, ds_.u16(rec));
    ds_.set_u16(g::kScratchY, ds_.u16(static_cast<std::uint16_t>(rec + 2)));
    ds_.set_u16(g::kHotX, ds_.u16(static_cast<std::uint16_t>(rec + 4)));
    ds_.set_u16(g::kHotY, ds_.u16(static_cast<std::uint16_t>(rec + 6)));
    ds_.set_u16(g::kSpawnParam, ds_.u16(static_cast<std::uint16_t>(rec + 12)));
    const int slot = spawn_actor(ds_.u16(static_cast<std::uint16_t>(rec + 8)), r, ds_.u16(static_cast<std::uint16_t>(rec + 10)));
    finish_spawn(slot);
    return slot;
}

// sub_11380: spawn record r if it overlaps the window in ds:12h-18h and no
// actor from it is alive. Returns false at the end of the table.
bool Vm::check_record(int r) {
    const std::uint16_t rec = static_cast<std::uint16_t>(ds_addr::kSpawnRecords + r * 14);
    auto w = [&](int o) { return ds_.u16(static_cast<std::uint16_t>(rec + o)); };
    if (w(0) == 0xFFFF) return false;
    if (w(8) == 0) return true;
    auto sgn = [](int v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(v)); };
    if (sgn(w(0) + w(4) - ds_.u16(g::kTmp12)) < 0) return true;
    if (sgn(w(0) - w(4) - ds_.u16(g::kTmp14)) >= 0) return true;
    if (sgn(w(2) + w(6) - ds_.u16(g::kTmp16)) < 0) return true;
    if (sgn(w(2) - w(6) - ds_.u16(g::kTmp18)) >= 0) return true;
    for (int s = ds_.u16(g::kFirstSpawnSlot); s < ds_.s16(ds_addr::kActorEnd); s += 2)
        if (ds_.field(field::kScriptSeg, s) && ds_.field(field::kSpawnIndex, s) == r) return true;
    const std::uint16_t window[4] = {ds_.u16(g::kTmp12), ds_.u16(g::kTmp14), ds_.u16(g::kTmp16), ds_.u16(g::kTmp18)};
    ds_.set_u16(g::kScratchX, w(0));
    ds_.set_u16(g::kScratchY, w(2));
    ds_.set_u16(g::kHotX, w(4));
    ds_.set_u16(g::kHotY, w(6));
    ds_.set_u16(g::kSpawnParam, w(12));
    spawn_actor(w(8), r, w(10));
    for (int k = 0; k < 4; ++k) ds_.set_u16(static_cast<std::uint16_t>(g::kTmp12 + 2 * k), window[k]);
    return true;
}

// sub_1130E: spawn every record overlapping the camera window.
void Vm::spawn_visible_records() {
    const std::uint16_t cx = ds_.u16(g::kCameraX), cy = ds_.u16(g::kCameraY);
    std::int16_t win[4] = {static_cast<std::int16_t>(cx - 0x10), static_cast<std::int16_t>(cx - 0x10 + 0x120),
                           static_cast<std::int16_t>(cy - 0x10), static_cast<std::int16_t>(cy - 0x10 + 0x100)};
    for (int k = 0; k < 4; ++k) ds_.set_u16(static_cast<std::uint16_t>(g::kTmp12 + 2 * k), static_cast<std::uint16_t>(win[k] < 0 ? 0 : win[k]));
    const std::uint16_t saved = ds_.u16(ds_addr::kCurrentSlot);
    ds_.set_u16(ds_addr::kCurrentSlot, 0xFFFF);
    for (int r = 0; check_record(r); ++r) {}
    ds_.set_u16(ds_addr::kCurrentSlot, saved);
}

// loc_114AE: flag actors whose hot box left the window (they restart, which
// for most scripts means despawning).
void Vm::mark_offscreen_actors() {
    const std::uint16_t cx = ds_.u16(g::kCameraX), cy = ds_.u16(g::kCameraY);
    const int x0 = static_cast<std::int16_t>(cx - 0x10), y0 = static_cast<std::int16_t>(cy - 0x10);
    ds_.set_u16(g::kTmp12, static_cast<std::uint16_t>(x0 < 0 ? 0 : x0));
    ds_.set_u16(g::kTmp14, static_cast<std::uint16_t>(x0 + 0x120));
    ds_.set_u16(g::kTmp16, static_cast<std::uint16_t>(y0 < 0 ? 0 : y0));
    ds_.set_u16(g::kTmp18, static_cast<std::uint16_t>(y0 + 0x100));
    auto sgn = [](int v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(v)); };
    for (int s = ds_.u16(g::kFirstSpawnSlot); s < ds_.s16(ds_addr::kActorEnd); s += 2) {
        if (!ds_.field(field::kScriptSeg, s) || (ds_.field(field::kFlags, s) & 0x800)) continue;
        const int x = ds_.field(field::kX, s), y = ds_.field(field::kY, s);
        const int hx = ds_.field(field::kHalfWidth, s), hy = ds_.field(field::kHalfHeight, s);
        const bool out = sgn(x + hx - ds_.u16(g::kTmp12)) < 0 || sgn(x - hx - ds_.u16(g::kTmp14)) >= 0 ||
                         sgn(y + hy - ds_.u16(g::kTmp16)) < 0 || sgn(y - hy - ds_.u16(g::kTmp18)) >= 0;
        if (out) ds_.set_field(field::kFlags, s, ds_.field(field::kFlags, s) | 0x200);
    }
}

// sub_11560: free an actor, its parts, frame buffer slot and palette ref.
void Vm::despawn_actor(int s) {
    // sub_11605: give the frame buffer slot back.
    const std::size_t rec = static_cast<std::size_t>(ds_.field(field::kType, s)) * 16;
    if ((scripts_.at(rec) | (scripts_.at(rec + 1) << 8)) == 0xFFFE) {
        const std::uint16_t key = static_cast<std::uint16_t>(scripts_.at(rec + 10) | (scripts_.at(rec + 11) << 8));
        for (std::uint16_t e = ds_.u16(g::kFrameSlots); ds_.u16(e) != 0xFFFF; e = static_cast<std::uint16_t>(e + 4))
            if (ds_.u16(e) == ds_.field(f::kFrameBuffer, s)) {
                ds_.set_u16(static_cast<std::uint16_t>(e + 2), key);
                break;
            }
    }
    release_palette(s);
    for (int i = 0, p = ds_.field(f::kFirstPart, s); i < ds_.field(f::kPartCount, s); ++i, p += 2) {
        ds_.set_field(part::kFlags, p, 0);
        ds_.set_field(part::kRedraw, p, 0x200);
    }
    const int parent = static_cast<std::int16_t>(ds_.field(f::kParent, s));
    if (parent >= 0) ds_.set_field(f::kChild, parent, 0xFFFF);
    const int child = static_cast<std::int16_t>(ds_.field(f::kChild, s));
    if (child >= 0) ds_.set_field(f::kParent, child, 0xFFFF);
    ds_.set_field(field::kScriptSeg, s, 0);
    ds_.set_field(f::kAnimSeg, s, 0);
    ds_.set_field(f::kAnimIp, s, 0xFFFF);
    if (s + 2 == ds_.s16(ds_addr::kActorEnd)) {
        int e = s;
        while (e > 0 && ds_.field(field::kScriptSeg, e - 2) == 0) e -= 2;
        ds_.set_u16(ds_addr::kActorEnd, static_cast<std::uint16_t>(e));
    }
    const int r = static_cast<std::int16_t>(ds_.field(field::kSpawnIndex, s));
    if (r >= 0 && (ds_.field(field::kFlags, s) & 0x100)) {
        const std::uint16_t cx = ds_.u16(g::kCameraX), cy = ds_.u16(g::kCameraY);  // sub_11313
        ds_.set_u16(g::kTmp12, static_cast<std::uint16_t>(cx - 0x10));
        ds_.set_u16(g::kTmp14, static_cast<std::uint16_t>(cx - 0x10 + 0x120));
        ds_.set_u16(g::kTmp16, static_cast<std::uint16_t>(cy - 0x10));
        ds_.set_u16(g::kTmp18, static_cast<std::uint16_t>(cy - 0x10 + 0x100));
        const std::uint16_t saved = ds_.u16(ds_addr::kCurrentSlot);
        ds_.set_u16(ds_addr::kCurrentSlot, 0xFFFF);
        check_record(r);
        ds_.set_u16(ds_addr::kCurrentSlot, saved);
    }
}

// 0x0E: despawn self and end the turn.
Vm::Flow Vm::op_despawn_self() {
    const std::uint16_t type = ds_.field(field::kType, self());
    despawn_actor(self());
    // The original's ip register (bx) now points at the type record
    // (sub_11605 -> sub_1162E); an animation script that despawned its actor
    // leaves that value as its ip (sub_1207C stores bx back).
    ip_ = static_cast<std::uint16_t>(type * 16);
    return Flow::EndTurn;
}

// F2: this actor's spawn record will not respawn.
Vm::Flow Vm::op_forget_record() {
    const int r = static_cast<std::int16_t>(ds_.field(field::kSpawnIndex, self()));
    if (r >= 0) ds_.set_u16(static_cast<std::uint16_t>(ds_addr::kSpawnRecords + 8 + r * 14), 0);
    return Flow::Continue;
}

// ---- palette slots ----------------------------------------------------------

// sub_1153A: drop this actor's palette slot reference.
void Vm::release_palette(int s) {
    if (!(ds_.field(field::kFlags, s) & 0x2000)) return;
    ds_.set_field(field::kFlags, s, ds_.field(field::kFlags, s) & 0xDFFF);
    const int slot = (((ds_.field(part::kColour, ds_.field(f::kFirstPart, s)) >> 4) - 8) & 7) * 2;
    const std::uint16_t a = static_cast<std::uint16_t>(g::kPalRefs + slot);
    ds_.set_u16(a, static_cast<std::uint16_t>(ds_.u16(a) - 1));
}

// sub_121F2 + sub_19062: get a palette slot (0..14 step 2) holding palette `id`.
int Vm::acquire_palette(std::uint16_t id) {
    int si = 0xE;
    for (; si >= 0; si -= 2)
        if (ds_.u16(static_cast<std::uint16_t>(g::kPalIds + si)) == id) break;
    if (si < 0) {
        si = 0xE;
        while (si >= 0 && ds_.u16(static_cast<std::uint16_t>(g::kPalRefs + si)) != 0) si -= 2;
        if (si < 0) si = 0;
        ds_.set_u16(static_cast<std::uint16_t>(g::kPalIds + si), id);
        // sub_19062: copy the 16 colours into palette slot 8 + si/2 and re-tint them.
        int idx = -1;
        for (std::uint16_t d = g::kPalTable;; d = static_cast<std::uint16_t>(d + 2)) {
            const std::uint16_t v = ds_.u16(d);
            if (v == 0xFFFF) throw std::runtime_error("unknown palette id (assertion 0x69)");
            if (v == id) {
                idx = (d - g::kPalTable) / 2;
                break;
            }
        }
        const std::uint16_t dst = static_cast<std::uint16_t>((si + 16) * 24);
        for (int k = 0; k < 48; ++k)
            ds_.set_u8(static_cast<std::uint16_t>(g::kBasePalette + dst + k), ds_.u8(static_cast<std::uint16_t>(g::kPalData + idx * 48 + k)));
        retint_palette(dst, 16);
    }
    const std::uint16_t a = static_cast<std::uint16_t>(g::kPalRefs + si);
    ds_.set_u16(a, static_cast<std::uint16_t>(ds_.u16(a) + 1));
    return si;
}

// sub_12671 on a range of colours starting at byte offset `start`.
void Vm::retint_palette(std::uint16_t start, int colours) {
    const bool subtract = ds_.u16(g::kTintMode) & 0x80;
    const std::uint8_t delta[3] = {ds_.u8(g::kTint), ds_.u8(g::kTint + 2), ds_.u8(g::kTint + 4)};
    for (int i = 0; i < colours * 3; ++i) {
        const std::uint8_t base = ds_.u8(static_cast<std::uint16_t>(g::kBasePalette + start + i));
        std::uint8_t c = static_cast<std::uint8_t>(subtract ? base - delta[i % 3] : base + delta[i % 3]);
        if (c & 0x80) c = 0;
        if (c & 0x40) c = 0x3F;
        ds_.set_u8(static_cast<std::uint16_t>(g::kShownPalette + start + i), c);
    }
}

// F1 (sub_1221F): give the animation's parts a reference-counted palette slot.
Vm::Flow Vm::op_palette() {
    const int s = self();
    if (ds_.field(field::kFlags, s) & 0x2000) {
        ds_.set_field(field::kFlags, s, ds_.field(field::kFlags, s) & 0xDFFF);
        const int slot = (((ds_.field(part::kColour, ds_.u16(g::kFirstPart)) >> 4) - 8) & 7) * 2;
        const std::uint16_t a = static_cast<std::uint16_t>(g::kPalRefs + slot);
        ds_.set_u16(a, static_cast<std::uint16_t>(ds_.u16(a) - 1));
    }
    const std::uint16_t id = fetch16();
    std::uint16_t colour = 0x0707;
    if (id != 0x48) {
        ds_.set_field(field::kFlags, s, ds_.field(field::kFlags, s) | 0x2000);
        const std::uint8_t c = static_cast<std::uint8_t>((acquire_palette(id) + 16) << 3);
        colour = static_cast<std::uint16_t>(c | (c << 8));
    }
    const int count = ds_.u16(g::kPartCount);
    for (int i = 0, p = ds_.u16(g::kFirstPart); i < count; ++i, p += 2) {
        ds_.set_field(part::kColour, p, colour);
        ds_.set_field(part::kRedraw, p, 0x202);
    }
    return Flow::Continue;
}

// ---- event checks and collision -------------------------------------------

std::uint16_t Vm::check_bit() const {
    return ds_.u16(static_cast<std::uint16_t>(ds_addr::kBitMasks + ds_.u16(g::kCheckOrdinal)));
}

// After a check: advance the ordinal; in the main pass call the handler if
// the bit was set, otherwise step over the target.
void Vm::finish_check(bool fire) {
    ds_.set_u16(g::kCheckOrdinal, static_cast<std::uint16_t>(ds_.u16(g::kCheckOrdinal) + 2));
    const std::uint16_t target = fetch16();
    if (fire) call(target);
}

// 0x17 / 0x1A / 0x2F / 0x30 (sub_17B59 / sub_17C38): overlap with an actor of
// a class (id or mask). Detects in pass -1, fires in the main pass.
template <bool KyleOnly, bool ByMask>
Vm::Flow Vm::op_event_overlap() {
    const std::uint16_t saved_end = ds_.u16(ds_addr::kActorEnd);
    if (KyleOnly) ds_.set_u16(ds_addr::kActorEnd, 2);
    const int pass = static_cast<std::int16_t>(ds_.u16(g::kPass));
    const int s = self();
    bool fire = false;
    if (pass == 0) {
        ip_ = static_cast<std::uint16_t>(ip_ + (ByMask ? 2 : 1));
        if (ds_.field(f::kEventBits, s) & check_bit()) {
            const std::uint16_t hit = static_cast<std::uint16_t>(s * 8 + ds_.u16(g::kCheckOrdinal) / 2);
            ds_.set_field(field::kRelated, s, ds_.u8(static_cast<std::uint16_t>(g::kHitList + hit)));
            fire = true;
        }
    } else if (pass > 0) {
        ip_ = static_cast<std::uint16_t>(ip_ + (ByMask ? 2 : 1));
    } else {
        const std::uint16_t key = ByMask ? fetch16() : fetch8();
        auto fld = [&](std::uint16_t base, int slot) { return static_cast<int>(static_cast<std::int16_t>(ds_.field(base, slot))); };
        ds_.set_u16(g::kTmp12, ds_.field(field::kLeft, s));
        ds_.set_u16(g::kTmp14, ds_.field(field::kRight, s));
        ds_.set_u16(g::kTmp16, ds_.field(field::kTop, s));
        ds_.set_u16(g::kTmp18, ds_.field(field::kBottom, s));
        for (int o = 0; o < ds_.s16(ds_addr::kActorEnd); o += 2) {
            if (!ds_.field(field::kScriptSeg, o)) continue;
            if (ByMask ? !(ds_.field(f::kClassMask, o) & key) : ds_.field(f::kClassId, o) != key) continue;
            if (o == s) continue;
            if (fld(field::kRight, s) < fld(field::kLeft, o) || fld(field::kRight, o) < fld(field::kLeft, s) ||
                fld(field::kBottom, s) < fld(field::kTop, o) || fld(field::kBottom, o) < fld(field::kTop, s))
                continue;
            ds_.set_field(f::kEventBits, s, ds_.field(f::kEventBits, s) | check_bit());
            ds_.set_u8(static_cast<std::uint16_t>(g::kHitList + s * 8 + ds_.u16(g::kCheckOrdinal) / 2), static_cast<std::uint8_t>(o));
            break;
        }
    }
    if (KyleOnly) ds_.set_u16(ds_addr::kActorEnd, saved_end);
    finish_check(fire);
    return Flow::Continue;
}

// Snaps for terrain hits (sub_17E94 horizontal, sub_17EDC vertical). `dir`
// 0 = moving right/down, 1 = left/up (and negative = push up by the low byte).
void Vm::snap_horizontal(int s, int dir) {
    const std::uint16_t x = ds_.field(field::kX, s);
    if (dir == 0) {
        const std::uint16_t old = ds_.field(field::kRight, s);
        const std::uint16_t nr = static_cast<std::uint16_t>((old & 0xFFF0) - 1);
        ds_.set_field(field::kRight, s, nr);
        const std::uint16_t d = static_cast<std::uint16_t>(old - nr);
        ds_.set_field(field::kX, s, static_cast<std::uint16_t>(x - d));
        ds_.set_field(field::kLeft, s, static_cast<std::uint16_t>(ds_.field(field::kLeft, s) - d));
    } else {
        const std::uint16_t old = ds_.field(field::kLeft, s);
        const std::uint16_t nl = static_cast<std::uint16_t>((old | 0xF) + 1);
        ds_.set_field(field::kLeft, s, nl);
        const std::uint16_t d = static_cast<std::uint16_t>(nl - old);
        ds_.set_field(field::kX, s, static_cast<std::uint16_t>(x + d));
        ds_.set_field(field::kRight, s, static_cast<std::uint16_t>(ds_.field(field::kRight, s) + d));
    }
    ds_.set_field(f::kSubX, s, 0);
}

void Vm::snap_vertical(int s, int dir) {
    std::uint16_t d = 0;
    if (dir == 0 || dir < 0) {
        if (dir == 0) {
            const std::uint16_t old = ds_.field(field::kBottom, s);
            const std::uint16_t nb = static_cast<std::uint16_t>((old & 0xFFF0) - 1);
            ds_.set_field(field::kBottom, s, nb);
            d = static_cast<std::uint16_t>(old - nb);
        } else {
            d = static_cast<std::uint8_t>(dir);
            ds_.set_field(field::kBottom, s, static_cast<std::uint16_t>(ds_.field(field::kBottom, s) - d));
        }
        ds_.set_field(field::kY, s, static_cast<std::uint16_t>(ds_.field(field::kY, s) - d));
        ds_.set_field(field::kTop, s, static_cast<std::uint16_t>(ds_.field(field::kTop, s) - d));
    } else {
        const std::uint16_t old = ds_.field(field::kTop, s);
        const std::uint16_t nt = static_cast<std::uint16_t>((old | 0xF) + 1);
        ds_.set_field(field::kTop, s, nt);
        d = static_cast<std::uint16_t>(nt - old);
        ds_.set_field(field::kY, s, static_cast<std::uint16_t>(ds_.field(field::kY, s) + d));
        ds_.set_field(field::kBottom, s, static_cast<std::uint16_t>(ds_.field(field::kBottom, s) + d));
    }
    ds_.set_field(f::kSubY, s, 0);
}

// Solid-actor tests (sub_180F8 / sub_18154 / sub_184CE): returns the
// direction (0/1) and the actor slot hit, or -1.
int Vm::solid_actor_hit(int axis, std::uint8_t list, int& dir) {
    const int s = self();
    auto fld = [&](std::uint16_t base, int slot) { return static_cast<int>(static_cast<std::int16_t>(ds_.field(base, slot))); };
    ds_.set_u16(g::kTmp18, list);
    for (int o = 0; o < ds_.s16(ds_addr::kActorEnd); o += 2) {
        if (!ds_.field(field::kScriptSeg, o) || o == s) continue;
        ds_.set_u16(g::kTmp16, static_cast<std::uint16_t>(o));
        if (!in_class_list(list, static_cast<std::uint8_t>(ds_.field(field::kClass, o)))) continue;
        const int rel = static_cast<std::int16_t>(static_cast<std::uint16_t>(
            ds_.field(axis == 0 ? f::kMoveX : f::kMoveY, s) - ds_.field(axis == 0 ? f::kMoveX : f::kMoveY, o)));
        if (rel == 0) continue;
        if (axis == 2) {  // landing: only when moving down relative to the other actor (sub_18521)
            if (rel < 0) continue;
            if (fld(field::kRight, s) - fld(field::kLeft, o) < 0 || fld(field::kRight, o) - fld(field::kLeft, s) < 0) continue;
            const int b = fld(field::kBottom, s) < 0 ? 0 : fld(field::kBottom, s);
            ds_.set_u16(g::kTmp10, static_cast<std::uint16_t>(b));
            int prev = fld(field::kBottom, s) - fld(field::kY, s) + fld(f::kPrevY, s);
            if (prev <= 0) prev = 0;
            int lo = 0, hi = 0;
            if (prev < b) { lo = prev; hi = b; } else { hi = prev; lo = b; }
            ds_.set_u16(g::kTmp12, static_cast<std::uint16_t>(lo));
            ds_.set_u16(g::kTmp14, static_cast<std::uint16_t>(hi));
            const int otop_prev = fld(field::kTop, o) - fld(field::kY, o) + fld(f::kPrevY, o);
            bool hit = false;
            if (otop_prev < fld(field::kTop, o)) {
                ds_.set_u16(g::kTmp10, static_cast<std::uint16_t>(otop_prev));
                hit = hi >= otop_prev && fld(field::kTop, o) >= lo;
            } else {
                ds_.set_u16(g::kTmp10, static_cast<std::uint16_t>(otop_prev));
                hit = hi >= fld(field::kTop, o) && otop_prev >= lo;
            }
            if (!hit) continue;
            dir = 0;
            return o;
        }
        const bool negative = rel < 0;
        bool hit = false;
        if (axis == 0) {  // sub_181B0 (left edge) / sub_181B6 (right edge)
            const int edge = negative ? fld(field::kLeft, s) : fld(field::kRight, s);
            hit = edge >= fld(field::kLeft, o) && edge - 1 < fld(field::kRight, o) &&
                  fld(field::kBottom, s) - fld(f::kMoveY, s) >= fld(field::kTop, o) - fld(f::kMoveY, o) &&
                  fld(field::kBottom, o) - fld(f::kMoveY, o) >= fld(field::kTop, s) - fld(f::kMoveY, s);
        } else {  // sub_181F3 (top edge) / sub_181F9 (bottom edge)
            const int edge = negative ? fld(field::kTop, s) : fld(field::kBottom, s);
            hit = edge >= fld(field::kTop, o) && edge - 1 < fld(field::kBottom, o) &&
                  fld(field::kRight, s) - fld(field::kLeft, o) >= 0 && fld(field::kRight, o) - fld(field::kLeft, s) >= 0;
        }
        if (!hit) continue;
        dir = negative ? 1 : 0;
        return o;
    }
    return -1;
}

// sub_18222 / sub_1825E: push self out of solid actor o.
void Vm::push_out(int axis, int o, int dir) {
    const int s = self();
    const std::uint16_t lo = axis == 0 ? field::kLeft : field::kTop;
    const std::uint16_t hi = axis == 0 ? field::kRight : field::kBottom;
    const std::uint16_t pos = axis == 0 ? field::kX : field::kY;
    std::uint16_t d;
    if (dir == 0) {
        d = static_cast<std::uint16_t>(ds_.field(hi, s) - ds_.field(lo, o) + 1);
        d = static_cast<std::uint16_t>(-d);
    } else {
        d = static_cast<std::uint16_t>(ds_.field(hi, o) - ds_.field(lo, s) + 1);
    }
    for (std::uint16_t fld : {pos, lo, hi}) ds_.set_field(fld, s, static_cast<std::uint16_t>(ds_.field(fld, s) + d));
    ds_.set_field(axis == 0 ? f::kSubX : f::kSubY, s, 0);
}

// 0x2D (horizontal), 0x2E (vertical), 0x33 (landing): terrain collision
// events. Detect and push out in pass +1, fire in the main pass.
template <int Axis>
Vm::Flow Vm::op_event_terrain() {
    const int pass = static_cast<std::int16_t>(ds_.u16(g::kPass));
    const int s = self();
    bool fire = false;
    if (pass == 0) {
        ++ip_;
        fire = ds_.field(f::kEventBits, s) & check_bit();
    } else if (pass < 0) {
        ++ip_;
    } else {
        const std::uint8_t list = fetch8();
        auto fld = [&](std::uint16_t base) { return static_cast<int>(static_cast<std::int16_t>(ds_.field(base, s))); };
        auto clamp0 = [](int v) { return v < 0 ? 0 : v; };
        bool hit = false;
        int dir = 0;
        if (Axis == 0) {  // sub_17E5C
            const int dx = fld(field::kX) - fld(f::kPrevX);
            if (dx != 0) {
                dir = dx < 0 ? 1 : 0;
                const int x = dx < 0 ? fld(field::kLeft) : fld(field::kRight);
                hit = scan_column(list, x, clamp0(fld(field::kTop) - fld(f::kMoveY)), clamp0(fld(field::kBottom) - fld(f::kMoveY)));
            }
            if (hit) snap_horizontal(s, dir);
        } else if (Axis == 1) {  // sub_17E78
            const int dy = fld(field::kY) - fld(f::kPrevY);
            if (dy != 0) {
                dir = dy < 0 ? 1 : 0;
                const int y = dy < 0 ? fld(field::kTop) : fld(field::kBottom);
                hit = scan_row(list, y, fld(field::kLeft), fld(field::kRight));
            }
            if (hit) snap_vertical(s, dir);
        } else {  // sub_1806F: bottom edge entered a new 16-pixel row while moving down
            const int dy = fld(field::kY) - fld(f::kPrevY);
            if (dy > 0) {
                ds_.set_u16(g::kScratchX, list);
                ds_.set_u16(g::kScratchY, static_cast<std::uint16_t>(s));
                const int old_row = (fld(field::kBottom) - fld(field::kY) + fld(f::kPrevY)) & 0xFFF0;
                ds_.set_u16(g::kTmp10, static_cast<std::uint16_t>(old_row));
                if ((fld(field::kBottom) & 0xFFF0) != old_row) hit = scan_row(list, fld(field::kBottom), fld(field::kLeft), fld(field::kRight));
            }
            if (hit) snap_vertical(s, 0);
        }
        if (!hit) {
            const int o = solid_actor_hit(Axis, list, dir);
            if (o >= 0) {
                push_out(Axis == 0 ? 0 : 1, o, dir);
                hit = true;
            }
        }
        if (hit) ds_.set_field(f::kEventBits, s, ds_.field(f::kEventBits, s) | check_bit());
    }
    finish_check(fire);
    return Flow::Continue;
}

// Runs the event-check block (from the last yield to opcode 0x01) for one
// actor in pass +1 or -1 (sub_17AE9).
void Vm::run_checks(int slot) {
    if (!ds_.field(field::kScriptSeg, slot) || (ds_.field(field::kFlags, slot) & 0x400)) return;
    ds_.set_u16(ds_addr::kCurrentSlot, static_cast<std::uint16_t>(slot));
    ds_.set_u16(g::kCheckOrdinal, 0);
    seg_ = ds_.field(field::kScriptSeg, slot);
    ip_ = ds_.field(field::kScriptIp, slot);
    for (;;) {
        last_op_ = fetch8();
        if (last_op_ == 0x01) return;
        if ((this->*table_[last_op_])() == Flow::EndTurn) return;
    }
}

// ---- scans and misc -----------------------------------------------------------

bool Vm::onscreen(int s) const {
    const std::uint16_t cx = ds_.u16(g::kCameraX), cy = ds_.u16(g::kCameraY);
    auto sgn = [](int v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(v)); };
    return sgn(cx - ds_.field(field::kRight, s)) < 0 && sgn(cx + 0x100 - ds_.field(field::kLeft, s)) >= 0 &&
           sgn(cy - ds_.field(field::kBottom, s)) < 0 && sgn(cy + 0xE0 - ds_.field(field::kTop, s)) >= 0;
}

template <bool Target, bool Negate>
Vm::Flow Vm::op_if_onscreen() {
    const std::uint16_t target = fetch16();
    const int s = Target ? ds_.field(field::kTarget, self()) : self();
    if (onscreen(s) != Negate) jump(target);
    return Flow::Continue;
}

// EF / F0 (sub_1864D / sub_185D6): first overlapping actor by mask or class.
template <bool ByMask>
Vm::Flow Vm::op_find_overlap() {
    const std::uint16_t key = fetch16();
    const std::uint16_t target = fetch16();
    const int s = self();
    auto fld = [&](std::uint16_t base, int slot) { return static_cast<int>(static_cast<std::int16_t>(ds_.field(base, slot))); };
    for (int o = 0; o < ds_.s16(ds_addr::kActorEnd); o += 2) {
        if (!ds_.field(field::kScriptSeg, o) || o == s) continue;
        if (ByMask ? !(ds_.field(f::kClassMask, o) & key) : ds_.field(f::kClassId, o) != key) continue;
        if (fld(field::kRight, s) >= fld(field::kLeft, o) && fld(field::kRight, o) >= fld(field::kLeft, s) &&
            fld(field::kBottom, s) >= fld(field::kTop, o) && fld(field::kBottom, o) >= fld(field::kTop, s)) {
            ds_.set_field(field::kRelated, s, static_cast<std::uint16_t>(o));
            jump(target);
            return Flow::Continue;
        }
    }
    return Flow::Continue;
}

// CC / F8 (sub_17870): horizontal line-of-sight scan ahead of the actor.
template <bool KyleOnly>
Vm::Flow Vm::op_scan() {
    const std::uint16_t saved_end = ds_.u16(ds_addr::kActorEnd);
    if (KyleOnly) ds_.set_u16(ds_addr::kActorEnd, 2);
    const int s = self();
    auto fld = [&](std::uint16_t base, int slot) { return static_cast<int>(static_cast<std::int16_t>(ds_.field(base, slot))); };
    int dx = static_cast<std::int16_t>(fetch16());
    const int dy = static_cast<std::int16_t>(fetch16());
    const std::uint8_t sel = fetch8();
    const std::uint16_t mask = fetch16();
    const std::uint16_t a = fetch16(), b = fetch16();
    const int dir = (ds_.field(field::kFlags, s) & flag::kFacing) ? -16 : 16;
    ds_.set_u16(g::kScanDir, static_cast<std::uint16_t>(dir));
    if (dir < 0) dx = -dx;
    const int x0 = static_cast<std::int16_t>(((fld(field::kX, s) + dx) & 0xFFF0) | 8);
    const int y = static_cast<std::int16_t>(static_cast<std::uint16_t>(fld(field::kY, s) + dy));
    ds_.set_u16(g::kScanX, static_cast<std::uint16_t>(x0));
    ds_.set_u16(g::kScanStart, static_cast<std::uint16_t>(x0));
    ds_.set_u16(g::kScanY, static_cast<std::uint16_t>(y));
    ds_.set_u16(g::kScanSel, sel);
    ds_.set_u16(g::kScanMask, mask);
    // Nearest candidate actor ahead.
    int best_slot = -1, best_edge = dir > 0 ? 0x7FFF : 0;
    std::uint16_t best_match = 0;
    for (int o = 0; o < ds_.s16(ds_addr::kActorEnd); o += 2) {
        if (!ds_.field(field::kScriptSeg, o) || o == s) continue;
        const std::uint16_t m = ds_.field(f::kClassMask, o) & mask;
        if (!m && !in_class_list(sel, static_cast<std::uint8_t>(ds_.field(field::kClass, o)))) continue;
        if (!(fld(field::kTop, o) <= y && y <= fld(field::kBottom, o))) continue;
        if (dir > 0) {
            const int e = fld(field::kRight, o);
            if (x0 <= e && e < best_edge) { best_edge = e; best_slot = o; best_match = m; }
        } else {
            const int e = fld(field::kLeft, o);
            if (best_edge <= e && e <= x0) { best_edge = e; best_slot = o; best_match = m; }
        }
    }
    ds_.set_u16(g::kScanSlot, static_cast<std::uint16_t>(best_slot));
    if (best_slot >= 0) ds_.set_u16(g::kScanMatch, best_match);  // left alone when nothing is found
    ds_.set_u16(g::kScanEdge, static_cast<std::uint16_t>(best_edge));
    const int cam = static_cast<std::int16_t>(ds_.u16(g::kCameraX));
    int range = dir > 0 ? std::min(cam + 0x110, best_edge) - x0
                        : x0 - std::max(std::max(cam - 16, 0), best_edge);
    int x = x0;
    bool blocked = false;
    ds_.set_u16(0x62, static_cast<std::uint16_t>(range));  // the original keeps the range in ds:62h
    for (;;) {
        ds_.set_u16(g::kScratchX, static_cast<std::uint16_t>(x));
        ds_.set_u16(g::kScratchY, static_cast<std::uint16_t>(y));
        if (scan_row(sel, y, x, x)) { blocked = true; break; }
        x += dir;
        ds_.set_u16(g::kScanX, static_cast<std::uint16_t>(x));
        range -= 16;
        ds_.set_u16(0x62, static_cast<std::uint16_t>(range));
        if (range < 0) break;
    }
    if (KyleOnly) ds_.set_u16(ds_addr::kActorEnd, saved_end);
    auto hit_actor = [&]() {
        const int o = best_slot;
        ds_.set_field(field::kRelated, s, static_cast<std::uint16_t>(o));
        ds_.set_u16(ds_addr::kProbeSlot, static_cast<std::uint16_t>(o));
        if (ds_.u16(g::kScanX) != ds_.u16(g::kScanStart))
            ds_.set_u16(g::kScanX, dir > 0 ? ds_.field(field::kLeft, o) : ds_.field(field::kRight, o));
        call(best_match ? b : a);
    };
    if (!blocked) {
        if (best_slot >= 0) hit_actor();
        return Flow::Continue;  // nothing hit: ip already past A and B
    }
    const int sx = static_cast<std::int16_t>(ds_.u16(g::kScanX));
    if (best_slot >= 0 && (dir > 0 ? fld(field::kLeft, best_slot) < sx : fld(field::kRight, best_slot) >= sx)) {
        hit_actor();
        return Flow::Continue;
    }
    ds_.set_u16(g::kScanX, dir > 0 ? static_cast<std::uint16_t>(sx & 0xFFF0) : static_cast<std::uint16_t>(sx | 0x000F));
    call(a);
    return Flow::Continue;
}

// CD / CE: indexed global store / load (byte offset index).
Vm::Flow Vm::op_indexed_store() {
    const std::uint8_t mode = fetch8();
    const std::uint16_t addr = fetch16();
    const std::uint16_t idx = read_operand((mode >> 3) & 7);
    ds_.set_u16(static_cast<std::uint16_t>(addr + idx), acc());
    return Flow::Continue;
}

Vm::Flow Vm::op_indexed_load() {
    const std::uint8_t mode = fetch8();
    const std::uint16_t addr = fetch16();
    const std::uint16_t idx = read_operand((mode >> 3) & 7);
    set_acc(ds_.u16(static_cast<std::uint16_t>(addr + idx)));
    return Flow::Continue;
}

}  // namespace bt
