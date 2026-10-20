#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "data/archive.h"
#include "engine/data_segment.h"
#include "engine/machine.h"

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

// Something the VM asks the host to do (sound, saving, quitting).
struct VmEvent {
    enum class Kind { PlaySound, StopSound, MusicLoad, MusicStart, MusicRestart, MusicStop, SavePassword, Quit };
    Kind kind;
    int id;
};

// The actor-script interpreter (sub_1609A and its 256 handlers).
//
// Script bytes are DATA.DAT entry 64. A script address is a (segment, ip)
// pair; segments are kept in the original encoding, relative to the value the
// engine stores at ds:259Ah, so fields and globals hold the same numbers the
// original game would.
class Vm {
public:
    explicit Vm(Machine& machine);

    // Runs the actor in `slot` from its saved position until it yields or
    // ends its turn (sub_1603F's tail plus the interpreter loop).
    void run_actor(int slot);

    // Per-tick routines driven by the game loop (see docs/vm/README.md).
    int spawn_actor(int type, int record, std::uint16_t flags);  // sub_1115D; slot or -1
    void despawn_actor(int slot);                                // sub_11560
    void run_checks(int slot);                                   // sub_17AE9 (pass in ds:33Eh)
    void spawn_visible_records();                                // sub_1130E
    void mark_offscreen_actors();                                // loc_114AE
    void anim_tick(int slot);                                    // sub_1207C
    void physics(int slot);                                      // loc_12404
    void schedule_actor(int slot);                               // sub_1603F
    std::uint16_t random() { return rand16(); }                  // sub_10C9F
    void retint_range(std::uint16_t start, int colours) { retint_palette(start, colours); }  // sub_12671
    bool alloc_frame_buffer(std::uint16_t bank, std::uint16_t& seg, std::uint16_t& off);  // sub_11F9E
    void run_scheduler();                                        // sub_1601A + sub_15FCE
    void integrate_velocity();                                   // sub_111BC
    void run_check_pass(int pass);                               // sub_17AC7 (+1), sub_17AB2 (-1)
    void apply_anchors();                                        // sub_11260
    void move_parts();                                           // sub_11F49

    // Host interaction.
    std::vector<VmEvent>& events() { return events_; }
    std::function<bool(std::array<char, 4>&)> load_password_;  // FB: read the save file
    std::array<char, 4> current_password() const;

    // Opcodes without a handler yet (for tests).
    std::vector<int> unimplemented_opcodes() const;

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
    // Write-back by addressing mode (loc_175D6): modes 1-4 only.
    void write_operand(int mode, std::uint16_t value);
    std::uint16_t rand16();

    void jump(std::uint16_t target) { ip_ = target; }
    // The interpreter loop: runs from seg_:ip_ until a handler ends the turn.
    void execute();

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

    // vm_world.cpp: actor orientation, movement modes, map access and probes.
    void register_world_ops();
    template <bool Vertical, bool Set> Flow op_flip_to();  // 0x07-0x0A
    template <bool Vertical> Flow op_flip();               // 0x0B, 0x0C
    Flow op_exit_level();                                  // 0x0D
    Flow op_scratch_fn();                                  // 0x0F
    template <int Who> Flow op_offset_to();                // 0x11, 0x12, 0x32
    template <int Who> Flow op_anchor();                   // 0x13, 0x14, 0x18
    Flow op_anim_start();                                  // 0x15
    Flow op_anim_start_far();                              // 0x16
    Flow op_if_anim_idle();                                // 0x19
    template <int Kind, bool Negate> Flow op_edge_probe(); // 0x1B-0x22, 0x2B, 0x2C
    Flow op_to_tile();                                     // 0x23
    Flow op_tile_class();                                  // 0x24
    Flow op_snap_to_tile();                                // 0x25
    template <int What> Flow op_write_cell();              // 0x26-0x28
    Flow op_find_exit();                                   // 0x29
    Flow op_breakpoint();                                  // 0x31
    Flow op_tint();                                        // 0x34
    Flow op_tint_reset();                                  // 0x35
    template <bool Hide> Flow op_parts_flag();             // 0x36, 0x37
    Flow op_teleport();                                    // 0x38
    template <bool Negate> Flow op_point_probe();          // 0x39, 0x3A

    void flip(int slot, bool vertical);                    // sub_1103A, sub_110B0
    // Map cell scans (sub_17FB8 family): true on a class hit, sets ds:354h.
    bool scan_row(std::uint8_t list, int y, int x0, int x1);
    bool scan_column(std::uint8_t list, int x, int y0, int y1);
    bool probe_point_actor(std::uint8_t list, int px, int py);  // sub_183BE / sub_1844F
    void redraw_tile(std::uint16_t tx, std::uint16_t ty, std::uint16_t v);  // sub_18910
    void rebuild_palette();                                // sub_1266C

    // vm_anim.cpp: animation scripts, sprite parts, frames and physics.
    void register_anim_ops();
    Flow op_anim_tick();                                   // 0x2A
    template <int Dest, bool AddWhenFlipped> Flow op_vflip_add();  // 0xC4-0xCB
    Flow op_part_cells();                                  // 0xCF
    template <bool Vertical> Flow op_velocity();           // 0xD0, 0xD2
    template <bool Vertical> Flow op_part_offsets();       // 0xD1, 0xD3
    Flow op_part_palettes();                               // 0xD4
    Flow op_part_filter();                                 // 0xD5
    Flow op_anim_end_tick();                               // 0xD6
    Flow op_anim_wait();                                   // 0xD7
    template <std::uint16_t Bits> Flow op_part_flip();     // 0xD8-0xDA
    Flow op_part_groups();                                 // 0xDB
    Flow op_frame();                                       // 0xDC
    Flow op_part_sizes();                                  // 0xDD
    Flow op_part_priority();                               // 0xDE
    Flow op_select_bank();                                 // 0xDF
    Flow op_part_hide();                                   // 0xE0
    Flow op_part_show();                                   // 0xE1
    Flow op_part_xor();                                    // 0xE2
    Flow op_if_anim_waiting();                             // 0xE4

    void load_frame(int slot, std::uint16_t frame);        // sub_15BF2
    void mirror_parts(int slot, bool vertical);            // the part half of sub_1103A / sub_110B0

    // vm_actors.cpp: actor lifetime, event checks, collision, scans.
    void register_actor_ops();
    Flow op_despawn_self();                                // 0x0E
    Flow op_spawn();                                       // 0x10
    template <bool KyleOnly, bool ByMask> Flow op_event_overlap();  // 0x17, 0x1A, 0x2F, 0x30
    template <int Axis> Flow op_event_terrain();           // 0x2D, 0x2E, 0x33
    template <bool Target, bool Negate> Flow op_if_onscreen();  // 0xC0-0xC3
    template <bool KyleOnly> Flow op_scan();               // 0xCC, 0xF8
    Flow op_indexed_store();                               // 0xCD
    Flow op_indexed_load();                                // 0xCE
    Flow op_spawn_relative();                              // 0xE3
    Flow op_spawn_record();                                // 0xE5
    template <bool ByMask> Flow op_find_overlap();         // 0xEF, 0xF0
    Flow op_palette();                                     // 0xF1
    Flow op_forget_record();                               // 0xF2

    bool alloc_parts(int count, int& first);               // sub_116A8 / loc_11652
    void set_part_size(int p, bool large);                 // sub_11FF4
    void finish_spawn(int slot);                           // loc_1706D tail
    int spawn_from_record(int r);
    bool check_record(int r);                              // sub_11380
    void release_palette(int slot);                        // sub_1153A
    int acquire_palette(std::uint16_t id);                 // sub_121F2 + sub_19062
    void retint_palette(std::uint16_t start, int colours); // sub_12671
    std::uint16_t check_bit() const;
    void finish_check(bool fire);
    void snap_horizontal(int slot, int dir);               // sub_17E94
    void snap_vertical(int slot, int dir);                 // sub_17EDC
    int solid_actor_hit(int axis, std::uint8_t list, int& dir);
    void push_out(int axis, int other, int dir);           // sub_18222 / sub_1825E
    bool onscreen(int slot) const;

    // vm_misc.cpp: sound, text boxes, portraits, shake, passwords, exit.
    void register_misc_ops();
    Flow op_sound();                                       // 0x02
    Flow op_sound_stop();                                  // 0xF3
    template <bool Restart> Flow op_music();               // 0xF4, 0xF5
    Flow op_text_near_actor();                             // 0xE6
    Flow op_text_at();                                     // 0xE7
    Flow op_put_char();                                    // 0xE8
    Flow op_set_1b46();                                    // 0xE9
    Flow op_clear_text();                                  // 0xEA
    Flow op_portrait_update();                             // 0xEB
    Flow op_portrait_update_ec();                          // 0xEC
    template <bool Vertical> Flow op_shake();              // 0xED, 0xEE
    Flow op_apply_password();                              // 0xF6
    Flow op_save_password();                               // 0xF7
    Flow op_fatal();                                       // 0xF9
    Flow op_quit();                                        // 0xFA
    Flow op_load_password();                               // 0xFB
    void prepare_text(std::uint16_t text, std::uint16_t style);  // sub_15C57 + sub_15C82
    void draw_text_box(int col, int row);                  // sub_15D83
    void draw_text(int col, int row);                      // sub_15E51
    void portrait_update();                                // sub_15EB5

    std::uint16_t dest_addr(int dest);                  // reads a field byte or addr16
    std::uint16_t bit_mask(std::uint8_t b) const { return ds_.u16(static_cast<std::uint16_t>(ds_addr::kBitMasks + b)); }
    std::uint16_t read_bit(int mode);                   // bit8 then source operand -> 0/1
    void call(std::uint16_t target);                    // sub_16135 after the target word
    bool in_class_list(std::uint8_t list, std::uint8_t cls) const;
    bool probe(int side, std::uint8_t list);            // sub_18331 / 1833E / 1829A / 182A7
    void spawn_record_store(int offset, std::uint16_t value);

    Machine& m_;
    DataSegment& ds_;
    const Bytes& scripts_;
    std::array<Handler, 256> table_{};
    std::uint16_t seg_ = 0, ip_ = 0;
    std::uint8_t last_op_ = 0;
    std::vector<VmEvent> events_;
    int steps_ = 0;
};

}  // namespace bt
