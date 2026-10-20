#include "engine/vm.h"

#include <cstdio>
#include <stdexcept>

namespace bt {
namespace {

// Operand addressing modes (sub_17569).
enum Mode { kImm = 0, kSelf = 1, kGlobal = 2, kTarget = 3, kRelated = 4, kRandom = 5 };

// Combine operations for 0x41-0x64, in table order.
enum Op { kStore, kAdd, kSub, kAnd, kOr, kXor, kShl, kShr, kSar };

// Branch conditions for 0x65-0x82, in table order.
enum Cmp { kGe, kLt, kEq, kNe, kGeNeg, kLtNeg };

std::uint16_t combine(int op, std::uint16_t x, std::uint16_t acc) {
    const int c = acc & 0x1F;  // 386 masks 16-bit shift counts to 5 bits
    switch (op) {
        case kStore: return acc;
        case kAdd: return static_cast<std::uint16_t>(x + acc);
        case kSub: return static_cast<std::uint16_t>(x - acc);
        case kAnd: return x & acc;
        case kOr: return x | acc;
        case kXor: return x ^ acc;
        case kShl: return c >= 16 ? 0 : static_cast<std::uint16_t>(x << c);
        case kShr: return c >= 16 ? 0 : static_cast<std::uint16_t>(x >> c);
        case kSar: return static_cast<std::uint16_t>(static_cast<std::int16_t>(x) >> (c >= 16 ? 15 : c));
        default: return x;
    }
}

// 0x79-0x82 compute acc + (-v) and test SF == OF (sub_16965).
bool ge_neg(std::uint16_t acc, std::uint16_t v) {
    const auto nv = static_cast<std::int16_t>(static_cast<std::uint16_t>(-v));
    return static_cast<std::int32_t>(static_cast<std::int16_t>(acc)) + nv >= 0;
}

bool compare(int cmp, std::uint16_t acc, std::uint16_t v) {
    const auto a = static_cast<std::int16_t>(acc), b = static_cast<std::int16_t>(v);
    switch (cmp) {
        case kGe: return a >= b;
        case kLt: return a < b;
        case kEq: return acc == v;
        case kNe: return acc != v;
        case kGeNeg: return ge_neg(acc, v);
        case kLtNeg: return !ge_neg(acc, v);
        default: return false;
    }
}

}  // namespace

std::uint16_t Vm::page_segment(std::uint16_t base, std::uint8_t page) {
    const std::uint16_t d = static_cast<std::uint16_t>(page - 0x82);
    const std::uint16_t rot = static_cast<std::uint16_t>((d >> 5) | (d << 11));  // ror 5
    return static_cast<std::uint16_t>(base + rot - 0x800);
}

Vm::Vm(DataSegment& ds, const Bytes& scripts) : ds_(ds), scripts_(scripts) {
    ds_.set_u16(ds_addr::kScriptBaseSeg, kScriptBase);
    table_.fill(&Vm::op_unimplemented);
    table_[0x00] = &Vm::op_yield;
    table_[0x01] = &Vm::op_nop;
    table_[0x03] = &Vm::op_goto;
    table_[0x04] = &Vm::op_call;
    table_[0x05] = &Vm::op_far_call;
    table_[0x06] = &Vm::op_return;

    table_[0x3B] = &Vm::op_load<kImm>;
    table_[0x3C] = &Vm::op_load<kSelf>;
    table_[0x3D] = &Vm::op_load<kGlobal>;
    table_[0x3E] = &Vm::op_load<kTarget>;
    table_[0x3F] = &Vm::op_load<kRelated>;
    table_[0x40] = &Vm::op_random;

    // 0x41-0x64: nine operations x (self, global, target, related).
    using H = Handler;
    const std::array<std::array<H, 4>, 9> combines = {{
        {&Vm::op_combine<kSelf, kStore>, &Vm::op_combine<kGlobal, kStore>, &Vm::op_combine<kTarget, kStore>, &Vm::op_combine<kRelated, kStore>},
        {&Vm::op_combine<kSelf, kAdd>, &Vm::op_combine<kGlobal, kAdd>, &Vm::op_combine<kTarget, kAdd>, &Vm::op_combine<kRelated, kAdd>},
        {&Vm::op_combine<kSelf, kSub>, &Vm::op_combine<kGlobal, kSub>, &Vm::op_combine<kTarget, kSub>, &Vm::op_combine<kRelated, kSub>},
        {&Vm::op_combine<kSelf, kAnd>, &Vm::op_combine<kGlobal, kAnd>, &Vm::op_combine<kTarget, kAnd>, &Vm::op_combine<kRelated, kAnd>},
        {&Vm::op_combine<kSelf, kOr>, &Vm::op_combine<kGlobal, kOr>, &Vm::op_combine<kTarget, kOr>, &Vm::op_combine<kRelated, kOr>},
        {&Vm::op_combine<kSelf, kXor>, &Vm::op_combine<kGlobal, kXor>, &Vm::op_combine<kTarget, kXor>, &Vm::op_combine<kRelated, kXor>},
        {&Vm::op_combine<kSelf, kShl>, &Vm::op_combine<kGlobal, kShl>, &Vm::op_combine<kTarget, kShl>, &Vm::op_combine<kRelated, kShl>},
        {&Vm::op_combine<kSelf, kShr>, &Vm::op_combine<kGlobal, kShr>, &Vm::op_combine<kTarget, kShr>, &Vm::op_combine<kRelated, kShr>},
        {&Vm::op_combine<kSelf, kSar>, &Vm::op_combine<kGlobal, kSar>, &Vm::op_combine<kTarget, kSar>, &Vm::op_combine<kRelated, kSar>},
    }};
    for (int op = 0; op < 9; ++op)
        for (int d = 0; d < 4; ++d) table_[0x41 + op * 4 + d] = combines[op][d];

    // 0x65-0x82: six conditions x (imm, self, global, target, related).
    const std::array<std::array<H, 5>, 6> branches = {{
        {&Vm::op_branch<kImm, kGe>, &Vm::op_branch<kSelf, kGe>, &Vm::op_branch<kGlobal, kGe>, &Vm::op_branch<kTarget, kGe>, &Vm::op_branch<kRelated, kGe>},
        {&Vm::op_branch<kImm, kLt>, &Vm::op_branch<kSelf, kLt>, &Vm::op_branch<kGlobal, kLt>, &Vm::op_branch<kTarget, kLt>, &Vm::op_branch<kRelated, kLt>},
        {&Vm::op_branch<kImm, kEq>, &Vm::op_branch<kSelf, kEq>, &Vm::op_branch<kGlobal, kEq>, &Vm::op_branch<kTarget, kEq>, &Vm::op_branch<kRelated, kEq>},
        {&Vm::op_branch<kImm, kNe>, &Vm::op_branch<kSelf, kNe>, &Vm::op_branch<kGlobal, kNe>, &Vm::op_branch<kTarget, kNe>, &Vm::op_branch<kRelated, kNe>},
        {&Vm::op_branch<kImm, kGeNeg>, &Vm::op_branch<kSelf, kGeNeg>, &Vm::op_branch<kGlobal, kGeNeg>, &Vm::op_branch<kTarget, kGeNeg>, &Vm::op_branch<kRelated, kGeNeg>},
        {&Vm::op_branch<kImm, kLtNeg>, &Vm::op_branch<kSelf, kLtNeg>, &Vm::op_branch<kGlobal, kLtNeg>, &Vm::op_branch<kTarget, kLtNeg>, &Vm::op_branch<kRelated, kLtNeg>},
    }};
    for (int c = 0; c < 6; ++c)
        for (int m = 0; m < 5; ++m) table_[0x65 + c * 5 + m] = branches[c][m];
}

std::uint8_t Vm::script_byte(std::uint16_t seg, std::uint16_t ip) const {
    const std::int64_t off = (static_cast<std::int64_t>(seg) - kScriptBase) * 16 + ip;
    if (off < 0 || off >= static_cast<std::int64_t>(scripts_.size()))
        throw std::runtime_error("script address out of range");
    return scripts_[static_cast<std::size_t>(off)];
}

std::uint8_t Vm::fetch8() { return script_byte(seg_, ip_++); }

std::uint16_t Vm::fetch16() {
    const std::uint8_t lo = fetch8();
    return static_cast<std::uint16_t>(lo | (fetch8() << 8));
}

std::uint16_t Vm::field_addr(std::uint8_t b, int slot) const {
    return static_cast<std::uint16_t>(0x13C2 + ds_.u16(static_cast<std::uint16_t>(ds_addr::kFieldTable + b)) + slot);
}

std::uint16_t Vm::read_operand(int mode) {
    switch (mode) {
        case kImm: return fetch16();
        case kSelf: return ds_.u16(self_field(fetch8()));
        case kGlobal: return ds_.u16(fetch16());
        case kTarget: return ds_.u16(target_field(fetch8()));
        case kRelated: return ds_.u16(related_field(fetch8()));
        case kRandom: return rand16();
        default: throw std::runtime_error("bad operand mode");
    }
}

// sub_10C9F: lagged-Fibonacci-like generator over the 10 bytes at ds:4Eh,
// with unaligned word accesses exactly as the original.
std::uint16_t Vm::rand16() {
    auto rd = [&](int o) { return ds_.u16(static_cast<std::uint16_t>(ds_addr::kRngState + o)); };
    auto wr = [&](int o, std::uint16_t v) { ds_.set_u16(static_cast<std::uint16_t>(ds_addr::kRngState + o), v); };
    const std::uint32_t s = static_cast<std::uint16_t>(rd(0) + 1) + static_cast<std::uint32_t>(rd(3));
    wr(6, static_cast<std::uint16_t>(s));
    wr(8, static_cast<std::uint16_t>(rd(2) + rd(5) + (s >> 16)));
    wr(0, rd(3));
    wr(2, rd(5));
    wr(4, rd(7));
    return rd(1);
}

void Vm::run_actor(int slot) {
    ds_.set_u16(ds_addr::kCurrentSlot, static_cast<std::uint16_t>(slot));
    ds_.set_u16(ds_addr::kLinkIp, ds_.field(field::kSavedLink, slot));
    ds_.set_u16(ds_addr::kLinkSeg, ds_.field(field::kScriptSeg, slot));
    seg_ = ds_.field(field::kScriptSeg, slot);
    ip_ = ds_.field(field::kScriptIp, slot);
    steps_ = 0;
    for (;;) {
        last_op_ = fetch8();
        ++steps_;
        if ((this->*table_[last_op_])() == Flow::EndTurn) return;
    }
}

Vm::Flow Vm::op_unimplemented() {
    char msg[96];
    std::snprintf(msg, sizeof msg, "unimplemented opcode 0x%02X at %04X:%04X", last_op_, seg_,
                  static_cast<unsigned>(ip_ - 1));
    throw std::runtime_error(msg);
}

// 0x00 (sub_160F7): save ip and link, end the turn.
Vm::Flow Vm::op_yield() {
    ds_.set_field(field::kScriptIp, self(), ip_);
    ds_.set_field(field::kSavedLink, self(), ds_.u16(ds_addr::kLinkIp));
    return Flow::EndTurn;
}

Vm::Flow Vm::op_nop() { return Flow::Continue; }

// 0x03 (sub_16141): goto addr16 in the current segment.
Vm::Flow Vm::op_goto() {
    jump(fetch16());
    return Flow::Continue;
}

// 0x04 (sub_16135): link = ip + 2, then goto.
Vm::Flow Vm::op_call() {
    ds_.set_u16(ds_addr::kLinkIp, static_cast<std::uint16_t>(ip_ + 2));
    ds_.set_u16(ds_addr::kLinkSeg, seg_);
    return op_goto();
}

// 0x05 (sub_1610C): link = ip + 3; page byte selects the segment; goto.
Vm::Flow Vm::op_far_call() {
    ds_.set_u16(ds_addr::kLinkIp, static_cast<std::uint16_t>(ip_ + 3));
    ds_.set_u16(ds_addr::kLinkSeg, seg_);
    const std::uint8_t page = fetch8();
    const std::uint16_t target = fetch16();
    seg_ = page_segment(ds_.u16(ds_addr::kScriptBaseSeg), page);
    jump(target);
    return Flow::Continue;
}

// 0x06 (sub_16151): return through the link register.
Vm::Flow Vm::op_return() {
    ip_ = ds_.u16(ds_addr::kLinkIp);
    seg_ = ds_.u16(ds_addr::kLinkSeg);
    return Flow::Continue;
}

template <int Mode>
Vm::Flow Vm::op_load() {
    set_acc(read_operand(Mode));
    return Flow::Continue;
}

Vm::Flow Vm::op_random() {
    set_acc(rand16());
    return Flow::Continue;
}

template <int Dest, int Op>
Vm::Flow Vm::op_combine() {
    std::uint16_t addr = 0;
    switch (Dest) {
        case kSelf: addr = self_field(fetch8()); break;
        case kGlobal: addr = fetch16(); break;
        case kTarget: addr = target_field(fetch8()); break;
        case kRelated: addr = related_field(fetch8()); break;
        default: break;
    }
    ds_.set_u16(addr, combine(Op, ds_.u16(addr), acc()));
    return Flow::Continue;
}

template <int Mode, int Cmp>
Vm::Flow Vm::op_branch() {
    const std::uint16_t v = read_operand(Mode);
    const std::uint16_t target = fetch16();
    if (compare(Cmp, acc(), v)) jump(target);
    return Flow::Continue;
}

}  // namespace bt
