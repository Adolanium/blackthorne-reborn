"""Writes a minimal SoundFont 2 file for tests: one looping sine-wave
instrument used by preset 0 in bank 0 (melodic) and bank 128 (drums).

Usage: python make_test_sf2.py out.sf2
"""
import math
import struct
import sys


def chunk(tag, data):
    pad = b'\0' if len(data) % 2 else b''
    return tag + struct.pack('<I', len(data)) + data + pad


def riff_list(kind, chunks):
    body = kind + b''.join(chunks)
    return b'LIST' + struct.pack('<I', len(body)) + body


def name20(s):
    return s.encode('ascii')[:20].ljust(20, b'\0')


def main(path):
    rate, n = 44100, 4410  # 100 samples per period: 441 Hz, close to A4
    samples = [int(12000 * math.sin(2 * math.pi * i / 100)) for i in range(n)]
    smpl = struct.pack('<%dh' % n, *samples) + b'\0' * 92  # 46 zero samples after each sample

    info = riff_list(b'INFO', [
        chunk(b'ifil', struct.pack('<HH', 2, 1)),
        chunk(b'isng', b'EMU8000\0'),
        chunk(b'INAM', b'Test sine\0'),
    ])
    sdta = riff_list(b'sdta', [chunk(b'smpl', smpl)])

    phdr = b''.join([
        name20('Sine') + struct.pack('<HHHIII', 0, 0, 0, 0, 0, 0),
        name20('Sine drums') + struct.pack('<HHHIII', 0, 128, 1, 0, 0, 0),
        name20('EOP') + struct.pack('<HHHIII', 0, 0, 2, 0, 0, 0),
    ])
    pbag = struct.pack('<HHHHHH', 0, 0, 1, 0, 2, 0)
    pmod = b'\0' * 10
    pgen = struct.pack('<HHHHHH', 41, 0, 41, 0, 0, 0)  # instrument 0
    inst = name20('Sine') + struct.pack('<H', 0) + name20('EOI') + struct.pack('<H', 1)
    ibag = struct.pack('<HHHH', 0, 0, 2, 0)
    imod = b'\0' * 10
    igen = struct.pack('<HHHHHH', 54, 1, 53, 0, 0, 0)  # loop continuously, sample 0
    shdr = (name20('Sine') + struct.pack('<IIIIIBbHH', 0, n, 0, n, rate, 69, 0, 0, 1) +
            name20('EOS') + struct.pack('<IIIIIBbHH', 0, 0, 0, 0, 0, 0, 0, 0, 0))
    pdta = riff_list(b'pdta', [chunk(b'phdr', phdr), chunk(b'pbag', pbag), chunk(b'pmod', pmod),
                               chunk(b'pgen', pgen), chunk(b'inst', inst), chunk(b'ibag', ibag),
                               chunk(b'imod', imod), chunk(b'igen', igen), chunk(b'shdr', shdr)])
    body = b'sfbk' + info + sdta + pdta
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


if __name__ == '__main__':
    main(sys.argv[1])
