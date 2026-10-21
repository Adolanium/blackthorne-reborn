"""Repaints an artpack.py work folder with Codex CLI and its image model
(docs/VISUALS.md): every level section in <work>/<ID>/<fg|bg>/in/ and every
sprite sheet in <work>/sprites/in/ that has no out/ file yet.

Each job runs `codex exec` in its own folder with the input, the style
references and a prompt, and expects out.png back. Jobs run in parallel,
the run can be stopped and resumed, and a result whose painted shape
strays too far from the original's (a model that moved platforms) is
retried.

  python tools/codex_repaint.py <work> --style <style dir> [--jobs 6] [--only C0,sprites] [--tries 3]

<style dir> holds the approved look: fg.png, bg.png (repainted level
sections) and sprites.png (a repainted sprite sheet). They are attached to
every job as references.

Works with whatever the work folder holds; it doesn't commit or publish
anything. The images come from the original game: keep them git-ignored.
"""

import argparse
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import threading
import time

import numpy as np
from PIL import Image

KEY = np.array((255, 0, 255), dtype=np.float64)

STYLE = (
    "Art direction: hand-painted 2D game art in the style of Hollow Knight. Clean, confident shapes with crisp painted "
    "edges, soft gradients, muted moody colours with a few glowing accents, atmospheric depth, subtle rim light. "
    "Absolutely no pixel art, no pixelation, no dithering, no hard stair-step edges. Match the look of the attached "
    "style reference images exactly: same rendering, line quality, level of detail and lighting."
)

PROMPTS = {
    "fg": """in.png is the foreground layer of one section of a level from a 1994 side-scrolling platformer, upscaled from pixel art. style.png shows the approved repaint of another section of the game: it defines the target style.

Use your image generation tool to repaint in.png in that style, then save the result as out.png in the current folder at exactly {w}x{h} pixels.

{style}

Keep the same content: every structure and object stays recognisable, in the same place, at the same scale, in the same material and colour family (rock, stone, wood, metal, bone, vegetation, water, fire, machinery...). Redraw shapes, textures and details freely in the new style.

Gameplay constraints you must keep: characters walk on the top edges of platforms, so every platform top must stay flat and at exactly the same height and horizontal extent. Walls, ladders, doors, bridges and openings stay where they are. Do not add new platforms or block openings.

The flat magenta (#ff00ff) areas are empty space (another layer shows through there): keep them flat magenta, with clean edges against the painted objects. Keep any people or creatures already in the image (cutscene pictures have them) and add none. No new text, no border.

When out.png is saved, reply with only the word DONE.""",
    "bg": """in.png is the background layer of one section of a level from a 1994 side-scrolling platformer, upscaled from pixel art. It sits behind the playable foreground. style.png shows the approved repaint of another background section: it defines the target style.

Use your image generation tool to repaint in.png in that style, then save the result as out.png in the current folder at exactly {w}x{h} pixels.

{style}

Keep the same content: every wall, structure and object stays recognisable, in the same place, at the same scale and in the same colour family. Redraw them freely in the new style. As a background it should be darker, softer and less contrasted than foreground art, with depth and atmosphere (haze, faint light).

The flat magenta (#ff00ff) areas are empty (the foreground covers them or they stay black): keep them flat magenta with clean edges. Keep any people or creatures already in the image (cutscene pictures have them) and add none. No new text, no border.

When out.png is saved, reply with only the word DONE.""",
    "hud": """in.png is part of the HUD of a 1994 side-scrolling platformer, upscaled from pixel art: {what}. style.png shows the approved style of the game's repainted levels.

Use your image generation tool to repaint in.png in that style, then save it as out.png in the current folder at exactly {w}x{h} pixels.

{style}

Keep the layout exactly: every frame, border, slot and box at the same place and size, because the game draws icons and text into them. The flat black boxes and slots stay flat black. Keep the same motifs (bones, skulls, rope) and colour family. {extra}No text, no border around the image.

When out.png is saved, reply with only the word DONE.""",
    "poses": """in.png is a sheet of poses of Kyle Blackthorne, the hero of the 1994 platformer Blackthorne, upscaled from the original pixel art, each pose on a flat magenta (#ff00ff) background: standing, walking, running, aiming and firing his shotgun, climbing, jumping, hanging, hurt and dying. style.png is a sprite sheet already repainted in the target style.

Use your image generation tool to repaint the whole sheet in that style, then save it as out.png in the current folder at exactly {w}x{h} pixels.

{style}

Kyle must look like the same man in every pose, faithful to the original art: a tall, muscular man with long straight jet-black hair falling past his shoulders, a stern tanned face with dark eyes and a strong jaw (no beard, no headband), a light grey sleeveless shirt, blue jeans worn through at the knees, black boots and a black belt, holding a pump-action shotgun (dark metal barrel, brown wooden grip). Keep his anatomy natural: arms attach at the shoulders with believable proportions, hands hold the shotgun firmly, the head sits on the neck without a seam. Keep every pose exactly where it is, at the same size and with the same silhouette and limb positions, because the game cuts each pose into pieces and animates them. Keep the magenta background flat magenta with clean edges. No text, no border.

When out.png is saved, reply with only the word DONE.""",
    "cells": """in.png is a sheet of map cells from a 1994 side-scrolling platformer, upscaled from pixel art, each on a flat magenta (#ff00ff) background: the tiles the game swaps into a level while playing (opened doors and gates, bridges, switches, broken walls, menu text and similar), each 16x16 game pixels. style.png shows the approved style of the game's repainted levels.

Use your image generation tool to repaint the whole sheet in that style, then save it as out.png in the current folder at exactly {w}x{h} pixels.

{style}

Each cell keeps exactly its position, size and outline on the sheet, and its content stays recognisable (the same object, material and colours, and any letters stay the same letters). Cells are pieces of larger structures, so keep edges that continue to the cell border running to the border. Keep the magenta background flat magenta with clean edges. Do not add or remove cells.

When out.png is saved, reply with only the word DONE.""",
    "font": """in.png is the text font of a 1994 side-scrolling platformer, upscaled from pixel art: one character per cell on a flat magenta (#ff00ff) background, 16 cells per row: capital letters, digits, punctuation, a few symbols, and pieces of a dialogue box frame (corners and edges). style.png shows the approved style of the game's repainted art.

Use your image generation tool to repaint the whole sheet as crisp hand-painted lettering in that style, then save it as out.png in the current folder at exactly {w}x{h} pixels.

{style}

Every character must stay exactly the same character, clearly legible, in the same cell, filling the cell the same way: the same letter shapes and stroke weights, only clean and smooth instead of pixelated (like a carved or engraved fantasy serif with a subtle bevel). Keep each cell's colours (the light letters, their dark outline or shadow, and any dark cell background). Frame pieces stay frame pieces that join up with their neighbours. Keep the magenta between cells flat magenta. Do not add, remove or swap characters.

When out.png is saved, reply with only the word DONE.""",
    "sprites": """in.png is a sprite sheet from a 1994 side-scrolling platformer, upscaled from pixel art, on a flat magenta (#ff00ff) background: animation frames of characters, creatures, items and effects, some of them pieces of a larger character (a head, an arm, a weapon drawn separately). The hero is a muscular man with long black hair, a sleeveless vest, blue trousers, black boots and a shotgun. style.png is a sheet already repainted in the target style.

Use your image generation tool to repaint the whole of in.png in that style, then save it as out.png in the current folder at exactly {w}x{h} pixels.

{style}

Each sprite must keep exactly its position on the sheet, its size, its pose and its outline, because the game shows and animates them in these exact places. Repaint inside each silhouette. Human faces are human faces, not masks. Pale blue-grey silhouettes are flash effects: keep them as smooth pale glowing silhouettes. Keep the magenta background flat magenta with clean edges. Do not add or remove sprites. No text, no border.

When out.png is saved, reply with only the word DONE.""",
}

HUD_PARTS = {
    "side.png": ("the right-hand panel, a frame of bones and rope around eight inventory slots", ""),
    "bottom.png": ("the bottom bar, a strip of bones and skulls with a black box in the middle for the item name", ""),
    "icons.png": ("a sheet of inventory item icons (potions, bombs, keys, a wasp and similar, an empty slot frame and a "
                  "selection frame) on a flat magenta (#ff00ff) background",
                  "Each icon keeps its position, size and outline on the sheet; keep the magenta background flat magenta. "),
}

MIN_SHAPE = None  # --min-shape
print_lock = threading.Lock()


def log(msg):
    with print_lock:
        line = time.strftime("%H:%M:%S") + " " + msg
        print(line.encode("ascii", "replace").decode("ascii"), flush=True)


def coverage(img):
    """Opaque mask of an image on the magenta key."""
    rgb = np.asarray(img.convert("RGB"), dtype=np.float64)
    return np.sqrt(((rgb - KEY) ** 2).sum(axis=2)) > 100


def shape_ok(src, out, kind):
    """The painting must cover roughly what the original covers."""
    a = coverage(Image.open(src))
    b = coverage(Image.open(out).resize(Image.open(src).size, Image.BILINEAR))
    if not a.any():
        return True, 1.0
    iou = (a & b).sum() / max(1, (a | b).sum())
    return iou >= (MIN_SHAPE if MIN_SHAPE is not None else 0.65 if kind == "poses" else 0.8 if kind in ("sprites", "hud", "font", "cells") else 0.6), iou


def jobs_in(work, only):
    jobs = []
    for level in sorted(os.listdir(work)):
        if only and level.upper() not in only:
            continue
        if level == "poses":
            base = os.path.join(work, level)
            for name in sorted(os.listdir(os.path.join(base, "in"))):
                jobs.append(("poses", base, name))
            continue
        if level == "cells":
            base = os.path.join(work, level)
            for name in sorted(os.listdir(os.path.join(base, "in"))):
                jobs.append(("cells", base, name))
            continue
        if level == "font":
            base = os.path.join(work, level)
            for name in sorted(os.listdir(os.path.join(base, "in"))):
                jobs.append(("font", base, name))
            continue
        if level == "hud":
            base = os.path.join(work, level)
            for name in sorted(os.listdir(os.path.join(base, "in"))):
                jobs.append(("hud", base, name))
            continue
        if level == "sprites":
            base = os.path.join(work, level)
            if os.path.isdir(os.path.join(base, "in")):
                for name in sorted(os.listdir(os.path.join(base, "in"))):
                    jobs.append(("sprites", base, name))
            continue
        for layer in ("fg", "bg"):
            base = os.path.join(work, level, layer)
            if os.path.isdir(os.path.join(base, "in")):
                for name in sorted(os.listdir(os.path.join(base, "in"))):
                    jobs.append((layer, base, name))
    return jobs


def digest(job):
    with open(os.path.join(job[1], "in", job[2]), "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def share_duplicates(jobs):
    """Identical inputs (the same scene shown on several screens) share one
    painting: the first one's, so they look the same everywhere."""
    first = {}
    copied = 0
    for j in jobs:
        d = digest(j)
        out = os.path.join(j[1], "out", j[2])
        if d not in first:
            if os.path.exists(out):
                first[d] = out
            continue
        if os.path.abspath(first[d]) != os.path.abspath(out):
            shutil.copy(first[d], out)
            copied += 1
    return copied


def run_job(job, args):
    kind, base, name = job
    src = os.path.join(base, "in", name)
    w, h = Image.open(src).size
    label = os.path.relpath(src, args.work)
    jobdir = os.path.join(args.work, "_jobs", label.replace(os.sep, "_").replace(".png", ""))
    style_ref = os.path.join(args.style, {"fg": "fg.png", "bg": "bg.png", "sprites": "sprites.png", "hud": "fg.png", "font": "fg.png", "cells": "fg.png", "poses": "sprites.png"}[kind])
    for attempt in range(1, args.tries + 1):
        shutil.rmtree(jobdir, ignore_errors=True)
        os.makedirs(jobdir)
        shutil.copy(src, os.path.join(jobdir, "in.png"))
        shutil.copy(style_ref, os.path.join(jobdir, "style.png"))
        what, extra = HUD_PARTS.get(name, ("", ""))
        prompt = PROMPTS[kind].format(w=w, h=h, style=STYLE, what=what, extra=extra)
        if args.note:
            prompt = prompt.replace("When out.png is saved", args.note + "\n\nWhen out.png is saved")
        with open(os.path.join(jobdir, "prompt.txt"), "w") as f:
            f.write(prompt)
        start = time.time()
        with open(os.path.join(jobdir, "prompt.txt")) as stdin, open(os.path.join(jobdir, "codex.log"), "w") as logf:
            try:
                model = ["-m", args.model] if args.model else []
                r = subprocess.run([shutil.which(args.codex) or args.codex, "exec", *model, "--skip-git-repo-check", "--sandbox", "workspace-write", "-C", jobdir,
                                    "-i", "in.png", "style.png"], stdin=stdin, stdout=logf, stderr=subprocess.STDOUT,
                                   timeout=args.timeout)
                code = r.returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        out = os.path.join(jobdir, "out.png")
        if code == 0 and os.path.exists(out):
            ok, iou = shape_ok(src, out, kind)
            if ok:
                img = Image.open(out).convert("RGB")
                if img.size != (w, h):
                    img = img.resize((w, h), Image.LANCZOS)
                img.save(os.path.join(base, "out", name))
                log(f"done {label} (try {attempt}, {time.time() - start:.0f}s, shape {iou:.2f})")
                return True
            log(f"retry {label}: shape {iou:.2f} too far from the original")
        else:
            tail = ""
            try:
                with open(os.path.join(jobdir, "codex.log"), errors="replace") as f:
                    tail = f.read()[-300:].replace("\n", " ")
            except OSError:
                pass
            log(f"retry {label}: exit {code} {tail}")
            if "rate limit" in tail.lower() or "usage limit" in tail.lower():
                time.sleep(300)
    log(f"FAILED {label} after {args.tries} tries")
    return False


def safe_job(job, args):
    try:
        return run_job(job, args)
    except Exception as e:  # one bad job must not stop the run
        log(f"FAILED {job[1]} {job[2]}: {e!r}")
        return False


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("work")
    p.add_argument("--style", required=True, help="folder with fg.png, bg.png, sprites.png")
    p.add_argument("--jobs", type=int, default=6)
    p.add_argument("--only", help="comma-separated level ids and/or 'sprites'")
    p.add_argument("--tries", type=int, default=3)
    p.add_argument("--timeout", type=int, default=1200, help="seconds per Codex run")
    p.add_argument("--codex", default="codex")
    p.add_argument("--model", help="Codex model (default: Codex's configured one)")
    p.add_argument("--min-shape", type=float, help="the least overlap of painted and original shape to accept")
    p.add_argument("--note", help="an extra instruction added to every prompt")
    p.add_argument("--redo", help="comma-separated input names (like 0_576.png) to repaint even if done")
    args = p.parse_args()
    global MIN_SHAPE
    MIN_SHAPE = args.min_shape
    only = {o.strip().upper() for o in args.only.split(",")} if args.only else None
    for name in ("fg.png", "bg.png", "sprites.png"):
        if not os.path.exists(os.path.join(args.style, name)):
            sys.exit(f"missing {os.path.join(args.style, name)}")
    every = jobs_in(args.work, only)
    if args.redo:
        names = {n.strip() for n in args.redo.split(",")}
        for j in every:
            if j[2] in names and os.path.exists(os.path.join(j[1], "out", j[2])):
                os.remove(os.path.join(j[1], "out", j[2]))
    share_duplicates(every)
    seen, jobs = set(), []
    for j in every:
        d = digest(j)
        if d in seen:
            continue
        seen.add(d)
        if not os.path.exists(os.path.join(j[1], "out", j[2])):
            jobs.append(j)
    log(f"{len(jobs)} job(s) to do ({len(every) - len(seen)} duplicates share a painting), {args.jobs} at a time")
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for ok in pool.map(lambda j: safe_job(j, args), jobs):
            failed += not ok
    copied = share_duplicates(every)
    log(f"finished: {len(jobs) - failed} done, {failed} failed, {copied} duplicate(s) copied")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
