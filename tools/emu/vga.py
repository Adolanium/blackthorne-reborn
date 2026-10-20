"""A VGA model for btemu.py: enough of the hardware to see what the original
puts on screen in its unchained 256-colour mode (Mode X).

- Video memory: 4 planes of 64 KB behind A000:0000, written through the
  sequencer map mask, read through the graphics controller read map, with
  the four latches (write modes 0 and 1, bit mask, set/reset).
- Registers: sequencer, graphics controller, CRTC (start address, offset,
  line compare), attribute controller (pixel panning) and the DAC palette.
- screen() returns the displayed 320x240 image as rows of palette indices,
  plus the palette as 6-bit RGB triples.
"""

from unicorn.x86_const import UC_X86_REG_EAX  # noqa: F401  (keeps the import order of btemu)

VRAM = 0xA0000
PLANE = 0x10000


class Vga:
    def __init__(self, uc):
        self.planes = [bytearray(PLANE) for _ in range(4)]
        self.latch = [0, 0, 0, 0]
        self.seq = bytearray(8)
        self.seq[2] = 0x0F
        self.gc = bytearray(16)
        self.gc[8] = 0xFF
        self.crtc = bytearray(32)
        self.attr = bytearray(32)
        self.attr_index = 0
        self.attr_flip = False
        self.seq_index = self.gc_index = self.crtc_index = 0
        self.dac = bytearray(768)
        self.dac_write = self.dac_read = 0
        uc.mem_unmap(VRAM, PLANE)
        uc.mmio_map(VRAM, PLANE, self._read, None, self._write, None)

    # --- memory ----------------------------------------------------------
    def _read(self, uc, offset, size, _):
        value = 0
        for k in range(size):
            off = (offset + k) & 0xFFFF
            for p in range(4):
                self.latch[p] = self.planes[p][off]
            value |= self.planes[self.gc[4] & 3][off] << (8 * k)
        return value

    def _write(self, uc, offset, size, value, _):
        mask = self.seq[2] & 0x0F
        mode = self.gc[5] & 3
        bits = self.gc[8]
        sr_enable, sr = self.gc[1] & 0x0F, self.gc[0] & 0x0F
        for k in range(size):
            off = (offset + k) & 0xFFFF
            data = (value >> (8 * k)) & 0xFF
            for p in range(4):
                if not mask & (1 << p):
                    continue
                if mode == 1:
                    out = self.latch[p]
                elif mode == 2:
                    fill = 0xFF if data & (1 << p) else 0
                    out = (fill & bits) | (self.latch[p] & ~bits & 0xFF)
                else:
                    d = (0xFF if sr & (1 << p) else 0) if sr_enable & (1 << p) else data
                    out = (d & bits) | (self.latch[p] & ~bits & 0xFF)
                self.planes[p][off] = out

    # --- ports -----------------------------------------------------------
    def port_out(self, port, size, value):
        if size == 2 and port in (0x3C4, 0x3CE, 0x3D4):
            self.port_out(port, 1, value & 0xFF)
            self.port_out(port + 1, 1, value >> 8)
            return True
        v = value & 0xFF
        if port == 0x3C4:
            self.seq_index = v & 7
        elif port == 0x3C5:
            self.seq[self.seq_index] = v
        elif port == 0x3CE:
            self.gc_index = v & 15
        elif port == 0x3CF:
            self.gc[self.gc_index] = v
        elif port == 0x3D4:
            self.crtc_index = v & 31
        elif port == 0x3D5:
            self.crtc[self.crtc_index] = v
        elif port == 0x3C0:
            if not self.attr_flip:
                self.attr_index = v & 31
            else:
                self.attr[self.attr_index] = v
            self.attr_flip = not self.attr_flip
        elif port == 0x3C8:
            self.dac_write = v * 3
        elif port == 0x3C7:
            self.dac_read = v * 3
        elif port == 0x3C9:
            self.dac[self.dac_write % 768] = v & 0x3F
            self.dac_write = (self.dac_write + 1) % 768
        else:
            return False
        return True

    def port_in(self, port):
        if port == 0x3C9:
            v = self.dac[self.dac_read % 768]
            self.dac_read = (self.dac_read + 1) % 768
            return v
        if port == 0x3C5:
            return self.seq[self.seq_index]
        if port == 0x3CF:
            return self.gc[self.gc_index]
        if port == 0x3D5:
            return self.crtc[self.crtc_index]
        return None

    def reset_flipflop(self):
        self.attr_flip = False

    # --- the picture -----------------------------------------------------
    def screen(self, width=320, height=240):
        start = (self.crtc[0x0C] << 8) | self.crtc[0x0D]
        stride = self.crtc[0x13] * 2
        line_compare = self.crtc[0x18] | ((self.crtc[7] >> 4) & 1) << 8 | ((self.crtc[9] >> 6) & 1) << 9
        scan_per_row = (self.crtc[9] & 0x1F) + 1
        pan = (self.attr[0x13] & 7) >> 1
        rows = []
        for y in range(height):
            scan = y * scan_per_row
            if scan > line_compare:
                base = ((y - (line_compare + 1 + scan_per_row - 1) // scan_per_row) * stride) & 0xFFFF
                shift = 0
            else:
                base = (start + y * stride) & 0xFFFF
                shift = pan
            row = bytearray(width)
            for x in range(width):
                px = x + shift
                row[x] = self.planes[px & 3][(base + (px >> 2)) & 0xFFFF]
            rows.append(row)
        return rows, bytes(self.dac)

    def save_png(self, path):
        from PIL import Image
        rows, dac = self.screen()
        img = Image.new('P', (len(rows[0]), len(rows)))
        img.putpalette([c * 255 // 63 for c in dac])
        img.putdata(b''.join(rows))
        img.convert('RGB').save(path)
