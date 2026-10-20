#pragma once

#include <array>
#include <cstdint>

#include "data/archive.h"
#include "engine/data_segment.h"

namespace bt {

// Known data-segment addresses (seg003 offsets). See docs/vm/README.md.
namespace ds_addr {
constexpr std::uint16_t kCurrentSlot = 0x2A;    // actor being run
constexpr std::uint16_t kLinkIp = 0x30;         // call/return link register
constexpr std::uint16_t kLinkSeg = 0x32;
constexpr std::uint16_t kAcc = 0x38;            // VM accumulator
constexpr std::uint16_t kRngState = 0x4E;       // 10 bytes
constexpr std::uint16_t kScriptBaseSeg = 0x259A;  // segment of entry 64
constexpr std::uint16_t kFieldTable = 0x28CA;
}  // namespace ds_addr

// Actor field bases (field k at 0x13C2 + k * 0x28, indexed by slot).
namespace field {
constexpr std::uint16_t kScriptIp = 0x1232;
constexpr std::uint16_t kScriptSeg = 0x125A;
constexpr std::uint16_t kSavedLink = 0x184A;
constexpr std::uint16_t kFlags = 0x1462;
constexpr std::uint16_t kX = 0x15F2;
constexpr std::uint16_t kY = 0x161A;
constexpr std::uint16_t kType = 0x15A2;
constexpr std::uint16_t kTarget = 0x17FA;
constexpr std::uint16_t kRelated = 0x1822;
}  // namespace field

// The actor-script interpreter (sub_1609A and its 256 handlers).
//
// Script bytes are DATA.DAT entry 64. A script address is a (segment, ip)
// pair; segments are kept in the original encoding, relative to the value the
// engine stores at ds:259Ah, so fields and globals hold the same numbers the
// original game would.
class Vm {
public:
    Vm(DataSegment& ds, const Bytes& scripts);

    // Runs the actor in `slot` from its saved position until it yields or
    // ends its turn (sub_1603F's tail plus the interpreter loop).
    void run_actor(int slot);

    // Instructions executed by the last run_actor call (for tests/tracing).
    int last_steps() const { return steps_; }

    static constexpr std::uint16_t kScriptBase = 0x4000;  // arbitrary, stored at ds:259Ah

    // Segment for a page byte (0x82, 0x83, ...): base + ((page-0x82) ror 5) - 0x800.
    static std::uint16_t page_segment(std::uint16_t base, std::uint8_t page);

private:
    enum class Flow { Continue, EndTurn };
    using Handler = Flow (Vm::*)();

    std::uint8_t fetch8();
    std::uint16_t fetch16();
    std::uint8_t script_byte(std::uint16_t seg, std::uint16_t ip) const;

    int self() const { return ds_.u16(ds_addr::kCurrentSlot); }
    std::uint16_t acc() const { return ds_.u16(ds_addr::kAcc); }
    void set_acc(std::uint16_t v) { ds_.set_u16(ds_addr::kAcc, v); }

    // Field selector byte b (= 2k) -> field address for an actor slot.
    std::uint16_t field_addr(std::uint8_t b, int slot) const;
    std::uint16_t self_field(std::uint8_t b) const { return field_addr(b, self()); }
    std::uint16_t target_field(std::uint8_t b) const { return field_addr(b, ds_.u16(static_cast<std::uint16_t>(field::kTarget + self()))); }
    std::uint16_t related_field(std::uint8_t b) const { return field_addr(b, ds_.u16(static_cast<std::uint16_t>(field::kRelated + self()))); }

    // Operand by addressing mode (sub_17569): reads its operand bytes.
    std::uint16_t read_operand(int mode);
    std::uint16_t rand16();

    void jump(std::uint16_t target) { ip_ = target; }

    // Handlers.
    Flow op_unimplemented();
    Flow op_yield();          // 0x00
    Flow op_nop();            // 0x01
    Flow op_goto();           // 0x03
    Flow op_call();           // 0x04
    Flow op_far_call();       // 0x05
    Flow op_return();         // 0x06
    template <int Mode> Flow op_load();                 // 0x3B-0x3F
    Flow op_random();                                   // 0x40
    template <int Dest, int Op> Flow op_combine();      // 0x41-0x64
    template <int Mode, int Cmp> Flow op_branch();      // 0x65-0x82

    DataSegment& ds_;
    const Bytes& scripts_;
    std::array<Handler, 256> table_{};
    std::uint16_t seg_ = 0, ip_ = 0;
    std::uint8_t last_op_ = 0;
    int steps_ = 0;
};

}  // namespace bt
