// Blackthorne level and sprite viewer.
// Usage: btviewer [path to DATA.DAT]

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "data/actors.h"
#include "data/archive.h"
#include "data/level.h"
#include "data/sprites.h"
#include "engine/game.h"
#include "render/renderer.h"

namespace {

constexpr const char* kDefaultData = "C:/Program Files (x86)/Blizzard Classics/Blackthorne/BTHORNE/DATA.DAT";
// Palette base for 4-bit sprite banks is not traced yet; 128 is Kyle's slot.
constexpr int kFourBitSpriteBase = 128;

struct TextureDeleter {
    void operator()(SDL_Texture* t) const { SDL_DestroyTexture(t); }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;

Texture make_texture(SDL_Renderer* r, int w, int h, const std::vector<std::uint32_t>& argb) {
    Texture tex(SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h));
    SDL_UpdateTexture(tex.get(), nullptr, argb.data(), w * 4);
    SDL_SetTextureScaleMode(tex.get(), SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(tex.get(), SDL_BLENDMODE_BLEND);
    return tex;
}

std::uint32_t argb(const bt::Rgb& c) {
    return 0xFF000000u | (c.r << 16) | (c.g << 8) | c.b;
}

class Viewer {
public:
    Viewer(SDL_Renderer* r, const bt::Archive& a, std::filesystem::path exe)
        : renderer_(r), archive_(a), exe_path_(std::move(exe)), game_renderer_(a) { load(0); }

    void handle(const SDL_Event& e) {
        if (e.type == SDL_EVENT_KEY_DOWN) {
            switch (e.key.key) {
                case SDLK_PAGEDOWN: load((level_index_ + 1) % 18); break;
                case SDLK_PAGEUP: load((level_index_ + 17) % 18); break;
                case SDLK_B: show_bg_ = !show_bg_; rebuild_map(); break;
                case SDLK_F: show_fg_ = !show_fg_; rebuild_map(); break;
                case SDLK_TAB: sprite_mode_ = !sprite_mode_; break;
                case SDLK_C: show_collision_ = !show_collision_; break;
                case SDLK_O: show_objects_ = !show_objects_; break;
                case SDLK_P: toggle_play(); break;
                case SDLK_G: toggle_play(true); break;
                case SDLK_RIGHT: if (sprite_mode_) select_bank(bank_ + 1); break;
                case SDLK_LEFT: if (sprite_mode_) select_bank(bank_ - 1); break;
                default: break;
            }
        } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            zoom_ = std::clamp(zoom_ + (e.wheel.y > 0 ? 1 : -1), 1, 8);
        } else if (e.type == SDL_EVENT_MOUSE_MOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
            cam_x_ -= e.motion.xrel / zoom_;
            cam_y_ -= e.motion.yrel / zoom_;
        }
    }

    void update(float dt) {
        if (game_) {
            const bool* k = SDL_GetKeyboardState(nullptr);
            using G = bt::Game;
            std::uint16_t bits = 0;
            if (k[SDL_SCANCODE_LEFT]) bits |= G::kLeft;
            if (k[SDL_SCANCODE_RIGHT]) bits |= G::kRight;
            if (k[SDL_SCANCODE_UP]) bits |= G::kUp;
            if (k[SDL_SCANCODE_DOWN]) bits |= G::kDown;
            if (k[SDL_SCANCODE_F]) bits |= G::kFire;
            if (k[SDL_SCANCODE_R]) bits |= G::kShootBack;
            if (k[SDL_SCANCODE_SPACE]) bits |= G::kJump;
            if (k[SDL_SCANCODE_D]) bits |= G::kRun;
            if (k[SDL_SCANCODE_S]) bits |= G::kGun;
            if (k[SDL_SCANCODE_E]) bits |= G::kUseItem;
            if (k[SDL_SCANCODE_LEFTBRACKET]) bits |= G::kItemPrev;
            if (k[SDL_SCANCODE_RIGHTBRACKET]) bits |= G::kItemNext;
            if (k[SDL_SCANCODE_RETURN]) bits |= G::kEnter;
            bits |= scripted_input_;
            for (const auto& r : input_script_)
                if (ticks_ >= r.from && ticks_ <= r.to) bits |= r.bits;
            game_->set_input(bits);
            tick_acc_ += dt;
            int steps = 0;
            while (tick_acc_ >= 1.0f / 30.0f && steps++ < 4) {
                tick_acc_ -= 1.0f / 30.0f;
                try {
                    game_->tick();
                } catch (const std::exception& ex) {
                    play_error_ = ex.what();
                    game_.reset();
                    return;
                }
                ++ticks_;
            }
            return;
        }
        if (sprite_mode_) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const float speed = 600.0f * dt / zoom_;
        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) cam_x_ -= speed;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) cam_x_ += speed;
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) cam_y_ -= speed;
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) cam_y_ += speed;
        anim_time_ += dt;
    }

    void open(int level, bool sprites, int bank, bool collision, bool objects, float x, float y, int zoom) {
        zoom_ = zoom;
        load(level);
        sprite_mode_ = sprites;
        select_bank(bank);
        show_collision_ = collision;
        show_objects_ = objects;
        cam_x_ = x;
        cam_y_ = y;
    }

    void draw(bool present = true) {
        SDL_SetRenderDrawColor(renderer_, 24, 24, 32, 255);
        SDL_RenderClear(renderer_);
        int ww, wh;
        SDL_GetCurrentRenderOutputSize(renderer_, &ww, &wh);
        if (game_) draw_game(ww, wh);
        else if (sprite_mode_) draw_sprites(ww);
        else draw_map(ww, wh);
        draw_hud();
        if (present) SDL_RenderPresent(renderer_);
    }

    // Starts the current level in the engine (play mode), Kyle at the level's
    // password-table start position when it has one.
    void toggle_play(bool from_boot = false) {
        if (game_) {
            game_.reset();
            return;
        }
        play_error_.clear();
        try {
            if (from_boot) {
                game_ = std::make_unique<bt::Game>(archive_, exe_path_);
                game_->boot();
                ticks_ = 0;
                tick_acc_ = 0;
                return;
            }
            const int level_id = level_.descriptor_entry + 1;
            game_ = std::make_unique<bt::Game>(archive_, exe_path_);
            int x = level_.objects.empty() ? 128 : level_.objects[0].x;
            int y = level_.objects.empty() ? 112 : level_.objects[0].y;
            const bt::DataSegment& ds = game_->machine().ds;
            for (std::uint16_t e = 0x543A; ds.u16(e) != 0xFFFF; e = static_cast<std::uint16_t>(e + 10))
                if (ds.u16(e) == level_id) {
                    x = ds.u16(static_cast<std::uint16_t>(e + 2));
                    y = ds.u16(static_cast<std::uint16_t>(e + 4));
                    break;
                }
            game_->load_level(level_id, x, y);
            ticks_ = 0;
            tick_acc_ = 0;
        } catch (const std::exception& ex) {
            play_error_ = ex.what();
            game_.reset();
        }
    }

    void draw_game(int ww, int wh) {
        game_renderer_.render(*game_, frame_);
        if (!play_tex_) {
            play_tex_.reset(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, frame_.width, frame_.height));
            SDL_SetTextureScaleMode(play_tex_.get(), SDL_SCALEMODE_NEAREST);
        }
        SDL_UpdateTexture(play_tex_.get(), nullptr, frame_.pixels.data(), frame_.width * 4);
        const int scale = std::max(1, std::min((ww) / frame_.width, (wh - 24) / frame_.height));
        const SDL_FRect dst{static_cast<float>((ww - frame_.width * scale) / 2), 24.0f,
                            static_cast<float>(frame_.width * scale), static_cast<float>(frame_.height * scale)};
        SDL_RenderTexture(renderer_, play_tex_.get(), nullptr, &dst);
    }

private:
    void load(int index) {
        game_.reset();
        level_index_ = index;
        level_ = bt::load_level(archive_, bt::kLevelDescriptors[index]);
        rebuild_map();
        select_bank(0);
        cam_x_ = cam_y_ = 0;
    }

    void rebuild_map() {
        const std::vector<int> idx = bt::render_level(level_, show_bg_, show_fg_);
        const bt::MapLayer& l = level_.foreground.cells.empty() ? level_.background : level_.foreground;
        map_w_ = l.width * 16;
        map_h_ = l.height * 16;
        std::vector<std::uint32_t> px(idx.size());
        for (std::size_t i = 0; i < idx.size(); ++i) px[i] = idx[i] < 0 ? 0xFF000000u : argb(level_.palette[idx[i]]);
        map_ = make_texture(renderer_, map_w_, map_h_, px);
    }

    void select_bank(int b) {
        if (level_.sprite_banks.empty()) return;
        const int n = static_cast<int>(level_.sprite_banks.size());
        bank_ = (b % n + n) % n;
        const bt::SpriteBankRef& ref = level_.sprite_banks[bank_];
        const auto frames = bt::decode_sprite_bank(archive_.entry(ref.entry), ref.width, ref.height);
        frame_count_ = static_cast<int>(frames.size());
        const int cols = 16, rows = (frame_count_ + cols - 1) / cols;
        sheet_w_ = cols * (ref.width + 2);
        sheet_h_ = std::max(1, rows) * (ref.height + 2);
        std::vector<std::uint32_t> px(static_cast<std::size_t>(sheet_w_) * sheet_h_, 0);
        for (int i = 0; i < frame_count_; ++i) {
            const auto& f = frames[i];
            int maxv = 0;
            for (auto v : f.pixels) maxv = std::max<int>(maxv, v);
            const int base = maxv < 16 ? kFourBitSpriteBase : 0;
            const int ox = (i % cols) * (ref.width + 2), oy = (i / cols) * (ref.height + 2);
            for (int y = 0; y < f.height; ++y)
                for (int x = 0; x < f.width; ++x) {
                    const int v = f.pixels[y * f.width + x];
                    if (v) px[static_cast<std::size_t>(oy + y) * sheet_w_ + ox + x] = argb(level_.palette[v + base]);
                }
        }
        sheet_ = make_texture(renderer_, sheet_w_, sheet_h_, px);
    }

    void draw_map(int ww, int wh) {
        const float view_w = static_cast<float>(ww) / zoom_, view_h = static_cast<float>(wh) / zoom_;
        cam_x_ = std::clamp(cam_x_, 0.0f, std::max(0.0f, map_w_ - view_w));
        cam_y_ = std::clamp(cam_y_, 0.0f, std::max(0.0f, map_h_ - view_h));
        const SDL_FRect src{cam_x_, cam_y_, std::min(view_w, static_cast<float>(map_w_)), std::min(view_h, static_cast<float>(map_h_))};
        const SDL_FRect dst{0, 0, src.w * zoom_, src.h * zoom_};
        SDL_RenderTexture(renderer_, map_.get(), &src, &dst);
        if (show_collision_) draw_collision();
        if (show_objects_) draw_objects();
    }

    // Screen rectangle for a map-space rectangle.
    SDL_FRect to_screen(float x, float y, float w, float h) const {
        return {(x - cam_x_) * zoom_, (y - cam_y_) * zoom_, w * zoom_, h * zoom_};
    }

    // Collision codes (map cell bits 10-15). Names are provisional until the
    // movement code that tests them (lists at ds:60CCh) is ported.
    static SDL_Color collision_colour(int code) {
        switch (code) {
            case 1: return {255, 60, 60, 110};    // solid
            case 3: return {255, 160, 0, 140};    // ladder
            case 4: return {255, 255, 0, 140};    // ladder top
            case 5: return {0, 255, 255, 160};    // beside an elevator shaft
            case 6: return {160, 80, 255, 160};   // unknown, vertical strips
            case 7: return {0, 255, 0, 170};      // ledge, left end
            case 8: return {0, 160, 255, 170};    // ledge, right end
            case 16: return {255, 0, 255, 170};   // water floor
            case 32: return {255, 255, 255, 140}; // elevator stop, standing cell
            case 33: return {120, 255, 160, 140}; // elevator stop, floor over shaft
            default: return {128, 128, 128, 160};
        }
    }

    void draw_collision() {
        const bt::MapLayer& l = level_.foreground;
        for (int y = 0; y < l.height; ++y)
            for (int x = 0; x < l.width; ++x) {
                const int code = l.collision(x, y);
                if (!code) continue;
                const SDL_Color c = collision_colour(code);
                SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
                const SDL_FRect r = to_screen(x * 16.0f, y * 16.0f, 16, 16);
                SDL_RenderFillRect(renderer_, &r);
                if (code != 1 && zoom_ >= 2) {
                    char label[8];
                    std::snprintf(label, sizeof label, "%d", code);
                    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
                    SDL_RenderDebugText(renderer_, r.x + 2, r.y + 2, label);
                }
            }
    }

    void draw_objects() {
        for (const bt::ObjectSpawn& o : level_.objects) {
            const bool at_start = o.flags & 0x800;
            SDL_SetRenderDrawColor(renderer_, at_start ? 255 : 255, at_start ? 80 : 220, at_start ? 80 : 0, 255);
            const SDL_FRect box = to_screen(static_cast<float>(o.x - o.rx), static_cast<float>(o.y - o.ry),
                                            static_cast<float>(o.rx * 2), static_cast<float>(o.ry * 2));
            SDL_RenderRect(renderer_, &box);
            const SDL_FRect dot = to_screen(o.x - 1.5f, o.y - 1.5f, 3, 3);
            SDL_RenderFillRect(renderer_, &dot);
            char label[48];
            const bt::ActorType& t = actor_types_[o.type < bt::kActorTypeCount ? o.type : 0];
            std::snprintf(label, sizeof label, "%02X p%X", o.type, o.param);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 200);
            const SDL_FRect bg{dot.x + 4, dot.y - 10, 8.0f * static_cast<float>(std::strlen(label)) + 2, 10};
            SDL_RenderFillRect(renderer_, &bg);
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
            SDL_RenderDebugText(renderer_, dot.x + 5, dot.y - 9, label);
            if (t.box_w && t.box_w < 128 && t.box_h < 128) {
                // Actor bounding box, centred on the spawn point.
                SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 90);
                const SDL_FRect fr = to_screen(o.x - t.box_w / 2.0f, o.y - t.box_h / 2.0f,
                                               static_cast<float>(t.box_w), static_cast<float>(t.box_h));
                SDL_RenderRect(renderer_, &fr);
            }
        }
    }

    void draw_sprites(int ww) {
        const float scale = std::max(1.0f, std::floor(static_cast<float>(ww - 20) / sheet_w_));
        const SDL_FRect dst{10, 40, sheet_w_ * scale, sheet_h_ * scale};
        SDL_RenderTexture(renderer_, sheet_.get(), nullptr, &dst);
    }

    void draw_hud() {
        char line[256];
        if (game_) {
            const bt::DataSegment& ds = game_->machine().ds;
            std::snprintf(line, sizeof line, "PLAY level %02X  tick %d  Kyle (%d,%d)  actors %d  [P] stop",
                          game_->level_id(), ticks_, ds.field(bt::field::kX, 0), ds.field(bt::field::kY, 0), ds.u16(0x32C) / 2);
        } else if (!play_error_.empty()) {
            std::snprintf(line, sizeof line, "engine stopped: %s", play_error_.c_str());
        } else if (sprite_mode_) {
            const auto& b = level_.sprite_banks[bank_];
            std::snprintf(line, sizeof line, "Level %d  sprite bank %d/%zu  entry %d  %dx%d  %d frames   [<-/->] bank  [Tab] map",
                          level_index_ + 1, bank_ + 1, level_.sprite_banks.size(), b.entry, b.width, b.height, frame_count_);
        } else {
            std::snprintf(line, sizeof line, "Level %d/18 (entry %d)  %dx%d px  zoom %dx  [PgUp/PgDn] level  [B]g %s [F]g %s [C]ollision [O]bjects (%zu)  [Tab] sprites",
                          level_index_ + 1, level_.descriptor_entry, map_w_, map_h_, zoom_, show_bg_ ? "on" : "off", show_fg_ ? "on" : "off",
                          level_.objects.size());
        }
        SDL_SetRenderScale(renderer_, 2, 2);
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 180);
        const SDL_FRect bar{0, 0, 2000, 12};
        SDL_RenderFillRect(renderer_, &bar);
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        SDL_RenderDebugText(renderer_, 4, 2, line);
        SDL_SetRenderScale(renderer_, 1, 1);
    }

    SDL_Renderer* renderer_;
    const bt::Archive& archive_;
    std::filesystem::path exe_path_;
    bt::Renderer game_renderer_;
    std::unique_ptr<bt::Game> game_;
    bt::Image frame_;
    Texture play_tex_;
    float tick_acc_ = 0;
    int ticks_ = 0;
    std::string play_error_;
public:
    std::uint16_t scripted_input_ = 0;  // for --hold in screenshot runs
    struct InputRange { int from, to; std::uint16_t bits; };
    std::vector<InputRange> input_script_;  // --input from-to:bits,...
private:
    std::vector<bt::ActorType> actor_types_ = bt::load_actor_types(archive_);
    bt::Level level_;
    int level_index_ = 0;
    Texture map_, sheet_;
    int map_w_ = 0, map_h_ = 0, sheet_w_ = 1, sheet_h_ = 1;
    int bank_ = 0, frame_count_ = 0;
    int zoom_ = 3;
    float cam_x_ = 0, cam_y_ = 0, anim_time_ = 0;
    bool show_bg_ = true, show_fg_ = true, sprite_mode_ = false;
    bool show_collision_ = false, show_objects_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    // btviewer [DATA.DAT] [--level N] [--sprites] [--bank N] [--collision] [--objects]
    //          [--zoom N] [--x PX --y PX] [--shot out.bmp]
    std::filesystem::path data_path = kDefaultData;
    std::string shot;
    int start_level = 0, start_bank = 0;
    bool start_sprites = false, start_collision = false, start_objects = false, start_play = false, start_boot = false;
    int start_ticks = 0;
    std::uint16_t hold_bits = 0;
    std::string input_spec;
    float start_x = 0, start_y = 0;
    int start_zoom = 3;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--level" && i + 1 < argc) start_level = std::clamp(std::atoi(argv[++i]) - 1, 0, 17);
        else if (a == "--bank" && i + 1 < argc) start_bank = std::atoi(argv[++i]) - 1;
        else if (a == "--sprites") start_sprites = true;
        else if (a == "--collision") start_collision = true;
        else if (a == "--play") start_play = true;
        else if (a == "--boot") start_boot = true;
        else if (a == "--ticks" && i + 1 < argc) start_ticks = std::atoi(argv[++i]);
        else if (a == "--input" && i + 1 < argc) input_spec = argv[++i];
        else if (a == "--hold" && i + 1 < argc) hold_bits = static_cast<std::uint16_t>(std::strtol(argv[++i], nullptr, 16));
        else if (a == "--objects") start_objects = true;
        else if (a == "--zoom" && i + 1 < argc) start_zoom = std::clamp(std::atoi(argv[++i]), 1, 8);
        else if (a == "--x" && i + 1 < argc) start_x = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--y" && i + 1 < argc) start_y = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else data_path = a;
    }
    std::unique_ptr<bt::Archive> archive;
    try {
        archive = std::make_unique<bt::Archive>(data_path);
    } catch (const std::exception& ex) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Blackthorne viewer",
                                 (std::string(ex.what()) + "\n\nUsage: btviewer <path to DATA.DAT>").c_str(), nullptr);
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Blackthorne Reborn - viewer", 1280, 720, SDL_WINDOW_RESIZABLE | (shot.empty() ? 0 : SDL_WINDOW_HIDDEN), &window, &renderer))
        return 1;
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    {
        Viewer viewer(renderer, *archive, data_path.parent_path() / "BTHORNE.EXE");
        viewer.open(start_level, start_sprites, start_bank, start_collision, start_objects, start_x, start_y, start_zoom);
        if (start_play || start_boot) {
            viewer.scripted_input_ = hold_bits;
            for (std::size_t pos = 0; pos < input_spec.size();) {
                const std::size_t end = std::min(input_spec.find(',', pos), input_spec.size());
                // "from-to:hexbits"
                const std::string item = input_spec.substr(pos, end - pos);
                char* p = nullptr;
                const long from = std::strtol(item.c_str(), &p, 10);
                if (*p == '-') {
                    const long to = std::strtol(p + 1, &p, 10);
                    if (*p == ':')
                        viewer.input_script_.push_back({static_cast<int>(from), static_cast<int>(to),
                                                        static_cast<std::uint16_t>(std::strtol(p + 1, nullptr, 16))});
                }
                pos = end + 1;
            }
            viewer.toggle_play(start_boot);
            for (int t = 0; t < start_ticks; ++t) viewer.update(1.0f / 30.0f);
        }
        if (!shot.empty()) {
            // Render one frame to a file instead of running interactively.
            viewer.draw(false);
            SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr);
            const bool ok = s && SDL_SaveBMP(s, shot.c_str());
            SDL_DestroySurface(s);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return ok ? 0 : 1;
        }
        Uint64 last = SDL_GetTicksNS();
        for (bool running = true; running;) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) running = false;
                viewer.handle(e);
            }
            const Uint64 now = SDL_GetTicksNS();
            viewer.update(static_cast<float>(now - last) / 1e9f);
            last = now;
            viewer.draw();
        }
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
