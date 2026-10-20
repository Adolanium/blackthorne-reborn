"""Unpack Blackthorne DATA.DAT.

Layout (observed): u32 count, u32 offsets[count]; each entry is
u32 unpacked_size followed by an LZSS stream. Bit 30 of an offset is a flag
whose meaning is still unknown; it is masked off and reported.
"""
import struct
import sys
from pathlib import Path


def lzss(src, out_size, n=4096):
    """Mirrors sub_10349: zeroed ring, write head at 0, LSB-first flags (1 = literal),
    reference word = 12-bit absolute ring offset | (length - 3) << 12."""
    ring = bytearray(n)
    r = 0
    out = bytearray()
    i = 0
    flags = 0
    while len(out) < out_size and i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xFF00
            i += 1
        if flags & 1:
            c = src[i]; i += 1
            out.append(c); ring[r] = c; r = (r + 1) & (n - 1)
        else:
            word = src[i] | src[i + 1] << 8; i += 2
            pos = word & 0xFFF
            length = (word >> 12) + 3
            for k in range(length):
                c = ring[(pos + k) & (n - 1)]
                out.append(c); ring[r] = c; r = (r + 1) & (n - 1)
    return bytes(out[:out_size]), i


def entries(data):
    count = struct.unpack_from("<I", data, 0)[0]
    raw = struct.unpack_from("<%dI" % count, data, 4)
    offs = [o & 0x3FFFFFFF for o in raw]
    ends = sorted(set(offs + [len(data)]))
    for idx, (r, o) in enumerate(zip(raw, offs)):
        end = ends[ends.index(o) + 1]
        yield idx, r >> 30, data[o:end]


def kind(b):
    if b[:4] == b"FORM": return "xmi"
    if b[:8] == b"Creative": return "voc"
    if b[2:11] == b"Copyright": return "txt"
    return "bin"


def main(dat, outdir):
    data = Path(dat).read_bytes()
    out = Path(outdir); out.mkdir(parents=True, exist_ok=True)
    bad = 0
    for idx, flag, blob in entries(data):
        size = struct.unpack_from("<I", blob, 0)[0]
        dec, used = lzss(blob[4:], size)
        ok = len(dec) == size
        bad += not ok
        k = kind(dec)
        (out / f"{idx:03d}.{k}").write_bytes(dec)
        print(f"{idx:3d} flag={flag} packed={len(blob):6d} size={size:6d} "
              f"consumed={used + 4:6d} {k} {'OK' if ok else 'SHORT'}")
    print("entries with short output:", bad, file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
