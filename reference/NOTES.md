# Tengen Tetris (NES) → GBA: verified mechanics and open questions

This is the distilled result of the research passes over `disasm/main.asm.txt`
(the Tengen Tetris NES disassembly — see `disasm/README.md.txt` for that
project's own scope/credits) plus `disasm/notes.txt.txt`, `disasm/tetris-ram.asm.txt`
and `disasm/constants.asm.txt`. Its job is to save the next session from
re-deriving anything below from scratch, and to point precisely at what's
still unknown so it can be tightened without re-reading the whole disassembly.

Every "VERIFIED" item was confirmed by reading the actual 6502 and, where the
logic wasn't obvious from a single glance (carry-flag conventions, nibble
packing, ASCII-digit arithmetic), tracing it by hand to a plain-English rule
before it went into `src/tengen_core.c`, with a test in
`tests/test_tengen.c` pinning the behavior. Several of these took two passes
to get right — the wall kick, the DAS charge, the spawn table's indexing and
the level-up rule were all initially plausible-looking and wrong — so treat
"it compiles and looks reasonable" as no evidence at all here.

## Resolution mapping (the reason this project is feasible as a "1:1" port)

- GBA screen: 240×160 px, tile modes are 8×8 tiles → 30×20 tiles visible.
- NES screen: 256×240 px → 32×30 tiles visible.
- Tengen's playfield is fixed at 10 columns × 20 rows of 8×8 tiles = **80×160 px**.
- 160 px is exactly the GBA's full screen height. The playfield needs **zero**
  vertical scaling or cropping on GBA — every row is visible, pixel-for-pixel,
  same as NES.
- With the frame's two wall columns included, the drawn field is 12 tiles
  wide: **96×160 px**, and it keeps the NES's own column position (2-13).
- **Horizontally**, 32 columns become 30. The two that go come out of the
  6-column decorative divider, the one element that is pure ornament; the
  4-column TETRIS banner inside it survives, as do both borders, the whole
  field and the full 10-column panel. Nothing is scaled or cropped.
- **Vertically**, 30 rows become 20 and the field needs all 20 — so the GBA
  window is exactly the NES's playfield rows (8-27). That costs the NES's
  header strip, which is where its SCORE / LINES / LEVEL / NEXT labels live,
  so those move into the side panel the NES left mostly empty. Same tiles,
  same lettering, stacked instead of spread. This is the one deliberate
  rearrangement in the whole port.
- `make gba-check` asserts all of it against a running ROM: the field's
  column bounds, that both wall columns reach the full 160px, and that the
  border, banner and panel are all actually drawn.

## VERIFIED

| Mechanic | ROM location | Summary |
|---|---|---|
| **Playfield geometry** | `initPlayer1orCoopPlayfield`, `main.asm.txt:3468-3503`; buffer size at `tetris-ram.asm.txt:206-208` | Each ROM row is 8 bytes / 16 nibbles: nibbles 3-12 are the ten playable cells, 2 and 13 are the walls, 0-1 and 14-15 padding. The buffer holds 26 open rows (0-25) plus two solid floor rows, so **the floor is row 26** and the buffer is 28×8 = 224 = `$E0` bytes exactly, matching the RAM reservation. The **visible field is ROM rows 6-25** (20 rows). Three independent things confirm this: the top-out check compares against row 6 (`main.asm.txt:589`), the scoring routine subtracts against 26 (`main.asm.txt:1062`), and the sprite-position math derived from it maps `$2D` 0→217px and 20→57px, i.e. exactly a 160px-tall, 20-row field (`main.asm.txt:220-229`). This core keeps the ROM's coordinates rather than remapping them, so piece positions can be diffed against the disassembly directly. |
| **Coop is 12 columns wide, not 10** | `main.asm.txt:3480-3489` and its own comment at `:3483` | In coop the ROM writes `$00` where it would otherwise write the `$F0`/`$0F` wall nibbles, "to add 1 column on either side". The core reproduces this by storing 12 columns always and placing a `TT_WALL` sentinel in the outer two only outside coop — same trick the ROM uses, and it makes both collision and full-row detection mode-agnostic for free. |
| Piece ids | `notes.txt.txt:11-18` | 0=none,1=I,2=T,3=O,4=J,5=L,6=S,7=Z. Matches `TengenTetromino`. |
| Orientation bitmaps (all 7 pieces × 4 orientations) | `main.asm.txt:1104-1124` | Transcribed verbatim into `kOrientationBitmap`. S has no explicitly-labeled table in the ROM but the disassembler's own comment at `main.asm.txt:1120-1121` confirms the `orientationTiles` bytes double as S's bitmap; used as such. |
| Sub-tile ids (cosmetic joined-block art) | `main.asm.txt:1125-1150` | Transcribed verbatim into `kTileIds`. Not used by collision, only by the renderer later. |
| Spawn position | `main.asm.txt:3688-3721`, `constants.asm.txt:63` | Row = 4 always (`TETROMINO_Y_INIT`) — that's two rows **above** the visible field, so pieces slide in from off-screen. Orientation resets to 0. The column table `tetrominoXSpawnTable = {3, 9, 7}` is **not** indexed the way its layout suggests: `main.asm.txt:3716-3720` loads entry [2] = 7 for 1P *and* 2P (dead centre), and only indexes by player — 3 for P1, 9 for P2 — in coop, where both share one wide field and start on opposite sides. Easy to get backwards; the first pass here did. |
| **Top-out rule** | `main.asm.txt:588-590` | Game over is decided at lock time by position, not by a blocked spawn: if the piece comes to rest with its bounding box still starting above the visible field (`y < 6`), the game ends and the piece is never planted. |
| RNG algorithm | `main.asm.txt:3812-3832` | 16-bit state split across two bytes (aliased onto `ppuControl`/`ppuMask` in the ROM — a space-saving trick, not a design constraint we need to keep). Pseudocode in the source comment was traced against the 6502 and matches exactly; see `tengen_rng_step`. |
| Piece selector ("reroll on 0 of 8", no anti-repeat) | `main.asm.txt:3688-3703` (`getNextTetromino`) | Step the RNG 5 times (`genNextPseudoRandom5x`), mask to 0..7, reroll while the result is 0. **There is no check against the previously dealt piece.** This is a real, documented Tengen quirk (unlike the Nintendo-published NES Tetris) and is why Tengen can deal long same-piece or S/Z droughts. |
| Both players' RNGs share a seed at game start | `main.asm.txt:3319-3326` | `player1RNGSeed`/`player2RNGSeed`/`savedRNGSeed` are all copied from the same `rngSeed` when a game starts. |
| DAS timing | `main.asm.txt:96-150` (`doSomethingWithInputDuringGameplay`) | A shift fires from two sources OR'd together: the fresh press (the caller hands the edge bits in via `player1ControllerNew`, `main.asm.txt:82`) and the DAS repeat. The counter increments on **every** frame the direction is held, the press frame included — so the first repeat lands on the 11th frame *of the hold*, not 11 frames after it. On firing it reloads to **5, not 0**, which is what makes every subsequent repeat 6 frames apart. Encoded as `TENGEN_DAS_CHARGE_FIRST`/`TENGEN_DAS_CHARGE_REPEAT`. |
| Auto-rotate | `main.asm.txt:153-183` | Holding B (`autoRotateCounterP1/2`) or A (`autoRotateClockwiseP1/2`) for 15 frames (`$0F`) starts auto-rotating. Unlike DAS, **the counter is never reloaded down** once past 15 — it fires again *every single frame* thereafter for as long as the button is held (until release resets it to 0). This is the source of Tengen's well-known "hold a button and the piece spins wildly" behavior. B increments orientation (this file calls that "clockwise"); A decrements it ("counter-clockwise") — the ROM's own variable names for these two counters are reversed from what they do, which is worth remembering if `main.asm.txt` is read again later. |
| Wall kick | `main.asm.txt:538-575` | Traced via the actual carry-flag convention of `checkPositionAndClearFlagsOnCarrySet` (confirmed by reading `main.asm.txt:1017-1073`: the routine returns **carry SET = valid position**, via the `$2D` sentinel — `$2D` starts at `$FF`/negative and a `bmi`+`sec` path returns carry set only when no collision was ever recorded during the scan). With that convention, rotation is: try the new orientation in place → if valid, keep it; else shift one column **left** and try the same new orientation → if valid, keep both; else revert orientation and position entirely. It never tries right. This matches the wiki quote already sitting in `notes.txt.txt:160`: *"Because basic rotation can fail when a piece is against the right wall, but not when the same piece is against the left wall, this game will wallkick one square to the left if basic rotation fails."* — including the (real, faithfully reproduced) oddity that it still only ever tries left even flush against the left wall, where a left kick can't possibly help. |
| Level-up thresholds | `main.asm.txt:1473-1478` (`bonusLinesTable`) | Bytes decode as ASCII digit pairs: 03,06,09,12,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95 — i.e. every 3 lines up to level 5, then every 5 lines. Encoded in `TENGEN_LEVEL_LINE_THRESHOLDS`. |
| **Level is recomputed, not incremented** | `main.asm.txt:3140-3186` | On every line clear the ROM walks `bonusLinesTable` from the start, counts how many thresholds the running line total has reached, and sets the level to `start_level + that count` — committing it only if it's higher than the current level. This is not equivalent to stepping the level by one per clear: a clear that crosses two thresholds at once advances two levels. The start level offsets the whole curve, and the ones digit is clamped at '7' so the result never exceeds 17. |
| "Plant piece into playfield" on lock | `main.asm.txt:856-908` (`L8565`) | Confirms the nibble-packing scheme; reimplemented behaviorally (not bit-for-bit) in `lock_piece`. |
| **Gravity curve** | `main.asm.txt:3970-4025` (`L9AEE`, `possibleFallTimerTable` at `$9B36`) | 18 entries, one per level 0-17: 33,28,24,20,17,14,11,9,7,6,5,5,4,4,3,4,3,3 frames per row. **Entry 15 (4) is genuinely slower than entry 14 (3)** — the ROM's bytes really do bump back up; it's not a transcription slip, and the fractional masks below depend on it. Coop uses a separate, strictly monotonic table (`L9B48` at `$9B48`): 33,28,24,20,18,17,16,15,14,13,12,11,10,9,8,7,6,5. |
| **Fractional gravity (levels 10-17)** | `main.asm.txt:3985-4000`, mask table `L9B50` at `$9B50` | For levels ≥10 the ROM ANDs the piece's current row with a per-level mask and either uses `table[level]` or falls back to `table[level-1]`, so a level can average a non-integer frames-per-row (level 15 alternates 4/3 for an effective 3.5; level 14 uses 3 one row in four for 3.75). The polarity of the test **flips** between the 10-15 band (`beq`) and the 16+ band (`bne`). Masks for levels 10-17: `01,00,01,00,03,01,03,00`. The mask bytes physically overlap the tail of the coop fall-timer table — deliberate ROM byte reuse, not an error. |
| **Level cap** | `main.asm.txt:3168-3170` | The level-up code clamps the displayed level to "17", which is exactly the length of the fall-timer table. `TENGEN_MAX_LEVEL`. |
| **Soft drop requires Down alone** | `main.asm.txt:185-188` | `and #DOWN+LEFT+RIGHT; cmp #DOWN` — holding Down together with Left or Right does **not** soft drop; it resets the soft-drop threshold to 5 instead. Same exclusivity applies in reverse to DAS (`main.asm.txt:111-114`): the DAS counter only charges while Down is *not* held. |
| **Soft-drop acceleration** | `main.asm.txt:189-216` | Every time the soft drop fires it tightens its own threshold by one (floored at 1), so a held Down accelerates: first step after 20 frames, then 19, 18… Releasing Down (or pressing a direction) resets the threshold to **5**, not back to 20 — so a second soft drop on the same piece bites much faster than the first. `L9AEE` additionally clamps the threshold to the level's gravity value, so soft dropping is never slower than plain gravity (`main.asm.txt:4008-4011`). |
| **Fresh direction press swallowed after Down** | `main.asm.txt:98-107` | A new Left/Right press is discarded outright if Down was held on the *previous* frame — you cannot start a horizontal move on the frame you stop soft-dropping. |
| **Scoring** | `L9A47` at `main.asm.txt:3874-3893`, multiply `L98D7` at `:3632-3685`, doubling `L9A17` at `:3843-3871`, digit accumulate `L9A6A` at `:3894-3948` | Points are awarded **per piece locked, not per line cleared** — clearing lines pays nothing directly. The award is `(level+1) × ((level+1) + rows_above_floor)`, where `rows_above_floor` is `$2D` = 26 − (row of the lowest cell the piece collided with), so **resting higher pays more**: it rewards building tall, not dropping far. It doubles when `dropRatePossible < 2`, i.e. when a soft drop has fully accelerated. The level term reads oddly in the ROM (ones digit + 1, plus a flat +10 when the tens digit is set) but works out to exactly `level + 1` across the whole 0-17 range, precisely because the level caps at 17. The running total is six ASCII digits, and its hundred-thousands digit is replaced by `'1'` rather than carrying when it would pass `'9'` (`:3942-3946`), so the score wraps to 100000 rather than saturating. |
| **Palettes** | `piecePaletteIndex0..B` at `main.asm.txt:5364-5399`; `setPiecePalette` at `:5338`; `setPlayfieldPaletteFromLevel` at `:5328` | One table of twelve three-colour entries serves double duty: indexed by PIECE ID it colours a piece (entry 1 = I, 2 = T, ... 7 = Z), and indexed by the LEVEL'S ONES DIGIT it colours the playfield — which is why the field recolours each level and repeats every ten. Entry 10 is the line-clear flash, 11 the bonus animation. Transcribed into `gba/palette.h`. The NES colour indices are in the ROM; what they *look like* is in the PPU hardware and is only ever approximated. |
| **Settled blocks are not coloured per piece** | `main.asm.txt:3723-3728` vs `:3205`, palette addresses at `:5334-5342` | `setPiecePalette` writes to `$3F11+`, a SPRITE palette, and is called once per piece dealt; `setPlayfieldPaletteFromLevel` writes to `$3F01+`, a BACKGROUND palette, and is called on level-up. So the falling piece and the next-piece preview are sprites carrying their own colour, while everything already locked is background drawn in one level-wide scheme. A renderer that tints settled blocks by the piece they came from looks wrong — this port did exactly that until the palette code was traced. |
| **The playfield stores tile ids, not piece ids** | `notes.txt.txt:35` ("nibble aligns with tile index"), planting code at `main.asm.txt:928-935` | Each playfield nibble holds the sub-tile index (1-14) taken from `orientationTiles`, not the piece id. This core stores piece ids instead, which is behaviourally identical (both just mean "occupied") and lets it keep per-piece information the ROM discards — but it means the joined-block artwork can't be reproduced exactly until the field also carries tile ids. That's the one remaining structural gap for pixel-perfect settled blocks; it only matters once real CHR art is available. |
| **Screen layout** | `gameModeNametable1P` at `main.asm.txt:C028` onward | The 1P screen is 32x30 tiles: border at columns 0-1 and 30-31, the playfield at columns 2-13 rows 8-27 (12 columns — 10 playable plus the two walls — by exactly 20 rows), a 6-column decorative divider at 14-19 carrying the vertical TETRIS banner, and the score/stats panel at 20-29. The header strip in rows 0-7 holds the SCORE / LINES / LEVEL / NEXT labels; those are multi-tile graphics, while "HIGH SCORE" and "STATS" are plain ASCII, because the tileset's letters and digits sit at their ASCII codes. |
| **The playfield's tiles come from the nibble itself** | `L8544` at `main.asm.txt:829-842` | The routine that fills the screen buffer stores the playfield nibble **directly** as the nametable tile id — no lookup, no offset. So cell value 1-14 is a block tile and the wall's `$F` is tile `$0F`, which is why walls need no special case in the renderer. `$0F` is a block graphic with transparent corners, not a solid bar; a renderer that assumes a solid wall column will look wrong. |
| **The dancers** | poses at `$C8BC-$C9FF`; driver at `main.asm.txt:6392-6499`; stage from `levelUpAnimationColsRows1` at `$B7FF` | The between-levels Cossack dancers are sprites: eight of them, each four 8x8 tiles in a 2x2 forming one 16x16 figure. A pose is just four tile ids; a per-dancer script advances one pose every 8 frames, the figure's X advances every 4 (they walk), and `eor #$40` on the OAM attribute flips them so they turn around. Their stage is the vertical TETRIS banner: the level-up blit is 4 columns x 18 rows at nametable (14,10), which clears exactly that banner to make room. **Not traced:** each dancer's individual choreography script — those have branch and random-selection entries — so the port walks the pose table from staggered starts at the ROM's cadence instead. |
| **Title and menu screens** | `titleScreenNametable` at `$CA00`; `menuNametable` at `$B8A8` | The title is 32x30 with a 4-tile-thick border; its nametable carries **no** attribute table (the next thing in the ROM is `fireworksData00`), because the whole screen is drawn in one palette — bgPalette0's blues. The menu is the same decorative frame with an empty middle the game writes its wording into at runtime, which is why its selection screens all look alike. |
| **The line-clear animation** | timer set at `main.asm.txt:1192-1197`; sprite staged by `L87FB` at `:1230-1273`; driver `stageLineClearAnimation` at `:1274-1338`; the write-back `L89E9` at `:1508-1546`; strings `lineClearSingle..lineClearTetris` at `:1548-1561`; palette `piecePaletteIndexA` at `:5394-5396` | Completing rows does **not** collapse them: the ROM marks each completed row with `$FE`, holds the game for `lineClearTimerP1` frames (`$1D` = 29 in 1P/2P, `$21` = 33 in coop) and animates them, collapsing only when the timer expires. The animation is a puff of smoke crossing each completed row left to right — five 8x8 sprites, tiles `$5B..$5F`, drawn in `piecePaletteIndexA`, which is `$0F,$0F,$0F`: **flat black**, a silhouette. Only the head is staged when the row completes; each step the sprite still sitting at the field's first column clones itself one OAM slot back with the next tile down, so the trail builds itself up to five. It advances one column **every other frame** — the driver decrements the timer every frame but acts only on odd values (`lsr a / bcc`, `:1280-1283`) — giving 14 steps for the 29-frame hold. The trailing sprite writes one character per column into the row it passes over, spelling `" SINGLE     "` / `" DOUBLE     "` / `" TRIPLE     "` / `" TETRIS     "` (12 characters, one per playfield column, walls included) chosen by `12 × rows_cleared` bytes past `lineClearTable`. Because the tail starts four columns behind the head and there are only 14 steps, it reaches the tenth column and no further, so the last two columns of the row keep their blocks until the collapse — reproduced as-is rather than tidied up. |
| **Pause** | `pauseOrUnpause` at `main.asm.txt:7184-7215`; gating at `:444-446`; plaque data at `:7293-7312` and `:8027-8028`, tiles at `:8061-8063` | Start toggles `gameState` between PLAYING (0) and PAUSED (1), and is ignored from any other state — so it does nothing on the game-over screen. Pausing stops gameplay but **not** the line-clear animation, because `stageLineClearAnimation` is called from the main loop unconditionally (`:66-70`) while `branchOnActiveDemoOrGameOver` returns early unless `gameState` is 0. The plaque is an 8×2 blit of the cartridge's own tiles at nametable (12,8), coloured with background palette 3 (`pauseAttrs` = `$EF,$BF`); unpausing restores those same two rows from `gameModeNametable1P+268`. |
| **The cheat codes** | `checkCodeInput` at `main.asm.txt:7025-7182`; tables at `:7175-7182`; snapshot `L85B3` at `:911-925`; undo disarm `L94E4` at `:3087-3092`; long-bar refresh at `:3189-3190` | Entered **while paused**, one button per frame, and only if that player is still alive. Level up = Up Down Up Down Left Right B B A; long bar = Down Down Left Right Left Right B A; undo = Left Down Right Up Left Down Right B A. All three live in ONE table and share ONE cursor (`codeInputYPlayer1`), which is why several behaviours fall out that look like bugs and are not: a first press is tested against all three starts and commits to the first that matches (undo, then long bar, then level up); a press that breaks a sequence is **swallowed**, not re-tested; and the cursor is **never rewound on success**, so the last button of a completed code re-fires it — that is how the level-up code is repeated with bare A presses. Limits: level up is unlimited but stops at 17 (`:7059-7067`); the long bar is once per level, and only levelling up **by play** hands it back (`:3189-3190` — the level-up code deliberately doesn't, so the two can't be alternated); the undo is once per game and needs a snapshot, which a line clear wipes. The undo takes the last locked piece back out of the field, makes the piece you were holding `next`, rewinds the RNG to `lastRNGSeedP1`, and drops the recovered piece in at the top. |

## PLACEHOLDER (implemented, but not yet checked against this ROM)

Nothing. Every mechanic the core implements is now traced to the
disassembly and cited both here and at its point of use in
`src/tengen_core.c`, and the graphics all come from the cartridge rather
than being redrawn.

One thing is deliberately *not* implemented rather than guessed at: each
dancer's individual choreography script. It doesn't affect the rules the core
models.

One known deviation, documented rather than reproduced: the ROM keeps score
and line counts as ASCII digits and does its arithmetic digit by digit. The
core uses plain integers and reproduces the one place where that's
observable — the score wrapping to 100000 past 999999. The line counter has
a similar digit clamp (`main.asm.txt:3129-3133`) that isn't modelled,
because reaching 10000 lines in one game isn't a realistic scenario to
preserve bug-for-bug.

## Suggested next disassembly targets (in priority order)

1. Each dancer's choreography script (the pointer tables the driver at
   `main.asm.txt:6392-6499` walks) — the only part of the level-up
   interlude still approximated.
2. The 2P and coop front end: the menu rows for game type, handicap and
   music (`main.asm.txt:4742-4830`), and the second player's screen half.

## A note on frame rate

NES NTSC runs at ~60.0988 Hz; GBA runs at ~59.7275 Hz. `tengen_step` is
designed to be called once per rendered frame on either platform with no
compensation for that ~0.6% difference — it's small enough (over a 20-line
game, well under a frame of total drift) that it isn't worth the complexity
before more of the core is verified. Revisit only if playtesting shows it
matters.
