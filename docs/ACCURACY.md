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
- **`tools/emu/scenarios.py`** goes after the objects themselves. For each
  level and each actor type in it, it moves Kyle beside one of its spawn
  records (the same pokes go to both engines). It then plays a scripted mix
  of drawing the gun, facing and firing, jumping, climbing and walking into
  the object, so enemies, bosses, elevators and pickups get exercised. It
  runs several scenarios in parallel.
- **`tools/emu/check.py`** does a full run per level:
  1. Start a new game.
  2. Restart at the level through its password via the give-up menu
     (P, Left, Enter).
  3. Play random input.
  4. Report the first frame where any game-state word differs.

Both tools accept `--poke frame:addr=value` to set data segment words between
frames, for example to set the password.

## Screens

`btemu.py --shots f1,f2,...` also models the VGA:
- the four planes of Mode X video memory, with latches, map mask and bit mask;
- the CRTC start address, offset and line compare;
- pixel panning and the DAC.

It saves what the original actually displays at those frames.
`bttrace --shots` renders ours, and `tools/emu/screens.py` compares the two
pixel by pixel.

Game::display() returns what the original's screen shows:
- the picture: the page drawn in the last frame routine (after the tile
  animation), which stays up through fades and waits;
- the palette: the VGA DAC as last uploaded (sub_128A1), which can lag
  behind the shown palette at ds:4D12h.

The game draws that, and the comparison uses it frame for frame.

This found:
- the vertical placement of screens without the HUD;
- the drawing order of sprite priorities and of tiles with bit 3 set (they
  go in front of sprites below priority 3);
- glyphs that are opaque with the normal font and masked over the map with
  the caption font;
- the item name box;
- sprite and HUD icon pixels with their own palette row (blood, potions);
- bit 10h of a part flips the part layout but never its pixels;
- the map, frame and static segments keep the previous level's bytes;
- the display model above, including the palette upload flag that palette
  cycling sets every frame.

The display also models how the original paints over time. Its full redraw
(sub_125BB) puts all background tiles under all foreground tiles. Only cells
repainted since then, under sprites or text, get the tile bit 3 order. Ours
tracks which cells were repainted since the last full redraw.

Remaining cosmetic differences come from how the original paints. It keeps two
video pages and repaints only the screen cells marked dirty. A sprite is
redrawn only where it overlaps such a cell (sub_15880). So where several
actors overlap and only some are redrawn, or a tile changes without its
cell being marked, the original shows stale pixels or a different overlap
order for a few frames. Ours repaints the whole picture every frame from the
same state. These spots are small and short, and the game state behind them
is identical.

The original's bottom bar also shares video memory with
the first rows of a page. Its last line from x=216 picks up stray bytes
after screen flips. Ours draws the bar intact.

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

Known difference: on the special level 15Eh a script asks for text A1h,
beyond the end of the English text table. The original then reads its width
from heap memory above the EXE image, at offset FFFFh of the text segment.
Ours models the text segment, seg002 and the data segment behind it, but not
the DOS heap, so the portrait position word ds:36Ch differs there. Nothing
else changes. Before this was modelled, that read crashed our engine.

Current result: on all 21 levels in the password table, 4,000 frames each
of random play (movement, jumping, running, shooting, items, and the level
changes that random play triggers), the game state matches the original
word for word. That run covers boot, the intro, the title menu and a
password restart through the give-up menu.
