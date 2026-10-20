// Animation scripts, sprite parts, frame loading and per-tick physics.
// Semantics: docs/vm/ops_C0-FF.md ("Part model") and docs/vm/ops_00-3F.md (0x2A).

#include <stdexcept>

#include "engine/vm.h"

namespace bt {
namespace {

constexpr int kSelf = 1, kGlobal = 2, kTarget = 3, kRelated = 4;

namespace g {
constexpr std::uint16_t kAnimWait = 0x2E;     // set by D7, stored into 193Ah
constexpr std::uint16_t kFirstPart = 0x34;    // while an animation runs
constexpr std::uint16_t kPartCount = 0x36;
constexpr std::uint16_t kPartFilter = 0x33A;  // D5 group filter
constexpr std::uint16_t kSmallPart = 0x350, kLargePart = 0x352;
constexpr std::uint16_t kAccelX = 0x1CEE, kGravity = 0x1CF0;  // level header words 0 and 1
constexpr std::uint16_t kMaxVelX = 0x1C8A, kMaxVelY = 0x1C8C;
constexpr std::uint16_t kBankIds = 0x1152, kBankHandles = 0x1172, kBankFrameSize = 0x1192;
}  // namespace g

namespace f {
constexpr std::uint16_t kFrameBuffer = 0x16BA, kFrameSeg = 0x18C2, kFrame = 0x1782, kBank = 0x19DA;
constexpr std::uint16_t kAnimIp = 0x18EA, kAnimSeg = 0x1912, kAnimDelay = 0x193A, kAnimLink = 0x1962;
constexpr std::uint16_t kFirstPart = 0x198A, kPartCount = 0x19B2;
constexpr std::uint16_t kMoveMode = 0x134A, kMoveX = 0x17AA, kMoveY = 0x17D2;
constexpr std::uint16_t kVelX = 0x1502, kVelY = 0x152A;
}  // namespace f

namespace part {
constexpr std::uint16_t kFlags = 0x452, kX = 0x552, kY = 0x652, kData = 0x752, kSeg = 0x852;
constexpr std::uint16_t kSize = 0x952, kRedraw = 0xE52, kColour = 0xF52, kGroup = 0x1052;
}  // namespace part

constexpr std::uint16_t kFlipV = 0x80;
constexpr std::uint16_t kRedraw = 0x202;

}  // namespace

void Vm::register_anim_ops() {
    table_[0x2A] = &Vm::op_anim_tick;
    table_[0xC4] = &Vm::op_vflip_add<kSelf, false>;
    table_[0xC5] = &Vm::op_vflip_add<kGlobal, false>;
    table_[0xC6] = &Vm::op_vflip_add<kTarget, false>;
    table_[0xC7] = &Vm::op_vflip_add<kRelated, false>;
    table_[0xC8] = &Vm::op_vflip_add<kSelf, true>;
    table_[0xC9] = &Vm::op_vflip_add<kGlobal, true>;
    table_[0xCA] = &Vm::op_vflip_add<kTarget, true>;
    table_[0xCB] = &Vm::op_vflip_add<kRelated, true>;
    table_[0xCF] = &Vm::op_part_cells;
    table_[0xD0] = &Vm::op_velocity<false>;
    table_[0xD1] = &Vm::op_part_offsets<false>;
    table_[0xD2] = &Vm::op_velocity<true>;
    table_[0xD3] = &Vm::op_part_offsets<true>;
    table_[0xD4] = &Vm::op_part_palettes;
    table_[0xD5] = &Vm::op_part_filter;
    table_[0xD6] = &Vm::op_anim_end_tick;
    table_[0xD7] = &Vm::op_anim_wait;
    table_[0xD8] = &Vm::op_part_flip<0x20>;
    table_[0xD9] = &Vm::op_part_flip<0x10>;
    table_[0xDA] = &Vm::op_part_flip<0x30>;
    table_[0xDB] = &Vm::op_part_groups;
    table_[0xDC] = &Vm::op_frame;
    table_[0xDD] = &Vm::op_part_sizes;
    table_[0xDE] = &Vm::op_part_priority;
    table_[0xDF] = &Vm::op_select_bank;
    table_[0xE0] = &Vm::op_part_hide;
    table_[0xE1] = &Vm::op_part_show;
    table_[0xE2] = &Vm::op_part_xor;
    table_[0xE4] = &Vm::op_if_anim_waiting;
}

// 0x2A (sub_1205B): run one animation step and the velocity physics, keeping
// the main script's position and link register.
Vm::Flow Vm::op_anim_tick() {
    const std::uint16_t seg = seg_, ip = ip_;
    const std::uint16_t link_ip = ds_.u16(ds_addr::kLinkIp), link_seg = ds_.u16(ds_addr::kLinkSeg);
    ds_.set_u16(g::kPartFilter, 0);
    anim_tick(self());
    physics(self());
    seg_ = seg;
    ip_ = ip;
    ds_.set_u16(ds_addr::kLinkIp, link_ip);
    ds_.set_u16(ds_addr::kLinkSeg, link_seg);
    return Flow::Continue;
}

// sub_1207C: count down the animation delay; when it runs out, interpret the
// animation script until D6/D7 ends the tick.
void Vm::anim_tick(int s) {
    if (ds_.field(f::kAnimIp, s) == 0xFFFF) return;
    std::uint16_t wait = ds_.field(f::kAnimDelay, s);
    if (wait != 0 && --wait != 0) {
        ds_.set_field(f::kAnimDelay, s, wait);
        return;
    }
    ds_.set_u16(g::kAnimWait, 0);
    ds_.set_u16(ds_addr::kLinkIp, ds_.field(f::kAnimLink, s));
    ds_.set_u16(ds_addr::kLinkSeg, ds_.field(f::kAnimSeg, s));
    ds_.set_u16(g::kFirstPart, ds_.field(f::kFirstPart, s));
    ds_.set_u16(g::kPartCount, ds_.field(f::kPartCount, s));
    seg_ = ds_.field(f::kAnimSeg, s);
    ip_ = ds_.field(f::kAnimIp, s);
    execute();
    ds_.set_field(f::kAnimIp, s, ip_);
    ds_.set_field(f::kAnimLink, s, ds_.u16(ds_addr::kLinkIp));
    ds_.set_field(f::kAnimDelay, s, ds_.u16(g::kAnimWait));
}

// loc_12404: for actors in velocity mode, apply acceleration and gravity,
// clamp, and add this tick's velocity to the displacement accumulators.
void Vm::physics(int s) {
    if (static_cast<std::int16_t>(ds_.field(f::kMoveMode, s)) >= 0) return;
    const std::uint16_t flags = ds_.field(field::kFlags, s);
    if (flags & 0x8000) ds_.set_field(f::kVelY, s, static_cast<std::uint16_t>(ds_.field(f::kVelY, s) + ds_.u16(g::kGravity)));
    if (flags & 0x4000) ds_.set_field(f::kVelX, s, static_cast<std::uint16_t>(ds_.field(f::kVelX, s) + ds_.u16(g::kAccelX)));
    auto clamp = [&](std::uint16_t v, std::uint16_t limit_addr) {
        const int lim = static_cast<std::int16_t>(ds_.u16(limit_addr));
        int a = static_cast<std::int16_t>(v);
        const bool neg = a < 0;
        // neg / cmp / jl / mov / neg, exactly (0x8000 stays 0x8000 under neg).
        std::int16_t m = static_cast<std::int16_t>(neg ? static_cast<std::uint16_t>(-a) : static_cast<std::uint16_t>(a));
        if (!(m < lim)) m = static_cast<std::int16_t>(lim);
        return static_cast<std::uint16_t>(neg ? static_cast<std::uint16_t>(-m) : static_cast<std::uint16_t>(m));
    };
    const std::uint16_t vx = clamp(ds_.field(f::kVelX, s), g::kMaxVelX);
    const std::uint16_t vy = clamp(ds_.field(f::kVelY, s), g::kMaxVelY);
    ds_.set_field(f::kVelX, s, vx);
    ds_.set_field(f::kVelY, s, vy);
    const std::uint16_t dx = (flags & flag::kFacing) ? static_cast<std::uint16_t>(-vx) : vx;
    const std::uint16_t dy = (flags & kFlipV) ? static_cast<std::uint16_t>(-vy) : vy;
    ds_.set_field(f::kMoveX, s, static_cast<std::uint16_t>(ds_.field(f::kMoveX, s) + dx));
    ds_.set_field(f::kMoveY, s, static_cast<std::uint16_t>(ds_.field(f::kMoveY, s) + dy));
}

// sub_15BF2: copy frame N of the actor's bank into its frame buffer as rows of
// [mask byte][8 pixels], mask bit 7 = first pixel non-zero.
void Vm::load_frame(int s, std::uint16_t frame) {
    const std::uint16_t bank = ds_.field(f::kBank, s);
    const std::uint32_t size = ds_.u16(static_cast<std::uint16_t>(g::kBankFrameSize + bank));
    const std::size_t slot = ds_.u16(static_cast<std::uint16_t>(g::kBankHandles + bank));
    if (slot >= m_.banks.size()) throw std::runtime_error("frame load: bad bank");
    const Bytes& src = m_.banks[slot];
    std::size_t in = static_cast<std::size_t>(frame) * size;
    if (in + size > src.size()) throw std::runtime_error("frame load: frame out of range");
    if (ds_.field(f::kFrameSeg, s) != Machine::kFrameSeg) throw std::runtime_error("frame load: no frame buffer");
    std::uint16_t out = ds_.field(f::kFrameBuffer, s);
    for (std::uint32_t n = 0; n < size; n += 8) {
        std::uint8_t mask = 0;
        for (int k = 0; k < 8; ++k) {
            const std::uint8_t px = src[in++];
            m_.frames[static_cast<std::uint16_t>(out + 1 + k)] = px;
            mask = static_cast<std::uint8_t>((mask << 1) | (px != 0));
        }
        m_.frames[out] = mask;
        out = static_cast<std::uint16_t>(out + 9);
    }
}

void Vm::mirror_parts(int s, bool vertical) {
    const std::uint16_t c2 = static_cast<std::uint16_t>(2 * ds_.field(vertical ? field::kY : field::kX, s));
    const std::uint16_t pos = vertical ? part::kY : part::kX;
    const int count = ds_.field(f::kPartCount, s);
    int p = ds_.field(f::kFirstPart, s);
    for (int i = 0; i < count; ++i, p += 2)
        ds_.set_field(pos, p, static_cast<std::uint16_t>(c2 - ds_.field(pos, p) - ds_.field(part::kSize, p)));
}

// 0xC4-0xC7 subtract when vertically flipped else add; 0xC8-0xCB the reverse.
template <int Dest, bool AddWhenFlipped>
Vm::Flow Vm::op_vflip_add() {
    const std::uint16_t addr = dest_addr(Dest);
    const bool flipped = ds_.field(field::kFlags, self()) & kFlipV;
    const std::uint16_t v = ds_.u16(addr);
    ds_.set_u16(addr, static_cast<std::uint16_t>(flipped == AddWhenFlipped ? v + acc() : v - acc()));
    return Flow::Continue;
}

// Iterates the animation's parts (ds:34h, count ds:36h). With `filtered`, only
// parts whose group mask matches ds:33Ah are visited (and consume operands).
template <typename Fn>
static void for_parts(DataSegment& ds, bool filtered, Fn fn) {
    const int count = ds.u16(g::kPartCount);
    const std::uint16_t filter = filtered ? ds.u16(g::kPartFilter) : 0;
    int p = ds.u16(g::kFirstPart);
    for (int i = 0; i < count; ++i, p += 2) {
        if (filter && !(ds.field(part::kGroup, p) & filter)) continue;
        fn(p);
    }
}

Vm::Flow Vm::op_part_cells() {
    const int s = self();
    for_parts(ds_, true, [&](int p) {
        ds_.set_field(part::kData, p, static_cast<std::uint16_t>(ds_.field(f::kFrameBuffer, s) + fetch8() * 0x48));
        ds_.set_field(part::kSeg, p, ds_.field(f::kFrameSeg, s));
        ds_.set_field(part::kRedraw, p, kRedraw);
    });
    return Flow::Continue;
}

// 0xD0 / 0xD2: add a signed byte (x256) to this tick's displacement, mirrored
// by the facing / vertical-flip flag.
template <bool Vertical>
Vm::Flow Vm::op_velocity() {
    const auto v = static_cast<std::int8_t>(fetch8());
    const int s = self();
    const bool mirrored = ds_.field(field::kFlags, s) & (Vertical ? kFlipV : flag::kFacing);
    const std::uint16_t d = static_cast<std::uint16_t>((mirrored ? -v : v) * 256);
    const std::uint16_t fld = Vertical ? f::kMoveY : f::kMoveX;
    ds_.set_field(fld, s, static_cast<std::uint16_t>(ds_.field(fld, s) + d));
    return Flow::Continue;
}

// 0xD1 / 0xD3: per-part offsets from the actor origin, mirrored when flipped.
template <bool Vertical>
Vm::Flow Vm::op_part_offsets() {
    const int s = self();
    const std::uint16_t origin = ds_.field(Vertical ? field::kY : field::kX, s);
    const std::uint16_t pos = Vertical ? part::kY : part::kX;
    for_parts(ds_, false, [&](int p) {
        ds_.set_field(pos, p, static_cast<std::uint16_t>(origin + fetch16()));
        ds_.set_field(part::kRedraw, p, kRedraw);
    });
    if (ds_.field(field::kFlags, s) & (Vertical ? kFlipV : flag::kFacing)) {
        mirror_parts(s, Vertical);
        if (Vertical) {  // sub_1110D sets (does not toggle) the vertical flip bit
            const int count = ds_.field(f::kPartCount, s);
            int p = ds_.field(f::kFirstPart, s);
            for (int i = 0; i < count; ++i, p += 2) ds_.set_field(part::kFlags, p, ds_.field(part::kFlags, p) | 0x10);
        }
    }
    return Flow::Continue;
}

Vm::Flow Vm::op_part_palettes() {
    for_parts(ds_, false, [&](int p) {
        const std::uint8_t c = static_cast<std::uint8_t>((fetch8() + 8) << 4);
        ds_.set_field(part::kColour, p, static_cast<std::uint16_t>(c | (c << 8)));
        ds_.set_field(part::kRedraw, p, kRedraw);
    });
    return Flow::Continue;
}

Vm::Flow Vm::op_part_filter() {
    ds_.set_u16(g::kPartFilter, fetch8());
    return Flow::Continue;
}

Vm::Flow Vm::op_anim_end_tick() { return Flow::EndTurn; }

Vm::Flow Vm::op_anim_wait() {
    ds_.set_u16(g::kAnimWait, fetch8());
    return Flow::EndTurn;
}

template <std::uint16_t Bits>
Vm::Flow Vm::op_part_flip() {
    for_parts(ds_, false, [&](int p) {
        ds_.set_field(part::kFlags, p, ds_.field(part::kFlags, p) ^ Bits);
        ds_.set_field(part::kRedraw, p, kRedraw);
    });
    return Flow::Continue;
}

Vm::Flow Vm::op_part_groups() {
    for_parts(ds_, true, [&](int p) { ds_.set_field(part::kGroup, p, fetch8()); });
    return Flow::Continue;
}

// 0xDC (sub_1201E): show frame N of the selected bank.
Vm::Flow Vm::op_frame() {
    const std::uint8_t mode = fetch8();
    const std::uint16_t n = read_operand(mode & 7);
    const int s = self();
    if (n == ds_.field(f::kFrame, s)) return Flow::Continue;
    ds_.set_field(f::kFrame, s, n);
    load_frame(s, n);
    for_parts(ds_, false, [&](int p) { ds_.set_field(part::kRedraw, p, kRedraw); });
    return Flow::Continue;
}

// 0xDD (sub_11FF4): choose the small or large part size per part.
Vm::Flow Vm::op_part_sizes() {
    for_parts(ds_, false, [&](int p) {
        const std::uint16_t fl = ds_.field(part::kFlags, p);
        if (fetch8()) {
            ds_.set_field(part::kSize, p, ds_.u16(g::kLargePart));
            ds_.set_field(part::kFlags, p, fl | 2);
        } else {
            ds_.set_field(part::kSize, p, ds_.u16(g::kSmallPart));
            ds_.set_field(part::kFlags, p, fl & ~2);
        }
        ds_.set_field(part::kRedraw, p, kRedraw);
    });
    return Flow::Continue;
}

Vm::Flow Vm::op_part_priority() {
    const std::uint8_t v = fetch8();
    for_parts(ds_, false, [&](int p) {
        ds_.set_field(part::kFlags, p, static_cast<std::uint16_t>((ds_.field(part::kFlags, p) & 0xFCFF) | (v << 8)));
        ds_.set_field(part::kRedraw, p, kRedraw);
    });
    return Flow::Continue;
}

// 0xDF: select the sprite bank holding resource `id` (0 when absent).
Vm::Flow Vm::op_select_bank() {
    const std::uint16_t id = fetch16();
    std::uint16_t bank = 0;
    for (int i = 15; i >= 0; --i)
        if (ds_.u16(static_cast<std::uint16_t>(g::kBankIds + 2 * i)) == id) {
            bank = static_cast<std::uint16_t>(2 * i);
            break;
        }
    ds_.set_field(f::kBank, self(), bank);
    return Flow::Continue;
}

Vm::Flow Vm::op_part_hide() {
    for_parts(ds_, true, [&](int p) {
        if (fetch8() & 1) {
            ds_.set_field(part::kFlags, p, ds_.field(part::kFlags, p) | 0x40);
            ds_.set_field(part::kRedraw, p, 0x200);
        }
    });
    return Flow::Continue;
}

Vm::Flow Vm::op_part_show() {
    for_parts(ds_, false, [&](int p) {
        if (fetch8() & 1) {
            ds_.set_field(part::kFlags, p, ds_.field(part::kFlags, p) & ~0x40);
            ds_.set_field(part::kRedraw, p, kRedraw);
        }
    });
    return Flow::Continue;
}

Vm::Flow Vm::op_part_xor() {
    for_parts(ds_, false, [&](int p) {
        ds_.set_field(part::kFlags, p, ds_.field(part::kFlags, p) ^ fetch8());
        ds_.set_field(part::kRedraw, p, kRedraw);
    });
    return Flow::Continue;
}

Vm::Flow Vm::op_if_anim_waiting() {
    const std::uint16_t target = fetch16();
    if (ds_.field(f::kAnimDelay, self()) != 0) jump(target);
    return Flow::Continue;
}

}  // namespace bt
