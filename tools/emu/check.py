"""Accuracy check: plays the same input on the original (btemu.py) and on our
engine (bttrace) and reports where their data segments diverge.

Each run starts a new game, enters a level's password through the give-up
menu (P, Left, Enter, the way a player restarts from a password), then plays
random input for a while.

Usage: python check.py <BTHORNE dir> <bttrace.exe> <work dir> [--levels C0,C1,...]
       [--frames N] [--seed S]
"""
import argparse
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402

K = 0x10000
PASSWORDS = {}  # level -> 4 characters, read from the password table at ds:543Ah


def read_passwords(exe):
    d = open(exe, 'rb').read()
    base, e = 0xE740, 0x543A
    while True:
        level = struct.unpack_from('<H', d, base + e)[0]
        if level == 0xFFFF:
            return
        PASSWORDS.setdefault(level, d[base + e + 6:base + e + 10].decode('ascii'))
        e += 10


def schedule(level, frames, seed):
    """Start a game, restart at `level` through its password, then play."""
    r = random.Random(seed * 1000 + level)
    spec = ['400-403:1', '600-603:1']  # title: start a new game
    pokes = ['700:%X=%X' % (0x372 + 2 * k, ord(c)) for k, c in enumerate(PASSWORDS[level])]
    spec += ['705-706:4000', '712-713:10', '720-721:1']  # give up? -> yes
    moves = [0x20, 0x10, 0x220, 0x210, 0x100, 0x120, 0x110, 0x8, 0x4, 0, 0x40, 0x80, 0x400, 0x800, 0x1000,
             0x2000, 0x60, 0x50, 0x24, 0x14]
    t = 800
    while t < frames:
        n = r.randint(3, 40)
        bits = r.choice(moves)
        if bits:
            spec.append('%d-%d:%x' % (t, t + n - 1, bits))
        t += n + r.randint(0, 6)
    return ','.join(spec), ','.join(pokes)


def first_divergence(a, b):
    """First frame where a word other than known scratch differs, with the words."""
    n = min(len(a), len(b)) // K
    for t in range(n):
        fa, fb = a[t * K:(t + 1) * K], b[t * K:(t + 1) * K]
        if fa == fb:
            continue
        diffs = []
        for addr in range(0, 0x7110, 2):
            if fa[addr:addr + 2] == fb[addr:addr + 2] or compare.ignored(addr) or 0x10 <= addr < 0x1C:  # scratch
                continue
            va, vb = struct.unpack_from('<H', fa, addr)[0], struct.unpack_from('<H', fb, addr)[0]
            if compare.same_segment(fa, va, vb):
                continue
            diffs.append((addr, va, vb))
        if diffs:
            return t + 1, diffs
    return None, []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('bttrace')
    ap.add_argument('work')
    ap.add_argument('--levels', default='')
    ap.add_argument('--frames', type=int, default=3000)
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    read_passwords(os.path.join(args.game_dir, 'BTHORNE.EXE'))
    levels = [int(x, 16) for x in args.levels.split(',')] if args.levels else sorted(l for l in PASSWORDS if 0xC0 <= l <= 0xF0)
    data = os.path.join(args.game_dir, 'DATA.DAT')
    emu = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'btemu.py')
    failures = 0
    for level in levels:
        spec, pokes = schedule(level, args.frames, args.seed)
        ours = os.path.join(args.work, 'ours_%02X.bin' % level)
        theirs = os.path.join(args.work, 'orig_%02X.bin' % level)
        subprocess.run([args.bttrace, data, str(args.frames), ours, '--input', spec, '--poke', pokes], check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, emu, args.game_dir, str(args.frames), theirs, '--input', spec, '--poke', pokes],
                       check=True, stdout=subprocess.DEVNULL)
        a, b = open(theirs, 'rb').read(), open(ours, 'rb').read()
        reached = struct.unpack_from('<H', a, (min(len(a), len(b)) // K - 1) * K + 0x1CA4)[0]
        frame, diffs = first_divergence(a, b)
        if frame is None:
            print('level %02X: identical for %d frames (original ended on level %02X)' % (level, args.frames, reached))
        else:
            failures += 1
            shown = ' '.join('%s %04X/%04X' % (compare.name(x), va, vb) for x, va, vb in diffs[:8])
            print('level %02X: diverges at frame %d (%d words): %s' % (level, frame, len(diffs), shown))
        os.remove(theirs)
        os.remove(ours)
    print('%d of %d level(s) diverged' % (failures, len(levels)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
