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
| Sprite banks (EMS) | 65-70, ... | One byte per pixel, values 0-15 (4-bit colour) (observed); layout **open** |
| Per-level groups | ~91-240 | Repeating set: 768 palette, large tile/graphic block, map-like u16 tables, small descriptors (observed) |

## Video (confirmed, `sub_125EC`)

Mode 13h unchained into Mode X with a custom 12-register CRTC table at
`ds:5316h`; misc output `0xE3` selects 480-line timing (320x240 expected,
**open** until the CRTC table is decoded). Virtual screen row stride is
0xAC bytes (688 pixels) to allow hardware scrolling.

## Sprite drawing (partly traced)

- Bank loader `sub_108A5` reads 5-byte descriptors: `u16 file, u8 a, u8 b, u8 ?`;
  frame byte size = `a * b * 64`.
- `sub_14E33` draws masked objects per Mode X plane: rows of 9 bytes
  (1 opacity-mask byte + 8 pixels), using a compiled-blit jump table at
  `loc_18C1A`.
- Hero bank (entry 65) layout: 48-row frames, planes stored separately, but
  plain fixed-size slicing does not yet produce clean images. **open**
