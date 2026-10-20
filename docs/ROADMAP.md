# Roadmap

The original engine is ~25k lines of hand-written 16-bit assembly
(597 functions, one code segment, no C runtime). The rewrite translates it
subsystem by subsystem into C++ with SDL, keeping gameplay identical, then
adds presentation upgrades on top.

## Phase 1: Formats and asset viewer
- [x] DATA.DAT container and LZSS
- [x] Identify music (XMIDI) and sound (VOC)
- [x] Sprite bank layout and level palettes (`tools/sprites.py`)
- [ ] Palette base for 4-bit banks; animation scripts and actor part lists
- [ ] Tiles, level maps, palettes per level
- [ ] Level descriptor (parsed by `sub_105B4` and helpers)
- [ ] Asset viewer: browse every sprite, tile and level

## Phase 2: Engine core (faithful)
- [ ] Main loop / tick (`sub_10043`), timing
- [ ] Map rendering and scrolling
- [ ] Kyle: input, animation state machine, physics
- [ ] Enemies, items, triggers, doors, elevators
- [ ] HUD, menus, passwords, save game
- [ ] Music (XMIDI to General MIDI / OPL emulation) and VOC sound
- [ ] Side-by-side comparison against DOSBox for accuracy

## Phase 3: Make it beautiful
- [ ] Widescreen viewport (wider camera over existing maps)
- [ ] High-resolution output with integer/sharp-bilinear scaling and CRT shader options
- [ ] Optional upscaled art pipeline (palette-aware)
- [ ] Lighting/parallax/particle polish where it fits the art
- [ ] Gamepad, rebinding, quick save
