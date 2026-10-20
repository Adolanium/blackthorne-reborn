# Actor script VM

Every actor (Kyle, enemies, doors, elevators, prisoners, effects) runs a
bytecode script from DATA.DAT entry 64. Per-opcode tables:
[ops_00-3F](ops_00-3F.md), [ops_40-7F](ops_40-7F.md),
[ops_80-BF](ops_80-BF.md), [ops_C0-FF](ops_C0-FF.md).

## Interpreter (`sub_1609A`)

```
loop:
    op = script[ip++]
    call funcs_160A1[op]       ; 256-entry table
```

The loop has no exit of its own. Opcodes that end the actor's turn discard
the return address (`pop ax`) and return to the scheduler. Opcode 0x00 yields:
it saves `ip` to the actor's field `1232h` and the link register to `184Ah`.

- `ds:2Ah` holds the current actor slot (0, 2, ..., 0x26: 20 actors).
- `es:bx` is the script pointer.
- `ds:30h`/`ds:32h` form a one-level link register (return ip and segment)
  used by call (0x04, 0x05) and return (0x06).

## Script addresses

An address is a 16-bit offset plus a page byte. The page selects a segment
relative to the entry-64 buffer: `seg = base + ((page - 0x82) ror 5) - 0x800`.
In file terms, page 0x82 means offset - 0x8000 and page 0x83 means the offset
as-is.

Actor type records give `(offset, page)` at +3 and +5. A newly spawned actor
starts at `offset + 3`; the first 3 bytes (always opcode 0x03 + address) are
the re-entry jump used when flag 0x200 restarts the script (`sub_1603F`).

## Actor fields

Structure of arrays: field `k` is a 20-word array at
`ds:0x13C2 + k * 0x28`, indexed by slot. Scripts name fields by `k` through
the table at `ds:28CAh` (entry k = k * 0x28). Fields below 0x13C2 are
internal (negative k).

| Address | k | Meaning |
|---|---|---|
| 13C2h | 0 | bbox top |
| 13EAh | 1 | bbox bottom |
| 1412h | 2 | bbox left |
| 143Ah | 3 | bbox right |
| 1462h | 4 | flags (0x40 and 0x80 are the facing / flip tests; 0x800 spawned at start; 0x200 restart) |
| 1552h | 10 | spawn parameter |
| 157Ah | 11 | spawn record index |
| 15A2h | 12 | actor type |
| 15F2h | 14 | x |
| 161Ah | 15 | y |
| 16BAh | 19 | frame buffer offset |
| 1782h | 24 | current frame |
| 17FAh | 27 | target actor slot (operand mode 3) |
| 1822h | 28 | second related actor slot (operand mode 4) |
| 18C2h | 32 | frame buffer segment |
| 18EAh | 33 | animation script ip (run by opcode 0x2A) |
| 1912h | 34 | animation script segment |
| 1232h | -10 | saved script ip |
| 1372h, 139Ah | -2, -1 | box width, height |

## Operand addressing modes (`sub_17569`, write-back `loc_175D6`)

Many opcodes take a mode byte holding two 3-bit modes (bits 0-2 and 3-5).
Each mode selects how the next operand is read:

| Mode | Script bytes | Value |
|---|---|---|
| 0 | u16 | immediate |
| 1 | u8 k | current actor's field k |
| 2 | u16 addr | global word at `ds:addr` |
| 3 | u8 k | field k of the target actor (`17FAh`) |
| 4 | u8 k | field k of the related actor (`1822h`) |
| 5 | none | random number (`sub_10C9F`) |

## Display

Mode X tweaked to 320x240: CRTC table at `ds:5316h` (vertical display end
0x1DF over 480 double-scanned lines, offset register 0x2B = 86 bytes =
344-pixel virtual rows). The playfield is 256x224 (map redraw clips at
0x100 x 0xE0 in `sub_18910`); the HUD uses the remaining area.
