"""Headless runner for the original BTHORNE.EXE, for accuracy checks.

Runs the DOS game in the Unicorn CPU emulator with just enough DOS, BIOS and
hardware to play it: no window, no sound. Every frame boundary (the game
spinning in sub_10113 for the retrace counter ds:5312h) is one tick: the data
segment is recorded, the next input word is written to ds:2A82h (the same
word the keyboard ISR and our Game::set_input fill) and the wait is released.

Usage:
  python btemu.py <BTHORNE dir> <ticks> <out.bin> [--input "from-to:hexbits,..."]
                  [--poke "frame:hexaddr=hexvalue,..."]

--poke writes data segment words at the boundary after the given frame (as
bttrace does before the next tick), e.g. to set a password.

out.bin holds one 64 KB data segment per tick. Requires `pip install unicorn`.
"""
import argparse
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR, UC_HOOK_INSN, UC_HOOK_CODE
from unicorn.x86_const import *

LOAD_SEG = 0x1000          # matches the IDA database, so seg000 offsets are linear - 0x10000
PSP_SEG = LOAD_SEG - 0x10
ENV_SEG = 0x0F00
MEM_TOP = 0xA000           # conventional memory ends where VGA memory starts
FRAME_WAIT = 0x10113       # sub_10113: cmp ds:5312h, 0 / jg sub_10113
EMS_FRAME = 0xE000
EMS_PAGES = 512            # 8 MB of expanded memory


def parse_pokes(spec):
    """'frame:addr=value,...' (hex address and value) -> {frame: [(addr, value)]}."""
    out = {}
    for part in filter(None, (spec or '').split(',')):
        frame, rest = part.split(':')
        addr, value = rest.split('=')
        out.setdefault(int(frame), []).append((int(addr, 16), int(value, 16)))
    return out


def parse_input(spec):
    """'from-to:hexbits,...' -> list of (from, to, bits), ticks inclusive."""
    out = []
    for part in filter(None, (spec or '').split(',')):
        rng, bits = part.split(':')
        a, b = rng.split('-')
        out.append((int(a), int(b), int(bits, 16)))
    return out


class Machine:
    def __init__(self, game_dir, scratch):
        self.game_dir = game_dir
        self.scratch = scratch
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x110000)
        self.files = {}
        self.next_handle = 5
        self.exited = None
        self.mem_free = MEM_TOP      # first free paragraph above the program block
        self.prog_end = MEM_TOP
        self.pit = 0xFFFF
        self.retrace = 0
        self.ems_store = bytearray(EMS_PAGES * 0x4000)
        self.ems_map = [None] * 4    # logical page mapped in each physical page
        self.ems_next = 0
        self.ems_handles = {}
        self.log = []
        self.vga = None  # enable_vga() swaps video memory for the VGA model
        self.load_exe(os.path.join(game_dir, 'BTHORNE.EXE'))
        self.uc.hook_add(UC_HOOK_INTR, self.on_int)
        self.uc.hook_add(UC_HOOK_INSN, self.on_in, None, 1, 0, UC_X86_INS_IN)
        self.uc.hook_add(UC_HOOK_INSN, self.on_out, None, 1, 0, UC_X86_INS_OUT)

    # --- loading ---------------------------------------------------------
    def load_exe(self, path):
        data = open(path, 'rb').read()
        (magic, cblp, cp, crlc, cparhdr, minalloc, maxalloc, ss, sp, csum, ip, cs, lfarlc) = struct.unpack_from('<13H', data)
        assert magic == 0x5A4D
        size = cp * 512 - (512 - cblp if cblp else 0)
        image = data[cparhdr * 16:size]
        self.uc.mem_write(LOAD_SEG * 16, image)
        for i in range(crlc):
            off, seg = struct.unpack_from('<HH', data, lfarlc + 4 * i)
            addr = (seg + LOAD_SEG) * 16 + off
            v = struct.unpack('<H', self.uc.mem_read(addr, 2))[0]
            self.uc.mem_write(addr, struct.pack('<H', (v + LOAD_SEG) & 0xFFFF))
        # PSP: int 20h, top of memory, environment, empty command tail.
        psp = bytearray(256)
        psp[0:2] = b'\xCD\x20'
        struct.pack_into('<H', psp, 2, MEM_TOP)
        struct.pack_into('<H', psp, 0x2C, ENV_SEG)
        psp[0x80] = 0
        psp[0x81] = 0x0D
        self.uc.mem_write(PSP_SEG * 16, bytes(psp))
        self.uc.mem_write(ENV_SEG * 16, b'PATH=C:\\\0\0\x01\0C:\\BTHORNE.EXE\0')
        # The program owns everything up to the top of memory until it shrinks.
        self.prog_end = MEM_TOP
        for reg, v in ((UC_X86_REG_CS, cs + LOAD_SEG), (UC_X86_REG_SS, ss + LOAD_SEG), (UC_X86_REG_SP, sp),
                       (UC_X86_REG_DS, PSP_SEG), (UC_X86_REG_ES, PSP_SEG), (UC_X86_REG_IP, ip)):
            self.uc.reg_write(reg, v)
        self.entry = ((cs + LOAD_SEG) << 4) + ip
        # Interrupt vectors: point everything at an IRET in the BIOS area.
        self.uc.mem_write(0xF0000, b'\xCF')
        self.uc.mem_write(0xF000A, b'EMMXXXX0')  # EMS driver signature, found via the int 67h vector
        # BIOS data area: equipment, memory size, video mode 3, CRTC at 3D4h.
        bda = bytearray(256)
        struct.pack_into('<H', bda, 0x10, 0x0026)
        struct.pack_into('<H', bda, 0x13, 640)
        bda[0x49] = 3
        struct.pack_into('<H', bda, 0x4A, 80)
        struct.pack_into('<H', bda, 0x63, 0x3D4)
        bda[0x84] = 24
        self.uc.mem_write(0x400, bytes(bda))
        self.uc.mem_write(0, struct.pack('<H', 0) * 0 + b''.join(struct.pack('<HH', 0, 0xF000) for _ in range(256)))

    # --- helpers ---------------------------------------------------------
    def reg(self, r):
        return self.uc.reg_read(r)

    def set(self, r, v):
        self.uc.reg_write(r, v & 0xFFFF)

    def lin(self, seg, off):
        return (seg << 4) + off

    def cstr(self, seg, off):
        b = bytes(self.uc.mem_read(self.lin(seg, off), 128))
        return b.split(b'\0')[0].decode('latin-1')

    def carry(self, on):
        f = self.reg(UC_X86_REG_EFLAGS)
        self.uc.reg_write(UC_X86_REG_EFLAGS, (f | 1) if on else (f & ~1))

    def zero(self, on):
        f = self.reg(UC_X86_REG_EFLAGS)
        self.uc.reg_write(UC_X86_REG_EFLAGS, (f | 0x40) if on else (f & ~0x40))

    def fail(self, code):
        self.set(UC_X86_REG_AX, code)
        self.carry(True)

    def host_path(self, name):
        base = name.replace('/', '\\').split('\\')[-1].lower()
        if base == 'data.dat':
            return os.path.join(self.game_dir, 'DATA.DAT')
        return os.path.join(self.scratch, base)

    # --- interrupts ------------------------------------------------------
    def on_int(self, uc, intno, _):
        try:
            handler = getattr(self, 'int_%02x' % intno, None)
            if handler:
                handler()
            else:
                self.log.append('int %02Xh ax=%04X ignored' % (intno, self.reg(UC_X86_REG_AX)))
        except Exception as e:  # keep the emulator alive, but remember
            self.log.append('int %02Xh failed: %r' % (intno, e))
            uc.emu_stop()

    def int_21(self):
        ax = self.reg(UC_X86_REG_AX)
        ah, al = ax >> 8, ax & 0xFF
        ds, dx, bx, cx = self.reg(UC_X86_REG_DS), self.reg(UC_X86_REG_DX), self.reg(UC_X86_REG_BX), self.reg(UC_X86_REG_CX)
        if ah == 0x09:
            s = bytes(self.uc.mem_read(self.lin(ds, dx), 256)).split(b'$')[0]
            self.log.append('print: ' + s.decode('latin-1').strip())
        elif ah == 0x4C or ah == 0x00:
            self.exited = al
            self.uc.emu_stop()
        elif ah == 0x30:
            self.set(UC_X86_REG_AX, 0x0005)  # DOS 5.0
        elif ah == 0x25:
            self.uc.mem_write(al * 4, struct.pack('<HH', dx, ds))
        elif ah == 0x35:
            off, seg = struct.unpack('<HH', self.uc.mem_read(al * 4, 4))
            self.set(UC_X86_REG_BX, off)
            self.set(UC_X86_REG_ES, seg)
        elif ah == 0x4A:  # resize the program block (ES = PSP)
            self.prog_end = self.reg(UC_X86_REG_ES) + bx
            self.mem_free = max(self.mem_free if self.mem_free < MEM_TOP else 0, self.prog_end)
            self.carry(False)
        elif ah == 0x48:
            start = max(self.mem_free, self.prog_end) if self.mem_free < MEM_TOP else self.prog_end
            avail = MEM_TOP - start
            if bx > avail:
                self.set(UC_X86_REG_BX, avail)
                self.fail(8)
            else:
                self.set(UC_X86_REG_AX, start)
                self.mem_free = start + bx
                self.carry(False)
        elif ah == 0x49:
            self.carry(False)
        elif ah in (0x3D, 0x3C):
            name = self.cstr(ds, dx)
            path = self.host_path(name)
            try:
                f = open(path, 'r+b' if ah == 0x3D and (al & 3) else ('rb' if ah == 0x3D else 'w+b'))
            except OSError:
                self.fail(2)
                return
            h = self.next_handle
            self.next_handle += 1
            self.files[h] = f
            self.set(UC_X86_REG_AX, h)
            self.carry(False)
        elif ah == 0x3E:
            f = self.files.pop(bx, None)
            if f:
                f.close()
            self.carry(False)
        elif ah == 0x3F:
            f = self.files.get(bx)
            if not f:
                self.fail(6)
                return
            data = bytearray(f.read(cx))
            if dx == 0x25E6 and len(data) >= 0x18:
                # The SETUP block (sub_10D0D): no digital sound card (25ECh)
                # and no music card (25FCh), so no sound hardware is needed.
                self.log.append('setup block: digital %04X, music %04X -> none' %
                                (struct.unpack_from('<H', data, 6)[0], struct.unpack_from('<H', data, 0x16)[0]))
                struct.pack_into('<H', data, 6, 0xFFFF)
                struct.pack_into('<H', data, 0x16, 0xFFFF)
            self.uc.mem_write(self.lin(ds, dx), bytes(data))
            self.set(UC_X86_REG_AX, len(data))
            self.carry(False)
        elif ah == 0x40:
            f = self.files.get(bx)
            if bx in (1, 2):
                self.set(UC_X86_REG_AX, cx)
                self.carry(False)
                return
            if not f:
                self.fail(6)
                return
            f.write(bytes(self.uc.mem_read(self.lin(ds, dx), cx)))
            self.set(UC_X86_REG_AX, cx)
            self.carry(False)
        elif ah == 0x42:
            f = self.files.get(bx)
            if not f:
                self.fail(6)
                return
            off = (cx << 16) | dx
            if off & 0x80000000:
                off -= 1 << 32
            f.seek(off, al)
            pos = f.tell()
            self.set(UC_X86_REG_AX, pos & 0xFFFF)
            self.set(UC_X86_REG_DX, pos >> 16)
            self.carry(False)
        elif ah == 0x2C:  # time: fixed, for determinism
            self.set(UC_X86_REG_CX, 0x0C00)
            self.set(UC_X86_REG_DX, 0)
        elif ah == 0x2A:
            self.set(UC_X86_REG_CX, 1994)
            self.set(UC_X86_REG_DX, 0x0101)
        else:
            self.log.append('int 21h ah=%02X ignored' % ah)
            self.carry(False)

    def int_10(self):
        ax = self.reg(UC_X86_REG_AX)
        if ax == 0x1A00:
            self.set(UC_X86_REG_AX, 0x001A)
            self.set(UC_X86_REG_BX, 0x0008)  # VGA with colour display
        elif ax >> 8 == 0x0F:
            self.set(UC_X86_REG_AX, 0x5003)
            self.set(UC_X86_REG_BX, 0)

    def int_11(self):
        self.set(UC_X86_REG_AX, 0x0026)

    def int_12(self):
        self.set(UC_X86_REG_AX, 640)

    def int_1a(self):
        self.set(UC_X86_REG_CX, 0)
        self.set(UC_X86_REG_DX, 0)
        self.set(UC_X86_REG_AX, self.reg(UC_X86_REG_AX) & 0xFF00)

    def int_67(self):
        ah = self.reg(UC_X86_REG_AX) >> 8
        al = self.reg(UC_X86_REG_AX) & 0xFF
        bx, dx = self.reg(UC_X86_REG_BX), self.reg(UC_X86_REG_DX)
        ok = lambda: self.set(UC_X86_REG_AX, al)  # AH = 0: success
        if ah == 0x40:
            ok()
        elif ah == 0x41:
            ok()
            self.set(UC_X86_REG_BX, EMS_FRAME)
        elif ah == 0x42:
            ok()
            self.set(UC_X86_REG_BX, EMS_PAGES - self.ems_next)
            self.set(UC_X86_REG_DX, EMS_PAGES)
        elif ah == 0x43:
            if bx > EMS_PAGES - self.ems_next:
                self.set(UC_X86_REG_AX, 0x8800)
                return
            h = len(self.ems_handles) + 1
            self.ems_handles[h] = (self.ems_next, bx)
            self.ems_next += bx
            ok()
            self.set(UC_X86_REG_DX, h)
        elif ah == 0x44:  # map logical page BX of handle DX into physical page AL
            self.ems_flush(al)
            if bx == 0xFFFF:
                self.ems_map[al] = None
            else:
                first, count = self.ems_handles[dx]
                page = first + bx
                self.ems_map[al] = page
                self.uc.mem_write(EMS_FRAME * 16 + al * 0x4000, bytes(self.ems_store[page * 0x4000:(page + 1) * 0x4000]))
            ok()
        elif ah == 0x45:
            ok()
        elif ah == 0x46:
            self.set(UC_X86_REG_AX, 0x0040)
        elif ah == 0x4B:
            ok()
            self.set(UC_X86_REG_BX, len(self.ems_handles))
        elif ah == 0x4C:
            ok()
            self.set(UC_X86_REG_BX, self.ems_handles.get(dx, (0, 0))[1])
        else:
            self.log.append('int 67h ah=%02X ignored' % ah)
            ok()

    def ems_flush(self, phys):
        page = self.ems_map[phys]
        if page is not None:
            self.ems_store[page * 0x4000:(page + 1) * 0x4000] = self.uc.mem_read(EMS_FRAME * 16 + phys * 0x4000, 0x4000)

    # --- ports -----------------------------------------------------------
    def on_in(self, uc, port, size, _):
        if port == 0x3DA:
            if self.vga:
                self.vga.reset_flipflop()
            self.retrace ^= 9
            return self.retrace
        if self.vga:
            v = self.vga.port_in(port)
            if v is not None:
                return v
        if port == 0x40:
            self.pit = (self.pit - 0x40) & 0xFFFF
            return (self.pit >> 8) & 0xFF if getattr(self, '_pit_hi', False) else self.pit & 0xFF
        if port == 0x201:
            self.pit = (self.pit - 8) & 0xFFFF
            return 0xFF
        if port == 0x60:
            return 0
        return 0xFF if size == 1 else 0xFFFF

    def on_out(self, uc, port, size, value, _):
        if self.vga:
            self.vga.port_out(port, size, value)

    def enable_vga(self):
        from vga import Vga
        self.vga = Vga(self.uc)

    # --- running ---------------------------------------------------------
    def ds_bytes(self):
        return bytes(self.uc.mem_read(self.reg(UC_X86_REG_DS) << 4, 0x10000))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('ticks', type=int)
    ap.add_argument('out')
    ap.add_argument('--input', default='')
    ap.add_argument('--poke', default='')
    ap.add_argument('--map', action='store_true', help='also write the map segment per tick to <out>.map')
    ap.add_argument('--savegame', default='', help='4-character password to put in savegame.000')
    ap.add_argument('--budget', type=int, default=20_000_000, help='instructions allowed per tick')
    ap.add_argument('--shots', default='', help='frames to save as <out>.<frame>.png (emulates the VGA; slower)')
    ap.add_argument('--sounds', default='', help='write "frame id" for every sound request (sub_11D2E), and '
                    '"frame start" / "frame stop" for the music (sub_197B4, sub_19566), to this file')
    args = ap.parse_args()
    schedule = parse_input(args.input)
    pokes = parse_pokes(args.poke)
    scratch = tempfile.mkdtemp(prefix='btemu_')
    if args.savegame:
        open(os.path.join(scratch, 'savegame.000'), 'wb').write(args.savegame.encode('ascii')[:4])
    m = Machine(args.game_dir, scratch)
    shots = {int(x) for x in args.shots.split(',') if x}
    if shots:
        m.enable_vga()
    out = open(args.out, 'wb')
    map_out = open(args.out + '.map', 'wb') if args.map else None
    state = {'tick': 0}

    def on_wait(uc, address, size, _):
        ds = uc.reg_read(UC_X86_REG_DS)
        wait = struct.unpack('<h', uc.mem_read((ds << 4) + 0x5312, 2))[0]
        if wait <= 0:
            return
        out.write(m.ds_bytes())
        if state['tick'] + 1 in shots:
            m.vga.save_png('%s.%d.png' % (args.out, state['tick'] + 1))
        if map_out:
            seg = struct.unpack('<H', uc.mem_read((ds << 4) + 0x259C, 2))[0]
            map_out.write(bytes(uc.mem_read(seg << 4, 0x10000)))
        state['tick'] += 1
        t = state['tick']
        bits = 0
        for a, b, v in schedule:
            if a <= t <= b:
                bits |= v
        uc.mem_write((ds << 4) + 0x2A82, struct.pack('<H', bits))
        for addr, value in pokes.get(t, []):
            uc.mem_write((ds << 4) + addr, struct.pack('<H', value))
        uc.mem_write((ds << 4) + 0x5312, b'\0\0')
        if t >= args.ticks:
            uc.emu_stop()

    m.uc.hook_add(UC_HOOK_CODE, on_wait, None, FRAME_WAIT, FRAME_WAIT)
    sounds = open(args.sounds, 'w') if args.sounds else None
    if sounds:
        def on_sound(uc, address, size, _):  # sub_11D2E: AX = sound id
            sounds.write('%d %02X%s' % (state['tick'] + 1, uc.reg_read(UC_X86_REG_AX) & 0xFF, chr(10)))
        m.uc.hook_add(UC_HOOK_CODE, on_sound, None, 0x11D2E, 0x11D2E)

        def on_music(what):
            def hook(uc, address, size, _):
                sounds.write('%d %s%s' % (state['tick'] + 1, what, chr(10)))
            return hook
        m.uc.hook_add(UC_HOOK_CODE, on_music('start'), None, 0x197B4, 0x197B4)
        m.uc.hook_add(UC_HOOK_CODE, on_music('stop'), None, 0x19566, 0x19566)
    pc = m.entry
    while state['tick'] < args.ticks and m.exited is None:
        before = state['tick']
        try:
            m.uc.emu_start(pc, 0xFFFFFFFF, count=args.budget)
        except UcError as e:
            cs, ip = m.reg(UC_X86_REG_CS), m.reg(UC_X86_REG_IP)
            print('CPU error at %04X:%04X: %s' % (cs, ip, e))
            break
        cs, ip = m.reg(UC_X86_REG_CS), m.reg(UC_X86_REG_IP)
        pc = (cs << 4) + ip
        if any(s.startswith('int') and 'failed' in s for s in m.log):
            break
        if state['tick'] == before and m.exited is None:
            print('no frame within %d instructions, at %04X:%04X' % (args.budget, cs, ip))
            break
    out.close()
    if map_out:
        map_out.close()
    if sounds:
        sounds.close()  # the hook keeps a reference: close it to flush
    for line in m.log[-40:]:
        print(line)
    print('ticks recorded: %d%s' % (state['tick'], '' if m.exited is None else ', exited with %d' % m.exited))


if __name__ == '__main__':
    main()
