#include "render/hd.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <string>

#include "engine/machine.h"
#include "render/filters.h"
#include "render/png.h"

namespace bt {
namespace {

std::uint32_t pal_colour(const std::uint8_t* palette, int index) {
    auto c = [&](int k) { return static_cast<std::uint32_t>((palette[index * 3 + k] & 0x3F) * 255 / 63); };
    return 0xFF000000u | (c(0) << 16) | (c(1) << 8) | c(2);
}

// Over: blend a straight-alpha colour onto an opaque one.
std::uint32_t over(std::uint32_t dst, std::uint32_t src) {
    const std::uint32_t a = src >> 24;
    if (a == 255) return src;
    auto mix = [&](int shift) {
        const std::uint32_t s = (src >> shift) & 0xFF, d = (dst >> shift) & 0xFF;
        return ((s * a + d * (255 - a)) / 255) << shift;
    };
    return 0xFF000000u | mix(16) | mix(8) | mix(0);
}

// How bright palette entry v is shown against how it was painted (for
// cycled colours, which move between entries rather than fade).
float cycle_gain(const std::uint8_t* shown, const std::uint8_t* painted, int v) {
    auto lum = [&](const std::uint8_t* pal) {
        return 0.30f * static_cast<float>(pal[v * 3] & 0x3F) + 0.59f * static_cast<float>(pal[v * 3 + 1] & 0x3F) +
               0.11f * static_cast<float>(pal[v * 3 + 2] & 0x3F);
    };
    const float b = lum(painted), s = lum(shown);
    return b < 2.0f ? 1.0f : std::clamp(s / b, 0.4f, 2.0f);
}

// A painted pixel shown in a colour its palette entry didn't have when it
// was painted (it was black then: flashes, effects): the painting's shading
// in the shown colour.
std::uint32_t colourise(std::uint32_t p, std::uint32_t shown) {
    const float l = (0.30f * static_cast<float>((p >> 16) & 0xFF) + 0.59f * static_cast<float>((p >> 8) & 0xFF) +
                     0.11f * static_cast<float>(p & 0xFF)) / 255.0f;
    const float g = std::clamp(0.35f + 1.3f * l, 0.0f, 1.6f);
    auto ch = [&](int shift) {
        return static_cast<std::uint32_t>(std::clamp(static_cast<int>(static_cast<float>((shown >> shift) & 0xFF) * g + 0.5f), 0, 255)) << shift;
    };
    return (p & 0xFF000000u) | ch(16) | ch(8) | ch(0);
}

std::uint32_t scale_rgb(std::uint32_t p, const float f[3]) {
    auto ch = [&](int shift, float k) {
        const int v = static_cast<int>(((p >> shift) & 0xFF) * k + 0.5f);
        return static_cast<std::uint32_t>(std::clamp(v, 0, 255)) << shift;
    };
    return (p & 0xFF000000u) | ch(16, f[0]) | ch(8, f[1]) | ch(0, f[2]);
}

}  // namespace

ArtPack::ArtPack(std::filesystem::path dir, bool background_loading) : dir_(std::move(dir)), background_loading_(background_loading) {
    std::ifstream in(dir_ / "pack.json");
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t at = json.find("\"scale\"");
    if (at != std::string::npos) {
        const std::size_t colon = json.find(':', at);
        if (colon != std::string::npos) scale_ = std::clamp(std::atoi(json.c_str() + colon + 1), 1, 8);
    }
    auto flag = [&](const char* name) {
        const std::size_t at = json.find(name);
        return at != std::string::npos && json.find("true", at) < json.find_first_of(",}", at);
    };
    smooth_text_ = flag("\"smooth_text\"");
    smooth_fallback_ = flag("\"smooth_fallback\"");
    std::error_code ec;
    has_sprites_ = std::filesystem::is_directory(dir_ / "sprites", ec);
    std::ifstream pal(dir_ / "hud" / "hud.pal", std::ios::binary);
    pal.read(reinterpret_cast<char*>(hud_palette_.data()), static_cast<std::streamsize>(hud_palette_.size()));
    has_hud_ = static_cast<bool>(pal);
    std::ifstream fpal(dir_ / "font" / "font.pal", std::ios::binary);
    fpal.read(reinterpret_cast<char*>(font_palette_.data()), static_cast<std::streamsize>(font_palette_.size()));
    has_font_ = static_cast<bool>(fpal);
    has_cells_ = std::filesystem::is_directory(dir_ / "cells", ec);
}

const Image* ArtPack::cell(int level, int metatile) const {
    if (!has_cells_) return nullptr;
    const int key = level * 1024 + metatile;
    auto it = cells_.find(key);
    if (it == cells_.end()) {
        char name[32];
        std::snprintf(name, sizeof name, "%X_%03x.png", level, metatile);
        auto img = std::make_unique<Image>();
        std::error_code ec;
        const std::filesystem::path path = dir_ / "cells" / name;
        if (!std::filesystem::exists(path, ec) || !read_png(path, *img) || img->width != 16 * scale_ || img->height != 16 * scale_) img.reset();
        it = cells_.emplace(key, std::move(img)).first;
    }
    return it->second.get();
}

const Image* ArtPack::anim_cell(int level, std::uint64_t key) const {
    if (!has_cells_) return nullptr;
    const auto k = std::make_pair(level, key);
    auto it = anim_cells_.find(k);
    if (it == anim_cells_.end()) {
        char name[48];
        std::snprintf(name, sizeof name, "%X_k%016llx.png", level, static_cast<unsigned long long>(key));
        auto img = std::make_unique<Image>();
        std::error_code ec;
        const std::filesystem::path path = dir_ / "cells" / name;
        if (!std::filesystem::exists(path, ec) || !read_png(path, *img) || img->width != 16 * scale_ || img->height != 16 * scale_) img.reset();
        it = anim_cells_.emplace(k, std::move(img)).first;
    }
    return it->second.get();
}

const Image* ArtPack::glyph(int font, int index) const {
    if (!has_font_) return nullptr;
    const int key = font * 4096 + index;
    auto it = glyphs_.find(key);
    if (it == glyphs_.end()) {
        char name[24];
        std::snprintf(name, sizeof name, "%d_%03x.png", font, index);
        auto img = std::make_unique<Image>();
        std::error_code ec;
        const std::filesystem::path path = dir_ / "font" / name;
        if (!std::filesystem::exists(path, ec) || !read_png(path, *img) || img->width != 8 * scale_ || img->height != 8 * scale_) img.reset();
        it = glyphs_.emplace(key, std::move(img)).first;
    }
    return it->second.get();
}

const Image* ArtPack::hud(const std::string& name, int width, int height) const {
    if (!has_hud_) return nullptr;
    auto it = hud_.find(name);
    if (it == hud_.end()) {
        auto img = std::make_unique<Image>();
        std::error_code ec;
        const std::filesystem::path path = dir_ / "hud" / (name + ".png");
        if (!std::filesystem::exists(path, ec) || !read_png(path, *img) || img->width != width * scale_ || img->height != height * scale_)
            img.reset();
        it = hud_.emplace(name, std::move(img)).first;
    }
    return it->second.get();
}

const ArtPack::Sprite* ArtPack::sprite(std::uint64_t key, int size) const {
    if (!has_sprites_) return nullptr;
    auto it = sprites_.find(key);
    if (it == sprites_.end()) {
        char name[24];
        std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(key));
        auto sprite = std::make_unique<Sprite>();
        std::error_code ec;
        const std::filesystem::path path = dir_ / "sprites" / (std::string(name) + ".png");
        std::ifstream pal(dir_ / "sprites" / (std::string(name) + ".pal"), std::ios::binary);
        pal.read(reinterpret_cast<char*>(sprite->palette.data()), static_cast<std::streamsize>(sprite->palette.size()));
        if (!pal || !std::filesystem::exists(path, ec) || !read_png(path, sprite->image) || sprite->image.width != size * scale_ ||
            sprite->image.height != size * scale_)
            sprite.reset();
        it = sprites_.emplace(key, std::move(sprite)).first;
    }
    return it->second.get();
}

const Image* ArtPack::layer(int level, bool background, int width, int height) const {
    if (level != cached_level_) {  // a new level: let the last one's layers go
        cache_.clear();
        cached_level_ = level;
    }
    const auto key = std::make_pair(level, background);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        char id[8];
        std::snprintf(id, sizeof id, "%X", level);
        const std::filesystem::path path = dir_ / "levels" / id / (background ? "bg.png" : "fg.png");
        const int w = width * scale_, h = height * scale_;
        auto load = [path, w, h]() {
            auto img = std::make_shared<Image>();
            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || !read_png(path, *img) || img->width != w || img->height != h) img.reset();
            return img;
        };
        it = cache_.emplace(key, std::async(background_loading_ ? std::launch::async : std::launch::deferred, load).share()).first;
    }
    if (background_loading_ && it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    return it->second.get().get();
}

void HdRenderer::render_screen(const Scene& scene, Image& out, const RenderOptions& opt) const {
    const int S = scale_;
    const Machine& m = *scene.machine;
    const DataSegment& ds = m.ds;

    // The original screen without the shake: the HUD and anything outside
    // the playfield come from it, upscaled.
    RenderOptions flat = opt;
    flat.no_shake = true;
    cov_ = {};
    anim_keys_.clear();
    Image& classic = classic_;
    base_.render_screen(scene, classic, flat);
    Renderer::Layers& layers = layers_;
    base_.render_layers(scene, opt, layers);
    const int W = layers.foreground_back.width, H = Renderer::kHeight;

    // Colours the game animates through palette cycling keep the original
    // pixels, so waterfalls and torches still move.
    std::array<bool, 256> animated{};
    for (int si = 0; si < 0x10; si += 2) {
        if (!(ds.u16(0x1B86) & ds.u16(static_cast<std::uint16_t>(0x288A + si)))) continue;
        const int a = ds.u16(static_cast<std::uint16_t>(0x1BB8 + si)) & 0xFF, b = ds.u16(static_cast<std::uint16_t>(0x1BC8 + si)) & 0xFF;
        for (int i = std::min(a, b); i <= std::max(a, b); ++i) animated[static_cast<std::size_t>(i)] = true;
    }

    // Fades and tints act on the palette. A painted pixel follows its
    // original colour: it is scaled per channel by how much that palette entry
    // changed from the level's palette at load. Where the original pixel is
    // transparent the overall change applies (cycled colours left out: they
    // move, they don't fade). An entry that grew out of black can't be
    // followed, so it keeps the original pixels.
    float factor[3] = {1, 1, 1};
    for (int k = 0; k < 3; ++k) {
        int shown = 0, base = 0;
        for (int i = 0; i < 256; ++i) {
            if (animated[static_cast<std::size_t>(i)]) continue;
            shown += scene.palette[i * 3 + k] & 0x3F;
            base += m.palette_at_load[static_cast<std::size_t>(i * 3 + k)] & 0x3F;
        }
        factor[k] = base ? static_cast<float>(shown) / static_cast<float>(base) : 1.0f;
    }
    // smooth_fallback packs also paint cycled colours: the painting's
    // brightness follows the cycling colour (shimmer), blended between pixels.
    const bool smooth = pack_ && pack_->smooth_fallback() && S == 4;
    std::array<float, 256> shimmer{};
    for (int i = 0; i < 256; ++i) {
        auto lum = [&](const std::uint8_t* pal) {
            return 0.30f * static_cast<float>(pal[i * 3] & 0x3F) + 0.59f * static_cast<float>(pal[i * 3 + 1] & 0x3F) +
                   0.11f * static_cast<float>(pal[i * 3 + 2] & 0x3F);
        };
        const float b = lum(m.palette_at_load.data()), s = lum(scene.palette);
        shimmer[static_cast<std::size_t>(i)] = b < 2.0f ? 1.0f : std::clamp(s / b, 0.4f, 2.0f);
    }
    std::array<std::array<float, 3>, 256> tint{};
    std::array<bool, 256> unfollowable{};  // for the map: grown out of black, or cycled (without smooth_fallback)
    if (!smooth) unfollowable = animated;
    std::array<bool, 256> identity{};
    for (int i = 0; i < 256; ++i) {
        const std::uint32_t shown = pal_colour(scene.palette, i), base = pal_colour(m.palette_at_load.data(), i);
        for (int k = 0; k < 3; ++k) {
            const int s = static_cast<int>((shown >> (16 - 8 * k)) & 0xFF), b = static_cast<int>((base >> (16 - 8 * k)) & 0xFF);
            if (b) tint[i][k] = static_cast<float>(s) / static_cast<float>(b);
            else if (s) unfollowable[static_cast<std::size_t>(i)] = true;
            else tint[i][k] = factor[k];
        }
        identity[static_cast<std::size_t>(i)] = tint[i][0] == 1.0f && tint[i][1] == 1.0f && tint[i][2] == 1.0f;
    }
    const bool factor_identity = factor[0] == 1.0f && factor[1] == 1.0f && factor[2] == 1.0f;
    const int level = ds.u16(0x1CA4);
    // Layer sizes in tiles.
    const int fg_w = ds.u16(Machine::kMapWidth), fg_h = ds.u16(Machine::kMapHeight);
    const int bg_w = ds.u16(0x1D15), bg_h = ds.u16(0x1D17);
    const Image* fg_paint = pack_ ? pack_->layer(level, false, fg_w * 16, fg_h * 16) : nullptr;
    const Image* bg_paint = pack_ && scene.has_background ? pack_->layer(level, true, bg_w * 16, bg_h * 16) : nullptr;
    // Tiles that tile animations rewrote since the load (8x8, 64 bytes each).
    std::array<bool, 0x400> tile_changed{};
    for (std::size_t t = 0; t < tile_changed.size(); ++t)
        tile_changed[t] = !std::equal(m.tiles.begin() + static_cast<std::ptrdiff_t>(t * 64), m.tiles.begin() + static_cast<std::ptrdiff_t>(t * 64 + 64),
                                      m.tiles_at_load.begin() + static_cast<std::ptrdiff_t>(t * 64)) ||
                          !std::equal(m.tile_masks.begin() + static_cast<std::ptrdiff_t>((t * 8) & 0x1FFF), m.tile_masks.begin() + static_cast<std::ptrdiff_t>(((t * 8) & 0x1FFF) + 8),
                                      m.tile_masks_at_load.begin() + static_cast<std::ptrdiff_t>((t * 8) & 0x1FFF));
    // A map cell the game changed since the load (doors, bridges), or one
    // showing an animated tile, is drawn from the original tiles.
    const std::uint16_t metatiles = ds.u16(Machine::kMetatileTable);
    auto changed = [&](bool background, int mx, int my) {
        const std::uint16_t base = ds.u16(background ? 0x25B2 : 0x25B0);
        const int row = background ? ds.u16(0x1CA0) : ds.u16(Machine::kMapRowBytes);
        const std::size_t at = static_cast<std::size_t>((base + my * row + mx * 2) & 0xFFFF);
        if (m.map[at] != m.map_at_load[at] || m.map[(at + 1) & 0xFFFF] != m.map_at_load[(at + 1) & 0xFFFF]) return 2;
        const std::uint16_t mt = static_cast<std::uint16_t>(metatiles + (m.map_u16(static_cast<std::uint16_t>(at)) & 0x3FF) * 8);
        for (int k = 0; k < 4; ++k)
            if (tile_changed[TileRef{m.map_u16(static_cast<std::uint16_t>(mt + 2 * k))}.tile() & 0x3FF]) return 1;
        return 0;
    };

    Image& play = play_;
    play.resize(W * S, H * S);
    auto block = [&](int x, int y, std::uint32_t c) {
        for (int yy = 0; yy < S; ++yy)
            std::fill_n(play.pixels.begin() + static_cast<std::ptrdiff_t>((y * S + yy) * play.width + x * S), S, c);
    };
    // Screen cells nothing went over since the last full redraw show it: all
    // background tiles under all foreground tiles, no sprites
    // (Renderer::render_screen).
    // Text cells in the caption font are redrawn from the full redraw too
    // (Renderer::draw_text).
    const bool captions = opt.text && (ds.u16(0x1CF6) & 0x100);
    auto full = [&](int x, int y) {
        const int sx = x - layers.shift;
        if (sx < 0 || sx >= 256 || y >= 224) return false;
        const int cell = (y >> 3) * 32 + (sx >> 3);
        return !m.repainted[static_cast<std::size_t>(cell)] || (captions && ds.u16(static_cast<std::uint16_t>(0x2D08 + cell * 2)) != 0x20);
    };
    // smooth_fallback: the original layers rounded off with Scale2x twice,
    // for everything that keeps the original pixels.
    if (smooth) {
        const Image* sources[4] = {&layers.background_back, &layers.background_front, &layers.foreground_back, &layers.foreground_front};
        for (int k = 0; k < 4; ++k) {
            scale2x(*sources[k], smooth2_);
            scale2x(smooth2_, smooth4_[k]);
        }
    }
    // The original pixel (x, y) of a layer: a block, or its rounded-off version.
    auto original_pixel = [&](int x, int y, int index, const Image* smooth4) {
        if (!smooth4) {
            if (index >= 0) block(x, y, pal_colour(scene.palette, index));
            return;
        }
        for (int yy = 0; yy < S; ++yy)
            for (int xx = 0; xx < S; ++xx) {
                const int v = Renderer::layer_index(smooth4->pixels[static_cast<std::size_t>(y * S + yy) * smooth4->width + x * S + xx]);
                if (v >= 0) play.pixels[static_cast<std::size_t>(y * S + yy) * play.width + x * S + xx] = pal_colour(scene.palette, v);
            }
    };
    // A map layer pass: back (bit 3 clear) or front tiles of the background or
    // the foreground. In full-redraw cells the background's back pass draws
    // its front tiles too.
    auto map_pass = [&](bool background, bool front) {
        const Image& back_1x = background ? layers.background_back : layers.foreground_back;
        const Image& front_1x = background ? layers.background_front : layers.foreground_front;
        const Image& own = front ? front_1x : back_1x;
        const Image* paint = background ? bg_paint : fg_paint;
        const int ox = background ? layers.bg_x : layers.view_x, oy = background ? layers.bg_y : layers.cam_y;
        const int lw = background ? bg_w : fg_w, lh = background ? bg_h : fg_h;
        const Image* own4 = smooth ? &smooth4_[(background ? 0 : 2) + (front ? 1 : 0)] : nullptr;
        const Image* front4 = smooth ? &smooth4_[background ? 1 : 3] : nullptr;
        // changed() for the cells in view, worked out once.
        const int cx0 = ox >> 4, cy0 = oy >> 4, cw = ((ox + W - 1) >> 4) - cx0 + 1, ch = ((oy + H - 1) >> 4) - cy0 + 1;
        cells_.assign(static_cast<std::size_t>(cw) * ch, 0);
        for (int cy = 0; cy < ch; ++cy)
            for (int cx = 0; cx < cw; ++cx) {
                const int mx = cx0 + cx, my = cy0 + cy;
                const bool inside = mx >= 0 && my >= 0 && mx < lw && my < lh;
                cells_[static_cast<std::size_t>(cy) * cw + cx] = static_cast<std::uint8_t>(!inside ? 1 : changed(background, mx, my));
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool merged = background && full(x, y);
                if (merged && front) continue;
                const std::size_t i1 = static_cast<std::size_t>(y) * W + x;
                const std::uint8_t state = cells_[static_cast<std::size_t>(((oy + y) >> 4) - cy0) * cw + (((ox + x) >> 4) - cx0)];
                const bool original = state != 0;
                const int idx = Renderer::layer_index(own.pixels[i1]);
                const int front_idx = Renderer::layer_index(front_1x.pixels[i1]);
                // A map cell the game changed (2) or that shows animated tiles
                // (1): its painted metatile or animation state, if the pack has one.
                if (state != 0 && pack_ && (ox + x) >= 0 && (oy + y) >= 0) {
                    const int mx = (ox + x) >> 4, my = (oy + y) >> 4;
                    const std::uint16_t lbase = ds.u16(background ? 0x25B2 : 0x25B0);
                    const int row = background ? ds.u16(0x1CA0) : ds.u16(Machine::kMapRowBytes);
                    const int mtv = m.map_u16(static_cast<std::uint16_t>(lbase + my * row + mx * 2)) & 0x3FF;
                    const Image* cellp = nullptr;
                    if (state == 2) cellp = pack_->cell(level, mtv);
                    if (!cellp && Renderer::metatile_animated(scene, mtv)) {
                        auto k = anim_keys_.find(mtv);
                        if (k == anim_keys_.end()) k = anim_keys_.emplace(mtv, Renderer::metatile_key(scene, mtv)).first;
                        cellp = pack_->anim_cell(level, k->second);
                    }
                    const int ci = merged && front_idx >= 0 ? front_idx : idx;
                    if (cellp && !(ci >= 0 && unfollowable[static_cast<std::size_t>(ci)])) {
                        const bool owner_front = front_idx >= 0 && !merged;
                        if (owner_front != front || ci < 0) continue;
                        ++cov_.painted;
                        const float* f = tint[static_cast<std::size_t>(ci)].data();
                        const bool plain = identity[static_cast<std::size_t>(ci)];
                        const int lx = (ox + x) & 15, ly = (oy + y) & 15;
                        for (int yy = 0; yy < S; ++yy) {
                            const std::uint32_t* src = &cellp->pixels[static_cast<std::size_t>(ly * S + yy) * cellp->width + lx * S];
                            std::uint32_t* dst = &play.pixels[static_cast<std::size_t>(y * S + yy) * play.width + x * S];
                            for (int xx = 0; xx < S; ++xx) {
                                const std::uint32_t a = src[xx] >> 24;
                                if (!a) continue;
                                const std::uint32_t c = plain ? src[xx] : scale_rgb(src[xx], f);
                                dst[xx] = a == 255 ? c : over(dst[xx], c);
                            }
                        }
                        continue;
                    }
                }
                if (!paint || original || (!smooth && ((idx >= 0 && unfollowable[static_cast<std::size_t>(idx)]) ||
                                                       (merged && front_idx >= 0 && unfollowable[static_cast<std::size_t>(front_idx)])))) {
                    original_pixel(x, y, idx, own4);
                    if (merged) original_pixel(x, y, front_idx, front4);
                    if (idx >= 0 || (merged && front_idx >= 0)) ++cov_.original, ++cov_.original_map;
                    continue;
                }
                // The painting decides the shape; the original decides whether
                // this spot belongs to the front or the back pass.
                const bool owner_front = front_idx >= 0 && !merged;
                if (owner_front != front) continue;
                const int tint_idx = merged && front_idx >= 0 ? front_idx : idx;
                if (tint_idx >= 0) ++cov_.painted;
                const float* f = tint_idx >= 0 ? tint[static_cast<std::size_t>(tint_idx)].data() : factor;
                const bool plain = tint_idx >= 0 ? identity[static_cast<std::size_t>(tint_idx)] : factor_identity;
                if (smooth && tint_idx >= 0 && unfollowable[static_cast<std::size_t>(tint_idx)] && !animated[static_cast<std::size_t>(tint_idx)]) {
                    const std::uint32_t shown = pal_colour(scene.palette, tint_idx);
                    for (int yy = 0; yy < S; ++yy) {
                        const std::uint32_t* src = &paint->pixels[static_cast<std::size_t>((oy + y) * S + yy) * paint->width + (ox + x) * S];
                        std::uint32_t* dst = &play.pixels[static_cast<std::size_t>(y * S + yy) * play.width + x * S];
                        for (int xx = 0; xx < S; ++xx) {
                            const std::uint32_t a = src[xx] >> 24;
                            if (!a) continue;
                            const std::uint32_t c = colourise(src[xx], shown);
                            dst[xx] = a == 255 ? c : over(dst[xx], c);
                        }
                    }
                    continue;
                }
                if (smooth && tint_idx >= 0 && animated[static_cast<std::size_t>(tint_idx)]) {
                    // Cycled colour: brightness from the cycling, blended with
                    // the neighbouring pixels' so the shimmer has no blocks.
                    const Image& from = merged && front_idx >= 0 ? front_1x : own;
                    const float mine = shimmer[static_cast<std::size_t>(tint_idx)];
                    auto at = [&](int px, int py) {
                        if (px < 0 || py < 0 || px >= W || py >= H) return mine;
                        const int v = Renderer::layer_index(from.pixels[static_cast<std::size_t>(py) * W + px]);
                        return v >= 0 && animated[static_cast<std::size_t>(v)] ? shimmer[static_cast<std::size_t>(v)] : mine;
                    };
                    for (int yy = 0; yy < S; ++yy) {
                        const float fy = (static_cast<float>(yy) + 0.5f) / static_cast<float>(S) - 0.5f;
                        const int y0 = fy < 0 ? y - 1 : y;
                        const float ty = fy < 0 ? fy + 1.0f : fy;
                        const std::uint32_t* src = &paint->pixels[static_cast<std::size_t>((oy + y) * S + yy) * paint->width + (ox + x) * S];
                        std::uint32_t* dst = &play.pixels[static_cast<std::size_t>(y * S + yy) * play.width + x * S];
                        for (int xx = 0; xx < S; ++xx) {
                            const std::uint32_t a = src[xx] >> 24;
                            if (!a) continue;
                            const float fx = (static_cast<float>(xx) + 0.5f) / static_cast<float>(S) - 0.5f;
                            const int x0 = fx < 0 ? x - 1 : x;
                            const float tx = fx < 0 ? fx + 1.0f : fx;
                            const float l = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
                            const float g[3] = {l, l, l};
                            const std::uint32_t c = scale_rgb(src[xx], g);
                            dst[xx] = a == 255 ? c : over(dst[xx], c);
                        }
                    }
                    continue;
                }
                for (int yy = 0; yy < S; ++yy) {
                    const std::uint32_t* src = &paint->pixels[static_cast<std::size_t>((oy + y) * S + yy) * paint->width + (ox + x) * S];
                    std::uint32_t* dst = &play.pixels[static_cast<std::size_t>(y * S + yy) * play.width + x * S];
                    for (int xx = 0; xx < S; ++xx) {
                        const std::uint32_t a = src[xx] >> 24;
                        if (!a) continue;
                        const std::uint32_t c = plain ? src[xx] : scale_rgb(src[xx], f);
                        dst[xx] = a == 255 ? c : over(dst[xx], c);
                    }
                }
            }
    };
    auto image_pass = [&](const Image& img, bool sprites = true) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (sprites && full(x, y)) continue;
                const std::uint32_t p = img.pixels[static_cast<std::size_t>(y) * W + x];
                if (p >> 24) block(x, y, p);
            }
    };
    // Sprite parts of one priority: the pack's replacement where it has one
    // (inside the original's shape, tinted like the map), else the original.
    auto sprite_pass = [&](int priority) {
        if (!opt.sprites) return;
        Renderer::parts(scene, priority, parts_);
        for (const Renderer::Part& part : parts_) {
            const int px = part.x - layers.view_x, py = part.y - layers.cam_y, size = part.size;
            if (px >= W || py >= H || px + size <= 0 || py + size <= 0) continue;
            const ArtPack::Sprite* rep = pack_ ? pack_->sprite(Renderer::part_key(part), size) : nullptr;
            // smooth_fallback: the part rounded off, made when first needed.
            bool rounded = false;
            auto original_part_pixel = [&](int x, int y, int sx, int sy, int v) {
                if (!smooth) {
                    block(sx, sy, pal_colour(scene.palette, v));
                    return;
                }
                if (!rounded) {
                    part1_.resize(size, size);
                    for (int yy = 0; yy < size; ++yy)
                        for (int xx = 0; xx < size; ++xx) {
                            const int c = Renderer::part_pixel(part, xx, yy);
                            part1_.pixels[static_cast<std::size_t>(yy) * size + xx] = c < 0 ? 0u : Renderer::index_pixel(c);
                        }
                    scale2x(part1_, part2_);
                    scale2x(part2_, part4_);
                    rounded = true;
                }
                for (int yy = 0; yy < S; ++yy)
                    for (int xx = 0; xx < S; ++xx) {
                        const int c = Renderer::layer_index(part4_.pixels[static_cast<std::size_t>(y * S + yy) * part4_.width + x * S + xx]);
                        if (c >= 0) play.pixels[static_cast<std::size_t>(sy * S + yy) * play.width + sx * S + xx] = pal_colour(scene.palette, c);
                    }
            };
            for (int y = std::max(0, -py); y < size && py + y < H; ++y)
                for (int x = std::max(0, -px); x < size && px + x < W; ++x) {
                    const int sx = px + x, sy = py + y;
                    const int v = Renderer::part_pixel(part, x, y);
                    if (v < 0 || full(sx, sy)) continue;
                    // The replacement follows the colour change of its pixel
                    // since it was painted (fades, tints, other palettes).
                    float f[3] = {1, 1, 1};
                    const bool cyc = animated[static_cast<std::size_t>(v)];
                    bool plain = true, follow = rep != nullptr && (!cyc || smooth), recolour = false;
                    if (follow && cyc) {  // brightness from the cycling colour
                        const float g = cycle_gain(scene.palette, rep->palette.data(), v);
                        f[0] = f[1] = f[2] = g;
                        plain = false;
                    } else if (follow) {
                        const std::uint32_t shown = pal_colour(scene.palette, v), painted = pal_colour(rep->palette.data(), v);
                        for (int k = 0; k < 3; ++k) {
                            const int s = static_cast<int>((shown >> (16 - 8 * k)) & 0xFF), b = static_cast<int>((painted >> (16 - 8 * k)) & 0xFF);
                            if (b) f[k] = static_cast<float>(s) / static_cast<float>(b);
                            follow = follow && (b != 0 || s == 0);
                            plain = plain && s == b;
                        }
                        if (!follow && smooth) follow = recolour = true;  // grew out of black
                    }
                    if (!follow) {
                        original_part_pixel(x, y, sx, sy, v);
                        ++cov_.original, ++cov_.original_sprites;
                        continue;
                    }
                    ++cov_.painted;
                    // The replacement is drawn the way the part's data is stored:
                    // mirrored parts read it from the other side.
                    const int rx = (part.mirror ? size - 1 - x : x) * S;
                    for (int yy = 0; yy < S; ++yy) {
                        const std::uint32_t* src = &rep->image.pixels[static_cast<std::size_t>(y * S + yy) * rep->image.width + rx];
                        std::uint32_t* dst = &play.pixels[static_cast<std::size_t>(sy * S + yy) * play.width + sx * S];
                        for (int xx = 0; xx < S; ++xx) {
                            const std::uint32_t s = src[part.mirror ? S - 1 - xx : xx];
                            const std::uint32_t a = s >> 24;
                            if (!a) continue;
                            const std::uint32_t c = recolour ? colourise(s, pal_colour(scene.palette, v)) : plain ? s : scale_rgb(s, f);
                            dst[xx] = a == 255 ? c : over(dst[xx], c);
                        }
                    }
                }
        }
    };
    map_pass(true, false);
    sprite_pass(0);
    map_pass(false, false);
    sprite_pass(1);
    sprite_pass(2);
    map_pass(true, true);
    map_pass(false, true);
    sprite_pass(3);
    // Painted glyphs where the pack has them; the rest as before.
    std::vector<bool> painted_cell(32 * 28, false);
    if (pack_ && pack_->has_font() && opt.text) {
        const int which = (ds.u16(0x1CF6) & 0x100) ? 1 : 0;
        const auto& font = base_.font(which);
        for (int cell = 0; cell < 32 * 28; ++cell) {
            const std::uint16_t v = ds.u16(static_cast<std::uint16_t>(0x2D08 + cell * 2));
            if (v == 0x20 || v >= font.size()) continue;
            if (draw_glyph(scene, which, v, layers.shift + (cell % 32) * 8, (cell / 32) * 8, play)) painted_cell[static_cast<std::size_t>(cell)] = true;
        }
    }
    auto text_only_unpainted = [&](Image& img) {  // clears painted cells from a 1x text layer copy
        for (int cell = 0; cell < 32 * 28; ++cell) {
            if (!painted_cell[static_cast<std::size_t>(cell)]) continue;
            const int x0 = layers.shift + (cell % 32) * 8, y0 = (cell / 32) * 8;
            for (int y = y0; y < y0 + 8; ++y)
                std::fill_n(img.pixels.begin() + static_cast<std::ptrdiff_t>(y * img.width + x0), 8, 0u);
        }
    };
    text1_ = layers.text;
    text_only_unpainted(text1_);
    for (std::uint32_t p : text1_.pixels)
        if (p >> 24) ++cov_.original, ++cov_.original_text;
    if (pack_ && pack_->smooth_text() && S == 4 && opt.text) {
        // Text rounded off: Scale2x twice, drawn over the playfield.
        scale2x(text1_, text2_);
        scale2x(text2_, text4_);
        for (int y = 0; y < play.height; ++y)
            for (int x = 0; x < play.width; ++x) {
                const std::uint32_t p = text4_.pixels[static_cast<std::size_t>(y) * text4_.width + x];
                if (p >> 24) play.pixels[static_cast<std::size_t>(y) * play.width + x] = p;
            }
    } else {
        image_pass(text1_, false);
    }
    // Widescreen: the columns beside the game screen are dimmed, as in
    // Renderer::render_screen.
    if (opt.extra && opt.dim_sides) {
        for (int y = 0; y < play.height; ++y)
            for (int x = 0; x < play.width; ++x) {
                if (x / S >= layers.shift && x / S < layers.shift + Renderer::kWidth) continue;
                std::uint32_t& p = play.pixels[static_cast<std::size_t>(y) * play.width + x];
                p = 0xFF000000u | (((p >> 1) & 0x7F7F7Fu) + ((p >> 3) & 0x1F1F1Fu));
            }
    }

    // The page: the original screen upscaled, with the playfield on top.
    // Without a shake it is the output itself.
    const int sx = static_cast<std::int16_t>(ds.u16(0x344)) * S, sy = static_cast<std::int16_t>(ds.u16(0x346)) * S;
    Image& page = sx || sy ? page_ : out;
    page.resize(classic.width * S, classic.height * S);
    for (int y = 0; y < classic.height; ++y) {
        std::uint32_t* row = &page.pixels[static_cast<std::size_t>(y * S) * page.width];
        for (int x = 0; x < classic.width; ++x) std::fill_n(row + x * S, S, classic.pixels[static_cast<std::size_t>(y) * classic.width + x]);
        for (int yy = 1; yy < S; ++yy) std::copy_n(row, page.width, row + static_cast<std::size_t>(yy) * page.width);
    }
    const int px = (ds.u16(0x1CF6) & 1) ? 0 : (classic.width - W) / 2;
    for (int y = 0; y < play.height; ++y)
        std::copy_n(play.pixels.begin() + static_cast<std::ptrdiff_t>(y * play.width), play.width,
                    page.pixels.begin() + static_cast<std::ptrdiff_t>(y * page.width + px * S));
    // The HUD from the pack (Renderer::draw_hud at scale).
    if ((ds.u16(0x1CF6) & 1) && pack_ && pack_->has_hud()) draw_hud(scene, classic, animated, page);

    // The shake moves the whole screen (see Renderer::render_screen).
    if (&page == &out) return;
    out.resize(page.width, page.height);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            const int fx = x + sx, fy = y + sy;
            out.pixels[static_cast<std::size_t>(y) * out.width + x] =
                (fx >= 0 && fx < page.width && fy >= 0 && fy < page.height) ? page.pixels[static_cast<std::size_t>(fy) * page.width + fx] : 0xFF000000u;
        }
}

void HdRenderer::draw_hud(const Scene& scene, const Image& classic, const std::array<bool, 256>& cycled, Image& page) const {
    const int S = scale_;
    const DataSegment& ds = scene.machine->ds;
    const std::array<std::uint8_t, 768>& painted_pal = pack_->hud_palette();
    const int stretch = classic.width - Renderer::kScreenWidth;
    auto block = [&](int x, int y, std::uint32_t c) {
        for (int yy = 0; yy < S; ++yy)
            std::fill_n(page.pixels.begin() + static_cast<std::ptrdiff_t>((y * S + yy) * page.width + x * S), S, c);
    };
    // Screen pixel (x, y) shows palette entry v; `rep` is the replacement it
    // comes from, at (rx, ry) in game pixels.
    const bool smooth = pack_->smooth_fallback() && S == 4;
    auto put = [&](int x, int y, int v, const Image* rep, int rx, int ry) {
        float f[3] = {1, 1, 1};
        const bool cyc = cycled[static_cast<std::size_t>(v)];
        bool plain = true, follow = rep != nullptr && (!cyc || smooth), recolour = false;
        if (follow && cyc) {
            const float g = cycle_gain(scene.palette, painted_pal.data(), v);
            f[0] = f[1] = f[2] = g;
            plain = false;
        } else if (follow) {
            const std::uint32_t shown = pal_colour(scene.palette, v), painted = pal_colour(painted_pal.data(), v);
            for (int k = 0; k < 3; ++k) {
                const int s = static_cast<int>((shown >> (16 - 8 * k)) & 0xFF), b = static_cast<int>((painted >> (16 - 8 * k)) & 0xFF);
                if (b) f[k] = static_cast<float>(s) / static_cast<float>(b);
                follow = follow && (b != 0 || s == 0);
                plain = plain && s == b;
            }
            if (!follow && smooth) follow = recolour = true;
        }
        if (!follow) {
            block(x, y, pal_colour(scene.palette, v));
            ++cov_.original, ++cov_.original_hud;
            return;
        }
        ++cov_.painted;
        for (int yy = 0; yy < S; ++yy) {
            const std::uint32_t* src = &rep->pixels[static_cast<std::size_t>(ry * S + yy) * rep->width + rx * S];
            std::uint32_t* dst = &page.pixels[static_cast<std::size_t>(y * S + yy) * page.width + x * S];
            for (int xx = 0; xx < S; ++xx) {
                const std::uint32_t a = src[xx] >> 24;
                if (!a) continue;
                const std::uint32_t c = recolour ? colourise(src[xx], pal_colour(scene.palette, v)) : plain ? src[xx] : scale_rgb(src[xx], f);
                dst[xx] = a == 255 ? c : over(dst[xx], c);
            }
        }
    };
    const Renderer::Panel& side = base_.side_panel();
    const Renderer::Panel& bottom = base_.bottom_bar();
    const Image* side_rep = pack_->hud("side", side.width, side.height);
    const Image* bottom_rep = pack_->hud("bottom", bottom.width, bottom.height);
    for (int y = 0; y < side.height; ++y)
        for (int x = 0; x < side.width; ++x)
            put(Renderer::kWidth + stretch + x, y, side.pixels[static_cast<std::size_t>(y) * side.width + x], side_rep, x, y);
    // In widescreen the bar repeats a column inside its text box.
    for (int y = 0; y < bottom.height; ++y)
        for (int x = 0; x < bottom.width + stretch; ++x) {
            const int src = x < 128 ? x : x < 128 + stretch ? 128 : x - stretch;
            put(x, Renderer::kHeight + y, bottom.pixels[static_cast<std::size_t>(y) * bottom.width + src], bottom_rep, src, y);
        }
    for (int x = 216 + stretch; x < classic.width; ++x) block(x, Renderer::kScreenHeight - 1, pal_colour(scene.palette, 0));
    // The inventory: empty slot, item icon, selection frame (stamped like sprites).
    auto stamp = [&](const std::vector<std::uint8_t>& pixels, const Image* rep, int x0, int y0, int base) {
        for (int yy = 0; yy < 32; ++yy)
            for (int xx = 0; xx < 32; ++xx) {
                const std::uint8_t v = pixels[static_cast<std::size_t>(yy) * 32 + xx];
                if (v) put(x0 + xx, y0 + yy, v >= 16 ? v : (v | base), rep, xx, yy);
            }
    };
    const auto& icons = base_.item_icons();
    const int selected = ds.u16(0x1CB0);
    char name[16];
    for (int di = 0; di < 0x10; di += 2) {
        const int x = 0x110 + stretch, y = 0xF * di;
        if (!icons.empty()) stamp(icons[0], pack_->hud("icon_00", 32, 32), x, y, 0x80);
        const int item = ds.u16(static_cast<std::uint16_t>(0x1CB2 + di));
        if (item > 0 && item < static_cast<int>(icons.size())) {
            std::snprintf(name, sizeof name, "icon_%02x", item);
            stamp(icons[static_cast<std::size_t>(item)], pack_->hud(name, 32, 32), x, y, 0xF0);
        }
        if (di == selected) stamp(base_.selection_frame(), pack_->hud("selection", 32, 32), x, y, 0xF0);
    }
    // The item name: painted glyphs, else the original font (rounded off
    // with smooth_text).
    const int cells = 16 + stretch / 8;
    if (pack_->has_font()) {
        const int item = ds.u16(static_cast<std::uint16_t>(0x1CB2 + selected));
        const std::uint16_t names = ds.u16(static_cast<std::uint16_t>(0x579E + ds.u16(0x6A) * 2));
        const int pad = stretch / 16;
        bool all = true;
        for (int c = 0; c < cells; ++c) {
            const int i = c - pad;
            const std::uint8_t ch = (i >= 0 && i < 16) ? ds.u8(static_cast<std::uint16_t>(names + item * 16 + i)) : ' ';
            all = draw_glyph(scene, 0, ch - 0x20, 64 + c * 8, Renderer::kHeight + 4, page) && all;
        }
        if (all) return;
    }
    if (pack_->smooth_text() && S == 4) {
        Image name1, name2, name4;
        name1.resize(cells * 8 + 2, 10);  // a pixel of the bar around it, for Scale2x's neighbours
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < name1.width; ++x)
                name1.pixels[static_cast<std::size_t>(y) * name1.width + x] =
                    classic.pixels[static_cast<std::size_t>(Renderer::kHeight + 3 + y) * classic.width + 63 + x];
        scale2x(name1, name2);
        scale2x(name2, name4);
        for (int y = 4; y < 36; ++y)
            for (int x = 4; x < name4.width - 4; ++x)
                page.pixels[static_cast<std::size_t>((Renderer::kHeight + 3) * 4 + y) * page.width + 63 * 4 + x] =
                    name4.pixels[static_cast<std::size_t>(y) * name4.width + x];
    } else {
        for (int y = Renderer::kHeight + 4; y < Renderer::kHeight + 12; ++y)
            for (int x = 64; x < 64 + cells * 8; ++x) block(x, y, classic.pixels[static_cast<std::size_t>(y) * classic.width + x]);
    }
}

bool HdRenderer::draw_glyph(const Scene& scene, int font, int index, int x, int y, Image& out) const {
    const auto& glyphs = base_.font(font);
    if (index < 0 || index >= static_cast<int>(glyphs.size())) return false;
    const Renderer::Glyph& g = glyphs[static_cast<std::size_t>(index)];
    const int S = scale_;
    const Image* rep = pack_ ? pack_->glyph(font, index) : nullptr;
    // The normal font's background is the glyph's commonest colour; a
    // glyph of one colour (a text box's empty cell) is drawn flat, so the
    // cells of a box don't show.
    int counts[256] = {};
    for (std::uint8_t v : g.pixels) ++counts[v];
    int uniform = -1;  // the colour of a one-colour glyph
    for (int v = 0; v < 256; ++v)
        if (counts[v] == 64) uniform = v;
    // The text background: the colour of glyph 0, the empty cell.
    const int bgv = glyphs.empty() ? 0 : glyphs[0].pixels[0];
    if (font == 0 && rep && uniform == bgv) rep = nullptr;
    if (!rep) {
        // Glyphs of one colour have no painting (or don't use it): the normal
        // font fills the cell with that colour, captions draw nothing.
        if (!pack_ || !pack_->has_font() || uniform < 0) return false;
        if (font == 0)
            for (int yy = y * S; yy < (y + 8) * S && yy < out.height; ++yy)
                for (int xx = x * S; xx < (x + 8) * S && xx < out.width; ++xx)
                    if (yy >= 0 && xx >= 0) out.pixels[static_cast<std::size_t>(yy) * out.width + xx] = pal_colour(scene.palette, uniform);
        return true;
    }
    const std::array<std::uint8_t, 768>& painted_pal = pack_->font_palette();
    for (int gy = 0; gy < 8; ++gy)
        for (int gx = 0; gx < 8; ++gx) {
            if (font == 1 && !g.set(gx, gy)) continue;  // captions: only where the mask is set
            const int v = g.pixels[static_cast<std::size_t>(gy * 8 + gx)];
            ++cov_.painted;
            const std::uint32_t shown = pal_colour(scene.palette, v), painted = pal_colour(painted_pal.data(), v);
            float f[3] = {1, 1, 1};
            bool plain = true, follow = true;
            for (int k = 0; k < 3; ++k) {
                const int s = static_cast<int>((shown >> (16 - 8 * k)) & 0xFF), b = static_cast<int>((painted >> (16 - 8 * k)) & 0xFF);
                f[k] = b ? static_cast<float>(s) / static_cast<float>(b) : 1.0f;
                plain = plain && s == b;
                follow = follow && (b != 0 || s == 0);
            }
            for (int yy = 0; yy < S; ++yy) {
                const int py = (y + gy) * S + yy;
                if (py < 0 || py >= out.height) continue;
                for (int xx = 0; xx < S; ++xx) {
                    const int px = (x + gx) * S + xx;
                    if (px < 0 || px >= out.width) continue;
                    std::uint32_t src = rep->pixels[static_cast<std::size_t>(gy * S + yy) * rep->width + gx * S + xx];
                    if (!follow) src = shown;  // a colour the painting can't follow: the original's
                    // The normal font's background: one flat colour where the painting
                    // is darker than the letters, so the cells of a text box don't show.
                    if (font == 0 && v == bgv && src != shown) {  // dark parts fade to the flat background, smoothly by brightness
                        const float lum = static_cast<float>((src >> 16 & 0xFF) * 3 + (src >> 8 & 0xFF) * 6 + (src & 0xFF)) / 10.0f;
                        const float t = std::clamp((lum - 70.0f) / 90.0f, 0.0f, 1.0f);
                        const std::uint32_t bg = pal_colour(scene.palette, bgv);
                        auto mixc = [&](int sh) {
                            const float a = static_cast<float>((bg >> sh) & 0xFF), b = static_cast<float>((src >> sh) & 0xFF);
                            return static_cast<std::uint32_t>(a + (b - a) * t + 0.5f) << sh;
                        };
                        src = (src & 0xFF000000u) | mixc(16) | mixc(8) | mixc(0);
                    }
                    const std::uint32_t a = src >> 24;
                    if (!a) continue;
                    const std::uint32_t c = plain ? src : scale_rgb(src, f);
                    std::uint32_t& dst = out.pixels[static_cast<std::size_t>(py) * out.width + px];
                    dst = a == 255 ? c : over(dst, c);
                }
            }
        }
    return true;
}

}  // namespace bt
