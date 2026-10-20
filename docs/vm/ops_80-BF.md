# Opcodes 0x80-0xBF

Source: handlers in `re/handlers.txt` and the full listing
`re/BTHORNE.EXE.asm`, including every subroutine they call. Each operand
byte count below comes from the code that advances `bx`.

## Notation

- `ACC` is the word at `ds:38h`. Ops 0x3B/0x3C/0x3D load it, 0x41/0x42 store
  it, and almost every op in this range compares against it or uses it.
- `self` is the current slot (`ds:2Ah`). `tgt` is `self.[17FAh]` (target
  slot) and `rel` is `self.[1822h]` (related slot).
- **Field operand `f8` (u8).** The handlers use the byte as a *byte offset*
  into the word table at `ds:28CAh` (`mov si,[si+28CAh]` with no shift).
  That table holds `0, 0x28, 0x50, ...`, so a byte `b` selects field
  `k = b/2`, at address `0x13C2 + (b/2)*0x28 + slot`. Every field byte seen in
  064.bin is even. For example, `08` is flags (1462h), `14` is the spawn param
  (1552h), `1C` is x (15F2h), `1E` is y (161Ah) and `2A` is field 21 (170Ah).
  An odd byte would read a misaligned table word; the scripts never use one.
  - `F(b)` = self field, `T(b)` = field of slot `tgt`, `R(b)` = field of
    slot `rel`.
- **Bit operand `bit8` (u8).** This is also a byte offset, into word tables
  at `ds:288Ah` (`1<<i`, i = 0..15) and `ds:28AAh` (`~(1<<i)`). So
  `MASK(b) = 1 << (b/2)` and `NMASK(b) = ~MASK(b)`. For example, `0C` is
  0x0040 and `1E` is 0x8000.
- `G(a)` is the word at `ds:a` (from an `addr16` operand).
- `goto(t)` is `sub_16141`: `ip = t` in the current segment. Its check
  `cmp bx,8000h / jl` is a signed compare against -32768, so it never fires
  (the error path is dead).
- `call(t)` is `sub_16135`: link `ds:30h = ip_after_target_word`,
  `ds:32h = es`, then `goto(t)`. The link has one level, the same as op 0x04.
- `skip` is `ip += 2`: step over the unused target word.
- Operand-mode helpers used by the compare families. The number in brackets is
  the script bytes consumed:
  `sub_17581` imm u16 [2], `sub_17588` F(u8) [1], `sub_1759A` G(addr16) [2],
  `sub_175A3` T(u8) [1], `sub_175BB` R(u8) [1].
- Bit-test helpers (`bit8` first, then the source operand). Each returns
  0 or 1 = `(src & MASK(bit8)) != 0`:
  `sub_174C6` src = imm u16 [1+2], `sub_174DB` src = F(u8) [1+1],
  `sub_174FD` src = G(addr16) [1+2], `sub_17516` src = T(u8) [1+1],
  `sub_1753E` src = R(u8) [1+1].

No opcode in this range ends the actor's turn (none does `pop ax`). All of
them return to the interpreter loop.

## Families

| Ops | Family |
|---|---|
| 0x80-0x82 | last three members of the 0x7E-0x82 family "goto if ACC < operand (signed)"; 0x7E = imm, 0x7F = F |
| 0x83-0x87 | call if ACC == operand (modes imm, F, G, T, R) |
| 0x88-0x8C | call if ACC != operand (same mode order) |
| 0x8D-0x90 | facing-signed subtract: if self flag 0x40 then dest -= ACC else dest += ACC |
| 0x91-0x94 | facing-signed add: if self flag 0x40 then dest += ACC else dest -= ACC |
| 0x95 | tgt = ACC |
| 0x96-0x9A | ACC = bit test (0/1), source modes imm, F, G, T, R |
| 0x9B | ACC = random & 1 |
| 0x9C-0x9F | write ACC (as a boolean) into one bit of F, G, T, R |
| 0xA0-0xA4 | goto if bit test == ACC |
| 0xA5-0xA9 | goto if bit test != ACC |
| 0xAA-0xAE | call if bit test == ACC |
| 0xAF-0xB3 | call if bit test != ACC |
| 0xB4-0xB7 | goto if slot 0 is touching self's edge (up/down/front/back) and in class list |
| 0xB8-0xBB | same probes, goto if NOT found |
| 0xBC-0xBF | write ACC into the actor's persistent spawn record (x, y, flags, param) |

## Opcode table

| Op | Handler | Encoding (bytes after opcode) | Effect | Notes |
|---|---|---|---|---|
| 0x80 | sub_16A25 | `addr16`, `u16 target` (4) | `if ((int16)ACC + (int16)(-v) < 0) goto(target) else skip;` with `v = G(addr16)` | Signed "ACC < v" done as `ACC + (-v)` with SF^OF. Edge case: v = 0x8000 always branches (see below). Seen in 064.bin: `80 04 02 e3 be` (G(0204h)), `80 4e 1b ce d3` (G(1B4Eh), camera x). High |
| 0x81 | sub_16A3D | `u8 f8`, `u16 target` (3) | same with `v = T(f8)` | High |
| 0x82 | sub_16A55 | `u8 f8`, `u16 target` (3) | same with `v = R(f8)` | High |
| 0x83 | sub_16B45 | `u16 imm`, `u16 target` (4) | `if (ACC == imm) call(target) else skip;` | Link = address after the target word, segment = es. Seen: `83 4c 00 91 c4`. High |
| 0x84 | sub_16B55 | `u8 f8`, `u16 target` (3) | `if (ACC == F(f8)) call(target) else skip;` | Seen: `84 14 17 cc`. High |
| 0x85 | sub_16B65 | `addr16`, `u16 target` (4) | `if (ACC == G(addr16)) call(target) else skip;` | High |
| 0x86 | sub_16B75 | `u8 f8`, `u16 target` (3) | `if (ACC == T(f8)) call(target) else skip;` | High |
| 0x87 | sub_16B85 | `u8 f8`, `u16 target` (3) | `if (ACC == R(f8)) call(target) else skip;` | High |
| 0x88 | sub_16BA5 | `u16 imm`, `u16 target` (4) | `if (ACC != imm) call(target) else skip;` | High |
| 0x89 | sub_16BB5 | `u8 f8`, `u16 target` (3) | `if (ACC != F(f8)) call(target) else skip;` | Most common op in this range. The idiom `3b 00 00` / `89 2a XXXX` calls a handler when field 21 (170Ah) != 0. High |
| 0x8A | sub_16BC5 | `addr16`, `u16 target` (4) | `if (ACC != G(addr16)) call(target) else skip;` | High |
| 0x8B | sub_16BD5 | `u8 f8`, `u16 target` (3) | `if (ACC != T(f8)) call(target) else skip;` | High |
| 0x8C | sub_16BE5 | `u8 f8`, `u16 target` (3) | `if (ACC != R(f8)) call(target) else skip;` | High |
| 0x8D | sub_16553 | `u8 f8` (1) | `if (self.flags & 0x40) F(f8) -= ACC; else F(f8) += ACC;` | Jumps to sub_164FC / sub_164A5 (the bodies of ops 0x49 / 0x45). High |
| 0x8E | sub_16564 | `addr16` (2) | `if (self.flags & 0x40) G(a) -= ACC; else G(a) += ACC;` | sub_16511 / sub_164BA (= ops 0x4A / 0x46). High |
| 0x8F | sub_16575 | `u8 f8` (1) | `if (self.flags & 0x40) T(f8) -= ACC; else T(f8) += ACC;` | Tests **self's** flag, not the target's. sub_1651D / sub_164C6 (= 0x4B / 0x47). High |
| 0x90 | sub_16586 | `u8 f8` (1) | `if (self.flags & 0x40) R(f8) -= ACC; else R(f8) += ACC;` | Tests self's flag. sub_16538 / sub_164E1 (= 0x4C / 0x48). High |
| 0x91 | sub_16597 | `u8 f8` (1) | `if (self.flags & 0x40) F(f8) += ACC; else F(f8) -= ACC;` | The extra `mov ax,[si+1462h]` is a dead load. Seen: `91 28` (field 20, 16E2h). High |
| 0x92 | sub_165AD | `addr16` (2) | `if (self.flags & 0x40) G(a) += ACC; else G(a) -= ACC;` | High |
| 0x93 | sub_165BF | `u8 f8` (1) | `if (self.flags & 0x40) T(f8) += ACC; else T(f8) -= ACC;` | High |
| 0x94 | sub_165D1 | `u8 f8` (1) | `if (self.flags & 0x40) R(f8) += ACC; else R(f8) -= ACC;` | Dead load as in 0x91. High |
| 0x95 | sub_16442 | none (0) | `self.[17FAh] = ACC;` (set target slot) | Typical use: `3d 56 03` (ACC = ds:356h, slot found by 0xB4-0xBB) then `95`. High |
| 0x96 | sub_16C05 | `u8 bit8`, `u16 imm` (3) | `ACC = (imm & MASK(bit8)) != 0;` | Scripts use it as "ACC = 0/1": `96 00 01 00` gives ACC = 1, `96 00 00 00` gives ACC = 0. High |
| 0x97 | sub_16C0C | `u8 bit8`, `u8 f8` (2) | `ACC = (F(f8) & MASK(bit8)) != 0;` | Seen: `97 0c 08` gives ACC = facing bit (flags & 0x40). High |
| 0x98 | sub_16C13 | `u8 bit8`, `addr16` (3) | `ACC = (G(addr16) & MASK(bit8)) != 0;` | Seen: `98 0c 5c 03` (bit 6 of ds:035Ch). High |
| 0x99 | sub_16C1A | `u8 bit8`, `u8 f8` (2) | `ACC = (T(f8) & MASK(bit8)) != 0;` | High |
| 0x9A | sub_16C21 | `u8 bit8`, `u8 f8` (2) | `ACC = (R(f8) & MASK(bit8)) != 0;` | High |
| 0x9B | sub_16C28 | none (0) | `ACC = rand() & 1;` | `sub_10C9F` = game RNG (the same as operand mode 5 and op 0x40). High |
| 0x9C | sub_16C32 | `u8 bit8`, `u8 f8` (2) | `if (ACC) ACC = MASK(bit8); F(f8) = (F(f8) & NMASK(bit8)) \| ACC;` | Sets the bit if ACC != 0, clears it if ACC == 0. **Side effect:** a nonzero ACC is replaced by the mask. Seen: `9c 1e 2e` (bit 15 of field 23, 175Ah). High |
| 0x9D | sub_16C6A | `u8 bit8`, `addr16` (3) | same on `G(addr16)` | High |
| 0x9E | sub_16C97 | `u8 bit8`, `u8 f8` (2) | same on `T(f8)` | High |
| 0x9F | sub_16CD5 | `u8 bit8`, `u8 f8` (2) | same on `R(f8)` | High |
| 0xA0 | sub_16D13 | `u8 bit8`, `u16 imm`, `u16 target` (5) | `b = (imm & MASK(bit8)) != 0; if (b == ACC) goto(target) else skip;` | Compares the 0/1 result with the full ACC word. If ACC is not 0 or 1, the "==" ops never branch and the "!=" ops always branch. Seen: `a0 00 01 00 XXXX` = "goto if ACC == 1". High |
| 0xA1 | sub_16D23 | `u8 bit8`, `u8 f8`, `u16 target` (4) | `b = (F(f8) & MASK(bit8)) != 0; if (b == ACC) goto(target) else skip;` | Seen: `a1 1e 2e 06 9f`. High |
| 0xA2 | sub_16D33 | `u8 bit8`, `addr16`, `u16 target` (5) | same with `G(addr16)` | High |
| 0xA3 | sub_16D43 | `u8 bit8`, `u8 f8`, `u16 target` (4) | same with `T(f8)` | High |
| 0xA4 | sub_16D53 | `u8 bit8`, `u8 f8`, `u16 target` (4) | same with `R(f8)` | High |
| 0xA5 | sub_16D76 | `u8 bit8`, `u16 imm`, `u16 target` (5) | `if (bit(imm) != ACC) goto(target) else skip;` | High |
| 0xA6 | sub_16D86 | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(F(f8)) != ACC) goto(target) else skip;` | High |
| 0xA7 | sub_16D96 | `u8 bit8`, `addr16`, `u16 target` (5) | `if (bit(G(a)) != ACC) goto(target) else skip;` | High |
| 0xA8 | sub_16DA6 | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(T(f8)) != ACC) goto(target) else skip;` | High |
| 0xA9 | sub_16DB6 | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(R(f8)) != ACC) goto(target) else skip;` | High |
| 0xAA | sub_16DD9 | `u8 bit8`, `u16 imm`, `u16 target` (5) | `if (bit(imm) == ACC) call(target) else skip;` | High |
| 0xAB | sub_16DE9 | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(F(f8)) == ACC) call(target) else skip;` | High |
| 0xAC | sub_16DF9 | `u8 bit8`, `addr16`, `u16 target` (5) | `if (bit(G(a)) == ACC) call(target) else skip;` | High |
| 0xAD | sub_16E09 | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(T(f8)) == ACC) call(target) else skip;` | High |
| 0xAE | sub_16E19 | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(R(f8)) == ACC) call(target) else skip;` | High |
| 0xAF | sub_16E3C | `u8 bit8`, `u16 imm`, `u16 target` (5) | `if (bit(imm) != ACC) call(target) else skip;` | High |
| 0xB0 | sub_16E4C | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(F(f8)) != ACC) call(target) else skip;` | High |
| 0xB1 | sub_16E5C | `u8 bit8`, `addr16`, `u16 target` (5) | `if (bit(G(a)) != ACC) call(target) else skip;` | High |
| 0xB2 | sub_16E6C | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(T(f8)) != ACC) call(target) else skip;` | High |
| 0xB3 | sub_16E7C | `u8 bit8`, `u8 f8`, `u16 target` (4) | `if (bit(R(f8)) != ACC) call(target) else skip;` | High |
| 0xB4 | sub_1722B | `u8 list`, `u16 target` (3) | `if (probe_up(list)) goto(target) else skip;` | See "Contact probes". Seen: `b4 24 e8 af` (list 0x24 = {0x2F}). High |
| 0xB5 | sub_17258 | `u8 list`, `u16 target` (3) | `if (probe_down(list)) goto(target) else skip;` | High |
| 0xB6 | sub_172BE | `u8 list`, `u16 target` (3) | `if (self.flags & 0x40 ? probe_right(list) : probe_left(list)) goto(target) else skip;` | High |
| 0xB7 | sub_17285 | `u8 list`, `u16 target` (3) | `if (self.flags & 0x40 ? probe_left(list) : probe_right(list)) goto(target) else skip;` | High |
| 0xB8 | sub_1722F | `u8 list`, `u16 target` (3) | `if (!probe_up(list)) goto(target) else skip;` | High |
| 0xB9 | sub_1725C | `u8 list`, `u16 target` (3) | `if (!probe_down(list)) goto(target) else skip;` | High |
| 0xBA | sub_172C2 | `u8 list`, `u16 target` (3) | `if (!(flags & 0x40 ? probe_right : probe_left)(list)) goto(target) else skip;` | High |
| 0xBB | sub_17289 | `u8 list`, `u16 target` (3) | `if (!(flags & 0x40 ? probe_left : probe_right)(list)) goto(target) else skip;` | High |
| 0xBC | sub_17311 | none (0) | `spawn_record_store(+0, ACC)` (x) | See "Spawn record store". Seen: `3c 1c bc` (ACC = x). High |
| 0xBD | sub_17316 | none (0) | `spawn_record_store(+2, ACC)` (y) | Seen: `3c 1e bd` (ACC = y). High |
| 0xBE | sub_17320 | none (0) | `ACC &= 0xCDFF; spawn_record_store(+0Ah, ACC)` (flags) | Clears bits 0x0200, 0x1000 and 0x2000 in ACC itself (written back to ds:38h). Seen: `3c 08 be` (ACC = flags). High |
| 0xBF | sub_1731B | none (0) | `spawn_record_store(+0Ch, ACC)` (param) | Seen: `3c 14 bf` (ACC = spawn param). High |

### 0x80-0x82 compare detail

```c
// exact emulation of: neg ax; add ax, ACC; branch on SF^OF
int32_t s = (int32_t)(int16_t)ACC + (int32_t)(int16_t)(uint16_t)(0u - v);
if (s < 0) ip = read_u16(ip); else ip += 2;
```

This equals `(int16)ACC < (int16)v` for every v except 0x8000. For v = 0x8000,
`-v` is 0x8000, and the op branches for every ACC.

### Contact probes (0xB4-0xBB)

Common prologue, in this order:

```c
G(356h) = 0xFFFF;                 // "found slot" = none
saved = G(32Ch); G(32Ch) = 2;     // actor scan limit -> only slot 0 is scanned
list = read_u8(ip++);
found = probe_X(self, list);      // sets CF
G(32Ch) = saved;
if (found ^ negate) ip = read_u16(ip); else ip += 2;   // ds:60C8h table: [0]=0x630E, [2]=0x6318
```

`ds:32Ch` is the end of the active slot range (`2 * actor count`, used by the
scheduler `sub_15FF1`/`sub_1601A`). Because it is forced to 2, these ops only
test **slot 0**, which is Kyle. Ops 0x1B-0x22 use the same probe routines
without the override, so they scan all actors.

`probe_X` (sub_18331 up, sub_1833E down, sub_1829A left, sub_182A7 right)
checks each slot `o` in `[0, G(32Ch))`, in steps of 2:

1. Skip `o` if `o.[125Ah] == 0` (slot not in use) or `o == self`.
2. Class membership: `c = low byte of o.[1642h]` (field 16). `list` is a byte
   offset into a table at `ds:60CCh` of sorted, 0xFF-terminated byte lists.
   Walk the list: `c < e` means not a member, `c == e` means member, otherwise
   move to the next entry. A class 0xFF would match any list. List starts
   are at 00 {0}, 02 {1,2}, 05 {1}, 07 {2}, 09 {1,2,4,5,7,8,A,B,C},
   13 {1,2,4,5,A,B,C}, 1B {3,4}, 1E {4}, 20 {1,2,5}, 24 {2F}, 26 {2F,30},
   29 {2E}, 2B {6}, 2D {7}, 2F {8}, 31 {7,8}, 34 {1,2,20}, 38 {21}, 3A {28},
   3C {10}, 3E {11}, 40 {12}, 42 {A,B,C}, 46 {A,B}, 49 {A,C}, 4C {1,2,1C},
   50 {1,2,11}.
3. Geometry. All compares are signed 16-bit. `top/bottom/left/right` are
   fields 0..3, and `dy` = `self.[17D2h]` (field 26, the y offset written by
   ops 0x13/0x14/0x38):
   - up: `P = self.top - 1`; down: `P = self.bottom + 1`. Hit if
     `o.top <= P && P - 1 < o.bottom && (int16)(self.right - o.left) >= 0 &&
     (int16)(o.right - self.left) >= 0`.
   - left: `P = self.left - 1`; right: `P = self.right + 1`. Hit if
     `o.left <= P && P - 1 < o.right && self.bottom - dy >= o.top &&
     self.top - dy <= o.bottom`.
4. On the first hit: `G(354h) = o.[1642h]` (full word), `G(356h) = o`, return
   found. Otherwise return not found, with G(356h) still 0xFFFF.

Scratch globals clobbered: `ds:12h`, `ds:14h`, `ds:18h`.

### Spawn record store (0xBC-0xBF, `loc_1732C`)

Records are at `ds:1D24h`, 14 bytes each, ending with a record whose `+0`
word is 0xFFFF. Layout: `+0` x, `+2` y, `+4` = actor field 30 (1872h, half
width), `+6` = field 31 (189Ah, half height), `+8` = owner actor type
(0 = free; op 0xF2 frees it), `+0Ah` flags, `+0Ch` param. `sub_11380` uses
+0..+6 for on-screen culling.

```c
void spawn_record_store(int off, uint16_t acc) {   // off = 0, 2, 0x0A, 0x0C
    int idx = (int16_t)self.[157Ah];
    if (idx >= 0) {
        rec[idx].w[off] = acc;
        rec[idx].half_w = self.[1872h];
        rec[idx].half_h = self.[189Ah];
        return;
    }
    for (i = 0; rec[i].x != 0xFFFF; i++)        // allocate first free record
        if (rec[i].owner_type == 0) {
            self.[157Ah] = i;
            rec[i].owner_type = self.[15A2h];
            rec[i].half_w = self.[1872h];
            rec[i].half_h = self.[189Ah];
            rec[i].w[off] = acc;
            return;
        }
    // table full: nothing is stored
}
```

The allocation path computes `i` with `div cx` (cx = 14) but never clears
`dx`, so it divides `dx:ax`. The interpreter loop does not touch `dx`. Ops
such as 0x65 do (`mov dx, ACC`), so a stale nonzero `dx` would give a wrong
index or a divide fault. A port should use `i` directly. If 0xBC stores
ACC = 0xFFFF into a record, that record becomes the table terminator.

## Unresolved or partly resolved

All 64 opcodes have exact encodings and effects. The open points are about
meaning, not decoding:

- **Field 16 (1642h)** is the "class" that the probe lists match against.
  Its relation to actor type (15A2h) is unconfirmed.
- **ds:035Ch, ds:0204h, field 21 (170Ah), field 20 (16E2h), field 23
  (175Ah)**, as used in the samples, are unnamed globals and fields.
- **0xBC-0xBF allocation path**: whether `dx` is always 0 at run time (see
  the `div` note above) is not proven. In practice the record usually exists
  already (`157Ah >= 0`), and then no `div` runs.
- **0x80-0x82 with v = 0x8000**: the branch is taken for every ACC, as
  documented above. No script uses this value.

## Verification against 064.bin

The verification used a decoder written for this check. It covers this range
plus the lengths of the 0x00-0x7F and 0xC0-0xFF ops met on the way
(mode-byte ops are decoded from their mode bits). It walks all 0x56 actor
records recursively: it starts at `offset` and `offset+3` and follows every
goto, call and conditional target, including the targets of 0x80-0x8C,
0xA0-0xBB and the far call 0x05. It stops on ops whose length depends on
deeper subroutines (0x0F, 0x10, 0x23, 0x29, 0x2D-0x30, 0x33, 0xCC, 0xE3,
0xE6, 0xE7, 0xEA, 0xF0, and others).

Results:

- 3460 decoded instruction starts, with **0 overlaps**: no decoded instruction
  starts inside another one. Every branch target in this range landed on an
  instruction boundary that was consistent with the linear decode.
- 162 instances of ops 0x80-0xBF were reached (0x80 x4, 0x83, 0x84, 0x89 x38,
  0x91 x3, 0x95 x9, 0x96 x21, 0x97 x5, 0x98 x18, 0x9C x3, 0xA0 x18, 0xA1 x14,
  0xA2 x5, 0xA6 x4, 0xB4, 0xBC x4, 0xBD x4, 0xBE x3, 0xBF x6). Every field byte
  is even. The one probe list operand (0x24) is a list start.

### Script 1: record 1 (offset 0xAFB5, page 0x83), entry 0xAFB8

```
afb8: 15 5c b0        anim script = b05c
afbb: 2a              run anim
afbc: 3b 00 10        ACC = 0x1000
afbf: 51 08           flags |= ACC
afc1: 3b 01 00        ACC = 1
afc4: 41 20           F(0x20) = ACC
afc6: 00              yield
afc7: 01              nop
afc8: b4 24 e8 af     if slot 0 (class in {2F}) touches self's top edge: goto afe8
afcc: 3b 02 00        ACC = 2
afcf: 70 2c fc af
afd3: 03 c6 af        goto afc6
afd6: 3b ff ff
afd9: 71 56 03 52 cc  (compare with G(0356h))
afde: 3d 56 03        ACC = G(0356h)   ; slot found by the probe
afe1: 95              tgt = ACC
afe2: 3b 01 00
afe5: 47 2c
afe7: 06              return
afe8: 3d 14 02        <- 0xB4 target, an instruction boundary
```

The 3-byte B4 keeps the stream aligned: `3b 02 00` follows. The code also
uses the B4 side effect, reading back `ds:356h` and passing it to 0x95.

### Script 2: record 3 (offset 0xB0E4, page 0x83), entry 0xB0E7

```
b11e: 3b 00 00        ACC = 0
b121: 89 2a c2 b1     if (F(0x2A) != 0) call b1c2
b125: 2a              run anim
b126: 00 01           yield; nop
b128: 3b 00 00
b12b: 75 1a 1e b1
b12f: 3b 00 00
b132: 41 12
b134: 15 fb b1
b137: 3b 00 00
b13a: 89 2a c2 b1     if (F(0x2A) != 0) call b1c2
b13e: 2a
b13f: 00 01
b141: 03 37 b1        goto b137
```

0x89 takes 3 operand bytes, so the stream resumes on `2a 00 01` (the
run-animation, yield, nop idiom) both times.

### Script 3: records 13-16 (offset 0x9D39, page 0x82), shared code at 0xA0C5 and 0x9BB2

```
a0c5: 3b 03 00
a0c8: 51 2e           F(0x2E) |= 3
a0ca: 97 0c 08        ACC = (flags & 0x40) != 0      ; facing
a0cd: 9c 1e 2e        F(0x2E) bit15 = ACC            ; remember facing
a0d0: 3b 00 80
...
a105: 97 0c 08        ACC = facing
a108: a1 1e 2e 06 9f  if (F(0x2E) bit15 == ACC) goto 9f06
a10d: 3c 2e
...
a123: 96 00 00 00     ACC = 0
a127: a1 0a 2e 75 9b  if (F(0x2E) bit5 == 0) goto 9b75
a12c: 3b 08 00
...
a13c: 96 00 00 00     ACC = 0
a140: a1 08 2e 75 9b  if (F(0x2E) bit4 == 0) goto 9b75
a145: 3b ff ff

9bb2: 3c 1c           ACC = x
9bb4: bc              record.x = ACC
9bb5: 3c 1e           ACC = y
9bb7: bd              record.y = ACC
9bb8: 3c 0a
9bba: 51 14           param |= F(0x0A)
9bbc: 3c 14           ACC = param
9bbe: bf              record.param = ACC
9bbf: 3c 08           ACC = flags
9bc1: be              record.flags = ACC & 0xCDFF
9bc2: 0e              end turn
```

The 97/9C pair stores and later re-tests the facing bit (2-byte operands,
`bit8` then `f8`). 0x96 with an immediate is the scripts' way to load
ACC = 0 or 1, and its 3 operand bytes keep `a1 ...` aligned. The BC/BD/BF/BE
block saves x, y, param and flags into the spawn record before the actor
ends its turn. This confirms that these four ops take no operand bytes and
gives the meaning of record offsets +0, +2, +0Ch and +0Ah.

The decoder never diverged inside this range. The only misalignment found
during the check came from ops outside it (0xCD/0xCE take
`mode, addr16, operand(mode bits 3-5)`, not a fixed 3 bytes). Fixing that
removed the misalignment.
