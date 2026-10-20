"""Compares the original's screens (btemu.py --shots, <prefix>.<frame>.png)
with ours (bttrace --shots, <prefix>.<frame>.ppm) and writes a side-by-side
image per frame: original | ours | differences in red.

Usage: python screens.py <original prefix> <our prefix> <frames> [<out dir>]
"""
import os
import sys

from PIL import Image, ImageChops


def compare(orig_path, ours_path, out_path=None):
    a = Image.open(orig_path).convert('RGB')
    b = Image.open(ours_path).convert('RGB')
    if a.size != b.size:
        return None, 'sizes differ: %s vs %s' % (a.size, b.size)
    diff = ImageChops.difference(a, b).convert('L').point(lambda v: 255 if v else 0)
    changed = sum(1 for v in diff.getdata() if v)
    if out_path:
        w, h = a.size
        side = Image.new('RGB', (w * 3, h))
        side.paste(a, (0, 0))
        side.paste(b, (w, 0))
        red = Image.new('RGB', (w, h), (255, 0, 0))
        side.paste(Image.composite(red, a.point(lambda v: v // 3), diff), (w * 2, 0))
        side.save(out_path)
    return changed / (a.size[0] * a.size[1]), ''


def main():
    orig, ours, frames = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(',')]
    out_dir = sys.argv[4] if len(sys.argv) > 4 else None
    worst = 0.0
    for f in frames:
        out = os.path.join(out_dir, 'screen_%d.png' % f) if out_dir else None
        ratio, note = compare('%s.%d.png' % (orig, f), '%s.%d.ppm' % (ours, f), out)
        if ratio is None:
            print('frame %d: %s' % (f, note))
            continue
        worst = max(worst, ratio)
        print('frame %d: %.2f%% of pixels differ' % (f, ratio * 100))
    return 0 if worst == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
