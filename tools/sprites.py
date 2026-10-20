"""Decode Blackthorne sprite banks and export sprite sheets per level.

Usage: python -I tools/sprites.py <extracted dir> <out dir>

Format notes live in docs/FORMATS.md ("Sprite banks").
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

LEVELS = [191, 192, 193, 194, 195, 203, 204, 205, 206,
          217, 218, 219, 220, 235, 236, 237, 238, 239]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def entry(src, res_id):
    """Level descriptors refer to resources by 1-based id."""
    return (src / f"{res_id - 1:03d}.bin").read_bytes()


def parse_level(b):
    """Walk the descriptor in the same order as sub_105B4's helpers."""
    o = 0x36
    while u16(b, o) != 0xFFFF:          # sub_1074E: 14-byte records
        o += 14
    o += 2
    resources = []                       # sub_10760: (id, slot) pairs
    while u16(b, o) != 0xFFFF:
        resources.append((u16(b, o), b[o + 2]))
        o += 3
    o += 2 + 2                           # sub_107BC leading u16
    while b[o] != 0:
        o += 3
        while u16(b, o) != 0xFFFF:
            o += 2
        o += 2
    o += 1 + 2                           # sub_10802 leading u16
    while b[o] != 0:
        o += 8
    o += 1
    while u16(b, o) != 0xFFFF:          # sub_10876: 6-byte records
        o += 6
    o += 2
    banks = []                           # sub_108A5: (id, w8, h8, flag)
    while u16(b, o) != 0xFFFF:
        banks.append((u16(b, o), b[o + 2], b[o + 3], b[o + 4]))
        o += 5
    return resources, banks


def level_palette(src, resources):
    """256 colours: a 768-byte palette at slot 0, 16-colour fragments at their slot."""
    pal = np.zeros((256, 3), int)
    for res_id, slot in resources:
        data = entry(src, res_id)
        if len(data) == 768:
            pal[:] = np.frombuffer(data, np.uint8).reshape(256, 3)
        elif len(data) == 48:
            pal[slot:slot + 16] = np.frombuffer(data, np.uint8).reshape(16, 3)
    return np.clip(pal * 255 // 63, 0, 255).astype(np.uint8)


def cell(data, size):
    """Mode X planar cell: 4 planes x size rows x size/4 bytes; x = col*4 + plane."""
    return (np.frombuffer(data, np.uint8)
            .reshape(4, size, size // 4).transpose(1, 2, 0).reshape(size, size))


def cell_layout(w, h):
    """(size, x, y) in storage order for a w x h pixel frame."""
    out = []
    y = 0
    if h % 32:
        out += [(16, x, 0) for x in range(0, w, 16)]
        y = 16
    for band in range(y, h, 32):
        x = 0
        while x + 32 <= w:
            out.append((32, x, band))
            x += 32
        if x < w:
            out += [(16, x, band), (16, x, band + 16)]
    return out


def decode_bank(data, w8, h8):
    w, h = w8 * 8, h8 * 8
    size = w * h
    layout = cell_layout(w, h)
    frames = []
    for i in range(len(data) // size):
        f = data[i * size:(i + 1) * size]
        img = np.zeros((h, w), np.uint8)
        o = 0
        for s, x, y in layout:
            img[y:y + s, x:x + s] = cell(f[o:o + s * s], s)
            o += s * s
        frames.append(img)
    return frames


def sheet(frames, pal, base, cols=16):
    h, w = frames[0].shape
    rows = (len(frames) + cols - 1) // cols
    out = np.full((rows * (h + 1), cols * (w + 1), 4), 0, np.uint8)
    for i, f in enumerate(frames):
        r, c = divmod(i, cols)
        idx = f.astype(int) + (base if f.max() < 16 else 0)
        rgba = np.dstack([pal[idx], np.where(f == 0, 0, 255)]).astype(np.uint8)
        out[r * (h + 1):r * (h + 1) + h, c * (w + 1):c * (w + 1) + w] = rgba
    return Image.fromarray(out, "RGBA")


def main(src, dst):
    src, dst = Path(src), Path(dst)
    dst.mkdir(parents=True, exist_ok=True)
    for lv in LEVELS:
        resources, banks = parse_level(entry(src, lv + 1))
        pal = level_palette(src, resources)
        for res_id, w8, h8, flag in banks:
            data = entry(src, res_id)
            frames = decode_bank(data, w8, h8)
            # Palette base for 4-bit banks is not traced yet; 128 is Kyle's slot.
            sheet(frames, pal, 128).save(dst / f"L{lv}_bank{res_id - 1:03d}.png")
            print(f"level {lv} bank {res_id - 1:3d} {w8 * 8}x{h8 * 8} "
                  f"frames={len(frames)} rest={len(data) % (w8 * h8 * 64)} flag={flag}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
