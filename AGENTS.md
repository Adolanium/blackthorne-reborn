# Notes for coding agents

Blackthorne Reborn is a C++20/SDL3 reimplementation of Blackthorne (1994). It
reads the original game's `DATA.DAT` and `BTHORNE.EXE` at run time. The
engine reproduces the original frame for frame; `docs/ACCURACY.md` explains
how that is checked.

## Rules

- **Never commit game data or anything derived from it.** That covers
  `DATA.DAT` and `BTHORNE.EXE`, extracted or converted graphics, sound or
  music, upscaled or AI-regenerated art, and disassembly. `re/`, `extracted/`,
  `out/`, `packs/` and the `build*/` folders are git-ignored. Art packs are
  loaded from the user's own folders at run time, never from the repository.
- Stage files by path (`git add path/to/file`), never `git add -A` or
  `git add .`.
- Commit author: `Adolanium <94890352+Adolanium@users.noreply.github.com>`.
  Do not put any other name or e-mail address in commits or files.
- Commit messages and PR text carry no tool or AI attribution lines.

## Build and test

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The tests skip themselves when the original game is not installed. With it
installed, at `C:/Program Files (x86)/Blizzard Classics/Blackthorne/BTHORNE`:

- `build/Release/btsoak.exe <DATA.DAT> 1500`: every level with random input,
  including a save/load lockstep check.
- `python tools/playtest.py build/Release/blackthorne.exe <BTHORNE dir> <tmp>`:
  the real game, headless, on every level.
- `python tools/emu/check.py <BTHORNE dir> <abs path to bttrace.exe> <tmp>`:
  game state against the original (needs `pip install unicorn`).
- `python tools/emu/screencheck.py ...`: screens against the original.

Gameplay changes must keep `check.py` at "0 of 21 level(s) diverged". The
classic renderer must keep `screencheck.py` identical. Enhanced visuals go in
a separate rendering path; see `docs/VISUALS.md`.

## Layout

- `src/data`: DATA.DAT archive, LZSS, levels, sprite banks, XMIDI
- `src/engine`: the data segment model, the actor script VM, and `Game` (the
  original main loop, stepped one frame at a time)
- `src/render`: `Renderer` (the original's picture, from a `Scene`) and the
  display filters
- `src/audio`: sound effects and music (SoundFont or system MIDI)
- `src/game/main.cpp`: the game executable; `src/viewer`: btviewer
- `tools/emu`: the headless original (Unicorn) and the comparison tools
- `docs/`: file formats, the VM, accuracy, and visuals
