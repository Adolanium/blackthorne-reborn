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


def write_pack_json(pack):
    os.makedirs(pack, exist_ok=True)
    with open(os.path.join(pack, "pack.json"), "w") as f:
        json.dump({"scale": SCALE}, f)
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
    for level in sorted(os.listdir(args.work)):
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
            dst = layer_path(args.pack, level, layer)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            Image.fromarray(np.rint(out).clip(0, 255).astype(np.uint8), "RGBA").save(dst)
            done += 1
            print(f"{level} {layer}: {repainted}/{len(m['tiles'])} tiles repainted")
    print(f"{done} layer(s) written to {args.pack}")


def cmd_nearest(args):
    write_pack_json(args.pack)
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
    print(f"round-trip pack written to {args.pack}")


# Sprites -------------------------------------------------------------------

def random_input(seed, frames):
    """Scripted play like tools/playtest.py: random moves, jumps, shots."""
    r = random.Random(seed)
    moves = [0x20, 0x10, 0x220, 0x210, 0x100, 0x120, 0x110, 0x8, 0x4, 0, 0x40, 0x80, 0x400, 0x800, 0x1000, 0x2000, 0x60, 0x50]
    spec, t = [], 60
    while t < frames:
        n = r.randint(3, 40)
        bits = r.choice(moves)
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
                   "--pack", os.path.join(args.export, "no-pack"), "--parts", out,
                   "--input", random_input(int(level, 16) * 100 + run, args.frames)]
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
    names = sorted(n[:-4] for n in os.listdir(src) if n.endswith(".png"))
    by_size = {}
    for key in names:
        by_size.setdefault(Image.open(os.path.join(src, key + ".png")).width, []).append(key)
    base = os.path.join(args.work, "sprites")
    os.makedirs(os.path.join(base, "in"), exist_ok=True)
    os.makedirs(os.path.join(base, "out"), exist_ok=True)
    sheets = []
    for size, keys in sorted(by_size.items()):
        pitch = size + GAP
        cols, rows = max(1, (SHEET - GAP) // pitch), max(1, (224 - GAP) // pitch)
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
            name = f"s{size}_{first // (cols * rows)}.png"
            sheet.save(os.path.join(base, "in", name))
            sheets.append({"name": name, "width": w, "height": h, "cells": cells})
    with open(os.path.join(base, "manifest.json"), "w") as f:
        json.dump({"scale": SCALE, "key": "#%02x%02x%02x" % KEY, "sheets": sheets}, f, indent=1)
    print(f"{len(names)} parts on {len(sheets)} sheets in {base}")


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
    s = sub.add_parser("collect")
    s.add_argument("exe", help="blackthorne executable")
    s.add_argument("data", help="DATA.DAT")
    s.add_argument("export")
    s.add_argument("--levels")
    s.add_argument("--runs", type=int, default=3, help="random play-throughs per level")
    s.add_argument("--frames", type=int, default=3000)
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
