# Opcodes 0xC0-0xFF

Source: `funcs_160A1` in `BTHORNE.EXE.asm` (handler addresses cross-checked
against the table: index 0xC0 = `sub_173C5` ... 0xFB = `sub_10B07`,
0xFC-0xFF = `nullsub_3`) and every helper the handlers call or fall into:
`sub_16141`, `sub_164A5`..`sub_16538`, `sub_17566`/`sub_17569`, `sub_1101C`,
`sub_11029`, `sub_1103A`, `sub_1110D`, `sub_11FF4`, `sub_15BF2`, `sub_17A3A`,
`sub_179C2`, `sub_18057`/`loc_17FF7`, `sub_18984`, `sub_1115D` (spawn),
`sub_121F2`, `sub_19062`, `sub_15C57`, `sub_15C82`, `sub_15CEF`, `sub_15D03`,
`sub_15D4F`, `sub_15D83`, `sub_15E51`, `sub_15EB5`, `sub_10AC1`, `sub_1283B`,
`sub_11D73`, `sub_197B4`, `sub_195AC`, `sub_10568`/`sub_1056B`, `sub_10590`,
and the anim runner `sub_1205B`/`sub_1207C`.

## Notation

Same helpers as [ops_40-7F](ops_40-7F.md): `acc` = `ds:38h`, `SELF(b)`,
`TGT(b)`, `REL(b)`, `GLOB(a)`, `goto_(t)` (ip = t, same segment), `skip()`
(ip += 2). `W[x]` is the word at `ds:x`. Fields are named by address
(`self.15F2h` = x). Script bytes are little-endian.

| Notation | Bytes | Meaning |
|---|---|---|
| `u8`, `s8`, `u16`, `s16` | 1, 1, 2, 2 | Literal script bytes. |
| `fld8` | 1 | Field selector `b` (byte offset into the `28CAh` table, `b = 2k`). |
| `addr16` | 2 | Global word address `ds:addr16`. |
| `tgt16` | 2 | Branch / call target, offset in the current script segment. |
| `mode` | 1 | Mode byte: bits 0-2 = mode lo, bits 3-5 = mode hi. |
| `opA(mode lo)`, `opB(mode hi)` | 0-2 | Operand read by `sub_17569` with that 3-bit mode: 0 = u16 imm, 1 = u8 `fld8` (self), 2 = u16 `addr16`, 3 = u8 `fld8` (target), 4 = u8 `fld8` (related), 5 = random, no bytes. Modes 6/7 index past the 6-entry jump table (undefined; never used). |
| `n × X` | n·size | One `X` per sprite part, see "Part model" below. |

`val(m)` below means "read one operand with mode `m`" (consumes its bytes).
`call_(t)` = `W[30h] = ip_after_instruction; W[32h] = es; ip = t;` (the
one-level link register, as opcode 0x04).

## Part model (sprites)

An actor is drawn as a set of **parts** (hardware-sprite-like squares) taken
from a global part pool. Each part attribute is a word array with 0x100 bytes
between arrays; a part is addressed by its byte index `p` (= part number × 2),
so part field `X` of part `p` is `W[X + p]`:

| Array | Meaning |
|---|---|
| `452h + p` | attr. `0x02` size select (`DD`), `0x10` vertical flip, `0x20` horizontal flip, `0x40` hidden, `0x80` allocated, bits 8-9 priority/layer (`DE`; spawn sets them from actor flags `(flags & 0x30) << 4`). |
| `552h + p` | x, world pixels, left edge |
| `652h + p` | y, world pixels, top edge |
| `752h + p` | offset of the part's pixel data (in the actor's frame buffer) |
| `852h + p` | segment of the part's pixel data |
| `952h + p` | part size in pixels (square). `DD` picks `W[350h]` or `W[352h]` (per-level values from tables `25DAh`/`25E0h`). |
| `A52h + p`, `B52h + p` | set to 0x4000 at spawn (`sub_11771`); not used by this range |
| `E52h + p` | redraw flags: 0x202 after any change, 0x200 after hide, 0x300 when the dialog portrait is removed |
| `F52h + p` | colour base, both bytes equal: `((bank + 8) << 4)`, i.e. palette bank 0-7 = VGA colours 0x80 + 16·bank. Pixels are 4-bit and ORed with it (two at a time, hence both bytes). |
| `1052h + p` | group mask, set by `DB`, tested against the filter `ds:33Ah` |

Per actor: field `198Ah` = byte index of its first part, `19B2h` = part count
(from actor-record byte +2, bits 0-5; bit 7 / bit 6 choose where in the pool
the parts are allocated). Pool part 2 (byte index 4) is reserved for the
dialog portrait (`EB`).

**Globals while an animation script runs.** Opcode 0x2A (`sub_1205B`) sets
`W[33Ah] = 0` and calls `sub_1207C`, which:

```c
if (self.18EAh == 0xFFFF) return;                 // no anim script
es = self.1912h;  wait = self.193Ah;
if (wait != 0 && --wait != 0) { self.193Ah = wait; return; }   // still waiting
W[2Eh] = 0;                       // wait for next tick (D7 overwrites)
W[30h] = self.1962h;  W[32h] = self.1912h;      // anim link register
W[34h] = self.198Ah;              // first part (byte index)  = "p0"
W[36h] = self.19B2h;              // part count               = "n"
run interpreter at self.18EAh until D6/D7/00 pops out;
self.18EAh = ip;  self.1962h = W[30h];  self.193Ah = W[2Eh];
```

So inside an animation script, `ds:34h` = first part byte index and
`ds:36h` = part count `n`. **Every per-part opcode (`CF D1 D3 D4 DB DD E0 E1
E2`, `F1`, `D8-DA`, `DE`) loops `i = 0..n-1` over parts `p = p0 + 2i`**, and
the per-part ones consume one operand per part, so their length depends on the
actor's part count (record byte +2 & 0x3F). Nothing else sets `ds:34h`/`36h`;
in a main script they hold whatever the last anim run left (no script uses
part opcodes there, see verification). With `n = 0` the loops (`dec cx / jnz`)
would run 65536 times; no script does this.

**Part filter.** `D5 u8` sets `W[33Ah]`. Only `CF`, `DB` and `E0` honour it:
when `W[33Ah] != 0` they visit only parts with `(group & W[33Ah]) != 0` and
**consume an operand only for those parts**. The filter is reset to 0 at every
0x2A call, i.e. at the start of every anim tick (it does not survive a `D7`
wait).

**Frame data.** `DF id` selects a sprite bank: `self.19DAh` = 2·index of `id`
in the 16-word table `ds:1152h` (0 if absent). `DC` loads frame `N` of that
bank into the actor's frame buffer `self.18C2h:self.16BAh`
(`sub_15BF2`): source bytes start at `N * W[1192h + bank]` in bank handle
`W[1172h + bank]` (mapped by `sub_18FFA`); `W[1192h + bank]` pixel bytes are
copied, and before every group of 8 pixels one mask byte is inserted
(bit 7..0 = pixel non-zero). An 8×8 cell is therefore 8 × 9 = **0x48 bytes**.
`CF b...` points each part at cell `b`: data = `16BAh + b*0x48`. A 16×16
part uses 4 consecutive cells (Kyle: `CF 00 04 08`), the 32×32 portrait 16
cells (`0x480` bytes).

**Part positions.** `D1`/`D3` give per-part offsets from the actor origin:
`x = self.x + dx`, `y = self.y + dy` (left/top edge). When the actor faces
left (flag 0x40) or is vertically flipped (flag 0x80) the positions are
mirrored about the origin: `x' = 2*self.x - x - size`,
`y' = 2*self.y - y - size`, i.e. `x' = self.x - dx - size`.

## Table

| Op | Handler | Encoding | Effect | Notes |
|---|---|---|---|---|
| C0 | sub_173C5 | `tgt16` | `if (onscreen(self)) goto_(t); else skip();` | `onscreen(s)`: `(int16)(camX - s.143Ah) < 0 && (int16)(camX + 0x100 - s.1412h) >= 0 && (int16)(camY - s.13EAh) < 0 && (int16)(camY + 0xE0 - s.13C2h) >= 0`, camX/Y = `W[1B4Eh]`/`W[1B50h]`; i.e. bbox overlaps the 256×224 view (tests are the sign of the 16-bit difference, `jns`/`js`). Dispatch via `off_17405`/`off_17409`. High. |
| C1 | sub_173AF | `tgt16` | `if (onscreen(self.17FAh)) goto_(t); else skip();` | Target actor. High. |
| C2 | sub_173BD | `tgt16` | `if (!onscreen(self)) goto_(t); else skip();` | High. |
| C3 | sub_173AB | `tgt16` | `if (!onscreen(self.17FAh)) goto_(t); else skip();` | High. |
| C4 | sub_165E7 | `fld8` | `if (self.flags & 0x80) SELF(b) -= acc; else SELF(b) += acc;` | Jumps to 0x49 / 0x45 handlers. Vertical-flip variant of 0x8D-0x94 (which test 0x40). High. |
| C5 | sub_165F9 | `addr16` | `if (self.flags & 0x80) GLOB(a) -= acc; else GLOB(a) += acc;` | High. |
| C6 | sub_1660B | `fld8` | `if (self.flags & 0x80) TGT(b) -= acc; else TGT(b) += acc;` | High. |
| C7 | sub_1661D | `fld8` | `if (self.flags & 0x80) REL(b) -= acc; else REL(b) += acc;` | High. |
| C8 | sub_1662F | `fld8` | `if (self.flags & 0x80) SELF(b) += acc; else SELF(b) -= acc;` | High. |
| C9 | sub_16641 | `addr16` | `if (self.flags & 0x80) GLOB(a) += acc; else GLOB(a) -= acc;` | High. |
| CA | sub_16653 | `fld8` | `if (self.flags & 0x80) TGT(b) += acc; else TGT(b) -= acc;` | High. |
| CB | sub_16665 | `fld8` | `if (self.flags & 0x80) REL(b) += acc; else REL(b) -= acc;` | High. |
| CC | sub_17870 | `s16 dx`, `s16 dy`, `u8 sel`, `u16 mask`, `tgt16 A`, `tgt16 B` | Horizontal line-of-sight scan, see "CC / F8" below. Ends in `call_(A)`, `call_(B)` or (nothing hit) `ip += 4`. | Link = address after B. Writes `W[44h]`, `W[46h]` (hit x, y), `self.1822h`, `W[356h]`, `W[354h]`. 11 operand bytes. High. |
| CD | sub_163B0 | `mode`, `addr16`, `opB(mode hi)` | `GLOB(addr16 + val(mode hi)) = acc;` | Indexed global store; index is a byte offset (no scaling). Mode lo bits are ignored and consume nothing. Scripts use `CD 12 B2 1C 04 02` = `W[1CB2h + W[204h]] = acc`. High. |
| CE | sub_163C5 | `mode`, `addr16`, `opB(mode hi)` | `acc = GLOB(addr16 + val(mode hi));` | Indexed global load. High. |
| CF | sub_120D2 | `n × u8` (or `m × u8` with filter) | `for each visited part: P.752h = self.16BAh + u8*0x48; P.852h = self.18C2h; P.E52h = 0x202;` | Assign frame cells to parts. Visited = all n parts if `W[33Ah] == 0`, else only those with `P.1052h & W[33Ah]`; an operand byte is read only for visited parts. High. |
| D0 | sub_1213D | `s8 v` | `self.17AAh += (uint16)(((self.flags & 0x40) ? -v : v) << 8);` | X velocity (8.8 fixed point; physics in `loc_12404`), mirrored by facing. `xor ah,ah / xchg` = `(v & 0xFF) << 8`, equal to `v*256` mod 2^16. High. |
| D1 | sub_12159 | `n × s16` | `for i: P.552h = self.x + dx[i]; P.E52h = 0x202;` then, if `self.flags & 0x40`, mirror all parts: `P.552h = 2*self.x - P.552h - P.952h`. | Code: if facing left, `sub_1101C` un-mirrors (clears 0x40, mirrors bbox and all parts, toggles part 0x20), flag 0x40 is set again, offsets written, then `sub_11029` re-mirrors. Net: bbox and part 0x20 bits unchanged, new x mirrored. The mirror passes use `198Ah`/`19B2h` (all actor parts). High. |
| D2 | sub_121A2 | `s8 v` | `self.17D2h += (uint16)(((self.flags & 0x80) ? -v : v) << 8);` | Y velocity, mirrored by vertical flip. High. |
| D3 | sub_121BE | `n × s16` | `for i: P.652h = self.y + dy[i]; P.E52h = 0x202;` then, if `self.flags & 0x80`, for all actor parts: `P.652h = 2*self.y - P.652h - P.952h; P.452h \|= 0x10;` | `sub_1110D` (ORs 0x10, does not toggle). High. |
| D4 | sub_12288 | `n × u8` | `for i: c = ((u8 + 8) << 4) & 0xFF; P.F52h = c \| c << 8; P.E52h = 0x202;` | Set palette bank per part directly (no reference counting, unlike F1). Unused in 064.bin. High. |
| D5 | sub_1238C | `u8` | `W[33Ah] = u8;` | Part group filter for CF/DB/E0; reset by every 0x2A. High. |
| D6 | sub_12395 | (none) | End this anim tick: `pop ax; ret` out of the interpreter. Anim resumes at the next instruction on the next 0x2A call. | `W[2Eh]` stays 0, so wait = 1 tick. In a main script it would end the actor's turn **without** saving ip (next tick restarts at the old `1232h`); no script does this. High. |
| D7 | sub_12397 | `u8 N` | `W[2Eh] = N;` then end the anim tick like D6. | `sub_1207C` stores N in `self.193Ah`; the anim resumes on the `max(N,1)`-th following 0x2A call (N = 0 and 1 both mean next tick). Same main-script caveat as D6. High. |
| D8 | sub_123A1 | (none) | `for i: P.452h ^= 0x20; P.E52h = 0x202;` | Toggle horizontal flip of the images only (positions unchanged). High. |
| D9 | sub_123A6 | (none) | `for i: P.452h ^= 0x10; P.E52h = 0x202;` | Toggle vertical flip. Unused in 064.bin. High. |
| DA | sub_123AB | (none) | `for i: P.452h ^= 0x30; P.E52h = 0x202;` | Toggle both. Unused. High. |
| DB | sub_12356 | `n × u8` (or `m × u8` with filter) | `for each visited part: P.1052h = u8;` | Set group masks (no redraw flag). Filtered like CF. High. |
| DC | sub_1201E | `mode`, `opA(mode lo)` | `N = val(mode lo); if (N != self.1782h) { self.1782h = N; load_frame(N); for i: P.E52h = 0x202; }` | `load_frame` = `sub_15BF2` with bank `self.19DAh`, destination `self.18C2h:16BAh` (see Part model). Mode hi bits ignored. Scripts use mode 0: `DC 00 lo hi`. High. |
| DD | sub_122AC | `n × u8` | `for i: if (u8) { P.952h = W[352h]; P.452h \|= 2; } else { P.952h = W[350h]; P.452h &= ~2; } P.E52h = 0x202;` | `sub_11FF4`. Part size select. High. |
| DE | sub_1232F | `u8 v` | `for i: P.452h = (P.452h & 0xFCFF) \| (v << 8); P.E52h = 0x202;` | Priority bits 8-9. `v` is not masked, so v > 3 would set bits 10-15. High. |
| DF | sub_123C7 | `u16 id` | `self.19DAh = 2 * index of id in W[1152h..1170h] (searched 15 down to 0), or 0 if absent;` | Selects sprite bank for DC. High. |
| E0 | sub_122C3 | `n × u8` (or `m × u8` with filter) | `for each visited part: if (u8 & 1) { P.452h \|= 0x40; P.E52h = 0x200; }` | Hide parts. Filtered like CF. High. |
| E1 | sub_1230D | `n × u8` | `for i: if (u8 & 1) { P.452h &= ~0x40; P.E52h = 0x202; }` | Show parts. Not filtered. High. |
| E2 | sub_123E6 | `n × u8` | `for i: P.452h ^= u8; P.E52h = 0x202;` | XOR attr low byte (flip bits). High. |
| E3 | sub_17022 | `mode1`, `opA(m1 lo)`, `opB(m1 hi)`, `mode2`, `opA(m2 lo)`, `opB(m2 hi)`, `u8 type` | `dx = opA1; W[28h] = self.y + opB1; W[26h] = (self.flags & 0x40) ? self.x - dx : self.x + dx; W[32Eh] = opA2; f = (self.flags & 0xFE) \| (opB2 & 0x801); W[364h] = 0xFFFF; slot = spawn(type, rec = -1, f); self.1692h = slot; if (ok && slot <= W[2Ch]) W[332h + W[330h]++] (byte) = slot;` | Spawn relative to self (facing-aware); shares tail `loc_1706D` with 0x10. `spawn` = `sub_1115D` (position `W[26h]/[28h]`, spawn param `W[32Eh]` -> new `1552h`, hotspot default since `W[364h] < 0`); returns slot or 0 with CF on failure (then `1692h = 0`). The `332h` list makes the scheduler run a child whose slot was already passed this tick. High. |
| E4 | sub_16267 | `tgt16` | `if (self.193Ah != 0) goto_(t); else skip();` | "Anim still waiting". High. |
| E5 | sub_16F8B | `mode`, `opA(mode lo)` | `r = val(mode lo) & 0x7F; R = 0x1D24 + r*14; for s in W[31Eh] .. W[32Ch]-2: if (s.125Ah && s.157Ah == r) { self.1692h = s; return; } W[26h]=W[R]; W[28h]=W[R+2]; W[364h]=W[R+4]; W[366h]=W[R+6]; W[32Eh]=W[R+12]; slot = spawn(W[R+8], r, W[R+10]); self.1692h = slot; queue as E3;` | Spawn from spawn-record r (14-byte records `{x, y, hotX, hotY, type, flags, param}` at `ds:1D24h`) unless an actor from that record is alive. Does not check `type == 0`. Mode hi ignored. High. |
| E6 | sub_15F87 | `mode1`, `opA(m1 lo) text`, `opB(m1 hi) style`, `mode2`, `opA(m2 lo) dx`, `opB(m2 hi) dy` | `prepare_text(text, style); col = max(2, (self.x + dx*8 - camX) >> 3) (2 if negative); row = same with y, camY; clamp: if (col + w >= 31) col = max(0, 30 - w); if (row + h >= 26) row = max(0, 25 - h); draw_box(col, row); draw_text(col, row);` | Text box near the actor. See "Text boxes". Unused in 064.bin. High. |
| E7 | sub_15F96 | `mode1`, `opA(m1 lo) text`, `opB(m1 hi) style`, `mode2`, `opA(m2 lo) col`, `opB(m2 hi) row` | `prepare_text(text, style); draw_box(col, row); draw_text(col, row);` | Text box at absolute text-layer cell; no clamping. High. |
| E8 | loc_15FA8 | `mode1`, `opA(m1 lo) col`, `opB(m1 hi) row`, `mode2`, `opA(m2 lo) ch` | `W[2D08h + (row*32 + col)*2] = val - 0x20;` | Put one character tile. Code continues into bytes IDA shows as `byte_15FC6` + `adc`: they are `8B 36 10 00 89 04 C3` = `mov si,[10h]; mov [si],ax; retn`. No dirty mark (`4308h`). Mode2 hi ignored. Unused in 064.bin. High (decode), medium (no-dirty consequence). |
| E9 | sub_15F70 | `u16` | `W[1B46h] = u16;` | Also cleared at `seg000:2A72`. No reader found in the listing. Unused in 064.bin. Low (meaning). |
| EA | sub_15F4A | (none) | `W[36Ah] = 0; for i in 0..0x37F: if (W[2D08h+2i] != 0x20) { W[4308h+2i] \|= 3; W[2D08h+2i] = 0x20; } portrait_update();` | Clear the whole 32×28 text layer and remove the portrait. High. |
| EB | sub_15F7A | (none) | `portrait_update();` | `sub_15EB5`, see "Text boxes". High. |
| EC | sub_15F80 | (none) | `W[318h] \|= 4; portrait_update();` | Only bits 0-1 of `318h` are tested in the listing (bit 0 = level exit pending); no reader of bit 2 found. Medium. |
| ED | sub_17497 | `u8 mask`, `u8 ticks` | `W[340h] = mask; W[348h] = ticks;` | Horizontal screen shake: each tick `sub_10AC1` does `if (W[348h]) { W[348h]--; if (W[340h]) W[344h] ^= W[340h]; else W[344h] = 0; } else W[344h] = 0;`. `W[344h]` is added to the CRTC start / pel panning in `sub_1283B`. High. |
| EE | sub_174AC | `u8 mask`, `u8 ticks` | `W[342h] = mask; W[34Ah] = ticks;` | Vertical shake via `W[346h]` (row offset). High. |
| EF | sub_1864D | `u16 bits`, `tgt16` | `for s = 0; s < W[32Ch]; s += 2: if (s.125Ah && (s.14DAh & bits) && s != self && overlap(self, s)) { self.1822h = s; goto_(t); return; } skip();` | `overlap`: `self.143Ah >= s.1412h && s.143Ah >= self.1412h && self.13EAh >= s.13C2h && s.13EAh >= self.13C2h` (signed, inclusive). First match in slot order. Unused in 064.bin. High. |
| F0 | sub_185D6 | `u16 class`, `tgt16` | As EF but the test is `s.14B2h == class`. | High. |
| F1 | sub_1221F | `u16 id` | `if (self.flags & 0x2000) { self.flags &= ~0x2000; palRef[((W[F52h + p0] >> 4) - 8) & 7]--; } if (id == 0x48) c = 0x0707; else { self.flags \|= 0x2000; s = pal_acquire(id); c = (0x80 + 16*s) * 0x101; } for i: P.F52h = c; P.E52h = 0x202;` | Reference-counted palette: `pal_acquire` (`sub_121F2`) finds `id` in `W[1C7Ah..1C88h]` (slots 7..0) and increments `W[1C6Ah + 2s]`; else takes a slot with refcount 0 (7..0, slot 0 if none), stores id, loads 16 colours (`sub_19062`: id looked up in list `ds:6A78h`, 48 bytes from `6ABCh + idx*48` into palette copy `4A12h + (s+8)*48`, assert 0x69 if unknown). Id 0x48 sets 0x0707 without a reference; meaning of 0x0707 unknown. High (mechanics). |
| F2 | sub_172F7 | (none) | `if ((int16)self.157Ah >= 0) W[1D2Ch + self.157Ah*14] = 0;` | Clear the type of this actor's spawn record (record will not respawn). High. |
| F3 | sub_160BB | `u8 id` | `stop_sound(id);` | `sub_11D73`: if `W[380h]` (sound on): `v = W[2964h + (id-0x80)*2]`; `0xFFFF` = none; bit 15 clear -> stop digital sample `v` (`sub_196D5`), set -> `sub_19188` (stops the music-channel effect). Counterpart of 0x02 (play). Scripts use ids >= 0x80. Medium (driver semantics). |
| F4 | sub_160CB | `u8` (ignored) | `if (W[37Eh]) music_restart();` | `sub_197B4` ignores its argument: if a song is loaded, AIL ordinals 0xAA then 0xB1 (stop, start sequence `W[24FAh]`). Menu uses it right after setting `W[37Eh] = 1` (music on). Medium. |
| F5 | sub_160E3 | `u8` (ignored) | `if (W[37Eh]) music_stop();` | `sub_195AC`: AIL ordinal 0xAB on the current sequence. Menu calls it before clearing `W[37Eh]`. Medium. |
| F6 | sub_10B92 | (none) | `for (e = 0; T[e].level != 0xFFFF; e++) if (T[e].pw == password) break; if (end) { e = 0; password = T[0].pw; } W[1CA8h] = T[e].level; W[1CAAh] = T[e].x; W[1CACh] = T[e].y; W[37Ah] = 0;` | Password table `T` at `ds:543Ah`, 10-byte entries `{u16 level, u16 x, u16 y, char pw[4]}`; current password = words `W[372h], W[374h], W[376h], W[378h]` (one char each). Sets next level and start position. `W[37Ah]` meaning unknown. High. |
| F7 | sub_10C0D | (none) | `for (e = 0; (int16)T[e].level >= 0; e++) if (T[e].level == W[1CA4h]) { password = T[e].pw; if (W[304h] == 0) { buf[64E7h..] = pw; if (T[e].level != 0xC0) write 4 bytes to "d:\\savegame.000" (DOS create; on error 3 "savegame.000"); } return; }` | Record the current level's password and save it to disk. Kyle's script runs it at level start. High. |
| F8 | sub_1785E | as CC | `saved = W[32Ch]; W[32Ch] = 2; CC(...); W[32Ch] = saved;` | CC that only considers actor slot 0 (Kyle). High. |
| F9 | sub_12132 | (none) | `fatal(0x25);` never returns | `sub_10568`: prints the error banner and "Assertion failed!", error code 0x25, exits to DOS (`int 21h/4C00h`), after an `int 3`. Unused. High. |
| FA | sub_174C1 | (none) | `quit_to_dos();` never returns | `sub_10590`: restores the screen (`sub_10555`), prints "\r\nLast password:  \r\n" followed by the 4 bytes at `64E7h` (written by F7), exits to DOS. Menu "quit". High. |
| FB | sub_10B07 | (none) | `password = read 4 bytes of "d:\\savegame.000" (or "savegame.000" if the path is not found); on failure password = "STRT" (bytes 23980h-23983h); then falls through into F6.` | Continue a saved game. `bx` is preserved. High. |
| FC | nullsub_3 | (none) | no-op | `retn`. Unused. High. |
| FD | nullsub_3 | (none) | no-op | Unused. High. |
| FE | nullsub_3 | (none) | no-op | Unused. High. |
| FF | nullsub_3 | (none) | no-op | Unused. High. |

No opcode in this range ends the actor's main-script turn except D6/D7 when
misused there; D6/D7 end the animation tick. F9 and FA terminate the program.

### Instruction lengths (including the opcode byte)

| Length | Opcodes |
|---|---|
| 1 | D6, D8, D9, DA, EA, EB, EC, F2, F6, F7, F9, FA, FB, FC-FF |
| 2 | C4, C6, C7, C8, CA, CB, D0, D2, D5, D7, DE, F3, F4, F5 |
| 3 | C0-C3, C5, C9, DF, E4, E9, ED, EE, F1 |
| 5 | EF, F0 |
| 12 | CC, F8 |
| 1 + n | D4, DD, E1, E2; CF, DB, E0 when `W[33Ah] == 0` |
| 1 + m | CF, DB, E0 when `W[33Ah] != 0` (m = parts whose group matches) |
| 1 + 2n | D1, D3 |
| mode-dependent | CD, CE: 4 + size(hi); DC: 2 + size(lo); E5: 2 + size(lo); E3: 4 + size(m1 lo) + size(m1 hi) + size(m2 lo) + size(m2 hi); E6, E7: 3 + the four operand sizes; E8: 3 + size(m1 lo) + size(m1 hi) + size(m2 lo) |

with size(mode) = 2, 1, 2, 1, 1, 0 for modes 0-5.

### CC / F8: horizontal scan (`sub_17870`)

```c
dir = (self.flags & 0x40) ? -16 : 16;          W[4Ch] = dir;
if (dir < 0) dx = -dx;
x0 = ((self.x + dx) & 0xFFF0) | 8;  W[44h] = W[60h] = x0;   // cell centre
y  = self.y + dy;                    W[46h] = y;
W[48h] = sel; W[4Ah] = mask;
// pick the nearest candidate actor ahead (sub_17A3A / sub_179C2):
// s active (125Ah != 0), s != self, s < W[32Ch], and
//   m = s.14DAh & mask; (m != 0 || in_list(sel, low byte of s.1642h)), and
//   s.13C2h <= y <= s.13EAh
//   dir>0: x0 <= s.143Ah < best (start 0x7FFF), smallest right edge wins
//   dir<0: best <= s.1412h <= x0 (start 0), largest left edge wins (ties: later slot)
// -> W[5Ah] = slot or -1, W[5Ch] = m, W[5Eh] = edge
range = dir>0 ? min(camX + 0x110, W[5Eh]) - x0
              : x0 - max(max(camX - 16, 0), W[5Eh]);
for (;;) {                                     // W[26h]=x, W[28h]=y
    if (tile_in_list(x, y, sel)) goto blocked; // sub_18057, sets W[354h]
    x += dir; W[44h] = x;
    range -= 16; if (range < 0) break;
}
if (W[5Ah] < 0) { ip += 4; return; }           // nothing: skip A,B, link untouched
hit_actor:
    s = W[5Ah]; self.1822h = s; W[356h] = s;
    if (W[44h] != W[60h]) W[44h] = dir>0 ? s.1412h : s.143Ah;
    call_(W[5Ch] ? B : A); return;
blocked:
    s = W[5Ah];
    if (s >= 0 && (dir>0 ? s.1412h < W[44h] : s.143Ah >= W[44h])) goto hit_actor;
    W[44h] = dir>0 ? (W[44h] & 0xFFF0) : (W[44h] | 0x000F);   // wall edge
    call_(A);
```

`in_list(sel, v)` / `tile_in_list`: the byte list at `ds:60CCh + sel` is
sorted ascending; scan until `v < L[i]` (absent) or `v == L[i]` (present).
`tile_in_list` uses the tile class `map(x>>4, y>>4) >> 10` (`sub_18984`).
So A is called for a wall or an actor matched only by type list, B for an
actor matched by `mask` bits. Scripts follow CC with e.g.
`10 12 44 00 46 00 ...` (spawn at `W[44h]`, `W[46h]`), and often pass
A = `9B75` (a bare `06` return).

### Text boxes (E6, E7, EA, EB)

- Text layer: 32×28 words at `ds:2D08h` (tile = char − 0x20, 0x20 = blank),
  change flags at `ds:4308h`.
- `prepare_text(text, style)` (`sub_15C57`, `sub_15C82`): text record =
  near pointer `seg001:[text*2]` -> `{u8 w, u8 h, chars..., 0}` (0x0D =
  newline). `W[36Ah] = style`. Style 0x4000: portrait left, frame, `w += 7`,
  `h = max(h + 2, 6)`, text origin (+6, +1). 0x8000: portrait right, same
  sizes, origin (+1, +1). 0x2000: frame only, `w += 2`, `h += 2`, origin
  (+1, +1). Otherwise no frame, origin (0, 0).
- `draw_box` (`sub_15D83`, only if `style & 0xE000`): border tiles 1/2/3 top,
  4/0/5 middle rows, 6/7/8 bottom; with a portrait it blanks a 4×4 cell area
  at (col+1, row+1) (0x4000) or (col+w−5, row+1) (0x8000).
- `draw_text` (`sub_15E51`): writes the chars from the origin; sets portrait
  position `W[36Eh] = (row+1)*8`, `W[36Ch] = (col+1)*8` (0x4000) or
  `(col+w−5)*8`.
- `portrait_update` (`sub_15EB5`): if a portrait is shown (`W[370h]`), hide
  pool part 2 (`attr &= ~0x80`, redraw 0x300) and release its palette ref.
  Then if `W[36Ah] & 0xC000`: `W[370h] = style`, `k = style & 0x1FFF`,
  palette `pal_acquire(W[5560h + 2k])`, part 2 at camera + (`36Ch`, `36Eh`),
  `attr |= 0x382`, size 0x20, data = resource 0x4D (`sub_11F9E`) + `k*0x480`.

## Unresolved

- **F1 id 0x48 -> colour word 0x0707.** The value is not of the
  `(bank+8)<<4` form; what the renderer does with it (shadow / special blit)
  was not traced.
- **E9 / `ds:1B46h`.** Only writers exist in the listing (E9 and
  `seg000:2A72`); the consumer may be an access IDA did not resolve. Unused
  in 064.bin.
- **EC / `ds:318h` bit 2.** No reader found.
- **F6 `ds:37Ah`** (cleared) and **F7 `ds:304h`** (non-zero suppresses the
  save): game-state meaning not traced.
- **F3/F4/F5** call into the AIL sound driver by ordinal (0xAA, 0xAB, 0xB1,
  `sub_196D5`, `sub_19188`); the exact driver semantics are inferred from the
  ordinals and from the menu script, not read.
- **Part arrays `A52h`/`B52h`, part pool allocation for record flag bit 6**
  (`loc_11652`, top-down search) and the renderer's use of the redraw flags
  were not read; they do not affect any opcode's operands.
- **Modes 6/7** in a mode byte would jump past the `off_17575` table. Never
  used.

## Verification against 064.bin

Method: a recursive-descent disassembler over all 0x56 actor records
(entry = record offset + 3, page from record +5; part count n = record byte
+2 & 0x3F). It follows gotos, calls, far calls, all conditional targets,
CC/F8 targets A and B, and the anim entry points set by 0x15/0x16 (decoded
with the owning record's n). Lengths for 0x00-0xBF come from my own reading
of those handlers (operand helpers `sub_174C6`..`sub_1753E`,
`sub_17B59`/`17C38`/`17CFC`/`17D5A`/`17DB8`, write-back `loc_175D6`, the
`60C8h` branch table); spot checks against ops_00-3F and ops_80-BF agree.
Results:

- 14 338 distinct instructions decoded; **0 instructions overlap the start of
  another**; no unknown opcode or invalid mode anywhere.
- 614 distinct per-part opcode sites; **each is reached from records with a
  single part count**, and none from a record with n = 0.
- Per-part opcodes, D6, D7 and DC occur only in animation scripts; C0, E4,
  E5, F0, F4-F6, FA, FB only in main scripts.
- In-range opcodes seen: C0 C2 CC CD CE CF D0 D1 D2 D3 D5 D6 D7 D8 DB DC DD
  DE DF E0 E1 E2 E3 E4 E5 E7 EA EB EC ED EE F0 F1 F2 F3 F4 F5 F6 F7 F8 FA FB.
  Never reached: C1 C3 C4-CB D4 D9 DA E6 E8 E9 EF F9 FC-FF.
- The only stops are the filtered `CF`/`E0` in record 82 (below). Decoding
  them with n bytes instead of m produces garbage (C4, FC, FE, FF "appear"),
  which is why the filter rule matters.

### Script 1: record 0 (Kyle, n = 3, main page 0x82, anim page 0x83)

```
82:85B3  71 04 02 B9 85        if (acc == [204h]) goto 85B9    ; skips the 1-byte 07
82:85B8  07
82:85B9  F7                    save level password (1 byte)
82:85BA  16 83 6C 80           anim script = 83:806C
82:85BE  2A                    run anim now
...
83:806C  DD 00 00 01           sizes: part0 small, part1 small, part2 large  (1+3)
83:8070  CF 00 04 08           cells 0, 4, 8 -> three 16x16 parts       (1+3)
83:8074  04 5D 80              call 805D
83:805D  D3 E4 FF E4 FF F4 FF  dy = -28, -28, -12                        (1+6)
83:8064  D1 F0 FF 00 00 F0 FF  dx = -16, 0, -16                          (1+6)
83:806B  06                    return -> 8077
83:8077  F1 4F 00              palette 0x4F
83:807A  DF 42 00              sprite bank 0x42
83:807D  D6                    end anim tick
83:807E  04 5D 80              call 805D (re-place parts)
83:8081  3B FB FF / 4D 2E      acc = 0xFFFB; self.field(17h) &= acc
83:8086  DC 00 00 00           frame 0                                   (mode 0: 2+2)
...
83:80A1  DC 00 8B 01 / D7 02   frame 395, wait 2
83:80A7  DC 00 8C 01 / D7 02   frame 396, wait 2
```

The `06` at 806B lands exactly after the two 7-byte part lists, so n = 3 and
s16 offsets are confirmed; the return lands on F1 at 8077.

### Script 2: record 82 (8-part meter, n = 8, page 0x83) - filtered parts

```
83:ADBB  D3 F8 FF ×8                       dy = -8 for all 8 parts     (1+16)
83:ADCC  D1 88 FF 98 FF ... F8 FF          dx = -120, -104, ..., -8    (1+16)
83:ADDD  F1 28 01                          palette 0x128
83:ADE0  CF 0C ×8                          all parts cell 12           (1+8)
83:ADE9  DB 80 40 20 10 08 04 02 01        part i group = 0x80 >> i    (1+8)
83:ADF2  3B 00 00 / 71 2A 02 54 CB
83:ADFA  04 22 AE  ...  83:AE0F 04 25 AF   8 calls, one per part
83:AE22  D5 80                             filter = part 0
83:AE24  3D 2A 02 / 65 20 00 4D AF         if [22Ah] >= 0x20 goto AF4D
83:AE2C  3D 2A 02 / 6F 1F 00 50 AF         if == 0x1F goto AF50 ... (53, 56)
83:AE44  03 4A AF                          else goto AF4A
...                                        (same for D5 40, 20, ..., 01)
83:AF4A  E0 01   06                        hide the one filtered part   (1+1)
83:AF4D  CF 0C   06                        cell 12                      (1+1)
83:AF50  CF 08   06
83:AF53  CF 04   06
83:AF56  CF 00   06
```

Each filter is a single bit and DB gave each part a single bit, so exactly
one part matches and the filtered CF/E0 take **one** operand byte: the `06`
returns sit at +2. With n = 8 bytes they would swallow the following
handlers. Simulating the DB group masks, the record decodes with no issue
and no overlap.

### Script 3: record 21 (title menu, page 0x83)

```
83:A5E0  15 4D A8                          anim = A84D
83:A5E3  EA                                clear text layer
83:A5E4  E7 00 00 00 00 00 00 0C 00 0C 00  text 0, style 0, col 12, row 12 (3+2+2+2+2)
83:A5EF  EB                                portrait update
83:A5F0  2A
...
83:A66E  70 14 D6 A6                       if (acc == self.field(0Ah)) goto A6D6
83:A672  FA                                quit to DOS
83:A6D6  FB                                load saved password
83:A6D7  F6                                password -> next level, start pos
83:A6D8  03 26 A8
83:A6E3  EA
83:A6E4  E7 00 62 00 00 00 00 08 00 0C 00  text 0x62 at col 8, row 12
83:A6EF  EB
...
83:A786  3B 00 00 / 71 7E 03 99 A7         if (music flag [37Eh] == 0) goto A799
83:A78E  F5 00                             stop music                  (1+1)
83:A790  3B 00 00 / 42 7E 03 / 03 F0 A6    [37Eh] = 0
83:A799  3B 01 00 / 42 7E 03               [37Eh] = 1
83:A79F  F4 00                             restart music               (1+1)
83:A7A1  03 F0 A6
```

The branch at A789 lands on A799 exactly 16 bytes later, which requires
`F5` = 2 bytes; F4 at A79F is followed by the goto at A7A1. FB/F6/FA are
1 byte (A6D6, A6D7, then the goto at A6D8). E7 with mode bytes 00/00 takes
4 immediates (11 bytes total), and the next opcode (EB) follows directly.
