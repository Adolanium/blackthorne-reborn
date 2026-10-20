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
constexpr std::uint16_t kBitMasks = 0x288A;       // 1 << i
constexpr std::uint16_t kBitClearMasks = 0x28AA;  // ~(1 << i)
constexpr std::uint16_t kActorEnd = 0x32C;        // 2 * live actor slots
constexpr std::uint16_t kProbeClass = 0x354;      // class of actor found by a probe
constexpr std::uint16_t kProbeSlot = 0x356;       // slot of actor found by a probe
constexpr std::uint16_t kClassLists = 0x60CC;     // 0xFF-terminated sorted byte lists
constexpr std::uint16_t kLevelDescriptor = 0x1CEE;
constexpr std::uint16_t kSpawnRecords = 0x1D24;   // 14 bytes each, inside the descriptor
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
constexpr std::uint16_t kTop = 0x13C2;
constexpr std::uint16_t kBottom = 0x13EA;
constexpr std::uint16_t kLeft = 0x1412;
constexpr std::uint16_t kRight = 0x143A;
constexpr std::uint16_t kSpawnIndex = 0x157A;
constexpr std::uint16_t kClass = 0x1642;
constexpr std::uint16_t kProbeDy = 0x17D2;
constexpr std::uint16_t kHalfWidth = 0x1872;
constexpr std::uint16_t kHalfHeight = 0x189A;
}  // namespace field

namespace flag {
constexpr std::uint16_t kFacing = 0x40;  // selects add/sub direction and probe side
}  // namespace flag

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
    template <int Mode, bool Equal> Flow op_call_if();  // 0x83-0x8C
    template <int Dest, bool AddWhenFacing> Flow op_facing_add();  // 0x8D-0x94
    Flow op_set_target();                               // 0x95
    template <int Mode> Flow op_bit_test();             // 0x96-0x9A
    Flow op_random_bit();                               // 0x9B
    template <int Dest> Flow op_bit_write();            // 0x9C-0x9F
    template <int Mode, bool Equal, bool Call> Flow op_bit_branch();  // 0xA0-0xB3
    template <int Side, bool Negate> Flow op_probe();   // 0xB4-0xBB
    template <int Offset> Flow op_spawn_store();        // 0xBC-0xBF

    std::uint16_t dest_addr(int dest);                  // reads a field byte or addr16
    std::uint16_t bit_mask(std::uint8_t b) const { return ds_.u16(static_cast<std::uint16_t>(ds_addr::kBitMasks + b)); }
    std::uint16_t read_bit(int mode);                   // bit8 then source operand -> 0/1
    void call(std::uint16_t target);                    // sub_16135 after the target word
    bool in_class_list(std::uint8_t list, std::uint8_t cls) const;
    bool probe(int side, std::uint8_t list);            // sub_18331 / 1833E / 1829A / 182A7
    void spawn_record_store(int offset, std::uint16_t value);

    DataSegment& ds_;
    const Bytes& scripts_;
    std::array<Handler, 256> table_{};
    std::uint16_t seg_ = 0, ip_ = 0;
    std::uint8_t last_op_ = 0;
    int steps_ = 0;
};

}  // namespace bt
