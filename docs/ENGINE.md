# Engine architecture

The engine keeps the original game's memory model and runs its actor scripts
unchanged, so behaviour matches the original wherever the port is complete.

| Module | Role |
|---|---|
| `src/data/` | DATA.DAT access, LZSS, level/tile/sprite decoding, XMIDI parsing. No game state. |
| `src/engine/machine.h` | The memory the logic works on: the 64 KiB data segment (initialised from BTHORNE.EXE), the map segment, scripts, tileset, sprite memory and banks. |
| `src/engine/vm*.cpp` | The actor script interpreter and every routine the opcodes reach: animation and sprite parts, spawning, collision, probes, text boxes, palettes, sound events. |
| `src/engine/game.cpp` | Boot, level loading (`sub_105B4`), the per-tick main loop (`sub_10043`), HUD state, palette cycling, tile animation, shake. |
| `src/render/` | Software renderer: draws the 320x240 screen from the machine state the way the original composes it. |
| `src/audio/` | VOC samples mixed with SDL3; General MIDI music and effects through the system synthesizer. |
| `src/game/` | The `blackthorne` executable. |
| `src/viewer/` | Level / sprite viewer with an engine play mode and screenshot options. |
| `tests/` | Format and VM tests, plus `btprobe` for tracing an actor with scripted input. |

Names like `sub_1115D` refer to routines in the original BTHORNE.EXE (IDA
addresses), so every behaviour can be traced back to the code it ports.
