"""Targeted accuracy scenarios: for every kind of object in a level, put
Kyle next to one and play at it, on the original (btemu.py) and on our
engine (bttrace), and report where their data segments diverge.

Each scenario restarts the level through its password (as check.py does),
then at frame 760 moves Kyle beside a spawn record of that actor type (the
same data segment pokes go to both engines) and plays a scripted mix of
drawing the gun, facing the object, firing, jumping, climbing and walking
into it. This reaches enemies, bosses, elevators, doors and pickups that
random play from the level start rarely gets to.

Usage: python scenarios.py <BTHORNE dir> <bttrace.exe> <work dir>
       [--levels C0,C1,...] [--frames N] [--jobs N] [--seed S]
"""
import argparse
import concurrent.futures
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402
import compare  # noqa: E402

K = 0x10000
TELEPORT = 760          # a few frames after the password restart has loaded the level
RECORDS = 0x1D24        # spawn records: x, y, half w, half h, type, flags, param (14 bytes)
KYLE = {'x': 0x15F2, 'y': 0x161A, 'left': 0x1412, 'right': 0x143A, 'top': 0x13C2, 'bottom': 0x13EA,
        'prev_x': 0x12D2, 'prev_y': 0x12FA}
DATA = 'DATA.DAT'


def word(frame, addr):
    return struct.unpack_from('<H', frame, addr)[0]


def run(args, cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def level_state(args, level):
    """Our engine's data segment at the teleport frame (identical to the original's, see check.py)."""
    spec, pokes = check.schedule(level, TELEPORT, args.seed)
    spec = ','.join(s for s in spec.split(',') if int(s.split('-')[0]) < TELEPORT)
    out = os.path.join(args.work, 'state_%X.bin' % level)
    run(args, [args.bttrace, os.path.join(args.game_dir, DATA), str(TELEPORT + 1), out, '--input', spec, '--poke', pokes])
    data = open(out, 'rb').read()
    os.remove(out)
    return data[TELEPORT * K:(TELEPORT + 1) * K], spec, pokes


def records(ds):
    out = []
    for r in range(256):
        a = RECORDS + r * 14
        x, y, hx, hy, kind, flags, param = struct.unpack_from('<7H', ds, a)
        if x == 0xFFFF:
            break
        if kind and kind < 0x56:
            out.append({'index': r, 'x': x, 'y': y, 'hx': hx, 'hy': hy, 'type': kind})
    return out


def scenario_input(rng, start, frames, face):
    """Draw the gun, then a weighted mix of firing, facing, moving, jumping and climbing."""
    spec = ['%d-%d:400' % (start + 4, start + 5)]
    t = start + 12
    actions = [(face | 0x40, 6), (0x40, 6), (face, 5), (face | 0x200, 3), (0x100, 3), (face | 0x100, 3),
               (0x4, 3), (0x8, 3), (0x80, 2), (0x800, 1), (0x2000, 1), (0x30 ^ face, 2), (0, 3)]
    total = sum(w for _, w in actions)
    while t < frames:
        pick = rng.uniform(0, total)
        for bits, w in actions:
            pick -= w
            if pick <= 0:
                break
        n = rng.randint(2, 25) if bits & 0x40 == 0 else rng.randint(1, 4)
        if bits:
            spec.append('%d-%d:%x' % (t, t + n - 1, bits))
        t += n + rng.randint(1, 8)
    return spec


def one(args, level, base_spec, base_pokes, ds, rec):
    face = 0x20 if rec['index'] % 2 == 0 else 0x10   # alternate approaching from the left and the right
    kx, ky = word(ds, KYLE['x']), word(ds, KYLE['y'])
    half_h = word(ds, KYLE['bottom']) - ky
    nx = (rec['x'] - 40) if face == 0x20 else (rec['x'] + 40)
    ny = rec['y'] + rec['hy'] - half_h           # stand on the object's floor
    nx, ny = max(nx, 16), max(ny, 16)
    dx, dy = (nx - kx) & 0xFFFF, (ny - ky) & 0xFFFF
    pokes = [base_pokes]
    for name, addr in KYLE.items():
        delta = dx if name in ('x', 'left', 'right', 'prev_x') else dy
        pokes.append('%d:%X=%X' % (TELEPORT, addr, (word(ds, addr) + delta) & 0xFFFF))
    rng = random.Random(args.seed * 7919 + level * 131 + rec['index'])
    spec = ','.join([base_spec] + scenario_input(rng, TELEPORT + 1, args.frames, face))
    pokes = ','.join(pokes)
    tag = '%X_%02X_%d' % (level, rec['type'], rec['index'])
    ours = os.path.join(args.work, 'ours_%s.bin' % tag)
    theirs = os.path.join(args.work, 'orig_%s.bin' % tag)
    try:
        run(args, [args.bttrace, os.path.join(args.game_dir, DATA), str(args.frames), ours, '--input', spec, '--poke', pokes])
        run(args, [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'btemu.py'), args.game_dir,
                   str(args.frames), theirs, '--input', spec, '--poke', pokes])
        a, b = open(theirs, 'rb').read(), open(ours, 'rb').read()
        frame, diffs = check.first_divergence(a, b)
        last = (min(len(a), len(b)) // K - 1) * K
        kyle_hp = struct.unpack_from('<h', a, last + 0x148A)[0]
        where = 'level %X type %02X record %d (Kyle hp %d at the end, now level %02X)' % (
            level, rec['type'], rec['index'], kyle_hp, word(a[last:last + K], 0x1CA4))
        if frame is None:
            return True, '%s: identical for %d frames' % (where, args.frames)
        shown = ' '.join('%s %04X/%04X' % (compare.name(x), va, vb) for x, va, vb in diffs[:6])
        return False, '%s: diverges at frame %d (%d words): %s\n    repro: --input "%s" --poke "%s"' % (
            where, frame, len(diffs), shown, spec, pokes)
    except subprocess.CalledProcessError as e:
        return False, 'level %X type %02X record %d: tool failed (%s)' % (level, rec['type'], rec['index'], e)
    finally:
        for f in (ours, theirs):
            if os.path.exists(f):
                os.remove(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('bttrace')
    ap.add_argument('work')
    ap.add_argument('--levels', default='')
    ap.add_argument('--frames', type=int, default=2300)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    check.read_passwords(os.path.join(args.game_dir, 'BTHORNE.EXE'))
    levels = [int(x, 16) for x in args.levels.split(',')] if args.levels else sorted(l for l in check.PASSWORDS if 0xC0 <= l <= 0xF0)
    jobs = []
    for level in levels:
        ds, spec, pokes = level_state(args, level)
        seen = set()
        for rec in records(ds):
            if rec['type'] in seen:
                continue
            seen.add(rec['type'])
            jobs.append((level, spec, pokes, ds, rec))
    print('%d scenarios over %d level(s)' % (len(jobs), len(levels)), flush=True)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for ok, line in pool.map(lambda j: one(args, *j), jobs):
            failures += not ok
            print(line, flush=True)
    print('%d of %d scenario(s) diverged' % (failures, len(jobs)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
