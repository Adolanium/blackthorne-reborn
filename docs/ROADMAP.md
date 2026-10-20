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
- [x] Tiles, metatiles, level maps, palettes per level
- [ ] Level descriptor (parsed by `sub_105B4` and helpers)
- [x] Level and sprite viewer (`btviewer`)
- [x] Collision codes and object spawn list, shown in the viewer
- [ ] Actor scripts: initial frame per type so objects render as sprites
- [ ] Black backdrop areas

## Phase 2: Engine core (faithful)
- [x] Timing: 30 ticks/s locked to vsync (`FASTER` cheat = 60)
- [x] Data segment model initialised from BTHORNE.EXE
- [x] Actor script VM: all 256 opcodes (docs/vm)
- [x] Actor spawning, despawning, spawn records and the scheduler
- [x] Main loop: movement passes, event checks, collision, flip-screen camera
- [x] Boot flow and level changes (logos, title menu, intro, levels)
- [x] Renderer: map layers from the live map, sprite parts, text layer, HUD
- [x] Input (original controls), passwords and save file hooks
- [x] Palette cycling, tile animation, screen shake
- [x] Sound effects (VOC) and General MIDI music / MIDI effects
- [ ] Pause and the in-game Esc menu flow, black-frame transitions
- [ ] Verify gameplay against DOSBox (movement, combat, items, elevators)
- [ ] Remaining open details: palette id 0x48 (colour 0x0707), stray intro glyphs

## Phase 3: Make it beautiful
- [ ] Widescreen viewport (wider camera over existing maps)
- [ ] High-resolution output with integer/sharp-bilinear scaling and CRT shader options
- [ ] Optional upscaled art pipeline (palette-aware)
- [ ] Lighting/parallax/particle polish where it fits the art
- [ ] Gamepad, rebinding, quick save
