"""Headless play test of the real game executable.

Runs blackthorne.exe in its headless mode (--shot) for every level: same
ticks, events (sound, music, saving, quitting), rendering and presentation
as an interactive session, with scripted input for thousands of frames. A
run passes when the game exits cleanly and the last frame is not blank.
Also plays from boot through the title into the first level.

Usage: python playtest.py <blackthorne.exe> <BTHORNE dir> <work dir> [--frames N]
Your savegame.000 is backed up and restored around the runs.
"""
import argparse
import os
import random
import shutil
import struct
import subprocess
import sys

from PIL import Image


def passwords(exe):
    d = open(exe, 'rb').read()
    base, e, out = 0xE740, 0x543A, {}
    while True:
        level = struct.unpack_from('<H', d, base + e)[0]
        if level == 0xFFFF:
            return out
        out.setdefault(level, d[base + e + 6:base + e + 10].decode('ascii'))
        e += 10


def schedule(seed, start, frames):
    r = random.Random(seed)
    moves = [0x20, 0x10, 0x220, 0x210, 0x100, 0x120, 0x110, 0x8, 0x4, 0, 0x40, 0x80, 0x400, 0x800, 0x1000, 0x2000, 0x60, 0x50]
    spec, t = [], start
    while t < frames:
        n = r.randint(3, 40)
        bits = r.choice(moves)
        if bits:
            spec.append('%d-%d:%x' % (t, t + n - 1, bits))
        t += n + r.randint(0, 6)
    return spec


def run(game, args, extra, name):
    shot = os.path.join(args.work, name + '.bmp')
    if os.path.exists(shot):
        os.remove(shot)
    cmd = [game, os.path.join(args.data_dir, 'DATA.DAT'), '--ticks', str(args.frames), '--size', '640x480', '--shot', shot] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return False, 'exit code %d %s' % (r.returncode, r.stderr.strip()[:200])
    if not os.path.exists(shot):
        return False, 'no frame saved'
    colours = Image.open(shot).convert('RGB').getcolors(1 << 20)
    if not colours or len(colours) < 8:
        return False, 'last frame is (nearly) blank'
    return True, '%d colours in the last frame' % len(colours)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game')
    ap.add_argument('data_dir')
    ap.add_argument('work')
    ap.add_argument('--frames', type=int, default=4000)
    args = ap.parse_args()
    save = os.path.join(os.environ.get('APPDATA', ''), 'Adolanium', 'BlackthorneReborn', 'savegame.000')
    backup = save + '.playtest-backup'
    if os.path.exists(save):
        shutil.copy2(save, backup)
    failures = 0
    try:
        ok, note = run(args.game, args, ['--input', ','.join(['400-403:1', '600-603:1'] + schedule(1, 800, args.frames))], 'boot')
        failures += not ok
        print('boot -> title -> new game: %s (%s)' % ('ok' if ok else 'FAILED', note), flush=True)
        for level in sorted(passwords(os.path.join(args.data_dir, 'BTHORNE.EXE'))):
            ok, note = run(args.game, args, ['--level', '%X' % level, '--input', ','.join(schedule(level, 30, args.frames))],
                           'level_%X' % level)
            failures += not ok
            print('level %X: %s (%s)' % (level, 'ok' if ok else 'FAILED', note), flush=True)
    finally:
        if os.path.exists(backup):
            shutil.move(backup, save)
        elif os.path.exists(save):
            os.remove(save)
    print('%d run(s) failed' % failures)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
