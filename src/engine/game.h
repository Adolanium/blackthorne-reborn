#pragma once

#include <filesystem>
#include <memory>

#include "data/archive.h"
#include "data/level.h"
#include "engine/machine.h"
#include "engine/vm.h"

namespace bt {

// A running game: level loading (sub_105B4) and the per-tick main loop
// (sub_10043) on top of the original memory model and the script VM.
class Game {
public:
    Game(const Archive& archive, const std::filesystem::path& exe_path, int language = 0);

    // Starts the game the way the original does after its hardware setup
    // (sub_10E05, sub_128E9): password "STRT", then level 0x61 (the opening).
    void boot();

    // Loads level `level_id` (1-based resource id of its descriptor, e.g. 0xC0
    // for the first level) with Kyle starting at (x, y).
    void load_level(int level_id, int x, int y);

    // Input bits as the keyboard handler builds them (ds:2A82h).
    enum Input : std::uint16_t {
        kEnter = 0x1, kEscape = 0x2, kUp = 0x4, kDown = 0x8, kLeft = 0x10, kRight = 0x20,
        kFire = 0x40, kShootBack = 0x80, kJump = 0x100, kRun = 0x200, kGun = 0x400,
        kUseItem = 0x800, kItemPrev = 0x1000, kItemNext = 0x2000, kPause = 0x4000,
    };
    void set_input(std::uint16_t bits) { machine_->ds.set_u16(0x2A82, bits); }

    // One game tick (30 per second).
    void tick();

    Machine& machine() { return *machine_; }
    const Machine& machine() const { return *machine_; }
    Vm& vm() { return *vm_; }
    const Level& level() const { return level_; }  // static tile data for rendering
    int level_id() const { return level_id_; }

    std::uint16_t camera_x() const { return machine_->ds.u16(0x1B4E); }
    std::uint16_t camera_y() const { return machine_->ds.u16(0x1B50); }

    // Map layer offsets in the map segment (ds:25B0h foreground, 25B2h background).
    std::uint16_t foreground_offset() const { return machine_->ds.u16(0x25B0); }
    std::uint16_t background_offset() const { return machine_->ds.u16(0x25B2); }
    bool has_background() const { return has_background_; }
    bool has_hud() const { return machine_->ds.u16(0x1CF6) & 1; }

private:
    void walk_descriptor();
    void set_camera_bounds();      // sub_10936
    void place_camera();           // sub_10991: flip-screen camera on Kyle
    bool camera_needs_flip() const;  // sub_100B6 test
    void spawn_start_records();    // sub_11442
    void main_loop_actors();       // the actor part of sub_10043
    void change_level();           // sub_1011B, level-exit branch
    void music_action();           // sub_11C64: the header's music action
    void hud_init();               // sub_140DC + sub_1407B
    void hud_tick();               // sub_1411C (inventory cycling) + sub_14164 (health, inventory state)
    void cycle_palettes();         // sub_186C4
    void update_shake();           // sub_10AC1
    void animate_tiles();          // sub_18822

    const Archive& archive_;
    std::unique_ptr<Machine> machine_;
    std::unique_ptr<Vm> vm_;
    Level level_;
    int level_id_ = 0;
    bool has_background_ = false;
};

}  // namespace bt
