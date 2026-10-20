// Blackthorne Reborn: runs the original game logic on the reimplemented engine.
// Usage: blackthorne [path/to/DATA.DAT]   (BTHORNE.EXE must sit next to it)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cctype>
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
// A frame is shown for as many 60 Hz retraces as the original waits for it
// (ds:5312h): 2 normally (30 frames a second), 1 after the FASTER cheat,
// 4 during fades.
double frame_seconds(const bt::Game& game) {
    const int retraces = game.frame_retraces();
    return (retraces >= 1 && retraces <= 8 ? retraces : 2) / 60.0;
}

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

// Keyboard bindings: settings.ini can override them with lines such as
// "key.jump=Space,Left Ctrl" (SDL key names, several keys per action).
struct Binding {
    const char* name;
    std::uint16_t bit;
    std::vector<SDL_Scancode> keys;
};

std::vector<Binding> default_bindings() {
    using G = bt::Game;
    return {
        {"left", G::kLeft, {SDL_SCANCODE_LEFT}},
        {"right", G::kRight, {SDL_SCANCODE_RIGHT}},
        {"up", G::kUp, {SDL_SCANCODE_UP}},
        {"down", G::kDown, {SDL_SCANCODE_DOWN}},
        {"fire", G::kFire, {SDL_SCANCODE_F}},
        {"shoot_back", G::kShootBack, {SDL_SCANCODE_R}},
        {"jump", G::kJump, {SDL_SCANCODE_SPACE}},
        {"run", G::kRun, {SDL_SCANCODE_D}},
        {"gun", G::kGun, {SDL_SCANCODE_S}},
        {"use_item", G::kUseItem, {SDL_SCANCODE_E}},
        {"item_prev", G::kItemPrev, {SDL_SCANCODE_LEFTBRACKET}},
        {"item_next", G::kItemNext, {SDL_SCANCODE_RIGHTBRACKET}},
        {"enter", G::kEnter, {SDL_SCANCODE_RETURN}},
        {"menu", G::kEscape, {SDL_SCANCODE_ESCAPE}},
        {"give_up", G::kPause, {SDL_SCANCODE_P}},
    };
}

std::uint16_t read_controls(const std::vector<Binding>& bindings) {
    const bool* k = SDL_GetKeyboardState(nullptr);
    std::uint16_t bits = 0;
    for (const Binding& b : bindings)
        for (SDL_Scancode key : b.keys)
            if (k[key]) bits |= b.bit;
    return bits;
}

void fail(const std::string& message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Blackthorne Reborn", message.c_str(), nullptr);
}

// settings.ini in the user's preference folder: one "key=value" per line.
struct Settings {
    Filter filter = Filter::Sharp;
    bool wide = false;
    std::string soundfont;  // empty: the first .sf2 in the settings folder or next to the game
    std::vector<Binding> bindings = default_bindings();
};

// The SoundFont for the music: the configured one, else the first .sf2 found
// in the settings folder or the game's folder.
std::filesystem::path find_soundfont(const Settings& s, const std::filesystem::path& settings_dir) {
    if (!s.soundfont.empty()) return std::filesystem::path(s.soundfont);
    const char* base = SDL_GetBasePath();
    for (const std::filesystem::path& dir : {settings_dir, std::filesystem::path(base ? base : "")}) {
        std::error_code ec;
        if (dir.empty() || !std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::string ext = entry.path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".sf2") return entry.path();
        }
    }
    return {};
}

Settings load_settings(const std::filesystem::path& path) {
    Settings s;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("filter=", 0) == 0) {
            const int f = std::atoi(line.c_str() + 7);
            if (f >= 0 && f < static_cast<int>(Filter::Count)) s.filter = static_cast<Filter>(f);
        } else if (line.rfind("soundfont=", 0) == 0) {
            s.soundfont = line.substr(10);
        } else if (line.rfind("widescreen=", 0) == 0) {
            s.wide = std::atoi(line.c_str() + 11) != 0;
        } else if (line.rfind("key.", 0) == 0 && line.find('=') != std::string::npos) {
            const std::string name = line.substr(4, line.find('=') - 4);
            for (Binding& b : s.bindings) {
                if (name != b.name) continue;
                std::vector<SDL_Scancode> keys;
                std::string list = line.substr(line.find('=') + 1);
                for (std::size_t pos = 0; pos <= list.size();) {
                    const std::size_t comma = std::min(list.find(',', pos), list.size());
                    const SDL_Scancode sc = SDL_GetScancodeFromName(list.substr(pos, comma - pos).c_str());
                    if (sc != SDL_SCANCODE_UNKNOWN) keys.push_back(sc);
                    pos = comma + 1;
                }
                if (!keys.empty()) b.keys = keys;
            }
        }
    }
    return s;
}

void save_settings(const std::filesystem::path& path, const Settings& s) {
    std::ofstream out(path);
    out << "filter=" << static_cast<int>(s.filter) << "\nwidescreen=" << (s.wide ? 1 : 0) << "\nsoundfont=" << s.soundfont
        << '\n';
    for (const Binding& b : s.bindings) {
        out << "key." << b.name << '=';
        for (std::size_t i = 0; i < b.keys.size(); ++i) out << (i ? "," : "") << SDL_GetScancodeName(b.keys[i]);
        out << '\n';
    }
}

}  // namespace

// blackthorne [DATA.DAT] [--filter 0-3] [--wide 0|1] [--level hex]
//             [--size WxH --ticks N --shot out.bmp [--input spec] [--keys frame:TEXT]] [--size WxH --ticks N --shot out.bmp]
// The last three render one frame after N ticks to a BMP and exit (for tests).
int main(int argc, char** argv) {
    std::filesystem::path data_path = kDefaultData;
    int wide_arg = -1, filter_arg = -1, level_arg = 0, ticks_arg = 0, width = bt::Renderer::kScreenWidth * 4, height = bt::Renderer::kScreenHeight * 4;
    std::string shot, input_spec, keys_spec;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--filter") filter_arg = std::atoi(next), ++i;
        else if (a == "--wide") wide_arg = std::atoi(next), ++i;
        else if (a == "--level") level_arg = static_cast<int>(std::strtol(next, nullptr, 16)), ++i;
        else if (a == "--ticks") ticks_arg = std::atoi(next), ++i;
        else if (a == "--shot") shot = next, ++i;
        else if (a == "--input") input_spec = next, ++i;
        else if (a == "--keys") keys_spec = next, ++i;
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
    // The game's own progress file (the original's d:\savegame.000): the 4
    // characters of the last level password, read by CONTINUE (opcode FB).
    const std::filesystem::path password_path = std::filesystem::path(pref ? pref : "") / "savegame.000";
    game->vm().load_password_ = [password_path](std::array<char, 4>& pw) {
        std::ifstream in(password_path, std::ios::binary);
        return static_cast<bool>(in.read(pw.data(), 4));
    };
    SDL_free(const_cast<char*>(pref));
    SDL_Window* window = nullptr;
    SDL_Renderer* sdl = nullptr;
    if (!SDL_CreateWindowAndRenderer("Blackthorne Reborn", width, height,
                                     shot.empty() ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_HIDDEN, &window, &sdl))
        return 1;
    SDL_SetRenderVSync(sdl, 1);
    Settings settings = load_settings(settings_path);
    audio.open();  // the game runs silently if there is no audio device
    if (music.open(find_soundfont(settings, settings_path.parent_path()))) audio.music = &music;
    auto display = std::make_unique<Display>(sdl);
    if (filter_arg >= 0 && filter_arg < static_cast<int>(Filter::Count)) settings.filter = static_cast<Filter>(filter_arg);
    if (wide_arg >= 0) settings.wide = wide_arg != 0;
    display->filter = settings.filter;
    bt::RenderOptions render_opt;
    render_opt.extra = settings.wide ? bt::kWideExtra : 0;
    // What the game asks of the host after a tick: sounds and music, saving
    // the level password, quitting. Returns false when the game quits.
    auto handle_events = [&]() {
        bool keep_running = true;
        for (const bt::VmEvent& ev : game->vm().events()) {
            if (ev.kind == bt::VmEvent::Kind::Quit) keep_running = false;
            if (ev.kind == bt::VmEvent::Kind::SavePassword) {  // sub_10C0D writes ds:64E7h
                const bt::DataSegment& ds = game->machine().ds;
                const char pw[4] = {static_cast<char>(ds.u8(0x64E7)), static_cast<char>(ds.u8(0x64E8)),
                                    static_cast<char>(ds.u8(0x64E9)), static_cast<char>(ds.u8(0x64EA))};
                std::ofstream(password_path, std::ios::binary).write(pw, 4);
            }
            audio.handle(ev, game->machine().ds);
        }
        game->vm().events().clear();
        return keep_running;
    };
    if (!shot.empty()) {
        // Headless run: the same ticks, events, rendering and presentation as
        // the interactive loop, frame after frame, then the last frame saved.
        bt::Image frame;
        // Scripted input as bttrace takes it ("from-to:hexbits,..."; tick k
        // uses the bits at k - 1) and typed text ("frame:TEXT", one key per
        // frame from that frame on).
        struct Span { int from, to; unsigned bits; };
        std::vector<Span> spans;
        for (std::size_t pos = 0; pos < input_spec.size();) {
            std::size_t end = input_spec.find(',', pos);
            if (end == std::string::npos) end = input_spec.size();
            const std::string part = input_spec.substr(pos, end - pos);
            char* p = nullptr;
            Span s{};
            s.from = static_cast<int>(std::strtol(part.c_str(), &p, 10));
            if (*p == '-') s.to = static_cast<int>(std::strtol(p + 1, &p, 10));
            if (*p == ':') {
                s.bits = static_cast<unsigned>(std::strtoul(p + 1, nullptr, 16));
                spans.push_back(s);
            }
            pos = end + 1;
        }
        int keys_from = -1;
        std::string keys;
        if (const std::size_t colon = keys_spec.find(':'); colon != std::string::npos) {
            keys_from = std::atoi(keys_spec.c_str());
            keys = keys_spec.substr(colon + 1);
        }
        for (int t = 1; t <= ticks_arg; ++t) {
            unsigned bits = 0;
            for (const Span& s : spans)
                if (s.from <= t - 1 && t - 1 <= s.to) bits |= s.bits;
            game->set_input(static_cast<std::uint16_t>(bits));
            const int k = t - 1 - keys_from;
            if (keys_from >= 0 && k >= 0 && k < static_cast<int>(keys.size())) game->type_key(keys[static_cast<std::size_t>(k)]);
            try {
                game->tick();
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "engine error at tick %d: %s\n", t, ex.what());
                return 2;
            }
            const bool keep_running = handle_events();
            renderer.render_screen(*game, frame, render_opt);
            display->present(frame, false);
            if (!keep_running) break;
        }
        SDL_Surface* pixels = SDL_RenderReadPixels(sdl, nullptr);
        const bool ok = pixels && SDL_SaveBMP(pixels, shot.c_str());
        SDL_DestroySurface(pixels);
        display.reset();
        SDL_DestroyRenderer(sdl);
        SDL_DestroyWindow(window);
        audio.close();
        music.close();
        SDL_Quit();
        return ok ? 0 : 1;
    }
    // Write the file on first run so the bindings are easy to find and edit.
    if (!std::filesystem::exists(settings_path)) save_settings(settings_path, settings);

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
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {  // letters and digits for passwords and cheats
                const SDL_Keycode k = e.key.key;
                if (k >= SDLK_A && k <= SDLK_Z) game->type_key(static_cast<char>('A' + (k - SDLK_A)));
                else if (k >= SDLK_0 && k <= SDLK_9) game->type_key(static_cast<char>('0' + (k - SDLK_0)));
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
        for (int steps = 0; acc >= frame_seconds(*game) && steps < 5; ++steps) {
            acc -= frame_seconds(*game);
            game->set_input(static_cast<std::uint16_t>(read_controls(settings.bindings) | read_gamepad(pad)));
            try {
                game->tick();
            } catch (const std::exception& ex) {
                fail(std::string("The engine stopped: ") + ex.what());
                running = false;
                break;
            }
            if (!handle_events()) running = false;
        }
        if (acc > frame_seconds(*game) * 5) acc = 0;  // don't spiral after a stall

        renderer.render_screen(*game, frame, render_opt);
        display->present(frame);
    }
    if (pad) SDL_CloseGamepad(pad);
    display.reset();
    SDL_DestroyRenderer(sdl);
    SDL_DestroyWindow(window);
    audio.close();
    music.close();
    SDL_Quit();
    return 0;
}
