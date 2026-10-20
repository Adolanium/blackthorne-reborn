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

Exports, work folders and packs come from the original art: keep them in
git-ignored folders (out/, packs/).

  python tools/artpack.py split   out/export out/work [--levels C0,DB] [--tile 256x224] [--overlap 32]
  python tools/artpack.py stitch  out/export out/work packs/<name>
  python tools/artpack.py nearest out/export packs/<name> [--levels ...]
  python tools/artpack.py check   out/export packs/<name>

Work folder layout (one folder per level and layer):
  <work>/<ID>/<fg|bg>/in/<x>_<y>.png   the model's input (4x, key colour where transparent)
  <work>/<ID>/<fg|bg>/out/<x>_<y>.png  the repaint goes here (any size; resized on stitch)
  <work>/<ID>/<fg|bg>/manifest.json    tile rectangles at 1x
  <work>/<ID>/preview.png              the level, for context
"""

import argparse
import json
import os
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
            repainted = 0
            for t in m["tiles"]:
                path = os.path.join(base, "out", t["name"])
                if not os.path.exists(path):
                    continue
                repainted += 1
                tw, th = t["w"] * SCALE, t["h"] * SCALE
                tile = np.asarray(Image.open(path).convert("RGB").resize((tw, th), Image.LANCZOS), dtype=np.float64)
                col, row = feather(tw, th, (t["x"], t["y"], t["w"], t["h"]), m["width"], m["height"], args.overlap)
                wgt = np.outer(row, col)
                ox, oy = t["x"] * SCALE, t["y"] * SCALE
                colour[oy:oy + th, ox:ox + tw] += tile * wgt[:, :, None]
                weight[oy:oy + th, ox:ox + tw] += wgt
            if not repainted:
                continue
            out = up.copy()
            painted = (weight > 0) & (up[:, :, 3] > 0)
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
    print(f"round-trip pack written to {args.pack}")


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
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
