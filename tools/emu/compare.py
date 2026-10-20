"""Compares two data segment traces (btemu.py for the original, bttrace for
ours) frame by frame and names what differs.

Usage: python compare.py <original.bin> <ours.bin> [--frames a-b] [--show N]
"""
import argparse
import struct
from collections import Counter, defaultdict

K = 0x10000

# Regions that legitimately differ: things the original keeps for hardware
# or memory management that our engine does not model.
IGNORE = [
    (0x2400, 0x2610, 'segments, file and driver state'),
    (0x292E, 0x2938, 'DATA.DAT header, file handle, saved int 24h vector'),
    (0x2A86, 0x2C08, 'keyboard state table'),
    (0x3508, 0x4308, 'dirty tile maps of the two pages (sub_15A55)'),
    (0x4308, 0x4A08, 'text layer dirty map'),
    (0x5012, 0x5312, 'palette upload buffer'),
    (0x5316, 0x5400, 'CRTC and page row tables'),
    (0x5C26, 0x60A6, 'HUD graphics buffers'),
    (0x1172, 0x1192, 'sprite bank handles (EMS handles in the original)'),
    (0x4A08, 0x4A10, 'page addresses, video mode'),
    (0x5312, 0x5315, 'retrace counter'),
    (0x6A50, 0x6AC0, 'EMS bookkeeping'),
    (0x7110, 0x10000, 'stack and heap above the initialised data'),
]


def name(addr):
    """A readable name for a data segment address."""
    if 0x13C2 <= addr < 0x13C2 + 0x28 * 0x80:
        k, slot = divmod(addr - 0x13C2, 0x28)
        return 'actor field %02X slot %d' % (k * 2, slot // 2)
    if 0x452 <= addr < 0x452 + 0x100 * 0x10:
        k, p = divmod(addr - 0x452, 0x100)
        return 'part array %X52 part %d' % (4 + k, p // 2)
    if 0x4A12 <= addr < 0x4D12:
        return 'base palette %d' % ((addr - 0x4A12) // 3)
    if 0x4D12 <= addr < 0x5012:
        return 'shown palette %d' % ((addr - 0x4D12) // 3)
    return 'ds:%04X' % addr


# Parts 0 and 1 double as the HUD drawer's stamp (sub_14304): their position
# and colour are scratch, not game state.
HUD_STAMP = {base + p for base in (0x552, 0x652, 0xF52) for p in (0, 2)}


def ignored(addr):
    return addr in HUD_STAMP or any(a <= addr < b for a, b, _ in IGNORE)


# Segment registers the original allocates at run time, and the constants our
# engine uses instead (machine.h, vm.h): script data (ds:259Ah), actor frames
# (ds:259Eh), static sprites (ds:25A0h), tile animation frames (ds:2594h). A segment value matches when it sits
# at the same distance from its base.
SEGMENT_BASES = ((0x259A, 0x4000), (0x259E, 0x5000), (0x25A0, 0x6000), (0x2594, 0x8000))


def same_segment(frame_a, va, vb):
    for addr, ours in SEGMENT_BASES:
        theirs = struct.unpack_from('<H', frame_a, addr)[0]
        if theirs and (va - theirs) == (vb - ours) and abs(va - theirs) <= 0x1000:
            return True
    return va == 0x1E54 and vb == 0x7000  # the data segment itself


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('original')
    ap.add_argument('ours')
    ap.add_argument('--frames', default='')
    ap.add_argument('--show', type=int, default=30)
    ap.add_argument('--constant', action='store_true', help='also list words that differ but never change')
    args = ap.parse_args()
    a = open(args.original, 'rb').read()
    b = open(args.ours, 'rb').read()
    n = min(len(a), len(b)) // K
    lo, hi = 0, n
    if args.frames:
        x, y = args.frames.split('-')
        lo, hi = int(x) - 1, min(n, int(y))
    first_seen = {}
    counts = Counter()
    values = defaultdict(set)  # address -> distinct (original, ours) pairs
    for t in range(lo, hi):
        fa, fb = a[t * K:(t + 1) * K], b[t * K:(t + 1) * K]
        if fa == fb:
            continue
        for addr in range(0, K, 2):
            if fa[addr:addr + 2] == fb[addr:addr + 2] or ignored(addr):
                continue
            va = struct.unpack_from('<H', fa, addr)[0]
            vb = struct.unpack_from('<H', fb, addr)[0]
            if same_segment(fa, va, vb):
                continue
            counts[addr] += 1
            values[addr].add((va, vb))
            if addr not in first_seen:
                first_seen[addr] = (t + 1, va, vb)
    # A word whose two values never change is a table or setting our engine
    # does not keep; words that move are behaviour differences.
    constant = [x for x in first_seen if len(values[x]) == 1]
    moving = [x for x in first_seen if len(values[x]) > 1]
    print('frames %d-%d: %d differing words that change over time, %d constant' % (lo + 1, hi, len(moving), len(constant)))
    for addr in sorted(moving, key=lambda x: (first_seen[x][0], x))[:args.show]:
        t, va, vb = first_seen[addr]
        print('  frame %4d  %-28s original %04X  ours %04X  (%d frames, %d value pairs)' % (t, name(addr), va, vb, counts[addr], len(values[addr])))
    if args.constant:
        for addr in sorted(constant):
            t, va, vb = first_seen[addr]
            print('  constant   %-28s original %04X  ours %04X' % (name(addr), va, vb))


if __name__ == '__main__':
    main()
