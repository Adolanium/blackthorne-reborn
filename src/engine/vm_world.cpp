// VM opcodes that deal with actor orientation, movement modes, the map and
// map/actor probes. Semantics: docs/vm/ops_00-3F.md.

#include <stdexcept>

#include "engine/vm.h"

namespace bt {
namespace {

// Read/write operand modes (sub_17569 / loc_175D6).
constexpr int kImm = 0, kSelf = 1;

// Edge probe kinds.
constexpr int kAbove = 0, kBelow = 1, kRightIfFacing = 2, kLeftIfFacing = 3, kAheadBelow = 4;

// Who an offset or anchor refers to.
constexpr int kKyle = 0, kTargetActor = 1, kRelatedActor = 2;

namespace g {
constexpr std::uint16_t kScratchX = 0x26, kScratchY = 0x28, kScratch12 = 0x12;
constexpr std::uint16_t kScratchReg = 0x204;    // used by 0x0F
constexpr std::uint16_t kLevelState = 0x318;    // bit 0: exit requested
constexpr std::uint16_t kTint = 0x320;          // 320h, 322h, 324h: r, g, b deltas
constexpr std::uint16_t kTintMode = 0x326;      // bit 0x80: subtract
constexpr std::uint16_t kBasePalette = 0x4A12;  // 768 bytes, 6-bit
constexpr std::uint16_t kShownPalette = 0x4D12;
constexpr std::uint16_t kCameraX = 0x1B4E, kCameraY = 0x1B50;
constexpr std::uint16_t kTileBuffer = 0x3508, kTileDirty = 0x4308;
constexpr std::uint16_t kExitTable = 0x1CA6;    // pointer to 10-byte exit records
constexpr std::uint16_t kExitResult = 0x1CA8;   // 1CA8h, 1CAAh, 1CACh
}  // namespace g

namespace f {
constexpr std::uint16_t kMoveMode = 0x134A, kMoveX = 0x17AA, kMoveY = 0x17D2;
constexpr std::uint16_t kAnimIp = 0x18EA, kAnimSeg = 0x1912, kAnimDelay = 0x193A;
constexpr std::uint16_t kFirstPart = 0x198A, kPartCount = 0x19B2;
}  // namespace f

namespace part {
constexpr std::uint16_t kFlags = 0x452, kX = 0x552, kY = 0x652, kSize = 0x952, kRedraw = 0xE52;
}  // namespace part

constexpr std::uint16_t kFlipV = 0x80;

}  // namespace

void Vm::register_world_ops() {
    table_[0x07] = &Vm::op_flip_to<false, true>;
    table_[0x08] = &Vm::op_flip_to<false, false>;
    table_[0x09] = &Vm::op_flip_to<true, true>;
    table_[0x0A] = &Vm::op_flip_to<true, false>;
    table_[0x0B] = &Vm::op_flip<false>;
    table_[0x0C] = &Vm::op_flip<true>;
    table_[0x0D] = &Vm::op_exit_level;
    table_[0x0F] = &Vm::op_scratch_fn;
    table_[0x11] = &Vm::op_offset_to<kKyle>;
    table_[0x12] = &Vm::op_offset_to<kTargetActor>;
    table_[0x32] = &Vm::op_offset_to<kRelatedActor>;
    table_[0x13] = &Vm::op_anchor<kKyle>;
    table_[0x14] = &Vm::op_anchor<kTargetActor>;
    table_[0x18] = &Vm::op_anchor<-1>;  // anchor Kyle to self
    table_[0x15] = &Vm::op_anim_start;
    table_[0x16] = &Vm::op_anim_start_far;
    table_[0x19] = &Vm::op_if_anim_idle;
    table_[0x1B] = &Vm::op_edge_probe<kAbove, false>;
    table_[0x1C] = &Vm::op_edge_probe<kBelow, false>;
    table_[0x1D] = &Vm::op_edge_probe<kRightIfFacing, false>;
    table_[0x1E] = &Vm::op_edge_probe<kLeftIfFacing, false>;
    table_[0x1F] = &Vm::op_edge_probe<kAbove, true>;
    table_[0x20] = &Vm::op_edge_probe<kBelow, true>;
    table_[0x21] = &Vm::op_edge_probe<kRightIfFacing, true>;
    table_[0x22] = &Vm::op_edge_probe<kLeftIfFacing, true>;
    table_[0x2B] = &Vm::op_edge_probe<kAheadBelow, false>;
    table_[0x2C] = &Vm::op_edge_probe<kAheadBelow, true>;
    table_[0x23] = &Vm::op_to_tile;
    table_[0x24] = &Vm::op_tile_class;
    table_[0x25] = &Vm::op_snap_to_tile;
    table_[0x26] = &Vm::op_write_cell<0>;
    table_[0x27] = &Vm::op_write_cell<1>;
    table_[0x28] = &Vm::op_write_cell<2>;
    table_[0x29] = &Vm::op_find_exit;
    table_[0x31] = &Vm::op_breakpoint;
    table_[0x34] = &Vm::op_tint;
    table_[0x35] = &Vm::op_tint_reset;
    table_[0x36] = &Vm::op_parts_flag<true>;
    table_[0x37] = &Vm::op_parts_flag<false>;
    table_[0x38] = &Vm::op_teleport;
    table_[0x39] = &Vm::op_point_probe<false>;
    table_[0x3A] = &Vm::op_point_probe<true>;
}

// sub_1103A / sub_110B0: mirror the bounding box and every sprite part about
// the actor's x (or y) and toggle the flip flag.
void Vm::flip(int slot, bool vertical) {
    const std::uint16_t lo = vertical ? field::kTop : field::kLeft;
    const std::uint16_t hi = vertical ? field::kBottom : field::kRight;
    const std::uint16_t c2 = static_cast<std::uint16_t>(2 * ds_.field(vertical ? field::kY : field::kX, slot));
    ds_.set_field(field::kFlags, slot, ds_.field(field::kFlags, slot) ^ (vertical ? kFlipV : flag::kFacing));
    const std::uint16_t new_lo = static_cast<std::uint16_t>(c2 - ds_.field(hi, slot) - 1);
    const std::uint16_t new_hi = static_cast<std::uint16_t>(c2 - ds_.field(lo, slot) - 1);
    ds_.set_field(hi, slot, new_hi);
    ds_.set_field(lo, slot, new_lo);
    const int count = ds_.field(f::kPartCount, slot);
    int p = ds_.field(f::kFirstPart, slot);
    const std::uint16_t pos = vertical ? part::kY : part::kX;
    for (int i = 0; i < count; ++i, p += 2) {
        ds_.set_field(pos, p, static_cast<std::uint16_t>(c2 - ds_.field(pos, p) - ds_.field(part::kSize, p)));
        ds_.set_field(part::kFlags, p, ds_.field(part::kFlags, p) ^ (vertical ? 0x10 : 0x20));
        ds_.set_field(part::kRedraw, p, 0x202);
    }
}

template <bool Vertical, bool Set>
Vm::Flow Vm::op_flip_to() {
    const bool is_set = ds_.field(field::kFlags, self()) & (Vertical ? kFlipV : flag::kFacing);
    if (is_set != Set) flip(self(), Vertical);
    return Flow::Continue;
}

template <bool Vertical>
Vm::Flow Vm::op_flip() {
    flip(self(), Vertical);
    return Flow::Continue;
}

// 0x0D: request the level exit and end the turn without saving ip.
Vm::Flow Vm::op_exit_level() {
    ds_.set_u16(g::kLevelState, ds_.u16(g::kLevelState) | 1);
    return Flow::EndTurn;
}

// 0x0F: operations on the scratch register ds:204h.
Vm::Flow Vm::op_scratch_fn() {
    const std::uint8_t sel = fetch8();
    const std::uint16_t fn = fetch16();
    if (sel != 0) return Flow::Continue;
    const std::uint16_t r = ds_.u16(g::kScratchReg);
    switch (fn) {
        case 0: ds_.set_u16(g::kScratchReg, static_cast<std::uint16_t>(r << 8)); break;
        case 2: ds_.set_u16(g::kScratchReg, static_cast<std::uint16_t>(r >> 8)); break;
        case 4: ds_.set_u16(g::kScratchReg, static_cast<std::uint16_t>(static_cast<std::int16_t>(r) >> 1)); break;
        case 6: {  // base palette colour as 5:5:5
            const std::uint16_t i = static_cast<std::uint16_t>(r + (r >> 1));
            const int red = ds_.u8(static_cast<std::uint16_t>(g::kBasePalette + i)) & 0x3E;
            const int grn = ds_.u8(static_cast<std::uint16_t>(g::kBasePalette + i + 1)) & 0x3E;
            const int blu = ds_.u8(static_cast<std::uint16_t>(g::kBasePalette + i + 2)) & 0x3E;
            ds_.set_u16(g::kScratchReg, static_cast<std::uint16_t>((red >> 1) | (grn << 4) | (blu << 9)));
            break;
        }
        default: throw std::runtime_error("opcode 0x0F: bad function");
    }
    return Flow::Continue;
}

// 0x11/0x12/0x32: facing-relative offset from self to Kyle / target / related.
template <int Who>
Vm::Flow Vm::op_offset_to() {
    const int s = self();
    const int o = Who == kKyle ? 0 : ds_.field(Who == kTargetActor ? field::kTarget : field::kRelated, s);
    const std::uint16_t flags = ds_.field(field::kFlags, s);
    const std::uint16_t sx = ds_.field(field::kX, s), ox = ds_.field(field::kX, o);
    const std::uint16_t sy = ds_.field(field::kY, s), oy = ds_.field(field::kY, o);
    const std::uint16_t dx = static_cast<std::uint16_t>((flags & flag::kFacing) ? sx - ox : ox - sx);
    const std::uint16_t dy = static_cast<std::uint16_t>((flags & kFlipV) ? sy - oy : oy - sy);
    ds_.set_u16(g::kScratchX, dx);
    ds_.set_u16(g::kScratchY, dy);
    const std::uint8_t mode = fetch8();
    write_operand(mode & 7, dx);
    write_operand((mode >> 3) & 7, dy);
    return Flow::Continue;
}

// 0x13: anchor self to Kyle; 0x14: to the target; Who = -1 (0x18): anchor Kyle to self.
template <int Who>
Vm::Flow Vm::op_anchor() {
    const std::uint8_t mode = fetch8();
    const std::uint16_t a = read_operand(mode & 7);
    const std::uint16_t b = read_operand((mode >> 3) & 7);
    const int s = self();
    int moved = s;
    std::uint16_t anchor = 0;
    if (Who == kTargetActor) anchor = ds_.field(field::kTarget, s);
    if (Who == -1) {
        moved = 0;
        anchor = static_cast<std::uint16_t>(s);
    }
    ds_.set_field(f::kMoveMode, moved, anchor);
    ds_.set_field(f::kMoveX, moved, a);
    ds_.set_field(f::kMoveY, moved, b);
    return Flow::Continue;
}

Vm::Flow Vm::op_anim_start() {
    const std::uint16_t addr = fetch16();
    ds_.set_field(f::kAnimIp, self(), addr);
    ds_.set_field(f::kAnimSeg, self(), seg_);
    ds_.set_field(f::kAnimDelay, self(), 1);
    return Flow::Continue;
}

Vm::Flow Vm::op_anim_start_far() {
    const std::uint8_t page = fetch8();
    const std::uint16_t addr = fetch16();
    ds_.set_field(f::kAnimSeg, self(), page_segment(ds_.u16(ds_addr::kScriptBaseSeg), page));
    ds_.set_field(f::kAnimIp, self(), addr);
    ds_.set_field(f::kAnimDelay, self(), 1);
    return Flow::Continue;
}

Vm::Flow Vm::op_if_anim_idle() {
    const std::uint16_t target = fetch16();
    if (ds_.field(f::kAnimDelay, self()) == 0) jump(target);
    return Flow::Continue;
}

// sub_17FB8 family: scan cells of row y from x0 to x1 in 16-pixel steps, the
// last step clamped to exactly x1.
bool Vm::scan_row(std::uint8_t list, int y, int x0, int x1) {
    ds_.set_u16(g::kScratch12, list);
    ds_.set_u16(0x14, static_cast<std::uint16_t>(y));
    ds_.set_u16(0x16, static_cast<std::uint16_t>(x1));
    for (int x = x0;;) {
        const std::uint8_t cls = m_.pixel_class(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
        if (in_class_list(list, cls)) {
            ds_.set_u16(ds_addr::kProbeClass, cls);
            return true;
        }
        if (x == x1) return false;
        x = static_cast<std::int16_t>(x + 16);
        if (x >= x1) x = x1;
    }
}

// sub_17F2F family: scan a column from y0 to y1 the same way.
bool Vm::scan_column(std::uint8_t list, int x, int y0, int y1) {
    ds_.set_u16(g::kScratch12, list);
    ds_.set_u16(0x14, static_cast<std::uint16_t>(x));
    ds_.set_u16(0x16, static_cast<std::uint16_t>(y1));
    for (int y = y0;;) {
        const std::uint8_t cls = m_.pixel_class(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y));
        if (in_class_list(list, cls)) {
            ds_.set_u16(ds_addr::kProbeClass, cls);
            return true;
        }
        if (y == y1) return false;
        y = static_cast<std::int16_t>(y + 16);
        if (y >= y1) y = y1;
    }
}

// sub_183BE / sub_1844F: a live actor (not self) of a listed class whose box
// contains pixel (px, py).
bool Vm::probe_point_actor(std::uint8_t list, int px, int py) {
    auto fld = [&](std::uint16_t base, int slot) { return static_cast<int>(static_cast<std::int16_t>(ds_.field(base, slot))); };
    const int end = ds_.s16(ds_addr::kActorEnd);
    for (int o = 0; o < end; o += 2) {
        if (ds_.field(field::kScriptSeg, o) == 0 || o == self()) continue;
        if (!in_class_list(list, static_cast<std::uint8_t>(ds_.field(field::kClass, o)))) continue;
        if (px < fld(field::kLeft, o) || px - 1 >= fld(field::kRight, o)) continue;
        if (py < fld(field::kTop, o) || py - 1 >= fld(field::kBottom, o)) continue;
        ds_.set_u16(ds_addr::kProbeClass, ds_.field(field::kClass, o));
        ds_.set_u16(ds_addr::kProbeSlot, static_cast<std::uint16_t>(o));
        return true;
    }
    return false;
}

template <int Kind, bool Negate>
Vm::Flow Vm::op_edge_probe() {
    const std::uint8_t list = fetch8();
    const int s = self();
    auto fld = [&](std::uint16_t base) { return static_cast<int>(static_cast<std::int16_t>(ds_.field(base, s))); };
    const bool facing = fld(field::kFlags) & flag::kFacing;
    ds_.set_u16(ds_addr::kProbeSlot, 0xFFFF);
    auto clamp0 = [](int v) { return v < 0 ? 0 : v; };
    auto probe_side = [&](bool right) {
        const int dy = fld(field::kProbeDy);
        const int x = right ? fld(field::kRight) + 1 : fld(field::kLeft) - 1;
        return scan_column(list, x, clamp0(fld(field::kTop) - dy), clamp0(fld(field::kBottom) - dy)) ||
               probe(right ? 3 : 2, list);
    };
    bool hit = false;
    switch (Kind) {
        case kAbove:
            hit = scan_row(list, fld(field::kTop) - 1, fld(field::kLeft), fld(field::kRight)) || probe(0, list);
            break;
        case kBelow:
            hit = scan_row(list, fld(field::kBottom) + 1, fld(field::kLeft), fld(field::kRight)) || probe(1, list);
            break;
        case kRightIfFacing: hit = probe_side(facing); break;   // 0x1D
        case kLeftIfFacing: hit = probe_side(!facing); break;   // 0x1E
        case kAheadBelow: {
            const int x = facing ? fld(field::kLeft) - 1 : fld(field::kRight) + 1;
            const int y = fld(field::kBottom) + 1;
            hit = scan_row(list, y, x, x) || probe_point_actor(list, x, y);
            break;
        }
        default: break;
    }
    const std::uint16_t target = fetch16();
    if (hit != Negate) jump(target);
    return Flow::Continue;
}

// 0x23: pixel coordinates to tile coordinates.
Vm::Flow Vm::op_to_tile() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t a = read_operand(m1 & 7), b = read_operand((m1 >> 3) & 7);
    ds_.set_u16(g::kScratchX, static_cast<std::uint16_t>(a >> 4));
    ds_.set_u16(g::kScratchY, static_cast<std::uint16_t>(b >> 4));
    const std::uint8_t m2 = fetch8();
    write_operand(m2 & 7, ds_.u16(g::kScratchX));
    write_operand((m2 >> 3) & 7, ds_.u16(g::kScratchY));
    return Flow::Continue;
}

// 0x24: collision class of tile (opA, opB).
Vm::Flow Vm::op_tile_class() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t tx = read_operand(m1 & 7), ty = read_operand((m1 >> 3) & 7);
    ds_.set_u16(g::kScratchX, tx);
    const std::uint8_t m2 = fetch8();
    write_operand(m2 & 7, static_cast<std::uint16_t>(m_.cell(tx, ty) >> 10));
    return Flow::Continue;
}

// 0x25: snap a pixel position to the centre of its tile.
Vm::Flow Vm::op_snap_to_tile() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t a = read_operand(m1 & 7), b = read_operand((m1 >> 3) & 7);
    ds_.set_u16(g::kScratchX, static_cast<std::uint16_t>((a & 0xFFF0) | 8));
    ds_.set_u16(g::kScratchY, static_cast<std::uint16_t>((b & 0xFFF0) | 8));
    const std::uint8_t m2 = fetch8();
    write_operand(m2 & 7, ds_.u16(g::kScratchX));
    write_operand((m2 >> 3) & 7, ds_.u16(g::kScratchY));
    return Flow::Continue;
}

// sub_18910: refresh the on-screen tile buffer if the cell is in view.
void Vm::redraw_tile(std::uint16_t tx, std::uint16_t ty, std::uint16_t v) {
    const int sx = static_cast<std::int16_t>(static_cast<std::uint16_t>((tx << 4) - ds_.u16(g::kCameraX)));
    if (sx < 0 || sx >= 0x100) return;
    const int sy = static_cast<std::int16_t>(static_cast<std::uint16_t>((ty << 4) - ds_.u16(g::kCameraY)));
    if (sy < 0 || sy >= 0xE0) return;
    const std::uint16_t idx = static_cast<std::uint16_t>(((sy >> 3) * 0x20 + (sx >> 3)) * 2);
    const std::uint16_t src = static_cast<std::uint16_t>((v & 0x3FF) * 8 + ds_.u16(Machine::kMetatileTable));
    const std::uint16_t offs[4] = {0, 2, 0x40, 0x42};
    for (int k = 0; k < 4; ++k) {
        ds_.set_u16(static_cast<std::uint16_t>(g::kTileBuffer + idx + offs[k]), m_.map_u16(static_cast<std::uint16_t>(src + 2 * k)));
        const std::uint16_t d = static_cast<std::uint16_t>(g::kTileDirty + idx + offs[k]);
        ds_.set_u16(d, ds_.u16(d) | 3);
    }
}

// 0x26 (What 0): write a whole cell; 0x27 (1): replace the graphic, keep the
// class; 0x28 (2): replace the class, keep the graphic (no redraw).
template <int What>
Vm::Flow Vm::op_write_cell() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t tx = read_operand(m1 & 7), ty = read_operand((m1 >> 3) & 7);
    ds_.set_u16(g::kScratchX, tx);
    ds_.set_u16(g::kScratchY, ty);
    const std::uint8_t m2 = fetch8();
    const std::uint16_t v = read_operand(m2 & 7);
    std::uint16_t c = v;
    if (What == 1) {
        ds_.set_u16(g::kScratch12, v);
        c = static_cast<std::uint16_t>((m_.cell(tx, ty) & 0xFC00) | v);
    } else if (What == 2) {
        ds_.set_u16(g::kScratch12, static_cast<std::uint16_t>((v & 0x3F) << 10));
        c = static_cast<std::uint16_t>((m_.cell(tx, ty) & 0x3FF) | ds_.u16(g::kScratch12));
    }
    m_.set_cell(tx, ty, c);
    if (What != 2) redraw_tile(tx, ty, c);
    return Flow::Continue;
}

// 0x29: find the exit record (10 bytes: x0, x1, y0, y1, 3 words) containing
// the actor's tile.
Vm::Flow Vm::op_find_exit() {
    const int s = self();
    const int tx = ds_.field(field::kX, s) >> 4, ty = ds_.field(field::kY, s) >> 4;
    ds_.set_u16(g::kScratchX, static_cast<std::uint16_t>(tx));
    ds_.set_u16(g::kScratchY, static_cast<std::uint16_t>(ty));
    for (std::uint16_t r = ds_.u16(g::kExitTable); ds_.u8(r) != 0xFF; r = static_cast<std::uint16_t>(r + 10)) {
        if (ds_.u8(r) <= tx && tx <= ds_.u8(static_cast<std::uint16_t>(r + 1)) &&
            ds_.u8(static_cast<std::uint16_t>(r + 2)) <= ty && ty <= ds_.u8(static_cast<std::uint16_t>(r + 3))) {
            for (int k = 0; k < 3; ++k)
                ds_.set_u16(static_cast<std::uint16_t>(g::kExitResult + 2 * k), ds_.u16(static_cast<std::uint16_t>(r + 4 + 2 * k)));
            return Flow::Continue;
        }
    }
    ds_.set_u16(g::kExitResult, 0xFFFF);
    return Flow::Continue;
}

// 0x31: debug breakpoint (int 3), a no-op under DOS.
Vm::Flow Vm::op_breakpoint() {
    ip_ = static_cast<std::uint16_t>(ip_ + 3);
    return Flow::Continue;
}

// sub_1266C: shown palette = base palette tinted by ds:320h/322h/324h,
// clamped to 0..63 (subtracting when ds:326h bit 0x80 is set).
void Vm::rebuild_palette() {
    const bool subtract = ds_.u16(g::kTintMode) & 0x80;
    const std::uint8_t delta[3] = {ds_.u8(g::kTint), ds_.u8(g::kTint + 2), ds_.u8(g::kTint + 4)};
    for (int i = 0; i < 768; ++i) {
        const std::uint8_t base = ds_.u8(static_cast<std::uint16_t>(g::kBasePalette + i));
        std::uint8_t c = static_cast<std::uint8_t>(subtract ? base - delta[i % 3] : base + delta[i % 3]);
        if (c & 0x80) c = 0;
        if (c & 0x40) c = 0x3F;
        ds_.set_u8(static_cast<std::uint16_t>(g::kShownPalette + i), c);
    }
}

Vm::Flow Vm::op_tint() {
    for (int k = 0; k < 3; ++k) ds_.set_u16(static_cast<std::uint16_t>(g::kTint + 2 * k), fetch8());
    rebuild_palette();
    return Flow::Continue;
}

Vm::Flow Vm::op_tint_reset() {
    for (int k = 0; k < 3; ++k) ds_.set_u16(static_cast<std::uint16_t>(g::kTint + 2 * k), 0);
    rebuild_palette();
    return Flow::Continue;
}

// 0x36/0x37: set or clear part flag 0x40 on every sprite part. The original
// loops 65536 times when the actor has no parts; the port skips instead.
template <bool Hide>
Vm::Flow Vm::op_parts_flag() {
    const int s = self();
    const int count = ds_.field(f::kPartCount, s);
    int p = ds_.field(f::kFirstPart, s);
    for (int i = 0; i < count; ++i, p += 2) {
        const std::uint16_t fl = ds_.field(part::kFlags, p);
        ds_.set_field(part::kFlags, p, Hide ? (fl | 0x40) : (fl & ~0x40));
        ds_.set_field(part::kRedraw, p, Hide ? 0x200 : 0x202);
    }
    return Flow::Continue;
}

// 0x38: one-shot move to (opA, opB), applied by sub_11260.
Vm::Flow Vm::op_teleport() {
    const std::uint8_t mode = fetch8();
    const std::uint16_t a = read_operand(mode & 7), b = read_operand((mode >> 3) & 7);
    const int s = self();
    ds_.set_field(f::kMoveX, s, static_cast<std::uint16_t>(a - ds_.field(field::kX, s)));
    ds_.set_field(f::kMoveY, s, static_cast<std::uint16_t>(b - ds_.field(field::kY, s)));
    ds_.set_field(f::kMoveMode, s, 0x100);
    return Flow::Continue;
}

// 0x39/0x3A: does pixel (opA, opB) hit a listed class (cell, then actors)?
template <bool Negate>
Vm::Flow Vm::op_point_probe() {
    const std::uint8_t mode = fetch8();
    const std::uint16_t x = read_operand(mode & 7), y = read_operand((mode >> 3) & 7);
    ds_.set_u16(g::kScratchX, x);
    ds_.set_u16(g::kScratchY, y);
    const std::uint8_t list = fetch8();
    ds_.set_u16(ds_addr::kProbeSlot, 0xFFFF);
    const int px = static_cast<std::int16_t>(x), py = static_cast<std::int16_t>(y);
    const bool hit = scan_row(list, py, px, px) || probe_point_actor(list, px, py);
    const std::uint16_t target = fetch16();
    if (hit != Negate) jump(target);
    return Flow::Continue;
}

}  // namespace bt
