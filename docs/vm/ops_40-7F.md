# Opcodes 0x40-0x7F

Source: `funcs_160A1` in `BTHORNE.EXE.asm` (handler addresses cross-checked
against the table, index 0x40 = `sub_1643B` ... 0x7F = `sub_16A0D`) and every
helper they call: `sub_10C9F`, `sub_16141`, `sub_17581`, `sub_17588`,
`sub_1759A`, `sub_175A3`, `sub_175BB`, and the word table at `ds:28CAh`.

## The value register (accumulator)

All opcodes in this range work on one 16-bit register at **`ds:38h`** (call it
`acc`). It is loaded by 0x3B-0x3F (imm16 / self field / global / target field /
related field) and by 0x40 (random). 0x41-0x64 store `acc` into, or combine it
with, a destination. 0x65-0x82 compare `acc` with an operand and branch. No
opcode in 0x40-0x7F changes `acc` except 0x40. None of them ends the actor's
turn: all return normally to the interpreter loop.

## Operand notation

| Notation | Bytes | Meaning |
|---|---|---|
| `fld8` | 1 | Field selector `b`. The handler does `mov si,[b+28CAh]`, so `b` is a **byte offset** into the word table at `ds:28CAh` (entry j = j*0x28). The field index is `k = b/2`, so the script byte is **2k**, not k. Field address for slot `s` is `0x13C2 + (b/2)*0x28 + s`. Examples: `08` = flags `1462h`, `1C` = x `15F2h`, `1E` = y `161Ah`, `34` = target slot `17FAh`. Odd `b` would read a misaligned table word; no script does this (see verification). |
| `addr16` | 2 | Little-endian address of a global word `ds:addr16`. |
| `imm16` | 2 | Little-endian immediate. |
| `tgt16` | 2 | Little-endian branch target: an absolute offset in the **current** script segment (`es` is not changed). |

Helpers used below (all 16-bit, wraparound arithmetic):

```c
uint16_t& SELF(b)  = W[0x13C2 + tab28CA[b] + self];          // self = ds:2Ah
uint16_t& TGT(b)   = W[0x13C2 + tab28CA[b] + W[0x17FA + self]];
uint16_t& REL(b)   = W[0x13C2 + tab28CA[b] + W[0x1822 + self]];
uint16_t& GLOB(a)  = W[a];
// tab28CA[b] == (b/2)*0x28 for even b
void goto_(tgt16)  { ip = tgt16; }   // sub_16141 (same segment)
void skip()        { ip += 2; }      // not taken: step over tgt16
```

`sub_16141` also has `cmp bx,8000h / jl error(0x72)`. `jl` is a signed test,
and no 16-bit value is signed-less-than 0x8000 (-32768), so the error path is
dead code. A port can ignore it.

## Table

| Op | Handler | Encoding | Effect | Notes |
|---|---|---|---|---|
| 40 | sub_1643B | (none) | `acc = rand16();` | Calls `sub_10C9F` (also operand mode 5). Consumes no script bytes. RNG state is the 10 bytes `ds:4Eh..57h`; algorithm below. High. |
| 41 | sub_1644E | `fld8` | `SELF(b) = acc;` | High. |
| 42 | sub_16463 | `addr16` | `GLOB(a) = acc;` | High. |
| 43 | sub_1646F | `fld8` | `TGT(b) = acc;` | Target slot from self field `17FAh`. High. |
| 44 | sub_1648A | `fld8` | `REL(b) = acc;` | Related slot from self field `1822h`. High. |
| 45 | sub_164A5 | `fld8` | `SELF(b) += acc;` | High. |
| 46 | sub_164BA | `addr16` | `GLOB(a) += acc;` | High. |
| 47 | sub_164C6 | `fld8` | `TGT(b) += acc;` | High. |
| 48 | sub_164E1 | `fld8` | `REL(b) += acc;` | High. |
| 49 | sub_164FC | `fld8` | `SELF(b) -= acc;` | High. |
| 4A | sub_16511 | `addr16` | `GLOB(a) -= acc;` | High. |
| 4B | sub_1651D | `fld8` | `TGT(b) -= acc;` | High. |
| 4C | sub_16538 | `fld8` | `REL(b) -= acc;` | High. |
| 4D | sub_16677 | `fld8` | `SELF(b) &= acc;` | High. |
| 4E | sub_1668C | `addr16` | `GLOB(a) &= acc;` | High. |
| 4F | sub_16698 | `fld8` | `TGT(b) &= acc;` | High. |
| 50 | sub_166B3 | `fld8` | `REL(b) &= acc;` | High. |
| 51 | sub_166CE | `fld8` | `SELF(b) \|= acc;` | Often `51 08` = set bits in flags `1462h`. High. |
| 52 | sub_166E3 | `addr16` | `GLOB(a) \|= acc;` | High. |
| 53 | sub_166EF | `fld8` | `TGT(b) \|= acc;` | High. |
| 54 | sub_1670A | `fld8` | `REL(b) \|= acc;` | High. |
| 55 | sub_16725 | `fld8` | `SELF(b) ^= acc;` | High. |
| 56 | sub_1673A | `addr16` | `GLOB(a) ^= acc;` | High. |
| 57 | sub_16746 | `fld8` | `TGT(b) ^= acc;` | High. |
| 58 | sub_16761 | `fld8` | `REL(b) ^= acc;` | High. |
| 59 | sub_1677C | `fld8` | `SELF(b) = shl16(SELF(b), acc);` | `cx = acc`, `shl word,cl`. Shift helpers below. High. |
| 5A | sub_16792 | `addr16` | `GLOB(a) = shl16(GLOB(a), acc);` | High. |
| 5B | sub_1679F | `fld8` | `TGT(b) = shl16(TGT(b), acc);` | High. |
| 5C | sub_167BB | `fld8` | `REL(b) = shl16(REL(b), acc);` | High. |
| 5D | sub_167D7 | `fld8` | `SELF(b) = shr16(SELF(b), acc);` | Logical shift right. High. |
| 5E | sub_167ED | `addr16` | `GLOB(a) = shr16(GLOB(a), acc);` | High. |
| 5F | sub_167FA | `fld8` | `TGT(b) = shr16(TGT(b), acc);` | High. |
| 60 | sub_16816 | `fld8` | `REL(b) = shr16(REL(b), acc);` | High. |
| 61 | sub_16832 | `fld8` | `SELF(b) = sar16(SELF(b), acc);` | Arithmetic shift right. High. |
| 62 | sub_16848 | `addr16` | `GLOB(a) = sar16(GLOB(a), acc);` | High. |
| 63 | sub_16855 | `fld8` | `TGT(b) = sar16(TGT(b), acc);` | High. |
| 64 | sub_16871 | `fld8` | `REL(b) = sar16(REL(b), acc);` | High. |
| 65 | sub_1688D | `imm16`, `tgt16` | `if ((int16)acc >= (int16)imm) goto_(tgt); else skip();` | `cmp acc,v / jl skip`. High. |
| 66 | sub_1689F | `fld8`, `tgt16` | `if ((int16)acc >= (int16)SELF(b)) goto_(tgt); else skip();` | High. |
| 67 | sub_168B1 | `addr16`, `tgt16` | `if ((int16)acc >= (int16)GLOB(a)) goto_(tgt); else skip();` | High. |
| 68 | sub_168C3 | `fld8`, `tgt16` | `if ((int16)acc >= (int16)TGT(b)) goto_(tgt); else skip();` | High. |
| 69 | sub_168D5 | `fld8`, `tgt16` | `if ((int16)acc >= (int16)REL(b)) goto_(tgt); else skip();` | High. |
| 6A | sub_168F9 | `imm16`, `tgt16` | `if ((int16)acc < (int16)imm) goto_(tgt); else skip();` | `cmp acc,v / jge skip`. High. |
| 6B | sub_1690B | `fld8`, `tgt16` | `if ((int16)acc < (int16)SELF(b)) goto_(tgt); else skip();` | High. |
| 6C | sub_1691D | `addr16`, `tgt16` | `if ((int16)acc < (int16)GLOB(a)) goto_(tgt); else skip();` | High. |
| 6D | sub_1692F | `fld8`, `tgt16` | `if ((int16)acc < (int16)TGT(b)) goto_(tgt); else skip();` | High. |
| 6E | sub_16941 | `fld8`, `tgt16` | `if ((int16)acc < (int16)REL(b)) goto_(tgt); else skip();` | High. |
| 6F | sub_16A85 | `imm16`, `tgt16` | `if (acc == imm) goto_(tgt); else skip();` | High. |
| 70 | sub_16A95 | `fld8`, `tgt16` | `if (acc == SELF(b)) goto_(tgt); else skip();` | High. |
| 71 | sub_16AA5 | `addr16`, `tgt16` | `if (acc == GLOB(a)) goto_(tgt); else skip();` | High. |
| 72 | sub_16AB5 | `fld8`, `tgt16` | `if (acc == TGT(b)) goto_(tgt); else skip();` | High. |
| 73 | sub_16AC5 | `fld8`, `tgt16` | `if (acc == REL(b)) goto_(tgt); else skip();` | High. |
| 74 | sub_16AE5 | `imm16`, `tgt16` | `if (acc != imm) goto_(tgt); else skip();` | High. |
| 75 | sub_16AF5 | `fld8`, `tgt16` | `if (acc != SELF(b)) goto_(tgt); else skip();` | High. |
| 76 | sub_16B05 | `addr16`, `tgt16` | `if (acc != GLOB(a)) goto_(tgt); else skip();` | High. |
| 77 | sub_16B15 | `fld8`, `tgt16` | `if (acc != TGT(b)) goto_(tgt); else skip();` | High. |
| 78 | sub_16B25 | `fld8`, `tgt16` | `if (acc != REL(b)) goto_(tgt); else skip();` | High. |
| 79 | sub_16965 | `imm16`, `tgt16` | `if (ge_neg(acc, imm)) goto_(tgt); else skip();` | Same as 0x65 except when the operand is 0x8000 (see below). High. |
| 7A | sub_1697D | `fld8`, `tgt16` | `if (ge_neg(acc, SELF(b))) goto_(tgt); else skip();` | High. |
| 7B | sub_16995 | `addr16`, `tgt16` | `if (ge_neg(acc, GLOB(a))) goto_(tgt); else skip();` | High. |
| 7C | sub_169AD | `fld8`, `tgt16` | `if (ge_neg(acc, TGT(b))) goto_(tgt); else skip();` | High. |
| 7D | sub_169C5 | `fld8`, `tgt16` | `if (ge_neg(acc, REL(b))) goto_(tgt); else skip();` | High. |
| 7E | sub_169F5 | `imm16`, `tgt16` | `if (!ge_neg(acc, imm)) goto_(tgt); else skip();` | Same as 0x6A except when the operand is 0x8000. Family continues as 0x7F-0x82 (self, glob, tgt, rel). High. |
| 7F | sub_16A0D | `fld8`, `tgt16` | `if (!ge_neg(acc, SELF(b))) goto_(tgt); else skip();` | High. |

Instruction lengths including the opcode byte: 0x40 = 1; 0x41-0x64 = 2 for the
self/target/related forms (`(op-0x41) % 4 != 1`: 41,43,44,45,47,48,...)
and 3 for the global forms (42,46,4A,4E,52,56,5A,5E,62); branches 0x65-0x7F =
5 for the imm16 and addr16 forms (65,67,6A,6C,6F,71,74,76,79,7B,7E) and 4 for
the field forms (66,68,69,6B,6D,6E,70,72,73,75,77,78,7A,7C,7D,7F).

Operand reads go through `sub_17581` (imm16), `sub_17588` (self field),
`sub_1759A` (global), `sub_175A3` (target field), `sub_175BB` (related field),
i.e. modes 0-4 of `sub_17569` called directly, so there is **no mode byte** in
this range. The operand is read first, then `tgt16`.

### Shift helpers (0x59-0x64)

The count is `cl` = low byte of `acc`. The code uses `movzx` (386+), and on a
386+ a 16-bit shift masks the count to 5 bits:

```c
int c = acc & 0x1F;
uint16_t shl16(uint16_t x, uint16_t acc) { return c >= 16 ? 0 : (uint16_t)(x << c); }
uint16_t shr16(uint16_t x, uint16_t acc) { return c >= 16 ? 0 : x >> c; }
uint16_t sar16(uint16_t x, uint16_t acc) { return (uint16_t)((int16_t)x >> (c >= 16 ? 15 : c)); }
```

### ge_neg (0x79-0x82)

The handler computes `ax = -v; ax += acc` and branches on the sign of the true
result, `SF == OF` (the `jo`/`jns`/`js` ladder). Exact model:

```c
bool ge_neg(uint16_t acc, uint16_t v) {
    int16_t nv = (int16_t)(uint16_t)(-v);         // neg: 0x8000 stays 0x8000
    return (int32_t)(int16_t)acc + nv >= 0;       // SF==OF  <=> true sum >= 0
}
```

For any `v != 0x8000` this equals `(int16)acc >= (int16)v`, identical to
0x65-0x69 (and `!ge_neg` equals 0x6A-0x6E). For `v == 0x8000` it differs:
0x79-0x7D never branch and 0x7E-0x82 always branch, while 0x65 always and
0x6A never branch. Scripts use both families (e.g. 0x7E with thresholds
0xCC/0xDA/0xEC/0xF0 at 0x974E in script 0), so port it exactly.

### RNG `sub_10C9F` (0x40)

State bytes `R[0..9]` = `ds:4Eh..57h` (labels `word_1E58E..word_1E596`).
Unaligned word accesses; every step reads the state as already modified by
the previous step:

```c
uint16_t rd(int o) { return R[o] | R[o+1] << 8; }   // o relative to ds:4Eh
uint16_t rand16() {
    uint32_t s = (uint16_t)(rd(0) + 1) + rd(3);      // inc ax (no CF), add ax,[51h]
    wr(6, (uint16_t)s);  int cf = s >> 16;           // [54h]
    wr(8, (uint16_t)(rd(2) + rd(5) + cf));           // adc ax,[53h]  -> [56h]
    wr(0, rd(3));                                    // [4Eh] = [51h]
    wr(2, rd(5));                                    // [50h] = [53h]
    wr(4, rd(7));                                    // [52h] = [55h]
    return rd(1);                                    // ax = [4Fh]
}
```

## Unresolved

Nothing in 0x40-0x7F is unresolved: every handler is self-contained or calls
only the helpers listed above, all read in full.

Open points outside the handlers themselves:

- The initial RNG seed (`ds:4Eh..57h`) is set elsewhere; not traced here.
- The meaning of most field numbers and globals (e.g. `ds:204h`, `ds:1CA4h`)
  is game semantics, not opcode semantics, and is not covered here.
- Correction to `README.md`: the field operand byte is a byte offset into the
  `28CAh` table, so it equals `2k`. The README's field table lists `k`; a
  script byte `08` means k = 4 (flags `1462h`), not k = 8.

## Verification against 064.bin

Method: a recursive-descent disassembler (start at record offset + 3, follow
`goto`/`call`/branch targets). Opcodes in this range use the lengths above.
Other opcodes use lengths from a quick read of their handlers and are
provisional: 0x00/0x01/0x06-0x0E/0x2A/0x35-0x37 = 0 bytes, 0x03/0x04/0x15/0x19 =
u16, 0x05/0x16 = u8+u16, 0x02 = u8 u8, 0x0F = u8+u16, 0x1B-0x22/0x2B-0x2F/0x33 =
u8+tgt16, 0x30 = u16+tgt16, 0x13/0x14/0x18/0x38 = mode byte + two mode
operands, 0x3B-0x3F per the README. Decoding stops at any other opcode.

Over all 76 distinct scripts in the 0x56-record table, 643 distinct
instructions in 0x40-0x7F were decoded. Results:

- 0 branch targets land inside another decoded instruction. 160 of 164 branch
  targets land exactly on a decoded instruction start; the other 4 point past
  a stop at an opcode outside this range whose length was not modelled.
- 0 odd `fld8` bytes in any 0x41-0x64 or branch field operand.
- Records 17 (0xB84E, page 0x83) and 69 (0xD9A7, page 0x82) decode completely,
  with no unknown opcode anywhere in their reachable code.

### Script 1: record 17 (offset 0xB84E, page 0x83, file offset 0xB84E)

```
B851: 3B 00 10        acc = 0x1000
B854: 51 08           self.flags(1462h) |= acc
B856: 04 88 B8        call B888
...
B865: 3B 00 00        acc = 0
B868: 71 18 02 73 B8  if (acc == [218h]) goto B873   ; skips the next 2 insns
B86D: 3B 01 00        acc = 1
B870: 4A 18 02        [218h] -= acc
B873: 40              acc = rand()
B874: 42 04 02        [204h] = acc
B877: 3C 14           acc = self.field(k=0Ah)
B879: 76 4E 1B 75 B9  if (acc != [1B4Eh]) goto B975
...
B888: 3D A4 1C        acc = [1CA4h]
B88B: 6A CC 00 52 CC  if (acc < 0x00CC) goto CC52     ; CC52 = 06 (return)
B890: 3B DD 00        acc = 0x00DD
B893: 6C A4 1C 52 CC  if (acc < [1CA4h]) goto CC52    ; i.e. range check 0xCC..0xDD
B898: 3B D2 00        acc = 0x00D2
B89B: 41 1A           self.field(k=0Dh) = acc
B89D: 40              acc = rand()
B89E: 42 04 02        [204h] = acc
B8A1: 3B 3F 00        acc = 0x3F
B8A4: 4E 04 02        [204h] &= acc
B8A7: 3D 04 02        acc = [204h]
B8AA: 45 1A           self.field(k=0Dh) += acc        ; 0xD2 + rand&0x3F
B8AC: 06              return
...
B8F1: 3B 00 00        acc = 0
B8F4: 75 1A 52 CC     if (acc != self.field(k=0Dh)) goto CC52   ; 4-byte field form
...
B913: 65 00 09 52 CC  if (acc >= 0x0900) goto CC52
B918: 3B FF FF        acc = 0xFFFF
B91B: 56 06 02        [206h] ^= acc
```

Every branch target (B873, B975, CC52, B8C6, B8DD, B954, B950) is the start of
a decoded instruction.

### Script 2: record 69 (offset 0xD9A7, page 0x82, file offset 0x59A7)

```
D9AA: 15 20 DB        (op 15, anim script = DB20)
D9AD: 3B 00 80        acc = 0x8000
D9B0: 51 08           self.flags |= 0x8000
D9B2: 3B 02 00        acc = 2
D9B5: 70 14 FE D9     if (acc == self.field(k=0Ah)) goto D9FE
D9B9: 3B 03 00        acc = 3
D9BC: 70 14 0B DA     if (acc == self.field(k=0Ah)) goto DA0B
...                   (switch on field 0Ah = spawn param 1552h, values 2..9)
D9E6: 70 14 59 DA     if (acc == 9) goto DA59
D9EA: 04 66 DA        call DA66
...
DA6C: 3B 00 02        acc = 0x0200
DA6F: 41 10           self.field(k=08h) = acc
DA71: 40              acc = rand()
DA72: 42 04 02        [204h] = acc
DA75: 3B C0 01        acc = 0x01C0
DA78: 4E 04 02        [204h] &= acc
DA7B: 3D 04 02        acc = [204h]
DA7E: 45 10           self.field(k=08h) += acc
DA80: 40              acc = rand()
DA81: 6A E8 80 75 9B  if ((int16)acc < (int16)0x80E8) goto 9B75   ; 9B75 = 06
DA86: 3B 00 FE        acc = 0xFE00
DA89: 41 10           self.field(k=08h) = acc
...
DA98: 49 10           self.field(k=08h) -= acc
DA9A: 06              return
```

The eight 4-byte `70` instructions sit back to back with 3-byte loads between
them, and each target (D9FE, DA0B, ..., DA59) is exactly the 13-byte-spaced
case body (`3B xx xx 41 10 3B xx xx 41 12 03 F0 D9`). A wrong length for 0x70
would break this spacing. The reachable path also passes through
`D957: 7E 00 03 70 D9` (if acc < 0x0300 goto D970) and `D973: 75 18 86 98`
(if acc != self.field(k=0Ch) goto 9886), both landing on decoded starts.

### Script 3: record 0 (offset 0x8566, page 0x82, file offset 0x0566)

```
8569: 04 51 9A        call 9A51
856C: 3B 00 00        acc = 0
856F: 42 14 02        [214h] = acc
8572: 3D 16 02        acc = [216h]
8575: 41 2E           self.field(k=17h) = acc
8577: 3B 2F 00        acc = 0x2F
857A: 41 20           self.field(k=10h) = acc
857C: 3B 00 00        acc = 0
857F: 41 14           self.field(k=0Ah) = acc
8581: 3B 00 10        acc = 0x1000
8584: 51 08           self.flags |= acc
8586: 3D F6 1C        acc = [1CF6h]
8589: 42 04 02        [204h] = acc
858C: 3B 40 00        acc = 0x40
858F: 4E 04 02        [204h] &= acc
8592: 3B 00 00        acc = 0
8595: 76 04 02 D8 96  if (acc != [204h]) goto 96D8    ; i.e. bit 0x40 set
859A: 04 45 97        call 9745
859D: 04 76 97        call 9776
85A0: 04 B5 97        call 97B5
85A3: 08              (op 08: flip facing)
85A4: 3D F6 1C        acc = [1CF6h]
85A7: 42 04 02        [204h] = acc
85AA: 3B 00 04        acc = 0x0400
85AD: 4E 04 02        [204h] &= acc
85B0: 3B 00 00        acc = 0
85B3: 71 04 02 B9 85  if (acc == [204h]) goto 85B9
85B8: 07              (op 07, 1 byte)
85B9: F7 ...          (outside this range; decoding of this path stops)
```

Here the 5-byte branch at 85B3 jumps to 85B9, exactly over the 1-byte opcode
0x07, which confirms both the 5-byte length of 0x71 and the order
(operand first, target second). The subroutines 9745/9776/97B5 contain 25
more in-range instructions (`42`, `41`, `7E`), all aligned and ending in `06`.

Divergence: none was found in this range. Every decoding stop in the corpus
is at an opcode outside 0x40-0x7F whose length was not modelled (e.g. 0xF7,
0xCD, 0xB4).
