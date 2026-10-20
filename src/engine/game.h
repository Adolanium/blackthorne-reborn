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

    // Loads level `level_id` (1-based resource id of its descriptor, e.g. 0xC0
    // for the first level) with Kyle starting at (x, y).
    void load_level(int level_id, int x, int y);

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

private:
    void walk_descriptor();
    void set_camera_bounds();      // sub_10936
    void place_camera();           // sub_10991: flip-screen camera on Kyle
    bool camera_needs_flip() const;  // sub_100B6 test
    void spawn_start_records();    // sub_11442
    void main_loop_actors();       // the actor part of sub_10043

    const Archive& archive_;
    std::unique_ptr<Machine> machine_;
    std::unique_ptr<Vm> vm_;
    Level level_;
    int level_id_ = 0;
    bool has_background_ = false;
};

}  // namespace bt
