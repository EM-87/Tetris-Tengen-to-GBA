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

1. ~~Bootstrap the platform-independent core + tests~~ — done.
2. ~~Close out every PLACEHOLDER in `reference/NOTES.md`~~ — done. Gravity
   curve, fractional gravity, soft-drop ramp, scoring, level rule and the
   playfield geometry are all traced and tested; that section now reads
   "Nothing".
3. ~~Stand up `gba/`~~ — done. It builds with a stock `arm-none-eabi-gcc`
   (no devkitARM required), boots in mGBA, and `make gba-check` verifies it
   renders and plays rather than merely links.
4. **Real graphics.** The tiles in `gba/main.c` are placeholders generated at
   runtime. The genuine 8×8 art lives in the original ROM's CHR data, which
   is not in this repo — the disassembly build reads it from
   `gfx/game_tileset.chr` (`reference/disasm/entry.asm.txt`), and
   `reference/disasm/split_chr.py.txt` / `nes_chr_decode.py.txt` are the
   tools that extract it from a cartridge dump. The renderer is already
   built around 8×8 tiles and the core's `tengen_tile_id_for_cell` table, so
   this should be a data path, not a rewrite. NES 2bpp CHR maps onto GBA
   4bpp tiles directly.
5. HUD for the GBA's narrower side panels: score, lines, level, next piece,
   stats. `reference/disasm/gameModeNametable1P.asm.txt` shows what the NES
   drew and where; there are 72px on each side of the field to work with
   versus the NES's 88.
6. Long-bar/undo cheat codes (`reference/NOTES.md` → "Cheat-code state
   exists") — a well-known, well-loved Tengen feature; the RAM layout is
   mapped, the behavior isn't traced yet.
7. Title screen, menus, and the 2P/coop modes. The core already models coop
   (including its 12-column field) and two players; only the GBA front end
   is single-player.
8. Audio (`setMusicOrSoundEffect` plus the `MUSIC_*`/`SOUND_*` constants in
   `constants.asm.txt`) — lowest priority, gameplay fidelity comes first.

## Build

- `make test` — native core tests, gcc only. The everyday loop.
- `make gba` — cross-compiles `build/tengen.gba`.
- `make gba-check` — boots the ROM headlessly in mGBA and asserts it draws
  the field where the resolution mapping says it should and that a piece
  actually falls.

Run `make test` and `make gba-check` before considering a change done. The
first catches rule regressions; the second catches the ones that only appear
once the code runs on ARM (alignment, the `-nostartfiles` build, VRAM
layout), which the host tests structurally cannot.
