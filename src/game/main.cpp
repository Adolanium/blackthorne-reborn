// Blackthorne Reborn: runs the original game logic on the reimplemented engine.
// Usage: blackthorne [path/to/DATA.DAT]   (BTHORNE.EXE must sit next to it)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "data/archive.h"
#include "engine/game.h"
#include "render/filters.h"
#include "render/renderer.h"

namespace {

constexpr const char* kDefaultData = "C:/Program Files (x86)/Blizzard Classics/Blackthorne/BTHORNE/DATA.DAT";
constexpr double kTickSeconds = 1.0 / 30.0;  // the original runs at 60 Hz / 2

// Gamepad layout: d-pad / left stick move, A jump, X fire, Y draw/holster,
// B use item, LB/RB cycle items, RT run, LT shoot backward, Start = Enter,
// Back = Esc.
std::uint16_t read_gamepad(SDL_Gamepad* pad) {
    using G = bt::Game;
    if (!pad) return 0;
    auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    const int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    constexpr int kDead = 12000;
    std::uint16_t bits = 0;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -kDead) bits |= G::kLeft;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > kDead) bits |= G::kRight;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -kDead) bits |= G::kUp;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > kDead) bits |= G::kDown;
    if (button(SDL_GAMEPAD_BUTTON_SOUTH)) bits |= G::kJump;
    if (button(SDL_GAMEPAD_BUTTON_WEST)) bits |= G::kFire;
    if (button(SDL_GAMEPAD_BUTTON_NORTH)) bits |= G::kGun;
    if (button(SDL_GAMEPAD_BUTTON_EAST)) bits |= G::kUseItem;
    if (button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) bits |= G::kItemPrev;
    if (button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) bits |= G::kItemNext;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) bits |= G::kRun;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) bits |= G::kShootBack;
    if (button(SDL_GAMEPAD_BUTTON_START)) bits |= G::kEnter;
    if (button(SDL_GAMEPAD_BUTTON_BACK)) bits |= G::kEscape;
    return bits;
}

// How the 320x240 frame reaches the window (F8 cycles).
enum class Filter { Sharp, Pixel, Smooth, Crt, Count };
constexpr const char* kFilterNames[] = {"sharp", "pixel perfect", "smooth (Scale4x)", "CRT"};

// Draws the frame into the window. Sharp and CRT upscale by the largest whole
// factor with nearest-neighbour and cover the rest with linear filtering, so
// the picture fills the window without uneven pixels or blur; Pixel uses only
// whole factors; Smooth runs Scale2x twice first.
class Display {
public:
    explicit Display(SDL_Renderer* sdl) : sdl_(sdl) {}
    ~Display() {
        for (SDL_Texture* t : {source_, prescaled_}) if (t) SDL_DestroyTexture(t);
    }
    Filter filter = Filter::Sharp;

    void present(const bt::Image& frame, bool flip = true) {
        const bt::Image* image = &frame;
        if (filter == Filter::Smooth) {
            bt::scale2x(frame, x2_);
            bt::scale2x(x2_, x4_);
            image = &x4_;
        }
        source_ = fit(source_, image->width, image->height, SDL_TEXTUREACCESS_STREAMING);
        SDL_UpdateTexture(source_, nullptr, image->pixels.data(), image->width * 4);

        int ww = 0, wh = 0;
        SDL_GetCurrentRenderOutputSize(sdl_, &ww, &wh);
        const int whole = std::max(1, std::min(ww / frame.width, wh / frame.height));
        SDL_FRect dst;
        if (filter == Filter::Pixel) {
            dst = {static_cast<float>((ww - frame.width * whole) / 2), static_cast<float>((wh - frame.height * whole) / 2),
                   static_cast<float>(frame.width * whole), static_cast<float>(frame.height * whole)};
        } else {
            const float s = std::min(static_cast<float>(ww) / frame.width, static_cast<float>(wh) / frame.height);
            dst = {std::floor((ww - frame.width * s) / 2), std::floor((wh - frame.height * s) / 2), std::floor(frame.width * s),
                   std::floor(frame.height * s)};
        }
        SDL_Texture* shown = source_;
        if (filter == Filter::Smooth) {
            SDL_SetTextureScaleMode(source_, SDL_SCALEMODE_LINEAR);
        } else if (filter == Filter::Pixel) {
            SDL_SetTextureScaleMode(source_, SDL_SCALEMODE_NEAREST);
        } else {
            SDL_SetTextureScaleMode(source_, SDL_SCALEMODE_NEAREST);
            prescaled_ = fit(prescaled_, frame.width * whole, frame.height * whole, SDL_TEXTUREACCESS_TARGET);
            SDL_SetTextureScaleMode(prescaled_, SDL_SCALEMODE_LINEAR);
            SDL_SetRenderTarget(sdl_, prescaled_);
            SDL_RenderTexture(sdl_, source_, nullptr, nullptr);
            SDL_SetRenderTarget(sdl_, nullptr);
            shown = prescaled_;
        }
        SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 255);
        SDL_RenderClear(sdl_);
        SDL_RenderTexture(sdl_, shown, nullptr, &dst);
        if (filter == Filter::Crt) scanlines(dst, frame.height);
        if (flip) SDL_RenderPresent(sdl_);
    }

private:
    SDL_Texture* fit(SDL_Texture* t, int w, int h, SDL_TextureAccess access) {
        if (t && t->w == w && t->h == h) return t;
        if (t) SDL_DestroyTexture(t);
        return SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888, access, w, h);
    }
    // Darken the lower part of every source row, like the gaps between the
    // beam lines of a CRT; skipped when rows are too small to show them.
    void scanlines(const SDL_FRect& dst, int rows) {
        const float row = dst.h / static_cast<float>(rows);
        if (row < 2.5f) return;
        rects_.clear();
        for (int y = 0; y < rows; ++y)
            rects_.push_back({dst.x, dst.y + y * row + row * 0.6f, dst.w, row * 0.4f});
        SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 110);
        SDL_RenderFillRects(sdl_, rects_.data(), static_cast<int>(rects_.size()));
    }

    SDL_Renderer* sdl_;
    SDL_Texture* source_ = nullptr;
    SDL_Texture* prescaled_ = nullptr;
    bt::Image x2_, x4_;
    std::vector<SDL_FRect> rects_;
};

std::uint16_t read_controls() {
    using G = bt::Game;
    const bool* k = SDL_GetKeyboardState(nullptr);
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
    if (k[SDL_SCANCODE_ESCAPE]) bits |= G::kEscape;
    if (k[SDL_SCANCODE_P]) bits |= G::kPause;
    return bits;
}

void fail(const std::string& message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Blackthorne Reborn", message.c_str(), nullptr);
}

// settings.ini in the user's preference folder: one "key=value" per line.
struct Settings {
    Filter filter = Filter::Sharp;
    bool wide = false;
};

Settings load_settings(const std::filesystem::path& path) {
    Settings s;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("filter=", 0) == 0) {
            const int f = std::atoi(line.c_str() + 7);
            if (f >= 0 && f < static_cast<int>(Filter::Count)) s.filter = static_cast<Filter>(f);
        } else if (line.rfind("widescreen=", 0) == 0) {
            s.wide = std::atoi(line.c_str() + 11) != 0;
        }
    }
    return s;
}

void save_settings(const std::filesystem::path& path, const Settings& s) {
    std::ofstream(path) << "filter=" << static_cast<int>(s.filter) << "\nwidescreen=" << (s.wide ? 1 : 0) << '\n';
}

}  // namespace

// blackthorne [DATA.DAT] [--filter 0-3] [--wide 0|1] [--level hex] [--size WxH --ticks N --shot out.bmp]
// The last three render one frame after N ticks to a BMP and exit (for tests).
int main(int argc, char** argv) {
    std::filesystem::path data_path = kDefaultData;
    int wide_arg = -1, filter_arg = -1, level_arg = 0, ticks_arg = 0, width = bt::Renderer::kScreenWidth * 4, height = bt::Renderer::kScreenHeight * 4;
    std::string shot;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--filter") filter_arg = std::atoi(next), ++i;
        else if (a == "--wide") wide_arg = std::atoi(next), ++i;
        else if (a == "--level") level_arg = static_cast<int>(std::strtol(next, nullptr, 16)), ++i;
        else if (a == "--ticks") ticks_arg = std::atoi(next), ++i;
        else if (a == "--shot") shot = next, ++i;
        else if (a == "--size") {
            char* end = nullptr;
            width = static_cast<int>(std::strtol(next, &end, 10));
            height = *end == 'x' ? std::atoi(end + 1) : height;
            ++i;
        }
        else data_path = a;
    }
    std::unique_ptr<bt::Archive> archive;
    std::unique_ptr<bt::Game> game;
    try {
        archive = std::make_unique<bt::Archive>(data_path);
        game = std::make_unique<bt::Game>(*archive, data_path.parent_path() / "BTHORNE.EXE");
        game->boot();
        if (level_arg) game->start_level(level_arg);
    } catch (const std::exception& ex) {
        fail(std::string(ex.what()) + "\n\nUsage: blackthorne <path to BTHORNE/DATA.DAT>");
        return 1;
    }
    const bt::Renderer renderer(*archive);
    bt::Audio audio(*archive);
    bt::Music music(*archive);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return 1;
    SDL_Gamepad* pad = nullptr;
    const char* pref = SDL_GetPrefPath("Adolanium", "BlackthorneReborn");
    const std::filesystem::path save_path = std::filesystem::path(pref ? pref : "") / "quicksave.bts";
    const std::filesystem::path settings_path = std::filesystem::path(pref ? pref : "") / "settings.ini";
    SDL_free(const_cast<char*>(pref));
    SDL_Window* window = nullptr;
    SDL_Renderer* sdl = nullptr;
    if (!SDL_CreateWindowAndRenderer("Blackthorne Reborn", width, height,
                                     shot.empty() ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_HIDDEN, &window, &sdl))
        return 1;
    SDL_SetRenderVSync(sdl, 1);
    audio.open();  // the game runs silently if there is no audio device
    if (music.open()) audio.music = &music;
    auto display = std::make_unique<Display>(sdl);
    Settings settings = load_settings(settings_path);
    if (filter_arg >= 0 && filter_arg < static_cast<int>(Filter::Count)) settings.filter = static_cast<Filter>(filter_arg);
    if (wide_arg >= 0) settings.wide = wide_arg != 0;
    display->filter = settings.filter;
    bt::RenderOptions render_opt;
    render_opt.extra = settings.wide ? bt::kWideExtra : 0;
    if (!shot.empty()) {
        bt::Image frame;
        for (int t = 0; t < ticks_arg; ++t) {
            game->tick();
            game->vm().events().clear();
        }
        renderer.render_screen(*game, frame, render_opt);
        display->present(frame, false);
        SDL_Surface* pixels = SDL_RenderReadPixels(sdl, nullptr);
        const bool ok = pixels && SDL_SaveBMP(pixels, shot.c_str());
        SDL_DestroySurface(pixels);
        display.reset();
        SDL_DestroyRenderer(sdl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return ok ? 0 : 1;
    }

    bt::Image frame;
    double acc = 0;
    Uint64 last = SDL_GetTicksNS();
    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F8) {
                display->filter = static_cast<Filter>((static_cast<int>(display->filter) + 1) % static_cast<int>(Filter::Count));
                settings.filter = display->filter;
                save_settings(settings_path, settings);
                SDL_SetWindowTitle(window, (std::string("Blackthorne Reborn - ") + kFilterNames[static_cast<int>(display->filter)]).c_str());
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F7) {
                settings.wide = !settings.wide;
                render_opt.extra = settings.wide ? bt::kWideExtra : 0;
                save_settings(settings_path, settings);
                SDL_SetWindowTitle(window, settings.wide ? "Blackthorne Reborn - widescreen" : "Blackthorne Reborn - 4:3");
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                SDL_CloseGamepad(pad);
                pad = nullptr;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && (e.key.key == SDLK_F5 || e.key.key == SDLK_F9)) {
                try {
                    if (e.key.key == SDLK_F5) game->save_state(save_path);
                    else game->load_state(save_path);
                    SDL_SetWindowTitle(window, e.key.key == SDLK_F5 ? "Blackthorne Reborn - saved" : "Blackthorne Reborn - loaded");
                } catch (const std::exception& ex) {
                    SDL_SetWindowTitle(window, (std::string("Blackthorne Reborn - ") + ex.what()).c_str());
                }
            }
        }
        const Uint64 now = SDL_GetTicksNS();
        acc += static_cast<double>(now - last) / 1e9;
        last = now;
        for (int steps = 0; acc >= kTickSeconds && steps < 5; ++steps) {
            acc -= kTickSeconds;
            game->set_input(static_cast<std::uint16_t>(read_controls() | read_gamepad(pad)));
            try {
                game->tick();
            } catch (const std::exception& ex) {
                fail(std::string("The engine stopped: ") + ex.what());
                running = false;
                break;
            }
            for (const bt::VmEvent& ev : game->vm().events()) {
                if (ev.kind == bt::VmEvent::Kind::Quit) running = false;
                audio.handle(ev, game->machine().ds);
            }
            game->vm().events().clear();
        }
        if (acc > kTickSeconds * 5) acc = 0;  // don't spiral after a stall

        renderer.render_screen(*game, frame, render_opt);
        display->present(frame);
    }
    if (pad) SDL_CloseGamepad(pad);
    display.reset();
    SDL_DestroyRenderer(sdl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
