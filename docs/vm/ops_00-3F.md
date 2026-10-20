# Opcodes 0x00-0x3F

Source: `funcs_160A1` in `BTHORNE.EXE.asm` (entries 0x00 `sub_160F7` ... 0x3F
`sub_16420`) and every routine the handlers reach: `sub_10568`, `sub_10C9F`,
`sub_11036`/`sub_1103A`, `sub_110AC`/`sub_110B0`, `sub_1115D` (+ `loc_117B7`,
`sub_11795`), `sub_11560`, `sub_11D2E`, `sub_1205B`/`sub_1207C`/`loc_12404`,
`sub_1266C`, `sub_17569`/`sub_17566`, `loc_175D6`, `sub_17B59`, `sub_17C38`,
`sub_17CFC`, `sub_17D5A`, `sub_17DB8`, `sub_17E02`...`sub_17E4D` and their tile
and actor probes (`sub_17F2F`...`sub_1844F`), `sub_18910`, `sub_18984`,
`sub_18992`, `sub_1899E`, `sub_189A5`, `sub_189CD`, `sub_185B1`, `sub_185C4`.
Data tables read from seg003: `ds:60C8h` (branch stubs), `ds:60CCh` (tile-class
lists), `ds:288Ah` (bit masks), `ds:28CAh` (field offsets).

## Notation

| Notation | Bytes | Meaning |
|---|---|---|
| `u8`, `u16` | 1, 2 | Raw little-endian script bytes. |
| `addr16` | 2 | Branch target: offset in the **current** segment `es` (unchanged). |
| `page8` | 1 | Page byte: `seg = ds:[259Ah] + ((page-0x82) ror 5) - 0x800`. |
| `fld8` | 1 | Field selector `b` (even). Field index `b/2`; address `0x13C2 + (b/2)*0x28 + slot`. `1C` = x, `1E` = y, `08` = flags. |
| `tl8` | 1 | Byte offset of a tile-class list in `ds:60CCh` (see "Tile-class lists"). |
| `mode` | 1 | Mode byte. `m.lo = mode & 7`, `m.hi = (mode >> 3) & 7`. Bits 6-7 are ignored. |
| `opA(m.lo)`, `opB(m.hi)` | 0-2 each | Read operand decoded with `sub_17569` (see "Read modes"). Bytes follow in the order A then B. |
| `dstA(m.lo)`, `dstB(m.hi)` | 0-2 each | Write-back operand decoded with `loc_175D6`/`loc_175D3` (see "Write modes"). |

`self` = `ds:2Ah`. `F(s, addr)` = word `ds:[addr + s]`, so `self.x` = `F(self,15F2h)`.
`G(a)` = global word `ds:a`. `ip` = `bx`.

### Read modes (`sub_17569`; `sub_17566` = `ax >>= 3` first)

| Mode | Bytes | Value |
|---|---|---|
| 0 | 2 | `imm16` |
| 1 | 1 | `fld8` of `self` |
| 2 | 2 | `G(addr16)` |
| 3 | 1 | `fld8` of slot `self.17FAh` (target) |
| 4 | 1 | `fld8` of slot `self.1822h` (related) |
| 5 | 0 | `rand16()` = `sub_10C9F` (5-word lagged generator at `ds:4Eh..56h`) |
| 6, 7 | - | Invalid: `off_17575` has only 6 entries; the jump uses code bytes as a pointer. Never used. |

### Write modes (`loc_175D6`, entered with the value in `si`; `loc_175D3` does `ax >>= 3` first)

`loc_175D6` does `push si`, then jumps through `off_175E3[mode]`:

| Mode | Bytes | Store |
|---|---|---|
| 0 | 0 | **Invalid.** Target is a bare `retn` (`locret_1763E`) with the value still on the stack: the CPU returns to `value`. |
| 1 | 1 | `fld8` of `self` = value |
| 2 | 2 | `G(addr16)` = value |
| 3 | 1 | `fld8` of slot `self.17FAh` = value |
| 4 | 1 | `fld8` of slot `self.1822h` = value |
| 5 | 0 | **Invalid** (same bare `retn` as mode 0). |

No script in 064.bin uses write mode 0 or 5 or read mode 6 or 7 (checked by the decoder in the verification section).

### Conditional-branch stubs (`ds:60C8h`)

Opcodes that `push 0` / `push 2` and end with `pop si; jmp word ptr [si+60C8h]`
pick one of two code stubs by the carry flag of the probe (`pop` keeps the flags):

- `[60C8h]` = `seg000:630E`: `if (CF) ip = u16 at ip; else ip += 2;` (push 0: branch if the probe hit)
- `[60CAh]` = `seg000:6318`: `if (!CF) ip = u16 at ip; else ip += 2;` (push 2: branch if the probe missed)

### Tile-class lists (`ds:60CCh`)

A map cell is a word: bits 0-9 = tile graphic, bits 10-15 = class (`sub_189A5`;
a cell outside the map reads as `0x400`, class 1). A probe tests the class
against a sorted list at `ds:60CCh + tl8`, terminated by `FFh`. The list offsets
that exist, and their classes:

| tl8 | Classes | tl8 | Classes | tl8 | Classes |
|---|---|---|---|---|---|
| 00 | 0 | 24 | 2F | 3A | 28 |
| 02 | 1, 2 | 26 | 2F, 30 | 3C | 10 |
| 05 | 1 | 29 | 2E | 3E | 11 |
| 07 | 2 | 2B | 6 | 40 | 12 |
| 09 | 1, 2, 4, 5, 7, 8, A, B, C | 2D | 7 | 42 | A, B, C |
| 13 | 1, 2, 4, 5, A, B, C | 2F | 8 | 46 | A, B |
| 1B | 3, 4 | 31 | 7, 8 | 49 | A, C |
| 1E | 4 | 34 | 1, 2, 20 | 4C | 1, 2, 1C |
| 20 | 1, 2, 5 | 38 | 21 | 50 | 1, 2, 11 |

Every probe first scans map cells, then (if no cell matched) scans live actors
`s != self` (`s.125Ah != 0`) whose low byte of `s.1642h` ("solid class") is in
the same list. On a cell hit: `G(354h) = class`, `G(356h) = 0xFFFF`. On an actor
hit: `G(354h) = s.1642h`, `G(356h) = s`. `G(356h)` is set to `0xFFFF` before each probe.

### The event-check block (opcodes 0x17, 0x1A, 0x2D, 0x2E, 0x2F, 0x30, 0x33)

These opcodes run in three passes, selected by `G(33Eh)`, and number themselves
by `G(33Ch)` (reset to 0 at the start of each script run; each of these opcodes
adds 2). `n = G(33Ch)/2` is the opcode's ordinal in the block; its bit is
`word ds:[288Ah + 2n]` = `1 << n` (16 bits available).

Frame order in `sub_10043`:
1. `sub_1601A`: main run, `G(33Eh) = 0`, from `self.1232h`.
2. `sub_111BC`: velocity integration (actors with `134Ah < 0`), saves previous x/y in `12D2h`/`12FAh`.
3. `sub_17AC7`: `G(33Eh) = +1`. For every slot: `self.1322h = 0`; if `self.134Ah < 0`, run `sub_17AE9` (which itself skips free slots and slots with flag 0x400).
4. `sub_11260`: anchored and one-shot moves (`134Ah >= 0`).
5. `sub_17AB2`: `G(33Eh) = -1`. For every live slot without flag 0x400, run `sub_17AE9`.

`sub_17AE9` executes the script from `self.1232h` (the instruction after the
last yield) with the normal handler table and **stops at opcode 0x01** (the
loop tests the opcode byte before dispatching). Scripts therefore lay out
`00` (yield), then the check opcodes, then `01`, then code. The terrain checks
(0x2D, 0x2E, 0x33) detect in pass +1; the actor-overlap checks (0x17, 0x1A,
0x2F, 0x30) detect in pass -1. In the detecting pass the opcode sets its bit in
`self.1322h` (and records the hit actor for the overlap checks) and never
branches. In the other pre-pass it skips its operands. In the main run it
reads the bit and does a conditional call.

Overlap test used by 0x17/0x1A/0x2F/0x30 (inclusive, signed):
`self.right >= s.left && s.right >= self.left && self.bottom >= s.top && s.bottom >= self.top`.
The hit slot is stored in byte `ds:[1A02h + self*8 + n]` (`sub_185B1`) and read back by `sub_185C4`.

## Opcode table

| Op | Handler | Encoding | Effect | Notes |
|---|---|---|---|---|
| 00 | `sub_160F7` | (none) | `self.1232h = ip; self.184Ah = G(30h); end turn;` | Yield. Saves ip (after the opcode) and the link ip, but not `es` and not the link segment: the next run starts in segment `self.125Ah` with `G(32h) = self.125Ah` (`sub_1603F`). Ends turn. High. |
| 01 | `nullsub_2` | (none) | `;` (no-op) | Main run: no-op. In the pre-passes (`sub_17AE9`) it ends the event-check block (the loop exits before dispatch). Address = `sub_160F7+16`, a single `retn`. High. |
| 02 | `sub_160A9` | `u8 snd`, `u8 unused` | `play_sfx(snd)` | `sub_11D2E`: if `G(380h) != 0`: `v = W[2964h + (snd-0x80)*2]`; `v == 1000` = none; bit 15 clear: `sub_197EA(v)`; bit 15 set: sample `v & 0xFFF` via dword table `2A16h` (`sub_19146`/`sub_19B33`). The second byte is loaded into `si` and overwritten unread. Example `02 8E 7F`. Encoding high, sound internals medium. |
| 03 | `sub_16141` | `addr16` | `ip = addr16;` | Goto. The `cmp bx,8000h; jl` guard (error 0x72 via `sub_10568`, exit to DOS) is a signed compare against -32768 and never fires. High. |
| 04 | `sub_16135` | `addr16` | `G(30h) = ip+2; G(32h) = es; ip = addr16;` | Call (one-level link, no stack). Falls into 0x03. High. |
| 05 | `sub_1610C` | `page8`, `addr16` | `G(30h) = ip+3; G(32h) = es; es = seg(page8); ip = addr16;` | Far call. Unused in 064.bin. High. |
| 06 | `sub_16151` | (none) | `ip = G(30h); es = G(32h);` | Return via link. High. |
| 07 | `sub_11029` | (none) | `if (!(self.flags & 0x40)) hflip(self);` | Set flag 0x40 (face left). `hflip` = `sub_1103A`, see 0x0B. High. |
| 08 | `sub_1101C` | (none) | `if (self.flags & 0x40) hflip(self);` | Clear flag 0x40 (face right). High. |
| 09 | `sub_1109F` | (none) | `if (!(self.flags & 0x80)) vflip(self);` | Set flag 0x80. `vflip` = `sub_110B0`, see 0x0C. Unused. High. |
| 0A | `sub_11092` | (none) | `if (self.flags & 0x80) vflip(self);` | Clear flag 0x80. Unused. High. |
| 0B | `sub_11036` | (none) | `hflip(self)`: `flags ^= 0x40; l = left; r = right; left = 2*x - r - 1; right = 2*x - l - 1; for each part p: p.552h = 2*x - p.552h - p.952h; p.452h ^= 0x20; p.E52h = 0x202;` | Unconditional horizontal flip about x. Parts: `count = self.19B2h` (skipped if 0) starting at part index `self.198Ah`; part fields are `ds:[addr + partidx]`. High. |
| 0C | `sub_110AC` | (none) | `vflip(self)`: `flags ^= 0x80; t = top; b = bottom; top = 2*y - b - 1; bottom = 2*y - t - 1; for each part p: p.652h = 2*y - p.652h - p.952h; p.452h ^= 0x10; p.E52h = 0x202;` | Unconditional vertical flip. Uses `952h` (same size field as the horizontal flip) for the part's extent. Unused. High. |
| 0D | `sub_1615A` | (none) | `G(318h) \|= 1; end turn;` | Requests level exit (`318h` bit 0 also set by `sub_1290E` with `G(1CA8h) = 6Eh`). Does not save ip: next run restarts at the last yield point. Ends turn. Effect high, meaning medium. |
| 0E | `sub_16165` | (none) | `despawn(self); end turn;` | `sub_11560`: `sub_11605`, `sub_1153A`; clear part flags (`452h = 0`, `E52h = 0x200`); unlink parent/child (`166Ah`, `1692h`); `125Ah = 0` (slot free), `1912h = 0`, `18EAh = 0xFFFF`; shrink `G(32Ch)`; if `157Ah != 0xFFFF` and flag 0x100, update spawn record table. Ends turn without saving ip. High. |
| 0F | `sub_16172` | `u8 sel`, `u16 fn` | `if (sel == 0) switch (fn) { case 0: G(204h) <<= 8; case 2: G(204h) >>= 8 /*logical*/; case 4: G(204h) = (int16)G(204h) >> 1; case 6: i = G(204h) + G(204h)/2; G(204h) = ((B[4A12h+i] & 0x3E) >> 1) \| ((B[4A13h+i] & 0x3E) << 4) \| ((B[4A14h+i] & 0x3E) << 9); }` | `fn` is a raw byte offset into `off_16186`; only 0, 2, 4, 6 are valid. `sel != 0` consumes 3 bytes and does nothing. Case 6: the byte offset `G(204h)*1.5` (colour `G(204h)/2` when `G(204h)` is even) selects an RGB triple of the 6-bit base palette `4A12h`; the result is a 5:5:5 word. Used as `0F 00 0002`, `0F 00 0004`. High. |
| 10 | `sub_1705B` | `mode1`, `opA(m1.lo)`, `opB(m1.hi)`, `mode2`, `opA(m2.lo)`, `opB(m2.hi)`, `u8 type` | `G(26h) = x = opA1; G(28h) = y = opB1; G(32Eh) = param = opA2; G(10h) = opB2 & 0x801; G(364h) = 0xFFFF; s = spawn(type, flags = (self.flags & 0xFE) \| G(10h), rec = 0xFFFF); self.1692h = ok ? s : 0; if (ok && s <= G(2Ch)) B[332h + G(330h)++] = s;` | Spawn actor from type record `type` (16-byte record in the 064 table) centered at (x, y). `sub_1115D`/`loc_117B7`: new `15A2h = type`, `157Ah = 0xFFFF`, `1462h = flags & 0xDFFF`, `1552h = param`, `166Ah = self` (parent), x/y, bbox from record width/height, script from record (+3), then 0x40/0x80 in `flags` are applied as real flips (`sub_11795`). Fails (CF) if `G(316h) != 0` or no free slot; then `self.1692h = 0`. Slots `<= G(2Ch)` (the slot being run) are queued in `ds:332h` and run this frame by `sub_15FCE`. Example `10 09 1C 1E 00 00 00 00 00 04`. High. |
| 11 | `sub_171BF` | `mode`, `dstA(m.lo)`, `dstB(m.hi)` | `o = 0; dx = (self.flags & 0x40) ? self.x - o.x : o.x - self.x; dy = (self.flags & 0x80) ? self.y - o.y : o.y - self.y; G(26h) = dx; G(28h) = dy; dstA = dx; dstB = dy;` | Facing-relative offset to slot 0 (Kyle). High. |
| 12 | `sub_171D3` | `mode`, `dstA(m.lo)`, `dstB(m.hi)` | as 0x11 with `o = self.17FAh` | Offset to target. High. |
| 13 | `sub_161DA` | `mode`, `opA(m.lo)`, `opB(m.hi)` | `self.134Ah = 0; self.17AAh = opA; self.17D2h = opB;` | Anchor self to slot 0 for this frame. `sub_11260` then sets `self.x = s.x + ((self.flags & 0x40) ? -17AAh : 17AAh)`, same for y with 0x80 and `17D2h`, moves the bbox, stores the applied deltas back in `17AAh`/`17D2h` and sets `134Ah = -1`. Example `13 00 E8FF F0FF`. High. |
| 14 | `sub_161F0` | `mode`, `opA(m.lo)`, `opB(m.hi)` | `self.134Ah = self.17FAh; self.17AAh = opA; self.17D2h = opB;` | Anchor self to the target (same move as 0x13). Unused. High. |
| 15 | `sub_16215` | `addr16` | `self.18EAh = addr16; self.1912h = es; self.193Ah = 1;` | Start the secondary (animation) script in the current segment. It runs on the next 0x2A. High. |
| 16 | `sub_1622E` | `page8`, `addr16` | `self.1912h = seg(page8); self.18EAh = addr16; self.193Ah = 1;` | Far form of 0x15. Example `16 83 329C`. High. |
| 17 | `sub_17B23` | `u8 cls`, `addr16` | Pass -1: `for s in {0} (G(32Ch) forced to 2), s != self, live: if (s.14B2h == cls && overlap(self, s)) { self.1322h \|= bit(n); hit[self][n] = s; break; }` Pass +1: skip. Main: `if (self.1322h & bit(n)) { self.1822h = hit[self][n]; call addr16; } else ip += 2;` Always `G(33Ch) += 2`. | Event check: overlap with slot 0 (Kyle) of class `cls`. "call" = 0x04 semantics (link = ip after `addr16`). See "event-check block". High. |
| 18 | `sub_161E6` | `mode`, `opA(m.lo)`, `opB(m.hi)` | `F(0,134Ah) = self; F(0,17AAh) = opA; F(0,17D2h) = opB;` | Anchor slot 0 (Kyle) to self (0x13 move applied to slot 0, using slot 0's flip flags). Example `18 00 0000 0000`. High. |
| 19 | `sub_16259` | `addr16` | `if (self.193Ah == 0) ip = addr16; else ip += 2;` | `193Ah` is the animation-script delay (`sub_1207C` stores `G(2Eh)` there after each animation run). Effect high; "0 means animation idle/finished" medium. |
| 1A | `sub_17C02` | `u16 mask`, `addr16` | as 0x17 but the match is `(s.14DAh & mask) != 0`; detect pass reads 2 bytes, the other pre-pass skips 2 bytes; main run skips 2 bytes before the bit test. | Event check against slot 0 by class mask. Unused. High. |
| 1B | `sub_16279` | `tl8`, `addr16` | `if (probe_above(tl8)) ip = addr16; else ip += 2;` | `sub_17E2F`: cells in row `top-1`, columns `left..right` step 16 (last column exactly `right`); else actors with `s.top <= top-1 <= s.bottom` and horizontal overlap (`sub_18331`). High. |
| 1C | `sub_16292` | `tl8`, `addr16` | `if (probe_below(tl8)) ip = addr16; else ip += 2;` | `sub_17E3E`: row `bottom+1`, columns `left..right`; actors via `sub_1833E`. High. |
| 1D | `sub_162D0` | `tl8`, `addr16` | `if ((self.flags & 0x40) ? probe_right(tl8) : probe_left(tl8)) ip = addr16; else ip += 2;` | Probe behind (0x40 = facing left). `probe_left` = `sub_17E11`: column `left-1`, rows `max(0,top-17D2h) .. max(0,bottom-17D2h)` step 16; actors via `sub_1829A` (`s.left <= left-1 <= s.right` and vertical overlap of the velocity-adjusted rows). `probe_right` = `sub_17E20`: column `right+1`, `sub_182A7`. Unused. Effect high, front/back naming medium. |
| 1E | `sub_162AB` | `tl8`, `addr16` | `if ((self.flags & 0x40) ? probe_left(tl8) : probe_right(tl8)) ip = addr16; else ip += 2;` | Probe in front. High. |
| 1F | `sub_1627D` | `tl8`, `addr16` | `if (!probe_above(tl8)) ip = addr16; else ip += 2;` | Negated 0x1B. High. |
| 20 | `sub_16296` | `tl8`, `addr16` | `if (!probe_below(tl8)) ip = addr16; else ip += 2;` | Negated 0x1C. High. |
| 21 | `sub_162D4` | `tl8`, `addr16` | negated 0x1D | Unused. High. |
| 22 | `sub_162AF` | `tl8`, `addr16` | negated 0x1E | High. |
| 23 | `sub_16E9F` | `mode1`, `opA(m1.lo)`, `opB(m1.hi)`, `mode2`, `dstA(m2.lo)`, `dstB(m2.hi)` | `G(26h) = opA >> 4; G(28h) = opB >> 4; dstA = G(26h); dstB = G(28h);` | Pixel to tile coordinates (logical shift). Example `23 09 04 00 12 0402 0602`. High. |
| 24 | `sub_16ECB` | `mode1`, `opA(m1.lo)`, `opB(m1.hi)`, `mode2`, `dstA(m2.lo)` | `G(26h) = opA; dstA = tile_class(opA, opB);` | `tile_class` = `sub_18992`: `(cell(tx,ty) >> 10)`; `cell` = `sub_189A5`: if `tx >= G(1D0Ah)` or `ty >= G(1D0Ch)` (unsigned) then `0x400`, else word `[seg G(259Ch)] : ty*G(1C96h) + tx*2`. `m2.hi` is ignored. Unused. High. |
| 25 | `sub_16EEE` | `mode1`, `opA(m1.lo)`, `opB(m1.hi)`, `mode2`, `dstA(m2.lo)`, `dstB(m2.hi)` | `G(26h) = (opA & 0xFFF0) \| 8; G(28h) = (opB & 0xFFF0) \| 8; dstA = G(26h); dstB = G(28h);` | Snap to tile centre. Unused. High. |
| 26 | `sub_17115` | `mode1`, `opA(m1.lo)`, `opB(m1.hi)`, `mode2`, `opA(m2.lo)` | `G(26h) = tx = opA1; G(28h) = ty = opB1; v = opA2; cell(tx,ty) = v; redraw_tile(tx, ty, v);` | Write a whole map cell. No bounds check on the write (`sub_189CD`). `redraw_tile` = `sub_18910`: if `0 <= tx*16 - G(1B4Eh) < 0x100` and `0 <= ty*16 - G(1B50h) < 0xE0`, copy the 4 words of tile graphic `v & 0x3FF` from `[G(259Ch)]:G(25B4h) + (v&0x3FF)*8` into the screen tile buffer at `3508h` and mark them dirty (`4308h \|= 3`). `m2.hi` ignored. Example `26 12 0402 0602 00 0C18`. High. |
| 27 | `sub_17180` | `mode1`, `opA(m1.lo)`, `opB(m1.hi)`, `mode2`, `opA(m2.lo)` | `G(26h) = tx; G(28h) = ty; G(12h) = v = opA2; c = (cell(tx,ty) & 0xFC00) \| v; cell(tx,ty) = c; redraw_tile(tx, ty, c);` | Replace the graphic, keep the class. `v` is not masked. Out-of-map reads give `0x400` but the write is unchecked. High. |
| 28 | `sub_1713F` | `mode1`, `opA(m1.lo)`, `opB(m1.hi)`, `mode2`, `opA(m2.lo)` | `G(26h) = tx; G(28h) = ty; G(12h) = (v & 0x3F) << 10; cell(tx,ty) = (cell(tx,ty) & 0x3FF) \| G(12h);` | Replace the class, keep the graphic. No redraw. (`xchg ah,al; shl ax,2; and 0FC00h` = `(v & 0x3F) << 10`.) Example `28 12 0602 0802 02 0402`. High. |
| 29 | `sub_16F20` | (none) | `tx = self.x >> 4; ty = self.y >> 4; G(26h) = tx; G(28h) = ty; for (r = G(1CA6h); B[r] != 0xFF; r += 10) if (B[r] <= tx && tx <= B[r+1] && B[r+2] <= ty && ty <= B[r+3]) { G(1CA8h) = W[r+4]; G(1CAAh) = W[r+6]; G(1CACh) = W[r+8]; return; } G(1CA8h) = 0xFFFF;` | Look up the exit/door record (10 bytes: x0, x1, y0, y1, 3 words) that contains self's tile. `G(1CA8h)` is the exit destination that 0x0D/`sub_1290E` use. Effect high, meaning medium. |
| 2A | `sub_1205B` | (none) | `save(es, ip, G(30h), G(32h)); G(33Ah) = 0; anim_tick(self); physics(self); restore(...);` | `anim_tick` = `sub_1207C`: if `18EAh != 0xFFFF`: `d = 193Ah; if (d == 0 \|\| --d == 0) { G(2Eh) = 0; G(30h) = 1962h; G(32h) = 1912h; G(34h) = 198Ah; G(36h) = 19B2h; run interpreter at 1912h:18EAh until a handler ends it; 18EAh = ip; 1962h = G(30h); d = G(2Eh); } 193Ah = d;`. `physics` = `loc_12404`, only if `134Ah < 0`: `if (flags & 0x8000) 152Ah += G(1CF0h); if (flags & 0x4000) 1502h += G(1CEEh); clamp \|1502h\| <= G(1C8Ah), \|152Ah\| <= G(1C8Ch); 17AAh += (flags & 0x40) ? -1502h : 1502h; 17D2h += (flags & 0x80) ? -152Ah : 152Ah;`. The main script's ip, segment and link survive. Used once per loop iteration in almost every script. High. |
| 2B | `sub_162F5` | `tl8`, `addr16` | `if (probe_ahead_below(tl8)) ip = addr16; else ip += 2;` | `sub_17E4D`: single cell at `x' = (flags & 0x40) ? left-1 : right+1`, `y' = bottom+1` (`sub_1802F`); actors containing that point (`sub_183BE`). Ledge test. Unused. High. |
| 2C | `sub_162F9` | `tl8`, `addr16` | `if (!probe_ahead_below(tl8)) ip = addr16; else ip += 2;` | Negated 0x2B. High. |
| 2D | `sub_17CE8` | `tl8`, `addr16` | Pass +1: if x changed since `12D2h`: probe column `left` (moved left) or `right` (moved right), rows `top-17D2h .. bottom-17D2h`; on a cell hit push out (`sub_17E94`: moving right `right = (right & 0xFFF0) - 1`, moving left `left = (left \| 0xF) + 1`, shift x and the other edge by the same delta, `1282h = 0`) and set bit. Else (or no move) test solid actors by relative x velocity (`sub_180F8`), push out to `s.left-1`/`s.right+1` (`sub_18222`) and set bit. Pass -1: skip. Main: `if (self.1322h & bit(n)) call addr16; else ip += 2;` | Horizontal terrain collision event. Main run does not set `1822h`. Effect high, collision details medium. |
| 2E | `sub_17D46` | `tl8`, `addr16` | as 0x2D on the y axis: compare y with `12FAh`, probe row `top` (moved up) or `bottom` (moved down), columns `left..right`; push out (`sub_17EDC`: down `bottom = (bottom & 0xFFF0) - 1`, up `top = (top \| 0xF) + 1`, `12AAh = 0`); actors by relative y velocity (`sub_18154`/`sub_1825E`). | Vertical terrain collision event. Effect high, details medium. |
| 2F | `sub_17B45` | `u8 cls`, `addr16` | as 0x17 but scans all slots `0 .. G(32Ch)-1` | Event check: overlap with any actor of class `cls`. Example `2F 01 A8DC`. High. |
| 30 | `sub_17C24` | `u16 mask`, `addr16` | as 0x1A but scans all slots | Event check: overlap with any actor whose `14DAh & mask`. Example `30 2000 F5BB`. High. |
| 31 | `sub_1636F` | `u8`, `u8`, `u8` (ignored) | `ip += 3; int3();` | Debug breakpoint. Under plain DOS `int 3` is an `iret`, so this is a 4-byte no-op. The game does not hook vector 3. Unused. High. |
| 32 | `sub_171C8` | `mode`, `dstA(m.lo)`, `dstB(m.hi)` | as 0x11 with `o = self.1822h` | Offset to the related actor. Unused. High. |
| 33 | `sub_17DA4` | `tl8`, `addr16` | Pass +1: only if `y > 12FAh` and the bottom edge entered a new 16-px row (`((bottom - y + 12FAh) & 0xFFF0) != (bottom & 0xFFF0)`): probe row `bottom`, columns `left..right`; on a hit `bottom = (bottom & 0xFFF0) - 1` (shift y, top) and set bit; else solid actors moving down relative to self (`sub_184CE`, `sub_1825E`). Pass -1: skip. Main: as 0x2D. | Landing event (one-way floor). Example `33 09 55D9`. Effect high, details medium. |
| 34 | `sub_16335` | `u8 dr`, `u8 dg`, `u8 db` | `G(320h) = dr; G(322h) = dg; G(324h) = db; rebuild_palette();` | `sub_1266C` (falls into `sub_12671`): for 256 colours, `c = base[4A12h+i] + delta` as a byte add; negative (bit 7) becomes 0, bit 6 set becomes 0x3F; stored in `4D12h`. An alternate path runs when `G(326h) & 0x80` (`loc_126CA`, not traced). Values are zero-extended, so `F0h` = -16 only through the byte add. Unused. Encoding high, palette details medium. |
| 35 | `sub_16355` | (none) | `G(320h) = G(322h) = G(324h) = 0; rebuild_palette();` | Reset tint. Unused. High. |
| 36 | `sub_16374` | (none) | `for (i = 0, p = self.198Ah; i < self.19B2h; i++, p += 2) { W[452h+p] \|= 0x40; W[E52h+p] = 0x200; }` | Set part flag 0x40 on every sprite part (likely hide). `dec cx; jnz` loop: with `19B2h == 0` it runs 65536 times. Meaning medium. |
| 37 | `sub_16392` | (none) | same loop: `W[452h+p] &= ~0x40; W[E52h+p] = 0x202;` | Clear part flag 0x40 (likely show). Same `count == 0` hazard. Meaning medium. |
| 38 | `sub_170EA` | `mode`, `opA(m.lo)`, `opB(m.hi)` | `self.17AAh = opA - self.x; self.17D2h = opB - self.y; self.134Ah = 0x100;` | Teleport to (opA, opB): `sub_11260` applies `134Ah >= 0x100` as a one-shot delta (x, y and bbox), then `134Ah = -1`. Example `38 12 0402 0602`. High. |
| 39 | `sub_170C5` | `mode`, `opA(m.lo)`, `opB(m.hi)`, `tl8`, `addr16` | `G(26h) = opA; G(28h) = opB; if (probe_point(tl8, opA, opB)) ip = addr16; else ip += 2;` | `sub_17E02`: one cell at pixel (opA, opB) (`sub_18057`), else actors containing the point (`sub_1844F`). Example `39 09 1C 1E 02 54CB`. High. |
| 3A | `sub_170C9` | `mode`, `opA(m.lo)`, `opB(m.hi)`, `tl8`, `addr16` | as 0x39 with the branch negated | Example `3A 12 0602 0802 09 7199`. High. |
| 3B | `sub_163DA` | `u16 imm` | `G(38h) = imm;` | Load accumulator, mode 0. High. |
| 3C | `sub_163E4` | `fld8` | `G(38h) = self.fld;` | Mode 1. High. |
| 3D | `sub_163F9` | `addr16` | `G(38h) = G(addr16);` | Mode 2. High. |
| 3E | `sub_16405` | `fld8` | `G(38h) = F(self.17FAh, fld);` | Mode 3 (target). High. |
| 3F | `sub_16420` | `fld8` | `G(38h) = F(self.1822h, fld);` | Mode 4 (related). High. |

### Fields and globals discovered in this range

| Name | Meaning |
|---|---|
| `125Ah` | Script base segment; 0 = free slot. |
| `1232h` / `184Ah` | Saved ip / saved link ip. |
| `1282h`, `12AAh` | x / y sub-pixel accumulators (cleared on push-out). |
| `12D2h`, `12FAh` | x / y before this frame's velocity step. |
| `1322h` | Event bits from the check block. |
| `134Ah` | Move mode: `< 0` velocity (8.8 in `17AAh`/`17D2h`, clamped to `G(1C8Ah)`/`G(1C8Ch)` by `sub_111BC`); `0..0xFF` anchor to that slot; `>= 0x100` one-shot delta. |
| `14B2h`, `14DAh` | Class id and class mask (from the type record, +0Ch and +0Eh). |
| `1502h`, `152Ah` | x / y velocity added into `17AAh`/`17D2h` by 0x2A. |
| `1642h` | Solid class (low byte) used by the terrain probes. |
| `166Ah`, `1692h` | Parent slot / last spawned child slot. |
| `17AAh`, `17D2h` | Velocity or offset, per `134Ah`. |
| `18EAh`, `1912h`, `193Ah`, `1962h` | Animation script ip, segment, delay, link ip. |
| `198Ah`, `19B2h` | First sprite-part index, part count. |
| `G(26h)`, `G(28h)` | Scratch x/y (also spawn position). |
| `G(204h)` | Script scratch register (used with 0x0F). |
| `G(318h)` | Bit 0: level exit requested. |
| `G(33Ch)`, `G(33Eh)` | Event-check ordinal x2 and pass (0 main, +1 terrain, -1 overlap). |
| `G(354h)`, `G(356h)` | Last probe hit class and hit actor slot (0xFFFF = map cell). |

## Unresolved or partly resolved

All 64 encodings (operand byte counts) are resolved from the code. These
effects are only partly known:

- **0x02**: the sound path past the table lookup (`sub_197EA`, `sub_19146`, `sub_19B33`) was not traced; the second operand byte is provably unused.
- **0x0D**: the effect (`G(318h) |= 1`, end turn, ip not saved) is exact. That it means "exit level" is inferred from `sub_1290E`.
- **0x0E**: `sub_11605`, `sub_1153A`, `sub_11313` and the spawn-record update after `157Ah` were not followed in full.
- **0x19**: exact test, but the meaning of `193Ah == 0` depends on which animation opcodes set `G(2Eh)` (outside 0x00-0x3F).
- **0x1D/0x1E/0x21/0x22/0x2B/0x2C**: "front"/"back" naming assumes flag 0x40 = facing left; the probe geometry is exact.
- **0x2D/0x2E/0x33**: the actor-solid branches (`sub_180F8`, `sub_18154`, `sub_184CE`, `sub_18521`) were read but not checked case by case.
- **0x29**: the meaning of the three record words (`1CA8h`, `1CAAh`, `1CACh`) beyond "exit destination" is unknown.
- **0x34/0x35**: the `G(326h) & 0x80` palette path (`loc_126CA`) was not traced.
- **0x36/0x37**: the meaning of part flag 0x40 and of `E52h` values 0x200/0x202 (redraw state) is inferred.

## Verification against 064.bin

Method (scratch script, not in the repo): decode from every actor entry
(`offset` and `offset+3`, page from the record), follow every branch, call and
far target, and stop a path at an opcode above 0x3F. To get past the
accumulator opcodes that sit between almost every instruction, the decoder also
uses operand sizes for 0x40-0x94 read from `handlers.txt` (0x40, 0x95: none;
0x41-0x64: `fld8, addr16, fld8, fld8` repeating; 0x65-0x8C: one mode-0..4
operand by `(op-0x65) % 5` then `addr16`; 0x8D-0x94: `fld8, addr16, fld8, fld8`
repeating) and 0xF2 (none). Those sizes are only scaffolding here; they are
documented in the other range files.

Results:

- 4128 instructions decoded, **0 conflicts**: every branch, call and animation target was decoded as an instruction start, and no instruction starts inside the bytes of another.
- Every `tl8` operand of 0x1B-0x22, 0x2B-0x2E, 0x33, 0x39, 0x3A is one of the list starts at `ds:60CCh` (values seen: 02, 05, 09, 13, 24, 26, 31, 34, 3C, 3E).
- Every `fld8` operand of 0x3C/0x3E/0x3F is even.
- No read mode 6/7 and no write mode 0/5 appears.
- Usage counts: 0x05, 0x09, 0x0A, 0x0C, 0x14, 0x1A, 0x1D, 0x21, 0x24, 0x25, 0x2B, 0x31, 0x32, 0x34, 0x35 never appear; 0x3B (4620), 0x03 (1581), 0x3D (902), 0x3C (704), 0x15 (673), 0x01 (623), 0x00 (620), 0x06 (602), 0x2A (582) are the most common.

### Script 1: type 0x31 (record offset `D7E9`, page 0x83): 27 instructions, only 0x00-0x3F

Types 0x31-0x39 share a block of 9-byte records. Linear decoding runs across
all of them and each record offset in the type table (`D7E9`, `D7F2`, `D7FB`,
`D804`, `D80D`, `D816`, `D81F`, `D828`, `D831`) lands on an instruction start:

```
D7E9: 03 54 CB      goto CB54      ; restart entry -> CB54 = 0E (despawn)
D7EC: 15 51 D8      anim D851      ; initial entry (offset+3)
D7EF: 03 4B D8      goto D84B
D7F2: 03 54 CB      goto CB54      ; type 0x32
D7F5: 15 97 D8      anim D897
D7F8: 03 4B D8      goto D84B
...                                 ; same 3-instruction pattern for D7FB, D804, D80D, D816, D81F, D828
D831: 03 54 CB      goto CB54      ; type 0x39
D834: 15 5B DB      anim DB5B
D837: 3B 00 00      acc = 0
D83A: 70 ...        (0x70, outside this range)
```

### Script 2: types 0x3B-0x40 (record offset `DC2F`, page 0x83): 24 instructions, only 0x00-0x3F

```
DC2F: 03 54 CB      goto CB54      ; type 0x3B restart
DC32: 15 95 DC      anim DC95
DC35: 03 65 DC      goto DC65
DC38: 03 54 CB / DC3B: 15 15 DE / DC3E: 03 65 DC        ; type 0x3C
DC41: 03 54 CB / DC44: 15 44 DE / DC47: 03 65 DC        ; type 0x3D
DC4A: 03 54 CB / DC4D: 15 8D DE / DC50: 03 65 DC        ; type 0x3E
DC53: 03 54 CB / DC56: 15 20 DF / DC59: 03 65 DC        ; type 0x3F
DC5C: 03 54 CB / DC5F: 15 7C DF / DC62: 03 65 DC        ; type 0x40
DC65: 2A            anim tick + physics   ; goto target DC65 = this instruction
DC66: 00            yield
DC67: 01            end of (empty) check block
DC68: 04 6E DC      call DC6E             ; DC6E is the next instruction start after DC6B
DC6B: 03 65 DC      goto DC65
DC6E: 3D 4E 1B      acc = G(1B4Eh)        ; camera x
DC71: 42 ...        (0x42, outside this range)
```

### Script 3: type 0x19 (record offset `BB1B`, page 0x83): event block, mixed with 0x4x/0x7x

This script mixes in accumulator opcodes (marked `*`, sized as above). It is
included because it exercises the event-check block and several branch forms;
all targets land on decoded starts:

```
BB1E: 15 DD BC      anim BCDD
BB21: 2A            tick
BB22: 3B 00 18      acc = 0x1800
BB25: 51 08       * flags |= acc
BB27: 3B C8 00      acc = 0x00C8
BB2A: 41 1A       * self.field(0x1A) = acc
BB2C: 3B 01 00      acc = 1
BB2F: 75 14 5D BB * compare self.1552h, branch BB5D
BB33: 3B 00 80      acc = 0x8000
BB36: 51 08       * flags |= acc
BB38: 3B 00 08 / BB3B: 41 10 / BB3D: 3B 00 FA / BB40: 41 12 / BB42: 3B 03 00 / BB45: 41 14
BB47: 2A            tick                   ; target of BB5A
BB48: 00            yield
BB49: 30 20 00 F5 BB   check n=0: overlap any actor with 14DAh & 0x0020 -> call BBF5
BB4E: 2D 05 C8 BB      check n=1: horizontal terrain, classes {1} -> call BBC8
BB52: 33 09 5D BB      check n=2: landing, list 09 -> call BB5D
BB56: 01            end of check block
BB57: 04 03 BC      call BC03
BB5A: 03 47 BB      goto BB47
BB5D: 02 81 7F      sfx 0x81
...
BBBA: 22 05 5D BB   if front probe for classes {1} misses: goto BB5D
```

The spawn opcode is checked the same way: type 0x1D at `BDC4` decodes
`10 09 1C 1E 00 00 00 00 00 04` (10 bytes: spawn type 4 at self.x/self.y,
param 0, flags 0); the next bytes `F2` and `0E` (no operands, despawn) end
exactly at `BDD0`, the animation script that type's own `15 D0 BD` points to.
