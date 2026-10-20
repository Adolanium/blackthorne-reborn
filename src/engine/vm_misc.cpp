// Sound triggers, text boxes and portraits, screen shake, passwords and
// program exit. Semantics: docs/vm/ops_00-3F.md (0x02) and ops_C0-FF.md.

#include <stdexcept>

#include "engine/vm.h"

namespace bt {
namespace {

namespace g {
constexpr std::uint16_t kTmp10 = 0x10, kTextW = 0x12, kTextH = 0x14, kTextX = 0x16, kTextY = 0x18, kTextPtr = 0x1A;
constexpr std::uint16_t kScratchX = 0x26, kScratchY = 0x28;
constexpr std::uint16_t kShakeMaskX = 0x340, kShakeMaskY = 0x342, kShakeTicksX = 0x348, kShakeTicksY = 0x34A;
constexpr std::uint16_t kTextStyle = 0x36A, kPortraitX = 0x36C, kPortraitY = 0x36E, kPortraitShown = 0x370;
constexpr std::uint16_t kPassword = 0x372;  // four words, one char each
constexpr std::uint16_t kPasswordFlag = 0x37A, kNoSave = 0x304;
constexpr std::uint16_t kCameraX = 0x1B4E, kCameraY = 0x1B50;
constexpr std::uint16_t kLevelState = 0x318;
constexpr std::uint16_t kExitResult = 0x1CA8;  // next level, x, y
constexpr std::uint16_t kCurrentLevel = 0x1CA4;
constexpr std::uint16_t kPasswordTable = 0x543A;
constexpr std::uint16_t kTextLayer = 0x2D08, kTileDirty = 0x4308;
constexpr std::uint16_t kPortraitPalettes = 0x5560;
constexpr std::uint16_t kUnknown1B46 = 0x1B46;
constexpr std::uint16_t kLastPassword = 0x64E7;
}  // namespace g

// Portrait part: pool part 2 (byte index 4).
namespace portrait {
constexpr std::uint16_t kFlags = 0x456, kX = 0x556, kY = 0x656, kData = 0x756, kSeg = 0x856, kSize = 0x956;
constexpr std::uint16_t kRedraw = 0xE56, kColour = 0xF56;
}  // namespace portrait

constexpr int kPortraitResource = 0x4D;

}  // namespace

void Vm::register_misc_ops() {
    table_[0x02] = &Vm::op_sound;
    table_[0xE6] = &Vm::op_text_near_actor;
    table_[0xE7] = &Vm::op_text_at;
    table_[0xE8] = &Vm::op_put_char;
    table_[0xE9] = &Vm::op_set_1b46;
    table_[0xEA] = &Vm::op_clear_text;
    table_[0xEB] = &Vm::op_portrait_update;
    table_[0xEC] = &Vm::op_portrait_update_ec;
    table_[0xED] = &Vm::op_shake<false>;
    table_[0xEE] = &Vm::op_shake<true>;
    table_[0xF3] = &Vm::op_sound_stop;
    table_[0xF4] = &Vm::op_music<true>;
    table_[0xF5] = &Vm::op_music<false>;
    table_[0xF6] = &Vm::op_apply_password;
    table_[0xF7] = &Vm::op_save_password;
    table_[0xF9] = &Vm::op_fatal;
    table_[0xFA] = &Vm::op_quit;
    table_[0xFB] = &Vm::op_load_password;
    for (int op = 0xFC; op <= 0xFF; ++op) table_[op] = &Vm::op_nop;
}

// 0x02: play sound effect id (the second byte is unused).
Vm::Flow Vm::op_sound() {
    const std::uint8_t id = fetch8();
    fetch8();
    events_.push_back({VmEvent::Kind::PlaySound, id});
    return Flow::Continue;
}

Vm::Flow Vm::op_sound_stop() {
    events_.push_back({VmEvent::Kind::StopSound, fetch8()});
    return Flow::Continue;
}

template <bool Restart>
Vm::Flow Vm::op_music() {
    fetch8();
    events_.push_back({Restart ? VmEvent::Kind::MusicRestart : VmEvent::Kind::MusicStop, 0});
    return Flow::Continue;
}

// sub_15C57 + sub_15C82: look up text record `text` and apply the box style.
void Vm::prepare_text(std::uint16_t text, std::uint16_t style) {
    const Bytes& t = m_.text;
    const std::size_t ptr = static_cast<std::size_t>(t.at(text * 2u) | (t.at(text * 2u + 1) << 8));
    ds_.set_u16(g::kTextW, t.at(ptr));
    ds_.set_u16(g::kTextH, t.at(ptr + 1));
    ds_.set_u16(g::kTextPtr, static_cast<std::uint16_t>(ptr + 2));
    ds_.set_u16(g::kTextStyle, style);
    auto add = [&](std::uint16_t a, int v) { ds_.set_u16(a, static_cast<std::uint16_t>(ds_.u16(a) + v)); };
    if (style & 0xC000) {  // portrait left (0x4000) or right (0x8000)
        ds_.set_u16(g::kTextX, (style & 0x4000) ? 6 : 1);
        ds_.set_u16(g::kTextY, 1);
        add(g::kTextW, 7);
        const int h = ds_.u16(g::kTextH) + 2;
        ds_.set_u16(g::kTextH, static_cast<std::uint16_t>(h < 6 ? 6 : h));
    } else if (style & 0x2000) {  // frame only
        ds_.set_u16(g::kTextX, 1);
        ds_.set_u16(g::kTextY, 1);
        add(g::kTextW, 2);
        add(g::kTextH, 2);
    } else {
        ds_.set_u16(g::kTextX, 0);
        ds_.set_u16(g::kTextY, 0);
    }
}

// sub_15D83: border tiles 1/2/3, 4/0/5, 6/7/8; blank the 4x4 portrait area.
void Vm::draw_text_box(int col, int row) {
    const std::uint16_t style = ds_.u16(g::kTextStyle);
    if (!(style & 0xE000)) return;
    const int w = ds_.u16(g::kTextW), h = ds_.u16(g::kTextH);
    auto put = [&](int c, int r, std::uint16_t v) {
        ds_.set_u16(static_cast<std::uint16_t>(g::kTextLayer + (r * 32 + c) * 2), v);
    };
    put(col, row, 1);
    for (int c = 1; c < w - 1; ++c) put(col + c, row, 2);
    put(col + w - 1, row, 3);
    for (int r = 1; r < h - 1; ++r) {
        put(col, row + r, 4);
        for (int c = 1; c < w - 1; ++c) put(col + c, row + r, 0);
        put(col + w - 1, row + r, 5);
    }
    put(col, row + h - 1, 6);
    for (int c = 1; c < w - 1; ++c) put(col + c, row + h - 1, 7);
    put(col + w - 1, row + h - 1, 8);
    if (style & 0xC000) {
        const int pc = (style & 0x4000) ? col + 1 : col + w - 5;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) put(pc + c, row + 1 + r, 0x20);
    }
}

// sub_15E51: write the characters (0x0D = newline) and place the portrait.
void Vm::draw_text(int col, int row) {
    const std::uint16_t style = ds_.u16(g::kTextStyle);
    ds_.set_u16(g::kPortraitY, static_cast<std::uint16_t>((row + 1) * 8));
    ds_.set_u16(g::kPortraitX, static_cast<std::uint16_t>(((style & 0x4000) ? col + 1 : col + ds_.u16(g::kTextW) - 5) * 8));
    const int r0 = row - 1 + ds_.u16(g::kTextY);
    std::uint16_t line = static_cast<std::uint16_t>(g::kTextLayer + (r0 * 32 + col + ds_.u16(g::kTextX)) * 2);
    std::size_t p = ds_.u16(g::kTextPtr);
    const Bytes& t = m_.text;
    for (;;) {
        line = static_cast<std::uint16_t>(line + 0x40);
        std::uint16_t d = line;
        for (;;) {
            const std::uint8_t c = t.at(p++);
            if (c == 0) return;
            if (c == 0x0D) break;
            ds_.set_u16(d, static_cast<std::uint16_t>(c - 0x20));
            d = static_cast<std::uint16_t>(d + 2);
        }
    }
}

// sub_15EB5: hide the current portrait; show the one the style asks for.
void Vm::portrait_update() {
    if (ds_.u16(g::kPortraitShown)) {
        ds_.set_u16(portrait::kFlags, ds_.u16(portrait::kFlags) & 0xFF7F);
        ds_.set_u16(portrait::kRedraw, 0x300);
        const int slot = (((ds_.u16(portrait::kColour) >> 4) - 8) & 7) * 2;
        const std::uint16_t a = static_cast<std::uint16_t>(0x1C6A + slot);
        ds_.set_u16(a, static_cast<std::uint16_t>(ds_.u16(a) - 1));
    }
    ds_.set_u16(g::kPortraitShown, 0);
    const std::uint16_t style = ds_.u16(g::kTextStyle);
    if (!(style & 0xC000)) return;
    ds_.set_u16(g::kPortraitShown, style);
    const int k = style & 0x1FFF;
    const int slot = acquire_palette(ds_.u16(static_cast<std::uint16_t>(g::kPortraitPalettes + 2 * k)));
    const std::uint8_t c = static_cast<std::uint8_t>(((slot >> 1) + 8) << 4);
    ds_.set_u16(portrait::kColour, static_cast<std::uint16_t>(c | (c << 8)));
    ds_.set_u16(portrait::kX, static_cast<std::uint16_t>(ds_.u16(g::kPortraitX) + ds_.u16(g::kCameraX)));
    ds_.set_u16(portrait::kY, static_cast<std::uint16_t>(ds_.u16(g::kPortraitY) + ds_.u16(g::kCameraY)));
    ds_.set_u16(portrait::kFlags, ds_.u16(portrait::kFlags) | 0x382);
    ds_.set_u16(portrait::kSize, 0x20);
    std::uint16_t seg = 0, off = 0;
    alloc_frame_buffer(kPortraitResource, seg, off);
    ds_.set_u16(portrait::kData, static_cast<std::uint16_t>(off + 0x480 * k));
    ds_.set_u16(portrait::kSeg, seg);
}

// E6 (sub_15F87): text box near the actor, clamped to the screen.
Vm::Flow Vm::op_text_near_actor() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t text = read_operand(m1 & 7), style = read_operand((m1 >> 3) & 7);
    prepare_text(text, style);
    const std::uint8_t m2 = fetch8();
    auto cell = [&](std::uint16_t d, std::uint16_t pos, std::uint16_t cam) {
        const int v = static_cast<std::int16_t>(static_cast<std::uint16_t>((d << 3) + pos - cam));
        if (v < 0) return 2;
        return (v >> 3) < 2 ? 2 : (v >> 3);
    };
    const int s = self();
    int col = cell(read_operand(m2 & 7), ds_.field(field::kX, s), ds_.u16(g::kCameraX));
    int row = cell(read_operand((m2 >> 3) & 7), ds_.field(field::kY, s), ds_.u16(g::kCameraY));
    const int w = ds_.u16(g::kTextW), h = ds_.u16(g::kTextH);
    if (col + w >= 0x1F) col = 0x1E - w < 0 ? 0 : 0x1E - w;
    if (row + h >= 0x1A) row = 0x19 - h < 0 ? 0 : 0x19 - h;
    ds_.set_u16(g::kScratchX, static_cast<std::uint16_t>(col));
    draw_text_box(col, row);
    draw_text(col, row);
    return Flow::Continue;
}

// E7 (sub_15F96): text box at a text-layer cell.
Vm::Flow Vm::op_text_at() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t text = read_operand(m1 & 7), style = read_operand((m1 >> 3) & 7);
    prepare_text(text, style);
    const std::uint8_t m2 = fetch8();
    const std::uint16_t col = read_operand(m2 & 7);
    ds_.set_u16(g::kScratchX, col);
    const std::uint16_t row = read_operand((m2 >> 3) & 7);
    draw_text_box(col, row);
    draw_text(col, row);
    return Flow::Continue;
}

void Vm::show_text_block(std::uint16_t addr) {
    auto w = [&](int o) { return ds_.u16(static_cast<std::uint16_t>(addr + o)); };
    prepare_text(w(1), w(3));
    draw_text_box(w(6), w(8));
    draw_text(w(6), w(8));
    portrait_update();
}

// E8: put one character (no change flag, as the original).
Vm::Flow Vm::op_put_char() {
    const std::uint8_t m1 = fetch8();
    const std::uint16_t col = read_operand(m1 & 7), row = read_operand((m1 >> 3) & 7);
    const std::uint8_t m2 = fetch8();
    const std::uint16_t ch = read_operand(m2 & 7);
    ds_.set_u16(static_cast<std::uint16_t>(g::kTextLayer + (row * 32 + col) * 2), static_cast<std::uint16_t>(ch - 0x20));
    return Flow::Continue;
}

Vm::Flow Vm::op_set_1b46() {
    ds_.set_u16(g::kUnknown1B46, fetch16());
    return Flow::Continue;
}

// EA (sub_15F4A): clear the text layer and remove the portrait.
Vm::Flow Vm::op_clear_text() {
    ds_.set_u16(g::kTextStyle, 0);
    for (int i = 0; i < 0x380; ++i) {
        const std::uint16_t a = static_cast<std::uint16_t>(g::kTextLayer + 2 * i);
        if (ds_.u16(a) != 0x20) {
            const std::uint16_t d = static_cast<std::uint16_t>(g::kTileDirty + 2 * i);
            ds_.set_u16(d, ds_.u16(d) | 3);
            ds_.set_u16(a, 0x20);
        }
    }
    portrait_update();
    return Flow::Continue;
}

Vm::Flow Vm::op_portrait_update() {
    portrait_update();
    return Flow::Continue;
}

Vm::Flow Vm::op_portrait_update_ec() {
    ds_.set_u16(g::kLevelState, ds_.u16(g::kLevelState) | 4);
    portrait_update();
    return Flow::Continue;
}

// ED / EE: horizontal / vertical screen shake (mask, ticks).
template <bool Vertical>
Vm::Flow Vm::op_shake() {
    const std::uint8_t mask = fetch8(), ticks = fetch8();
    ds_.set_u16(Vertical ? g::kShakeMaskY : g::kShakeMaskX, mask);
    ds_.set_u16(Vertical ? g::kShakeTicksY : g::kShakeTicksX, ticks);
    return Flow::Continue;
}

// F6 (sub_10B92): look up the current password; set next level and position.
Vm::Flow Vm::op_apply_password() {
    auto entry = [&](int e) { return static_cast<std::uint16_t>(g::kPasswordTable + e * 10); };
    int found = -1;
    for (int e = 0; ds_.u16(entry(e)) != 0xFFFF; ++e) {
        bool match = true;
        for (int k = 0; k < 4; ++k)
            match &= ds_.u8(static_cast<std::uint16_t>(entry(e) + 6 + k)) == ds_.u8(static_cast<std::uint16_t>(g::kPassword + 2 * k));
        if (match) { found = e; break; }
    }
    if (found < 0) {
        found = 0;
        for (int k = 0; k < 4; ++k)
            ds_.set_u16(static_cast<std::uint16_t>(g::kPassword + 2 * k), ds_.u8(static_cast<std::uint16_t>(entry(0) + 6 + k)));
    }
    for (int k = 0; k < 3; ++k)
        ds_.set_u16(static_cast<std::uint16_t>(g::kExitResult + 2 * k), ds_.u16(static_cast<std::uint16_t>(entry(found) + 2 * k)));
    ds_.set_u16(g::kPasswordFlag, 0);
    return Flow::Continue;
}

std::array<char, 4> Vm::current_password() const {
    std::array<char, 4> pw{};
    for (int k = 0; k < 4; ++k) pw[k] = static_cast<char>(ds_.u8(static_cast<std::uint16_t>(g::kPassword + 2 * k)));
    return pw;
}

// F7 (sub_10C0D): remember the current level's password and save it.
Vm::Flow Vm::op_save_password() {
    auto entry = [&](int e) { return static_cast<std::uint16_t>(g::kPasswordTable + e * 10); };
    for (int e = 0; static_cast<std::int16_t>(ds_.u16(entry(e))) >= 0; ++e) {
        if (ds_.u16(entry(e)) != ds_.u16(g::kCurrentLevel)) continue;
        for (int k = 0; k < 4; ++k)
            ds_.set_u16(static_cast<std::uint16_t>(g::kPassword + 2 * k), ds_.u8(static_cast<std::uint16_t>(entry(e) + 6 + k)));
        if (ds_.u16(g::kNoSave) == 0) {
            for (int k = 0; k < 4; ++k)
                ds_.set_u8(static_cast<std::uint16_t>(g::kLastPassword + k), ds_.u8(static_cast<std::uint16_t>(entry(e) + 6 + k)));
            if (ds_.u16(entry(e)) != 0xC0) events_.push_back({VmEvent::Kind::SavePassword, 0});
        }
        break;
    }
    return Flow::Continue;
}

// FB (sub_10B07): load the saved password (the host fills it in), then F6.
Vm::Flow Vm::op_load_password() {
    std::array<char, 4> pw = {'S', 'T', 'R', 'T'};
    if (load_password_) {
        std::array<char, 4> saved{};
        if (load_password_(saved)) pw = saved;
    }
    for (int k = 0; k < 4; ++k) ds_.set_u16(static_cast<std::uint16_t>(g::kPassword + 2 * k), static_cast<std::uint8_t>(pw[k]));
    return op_apply_password();
}

Vm::Flow Vm::op_fatal() { throw std::runtime_error("script assertion failed (error 0x25)"); }

Vm::Flow Vm::op_quit() {
    events_.push_back({VmEvent::Kind::Quit, 0});
    return Flow::EndTurn;
}

}  // namespace bt
