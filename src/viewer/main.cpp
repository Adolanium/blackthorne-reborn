// Blackthorne level and sprite viewer.
// Usage: btviewer [path to DATA.DAT]

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "data/archive.h"
#include "data/level.h"
#include "data/sprites.h"

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
    Viewer(SDL_Renderer* r, const bt::Archive& a) : renderer_(r), archive_(a) { load(0); }

    void handle(const SDL_Event& e) {
        if (e.type == SDL_EVENT_KEY_DOWN) {
            switch (e.key.key) {
                case SDLK_PAGEDOWN: load((level_index_ + 1) % 18); break;
                case SDLK_PAGEUP: load((level_index_ + 17) % 18); break;
                case SDLK_B: show_bg_ = !show_bg_; rebuild_map(); break;
                case SDLK_F: show_fg_ = !show_fg_; rebuild_map(); break;
                case SDLK_TAB: sprite_mode_ = !sprite_mode_; break;
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
        if (sprite_mode_) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        const float speed = 600.0f * dt / zoom_;
        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) cam_x_ -= speed;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) cam_x_ += speed;
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) cam_y_ -= speed;
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) cam_y_ += speed;
        anim_time_ += dt;
    }

    void open(int level, bool sprites, int bank) {
        load(level);
        sprite_mode_ = sprites;
        select_bank(bank);
    }

    void draw(bool present = true) {
        SDL_SetRenderDrawColor(renderer_, 24, 24, 32, 255);
        SDL_RenderClear(renderer_);
        int ww, wh;
        SDL_GetCurrentRenderOutputSize(renderer_, &ww, &wh);
        if (sprite_mode_) draw_sprites(ww); else draw_map(ww, wh);
        draw_hud();
        if (present) SDL_RenderPresent(renderer_);
    }

private:
    void load(int index) {
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
    }

    void draw_sprites(int ww) {
        const float scale = std::max(1.0f, std::floor(static_cast<float>(ww - 20) / sheet_w_));
        const SDL_FRect dst{10, 40, sheet_w_ * scale, sheet_h_ * scale};
        SDL_RenderTexture(renderer_, sheet_.get(), nullptr, &dst);
    }

    void draw_hud() {
        char line[256];
        if (sprite_mode_) {
            const auto& b = level_.sprite_banks[bank_];
            std::snprintf(line, sizeof line, "Level %d  sprite bank %d/%zu  entry %d  %dx%d  %d frames   [<-/->] bank  [Tab] map",
                          level_index_ + 1, bank_ + 1, level_.sprite_banks.size(), b.entry, b.width, b.height, frame_count_);
        } else {
            std::snprintf(line, sizeof line, "Level %d/18 (entry %d)  %dx%d px  zoom %dx  [PgUp/PgDn] level  [B]g %s  [F]g %s  [Tab] sprites",
                          level_index_ + 1, level_.descriptor_entry, map_w_, map_h_, zoom_, show_bg_ ? "on" : "off", show_fg_ ? "on" : "off");
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
    bt::Level level_;
    int level_index_ = 0;
    Texture map_, sheet_;
    int map_w_ = 0, map_h_ = 0, sheet_w_ = 1, sheet_h_ = 1;
    int bank_ = 0, frame_count_ = 0;
    int zoom_ = 3;
    float cam_x_ = 0, cam_y_ = 0, anim_time_ = 0;
    bool show_bg_ = true, show_fg_ = true, sprite_mode_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    // btviewer [DATA.DAT] [--level N] [--sprites] [--bank N] [--shot out.bmp]
    std::filesystem::path data_path = kDefaultData;
    std::string shot;
    int start_level = 0, start_bank = 0;
    bool start_sprites = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--level" && i + 1 < argc) start_level = std::clamp(std::atoi(argv[++i]) - 1, 0, 17);
        else if (a == "--bank" && i + 1 < argc) start_bank = std::atoi(argv[++i]) - 1;
        else if (a == "--sprites") start_sprites = true;
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
        Viewer viewer(renderer, *archive);
        viewer.open(start_level, start_sprites, start_bank);
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
