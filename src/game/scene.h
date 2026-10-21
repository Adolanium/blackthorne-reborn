// The scene renderer (docs/VISUALS.md): draws the game as a scene of whole
// painted images on the GPU instead of rebuilding the original's pixels.
// Levels are two paintings (background, foreground) with a soft "in front"
// layer; characters are whole painted poses placed and flipped from the
// game's state; changed map cells swap painted pieces into the level
// painting; text and HUD are painted glyphs and panels. Everything is drawn
// at the window's resolution with smooth scaling. The game logic is the
// same; only the drawing differs from the classic and HD renderers.
#pragma once

#include <SDL3/SDL.h>

#include <map>
#include <string>
#include <vector>

#include "render/hd.h"
#include "render/renderer.h"

// One actor's pose as the game shows it this frame: its sprite parts
// assembled, named by a hash of their keys and layout (the same names
// blackthorne --poses writes), with where it is in the world.
struct PoseMatch {
    std::string name;
    std::string signature;           // key,size,x,y,mirror; per part
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the pose box in world pixels
    bool flip = false;                   // facing left: the pose is stored facing right
    int priority = 0;
    std::vector<bt::Renderer::Part> parts;  // as drawn, first on top
    std::vector<int> offsets;               // their part pool offsets
};
bool match_pose(const bt::Scene& scene, int slot, PoseMatch& out);

class SceneRenderer {
public:
    SceneRenderer(SDL_Renderer* sdl, const bt::Renderer& base, const bt::ArtPack& pack);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Whether the pack has what the scene renderer needs.
    bool usable() const { return !pose_index_.empty(); }
    // Loads this frame's level art; false while it isn't ready (the caller
    // draws another way meanwhile).
    bool prepare(const bt::Scene& scene);
    // Draws the screen (playfield, text, HUD) into `dst` of the current
    // render target. `extra`: widescreen columns per side (game pixels).
    void draw(const bt::Scene& scene, const SDL_FRect& dst, int extra);

private:
    struct Placed {
        float x, y, w, h;
    };
    SDL_Texture* texture(const bt::Image& img);
    SDL_Texture* pose_texture(const std::string& name);
    SDL_Texture* part_texture(const bt::Renderer::Part& part);
    SDL_Texture* glyph_texture(int font, int index);
    SDL_Texture* hud_texture(const std::string& name, int w, int h);
    void free_level();
    void build_front(const bt::Scene& scene, const bt::Image& fg);
    void update_cells(const bt::Scene& scene);
    void draw_tex(SDL_Texture* t, float x, float y, float w, float h, bool flip = false);

    SDL_Renderer* sdl_;
    const bt::Renderer& base_;
    const bt::ArtPack& pack_;
    std::map<std::string, std::array<int, 5>> pose_index_;  // name -> ox, oy, w, h, cell (game pixels)
    int cell_ = 64, scale_ = 4;

    int level_ = -1;
    SDL_Texture *bg_ = nullptr, *fg_ = nullptr, *front_ = nullptr;
    const bt::Image* fg_img_ = nullptr;  // the level's foreground painting
    bt::Image front_img_;
    std::vector<int> applied_;                    // the metatile each fg cell shows in the texture
    std::map<int, std::pair<int, int>> where_;    // metatile -> a cell showing it at load

    std::map<std::string, SDL_Texture*> poses_;
    std::map<std::uint64_t, SDL_Texture*> parts_;
    std::map<int, SDL_Texture*> glyphs_;
    std::map<std::string, SDL_Texture*> hud_;

    // This frame's mapping from game pixels to the target.
    float k_ = 1, ox_ = 0, oy_ = 0;
    Uint8 mod_[3] = {255, 255, 255};
};
