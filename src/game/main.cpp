// Blackthorne Reborn: runs the original game logic on the reimplemented engine.
// Usage: blackthorne [path/to/DATA.DAT]   (BTHORNE.EXE must sit next to it)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#include "audio/audio.h"
#include "data/archive.h"
#include "engine/game.h"
#include "render/renderer.h"

namespace {

constexpr const char* kDefaultData = "C:/Program Files (x86)/Blizzard Classics/Blackthorne/BTHORNE/DATA.DAT";
constexpr double kTickSeconds = 1.0 / 30.0;  // the original runs at 60 Hz / 2

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

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path data_path = argc > 1 ? argv[1] : kDefaultData;
    std::unique_ptr<bt::Archive> archive;
    std::unique_ptr<bt::Game> game;
    try {
        archive = std::make_unique<bt::Archive>(data_path);
        game = std::make_unique<bt::Game>(*archive, data_path.parent_path() / "BTHORNE.EXE");
        game->boot();
    } catch (const std::exception& ex) {
        fail(std::string(ex.what()) + "\n\nUsage: blackthorne <path to BTHORNE/DATA.DAT>");
        return 1;
    }
    const bt::Renderer renderer(*archive);
    bt::Audio audio(*archive);

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = nullptr;
    SDL_Renderer* sdl = nullptr;
    if (!SDL_CreateWindowAndRenderer("Blackthorne Reborn", bt::Renderer::kWidth * 4, bt::Renderer::kHeight * 4,
                                     SDL_WINDOW_RESIZABLE, &window, &sdl))
        return 1;
    SDL_SetRenderVSync(sdl, 1);
    audio.open();  // the game runs silently if there is no audio device
    SDL_Texture* tex = SDL_CreateTexture(sdl, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         bt::Renderer::kWidth, bt::Renderer::kHeight);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    bt::Image frame;
    double acc = 0;
    Uint64 last = SDL_GetTicksNS();
    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
        }
        const Uint64 now = SDL_GetTicksNS();
        acc += static_cast<double>(now - last) / 1e9;
        last = now;
        for (int steps = 0; acc >= kTickSeconds && steps < 5; ++steps) {
            acc -= kTickSeconds;
            game->set_input(read_controls());
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

        renderer.render(*game, frame);
        SDL_UpdateTexture(tex, nullptr, frame.pixels.data(), frame.width * 4);
        int ww = 0, wh = 0;
        SDL_GetCurrentRenderOutputSize(sdl, &ww, &wh);
        const int scale = std::max(1, std::min(ww / frame.width, wh / frame.height));
        const SDL_FRect dst{static_cast<float>((ww - frame.width * scale) / 2), static_cast<float>((wh - frame.height * scale) / 2),
                            static_cast<float>(frame.width * scale), static_cast<float>(frame.height * scale)};
        SDL_SetRenderDrawColor(sdl, 0, 0, 0, 255);
        SDL_RenderClear(sdl);
        SDL_RenderTexture(sdl, tex, nullptr, &dst);
        SDL_RenderPresent(sdl);
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(sdl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
