# Accuracy checks against the original

The engine is checked against the original `BTHORNE.EXE` frame by frame,
in the background, with no window. Both games get the same input. Their
64 KB data segments, where all game state lives, are compared after every
frame.

## The pieces

- **`tools/emu/btemu.py`** runs the original DOS executable inside the
  [Unicorn](https://www.unicorn-engine.org/) CPU emulator
  (`pip install unicorn`). It stubs only what the game touches:
  - DOS file, memory and vector calls.
  - LIM EMS with an 8 MB backing store.
  - A few BIOS calls and the BIOS data area.
  - VGA status ports and the timer.

  Sound is switched off by rewriting the SETUP block (DATA.DAT entry 0)
  as the game reads it.

  A frame is one pass through the game's frame wait (`sub_10113` with the
  page-flip counter `ds:5312h` above zero). At each wait the tool records
  the data segment, writes the next input word to `ds:2A82h` (the word the
  keyboard handler fills) and releases the wait.
- **`bttrace`** (`tests/trace.cpp`) runs our engine with the same input
  schedule and writes the same per-frame dump. `Game::tick()` stops at the
  same frame waits, so the frames line up one to one.
- **`tools/emu/compare.py`** names the words that differ. It also groups
  them into words that change during play and words that are constant.
- **`tools/emu/check.py`** does a full run per level:
  1. Start a new game.
  2. Restart at the level through its password via the give-up menu
     (P, Left, Enter).
  3. Play random input.
  4. Report the first frame where any game-state word differs.

Both tools accept `--poke frame:addr=value` to set data segment words between
frames, for example to set the password.

## Running it

```sh
pip install unicorn
cmake --build build --config Release --target bttrace
python tools/emu/check.py "<game>/BTHORNE" "$PWD/build/Release/bttrace.exe" /tmp --frames 4000
```

## What is not compared

Only state that the original keeps for its hardware, memory manager or
renderer and our engine does not model:
- Segment numbers (matched by offset from their base).
- Sprite bank EMS handles.
- The dirty-tile maps of the two video pages.
- The palette upload buffer and CRTC tables.
- The HUD graphics buffers.
- The stack.
- A few scratch words.

The `IGNORE` table in `compare.py` lists them.

## What it found

The first comparisons showed the main loop differing in timing:
- A screen flip runs a nested main-loop pass, so it takes two frames.
- A level change fades out over two frames, runs two passes on a black
  palette, then fades in over two frames.
- Menus and pauses wait real frames.

Since then it has caught:
- The random generator was never seeded. The original reseeds it on every
  level load.
- The hit flicker was missing.
- A palette-cycling bug left waterfalls and torches frozen.
- The music actions on a level exit were missing.
- The redraw counters, which decide when a freed sprite part can be reused,
  were not kept.
- Several smaller ordering details (see the git log).

From boot through the title, the intro and play on the tested levels, the
game state now matches the original word for word.
