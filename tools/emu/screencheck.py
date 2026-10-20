"""Screen accuracy over every level: the check.py schedule (restart at the
level through its password, then random play) on both engines, comparing
what the original displays (btemu.py --shots, VGA model) with our render
of the frame before (see docs/ACCURACY.md) every few hundred frames.

Usage: python screencheck.py <BTHORNE dir> <bttrace.exe> <work dir>
       [--levels C0,...] [--frames N] [--every N] [--jobs N]
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys

from PIL import Image, ImageChops

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402

# The original's bottom bar shares video memory with a page: its last line
# from x=216 picks up stray bytes (a known cosmetic difference).
KNOWN = (216, 239, 320, 240)


def differing(a, b, out=None):
    a, b = a.convert('RGB'), b.convert('RGB')
    diff = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v else 0)
    diff.paste(0, KNOWN)
    n = sum(1 for v in diff.getdata() if v)
    if n and out:
        w, h = a.size
        side = Image.new('RGB', (w * 3, h))
        side.paste(a, (0, 0))
        side.paste(b, (w, 0))
        side.paste(Image.composite(Image.new('RGB', a.size, (255, 0, 0)), a.point(lambda v: v // 3), diff), (w * 2, 0))
        side.save(out)
    return n


def one(args, level):
    spec, pokes = check.schedule(level, args.frames, 1)
    frames = list(range(args.every, args.frames + 1, args.every))
    ours = os.path.join(args.work, 'sc_ours_%X.bin' % level)
    theirs = os.path.join(args.work, 'sc_orig_%X.bin' % level)
    data = os.path.join(args.game_dir, 'DATA.DAT')
    subprocess.run([args.bttrace, data, str(args.frames), ours, '--input', spec, '--poke', pokes,
                    '--shots', ','.join(str(f - 1) for f in frames)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'btemu.py'), args.game_dir,
                    str(args.frames), theirs, '--input', spec, '--poke', pokes, '--shots', ','.join(map(str, frames))],
                   check=True, stdout=subprocess.DEVNULL)
    worst = []
    for f in frames:
        a, b = '%s.%d.png' % (theirs, f), '%s.%d.ppm' % (ours, f - 1)
        if not (os.path.exists(a) and os.path.exists(b)):
            continue
        n = differing(Image.open(a), Image.open(b), os.path.join(args.work, 'sc_%X_%d.png' % (level, f)))
        if n:
            worst.append((f, n))
        os.remove(a)
        os.remove(b)
    for f in (ours, theirs):
        if os.path.exists(f):
            os.remove(f)
    if not worst:
        return True, 'level %X: %d screens identical' % (level, len(frames))
    return False, 'level %X: %d of %d screens differ: %s' % (level, len(worst), len(frames),
                                                         ', '.join('frame %d (%d px)' % fw for fw in worst[:8]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('bttrace')
    ap.add_argument('work')
    ap.add_argument('--levels', default='')
    ap.add_argument('--frames', type=int, default=3000)
    ap.add_argument('--every', type=int, default=250)
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    check.read_passwords(os.path.join(args.game_dir, 'BTHORNE.EXE'))
    levels = [int(x, 16) for x in args.levels.split(',')] if args.levels else sorted(l for l in check.PASSWORDS if 0xC0 <= l <= 0xF0)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for ok, line in pool.map(lambda l: one(args, l), levels):
            failures += not ok
            print(line, flush=True)
    print('%d of %d level(s) with differing screens' % (failures, len(levels)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
