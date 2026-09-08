# Tengen Tetris (NES) → GBA: verified mechanics and open questions

This is the distilled result of one research pass over `disasm/main.asm.txt`
(the Tengen Tetris NES disassembly — see `disasm/README.md.txt` for that
project's own scope/credits) plus `disasm/notes.txt.txt`, `disasm/tetris-ram.asm.txt`
and `disasm/constants.asm.txt`. Its job is to save the next session from
re-deriving anything below from scratch, and to point precisely at what's
still unknown so it can be tightened without re-reading the whole disassembly.

Every "VERIFIED" item was confirmed by reading the actual 6502 and, where the
logic wasn't obvious from a single glance (carry-flag conventions, bit
packing), tracing it by hand to a plain-English rule before it went into
`src/tengen_core.c`. Every "PLACEHOLDER" item is real, working code in the
core, just not yet checked against this specific ROM — it's there so the game
is playable end-to-end while the remaining disassembly work happens.

## Resolution mapping (the reason this project is feasible as a "1:1" port)

- GBA screen: 240×160 px, tile modes are 8×8 tiles → 30×20 tiles visible.
- NES screen: 256×240 px → 32×30 tiles visible.
- Tengen's playfield is fixed at 10 columns × 20 rows of 8×8 tiles = **80×160 px**.
- 160 px is exactly the GBA's full screen height. The playfield needs **zero**
  vertical scaling or cropping on GBA — every row is visible, pixel-for-pixel,
  same as NES.
- Width: NES has 256-80=176px around the field for HUD (split across both
  sides — see `disasm/gameModeNametable1P.asm.txt` for the exact 1P layout:
  left column block ~8 tiles, playfield, right column block with stats/score/
  next-piece/level). GBA only has 240-80=160px to work with, 16px less.
- **Decision**: keep the playfield tile graphics and 80×160 footprint
  byte-for-byte identical to NES (same tile ids from `kTileIds` in
  `tengen_core.c`, same 8×8 CHR art once ported); redesign the surrounding
  HUD to fit 160px instead of 176px. This is exactly the "1:1 en jugabilidad
  y gráficos del campo, marco adaptado" split the project started from.

## VERIFIED

| Mechanic | ROM location | Summary |
|---|---|---|
| Playfield size | `tetris-ram.asm.txt:206-208`, `notes.txt.txt:35-37` | 10×20, nibble-packed with F0/0F wall sentinels + FF floor row in the ROM; reimplemented as a plain byte grid in `TengenPlayfield` (behavior-equivalent, not bit-equivalent — no reason to replicate the packing trick in C). |
| Piece ids | `notes.txt.txt:11-18` | 0=none,1=I,2=T,3=O,4=J,5=L,6=S,7=Z. Matches `TengenTetromino`. |
| Orientation bitmaps (all 7 pieces × 4 orientations) | `main.asm.txt:1104-1124` | Transcribed verbatim into `kOrientationBitmap`. S has no explicitly-labeled table in the ROM but the disassembler's own comment at `main.asm.txt:1120-1121` confirms the `orientationTiles` bytes double as S's bitmap; used as such. |
| Sub-tile ids (cosmetic joined-block art) | `main.asm.txt:1125-1150` | Transcribed verbatim into `kTileIds`. Not used by collision, only by the renderer later. |
| Spawn position | `main.asm.txt:3688-3721`, `constants.asm.txt:63` | Row = 4 always (`TETROMINO_Y_INIT`). Column by mode: `tetrominoXSpawnTable = {3, 9, 7}` for player1/player2/coop respectively (`main.asm.txt:3802-3803`). Orientation resets to 0. |
| RNG algorithm | `main.asm.txt:3812-3832` | 16-bit state split across two bytes (aliased onto `ppuControl`/`ppuMask` in the ROM — a space-saving trick, not a design constraint we need to keep). Pseudocode in the source comment was traced against the 6502 and matches exactly; see `tengen_rng_step`. |
| Piece selector ("reroll on 0 of 8", no anti-repeat) | `main.asm.txt:3688-3703` (`getNextTetromino`) | Step the RNG 5 times (`genNextPseudoRandom5x`), mask to 0..7, reroll while the result is 0. **There is no check against the previously dealt piece.** This is a real, documented Tengen quirk (unlike the Nintendo-published NES Tetris) and is why Tengen can deal long same-piece or S/Z droughts. |
| Both players' RNGs share a seed at game start | `main.asm.txt:3319-3326` | `player1RNGSeed`/`player2RNGSeed`/`savedRNGSeed` are all copied from the same `rngSeed` when a game starts. |
| DAS timing | `main.asm.txt:96-150` (`doSomethingWithInputDuringGameplay`) | Press = immediate single shift (edge-triggered elsewhere, not shown in this table but consistent with the counter reset to 0 on press). Then: counter increments every frame held; **at 11 the shift fires and the counter reloads to 5** (not 0) — so the first repeat takes 11 frames, every repeat after that takes 6 more (11-5). Encoded as `TENGEN_DAS_CHARGE_FIRST`/`TENGEN_DAS_CHARGE_REPEAT`. |
| Auto-rotate | `main.asm.txt:153-183` | Holding B (`autoRotateCounterP1/2`) or A (`autoRotateClockwiseP1/2`) for 15 frames (`$0F`) starts auto-rotating. Unlike DAS, **the counter is never reloaded down** once past 15 — it fires again *every single frame* thereafter for as long as the button is held (until release resets it to 0). This is the source of Tengen's well-known "hold a button and the piece spins wildly" behavior. B increments orientation (this file calls that "clockwise"); A decrements it ("counter-clockwise") — the ROM's own variable names for these two counters are reversed from what they do, which is worth remembering if `main.asm.txt` is read again later. |
| Wall kick | `main.asm.txt:538-575` | Traced via the actual carry-flag convention of `checkPositionAndClearFlagsOnCarrySet` (confirmed by reading `main.asm.txt:1017-1073`: the routine returns **carry SET = valid position**, via the `$2D` sentinel — `$2D` starts at `$FF`/negative and a `bmi`+`sec` path returns carry set only when no collision was ever recorded during the scan). With that convention, rotation is: try the new orientation in place → if valid, keep it; else shift one column **left** and try the same new orientation → if valid, keep both; else revert orientation and position entirely. It never tries right. This matches the wiki quote already sitting in `notes.txt.txt:160`: *"Because basic rotation can fail when a piece is against the right wall, but not when the same piece is against the left wall, this game will wallkick one square to the left if basic rotation fails."* — including the (real, faithfully reproduced) oddity that it still only ever tries left even flush against the left wall, where a left kick can't possibly help. |
| Level-up thresholds | `main.asm.txt:1473-1478` (`bonusLinesTable`) | Bytes decode as ASCII digit pairs: 03,06,09,12,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95 — i.e. every 3 lines up to level 5, then every 5 lines. Encoded in `TENGEN_LEVEL_LINE_THRESHOLDS`. |
| "Plant piece into playfield" on lock | `main.asm.txt:856-908` (`L8565`) | Confirms the nibble-packing scheme; reimplemented behaviorally (not bit-for-bit) in `lock_piece`. |
| Cheat-code state exists (long bar / undo) | `tetris-ram.asm.txt:121-134` | `codeInputYPlayer1/2`, `longBarCodeUsedP1/2`, `undoCodeUsedP1/2`, `lastCurrentBlockP1/2` etc. Tengen's famous in-game level-up entry codes and the "undo" cheat have dedicated RAM; **not yet implemented in the core** — worth a dedicated pass since these are a well-known, requested-by-fans Tengen feature. |

## PLACEHOLDER (implemented, but not yet checked against this ROM)

These live in `tengen_core.c`, each marked `TODO(verify)` at the point of use:

- **Gravity curve** (`frames_per_row_placeholder`): the spawn-time fall timer
  value of 20 frames *is* verified (`main.asm.txt:3689-3691`), but the
  per-level reload table past that point was not located in this pass — the
  natural-fall code (`main.asm.txt:502-513`, `L8320`) decrements a per-frame
  counter and calls `L9AEE` on reaching zero, and `L9AEE` (not yet read) is
  the most likely place the level-dependent reload value lives. The current
  table is the commonly-cited classic-NES-Tetris curve (48 frames/row at
  level 0 down to 1 at level 29), used as a reasonable stand-in.
- **Line-clear scoring** (`base_score[]` in `tengen_step`): score-add code
  exists around `main.asm.txt:3900` (near `player1ScoreOnes`, in the same
  area as `genNextPseudoRandom2x`/`dropRatePossibleP1` handling) but the
  exact per-line-count formula wasn't decoded this pass. Current values
  (40/100/300/1200 × (level+1)) are the classic NES Tetris shape, not
  confirmed Tengen-specific numbers.
- **Soft-drop rate ramp** (`drop_rate_possible`): confirmed values are 20 at
  spawn (`main.asm.txt:3689-3691`) and 5 immediately after one specific lock
  path (`main.asm.txt:624-629`, `L840B`) — but that second path looked like
  a distinct "soft-drop caused an immediate re-lock" case, not the general
  lock path (`L8417`), and wasn't fully disambiguated from it this session.
  The core currently never re-tightens `drop_rate_possible` after spawn.
- **Level-up threshold indexing for non-zero start levels**: the ROM
  computes an index into `bonusLinesTable` combined with `menuPlayer1StartLevel`
  (`main.asm.txt:3140-3182`) in a way this pass didn't fully untangle — the
  core currently indexes the table by `level - start_level`, which is a
  reasonable guess but not confirmed to match a game started above level 0.

## Suggested next disassembly targets (in priority order)

1. `L9AEE` (called from the natural-fall branch at `main.asm.txt:509`) — most
   likely location of the real gravity-speed-per-level table.
2. The code around `main.asm.txt:3900` (`player1ScoreOnes` addition) — likely
   the real line-clear/soft-drop scoring formula.
3. `codeInputYPlayer1/2` handling (search `tetris-ram.asm.txt:121` outward)
   — the long-bar/undo cheat codes, a well-known Tengen feature fans will
   expect in a faithful port.
4. Confirm `dropRatePossibleP1` transitions beyond spawn (20) and the one
   post-lock case found (5) — whether it ramps per soft-drop frame, per
   piece, or per level.

## A note on frame rate

NES NTSC runs at ~60.0988 Hz; GBA runs at ~59.7275 Hz. `tengen_step` is
designed to be called once per rendered frame on either platform with no
compensation for that ~0.6% difference — it's small enough (over a 20-line
game, well under a frame of total drift) that it isn't worth the complexity
before more of the core is verified. Revisit only if playtesting shows it
matters.
