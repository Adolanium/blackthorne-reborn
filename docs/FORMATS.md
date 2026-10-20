# Blackthorne (PC, 1994) data formats

Status markers: **confirmed** = matches game code and round-trips;
**observed** = consistent across the data, not yet traced in code;
**open** = not yet understood.

## DATA.DAT container (confirmed)

```
u32 count                      ; 353
u32 offset[count]              ; bit 30 = "stream into EMS" flag, mask with 0x3FFFFFFF
entry @ offset:
  u32 unpacked_size
  u8  lzss_stream[]
```

Entry 352 is a 4-byte end marker, not a resource.

Flagged entries (41 total, e.g. 65-70) are decompressed straight into EMS
pages by `sub_10451`; unflagged ones go to conventional memory via `sub_10349`.
The game reads the size with `sub_10278` and can also read raw bytes with
`sub_102A4`.

### LZSS (confirmed, `sub_10349`)

- 4096-byte ring buffer, zero-filled, write head starts at 0.
- Flag byte read LSB first; bit 1 = literal byte, bit 0 = back-reference.
- Back-reference = little-endian u16 `w`: ring offset `w & 0xFFF` (absolute),
  length `(w >> 12) + 3`.
- Decoding stops after `unpacked_size` bytes.

Implemented in `tools/datdump.py`; all 352 resources decode to their exact
stored size.

## Resource types

| Kind | Example entries | Notes |
|---|---|---|
| XMIDI music | 17-36 | `FORM....XDIR` (Miles AIL Extended MIDI) (confirmed by magic) |
| Creative VOC sound | 37-63 | `Creative Voice File` (confirmed by magic) |
| Audio drivers | 4-15 | Miles driver images with copyright strings; not needed by a port |
| VGA palette, 256 colours | 91, 97, 103, ... | 768 bytes, 6-bit RGB (observed) |
| Palette fragment, 16 colours | 71-90, 264-267, 292-304 | 48 bytes, 6-bit RGB (observed) |
| Sprite banks | 65-69, 75, 240-261, 314, 337-340 | See "Sprite banks" (confirmed) |
| Level descriptors | 191-195, 203-206, 217-220, 235-239 | See "Level descriptor" (partly traced) |
| Per-level groups | ~91-240 | Repeating set: 768 palette, large tile/graphic block, map-like u16 tables (observed) |

**Resource ids inside descriptors are 1-based**: id N is DATA.DAT entry N-1
(confirmed: with this offset every bank divides exactly into frames).

## Video (confirmed, `sub_125EC`)

Mode 13h unchained into Mode X with a custom 12-register CRTC table at
`ds:5316h`: 320x240 (480 double-scanned lines), offset register 0x2B, so
rows are 86 bytes = 344 pixels for hardware scrolling. The playfield is
256x224; see [vm/README.md](vm/README.md#display).

## Level descriptor (partly traced, `sub_105B4` and helpers)

Loaded to `ds:1CEEh` (linear 0x2022E; data segment base 0x1E540). Parsed by
`src/data/level.cpp` and `tools/sprites.py`.

Header, 0x36 bytes (u16 little-endian unless noted; ids are 1-based):

| Offset | Meaning |
|---|---|
| 0x08 | flags (`word_20236`; bit 0x100 selects HUD set 0x123 instead of 0x121) |
| 0x1C, 0x1E | foreground layer width, height (metatiles) |
| 0x21 | foreground layer id |
| 0x23 | tileset id; id+1 is the tile mask resource |
| 0x25 | metatile table id |
| 0x27, 0x29 | background layer width, height |
| 0x2C | background layer id (`FFFF`/negative = none) |
| others | **open** |

Body:

1. 14-byte records until `FFFF`.
2. `sub_10760`: `(u16 id, u8 slot)` until `FFFF`. Loads level resources; a
   768-byte palette goes to colour 0, 48-byte fragments to colour `slot`
   (e.g. Kyle's colours, entry 78, at slot 128).
3. `sub_107BC`: u16, then `(u8, u8, u8, u16 list until FFFF)` until a 0 byte.
4. `sub_10802`: u16, then 8-byte records until a 0 byte (last u16 = id).
5. `sub_10876`: 6-byte records `(u16 id, ...)` until `FFFF`.
6. `sub_108A5`: sprite banks, 5-byte records `(u16 id, u8 w8, u8 h8, u8 flag)`
   until `FFFF`. Frame = `w8*8` x `h8*8` pixels, `w8*h8*64` bytes.
7. Remaining helpers (`sub_108F3` onward) **open**.

## Tiles and maps (confirmed, `sub_1438A`)

- Tileset: 8x8 tiles, 64 bytes each, Mode X planar (4 planes x 8 rows x
  2 bytes, x = `col*4 + plane`), 4-bit pixel values, 0 = transparent.
- Tile mask (tileset id + 1): 8 bytes per tile, one bit per pixel, used by the
  blitter to skip transparent pixels.
- Metatile table: 8 bytes per metatile = four tile words (TL, TR, BL, BR),
  16x16 pixels.
- Tile word: bits 6-15 tile index, bits 4-5 select one of four blitters
  (x/y flip), bit 3 **open**, bits 0-2 palette row (colour = pixel + row*16).
- Map layer: `width*height` u16 cells, row-major. Bits 0-9 = metatile,
  bits 10-15 = collision code (`sub_18992`: `(cell & 0xFC00) >> 10`;
  `sub_189A5` returns 0x400, code 1, outside the map). Background is drawn
  first, foreground on top. Only the foreground carries collision.
- Tile pixels with a non-zero high nibble are absolute colours; only values
  1-15 get the palette row added (`loc_189EA`).

### Collision codes (confirmed values; names provisional)

| Code | Cells (all levels) | Meaning (from placement in the maps) |
|---|---|---|
| 1 | 16337 | solid |
| 3 | 331 | ladder |
| 4 | 77 | ladder top (always directly above a 3 column) |
| 5 | 17 | floor cell beside an elevator shaft |
| 6 | 125 | **open** (vertical strips) |
| 7 | 394 | ledge, left end (grab point) |
| 8 | 387 | ledge, right end (grab point) |
| 16 | 17 | water floor (bottom of waterfall basins) |
| 32 | 140 | elevator stop, cell above the platform |
| 33 | 156 | elevator stop, floor cell over the shaft |

Movement code tests codes against sets stored at `ds:60CCh` (byte lists
terminated by 0xFF), for example `{1,2}`, `{1,2,4,5,7,8,10,11,12}`,
`{3,4}`, `{7,8}`, `{0x20}`, `{0x21}`, `{0x2E}`, `{0x2F,0x30}`. Which set each
check uses is traced when the movement code is ported.

## Object spawns (confirmed, `sub_11380`, `sub_11458`)

Descriptor offset 0x36: 14-byte records until `FFFF`:

| Offset | Field |
|---|---|
| 0 | x (pixels, actor centre) |
| 2 | y |
| 4, 6 | activation half extents rx, ry |
| 8 | actor type (index into the actor table) |
| 10 | flags: 0x800 = spawn at level start; others **open** |
| 12 | parameter (e.g. per-prisoner id 0x7E, 0x80, ...) |

An object spawns when the camera rectangle overlaps `x +- rx, y +- ry`
(`sub_11380`), through `sub_1115D`.

## Actor table (confirmed layout, `loc_117B7`)

DATA.DAT entry 64 (resource 0x41) is stored raw. It starts with 0x56
16-byte actor types, followed by actor scripts:

| Offset | Field |
|---|---|
| 0 | sprite bank: 0xFFFE = level bank, 0xFFFF = none, else resource id |
| 2 | bits 0-5 part count, bits 6-7 flags |
| 3 | script offset within entry 64 (stored +3 as the script pointer) |
| 5 | script page (0x82/0x83) |
| 7, 8, 9 | box kind, box width, box height |
| 10 | frame size: low byte width/8, high byte height/8 (0x0604 = 32x48) |
| 12, 14 | **open** |

Types seen in the maps (provisional): 0x11 Kyle start, 0x0D/0x0E orc
guards, 0x47-0x4B chained prisoners, 0x08 elevator, 0x0A exit door.

## Raw entries

Entries 0-15 and 64 are stored uncompressed: their stored length equals the
size field. The game reads those with `sub_102A4` instead of decompressing.
- Black areas remain in some maps; whether the game draws a backdrop there is
  **open**.

## Sprite banks (confirmed)

Bank = consecutive frames of `w8*h8*64` bytes, one byte per pixel
(0 = transparent). A frame is stored as square cells:

1. If the height is not a multiple of 32, a 16-row top band of 16x16 cells,
   left to right.
2. Then each 32-row band: 32x32 cells left to right; a leftover 16-pixel
   column is two 16x16 cells, top then bottom.

Each cell is Mode X planar: 4 planes x `size` rows x `size/4` bytes, pixel
x = `col*4 + plane`. Examples: Kyle 32x48 = 2x(16x16) + 32x32; guard 48x48 =
3x(16x16) + 32x32 + 2x(16x16).

Some banks hold 4-bit pixels (max 15) that are offset into the level palette
(Kyle uses base 128); others store full 8-bit indices. Which field selects the
base is **open** (flag byte or actor data).

Drawing: `sub_15BF2` copies a frame into 9-byte rows (opacity mask + 8
pixels); actor parts point into that buffer at `script_byte * 0x48`
(`sub_120D2`); `loc_153C8` blits one 32x32 part per call through compiled
blitters (`loc_18D2A`, flipped variant at `loc_18DB2` when flag 0x20 is set).
Virtual screen stride in that path is 0x56 bytes (344 pixels).
