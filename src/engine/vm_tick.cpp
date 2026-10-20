// The per-tick actor pipeline from the main loop (sub_10043):
// scheduler, velocity integration, event-check passes, anchored moves and
// sprite parts following their actor.

#include "engine/vm.h"

namespace bt {
namespace {

namespace g {
constexpr std::uint16_t kSchedSlot = 0x2C;
constexpr std::uint16_t kRestartAll = 0x316;
constexpr std::uint16_t kQueueLen = 0x330, kQueue = 0x332;
constexpr std::uint16_t kCheckOrdinal = 0x33C, kPass = 0x33E;
constexpr std::uint16_t kTick200 = 0x200;
constexpr std::uint16_t kMaxStepX = 0x1C8A, kMaxStepY = 0x1C8C;
}  // namespace g

namespace f {
constexpr std::uint16_t kSubX = 0x1282, kSubY = 0x12AA, kPrevX = 0x12D2, kPrevY = 0x12FA;
constexpr std::uint16_t kEventBits = 0x1322, kMoveMode = 0x134A, kTimer = 0x15CA;
constexpr std::uint16_t kMoveX = 0x17AA, kMoveY = 0x17D2;
constexpr std::uint16_t kFirstPart = 0x198A, kPartCount = 0x19B2;
}  // namespace f

namespace part {
constexpr std::uint16_t kX = 0x552, kY = 0x652, kRedraw = 0xE52;
}  // namespace part

constexpr std::uint16_t kFlipV = 0x80;

}  // namespace

// sub_1603F: run one actor's main script for this tick.
void Vm::schedule_actor(int s) {
    if (!ds_.field(field::kScriptSeg, s)) return;
    if (ds_.field(field::kFlags, s) & 0x1000) {
        const std::uint16_t t = static_cast<std::uint16_t>(ds_.field(f::kTimer, s) - 1);
        ds_.set_field(f::kTimer, s, static_cast<std::int16_t>(t) < 0 ? 0 : t);
    }
    ds_.set_u16(ds_addr::kCurrentSlot, static_cast<std::uint16_t>(s));
    ds_.set_u16(g::kCheckOrdinal, 0);
    if ((ds_.field(field::kFlags, s) & 0x200) || ds_.u16(g::kRestartAll)) {
        const int type = static_cast<std::int16_t>(ds_.field(field::kType, s));
        if (type < 0) return;
        const std::size_t rec = static_cast<std::size_t>(type) * 16;
        ds_.set_field(field::kScriptIp, s, static_cast<std::uint16_t>(scripts_.at(rec + 3) | (scripts_.at(rec + 4) << 8)));
    }
    run_actor(s);
}

// sub_1601A + sub_15FCE: run every actor once; actors spawned behind the
// scheduler during this tick run right after their spawner. sub_15FF1 (after
// a screen flip) does the same for the actors marked offscreen only.
void Vm::run_scheduler(bool offscreen_only) {
    for (int s = ds_.s16(ds_addr::kActorEnd) - 2; s >= 0; s -= 2) {  // sub_1763F
        ds_.set_field(f::kMoveX, s, 0);
        ds_.set_field(f::kMoveY, s, 0);
    }
    ds_.set_u16(g::kTick200, 0);
    ds_.set_u16(g::kQueueLen, 0);
    ds_.set_u16(g::kPass, 0);
    for (int s = 0; s < ds_.s16(ds_addr::kActorEnd); s += 2) {
        if (offscreen_only) {
            if (!(ds_.field(field::kFlags, s) & 0x200)) continue;
        } else {
            ds_.set_u16(g::kSchedSlot, static_cast<std::uint16_t>(s));
        }
        schedule_actor(s);
        const int queued = ds_.u16(g::kQueueLen);
        if (queued) {
            for (int i = 0; i < queued; ++i) schedule_actor(ds_.u8(static_cast<std::uint16_t>(g::kQueue + i)));
            ds_.set_u16(g::kQueueLen, 0);
        }
    }
}

// sub_111BC: integrate this tick's 8.8 displacement for velocity-mode actors
// (sub-pixel remainder kept in 1282h/12AAh); remember the old position.
void Vm::integrate_velocity() {
    for (int s = ds_.s16(ds_addr::kActorEnd) - 2; s >= 0; s -= 2) {
        if (!ds_.field(field::kScriptSeg, s)) continue;
        ds_.set_field(f::kPrevX, s, ds_.field(field::kX, s));
        ds_.set_field(f::kPrevY, s, ds_.field(field::kY, s));
        if (static_cast<std::int16_t>(ds_.field(f::kMoveMode, s)) >= 0) continue;
        auto step = [&](std::uint16_t move, std::uint16_t sub, std::uint16_t limit_addr,
                        std::initializer_list<std::uint16_t> moved) {
            const int lim = static_cast<std::int16_t>(ds_.u16(limit_addr));
            int a = static_cast<std::int16_t>(ds_.field(move, s));
            const bool neg = a < 0;
            std::int16_t m = static_cast<std::int16_t>(neg ? static_cast<std::uint16_t>(-a) : static_cast<std::uint16_t>(a));
            if (!(m < lim)) m = static_cast<std::int16_t>(lim);
            std::uint16_t v = static_cast<std::uint16_t>(neg ? -m : m);
            // add [sub], al ; adc ah, 0 ; sar ax, 8
            const unsigned lo = (v & 0xFF) + (ds_.field(sub, s) & 0xFF);
            ds_.set_field(sub, s, static_cast<std::uint16_t>((ds_.field(sub, s) & 0xFF00) | (lo & 0xFF)));
            std::uint8_t hi = static_cast<std::uint8_t>((v >> 8) + (lo >> 8));
            v = static_cast<std::uint16_t>((hi << 8) | (v & 0xFF));
            const std::uint16_t d = static_cast<std::uint16_t>(static_cast<std::int16_t>(v) >> 8);
            ds_.set_field(move, s, d);
            for (std::uint16_t fld : moved) ds_.set_field(fld, s, static_cast<std::uint16_t>(ds_.field(fld, s) + d));
        };
        step(f::kMoveX, f::kSubX, g::kMaxStepX, {field::kX, field::kLeft, field::kRight});
        step(f::kMoveY, f::kSubY, g::kMaxStepY, {field::kY, field::kTop, field::kBottom});
    }
}

// sub_17AC7 (pass +1: terrain, velocity-mode actors, event bits cleared) and
// sub_17AB2 (pass -1: actor overlaps, every live actor).
void Vm::run_check_pass(int pass) {
    ds_.set_u16(g::kPass, static_cast<std::uint16_t>(pass));
    for (int s = 0; s < ds_.s16(ds_addr::kActorEnd); s += 2) {
        if (pass > 0) {
            ds_.set_field(f::kEventBits, s, 0);
            if (static_cast<std::int16_t>(ds_.field(f::kMoveMode, s)) >= 0) continue;
        }
        run_checks(s);
    }
    // The pass number stays set (sub_17AB2 leaves -1); the scheduler clears it.
}

// sub_11260: anchored moves (follow slot 134Ah) and one-shot moves (>= 0x100).
void Vm::apply_anchors() {
    for (int s = ds_.s16(ds_addr::kActorEnd) - 2; s >= 0; s -= 2) {
        if (!ds_.field(field::kScriptSeg, s)) continue;
        const int mode = static_cast<std::int16_t>(ds_.field(f::kMoveMode, s));
        if (mode < 0) continue;
        std::uint16_t dx, dy;
        if (mode >= 0x100) {
            dx = ds_.field(f::kMoveX, s);
            dy = ds_.field(f::kMoveY, s);
        } else {
            const std::uint16_t flags = ds_.field(field::kFlags, s);
            dx = static_cast<std::uint16_t>(ds_.field(field::kX, mode) - ds_.field(field::kX, s));
            dy = static_cast<std::uint16_t>(ds_.field(field::kY, mode) - ds_.field(field::kY, s));
            dx = static_cast<std::uint16_t>((flags & flag::kFacing) ? dx - ds_.field(f::kMoveX, s) : dx + ds_.field(f::kMoveX, s));
            dy = static_cast<std::uint16_t>((flags & kFlipV) ? dy - ds_.field(f::kMoveY, s) : dy + ds_.field(f::kMoveY, s));
            ds_.set_field(f::kMoveX, s, dx);
            ds_.set_field(f::kMoveY, s, dy);
        }
        for (std::uint16_t fld : {field::kX, field::kLeft, field::kRight}) ds_.set_field(fld, s, static_cast<std::uint16_t>(ds_.field(fld, s) + dx));
        for (std::uint16_t fld : {field::kY, field::kTop, field::kBottom}) ds_.set_field(fld, s, static_cast<std::uint16_t>(ds_.field(fld, s) + dy));
        ds_.set_field(f::kMoveMode, s, 0xFFFF);
    }
}

// sub_11F49 / sub_11F59: move every actor's parts by its movement this tick.
void Vm::move_parts() {
    for (int s = ds_.s16(ds_addr::kActorEnd) - 2; s >= 0; s -= 2) {
        if (!ds_.field(field::kScriptSeg, s) || !ds_.field(f::kPartCount, s)) continue;
        const std::uint16_t dx = static_cast<std::uint16_t>(ds_.field(field::kX, s) - ds_.field(f::kPrevX, s));
        const std::uint16_t dy = static_cast<std::uint16_t>(ds_.field(field::kY, s) - ds_.field(f::kPrevY, s));
        if (!(dx | dy)) continue;
        for (int i = 0, p = ds_.field(f::kFirstPart, s); i < ds_.field(f::kPartCount, s); ++i, p += 2) {
            ds_.set_field(part::kX, p, static_cast<std::uint16_t>(ds_.field(part::kX, p) + dx));
            ds_.set_field(part::kY, p, static_cast<std::uint16_t>(ds_.field(part::kY, p) + dy));
            ds_.set_field(part::kRedraw, p, 0x202);
        }
    }
}

}  // namespace bt
