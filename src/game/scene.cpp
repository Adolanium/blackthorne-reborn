#include "game/scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>

#include "engine/machine.h"
#include "render/png.h"

namespace {

std::uint32_t pal(const std::uint8_t* palette, int i) {
    auto c = [&](int k) { return static_cast<std::uint32_t>((palette[i * 3 + k] & 0x3F) * 255 / 63); };
    return 0xFF000000u | (c(0) << 16) | (c(1) << 8) | c(2);
}

}  // namespace

bool match_pose(const bt::Scene& scene, int slot, PoseMatch& out) {
    const bt::Machine& m = *scene.machine;
    const bt::DataSegment& ds = m.ds;
    const int first = ds.u16(static_cast<std::uint16_t>(0x198A + slot)), count = ds.u16(static_cast<std::uint16_t>(0x19B2 + slot));
    if (count <= 0 || count > 16 || first > 0xFE) return false;
    out = PoseMatch{};
    out.slot = slot;
    int priority = -1;
    for (int i = 0, p = first; i < count && p <= 0xFE; ++i, p += 2) {
        const std::uint16_t attr = ds.field(0x452, p);
        if (!(attr & 0x80)) continue;
        bt::Renderer::Part part;
        part.size = ds.field(0x952, p);
        if (part.size != 16 && part.size != 32) continue;
        part.data = m.sprite_data(ds.field(0x852, p), ds.field(0x752, p));
        if (!part.data) continue;
        part.x = static_cast<std::int16_t>(ds.field(0x552, p));
        part.y = static_cast<std::int16_t>(ds.field(0x652, p));
        part.mirror = (attr & 0x20) != 0;
        part.base = static_cast<std::uint8_t>(ds.field(0xF52, p));
        const int pr = (attr & 0x340) >> 8;
        if (priority < 0 && pr <= 3) priority = pr;
        out.parts.push_back(part);
        out.offsets.push_back(p);
    }
    if (out.parts.empty() || priority < 0) return false;
    out.priority = priority;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (const bt::Renderer::Part& q : out.parts) {
        x0 = std::min(x0, q.x), y0 = std::min(y0, q.y);
        x1 = std::max(x1, q.x + q.size), y1 = std::max(y1, q.y + q.size);
    }
    if (x1 - x0 > 96 || y1 - y0 > 96) return false;
    int mirrored = 0;
    for (const bt::Renderer::Part& q : out.parts) mirrored += q.mirror;
    out.flip = mirrored * 2 > static_cast<int>(out.parts.size());
    out.x0 = x0, out.y0 = y0, out.x1 = x1, out.y1 = y1;
    for (const bt::Renderer::Part& q : out.parts) {
        const int px = out.flip ? x1 - (q.x + q.size) : q.x - x0, py = q.y - y0;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%016llx,%d,%d,%d,%d;", static_cast<unsigned long long>(bt::Renderer::part_key(q)), q.size, px, py,
                      q.mirror != out.flip);
        out.signature += buf;
    }
    std::uint64_t h = 1469598103934665603ull;
    for (char c : out.signature) h = (h ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof name, "pose_%016llx", static_cast<unsigned long long>(h));
    out.name = name;
    return true;
}

SceneRenderer::SceneRenderer(SDL_Renderer* sdl, const bt::Renderer& base, const bt::ArtPack& pack) : sdl_(sdl), base_(base), pack_(pack) {
    scale_ = pack.scale();
    std::ifstream in(pack.dir() / "poses" / "poses.json");
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::regex cell("\"cell\"\\s*:\\s*(\\d+)");
    std::smatch cm;
    if (std::regex_search(json, cm, cell)) cell_ = std::stoi(cm[1]);
    const std::regex entry(
        "\"(pose_[0-9a-f]+)\"\\s*:\\s*\\[\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*(?:,\\s*(\\d+)\\s*)?\\]");
    for (auto it = std::sregex_iterator(json.begin(), json.end(), entry); it != std::sregex_iterator(); ++it)
        pose_index_[(*it)[1]] = {std::stoi((*it)[2]), std::stoi((*it)[3]), std::stoi((*it)[4]), std::stoi((*it)[5]),
                                 (*it)[6].matched ? std::stoi((*it)[6]) : cell_};
}

SceneRenderer::~SceneRenderer() {
    free_level();
    if (overlay_tex_) SDL_DestroyTexture(overlay_tex_);
    for (auto* m : {&poses_}) for (auto& [k, t] : *m) if (t) SDL_DestroyTexture(t);
    for (auto& [k, t] : parts_) if (t) SDL_DestroyTexture(t);
    for (auto& [k, t] : glyphs_) if (t) SDL_DestroyTexture(t);
    for (auto& [k, t] : hud_) if (t) SDL_DestroyTexture(t);
}

void SceneRenderer::free_level() {
    for (SDL_Texture* t : {bg_, fg_, front_, occluder_}) if (t) SDL_DestroyTexture(t);
    bg_ = fg_ = front_ = occluder_ = nullptr;
    fg_img_ = nullptr;
    applied_.clear();
    where_.clear();
    level_ = -1;
}

SDL_Texture* SceneRenderer::texture(const bt::Image& img) {
    SDL_Texture* t = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, img.width, img.height);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, img.pixels.data(), img.width * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

SDL_Texture* SceneRenderer::pose_texture(const std::string& name) {
    auto it = poses_.find(name);
    if (it != poses_.end()) return it->second;
    bt::Image img;
    SDL_Texture* t = nullptr;
    if (pose_index_.count(name) && bt::read_png(pack_.dir() / "poses" / (name + ".png"), img)) t = texture(img);
    poses_[name] = t;
    return t;
}

SDL_Texture* SceneRenderer::part_texture(const bt::Scene& scene, const bt::Renderer::Part& part) {
    const std::uint64_t key = bt::Renderer::part_key(part);
    auto it = parts_.find(key);
    if (it != parts_.end()) return it->second;
    const bt::ArtPack::Sprite* s = pack_.sprite(key, part.size);
    if (!s && pack_.alias(key)) s = pack_.sprite(pack_.alias(key), part.size);
    SDL_Texture* t = s ? texture(s->image) : nullptr;
    if (!t) {
        // No painting: the original pixels, smoothly scaled, so a character
        // is never missing. Stored as the data is (drawing mirrors it).
        bt::Image img;
        img.resize(part.size, part.size);
        bt::Renderer::Part plain = part;
        plain.mirror = false;
        for (int y = 0; y < part.size; ++y)
            for (int x = 0; x < part.size; ++x) {
                const int v = bt::Renderer::part_pixel(plain, x, y);
                img.pixels[static_cast<std::size_t>(y) * part.size + x] = v < 0 ? 0u : pal(scene.machine->palette_at_load.data(), v);
            }
        t = texture(img);
    }
    parts_[key] = t;
    return t;
}

SDL_Texture* SceneRenderer::glyph_texture(int font, int index) {
    const int key = font * 4096 + index;
    auto it = glyphs_.find(key);
    if (it != glyphs_.end()) return it->second;
    SDL_Texture* t = nullptr;
    if (const bt::Image* g = pack_.glyph(font, index)) {
        bt::Image img = *g;
        const auto& glyphs = base_.font(font);
        if (font == 0 && !glyphs.empty()) {
            // The box background: dark parts of the painted glyph fade to its
            // flat colour, so the cells of a text box don't show.
            const std::uint32_t bg = pal(pack_.font_palette().data(), glyphs[0].pixels[0]);
            for (std::uint32_t& p : img.pixels) {
                const float lum = static_cast<float>(((p >> 16) & 0xFF) * 3 + ((p >> 8) & 0xFF) * 6 + (p & 0xFF)) / 10.0f;
                const float a = std::clamp((lum - 70.0f) / 90.0f, 0.0f, 1.0f);
                auto mix = [&](int sh) {
                    const float c0 = static_cast<float>((bg >> sh) & 0xFF), c1 = static_cast<float>((p >> sh) & 0xFF);
                    return static_cast<std::uint32_t>(c0 + (c1 - c0) * a + 0.5f) << sh;
                };
                p = 0xFF000000u | mix(16) | mix(8) | mix(0);
            }
        }
        t = texture(img);
    }
    glyphs_[key] = t;
    return t;
}

SDL_Texture* SceneRenderer::hud_texture(const std::string& name, int w, int h) {
    auto it = hud_.find(name);
    if (it != hud_.end()) return it->second;
    const bt::Image* img = pack_.hud(name, w, h);
    SDL_Texture* t = img ? texture(*img) : nullptr;
    hud_[name] = t;
    return t;
}

// The foreground painting where the original draws tiles of `pass` (8: in
// front of the characters, tile bit 3; -1: all), with a soft edge.
void SceneRenderer::build_masked(const bt::Scene& scene, const bt::Image& fg, int pass, bt::Image& out) {
    bt::Image front1;
    base_.render_layer(scene, false, pass, front1);
    const int w1 = front1.width, h1 = front1.height, S = scale_;
    std::vector<float> m(static_cast<std::size_t>(w1) * h1);
    for (std::size_t i = 0; i < m.size(); ++i) m[i] = (front1.pixels[i] >> 24) ? 1.0f : 0.0f;
    // A small blur at 1x, for a soft edge.
    std::vector<float> b(m.size());
    for (int y = 0; y < h1; ++y)
        for (int x = 0; x < w1; ++x) {
            float s = 0;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w1 || yy >= h1) continue;
                    s += m[static_cast<std::size_t>(yy) * w1 + xx], ++n;
                }
            b[static_cast<std::size_t>(y) * w1 + x] = s / static_cast<float>(n);
        }
    out.resize(fg.width, fg.height);
    for (int y = 0; y < fg.height; ++y) {
        const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(S) - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, h1 - 1), y1 = std::min(y0 + 1, h1 - 1);
        const float ty = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        for (int x = 0; x < fg.width; ++x) {
            const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(S) - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, w1 - 1), x1 = std::min(x0 + 1, w1 - 1);
            const float tx = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
            const float v = (b[static_cast<std::size_t>(y0) * w1 + x0] * (1 - tx) + b[static_cast<std::size_t>(y0) * w1 + x1] * tx) * (1 - ty) +
                            (b[static_cast<std::size_t>(y1) * w1 + x0] * (1 - tx) + b[static_cast<std::size_t>(y1) * w1 + x1] * tx) * ty;
            const float a = std::clamp((v - 0.25f) * 2.0f, 0.0f, 1.0f);
            const std::uint32_t p = fg.pixels[static_cast<std::size_t>(y) * fg.width + x];
            const std::uint32_t na = static_cast<std::uint32_t>(static_cast<float>(p >> 24) * a);
            out.pixels[static_cast<std::size_t>(y) * fg.width + x] = (na << 24) | (p & 0xFFFFFFu);
        }
    }
}

bool SceneRenderer::prepare(const bt::Scene& scene) {
    const bt::DataSegment& ds = scene.machine->ds;
    const int level = ds.u16(0x1CA4);
    const int fw = ds.u16(bt::Machine::kMapWidth), fh = ds.u16(bt::Machine::kMapHeight);
    if (level != level_) {
        free_level();
        const bt::Image* fg = pack_.layer(level, false, fw * 16, fh * 16);
        const bt::Image* bg = scene.has_background ? pack_.layer(level, true, ds.u16(0x1D15) * 16, ds.u16(0x1D17) * 16) : nullptr;
        if (!fg || (scene.has_background && !bg)) return false;  // still decoding
        fg_ = texture(*fg);
        if (bg) bg_ = texture(*bg);
        {
            bt::Image masked;
            build_masked(scene, *fg, 8, masked);
            front_ = texture(masked);
            // Characters the original draws behind the foreground (priority 0,
            // e.g. on ladders) are hidden only where its tiles were solid; the
            // painting may fill gaps the original left open.
            build_masked(scene, *fg, -1, masked);
            occluder_ = texture(masked);
        }
        if (!fg_ || !front_ || !occluder_) return false;
        fg_img_ = fg;
        // Where each metatile appears at load, and what the texture shows.
        const bt::Machine& m = *scene.machine;
        const std::uint16_t base = ds.u16(0x25B0);
        const int row = ds.u16(bt::Machine::kMapRowBytes);
        applied_.assign(static_cast<std::size_t>(fw) * fh, -1);
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                const std::size_t at = static_cast<std::size_t>((base + y * row + x * 2) & 0xFFFF);
                const int v = (m.map_at_load[at] | (m.map_at_load[(at + 1) & 0xFFFF] << 8)) & 0x3FF;
                applied_[static_cast<std::size_t>(y) * fw + x] = v;
                where_.emplace(v, std::make_pair(x, y));
            }
        level_ = level;
    }
    update_cells(scene);
    return true;
}

// Cells the game changed (doors, bridges, menu text): the painted metatile,
// or the painting of the same metatile elsewhere in the level, copied into
// the level texture; the "in front" layer is cleared there.
void SceneRenderer::update_cells(const bt::Scene& scene) {
    const bt::Machine& m = *scene.machine;
    const bt::DataSegment& ds = m.ds;
    const int fw = ds.u16(bt::Machine::kMapWidth), fh = ds.u16(bt::Machine::kMapHeight);
    const std::uint16_t base = ds.u16(0x25B0);
    const int row = ds.u16(bt::Machine::kMapRowBytes);
    const int S = scale_, cs = 16 * S;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(cs) * cs);
    for (int y = 0; y < fh; ++y)
        for (int x = 0; x < fw; ++x) {
            const int v = m.map_u16(static_cast<std::uint16_t>(base + y * row + x * 2)) & 0x3FF;
            int& shown = applied_[static_cast<std::size_t>(y) * fw + x];
            if (v == shown) continue;
            shown = v;
            const bt::Image* cell = pack_.cell(level_, v);
            auto w = where_.find(v);
            for (int yy = 0; yy < cs; ++yy)
                for (int xx = 0; xx < cs; ++xx) {
                    std::uint32_t c = 0;
                    if (cell) c = cell->pixels[static_cast<std::size_t>(yy) * cell->width + xx];
                    else if (w != where_.end())
                        c = fg_img_->pixels[static_cast<std::size_t>(w->second.second * cs + yy) * fg_img_->width + w->second.first * cs + xx];
                    px[static_cast<std::size_t>(yy) * cs + xx] = c;
                }
            const SDL_Rect r{x * cs, y * cs, cs, cs};
            SDL_UpdateTexture(fg_, &r, px.data(), cs * 4);
            SDL_UpdateTexture(occluder_, &r, px.data(), cs * 4);
            std::fill(px.begin(), px.end(), 0u);
            SDL_UpdateTexture(front_, &r, px.data(), cs * 4);
        }
}

SceneRenderer::Camera SceneRenderer::camera(const bt::Scene& scene, int extra) const {
    const bt::DataSegment& ds = scene.machine->ds;
    auto s16 = [&](std::uint16_t a) { return static_cast<int>(static_cast<std::int16_t>(ds.u16(a))); };
    const int W = bt::Renderer::kWidth + 2 * extra, screen_w = bt::Renderer::kScreenWidth + 2 * extra;
    Camera c;
    const int cam_x = s16(0x1B4E);
    c.cam_y = s16(0x1B50);
    const int map_px = ds.u16(bt::Machine::kMapWidth) * 16;
    c.view_x = cam_x - extra;
    if (extra) c.view_x = map_px >= W ? std::clamp(c.view_x, 0, map_px - W) : (map_px - W) / 2;
    c.shift = cam_x - c.view_x;
    c.bg_x = s16(0x1B52) - c.shift;
    c.bg_y = s16(0x1B54);
    c.play_x = (ds.u16(0x1CF6) & 1) ? 0 : (screen_w - W) / 2;
    return c;
}

void SceneRenderer::tick(const bt::Scene& scene, int extra) {
    prev_ = cur_;
    cur_ = Snap{};
    cur_.valid = true;
    cur_.level = scene.machine->ds.u16(0x1CA4);
    cur_.cam = camera(scene, extra);
    for (int slot = 0; slot < 0x28; slot += 2) {
        PoseMatch pm;
        if (match_pose(scene, slot, pm)) cur_.actors[slot] = {pm.x0, pm.y0};
    }
}

void SceneRenderer::analyse(const bt::Scene& scene, int extra) {
    const bt::Machine& m = *scene.machine;
    const bt::DataSegment& ds = m.ds;
    bt::RenderOptions opt;
    opt.extra = extra;
    base_.render_layers(scene, opt, layers_);
    const Camera c = camera(scene, extra);
    const int W = layers_.foreground_back.width, H = bt::Renderer::kHeight;
    const int screen_w = bt::Renderer::kScreenWidth + 2 * extra;
    play_x_ = c.play_x;
    play_w_ = W;
    // Colours in palette cycles: water, fire, lava.
    std::array<bool, 256> cycled{};
    for (int si = 0; si < 0x10; si += 2) {
        if (!(ds.u16(0x1B86) & ds.u16(static_cast<std::uint16_t>(0x288A + si)))) continue;
        const int a = ds.u16(static_cast<std::uint16_t>(0x1BB8 + si)) & 0xFF, b = ds.u16(static_cast<std::uint16_t>(0x1BC8 + si)) & 0xFF;
        for (int i = std::min(a, b); i <= std::max(a, b); ++i) cycled[static_cast<std::size_t>(i)] = true;
    }
    auto bright = [](std::uint32_t p) {
        return (0.30f * static_cast<float>((p >> 16) & 0xFF) + 0.59f * static_cast<float>((p >> 8) & 0xFF) + 0.11f * static_cast<float>(p & 0xFF)) / 255.0f;
    };
    auto warm = [](std::uint32_t p) {
        const int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        return r > 110 && r > b + 40 && g < r + 10;
    };
    emission_.resize(screen_w, bt::Renderer::kScreenHeight);
    std::fill(emission_.pixels.begin(), emission_.pixels.end(), 0u);
    ui_.resize(screen_w, bt::Renderer::kScreenHeight);
    std::fill(ui_.pixels.begin(), ui_.pixels.end(), 0u);
    overlay_.resize(W, H);
    std::fill(overlay_.pixels.begin(), overlay_.pixels.end(), 0u);
    auto emit = [&](int x, int y, std::uint32_t col, float strength) {
        const std::uint32_t a = static_cast<std::uint32_t>(std::clamp(strength, 0.0f, 1.0f) * 255.0f);
        emission_.pixels[static_cast<std::size_t>(y) * emission_.width + play_x_ + x] = (a << 24) | (col & 0xFFFFFFu);
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * W + x;
            bool sprite_front = false;
            for (int k = 1; k < 4; ++k) sprite_front = sprite_front || (layers_.sprites[k].pixels[i] >> 24);
            // The map colour on top here.
            int v = -1;
            for (const bt::Image* l : {&layers_.foreground_front, &layers_.foreground_back, &layers_.background_front, &layers_.background_back}) {
                v = bt::Renderer::layer_index(l->pixels[i]);
                if (v >= 0) break;
            }
            if (v >= 0 && cycled[static_cast<std::size_t>(v)]) {
                const std::uint32_t col = pal(scene.palette, v);
                if (warm(col)) emit(x, y, col, bright(col) * 1.4f);
                if (!sprite_front) overlay_.pixels[i] = (static_cast<std::uint32_t>(warm(col) ? 120 : 105) << 24) | (col & 0xFFFFFFu);
            }
            // Sprites light the scene only when near white: muzzle flashes,
            // explosions, magic (skin and clothes don't).
            for (int k = 0; k < 4; ++k) {
                const std::uint32_t p = layers_.sprites[k].pixels[i];
                if (!(p >> 24)) continue;
                const float l = bright(p);
                const int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                if (l > 0.9f && std::min({r, g, b}) > 190) emit(x, y, p, l);
            }
        }
    // Text cells and whole dialogue boxes stay unlit.
    for (int row = 0; row < 28; ++row) {
        int first = -1, last = -1;
        bool frame = false;
        for (int col = 0; col < 32; ++col) {
            const std::uint16_t v = ds.u16(static_cast<std::uint16_t>(0x2D08 + (row * 32 + col) * 2));
            if (v == 0x20) continue;
            if (first < 0) first = col;
            last = col;
            frame = frame || (v >= 1 && v <= 8);
            for (int y = row * 8; y < row * 8 + 8; ++y)
                for (int x = col * 8; x < col * 8 + 8; ++x) {
                    const int sx = play_x_ + c.shift + x;
                    if (sx >= 0 && sx < ui_.width) ui_.pixels[static_cast<std::size_t>(y) * ui_.width + sx] = 0xFFFFFFFFu;
                }
        }
        if (!frame) continue;
        for (int y = row * 8; y < row * 8 + 8; ++y)
            for (int x = first * 8; x < last * 8 + 8; ++x) {
                const int sx = play_x_ + c.shift + x;
                if (sx >= 0 && sx < ui_.width) ui_.pixels[static_cast<std::size_t>(y) * ui_.width + sx] = 0xFFFFFFFFu;
            }
    }
    for (std::size_t i = 0; i < ui_.pixels.size(); ++i)
        if (ui_.pixels[i]) emission_.pixels[i] = 0u;
    // The animated layer as a texture (smoothly scaled).
    float tw = 0, th = 0;
    if (overlay_tex_) SDL_GetTextureSize(overlay_tex_, &tw, &th);
    if (!overlay_tex_ || static_cast<int>(tw) != W || static_cast<int>(th) != H) {
        if (overlay_tex_) SDL_DestroyTexture(overlay_tex_);
        overlay_tex_ = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
        if (overlay_tex_) {
            SDL_SetTextureBlendMode(overlay_tex_, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(overlay_tex_, SDL_SCALEMODE_LINEAR);
        }
    }
    // Soften it, more along the fall of water than across, so the cycling
    // reads as flowing streaks and flicker rather than a pixel pattern.
    {
        std::vector<float> acc(static_cast<std::size_t>(W) * H * 4, 0.0f), tmp(acc.size());
        for (std::size_t i = 0; i < overlay_.pixels.size(); ++i) {
            const std::uint32_t p = overlay_.pixels[i];
            const float a = static_cast<float>(p >> 24) / 255.0f;
            acc[i * 4] = static_cast<float>((p >> 16) & 0xFF) * a;
            acc[i * 4 + 1] = static_cast<float>((p >> 8) & 0xFF) * a;
            acc[i * 4 + 2] = static_cast<float>(p & 0xFF) * a;
            acc[i * 4 + 3] = a;
        }
        auto pass = [&](int rx, int ry) {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float sum[4] = {};
                    int n = 0;
                    for (int dy = -ry; dy <= ry; ++dy)
                        for (int dx = -rx; dx <= rx; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                            for (int k = 0; k < 4; ++k) sum[k] += acc[(static_cast<std::size_t>(yy) * W + xx) * 4 + static_cast<std::size_t>(k)];
                            ++n;
                        }
                    for (int k = 0; k < 4; ++k) tmp[(static_cast<std::size_t>(y) * W + x) * 4 + static_cast<std::size_t>(k)] = sum[k] / static_cast<float>(n);
                }
            acc.swap(tmp);
        };
        pass(1, 2);
        for (std::size_t i = 0; i < overlay_.pixels.size(); ++i) {
            const float a = acc[i * 4 + 3];
            if (a <= 0.0f) {
                overlay_.pixels[i] = 0u;
                continue;
            }
            auto ch = [&](int k) { return static_cast<std::uint32_t>(std::clamp(acc[i * 4 + static_cast<std::size_t>(k)] / a, 0.0f, 255.0f)); };
            overlay_.pixels[i] = (static_cast<std::uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24) | (ch(0) << 16) | (ch(1) << 8) | ch(2);
        }
    }
    if (overlay_tex_) SDL_UpdateTexture(overlay_tex_, nullptr, overlay_.pixels.data(), W * 4);
    // The camera as draw() will place it (smooth motion).
    view_xf_ = static_cast<float>(c.view_x);
    view_yf_ = static_cast<float>(c.cam_y);
    if (prev_.valid && cur_.valid && prev_.level == cur_.level) {
        const float dx = static_cast<float>(prev_.cam.view_x - cur_.cam.view_x), dy = static_cast<float>(prev_.cam.cam_y - cur_.cam.cam_y);
        if (std::abs(dx) < 48 && std::abs(dy) < 48) {
            view_xf_ += dx * (1.0f - interp);
            view_yf_ += dy * (1.0f - interp);
        }
    }
}

void SceneRenderer::draw_tex(SDL_Texture* t, float x, float y, float w, float h, bool flip) {
    if (!t) return;
    SDL_SetTextureColorMod(t, mod_[0], mod_[1], mod_[2]);
    const SDL_FRect r{ox_ + x * k_, oy_ + y * k_, w * k_, h * k_};
    SDL_RenderTextureRotated(sdl_, t, nullptr, &r, 0.0, nullptr, flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
}

void SceneRenderer::draw(const bt::Scene& scene, const SDL_FRect& dst, int extra) {
    const bt::Machine& m = *scene.machine;
    const bt::DataSegment& ds = m.ds;
    auto s16 = [&](std::uint16_t a) { return static_cast<int>(static_cast<std::int16_t>(ds.u16(a))); };
    const int W = bt::Renderer::kWidth + 2 * extra, H = bt::Renderer::kHeight;
    const int screen_w = bt::Renderer::kScreenWidth + 2 * extra;
    const bool hud = ds.u16(0x1CF6) & 1;
    const Camera cm = camera(scene, extra);
    const int shift = cm.shift, play_x = cm.play_x;
    // Smooth motion: the camera between the last two ticks (analyse()).
    const float view_x = view_xf_, cam_y = view_yf_;
    const float cdx = view_x - static_cast<float>(cm.view_x), cdy = cam_y - static_cast<float>(cm.cam_y);
    const float bg_x = static_cast<float>(cm.bg_x) + cdx, bg_y = static_cast<float>(cm.bg_y) + cdy;
    const int shake_x = s16(0x344), shake_y = s16(0x346);
    k_ = dst.w / static_cast<float>(screen_w);
    ox_ = dst.x - static_cast<float>(shake_x) * k_;
    oy_ = dst.y - static_cast<float>(shake_y) * k_;
    // Fades and tints: the shown palette's brightness against the level's.
    float f[3] = {1, 1, 1};
    for (int c = 0; c < 3; ++c) {
        int shown = 0, base = 0;
        for (int i = 0; i < 256; ++i) shown += scene.palette[i * 3 + c] & 0x3F, base += m.palette_at_load[static_cast<std::size_t>(i * 3 + c)] & 0x3F;
        f[c] = base ? std::min(1.0f, static_cast<float>(shown) / static_cast<float>(base)) : 1.0f;
    }
    for (int c = 0; c < 3; ++c) mod_[c] = static_cast<Uint8>(std::clamp(f[c], 0.0f, 1.0f) * 255.0f + 0.5f);
    const float S = static_cast<float>(scale_);

    // The playfield, clipped to its place on the screen.
    const SDL_Rect clip{static_cast<int>(ox_ + static_cast<float>(play_x) * k_), static_cast<int>(oy_), static_cast<int>(static_cast<float>(W) * k_ + 0.5f),
                        static_cast<int>(static_cast<float>(H) * k_ + 0.5f)};
    SDL_SetRenderClipRect(sdl_, &clip);
    auto world = [&](float wx, float wy) { return std::make_pair(static_cast<float>(play_x) + wx - view_x, wy - cam_y); };
    float tw = 0, th = 0;
    if (bg_) {
        SDL_GetTextureSize(bg_, &tw, &th);
        draw_tex(bg_, static_cast<float>(play_x) - bg_x, -bg_y, tw / S, th / S);
    }
    // Actors as whole poses where the pack has them; other parts one by one.
    std::vector<PoseMatch> poses;
    std::set<int> covered;
    for (int slot = 0; slot < 0x28; slot += 2) {
        PoseMatch pm;
        if (!match_pose(scene, slot, pm) || !pose_texture(pm.name)) continue;
        for (int o : pm.offsets) covered.insert(o);
        poses.push_back(std::move(pm));
    }
    std::vector<bt::Renderer::Part> loose;
    auto draw_pose = [&](const PoseMatch& pm) {
            const auto& idx = pose_index_[pm.name];
            const float cell = static_cast<float>(idx[4]);
            float left = pm.flip ? static_cast<float>(pm.x1 + idx[0]) - cell : static_cast<float>(pm.x0 - idx[0]);
            float top = static_cast<float>(pm.y0 - idx[1]);
            // Smooth motion: from where this actor was on the previous tick.
            auto pa = prev_.actors.find(pm.slot);
            auto ca = cur_.actors.find(pm.slot);
            if (prev_.valid && pa != prev_.actors.end() && ca != cur_.actors.end() && ca->second == std::make_pair(pm.x0, pm.y0)) {
                const float dx = static_cast<float>(pa->second.first - pm.x0), dy = static_cast<float>(pa->second.second - pm.y0);
                if (std::abs(dx) < 24 && std::abs(dy) < 24) left += dx * (1.0f - interp), top += dy * (1.0f - interp);
            }
            const auto [sx, sy] = world(left, top);
            draw_tex(pose_texture(pm.name), sx, sy, cell, cell, pm.flip);
    };
    auto actors = [&](int priority) {
        // In the original's drawing order (later on top): a pose where its
        // first part comes, other parts one by one. Slot order would put
        // e.g. an enemy over Kyle on a ladder where the game shows Kyle.
        bt::Renderer::parts(scene, priority, loose);
        std::vector<bool> drawn(poses.size(), false);
        for (const bt::Renderer::Part& part : loose) {
            std::size_t owner = poses.size();
            for (std::size_t i = 0; i < poses.size() && owner == poses.size(); ++i)
                for (const bt::Renderer::Part& q : poses[i].parts)
                    if (q.data == part.data && q.x == part.x && q.y == part.y) {
                        owner = i;
                        break;
                    }
            if (owner < poses.size()) {
                if (!drawn[owner] && poses[owner].priority == priority) draw_pose(poses[owner]), drawn[owner] = true;
                continue;
            }
            const auto [sx, sy] = world(static_cast<float>(part.x), static_cast<float>(part.y));
            draw_tex(part_texture(scene, part), sx, sy, static_cast<float>(part.size), static_cast<float>(part.size), part.mirror);
        }
        for (std::size_t i = 0; i < poses.size(); ++i)
            if (!drawn[i] && poses[i].priority == priority) draw_pose(poses[i]);
    };
    // The foreground painting, then characters behind it, then the
    // foreground again where the original's tiles were solid.
    if (fg_) {
        SDL_GetTextureSize(fg_, &tw, &th);
        const auto [sx, sy] = world(0, 0);
        draw_tex(fg_, sx, sy, tw / S, th / S);
    }
    actors(0);
    if (occluder_) {
        SDL_GetTextureSize(occluder_, &tw, &th);
        const auto [sx, sy] = world(0, 0);
        draw_tex(occluder_, sx, sy, tw / S, th / S);
    }
    // The animated fire and water: the game's colour cycles over the painting.
    if (overlay_tex_) {
        const auto [sx, sy] = world(static_cast<float>(cm.view_x), static_cast<float>(cm.cam_y));
        draw_tex(overlay_tex_, sx, sy, static_cast<float>(W), static_cast<float>(H));
    }
    actors(1);
    actors(2);
    if (front_) {
        SDL_GetTextureSize(front_, &tw, &th);
        const auto [sx, sy] = world(0, 0);
        draw_tex(front_, sx, sy, tw / S, th / S);
    }
    actors(3);
    // Text: painted glyphs on the 32x28 grid of the game screen.
    {
        const int which = (ds.u16(0x1CF6) & 0x100) ? 1 : 0;
        const auto& font = base_.font(which);
        for (int cell = 0; cell < 32 * 28; ++cell) {
            const std::uint16_t v = ds.u16(static_cast<std::uint16_t>(0x2D08 + cell * 2));
            if (v == 0x20 || v >= font.size()) continue;
            const float x = static_cast<float>(play_x + shift + (cell % 32) * 8), y = static_cast<float>((cell / 32) * 8);
            if (SDL_Texture* t = glyph_texture(which, v)) {
                draw_tex(t, x, y, 8, 8);
            } else if (which == 0) {  // a blank cell: its flat colour
                const std::uint32_t c = pal(scene.palette, font[v].pixels[0]);
                SDL_SetRenderDrawColor(sdl_, static_cast<Uint8>(c >> 16), static_cast<Uint8>(c >> 8), static_cast<Uint8>(c), 255);
                const SDL_FRect r{ox_ + x * k_, oy_ + y * k_, 8 * k_, 8 * k_};
                SDL_RenderFillRect(sdl_, &r);
            }
        }
    }
    // Widescreen: the columns beside the game screen, dimmed.
    if (extra) {
        SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 97);
        for (float x : {static_cast<float>(play_x), static_cast<float>(play_x + shift + bt::Renderer::kWidth)}) {
            const float w = x == static_cast<float>(play_x) ? static_cast<float>(shift) : static_cast<float>(W - shift - bt::Renderer::kWidth);
            const SDL_FRect r{ox_ + x * k_, oy_, w * k_, static_cast<float>(H) * k_};
            SDL_RenderFillRect(sdl_, &r);
        }
    }
    SDL_SetRenderClipRect(sdl_, nullptr);

    // The HUD: painted panels, icons and the item name.
    if (hud) {
        const int stretch = screen_w - bt::Renderer::kScreenWidth;
        const bt::Renderer::Panel& side = base_.side_panel();
        const bt::Renderer::Panel& bottom = base_.bottom_bar();
        draw_tex(hud_texture("side", side.width, side.height), static_cast<float>(bt::Renderer::kWidth + stretch), 0,
                 static_cast<float>(side.width), static_cast<float>(side.height));
        draw_tex(hud_texture("bottom", bottom.width, bottom.height), 0, static_cast<float>(H), static_cast<float>(bottom.width + stretch),
                 static_cast<float>(bottom.height));
        const auto& icons = base_.item_icons();
        const int selected = ds.u16(0x1CB0);
        for (int di = 0; di < 0x10; di += 2) {
            const float x = static_cast<float>(0x110 + stretch), y = static_cast<float>(0xF * di);
            draw_tex(hud_texture("icon_00", 32, 32), x, y, 32, 32);
            const int item = ds.u16(static_cast<std::uint16_t>(0x1CB2 + di));
            if (item > 0 && item < static_cast<int>(icons.size())) {
                char name[16];
                std::snprintf(name, sizeof name, "icon_%02x", item);
                draw_tex(hud_texture(name, 32, 32), x, y, 32, 32);
            }
            if (di == selected) draw_tex(hud_texture("selection", 32, 32), x, y, 32, 32);
        }
        const int item = ds.u16(static_cast<std::uint16_t>(0x1CB2 + selected));
        const std::uint16_t names = ds.u16(static_cast<std::uint16_t>(0x579E + ds.u16(0x6A) * 2));
        const int cells = 16 + stretch / 8, pad = stretch / 16;
        for (int c = 0; c < cells; ++c) {
            const int i = c - pad;
            const std::uint8_t ch = (i >= 0 && i < 16) ? ds.u8(static_cast<std::uint16_t>(names + item * 16 + i)) : ' ';
            draw_tex(glyph_texture(0, ch - 0x20), static_cast<float>(64 + c * 8), static_cast<float>(H + 4), 8, 8);
        }
    }
}
