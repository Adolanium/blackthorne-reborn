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
- `docs/ENGINE.md`: engine architecture
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

## Play

```
build/Release/blackthorne.exe [path/to/DATA.DAT]
```

Boots the original game flow on the new engine: logos, intro, the title menu
(PRACTICE, NEW GAME, OPTIONS for music and sound, CONTINUE, QUIT), then the
levels and the ending. Progress is saved the way the original saves it: each
level's password goes to `savegame.000` in the settings folder, and CONTINUE
resumes from it. Typing FASTER during play toggles 60 frames a second, as in
the original.

The engine runs the original's frame loop step for step, and it is checked
against the original game running headless (see
[docs/ACCURACY.md](docs/ACCURACY.md)).
Controls are the original ones: arrows move, F fires, R shoots backward,
Space jumps, D runs, S draws/holsters the gun, E uses an item, [ and ] cycle
the inventory, Enter confirms, Esc opens the menu, P asks to give up. F11
toggles fullscreen, F5 quick-saves and F9 quick-loads.

F8 cycles the display filter, and the choice is remembered:

- **Sharp** (default) fills the window. Pixels are scaled by whole numbers
  first, so only the last fraction is blended.
- **Pixel perfect** uses whole-number scaling only.
- **Smooth** runs Scale2x twice to round off the pixel staircases.
- **CRT** adds scanlines.

F7 toggles widescreen (432x240). The extra columns show the neighbouring
screens, dimmed so the active screen stays clear. Enemies there are not yet
awake, because the game still only spawns the screen you are on.

Settings live in `settings.ini` in `%APPDATA%/Adolanium/BlackthorneReborn`,
written on first run. Keys can be remapped there with SDL key names, and
several keys can share an action, for example `key.jump=Space,Left Ctrl`.

### Music

The build downloads [GeneralUser GS](https://github.com/mrbumpy409/GeneralUser-GS),
a free General MIDI SoundFont, and puts it next to the game with its license.
The music then renders through it on every platform. To skip the download,
configure with `-DBT_FETCH_SOUNDFONT=OFF`.

The game uses `soundfont=path` from `settings.ini` if set, else the first
`.sf2` in the settings folder or next to the game. Without any SoundFont the
music plays on the system MIDI synth: on Windows the Microsoft GS Wavetable
Synth, the same output as Blizzard's DOSBox release.

Options are `blackthorne [DATA.DAT] [--filter 0-3] [--wide 0|1] [--level hex]`. `--level`
starts a level directly, for example `--level C1`.

Gamepads work too: d-pad or left stick to move, A jump, X fire, Y draw or
holster, B use item, LB/RB cycle items, RT run, LT shoot backward, Start
confirms and Back opens the menu.
Sound effects play through SDL; music and MIDI effects use the General MIDI
versions of the songs on the system MIDI synthesizer (Windows), as Blizzard's
2013 release did. The HUD is not implemented yet.

## Level viewer

```
build/Release/btviewer.exe [path/to/DATA.DAT]
```

Without an argument it looks in the default Blizzard Classics install folder.
Arrows/WASD or mouse drag to scroll, wheel to zoom, PgUp/PgDn to change level,
B/F to toggle background/foreground, C for the collision overlay, O for
object spawns, Tab for the sprite browser (Left/Right
to change bank). P runs the current level in the engine and G boots the whole
game (logos, menu, intro); in play mode the arrows, F, R, Space, D, S, E, [ ]
and Enter are the original controls. `--boot` / `--play --ticks N --input
from-to:bits,...` drive the engine for screenshots. `--level N --zoom N --x PX --y PX --collision --objects --sprites --bank N
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

## License

The source code is released under the [MIT License](LICENSE). It covers this
project's code only: Blackthorne's game data, graphics, sound and music
belong to Blizzard Entertainment and are not included or licensed here. The
bundled GeneralUser GS SoundFont has its own license.
