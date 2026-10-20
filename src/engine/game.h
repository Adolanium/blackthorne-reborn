#pragma once

#include <exception>
#include <filesystem>
#include <memory>
#include <semaphore>
#include <thread>

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
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // Starts the game the way the original does after its hardware setup
    // (sub_10E05, sub_128E9): password "STRT", then level 0x61 (the opening).
    void boot();

    // Loads level `level_id` (1-based resource id of its descriptor, e.g. 0xC0
    // for the first level) with Kyle starting at (x, y).
    void load_level(int level_id, int x, int y);
    // Loads a level at its password-table start, else at its first object.
    void start_level(int level_id);

    // Input bits as the keyboard handler builds them (ds:2A82h).
    enum Input : std::uint16_t {
        kEnter = 0x1, kEscape = 0x2, kUp = 0x4, kDown = 0x8, kLeft = 0x10, kRight = 0x20,
        kFire = 0x40, kShootBack = 0x80, kJump = 0x100, kRun = 0x200, kGun = 0x400,
        kUseItem = 0x800, kItemPrev = 0x1000, kItemNext = 0x2000, kPause = 0x4000,
    };
    void set_input(std::uint16_t bits) { machine_->ds.set_u16(0x2A82, bits); }

    // One frame: runs the original's control flow from one frame wait
    // (sub_10113 with the page-flip counter ds:5312h above zero) to the next.
    // Usually that is one main loop iteration; a screen flip, a level change,
    // a fade or a menu step spreads its work over frames the same way.
    void tick();

    // Save states: the whole machine memory plus the level id. Banks and the
    // static level data are reloaded from DATA.DAT. Saving is possible at a
    // plain main-loop frame (can_save()); throws otherwise or on I/O errors.
    bool can_save() const { return saveable_; }
    void save_state(const std::filesystem::path& path) const;
    void load_state(const std::filesystem::path& path);

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
    void load_level_data(int level_id, int x, int y);  // sub_105B4

    // The original control flow, run on a worker that pauses at frame waits.
    void run_logic();
    void wait_frame();             // sub_10113: a frame boundary when ds:5312h > 0
    void frame();                  // sub_1008C: wait, then the per-frame effects
    void frame_tail();             // sub_1008C after the wait
    void iteration();              // sub_10043, falling into sub_1008C
    void latch_input();            // sub_10A87
    void settle_input();           // sub_10A93
    void level_frames();           // sub_1011B: level change / pause loop
    void change_level();           // sub_1011B, level-exit branch
    void flip_screen();            // sub_100B6, loc_100F5
    void blink_parts();            // sub_10179
    void fade(bool in);            // sub_1270D (in) / sub_12752 (out)
    void retint();                 // sub_1266C
    void page_flip();              // sub_1283B, the frame counter part
    void track_redraws();          // sub_158FA bookkeeping
    void stop_worker();
    struct Stop {};
    enum class Entry { AfterLoad, AfterWait, Loop };
    Entry entry_ = Entry::AfterLoad;
    std::thread worker_;
    std::binary_semaphore go_{0}, done_{0};
    bool stopping_ = false;
    bool saveable_ = false;
    int nesting_ = 0;              // > 0 inside flips, fades, menus and level changes
    std::exception_ptr error_;

    void music_action(int action); // the music action table at ds:295Ah
    void hud_init();               // sub_140DC + sub_1407B
    void hud_tick();               // sub_1411C (inventory cycling) + sub_14164 (health, inventory state)
    void cycle_palettes();         // sub_186C4
    void update_shake();           // sub_10AC1
    void animate_tiles();          // sub_18822
    // The modal yes/no menus (loc_12A12 pause, sub_1290E escape).
    void menu(bool quit_menu);
    void open_menu(bool quit_menu);
    void menu_select();            // sub_12AF5
    void menu_finish();            // the loop exit
    struct Menu {
        bool active = false;
        bool quit_menu = false;
        bool darkened = false;
    } menu_;

    const Archive& archive_;
    std::unique_ptr<Machine> machine_;
    std::unique_ptr<Vm> vm_;
    Level level_;
    int level_id_ = 0;
    bool has_background_ = false;
};

}  // namespace bt
