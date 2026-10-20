# Blackthorne Reborn

A from-scratch reimplementation of the Blackthorne (PC DOS, 1994) engine that
reads the original `DATA.DAT`, aiming for modern rendering (widescreen,
high-resolution output, smooth scaling, shaders), modern input and modern
saves.

No game data is included. You need your own copy of the game.

## Layout

- `docs/FORMATS.md`: reverse-engineered data formats and their status
- `docs/ROADMAP.md`: plan and milestones
- `tools/`: extraction and inspection scripts
- `re/`: local analysis workspace (game binary, IDA database and listing are git-ignored)

## Extract the data

```
python -I tools/datdump.py "<game dir>/BTHORNE/DATA.DAT" extracted
```
