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
`ds:5316h`; misc output `0xE3` selects 480-line timing (320x240 expected,
**open** until the CRTC table is decoded). Virtual screen row stride is
0xAC bytes (688 pixels) to allow hardware scrolling.

## Level descriptor (partly traced, `sub_105B4` and helpers)

Loaded to `ds:1CEEh`. Parsed in this order (implemented in `tools/sprites.py`):

1. 0x36-byte header (fields **open**), then 14-byte records until `FFFF`.
2. `sub_10760`: `(u16 id, u8 slot)` until `FFFF`. Loads level resources; a
   768-byte palette goes to colour 0, 48-byte fragments to colour `slot`
   (e.g. Kyle's colours, entry 78, at slot 128).
3. `sub_107BC`: u16, then `(u8, u8, u8, u16 list until FFFF)` until a 0 byte.
4. `sub_10802`: u16, then 8-byte records until a 0 byte (last u16 = id).
5. `sub_10876`: 6-byte records `(u16 id, ...)` until `FFFF`.
6. `sub_108A5`: sprite banks, 5-byte records `(u16 id, u8 w8, u8 h8, u8 flag)`
   until `FFFF`. Frame = `w8*8` x `h8*8` pixels, `w8*h8*64` bytes.
7. Remaining helpers (`sub_108F3` onward) **open**.

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
