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
    out = {};
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
    for (auto* m : {&poses_}) for (auto& [k, t] : *m) if (t) SDL_DestroyTexture(t);
    for (auto& [k, t] : parts_) if (t) SDL_DestroyTexture(t);
    for (auto& [k, t] : glyphs_) if (t) SDL_DestroyTexture(t);
    for (auto& [k, t] : hud_) if (t) SDL_DestroyTexture(t);
}

void SceneRenderer::free_level() {
    for (SDL_Texture* t : {bg_, fg_, front_}) if (t) SDL_DestroyTexture(t);
    bg_ = fg_ = front_ = nullptr;
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

SDL_Texture* SceneRenderer::part_texture(const bt::Renderer::Part& part) {
    const std::uint64_t key = bt::Renderer::part_key(part);
    auto it = parts_.find(key);
    if (it != parts_.end()) return it->second;
    const bt::ArtPack::Sprite* s = pack_.sprite(key, part.size);
    if (!s && pack_.alias(key)) s = pack_.sprite(pack_.alias(key), part.size);
    SDL_Texture* t = s ? texture(s->image) : nullptr;
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

// The "in front" layer: the foreground painting where the original draws
// tiles in front of the characters (tile bit 3), with a soft edge.
void SceneRenderer::build_front(const bt::Scene& scene, const bt::Image& fg) {
    bt::Image front1;
    base_.render_layer(scene, false, 8, front1);
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
    front_img_.resize(fg.width, fg.height);
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
            front_img_.pixels[static_cast<std::size_t>(y) * fg.width + x] = (na << 24) | (p & 0xFFFFFFu);
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
        build_front(scene, *fg);
        front_ = texture(front_img_);
        if (!fg_ || !front_) return false;
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
            std::fill(px.begin(), px.end(), 0u);
            SDL_UpdateTexture(front_, &r, px.data(), cs * 4);
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
    // The camera, as Renderer::render computes it.
    const int cam_x = s16(0x1B4E), cam_y = s16(0x1B50);
    const int map_px = ds.u16(bt::Machine::kMapWidth) * 16;
    int view_x = cam_x - extra;
    if (extra) view_x = map_px >= W ? std::clamp(view_x, 0, map_px - W) : (map_px - W) / 2;
    const int shift = cam_x - view_x;
    const int bg_x = s16(0x1B52) - shift, bg_y = s16(0x1B54);
    const int play_x = hud ? 0 : (screen_w - W) / 2;
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
    auto world = [&](float wx, float wy) { return std::make_pair(static_cast<float>(play_x) + wx - static_cast<float>(view_x), wy - static_cast<float>(cam_y)); };
    float tw = 0, th = 0;
    if (bg_) {
        SDL_GetTextureSize(bg_, &tw, &th);
        draw_tex(bg_, static_cast<float>(play_x - bg_x), static_cast<float>(-bg_y), tw / S, th / S);
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
    auto actors = [&](int priority) {
        for (const PoseMatch& pm : poses) {
            if (pm.priority != priority) continue;
            const auto& idx = pose_index_[pm.name];
            const float cell = static_cast<float>(idx[4]);
            const float left = pm.flip ? static_cast<float>(pm.x1 + idx[0]) - cell : static_cast<float>(pm.x0 - idx[0]);
            const auto [sx, sy] = world(left, static_cast<float>(pm.y0 - idx[1]));
            draw_tex(pose_texture(pm.name), sx, sy, cell, cell, pm.flip);
        }
        bt::Renderer::parts(scene, priority, loose);  // in drawing order, like the original
        for (const bt::Renderer::Part& part : loose) {
            // Renderer::parts lists them in drawing order; skip the posed ones.
            bool posed = false;
            for (const PoseMatch& pm : poses)
                for (const bt::Renderer::Part& q : pm.parts)
                    if (q.data == part.data && q.x == part.x && q.y == part.y) posed = true;
            if (posed) continue;
            const auto [sx, sy] = world(static_cast<float>(part.x), static_cast<float>(part.y));
            draw_tex(part_texture(part), sx, sy, static_cast<float>(part.size), static_cast<float>(part.size), part.mirror);
        }
    };
    actors(0);
    if (fg_) {
        SDL_GetTextureSize(fg_, &tw, &th);
        const auto [sx, sy] = world(0, 0);
        draw_tex(fg_, sx, sy, tw / S, th / S);
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
