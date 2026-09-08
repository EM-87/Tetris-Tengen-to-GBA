# Working in tengen_gba_port/

Port of Tetris (NES, Tengen) to GBA. Goal: 1:1 gameplay and graphics for the
playfield itself; the surrounding HUD is redesigned to fit the GBA's narrower
screen (see `reference/NOTES.md`'s resolution-mapping section for the exact
numbers and reasoning — short version: the 80×160px playfield fits the GBA's
160px height exactly with zero scaling; only the side panels need reflowing).

## Ground rules for this project specifically

1. **The disassembly is the source of truth, not general Tetris knowledge.**
   Tengen's version has real, documented quirks (a randomizer with no
   anti-repeat, a rotation system that only ever kicks one column left, an
   auto-rotate feature bound to holding A/B, specific DAS timing) that differ
   from both the Nintendo-published NES Tetris and from modern guideline
   Tetris. Before implementing or changing a rule, check `reference/NOTES.md`
   first — it's a curated index of what's already been traced in
   `reference/disasm/main.asm.txt`, with exact line numbers. If a mechanic
   isn't there yet, that file's disassembly (11,944 lines) is the place to
   look, not a Tetris wiki or memory of how another version works. External
   sources (tetris.wiki, harddrop.com — both linked in
   `reference/disasm/README.md.txt`) are fine for cross-checking a hunch, but
   the ROM decides ties.

2. **Never silently upgrade a PLACEHOLDER to VERIFIED.** `tengen_core.c` marks
   unverified constants with `TODO(verify)` comments (gravity curve, exact
   line-clear scoring, soft-drop rate ramp — full list in
   `reference/NOTES.md`). When one of these gets traced from the ROM, update
   the comment, the value, and `reference/NOTES.md`'s table together in the
   same change. Don't add new unverified "facts" presented as confirmed.

3. **Core stays platform-independent.** `src/tengen_core.{h,c}` must keep
   compiling with plain `gcc -std=c99`, no GBA headers, no `#ifdef GBA`
   branches. All hardware-specific code (tile/palette upload, REG_KEYINPUT
   reads, audio) belongs in a `gba/` layer that calls into this API — that's
   what keeps `make test` fast and keeps the rules honestly testable. If a
   rule seems to require touching hardware state directly, that's a sign the
   API needs a new return value or callback, not a leak.

4. **Every new rule gets a native test before it gets GBA integration.**
   `tests/test_tengen.c` runs in milliseconds with `make test`. Adding a rule
   (say, the long-bar/undo cheat codes from `reference/NOTES.md`'s TODO list)
   without a test for its documented behavior is how a subtle 6502-carry-flag
   misreading turns into a silent gameplay bug — see how `tengen_try_rotate`
   was only trusted once `test_wall_kick_only_ever_shifts_left` demonstrated
   the actual kick happening, not just "a plausible-looking function."

5. **Cite the ROM in comments, not just in NOTES.md.** A constant or algorithm
   lifted from the disassembly should have a `main.asm.txt:<line>` (or
   `tetris-ram.asm.txt:<line>`, `constants.asm.txt:<line>`) comment right next
   to it in the C source, the same way the existing code does. Someone
   reading `tengen_core.c` alone should be able to find the exact bytes it
   came from without going back to `reference/NOTES.md` first.

## Roadmap (rough order)

1. ~~Bootstrap the platform-independent core + tests~~ (done this session).
2. Close out the PLACEHOLDER list in `reference/NOTES.md` — gravity curve,
   scoring, soft-drop ramp, start-level threshold indexing — each backed by a
   fresh trace of `main.asm.txt` and a native test.
3. Long-bar/undo cheat codes (`reference/NOTES.md` → "Cheat-code state
   exists") — a well-known, well-loved Tengen feature; RAM layout is already
   mapped, behavior isn't traced yet.
4. Stand up `gba/` (devkitARM + libgba, per the earlier project discussion):
   a `main.c` that drives `tengen_step` at 60Hz, renders the playfield as an
   8×8-tile background using the tile ids `tengen_core.h` already exposes,
   and maps `REG_KEYINPUT` to `TengenButton`. Playfield rendering should be
   pixel-identical to NES from day one, since that's the part that needs no
   redesign — see `reference/NOTES.md`'s resolution section.
5. HUD/frame redesign for the GBA's narrower side panels (score, lines,
   level, next piece, stats — see `reference/disasm/gameModeNametable1P.asm.txt`
   for what NES shows and where).
6. Audio (`reference/disasm/main.asm.txt`'s `setMusicOrSoundEffect` and the
   `MUSIC_*`/`SOUND_*` constants in `constants.asm.txt`) — lowest priority,
   gameplay fidelity comes first.

## Build

`make test` (native, gcc only) is the everyday loop. `make gba` is a
documented stub until step 4 above exists — see the Makefile's own comments.
