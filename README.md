# Blackthorne Reborn

A from-scratch reimplementation of the Blackthorne (PC DOS, 1994) engine that
reads the original `DATA.DAT`, aiming for modern rendering (widescreen,
high-resolution output, smooth scaling, shaders), modern input and modern
saves.

No game data is included. You need your own copy of the game (it was released
as a free Blizzard Classics download in 2013).

This is an unofficial fan project, not affiliated with or endorsed by Blizzard
Entertainment. Blackthorne and all game assets are Blizzard's property.

## Layout

- `docs/FORMATS.md`: reverse-engineered data formats and their status
- `docs/ROADMAP.md`: plan and milestones
- `docs/vm/`: the actor script VM (interpreter model and opcode reference)
- `src/data/`: format library (archive, LZSS, levels, tiles, sprites)
- `src/viewer/`: SDL3 level and sprite viewer
- `tools/`: Python extraction and inspection scripts
- `re/`: local analysis workspace (game binary, IDA database and listing are git-ignored)

## Build

Requires CMake 3.24+ and a C++20 compiler (tested with Visual Studio 2026).
SDL 3 is downloaded and built automatically.

```
cmake -S . -B build
cmake --build build --config Release
```

## Tests

```
cd build && ctest -C Release --output-on-failure
```

The tests read the original DATA.DAT and are skipped if it is not installed.

## Level viewer

```
build/Release/btviewer.exe [path/to/DATA.DAT]
```

Without an argument it looks in the default Blizzard Classics install folder.
Arrows/WASD or mouse drag to scroll, wheel to zoom, PgUp/PgDn to change level,
B/F to toggle background/foreground, C for the collision overlay, O for
object spawns, Tab for the sprite browser (Left/Right
to change bank). `--level N --zoom N --x PX --y PX --collision --objects --sprites --bank N
--shot out.bmp` renders one
frame to a file.

## Python tools

### Extract the data

```
python -I tools/datdump.py "<game dir>/BTHORNE/DATA.DAT" extracted
```

### Export sprite sheets

```
python -I tools/sprites.py extracted out/sprites
```
