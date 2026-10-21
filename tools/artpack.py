"""Tools for making art packs (docs/VISUALS.md): repainted level layers.

An image model can't take a whole level layer (C0's foreground is 1792x224,
7168x896 at 4x), and it won't return exact sizes or transparency. So:

  split    cut each exported layer into tiles an image model can take: the
           1x pixels upscaled 4x (nearest), flattened on a key colour, with
           an overlap between neighbours, plus a manifest
  stitch   put the repainted tiles back together into a pack: each tile is
           resized to its exact 4x size, overlaps are cross-faded, and the
           transparency is set from the original layer, so the painting
           covers exactly what the original covered. Tiles without a
           repaint keep the original pixels.
  nearest  a pack of the exports upscaled 4x with nearest neighbour: the
           round-trip check for HdRenderer
  check    check a pack against the export: sizes, coverage
  collect  play every level headless with random input and save the sprite
           parts shown to <export>/sprites (blackthorne --parts)
  split-sprites / stitch-sprites
           the same for sprite parts: sheets of parts of one size for the
           model, cut back into the pack's sprites/<key>.png with the
           part's palette (<key>.pal)

Exports, work folders and packs come from the original art: keep them in
git-ignored folders (out/, packs/).

  python tools/artpack.py split   out/export out/work [--levels C0,DB] [--tile 256x224] [--overlap 32]
  python tools/artpack.py stitch  out/export out/work packs/<name>
  python tools/artpack.py nearest out/export packs/<name> [--levels ...]
  python tools/artpack.py check   out/export packs/<name>
  python tools/artpack.py collect <blackthorne.exe> <DATA.DAT> out/export [--runs 3] [--frames 3000]
  python tools/artpack.py split-sprites  out/export out/work
  python tools/artpack.py stitch-sprites out/export out/work packs/<name>

Work folder layout (one folder per level and layer):
  <work>/<ID>/<fg|bg>/in/<x>_<y>.png   the model's input (4x, key colour where transparent)
  <work>/<ID>/<fg|bg>/out/<x>_<y>.png  the repaint goes here (any size; resized on stitch)
  <work>/<ID>/<fg|bg>/manifest.json    tile rectangles at 1x
  <work>/<ID>/preview.png              the level, for context
"""

import argparse
import json
import os
import random
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

SCALE = 4
KEY = (255, 0, 255)  # the key colour behind transparent pixels in model inputs
LAYERS = ("fg", "bg")


def level_ids(export, wanted):
    ids = sorted(os.listdir(os.path.join(export, "levels")), key=lambda s: int(s, 16))
    if wanted:
        want = {w.strip().upper() for w in wanted.split(",")}
        ids = [i for i in ids if i in want]
    return ids


def layer_path(root, level, layer):
    return os.path.join(root, "levels", level, layer + ".png")


def write_pack_json(pack, smooth_text=True):
    """smooth_text rounds off the original font, and everything still drawn
    from the original pixels; the round-trip pack keeps it off so it stays
    pixel-exact."""
    os.makedirs(pack, exist_ok=True)
    with open(os.path.join(pack, "pack.json"), "w") as f:
        json.dump({"scale": SCALE, "smooth_text": smooth_text, "smooth_fallback": smooth_text}, f)
        f.write("\n")


def tiles_for(width, height, tw, th, overlap):
    """Tile rectangles (x, y, w, h) at 1x covering the layer, neighbours
    overlapping by `overlap` pixels."""
    def starts(total, size):
        if total <= size:
            return [0]
        step = size - overlap
        out = list(range(0, total - size, step))
        out.append(total - size)
        return out
    return [(x, y, min(tw, width), min(th, height)) for y in starts(height, th) for x in starts(width, tw)]


def cmd_split(args):
    tw, th = (int(v) for v in args.tile.lower().split("x"))
    count = 0
    for level in level_ids(args.export, args.levels):
        for layer in LAYERS:
            src = layer_path(args.export, level, layer)
            if not os.path.exists(src):
                continue
            img = Image.open(src).convert("RGBA")
            # Fully transparent tiles need no repaint.
            rects = [r for r in tiles_for(img.width, img.height, tw, th, args.overlap)
                     if img.crop((r[0], r[1], r[0] + r[2], r[1] + r[3])).getchannel("A").getbbox()]
            base = os.path.join(args.work, level, layer)
            os.makedirs(os.path.join(base, "in"), exist_ok=True)
            os.makedirs(os.path.join(base, "out"), exist_ok=True)
            for x, y, w, h in rects:
                tile = img.crop((x, y, x + w, y + h)).resize((w * SCALE, h * SCALE), Image.NEAREST)
                flat = Image.new("RGB", tile.size, KEY)
                flat.paste(tile, mask=tile.getchannel("A"))
                flat.save(os.path.join(base, "in", f"{x}_{y}.png"))
                count += 1
            with open(os.path.join(base, "manifest.json"), "w") as f:
                json.dump({"level": level, "layer": layer, "width": img.width, "height": img.height, "scale": SCALE,
                           "key": "#%02x%02x%02x" % KEY,
                           "tiles": [{"name": f"{x}_{y}.png", "x": x, "y": y, "w": w, "h": h} for x, y, w, h in rects]},
                          f, indent=1)
        preview = os.path.join(args.export, "levels", level, "preview.png")
        if os.path.exists(preview):
            Image.open(preview).save(os.path.join(args.work, level, "preview.png"))
    print(f"{count} tiles in {args.work}")


def feather(w, h, rect, layer_w, layer_h, overlap):
    """Blend weights for a tile at 4x: 1 inside, ramping down to 0 across the
    overlap on sides that have a neighbour."""
    x, y, tw, th = rect
    ramp = max(1, overlap * SCALE)
    left = x > 0
    top = y > 0
    right = x + tw < layer_w
    bottom = y + th < layer_h
    col = []
    for i in range(w):
        v = 1.0
        if left:
            v = min(v, (i + 0.5) / ramp)
        if right:
            v = min(v, (w - i - 0.5) / ramp)
        col.append(max(0.0, min(1.0, v)))
    row = []
    for j in range(h):
        v = 1.0
        if top:
            v = min(v, (j + 0.5) / ramp)
        if bottom:
            v = min(v, (h - j - 0.5) / ramp)
        row.append(max(0.0, min(1.0, v)))
    return col, row


def blur(a, radius):
    """Box blur (twice, close to Gaussian) of a 2D array."""
    for _ in range(2):
        for axis in (0, 1):
            c = np.cumsum(np.pad(a, [(radius + 1, radius) if i == axis else (0, 0) for i in range(2)], mode="edge"), axis=axis)
            a = (np.take(c, range(2 * radius + 1, c.shape[axis]), axis=axis) - np.take(c, range(0, c.shape[axis] - 2 * radius - 1), axis=axis)) / (2 * radius + 1)
    return a


def show_wall(out, original, level_dir, reach=0, tolerance=140.0):
    """Fires on the wall (braziers, torches): their foreground cells carry
    their own copy of the wall around them, close to the background's
    colours. In pixel art that blends in; repainted, the two walls differ
    and the cell shows as a square. Around fire (cells with flame pixels
    and their neighbours), foreground pixels close to the background's
    colour fade out by how close they are, and the background painting
    shows. Same-scroll layers only."""
    try:
        with open(os.path.join(level_dir, "level.json")) as f:
            info = json.load(f)
    except OSError:
        return
    bg_path = os.path.join(level_dir, "bg.png")
    if not info.get("background") or info.get("parallax") != [256, 256] or not os.path.exists(bg_path):
        return
    bg = Image.open(bg_path).convert("RGBA")
    if bg.size != original.size:
        return
    f = np.asarray(original, dtype=np.float64)
    b = np.asarray(bg, dtype=np.float64)
    h1, w1 = f.shape[:2]
    # Flame colours: orange to yellow (red cloth and blood have little green).
    warm = (f[:, :, 0] > 170) & (f[:, :, 1] > 80) & (f[:, :, 2] < 90) & (f[:, :, 3] > 0)
    cells = np.zeros(((h1 + 15) // 16, (w1 + 15) // 16), dtype=bool)
    ys, xs = np.nonzero(warm)
    cells[ys // 16, xs // 16] = True
    if not cells.any():
        return
    grown = cells.copy()
    for dy in range(-reach, reach + 1):
        for dx in range(-reach, reach + 1):
            grown |= np.roll(np.roll(cells, dy, 0), dx, 1)
    area = np.kron(grown, np.ones((16, 16), dtype=bool))[:h1, :w1]
    dist = np.abs(f[:, :, :3] - b[:, :, :3]).sum(axis=2)
    w = np.clip(1.0 - dist / tolerance, 0.0, 1.0) * area * (f[:, :, 3] > 0) * (b[:, :, 3] > 0) * ~warm
    if not w.any():
        return
    S = out.shape[0] // h1
    m = np.asarray(Image.fromarray(np.rint(w * 255).astype(np.uint8)).resize((w1 * S, h1 * S), Image.BILINEAR), dtype=np.float64) / 255.0
    out[:, :, 3] *= 1.0 - m


def fill_gaps(out, original, reach=24):
    """Empty foreground pixels enclosed by foreground on all four sides
    within `reach` game pixels (gaps between dark interior tiles) are filled
    from the surrounding painting, so they don't show as black blocks. Open
    edges keep their outline."""
    a1 = np.asarray(original.getchannel("A")) > 0
    h1, w1 = a1.shape
    def near(arr, axis, reverse):
        # distance to the nearest opaque pixel in one direction (capped)
        d = np.full(arr.shape, reach + 1, dtype=np.int32)
        run = np.full(arr.shape[1 - axis], reach + 1, dtype=np.int32)
        idx = range(arr.shape[axis] - 1, -1, -1) if reverse else range(arr.shape[axis])
        for i in idx:
            line = arr[i, :] if axis == 0 else arr[:, i]
            run = np.where(line, 0, np.minimum(run + 1, reach + 1))
            if axis == 0:
                d[i, :] = run
            else:
                d[:, i] = run
        return d
    enclosed = ~a1
    for axis in (0, 1):
        for rev in (False, True):
            enclosed &= near(a1, axis, rev) <= reach
    if not enclosed.any():
        return
    S = out.shape[0] // h1
    gap = np.asarray(Image.fromarray(enclosed.astype(np.uint8) * 255).resize((w1 * S, h1 * S), Image.NEAREST)) > 0
    gap &= out[:, :, 3] < 128
    rgb = out[:, :, :3].copy()
    known = out[:, :, 3] >= 128
    todo = gap.copy()
    for _ in range(reach * S):
        if not todo.any():
            break
        total = np.zeros_like(rgb)
        count = np.zeros(known.shape)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(np.roll(known, dy, 0), dx, 1)
            total += np.roll(np.roll(rgb, dy, 0), dx, 1) * k[:, :, None]
            count += k
        fill = todo & (count > 0)
        rgb[fill] = total[fill] / count[fill][:, None]
        known = known | fill
        todo &= ~fill
    done = gap & ~todo
    out[done, :3] = rgb[done]
    out[done, 3] = 255


def match_light(out, original, near=2, wide=8):
    """Scales the painting's local brightness to the original's, so the
    level keeps its lighting: the model tends to brighten dark areas, and
    gaps between dark tiles then show as black blocks. Where the original
    is near-black the match is tight (over `near` game pixels); elsewhere it
    is broad (`wide`), so the painting keeps its own detail."""
    def lum(rgba):
        return 0.30 * rgba[:, :, 0] + 0.59 * rgba[:, :, 1] + 0.11 * rgba[:, :, 2], rgba[:, :, 3] / 255.0
    w1, h1 = original.size
    o, oa = lum(np.asarray(original, dtype=np.float64))
    small = np.asarray(Image.fromarray(np.rint(out).clip(0, 255).astype(np.uint8), "RGBA").resize((w1, h1), Image.BOX), dtype=np.float64)
    p, pa = lum(small)
    def ratio(radius):
        o_mean = blur(o * oa, radius) / np.maximum(blur(oa, radius), 1e-3)
        p_mean = blur(p * pa, radius) / np.maximum(blur(pa, radius), 1e-3)
        return np.clip((o_mean + 3.0) / (p_mean + 3.0), 0.05, 2.0), o_mean
    tight, _ = ratio(near)
    broad, o_wide = ratio(wide)
    dark = np.clip((24.0 - o_wide) / 16.0, 0.0, 1.0)  # 1 where the original is near-black
    r = tight * dark + broad * (1.0 - dark)
    big = np.asarray(Image.fromarray(r.astype(np.float32), "F").resize((out.shape[1], out.shape[0]), Image.BILINEAR))
    return np.clip(out[:, :, :3] * big[:, :, None], 0, 255)


def smooth_alpha(alpha, size):
    """The 1x transparency upscaled smoothly and sharpened back to a clean
    edge about a game pixel wide."""
    a = np.asarray(alpha.resize(size, Image.BILINEAR), dtype=np.float64) / 255.0
    return np.clip((a - 0.5) * 2.5 + 0.5, 0.0, 1.0) * 255.0


def key_alpha(rgb):
    """Opacity from the magenta key: 0 on the key colour, 1 a little away
    from it."""
    d = np.sqrt(((rgb - np.array(KEY, dtype=np.float64)) ** 2).sum(axis=2))
    return np.clip((d - 60.0) / 80.0, 0.0, 1.0)


def defringe(rgb, passes=12):
    """Models blend edges into the magenta key. Pixels with a magenta cast
    are refilled from their clean neighbours, growing inwards."""
    r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
    bad = (r - g > 50) & (b - g > 50) & (np.abs(r - b) < 90)
    out = rgb.copy()
    for _ in range(passes):
        if not bad.any():
            break
        total = np.zeros_like(out)
        count = np.zeros(bad.shape)
        good = ~bad
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            shifted_good = np.roll(np.roll(good, dy, 0), dx, 1)
            total += np.roll(np.roll(out, dy, 0), dx, 1) * shifted_good[:, :, None]
            count += shifted_good
        fill = bad & (count > 0)
        out[fill] = total[fill] / count[fill][:, None]
        bad &= ~fill
    return out


def cmd_stitch(args):
    write_pack_json(args.pack)
    done = 0
    wanted = {w.strip().upper() for w in args.levels.split(",")} if getattr(args, "levels", None) else None
    for level in sorted(os.listdir(args.work)):
        if wanted and level not in wanted:
            continue
        for layer in LAYERS:
            base = os.path.join(args.work, level, layer)
            manifest = os.path.join(base, "manifest.json")
            if not os.path.exists(manifest):
                continue
            with open(manifest) as f:
                m = json.load(f)
            original = Image.open(layer_path(args.export, level, layer)).convert("RGBA")
            W, H = original.width * SCALE, original.height * SCALE
            # The original upscaled: the fallback, and the transparency.
            up = np.asarray(original.resize((W, H), Image.NEAREST), dtype=np.float64)
            colour = np.zeros((H, W, 3))
            weight = np.zeros((H, W))
            keyed = np.zeros((H, W))  # the model's own opacity (not magenta), weighted
            repainted = 0
            for t in m["tiles"]:
                path = os.path.join(base, "out", t["name"])
                if not os.path.exists(path):
                    continue
                repainted += 1
                tw, th = t["w"] * SCALE, t["h"] * SCALE
                raw = np.asarray(Image.open(path).convert("RGB").resize((tw, th), Image.LANCZOS), dtype=np.float64)
                tile = defringe(raw)
                col, row = feather(tw, th, (t["x"], t["y"], t["w"], t["h"]), m["width"], m["height"], args.overlap)
                wgt = np.outer(row, col)
                ox, oy = t["x"] * SCALE, t["y"] * SCALE
                colour[oy:oy + th, ox:ox + tw] += tile * wgt[:, :, None]
                weight[oy:oy + th, ox:ox + tw] += wgt
                keyed[oy:oy + th, ox:ox + tw] += key_alpha(raw) * wgt
            if not repainted:
                continue
            out = up.copy()
            if args.alpha == "key":
                # The painting's own outline: everything the model didn't
                # leave magenta. Shapes may differ from the original's.
                done_w = weight > 0
                out[done_w, 3] = keyed[done_w] / weight[done_w] * 255.0
            elif args.edges == "smooth":
                # Repainted parts get smooth outlines: the original's shape
                # within a fraction of a game pixel.
                alpha = smooth_alpha(original.getchannel("A"), (W, H))
                out[weight > 0, 3] = alpha[weight > 0]
            painted = (weight > 0) & (out[:, :, 3] > 0)
            out[painted, :3] = colour[painted] / weight[painted][:, None]
            if layer == "fg" and getattr(args, "show_wall", True):
                show_wall(out, original, os.path.join(args.export, "levels", level))
            if layer == "fg" and getattr(args, "fill_gaps", False):
                fill_gaps(out, original)
            if getattr(args, "match_light", True):
                out[:, :, :3] = match_light(out, original)
            dst = layer_path(args.pack, level, layer)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            Image.fromarray(np.rint(out).clip(0, 255).astype(np.uint8), "RGBA").save(dst)
            done += 1
            print(f"{level} {layer}: {repainted}/{len(m['tiles'])} tiles repainted")
    print(f"{done} layer(s) written to {args.pack}")


def cmd_nearest(args):
    write_pack_json(args.pack, smooth_text=False)
    for level in level_ids(args.export, args.levels):
        for layer in LAYERS:
            src = layer_path(args.export, level, layer)
            if not os.path.exists(src):
                continue
            img = Image.open(src)
            dst = layer_path(args.pack, level, layer)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST).save(dst)
    sprites = os.path.join(args.export, "sprites")
    if os.path.isdir(sprites) and not args.levels:
        out = os.path.join(args.pack, "sprites")
        os.makedirs(out, exist_ok=True)
        for name in os.listdir(sprites):
            if name.endswith(".png"):
                img = Image.open(os.path.join(sprites, name))
                img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST).save(os.path.join(out, name))
            elif name.endswith(".pal"):
                shutil.copy(os.path.join(sprites, name), out)
    for folder in ("hud", "font", "cells"):
        src = os.path.join(args.export, folder)
        if not os.path.isdir(src) or args.levels:
            continue
        dst = os.path.join(args.pack, folder)
        os.makedirs(dst, exist_ok=True)
        for name in os.listdir(src):
            if name.endswith(".png"):
                img = Image.open(os.path.join(src, name))
                img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST).save(os.path.join(dst, name))
            elif name.endswith(".pal"):
                shutil.copy(os.path.join(src, name), dst)
    print(f"round-trip pack written to {args.pack}")


def cmd_stitch_all(args):
    cmd_stitch(args)
    if os.path.exists(os.path.join(args.work, "sprites", "manifest.json")):
        cmd_stitch_sprites(args)
    if os.path.exists(os.path.join(args.work, "hud", "manifest.json")):
        cmd_stitch_hud(args)
    if os.path.exists(os.path.join(args.work, "font", "manifest.json")):
        cmd_stitch_font(args)
    if os.path.exists(os.path.join(args.work, "cells", "manifest.json")):
        cmd_stitch_cells(args)


# Changed cells -------------------------------------------------------------

CELL_COLS = 10


def cmd_split_cells(args):
    """The alternate metatiles (<export>/cells, from blackthorne --metatiles;
    with --only, just those listed in that cells.tsv) on magenta sheets."""
    src = os.path.join(args.export, "cells")
    names = sorted(n for n in os.listdir(src) if n.endswith(".png"))
    if args.only:
        wanted = set()
        for line in open(args.only):
            lvl, mt = line.split()
            wanted.add("%X_%03x.png" % (int(lvl, 16), int(mt, 16)))
        names = [n for n in names if n in wanted or n.split("_")[0] in args.whole.split(",")]
    base = os.path.join(args.work, "cells")
    os.makedirs(os.path.join(base, "in"), exist_ok=True)
    os.makedirs(os.path.join(base, "out"), exist_ok=True)
    pitch = 16 + GAP
    rows = (224 - GAP) // pitch
    # Sheets made before stay (they may be painted already); new cells go
    # on new sheets.
    sheets = []
    manifest = os.path.join(base, "manifest.json")
    if os.path.exists(manifest):
        with open(manifest) as f:
            sheets = json.load(f)["sheets"]
    done = {c["name"] for sh in sheets for c in sh["cells"]}
    names = [n for n in names if n not in done]
    number = len(sheets)
    for first in range(0, len(names), CELL_COLS * rows):
        batch = names[first:first + CELL_COLS * rows]
        used = (len(batch) + CELL_COLS - 1) // CELL_COLS
        w, h = GAP + CELL_COLS * pitch, GAP + used * pitch
        sheet = Image.new("RGB", (w * SCALE, h * SCALE), KEY)
        cells = []
        for i, name in enumerate(batch):
            x, y = GAP + (i % CELL_COLS) * pitch, GAP + (i // CELL_COLS) * pitch
            c = Image.open(os.path.join(src, name)).convert("RGBA").resize((16 * SCALE, 16 * SCALE), Image.NEAREST)
            sheet.paste(c, (x * SCALE, y * SCALE), mask=c.getchannel("A"))
            cells.append({"name": name, "x": x, "y": y})
        sname = f"c_{number}.png"
        number += 1
        sheet.save(os.path.join(base, "in", sname))
        sheets.append({"name": sname, "width": w, "height": h, "cells": cells})
    with open(os.path.join(base, "manifest.json"), "w") as f:
        json.dump({"scale": SCALE, "sheets": sheets}, f, indent=1)
    print(f"cells: {len(names)} new; {len(sheets)} sheets in {base}")


def cmd_stitch_cells(args):
    src = os.path.join(args.export, "cells")
    base = os.path.join(args.work, "cells")
    with open(os.path.join(base, "manifest.json")) as f:
        m = json.load(f)
    out = os.path.join(args.pack, "cells")
    os.makedirs(out, exist_ok=True)
    write_pack_json(args.pack)
    done = 0
    for sheet in m["sheets"]:
        path = os.path.join(base, "out", sheet["name"])
        if not os.path.exists(path):
            continue
        rgb = defringe(np.asarray(Image.open(path).convert("RGB").resize((sheet["width"] * SCALE, sheet["height"] * SCALE), Image.LANCZOS),
                                  dtype=np.float64))
        s = 16 * SCALE
        for c in sheet["cells"]:
            cell = rgb[c["y"] * SCALE:c["y"] * SCALE + s, c["x"] * SCALE:c["x"] * SCALE + s]
            alpha = np.rint(smooth_alpha(Image.open(os.path.join(src, c["name"])).convert("RGBA").getchannel("A"), (s, s))).astype(np.uint8)
            Image.fromarray(np.dstack([np.rint(cell).clip(0, 255).astype(np.uint8), alpha]), "RGBA").save(os.path.join(out, c["name"]))
            done += 1
    print(f"cells: {done}")


# Kyle's poses ---------------------------------------------------------------

POSE_CELL = 64  # game pixels per pose cell on a sheet (256 at 4x)


def cmd_split_poses(args):
    """Kyle's assembled poses (blackthorne --poses) on magenta sheets of
    4x3 cells, for repainting him as whole figures."""
    src = args.poses
    rows = [line.rstrip("\n").split("\t") for line in open(os.path.join(src, "poses.tsv"))]
    base = os.path.join(args.work, "poses")
    os.makedirs(os.path.join(base, "in"), exist_ok=True)
    os.makedirs(os.path.join(base, "out"), exist_ok=True)
    cols, per = 4, 12
    sheets = []
    for first in range(0, len(rows), per):
        batch = rows[first:first + per]
        n_rows = (len(batch) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * POSE_CELL * SCALE, n_rows * POSE_CELL * SCALE), KEY)
        cells = []
        for i, (name, w, h, sig) in enumerate(batch):
            w, h = int(w), int(h)
            if w > POSE_CELL or h > POSE_CELL:
                continue
            cx = (i % cols) * POSE_CELL + (POSE_CELL - w) // 2
            cy = (i // cols) * POSE_CELL + (POSE_CELL - h)  # standing on the cell's floor
            img = Image.open(os.path.join(src, name + ".png")).convert("RGBA").resize((w * SCALE, h * SCALE), Image.NEAREST)
            sheet.paste(img, (cx * SCALE, cy * SCALE), mask=img.getchannel("A"))
            parts = []
            for p in sig.strip(";").split(";"):
                key, size, x, y, mirror = p.split(",")
                parts.append({"key": key, "size": int(size), "x": int(x), "y": int(y), "mirror": mirror == "1"})
            cells.append({"pose": name, "x": cx, "y": cy, "w": w, "h": h, "parts": parts})
        sname = f"p_{first // per:03d}.png"
        sheet.save(os.path.join(base, "in", sname))
        sheets.append({"name": sname, "width": cols * POSE_CELL, "height": n_rows * POSE_CELL, "cells": cells})
    with open(os.path.join(base, "manifest.json"), "w") as f:
        json.dump({"scale": SCALE, "sheets": sheets}, f, indent=1)
    print(f"{len(rows)} poses on {len(sheets)} sheets in {base}")


def cmd_stitch_poses(args):
    """Cuts the repainted poses back into Kyle's parts: each part's region
    of its pose, turned back to how its data is stored, inside the part's
    own outline; written over the pack's loose-part paintings. A part in
    several poses takes its first."""
    base = os.path.join(args.work, "poses")
    with open(os.path.join(base, "manifest.json")) as f:
        m = json.load(f)
    out = os.path.join(args.pack, "sprites")
    os.makedirs(out, exist_ok=True)
    done = set()
    for sheet in m["sheets"]:
        path = os.path.join(base, "out", sheet["name"])
        if not os.path.exists(path):
            continue
        rgb = defringe(np.asarray(Image.open(path).convert("RGB").resize((sheet["width"] * SCALE, sheet["height"] * SCALE), Image.LANCZOS),
                                  dtype=np.float64))
        for c in sheet["cells"]:
            pal = os.path.join(args.poses, c["pose"] + ".pal")
            for p in c["parts"]:
                if p["key"] in done:
                    continue
                s = p["size"] * SCALE
                x0, y0 = (c["x"] + p["x"]) * SCALE, (c["y"] + p["y"]) * SCALE
                cell = rgb[y0:y0 + s, x0:x0 + s]
                if cell.shape[0] != s or cell.shape[1] != s:
                    continue
                if p["mirror"]:
                    cell = cell[:, ::-1]
                cell = defringe(cell.copy(), passes=s)  # wherever the outline reaches past the painted figure
                part_png = os.path.join(args.export, "sprites", p["key"] + ".png")
                if not os.path.exists(part_png):
                    continue
                alpha = np.rint(smooth_alpha(Image.open(part_png).convert("RGBA").getchannel("A"), (s, s))).astype(np.uint8)
                Image.fromarray(np.dstack([np.rint(cell).clip(0, 255).astype(np.uint8), alpha]), "RGBA").save(os.path.join(out, p["key"] + ".png"))
                shutil.copy(pal, os.path.join(out, p["key"] + ".pal"))
                done.add(p["key"])
    print(f"poses: {len(done)} parts of Kyle written to {out}")


# Fonts ---------------------------------------------------------------------

FONT_COLS = 16


def cmd_split_font(args):
    """The glyphs of both fonts on two magenta sheets (16 per row)."""
    src = os.path.join(args.export, "font")
    base = os.path.join(args.work, "font")
    os.makedirs(os.path.join(base, "in"), exist_ok=True)
    os.makedirs(os.path.join(base, "out"), exist_ok=True)
    sheets = []
    for f in (0, 1):
        names = sorted(n for n in os.listdir(src) if n.startswith(f"{f}_") and n.endswith(".png"))
        pitch = 8 + 4
        rows = (len(names) + FONT_COLS - 1) // FONT_COLS
        w, h = 4 + FONT_COLS * pitch, 4 + rows * pitch
        sheet = Image.new("RGB", (w * SCALE, h * SCALE), KEY)
        cells = []
        for i, name in enumerate(names):
            x, y = 4 + (i % FONT_COLS) * pitch, 4 + (i // FONT_COLS) * pitch
            g = Image.open(os.path.join(src, name)).convert("RGBA").resize((8 * SCALE, 8 * SCALE), Image.NEAREST)
            sheet.paste(g, (x * SCALE, y * SCALE), mask=g.getchannel("A"))
            cells.append({"name": name, "x": x, "y": y})
        sheet.save(os.path.join(base, "in", f"font{f}.png"))
        sheets.append({"name": f"font{f}.png", "font": f, "width": w, "height": h, "cells": cells})
    with open(os.path.join(base, "manifest.json"), "w") as fo:
        json.dump({"scale": SCALE, "sheets": sheets}, fo, indent=1)
    print(f"fonts: {sum(len(sh['cells']) for sh in sheets)} glyphs on {len(sheets)} sheets in {base}")


def cmd_stitch_font(args):
    """Cuts the painted font sheets into the pack's font/<f>_<nnn>.png."""
    src = os.path.join(args.export, "font")
    base = os.path.join(args.work, "font")
    with open(os.path.join(base, "manifest.json")) as fi:
        m = json.load(fi)
    out = os.path.join(args.pack, "font")
    os.makedirs(out, exist_ok=True)
    write_pack_json(args.pack)
    shutil.copy(os.path.join(src, "font.pal"), out)
    done = 0
    for sheet in m["sheets"]:
        path = os.path.join(base, "out", sheet["name"])
        if not os.path.exists(path):
            continue
        rgb = defringe(np.asarray(Image.open(path).convert("RGB").resize((sheet["width"] * SCALE, sheet["height"] * SCALE), Image.LANCZOS),
                                  dtype=np.float64))
        s = 8 * SCALE
        for c in sheet["cells"]:
            cell = rgb[c["y"] * SCALE:c["y"] * SCALE + s, c["x"] * SCALE:c["x"] * SCALE + s]
            if sheet["font"] == 0:  # the normal font fills its cell
                alpha = np.full((s, s), 255, dtype=np.uint8)
            else:
                alpha = np.rint(smooth_alpha(Image.open(os.path.join(src, c["name"])).convert("RGBA").getchannel("A"), (s, s))).astype(np.uint8)
            Image.fromarray(np.dstack([np.rint(cell).clip(0, 255).astype(np.uint8), alpha]), "RGBA").save(os.path.join(out, c["name"]))
            done += 1
    print(f"fonts: {done} glyphs")


# HUD -----------------------------------------------------------------------

def cmd_split_hud(args):
    """The HUD for the model: the side panel and the bottom bar as they are,
    and the icons (with the selection frame) on one magenta sheet."""
    src = os.path.join(args.export, "hud")
    base = os.path.join(args.work, "hud")
    os.makedirs(os.path.join(base, "in"), exist_ok=True)
    os.makedirs(os.path.join(base, "out"), exist_ok=True)
    for name in ("side.png", "bottom.png"):
        img = Image.open(os.path.join(src, name)).convert("RGB")
        img.resize((img.width * SCALE, img.height * SCALE), Image.NEAREST).save(os.path.join(base, "in", name))
    icons = sorted(n for n in os.listdir(src) if n.startswith("icon_") or n == "selection.png")
    pitch, cols = 32 + GAP, 6
    rows = (len(icons) + cols - 1) // cols
    sheet = Image.new("RGB", ((GAP + cols * pitch) * SCALE, (GAP + rows * pitch) * SCALE), KEY)
    cells = []
    for i, name in enumerate(icons):
        x, y = GAP + (i % cols) * pitch, GAP + (i // cols) * pitch
        icon = Image.open(os.path.join(src, name)).convert("RGBA").resize((32 * SCALE, 32 * SCALE), Image.NEAREST)
        sheet.paste(icon, (x * SCALE, y * SCALE), mask=icon.getchannel("A"))
        cells.append({"name": name, "x": x, "y": y})
    sheet.save(os.path.join(base, "in", "icons.png"))
    with open(os.path.join(base, "manifest.json"), "w") as f:
        json.dump({"scale": SCALE, "icons": cells, "icons_size": [sheet.width // SCALE, sheet.height // SCALE]}, f, indent=1)
    print(f"HUD: 2 panels and {len(icons)} icons in {base}")


def cmd_stitch_hud(args):
    src = os.path.join(args.export, "hud")
    base = os.path.join(args.work, "hud")
    with open(os.path.join(base, "manifest.json")) as f:
        m = json.load(f)
    out = os.path.join(args.pack, "hud")
    os.makedirs(out, exist_ok=True)
    write_pack_json(args.pack)
    shutil.copy(os.path.join(src, "hud.pal"), out)
    for name in ("side.png", "bottom.png"):
        path = os.path.join(base, "out", name)
        if os.path.exists(path):
            orig = Image.open(os.path.join(src, name))
            Image.open(path).convert("RGB").resize((orig.width * SCALE, orig.height * SCALE), Image.LANCZOS).convert("RGBA").save(os.path.join(out, name))
            print(f"HUD {name}")
    path = os.path.join(base, "out", "icons.png")
    if os.path.exists(path):
        w, h = m["icons_size"]
        rgb = defringe(np.asarray(Image.open(path).convert("RGB").resize((w * SCALE, h * SCALE), Image.LANCZOS), dtype=np.float64))
        s = 32 * SCALE
        for c in m["icons"]:
            cell = rgb[c["y"] * SCALE:c["y"] * SCALE + s, c["x"] * SCALE:c["x"] * SCALE + s]
            alpha = np.rint(smooth_alpha(Image.open(os.path.join(src, c["name"])).convert("RGBA").getchannel("A"), (s, s))).astype(np.uint8)
            Image.fromarray(np.dstack([np.rint(cell).clip(0, 255).astype(np.uint8), alpha]), "RGBA").save(os.path.join(out, c["name"]))
        print(f"HUD: {len(m['icons'])} icons")


# Sprites -------------------------------------------------------------------

def random_input(seed, frames, explore=False):
    """Scripted play like tools/playtest.py: random moves, jumps, shots.
    explore: mostly long walks and runs one way, to get further."""
    r = random.Random(seed)
    moves = [0x20, 0x10, 0x220, 0x210, 0x100, 0x120, 0x110, 0x8, 0x4, 0, 0x40, 0x80, 0x400, 0x800, 0x1000, 0x2000, 0x60, 0x50]
    way = r.choice((0x20, 0x10))
    spec, t = [], 60
    while t < frames:
        n = r.randint(3, 40)
        bits = r.choice(moves)
        if explore and r.random() < 0.6:
            if r.random() < 0.15:
                way ^= 0x30
            n = r.randint(30, 160)
            bits = way | r.choice((0, 0x200, 0x100))
        if bits:
            spec.append("%d-%d:%x" % (t, t + n - 1, bits))
        t += n + r.randint(0, 6)
    return ",".join(spec)


def cmd_collect(args):
    """Plays every exported level headless with random input and saves the
    sprite parts shown to <export>/sprites (blackthorne --parts)."""
    out = os.path.join(args.export, "sprites")
    tmp = os.path.join(args.export, "collect.bmp")
    for level in level_ids(args.export, args.levels):
        for run in range(args.runs):
            cmd = [args.exe, args.data, "--level", level, "--ticks", str(args.frames), "--size", "320x240", "--shot", tmp,
                   "--pack", os.path.join(args.export, "no-pack"), "--parts", out, "--godmode",
                   "--input", random_input(int(level, 16) * 100 + run, args.frames, explore=args.explore)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                print(f"{level} run {run}: exit {r.returncode} {r.stderr.strip()[:200]}")
        count = sum(1 for n in os.listdir(out) if n.endswith(".png")) if os.path.isdir(out) else 0
        print(f"{level}: {count} parts so far")
    if os.path.exists(tmp):
        os.remove(tmp)


SHEET = 256  # sprite sheet width at 1x (1024 at 4x)
GAP = 8      # magenta between sprites at 1x


def cmd_split_sprites(args):
    """Puts the collected parts on sheets for the model, by size: each sheet
    is at most 256x224 game pixels (1024x896 at 4x)."""
    src = os.path.join(args.export, "sprites")
    base = os.path.join(args.work, "sprites")
    os.makedirs(os.path.join(base, "in"), exist_ok=True)
    os.makedirs(os.path.join(base, "out"), exist_ok=True)
    # Sheets made before stay as they are (they may be repainted already);
    # parts collected since go on new sheets.
    sheets, done = [], set()
    manifest = os.path.join(base, "manifest.json")
    if os.path.exists(manifest):
        with open(manifest) as f:
            sheets = json.load(f)["sheets"]
        done = {c["key"] for sh in sheets for c in sh["cells"]}
    names = sorted(n[:-4] for n in os.listdir(src) if n.endswith(".png") and n[:-4] not in done)
    by_size = {}
    for key in names:
        by_size.setdefault(Image.open(os.path.join(src, key + ".png")).width, []).append(key)
    for size, keys in sorted(by_size.items()):
        pitch = size + GAP
        cols, rows = max(1, (SHEET - GAP) // pitch), max(1, (224 - GAP) // pitch)
        number = sum(1 for sh in sheets if sh["name"].startswith(f"s{size}_"))
        for first in range(0, len(keys), cols * rows):
            batch = keys[first:first + cols * rows]
            used_rows = (len(batch) + cols - 1) // cols
            w, h = GAP + cols * pitch, GAP + used_rows * pitch
            sheet = Image.new("RGB", (w * SCALE, h * SCALE), KEY)
            cells = []
            for i, key in enumerate(batch):
                x, y = GAP + (i % cols) * pitch, GAP + (i // cols) * pitch
                part = Image.open(os.path.join(src, key + ".png")).convert("RGBA")
                part = part.resize((size * SCALE, size * SCALE), Image.NEAREST)
                sheet.paste(part, (x * SCALE, y * SCALE), mask=part.getchannel("A"))
                cells.append({"key": key, "x": x, "y": y, "size": size})
            name = f"s{size}_{number}.png"
            number += 1
            sheet.save(os.path.join(base, "in", name))
            sheets.append({"name": name, "width": w, "height": h, "cells": cells})
    with open(os.path.join(base, "manifest.json"), "w") as f:
        json.dump({"scale": SCALE, "key": "#%02x%02x%02x" % KEY, "sheets": sheets}, f, indent=1)
    print(f"{len(names)} new parts; {len(sheets)} sheets in {base}")


def cmd_stitch_sprites(args):
    """Cuts repainted sheets back into the pack's sprites/<key>.png, inside
    each part's original shape, with its palette (<key>.pal)."""
    src = os.path.join(args.export, "sprites")
    base = os.path.join(args.work, "sprites")
    with open(os.path.join(base, "manifest.json")) as f:
        m = json.load(f)
    out = os.path.join(args.pack, "sprites")
    os.makedirs(out, exist_ok=True)
    write_pack_json(args.pack)
    done = 0
    for sheet in m["sheets"]:
        path = os.path.join(base, "out", sheet["name"])
        if not os.path.exists(path):
            continue
        rgb = defringe(np.asarray(Image.open(path).convert("RGB").resize((sheet["width"] * SCALE, sheet["height"] * SCALE), Image.LANCZOS),
                                  dtype=np.float64))
        for c in sheet["cells"]:
            s = c["size"] * SCALE
            cell = rgb[c["y"] * SCALE:c["y"] * SCALE + s, c["x"] * SCALE:c["x"] * SCALE + s]
            original = Image.open(os.path.join(src, c["key"] + ".png")).convert("RGBA").getchannel("A")
            if args.edges == "smooth":
                alpha = np.rint(smooth_alpha(original, (s, s))).astype(np.uint8)
            else:
                alpha = np.asarray(original.resize((s, s), Image.NEAREST))
            rgba = np.dstack([np.rint(cell).clip(0, 255).astype(np.uint8), alpha])
            Image.fromarray(rgba, "RGBA").save(os.path.join(out, c["key"] + ".png"))
            shutil.copy(os.path.join(src, c["key"] + ".pal"), out)
            done += 1
    print(f"{done} sprite(s) written to {out}")


def cmd_check(args):
    with open(os.path.join(args.pack, "pack.json")) as f:
        scale = json.load(f).get("scale", 4)
    problems = 0
    for level in level_ids(args.export, None):
        for layer in LAYERS:
            src = layer_path(args.export, level, layer)
            dst = layer_path(args.pack, level, layer)
            if not os.path.exists(src) or not os.path.exists(dst):
                continue
            a = Image.open(src)
            b = Image.open(dst)
            want = (a.width * scale, a.height * scale)
            if b.size != want:
                print(f"{level} {layer}: {b.size[0]}x{b.size[1]}, want {want[0]}x{want[1]} (the game ignores it)")
                problems += 1
                continue
            # Painted pixels where the original is transparent cover what is behind.
            mask = np.asarray(a.convert("RGBA").getchannel("A").resize(want, Image.NEAREST)) > 0
            extra = int(((np.asarray(b.convert("RGBA").getchannel("A")) > 0) & ~mask).sum())
            note = f", {extra} px painted where the original is transparent" if extra else ""
            print(f"{level} {layer}: ok{note}")
    sys.exit(1 if problems else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("split")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("--levels")
    s.add_argument("--tile", default="256x224", help="tile size at 1x (default 256x224: 1024x896 at 4x)")
    s.add_argument("--overlap", type=int, default=32, help="overlap between tiles at 1x")
    s.set_defaults(func=cmd_split)
    s = sub.add_parser("stitch")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("pack")
    s.add_argument("--overlap", type=int, default=32, help="the overlap split used, at 1x")
    s.add_argument("--edges", choices=("smooth", "hard"), default="smooth",
                   help="outlines: smooth, or hard (the 1x shape in 4x blocks)")
    s.add_argument("--alpha", choices=("shape", "key"), default="shape",
                   help="transparency from the original's shape, or from the painting (not magenta)")
    s.add_argument("--no-match-light", dest="match_light", action="store_false",
                   help="keep the painting's own brightness")
    s.add_argument("--levels", help="only these level ids (comma separated)")
    s.add_argument("--fill-gaps", action="store_true",
                   help="fill enclosed gaps in the foreground from the painting around them")
    s.add_argument("--no-show-wall", dest="show_wall", action="store_false",
                   help="keep foreground cells that repeat the wall behind them opaque")
    s.set_defaults(func=cmd_stitch)
    s = sub.add_parser("nearest")
    s.add_argument("export")
    s.add_argument("pack")
    s.add_argument("--levels")
    s.set_defaults(func=cmd_nearest)
    s = sub.add_parser("check")
    s.add_argument("export")
    s.add_argument("pack")
    s.set_defaults(func=cmd_check)
    s = sub.add_parser("stitch-all", help="stitch --alpha key, stitch-sprites and stitch-hud in one go")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("pack")
    s.add_argument("--overlap", type=int, default=32)
    s.add_argument("--edges", choices=("smooth", "hard"), default="smooth")
    s.add_argument("--alpha", choices=("shape", "key"), default="key")
    s.add_argument("--no-match-light", dest="match_light", action="store_false")
    s.add_argument("--fill-gaps", action="store_true")
    s.set_defaults(func=cmd_stitch_all)
    s = sub.add_parser("split-cells")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("--only", help="a cells.tsv (level, metatile in hex) of the cells to include")
    s.add_argument("--whole", default="", help="levels whose alternates are all included anyway (comma separated)")
    s.set_defaults(func=cmd_split_cells)
    s = sub.add_parser("stitch-cells")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("pack")
    s.set_defaults(func=cmd_stitch_cells)
    s = sub.add_parser("split-poses")
    s.add_argument("poses", help="the folder blackthorne --poses wrote")
    s.add_argument("work")
    s.set_defaults(func=cmd_split_poses)
    s = sub.add_parser("stitch-poses")
    s.add_argument("poses")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("pack")
    s.set_defaults(func=cmd_stitch_poses)
    s = sub.add_parser("split-font")
    s.add_argument("export")
    s.add_argument("work")
    s.set_defaults(func=cmd_split_font)
    s = sub.add_parser("stitch-font")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("pack")
    s.set_defaults(func=cmd_stitch_font)
    s = sub.add_parser("split-hud")
    s.add_argument("export")
    s.add_argument("work")
    s.set_defaults(func=cmd_split_hud)
    s = sub.add_parser("stitch-hud")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("pack")
    s.set_defaults(func=cmd_stitch_hud)
    s = sub.add_parser("collect")
    s.add_argument("exe", help="blackthorne executable")
    s.add_argument("data", help="DATA.DAT")
    s.add_argument("export")
    s.add_argument("--levels")
    s.add_argument("--runs", type=int, default=3, help="random play-throughs per level")
    s.add_argument("--frames", type=int, default=3000)
    s.add_argument("--explore", action="store_true", help="long walks one way instead of short random moves")
    s.set_defaults(func=cmd_collect)
    s = sub.add_parser("split-sprites")
    s.add_argument("export")
    s.add_argument("work")
    s.set_defaults(func=cmd_split_sprites)
    s = sub.add_parser("stitch-sprites")
    s.add_argument("export")
    s.add_argument("work")
    s.add_argument("pack")
    s.add_argument("--edges", choices=("smooth", "hard"), default="smooth")
    s.set_defaults(func=cmd_stitch_sprites)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
