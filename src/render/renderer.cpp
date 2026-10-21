#include "render/renderer.h"

#include <algorithm>
#include <array>

namespace bt {
namespace {


std::uint32_t colour(const std::uint8_t* palette, int index) {
    auto c = [&](int k) {
        const int v = palette[(index & 0xFF) * 3 + k] & 0x3F;
        return static_cast<std::uint32_t>(v * 255 / 63);
    };
    return 0xFF000000u | (c(0) << 16) | (c(1) << 8) | c(2);
}

}  // namespace

// Panel images: u16 width, u16 height, then Mode X planes (plane-major).
Renderer::Renderer(const Archive& archive) {
    auto panel = [&](int entry) {
        const Bytes b = archive.entry(entry);
        Panel p;
        p.width = b[0] | (b[1] << 8);
        p.height = b[2] | (b[3] << 8);
        p.pixels.resize(static_cast<std::size_t>(p.width) * p.height);
        const int cols = p.width / 4;
        for (int plane = 0; plane < 4; ++plane)
            for (int y = 0; y < p.height; ++y)
                for (int c = 0; c < cols; ++c)
                    p.pixels[static_cast<std::size_t>(y) * p.width + c * 4 + plane] = b[4 + (plane * p.height + y) * cols + c];
        return p;
    };
    side_panel_ = panel(73);
    bottom_bar_ = panel(74);
    for (const SpriteFrame& f : decode_sprite_bank(archive.resource(0x4C), 32, 32)) item_icons_.push_back(f.pixels);
    {  // resource 0x4D is stored in the blitter rows; frame 0 is the selection frame
        const Bytes b = archive.resource(0x4D);
        std::vector<std::uint8_t> raw;
        for (std::size_t g = 0; g < 128 && g * 9 + 9 <= b.size(); ++g)
            for (int k = 0; k < 8; ++k) raw.push_back(b[g * 9 + 1 + k]);
        raw.resize(1024);
        selection_frame_.assign(1024, 0);
        for (int plane = 0; plane < 4; ++plane)
            for (int y = 0; y < 32; ++y)
                for (int c = 0; c < 8; ++c)
                    selection_frame_[static_cast<std::size_t>(y) * 32 + c * 4 + plane] = raw[static_cast<std::size_t>((plane * 32 + y) * 8 + c)];
    }

    for (int f = 0; f < 2; ++f) {
        const Bytes font = archive.entry(288 + 2 * f);
        const Bytes masks = archive.entry(289 + 2 * f);
        for (std::size_t o = 0, i = 0; o + 64 <= font.size(); o += 64, ++i) {
            Glyph g{};
            for (int plane = 0; plane < 4; ++plane)
                for (int y = 0; y < 8; ++y)
                    for (int col = 0; col < 2; ++col) g.pixels[y * 8 + col * 4 + plane] = font[o + plane * 16 + y * 2 + col];
            for (int k = 0; k < 8 && i * 8 + k < masks.size(); ++k) g.mask[k] = masks[i * 8 + k];
            fonts_[f].push_back(g);
        }
    }
}

void Renderer::render_screen(const Scene& scene, Image& out, const RenderOptions& opt) const {
    Image play;
    render(scene, play, opt);
    const int screen_w = kScreenWidth + 2 * opt.extra;
    out.resize(screen_w, kScreenHeight);
    const bool hud = (scene.machine->ds.u16(0x1CF6) & 1);
    // Screen shake (sub_10AC1 offsets, applied by the page flip).
    const DataSegment& ds = (*scene.machine).ds;
    const int shake_x = opt.no_shake ? 0 : static_cast<std::int16_t>(ds.u16(0x344));
    const int shake_y = opt.no_shake ? 0 : static_cast<std::int16_t>(ds.u16(0x346));
    // Without the HUD the original centres the playfield horizontally but
    // keeps it at the top (the bottom 16 lines stay black).
    // The page: playfield and HUD as drawn, before the shake.
    Image page;
    page.resize(screen_w, kScreenHeight);
    const int px = hud ? 0 : (screen_w - play.width) / 2;
    for (int y = 0; y < kHeight; ++y)
        std::copy_n(play.pixels.begin() + static_cast<std::ptrdiff_t>(y * play.width), play.width,
                    page.pixels.begin() + static_cast<std::ptrdiff_t>(y * screen_w + px));
    if (hud) draw_hud(scene, page);
    // The shake moves the CRTC start address, so the whole screen moves,
    // HUD included.
    for (int y = 0; y < kScreenHeight; ++y) {
        const int sy = y + shake_y;
        for (int x = 0; x < screen_w; ++x) {
            const int sx = x + shake_x;
            out.pixels[static_cast<std::size_t>(y) * screen_w + x] =
                (sx >= 0 && sx < screen_w && sy >= 0 && sy < kScreenHeight) ? page.pixels[static_cast<std::size_t>(sy) * screen_w + sx]
                                                                           : 0xFF000000u;
        }
    }
}

// A HUD stamp, drawn like a sprite part (sub_157F0): pixels below 16 take the
// colour base, others are absolute colours; 0 is transparent.
void Renderer::stamp(const Scene& scene, Image& out, const std::vector<std::uint8_t>& pixels, int size, int x, int y,
                     std::uint8_t colour_base) const {
    for (int yy = 0; yy < size; ++yy)
        for (int xx = 0; xx < size; ++xx) {
            const std::uint8_t v = pixels[static_cast<std::size_t>(yy) * size + xx];
            const int sx = x + xx, sy = y + yy;
            if (!v || sx < 0 || sy < 0 || sx >= out.width || sy >= out.height) continue;
            out.pixels[static_cast<std::size_t>(sy) * out.width + sx] = colour(scene.palette, v >= 16 ? v : v | colour_base);  // drawn like sprites
        }
}

// The right panel with the 8 inventory slots (sub_1423E), the bottom bar and
// the selected item name (sub_142D4 / sub_14DE6).
void Renderer::draw_hud(const Scene& scene, Image& out) const {
    const DataSegment& ds = (*scene.machine).ds;
    // In widescreen the bar is stretched by repeating a column inside its
    // text box (x 67-190).
    const int stretch = out.width - kScreenWidth;
    auto blit = [&](const Panel& p, int x0, int y0, int width) {
        for (int y = 0; y < p.height; ++y)
            for (int x = 0; x < width; ++x) {
                const int src = width == p.width ? x : x < 128 ? x : x < 128 + stretch ? 128 : x - stretch;
                out.pixels[static_cast<std::size_t>(y + y0) * out.width + x + x0] =
                    colour(scene.palette, p.pixels[static_cast<std::size_t>(y) * p.width + src]);
            }
    };
    blit(side_panel_, kWidth + stretch, 0, side_panel_.width);
    blit(bottom_bar_, 0, kHeight, bottom_bar_.width + stretch);
    // On the original the bar's last line from x=216 shares video memory with
    // a page and shows black there (sub_14D46 copies over it).
    for (int x = 216 + stretch; x < out.width; ++x)
        out.pixels[static_cast<std::size_t>(kScreenHeight - 1) * out.width + x] = colour(scene.palette, 0);
    const int selected = ds.u16(0x1CB0);
    for (int di = 0; di < 0x10; di += 2) {
        const int x = 0x110 + stretch, y = 0xF * di;
        if (!item_icons_.empty()) stamp(scene, out, item_icons_[0], 32, x, y, 0x80);
        const int item = ds.u16(static_cast<std::uint16_t>(0x1CB2 + di));
        if (item > 0 && item < static_cast<int>(item_icons_.size())) stamp(scene, out, item_icons_[item], 32, x, y, 0xF0);
        if (di == selected) stamp(scene, out, selection_frame_, 32, x, y, 0xF0);
    }
    // Item name: 16 characters from the table for the language at ds:579Eh,
    // drawn whole by sub_146BA (sub_14DE6); the space glyphs fill the box. In
    // widescreen the box is wider: spaces fill it and the name is centred.
    const int item = ds.u16(static_cast<std::uint16_t>(0x1CB2 + selected));
    const std::uint16_t names = ds.u16(static_cast<std::uint16_t>(0x579E + ds.u16(0x6A) * 2));
    const auto& font = fonts_[0];
    const int pad = stretch / 16, cells = 16 + stretch / 8;
    for (int c = 0; c < cells; ++c) {
        const int i = c - pad;
        const std::uint8_t ch = (i >= 0 && i < 16) ? ds.u8(static_cast<std::uint16_t>(names + item * 16 + i)) : ' ';
        const int g = ch - 0x20;
        if (g < 0 || g >= static_cast<int>(font.size())) continue;
        const Glyph& gl = font[g];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                out.pixels[static_cast<std::size_t>(kHeight + 4 + y) * out.width + 64 + c * 8 + x] = colour(scene.palette, gl.pixels[y * 8 + x]);
    }
}

void Renderer::render(const Scene& scene, Image& out, const RenderOptions& opt) const {
    const int width = kWidth + 2 * opt.extra;
    out.resize(width, kHeight);
    const Machine& m = *scene.machine;
    const DataSegment& ds = m.ds;
    auto s16 = [&](std::uint16_t a) { return static_cast<int>(static_cast<std::int16_t>(ds.u16(a))); };
    // The view is the game camera widened on both sides, kept inside the map
    // (or centred on a map narrower than the view).
    const int cam_x = s16(0x1B4E), cam_y = s16(0x1B50);
    const int map_px = ds.u16(Machine::kMapWidth) * 16;
    int view_x = cam_x - opt.extra;
    if (opt.extra) view_x = map_px >= width ? std::clamp(view_x, 0, map_px - width) : (map_px - width) / 2;
    const int shift = cam_x - view_x;  // where the 256-wide game screen starts
    auto background = [&](int pass, Image& to) {
        if (opt.background && scene.has_background)
            draw_layer(scene, ds.u16(0x25B2), ds.u16(0x1CA0), ds.u16(0x1D15), ds.u16(0x1D17), s16(0x1B52) - shift, s16(0x1B54),
                       pass, to);
    };
    auto foreground = [&](int pass, Image& to) {
        if (opt.foreground)
            draw_layer(scene, ds.u16(0x25B0), ds.u16(Machine::kMapRowBytes), ds.u16(Machine::kMapWidth), ds.u16(Machine::kMapHeight),
                       view_x, cam_y, pass, to);
    };
    auto sprites = [&](int priority) {
        if (opt.sprites) draw_parts(scene, view_x, cam_y, priority, out);
    };
    // sub_15BA0's order for repainted cells: tiles with bit 3 set (in the
    // tile word) and sprite priority 3 are in front; within a layer of
    // sprites, lower part numbers are drawn later (on top).
    background(0, out);
    sprites(0);
    foreground(0, out);
    sprites(1);
    sprites(2);
    background(8, out);
    foreground(8, out);
    sprites(3);
    // The full redraw (sub_125BB): all background tiles under all foreground
    // tiles. Cells nothing has gone over since then still show it, and text
    // cells in the caption font are drawn over it.
    Image tiles;
    tiles.resize(width, kHeight);
    for (int pass : {0, 8}) background(pass, tiles);
    for (int pass : {0, 8}) foreground(pass, tiles);
    for (int cell = 0; cell < 32 * 28; ++cell) {
        if (m.repainted[static_cast<std::size_t>(cell)]) continue;
        const int x0 = shift + (cell % 32) * 8, y0 = (cell / 32) * 8;
        for (int y = y0; y < y0 + 8; ++y)
            std::copy_n(tiles.pixels.begin() + static_cast<std::ptrdiff_t>(y * width + x0), 8,
                        out.pixels.begin() + static_cast<std::ptrdiff_t>(y * width + x0));
    }
    if (opt.extra && opt.dim_sides) {
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < width; ++x) {
                if (x >= shift && x < shift + kWidth) continue;
                std::uint32_t& p = out.pixels[static_cast<std::size_t>(y) * width + x];
                p = 0xFF000000u | (((p >> 1) & 0x7F7F7Fu) + ((p >> 3) & 0x1F1F1Fu));  // about 62%
            }
    }
    if (opt.text) draw_text(scene, shift, (ds.u16(0x1CF6) & 0x100) ? &tiles : nullptr, out);
}

// One map layer (cells: bits 0-9 metatile) over the camera window.
void Renderer::draw_layer(const Scene& scene, std::uint16_t layer_offset, int row_bytes, int map_w, int map_h, int cam_x,
                          int cam_y, int pass, Image& out) const {
    const Machine& m = (*scene.machine);
    const DataSegment& ds = m.ds;
    const std::uint16_t metatiles = ds.u16(Machine::kMetatileTable);
    const int width = out.width, height = out.height;
    for (int my = cam_y >> 4; my <= (cam_y + height) >> 4; ++my) {
        for (int mx = cam_x >> 4; mx <= (cam_x + width) >> 4; ++mx) {
            if (mx < 0 || my < 0 || mx >= map_w || my >= map_h) continue;
            const std::uint16_t cell = m.map_u16(static_cast<std::uint16_t>(layer_offset + my * row_bytes + mx * 2));
            const std::uint16_t mt = static_cast<std::uint16_t>(metatiles + (cell & 0x3FF) * 8);
            for (int k = 0; k < 4; ++k) {
                const TileRef ref{m.map_u16(static_cast<std::uint16_t>(mt + 2 * k))};
                if ((ref.raw & 8) != pass) continue;  // bit 3: drawn in front of sprites below priority 3
                const std::size_t tile = static_cast<std::size_t>(ref.tile());
                const std::uint8_t* px = m.tiles.data() + tile * 64;
                const std::uint8_t* mask = m.tile_masks.data() + ((tile * 8) & 0x1FFF);
                const int ox = mx * 16 + (k & 1) * 8 - cam_x, oy = my * 16 + (k >> 1) * 8 - cam_y;
                for (int y = 0; y < 8; ++y) {
                    const int sy = oy + y;
                    if (sy < 0 || sy >= height) continue;
                    for (int x = 0; x < 8; ++x) {
                        const int sx = ox + x;
                        if (sx < 0 || sx >= width) continue;
                        // Planar 8x8: byte = plane * 16 + row * 2 + col; one mask bit per data byte.
                        const int ty = ref.flip_y() ? 7 - y : y, tx = ref.flip_x() ? 7 - x : x;
                        const int plane = tx & 3, col = tx >> 2;
                        if (!(mask[plane * 2 + (ty >> 2)] & (0x80 >> ((ty & 3) * 2 + col)))) continue;
                        const int v = px[plane * 16 + ty * 2 + col];
                        out.pixels[static_cast<std::size_t>(sy) * width + sx] = colour(scene.palette, v >= 16 ? v : v + ref.palette_row() * 16);
                    }
                }
            }
        }
    }
}

// Sprite parts: pool entries with attribute bit 0x80 (allocated) and without
// 0x40 (hidden). Pixel data is rows of [mask][8 pixels]; the pixel bytes form
// the same Mode X planar square as the sprite banks.
// sub_159F3: the parts of one priority, from the last part to the first.
void Renderer::parts(const Scene& scene, int priority, std::vector<Part>& out) {
    const Machine& m = (*scene.machine);
    const DataSegment& ds = m.ds;
    out.clear();
    for (int p = 0xFE; p >= 0; p -= 2) {
        const std::uint16_t attr = ds.field(0x452, p);
        if (!(attr & 0x80) || (attr & 0x340) != (priority << 8)) continue;
        Part part;
        part.size = ds.field(0x952, p);
        if (part.size != 8 && part.size != 16 && part.size != 32 && part.size != 64) continue;
        part.data = m.sprite_data(ds.field(0x852, p), ds.field(0x752, p));
        if (!part.data) continue;
        part.x = static_cast<std::int16_t>(ds.field(0x552, p));
        part.y = static_cast<std::int16_t>(ds.field(0x652, p));
        // Bit 0x10 mirrors the part layout only: the drawers (sub_14E33,
        // sub_15321) never flip pixels vertically.
        part.mirror = (attr & 0x20) != 0;
        part.base = static_cast<std::uint8_t>(ds.field(0xF52, p));
        out.push_back(part);
    }
}

int Renderer::part_pixel(const Part& part, int x, int y) {
    const int size = part.size;
    const int scol = part.mirror ? size - 1 - x : x;
    const int plane = scol & 3;
    // Index into the pixels without the mask bytes (one per 8 pixels).
    const int r = (plane * size + y) * (size / 4) + (scol >> 2);
    const std::uint8_t v = part.data[(r / 8) * 9 + 1 + r % 8];
    if (!v) return -1;
    return v >= 16 ? v : v | part.base;  // like tiles: a set high nibble is an absolute colour
}

std::uint64_t Renderer::part_key(const Part& part) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto mix = [&](std::uint8_t b) { h = (h ^ b) * 1099511628211ull; };
    mix(static_cast<std::uint8_t>(part.size));
    mix(part.base);
    for (int i = 0; i < part.size * part.size / 8 * 9; ++i) mix(part.data[i]);
    return h;
}

void Renderer::draw_parts(const Scene& scene, int cam_x, int cam_y, int priority, Image& out) const {
    const int width = out.width;
    std::vector<Part> list;
    parts(scene, priority, list);
    for (const Part& part : list) {
        const int px = part.x - cam_x, py = part.y - cam_y, size = part.size;
        if (px >= width || py >= kHeight || px + size <= 0 || py + size <= 0) continue;
        for (int y = 0; y < size; ++y) {
            const int sy = py + y;
            if (sy < 0 || sy >= kHeight) continue;
            for (int x = 0; x < size; ++x) {
                const int sx = px + x;
                if (sx < 0 || sx >= width) continue;
                const int v = part_pixel(part, x, y);
                if (v >= 0) out.pixels[static_cast<std::size_t>(sy) * width + sx] = colour(scene.palette, v);
            }
        }
    }
}

// The 32x28 text layer at ds:2D08h (sub_15AB3); 0x20 is an empty cell,
// otherwise the glyph number. With the normal font (sub_15AD2) a glyph is
// drawn whole by sub_146BA, all 64 pixels (palette entries 0-3). With the
// alternate font (ds:1CF6h bit 0x100, sub_15B2E) the cell is first redrawn
// from the map tiles alone, covering sprites, and the glyph goes on top
// where its mask is set.
void Renderer::draw_text(const Scene& scene, int x0, const Image* tiles, Image& out) const {
    const DataSegment& ds = (*scene.machine).ds;
    const bool alternate = ds.u16(0x1CF6) & 0x100;
    const auto& font = fonts_[alternate ? 1 : 0];
    for (int row = 0; row < 28; ++row) {
        for (int col = 0; col < 32; ++col) {
            const std::uint16_t v = ds.u16(static_cast<std::uint16_t>(0x2D08 + (row * 32 + col) * 2));
            if (v == 0x20 || v >= font.size()) continue;
            const Glyph& g = font[v];
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const std::size_t at = static_cast<std::size_t>(row * 8 + y) * out.width + x0 + col * 8 + x;
                    // Mask bytes follow the planar data: byte = plane*2 + (row >= 4),
                    // bit 7..0 = the 8 data bytes (4 rows x 2 columns) of that run.
                    const int plane = x & 3, data_col = x >> 2;
                    const int byte = plane * 2 + (y >> 2), bit = (y & 3) * 2 + data_col;
                    const bool set = g.mask[byte] & (0x80 >> bit);
                    if (!alternate || set) out.pixels[at] = colour(scene.palette, g.pixels[y * 8 + x]);
                    else if (tiles) out.pixels[at] = tiles->pixels[at];
                }
        }
    }
}

}  // namespace bt

namespace bt {

void Renderer::render_layer(const Scene& scene, bool background, int pass, Image& out) const {
    const DataSegment& ds = scene.machine->ds;
    const int w = background ? ds.u16(0x1D15) : ds.u16(Machine::kMapWidth);
    const int h = background ? ds.u16(0x1D17) : ds.u16(Machine::kMapHeight);
    out.resize(w * 16, h * 16);
    std::fill(out.pixels.begin(), out.pixels.end(), 0u);  // transparent
    for (int p : {0, 8}) {
        if (pass >= 0 && p != pass) continue;
        if (background)
            draw_layer(scene, ds.u16(0x25B2), ds.u16(0x1CA0), w, h, 0, 0, p, out);
        else
            draw_layer(scene, ds.u16(0x25B0), ds.u16(Machine::kMapRowBytes), w, h, 0, 0, p, out);
    }
}

// Index-encoding palette: entry i shows as red i >> 2, green i & 3 (6-bit
// values), which layer_index() turns back into i.
static const std::array<std::uint8_t, 768> kIndexPalette = [] {
    std::array<std::uint8_t, 768> p{};
    for (int i = 0; i < 256; ++i) {
        p[i * 3] = static_cast<std::uint8_t>(i >> 2);
        p[i * 3 + 1] = static_cast<std::uint8_t>(i & 3);
    }
    return p;
}();

std::uint32_t Renderer::index_pixel(int index) { return colour(kIndexPalette.data(), index); }

void Renderer::render_metatile(const Scene& scene, int metatile, bool front, Image& out) {
    const Machine& m = *scene.machine;
    out.resize(16, 16);
    std::fill(out.pixels.begin(), out.pixels.end(), 0u);
    const std::uint16_t mt = static_cast<std::uint16_t>(m.ds.u16(Machine::kMetatileTable) + (metatile & 0x3FF) * 8);
    for (int k = 0; k < 4; ++k) {
        const TileRef ref{m.map_u16(static_cast<std::uint16_t>(mt + 2 * k))};
        if (((ref.raw & 8) != 0) != front) continue;
        const std::size_t tile = static_cast<std::size_t>(ref.tile());
        const std::uint8_t* px = m.tiles.data() + tile * 64;
        const std::uint8_t* mask = m.tile_masks.data() + ((tile * 8) & 0x1FFF);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const int ty = ref.flip_y() ? 7 - y : y, tx = ref.flip_x() ? 7 - x : x;
                const int plane = tx & 3, col = tx >> 2;
                if (!(mask[plane * 2 + (ty >> 2)] & (0x80 >> ((ty & 3) * 2 + col)))) continue;
                const int v = px[plane * 16 + ty * 2 + col];
                out.pixels[static_cast<std::size_t>((k >> 1) * 8 + y) * 16 + (k & 1) * 8 + x] = index_pixel(v >= 16 ? v : v + ref.palette_row() * 16);
            }
    }
}

int Renderer::layer_index(std::uint32_t pixel) {
    if (!(pixel >> 24)) return -1;
    const int r = static_cast<int>(((pixel >> 16) & 0xFF) * 63 + 127) / 255, g = static_cast<int>(((pixel >> 8) & 0xFF) * 63 + 127) / 255;
    return (r << 2) | g;
}

void Renderer::render_layers(const Scene& scene, const RenderOptions& opt, Layers& out) const {
    const int width = kWidth + 2 * opt.extra;
    const DataSegment& ds = scene.machine->ds;
    auto s16 = [&](std::uint16_t a) { return static_cast<int>(static_cast<std::int16_t>(ds.u16(a))); };
    const int cam_x = s16(0x1B4E), cam_y = s16(0x1B50);
    const int map_px = ds.u16(Machine::kMapWidth) * 16;
    int view_x = cam_x - opt.extra;
    if (opt.extra) view_x = map_px >= width ? std::clamp(view_x, 0, map_px - width) : (map_px - width) / 2;
    out.view_x = view_x;
    out.cam_y = cam_y;
    out.shift = cam_x - view_x;
    out.bg_x = s16(0x1B52) - out.shift;
    out.bg_y = s16(0x1B54);
    const Scene indexed{scene.machine, scene.has_background, kIndexPalette.data()};
    auto clear = [&](Image& img) {
        img.resize(width, kHeight);
        std::fill(img.pixels.begin(), img.pixels.end(), 0u);
    };
    for (Image* img : {&out.background_back, &out.background_front, &out.foreground_back, &out.foreground_front, &out.text})
        clear(*img);
    if (opt.background && scene.has_background) {
        draw_layer(indexed, ds.u16(0x25B2), ds.u16(0x1CA0), ds.u16(0x1D15), ds.u16(0x1D17), out.bg_x, out.bg_y, 0, out.background_back);
        draw_layer(indexed, ds.u16(0x25B2), ds.u16(0x1CA0), ds.u16(0x1D15), ds.u16(0x1D17), out.bg_x, out.bg_y, 8, out.background_front);
    }
    if (opt.foreground) {
        draw_layer(indexed, ds.u16(0x25B0), ds.u16(Machine::kMapRowBytes), ds.u16(Machine::kMapWidth), ds.u16(Machine::kMapHeight),
                   view_x, cam_y, 0, out.foreground_back);
        draw_layer(indexed, ds.u16(0x25B0), ds.u16(Machine::kMapRowBytes), ds.u16(Machine::kMapWidth), ds.u16(Machine::kMapHeight),
                   view_x, cam_y, 8, out.foreground_front);
    }
    for (int priority = 0; priority < 4; ++priority) {
        clear(out.sprites[priority]);
        if (opt.sprites) draw_parts(scene, view_x, cam_y, priority, out.sprites[priority]);
    }
    if (opt.text) draw_text(scene, out.shift, nullptr, out.text);
}

Scene live_scene(const Game& game) {
    const Machine& m = game.machine();
    return {&m, game.has_background(), m.ds.raw() + 0x4D12};
}

void Renderer::render_screen(const Game& game, Image& out, const RenderOptions& opt) const {
    render_screen(game.display(), out, opt);
}

}  // namespace bt
