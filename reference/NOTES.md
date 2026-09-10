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
- Tengen's playfield is fixed at 10 columns × 20 rows of 8×8 tiles = **80×160 px**,
  at nametable columns 2-11, with the cartridge's braided frame at columns 0-1
  and 12-13 — so the framed board is 14 columns, **112×160 px**.
- 160 px is exactly the GBA's full screen height. The playfield needs **zero**
  vertical scaling or cropping on GBA — every row is visible, pixel-for-pixel,
  same as NES, and it keeps the NES's own column positions.
- **The walls are frame ART, not blocks.** The ROM never paints its playfield
  buffer's wall cells over them; they are drawn once with the screen. Three
  things say so: exactly ten blank columns sit between the frames in the
  nametable, the line-clear sweep runs from x `$10` to `$58` (columns 2 to 11
  and no further, `L8840`), and the pause plaque is blitted at column 12,
  right where the frame starts. A renderer that draws the core's wall
  sentinels as block tiles covers that art and shifts the field a column —
  which is exactly what an earlier pass of this port did.
- The NES screen holds TWO such framed board areas: columns 0-13 and columns
  18-31. 2P puts a player in each; 1P draws its score panel over the second.
- **Horizontally**, 32 columns become 30 and the columns are also
  RESEQUENCED. On the NES the 1P playfield sits well left of centre, because
  the screen is really two board areas and 1P plays in one; on a GBA showing
  one player that reads as lopsided. So the runs of columns are reordered to
  put the ten playable ones dead centre (port columns 10-19, 80px of screen
  either side) with the HUD split around them: counters and statistics left,
  next piece right. Every run moves whole — nothing is scaled or cropped —
  and the two columns that do not fit are the second board area's right
  frame, which has nothing left to frame. `SCREEN_SEGMENTS` in
  `tools/extract_assets.py` is the order; `make gba-check` asserts the
  centring against the running ROM.
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
| Level-up thresholds | `main.asm.txt:1473-1478` (`bonusLinesTable`), **and its two readers at `:1482-1487` and `:3145-3151`** | Bytes decode as ASCII digit pairs: 03,06,09,12,15,20,25,30,...,95. **THOSE PAIRS ARE HUNDREDS-AND-TENS, NOT TENS-AND-ONES.** Both readers compare them against `player1LinesHundreds` and `player1LinesTens` — the top two digits of the line counter — so the ones digit never enters the test and "03" means the first total whose tens digit is 3: **thirty lines**. The real curve is **30, 60, 90, 120, 150, then every 50 to 950**, which is also what the ROM's own comment above the table says ("first check at X03X, then every 30 lines until X150 at which point it's every 50 lines"). Reading the pairs as tens-and-ones gives 3, 6, 9 ... 95 — a level every three lines — and that is what this port shipped with until it was caught; the table is 21 entries either way, which is why the count matched while every value was ten times too small. `TENGEN_LEVEL_LINE_THRESHOLDS` now holds the line totals and `TENGEN_LEVEL_LINE_TENS` the ROM's own pairs. |
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
| **Attribute tables are NOT beside their nametables** | copy loop in `sendNametableToPPU`, `main.asm.txt:6941-6963`; screen addresses at `nametableAddressTable`, `:6990-6998` | A screen is 960 tile ids plus a 64-byte attribute table saying which of four palettes each 2x2 block uses, and `sendNametableToPPU` copies **1024** bytes to `$2000`. But the screens are stored **960 bytes apart**, so slicing "nametable then attributes" out of the ROM hands you the next screen's first two rows and calls them palettes. It looks almost right and is wrong everywhere: this port had the playfield's braided frame changing colour down its length and a monochrome title before the screens were read by RUNNING that routine on the 6502 interpreter instead (`read_screen` in `tools/extract_assets.py`). The truth: the 1P frame is uniformly bank 2 top to bottom, the playfield is bank 0 (which is what `setPlayfieldPaletteFromLevel` recolours), the banner is bank 3, and the whole header strip is bank 3. |
| **Which palette set each screen uses** | `updatePalette` at `main.asm.txt:5268-5287`, tables at `:5292-5321`; callers at `:2044`, `:3308-3310`, `:4492-4494`, `:4547-4549` | `updatePalette(n)` writes 16 bytes — four palettes — from `bgPalette0 + n*16` to the background palettes for n<3 and the sprite palettes for n>=3. Title: `bgPalette0` + `spritePalette1`. Menu: `bgPalette1` + `spritePalette0`. Game: `bgPalette2` + `spritePalette0`. The level-up interlude switches sprites to `spritePalette2`, and the dancers' own attribute bytes (`$8E78`) pick among its four — which is why the six are three different colours, not one. The title screen in particular uses **all four** of its palettes; assuming one covered it is what made this port's title monochrome. |
| **Screen layout** | `gameModeNametable1P` at `main.asm.txt:C028` onward | The 1P screen is 32x30 tiles and is built from TWO identical framed board areas side by side: braid at columns 0-1, ten playable columns at 2-11, braid at 12-13, the 4-column TETRIS banner at 14-17, then the same again — braid at 18-19, ten columns at 20-29, braid at 30-31. The playfield rows are 8-27, exactly 20. 2P puts a player in each area; 1P draws its score/stats panel over the second one, which is why that half is blank in the nametable. The header strip in rows 0-7 holds the SCORE / LINES / LEVEL / NEXT labels; those are multi-tile graphics, while "HIGH SCORE" and "STATS" are plain ASCII, because the tileset's letters and digits sit at their ASCII codes. |
| **The playfield's tiles come from the nibble itself** | `L8544` at `main.asm.txt:829-842` | The routine that fills the screen buffer stores the playfield nibble **directly** as the nametable tile id — no lookup, no offset. So cell value 1-14 is a block tile and the wall's `$F` is tile `$0F`, which is why walls need no special case in the renderer. `$0F` is a block graphic with transparent corners, not a solid bar; a renderer that assumes a solid wall column will look wrong. |
| **The dancers** | poses at `$C8BC-$C9FF`; positions at `$8E5C/$8E6A/$8E78`; how many, at `main.asm.txt:2085-2108`; stage blit `levelUpAnimationColsRows1` at `$B7FF` with tiles at `LC82C` ($C82C); driver `LB015` at `:6392-6499` | Sprites, four 8x8 tiles in a 2x2 each. **A 1P or 2P game shows six**, stacked in ONE column at x `$61` with y `$D0,$B8,$A0,$88,$70,$58` — 24 pixels apart; coop instead uses entries 6-13, in pairs down the two sides. How many actually appear is `L8D8B` (`:2050-2082`) and it is now ported: **one, plus one per triple and two per tetris cleared since the last level-up**, summed over the players still in the game, capped at 8 and then at **6 in 1P and 2P** — coop is the only mode that uses all eight, because it is the only one with a second column of positions. Singles and doubles buy nothing. The tally lives at `$6C-$73` (two players interleaved, `$6C`/`$6E`/`$70`/`$72` for p1 = singles/doubles/triples/tetrises), is zeroed on a new game (`:3455-3458`) and again by `finishLevelUpAnimation` (`:2476-2482`), so it is **per level, not per game**; the level-up screen also weights it x1/x4/x9/x25 (`L8E54`, `:2160`) for its bonus figures, which the port does not show. In the port it is `TengenPlayerState.clear_counts` and `tengen_dancer_count`. The level-up blit is not just clearing the TETRIS banner to make room: it **draws their stage** into it — 4 columns x 18 rows at nametable (14,10), five ledges of tile `$9D` one every three rows, i.e. 24 pixels apart, exactly under the six dancers' feet. They start just left of the banner and walk right onto it, one pixel every four frames, while the pose advances every eight. **The driver is now traced too** (`LB015`): each dancer holds a pointer into a little program (`$019A/$01A2`), advanced by one 2-byte entry every 8 frames, and what an entry MEANS is decided by comparing its value against two addresses — `>= $C8BC` is a pose (four tile ids), `>= $B14D` but below that is a jump to another program, and below `$B14D` it is a random branch: `shuffleRngSeed5x` picks one of sixteen pointers from the table the entry names. A dancer only walks while its program lies below `$B181`. **Still not wired up in the port:** the program DATA itself, so the port's dancers walk the pose table from staggered starts instead of following their own scripts. Everything else about them — art, poses, stage, positions, count, cadence — is the ROM's. |
| **Title and menu screens** | `titleScreenNametable` at `$CA00`; `menuNametable` at `$B8A8` | The title is 32x30 with a 4-tile-thick border; its nametable carries **no** attribute table (the next thing in the ROM is `fireworksData00`), because the whole screen is drawn in one palette — bgPalette0's blues. The menu is the same decorative frame with an empty middle the game writes its wording into at runtime, which is why its selection screens all look alike. |
| **The line-clear animation** | timer set at `main.asm.txt:1192-1197`; sprite staged by `L87FB` at `:1230-1273`; driver `stageLineClearAnimation` at `:1274-1338`; the write-back `L89E9` at `:1508-1546`; strings `lineClearSingle..lineClearTetris` at `:1548-1561`; palette `piecePaletteIndexA` at `:5394-5396` | Completing rows does **not** collapse them: the ROM marks each completed row with `$FE`, holds the game for `lineClearTimerP1` frames (`$1D` = 29 in 1P/2P, `$21` = 33 in coop) and animates them, collapsing only when the timer expires. The animation is a puff of smoke crossing each completed row left to right — five 8x8 sprites, tiles `$5B..$5F`, drawn in `piecePaletteIndexA`, which is `$0F,$0F,$0F`: **flat black**, a silhouette. Only the head is staged when the row completes; each step the sprite still sitting at the field's first column clones itself one OAM slot back with the next tile down, so the trail builds itself up to five. It advances one column **every other frame** — the driver decrements the timer every frame but acts only on odd values (`lsr a / bcc`, `:1280-1283`) — giving 14 steps for the 29-frame hold. The trailing sprite writes one character per column into the row it passes over, spelling `" SINGLE     "` / `" DOUBLE     "` / `" TRIPLE     "` / `" TETRIS     "` (12 characters, one per playfield column, walls included) chosen by `12 × rows_cleared` bytes past `lineClearTable`. The string is 12 bytes but the sweep only crosses the ten playable columns, so its last two characters are never used; the tail, starting four columns behind the head, reaches the tenth column on the last step of the hold. |
| **Pause** | `pauseOrUnpause` at `main.asm.txt:7184-7215`; gating at `:444-446`; plaque data at `:7293-7312` and `:8027-8028`, tiles at `:8061-8063` | Start toggles `gameState` between PLAYING (0) and PAUSED (1), and is ignored from any other state — so it does nothing on the game-over screen. Pausing stops gameplay but **not** the line-clear animation, because `stageLineClearAnimation` is called from the main loop unconditionally (`:66-70`) while `branchOnActiveDemoOrGameOver` returns early unless `gameState` is 0. The plaque is an 8×2 blit of the cartridge's own tiles at nametable (12,8), coloured with background palette 3 (`pauseAttrs` = `$EF,$BF`); unpausing restores those same two rows from `gameModeNametable1P+268`. |
| **The sound engine runs, it is not ported** | entry points `setMusicOrSoundEffect` at `main.asm.txt:8357` and `updateAudio` at `:8376`; APU write-out `setApuRegisters` at `:9617-9711`; note periods `LDD37` at `:10495`; music tracks from `musicTrackLoginska1` at `:10651` | Tengen's audio is a dense piece of 6502 — vibrato, portamento, per-channel envelopes and a sound-effect priority system that decides which channels an effect may steal from the music — working almost entirely through unlabelled RAM. Rather than transcribe (that is, guess at) it, the port **executes it**: `gba/nes6502.c` is a small 6502 interpreter and `gba/audio_prg.h` is the slice of the cartridge holding the engine and every note of its music. Two facts make that affordable and safe, and both are measured rather than assumed. It touches **nothing** outside RAM and `$4000-$4017` — no PPU, no mapper — which `tools/extract_assets.py` re-proves on every asset build by running all 25 tracks and trapping any other access. And it is small: ~600 6502 instructions per frame typical, ~2300 in its worst, which is why the interpreter fits in a GBA frame with room to spare (`make gba-check` asserts gravity still lands on exactly 33 frames per row). The APU-to-PSG mapping in `gba/nes_audio.c` is the one thing genuinely translated; it is documented there. |
| **The audio cues** | game over `main.asm.txt:608, 620`; piece drop `:637`; level-up interlude `:2038`; line clear and its level-up variant `:3207-3213`; title `:4489`; menu move `:4655`; pause suspend/resume `:7204-7211`; codes `:7089, 7127` | Which sound plays when, taken one by one rather than invented. Notably: a line clear that also raises the level plays `MUSIC_LEVELUP_INTRO` **instead of** `SOUND_LINECLEAR`, not as well as; topping out plays `MUSIC_SILENCE` and then `MUSIC_GAMEOVER`; and every applied cheat code plays `SOUND_SCREEN_SWITCH`. |
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
2. The 2P starting handicap: `initHandicapGarbage` (`main.asm.txt:3545-3598`)
   and its `garbageHeightData` at `$98A7` = `$B8,$A0,$88,$70`. The port's
   linked two-player game is a straight race with no handicap yet.
3. The coop front end and the second player's screen half, and the menu rows
   for game type and handicap (`main.asm.txt:4742-4830`). The port's GAME
   SELECT lists only the two modes it implements, in the cartridge's own
   wording; the ROM's five are 1 PLAYER / 2 PLAYER / COOPERATIVE / VERSUS
   COMPUTER / WITH COMPUTER at nametable rows 14-20.
4. `computerMove`, the AI behind VERSUS COMPUTER and WITH COMPUTER — the one
   two-player mode that needs no second console.

## The walls do not stop at the top of the visible field

The ROM's playfield is one flat array of 8-byte rows starting at row 0, and
`L89C3` (`main.asm.txt:1481-1503`) writes the `$F0`/`$0F` wall nibbles into
every row it builds — the rows a piece SPAWNS in included. Only coop leaves
them clear (`bit playMode` there), which is what widens its field to twelve.

The port stores only the twenty visible rows, so the spawn rows have to assert
the walls themselves. Treating them as open space instead was a real bug and a
nasty one: a piece could be walked sideways into the wall column while it was
still above the field, and the first row it descended into then blocked it —
so it came to rest at y=5, one short of `TENGEN_TOPOUT_ROW`, which ends the
game. Four pieces into an empty board, GAME OVER, with nothing on screen to
explain it.

Two tests guard it: `test_the_walls_reach_above_the_visible_field` for the
rule, and `test_random_play_never_tops_out_on_a_nearly_empty_board`, which
plays 300 random games and fails if any ends with fewer than 24 cells down.
The second is the one that would have caught it: the rule it tests is not a
ROM detail, it is "a game does not end for no reason".

## The GAME OVER plaque, and the frame the HUD borrows from it

`gameOverTiles` ($C800, `main.asm.txt:8064-8067`) is six columns by four rows,
blitted at nametable (4,12) in 1P — the middle of the playfield — by
`gameOver1pColsRows1` (`$86,$04`) and `gameOver1pPPUAddr1` ($2184), in
background palette 3:

    29 2A 2A 2A 2A 2B        a box top
    2C 47 41 4D 45 2F        | G  A  M  E |
    2C 4F 56 45 52 2F        | O  V  E  R |
    3A 3B 3B 3B 3B 3C        a box bottom

so the cartridge's own thin frame is in there, and the port's HUD panels are
drawn with it. What they had before was `$75`/`$76` with **`$79` as a
right-hand cap, and `$79` is not a cap** — it is an unrelated block, which is
what the grey stubs beside SCORE / LINES / LEVEL were. The header strip's real
rules run the width of the NES screen and are junctions of a grid the port has
no room for.

## The menu loses two columns, and not from the middle

The menu's horizontal TETRIS logo is at rows 10-12, columns 4-27: six letters
of exactly four columns each, with no empty middle to borrow from. Taking the
two spare columns from there cut the third letter's last column and the
fourth's first, mashing the T and the R together. They come out of the blank
padding at columns 2 and 29 instead.

## The 1P screen, and what the port does with it

The cartridge's 32 columns are three things side by side, and reading them
wrong is what made every earlier layout here lopsided:

| NES columns | What it is |
| --- | --- |
| 0-1 | braid `6A 6B` — the playfield's LEFT wall |
| 2-11 | the ten playable columns, rows 8-27 |
| 12-13 | braid `73 74` — its RIGHT wall |
| 14-17 | the vertical TETRIS banner, a raised pillar |
| 18-19 | braid `6A 6B` again |
| 20-29 | the score panel: blank canvas the game writes into |
| 30-31 | braid `73 74` |

`6A 6B` is a left-hand wall and `73 74` a right-hand one, and the pair either
side of a run is what makes it read as a recessed well; the banner has them
the other way round, which is what makes IT read as raised. Rows 0-1 and
28-29 close the frame top and bottom, and rows 2-7 are the header strip
(SCORE / HIGH SCORE / LINES / LEVEL / STATS / NEXT).

The port's thirty columns are `8 | 2 | 10 | 2 | 8`: a box, the board's own
braid, the ten playable columns, the braid, a box of the same width. What
that costs is the TETRIS banner, which has no room on the play screen any
more — the right-hand box becomes the dancers' stage during a level-up, the
way the cartridge's own blit takes over the banner.

Two fix-ups the column runs cannot express, both because a column is not
uniform down its length: NES rows 8-9 of columns 12-13 hold the banner box's
top corners rather than braid, and rows 26-27 of columns 21-27 hold the piece
icons. Both are patched in `reflow_screen`.

### The piece statistics are BARS

`L9997` (`main.asm.txt:3752-3798`) draws each piece's count as a vertical bar
growing out of a little picture of that piece, not as a number:

    tile = $21 + (count & 7)          eight steps of fill inside one tile
    row  = base - (count >> 3)        every eighth piece moves up a row

so after N pieces the bar is N/8 solid tiles with an N%8 partial on top. The
icons sit at nametable rows 26-27, columns 21-27, and the attribute table
gives the I its own palette (bank 3), T/O/J/L a second (bank 1) and S/Z a
third (bank 2) — which is why the row is not seven identical shapes. The ROM
caps the bar at 144 (`cmp #$90 / bcs`), exactly the 18 rows its panel is
tall; the port's box is shorter so the same rule caps lower.

The column is HEADED, not boxed. On the NES 1P screen the word STATS sits
between two grey rules and the icons stand on the frame's own bottom edge,
with nothing else in the column — and seven bars do not fit inside the
six-column interior a bordered box would leave. An earlier pass drew a single
rule immediately under the NEXT box's own bottom edge, which read as two
borders stacked and as a grey bar belonging to nothing; that is the same
complaint the header strip's rules drew before them.

### How long the dancers dance

Not a number to guess at. `checkLevelUp` (`main.asm.txt:1956-1979`) reuses
player1FallTimer as the interlude's clock and advances it once every SIXTEEN
frames (`lda frameCounterLow / and #$0F / bne`):

| Timer | What happens |
| --- | --- |
| 0 | `showLevelBonus` (:1926): silence, gameState = LEVELUP |
| 13 | `L8D6B` (:2034): level-up music, the dancers' palette, the stage blit, and the timer is forced to `$7C` = 124 |
| 124 → 244 | they perform — 120 steps of 16 frames, about 32 seconds |
| 244 | `L9035` (:2393) starts the wind-down, forcing the timer to `$F5` |
| wraps past 255 | `finishLevelUpAnimation` (:2465), back to play |

A button does not cut it short, it fast-forwards. `L9035` (`:2393-2411`)
reads `player1ControllerNew | player2ControllerNew` — either pad, any newly
pressed button, and only while the timer is still below `$F4` — then silences
the music and computes `$7C - timer - 5` **in eight bits, compared unsigned**
against `$F5`. That underflow is the whole behaviour: a press in the first
few steps lands on `$FB` and a press after about the sixth on `$F5`, so what
you buy is a wind-down of between five and eleven steps, one to three seconds,
never an instant cut. It also does `frameCounterLow &= $F0` so the next step
starts from a fresh sixteen.

The natural end at `$F4` reaches the same `$F5` by a different door (`beq
L9053`, taken BEFORE the silence), which is why the level-up music plays out
when you let the show finish and stops dead when you cut it.

Measured on the port: untouched the dancers are on screen 1871 frames; a
button ends it 176 frames (2.9s) later, whenever it is pressed.

### Pausing hides nothing

`stageCurrentAndNextSprites` (`main.asm.txt:1687-1693`) is the only thing that
decides whether the falling piece and the preview are on screen, and it reads
`gameState`:

    cmp #GAMESTATE_GAMEOVER ($F9)  -> draw
    cmp #GAMESTATE_DEMO     ($FB)  -> draw
    cmp #GAMESTATE_LEVELUP  ($03)  -> bcs: skip
    otherwise                      -> draw

`GAMESTATE_PAUSED` is `$01`, below `$03`, so it falls through to the drawing
path: **the falling piece and the next piece both stay on screen while
paused**, and so does the settled field — pausing only patches the PAUSE
plaque into the background (`updateGameBackground`, `:7217`) and leaves the
rest of the nametable alone. Only the level-up interlude ($03 and up) takes
the pieces down. The port matches; the one difference is that its falling
piece is a background tile rather than a sprite, so the plaque covers it
instead of the other way round on the two rows they share.

### The sixth dancer stands on the border, not on a ledge

The level-up blit lays FIVE ledges (`kDancerStage`, tile `$9D` at rows 3, 6,
9, 12 and 15 of the eighteen it writes from nametable row 10), so their tops
are at NES y 104, 128, 152, 176 and 200. `kDancerStartY` puts SIX dancers at
y 208, 184, 160, 136, 112 and 88, each sprite 16px tall, so their feet land at
224, 200, 176, 152, 128 and 104. Five of those are ledges; the sixth, at 224,
is the border tiles across the bottom of the NES screen.

The port's window is NES nametable rows 8-27 and stops one row short of that
border, which left the bottom dancer treading air on the screen edge. The show
therefore starts one tile row higher (`DANCER_LIFT`) and the port draws a
sixth ledge of the same `$9D` below the blit. The 24-pixel spacing that ties
the two ROM tables together is untouched; only the whole column moves.

### Where the PAUSE plaque goes

`pausePPUAddr1 = $210C` with `pauseColsRows1 = $88,$02` — eight columns by two
rows at nametable (12,8), i.e. columns 12-19 of 32. That is the middle of the
screen, and the relationship worth keeping is "centred", not "column 12":
on thirty columns it centres at 11.

### $4015 is this engine's note-off

The sound engine sets the length counter's halt bit on every note it starts
(`$4000 = $B7`) and ends notes by clearing the channel's bit in `$4015`,
several times a frame as it works through the voices. So the APU's length
counters never count down in this game, and a port that models them gets
nothing; what it must do instead is treat the enable bits as part of "has
this channel changed". Measured against the running ROM, not assumed.

## The title screen's sprites are the cartridge, running

Two routines draw everything that moves on the title screen, and neither is
reimplemented in the port — both are executed, on the same 6502 interpreter
that already runs the sound engine (`gba/nes6502.c`, `gba/audio_prg.h`), and
`gba/main.c` copies the sprites they leave in `oamStaging` ($0500) into GBA
OAM. `make gba-check --title` asserts all of it against a running ROM.

### drawCathedralSprites (`$B369`, main.asm.txt:6850)

Eighteen sprites laid over the cathedral, from a table the disassembly itself
labels *"this table is obfuscated"*. Each 4-byte entry is `tile, attr, packed,
packed`, and the position comes out of the last two by an ASL x3 for x, then
two `LSR`/`ROR` pairs, an `AND #$F8` and an `SBC ppuScrollYOffset` for y. Its
own worked example is the only readable description of the encoding:

    in  $02,$03,$CF,$01   ->   out y=$6F, tile=$02, attr=$03, x=$78

and the port reproduces it exactly, because it runs it. The eighteen come out
at NES x 120-160, y 111-175 — the central tower's stripes and the middle
domes, detail the background cannot hold under the NES's one-palette-per-16px
attribute grid.

The port stages them ONCE per visit to the title rather than every frame. The
cartridge re-runs the routine every frame only because its NMI rebuilds the
whole OAM page every frame; the inputs are a constant table and
`ppuScrollYOffset`, which nothing but the title's hidden both-Downs scroll
(main.asm.txt:4470-4476) changes and this port has no scroll. Interpreting
~500 6502 instructions to arrive at the same eighteen bytes was costing about
one frame in fifty-five, which `--title` measures directly.

### The fireworks (`$A9CE`, main.asm.txt:5730)

A little bytecode, run once per frame. `addrTableAB25` ($AB25) holds four
scripts; the title always takes the first, `relatedToFireworksTable0`, while a
top-out during a game picks one of the four at random and plays a top-out
sound with it. Each script entry is three bytes — the high and low halves of a
pointer, plus a step code — naming one of nine 8x6 blocks of tile ids
(`fireworksData00`..`08`, `$CDC0`-`$CF78`) that expand into a burst, or the
sparkle frames built from tiles `$14`-`$17`.

| Where | What |
| --- | --- |
| `LAA70` (:5836) | starts a burst: 45 sprites (OAM entries 19-63), y from `$50` on the title, x clamped to `$2C`..`$D4` then less `$1C`, rows 24px apart |
| `LACA0` (:6096) | one step: a random drift of -15..+15 in x and 0..7 in y applied to all 45, and a random one of four palettes |
| `LA9E9` (:5752) | advances the script every fourth frame (`frameCounterLow & 3`) |
| `LA9DE` (:5740) | counts down `player2FallTimer` to the next burst — `rng & $3F + 8`, so 8 to 71 frames |
| `LAA07` (:5772) | ON THE TITLE, stops scheduling once `frameCounterHigh` reaches 4 |

That last row is why `initializeTitleScreen` zeroing the frame counter
(main.asm.txt:4483-4485) matters: the show lasts about 1024 frames — seventeen
seconds — per visit, and without restarting the counter it would play once and
never again. (At `frameCounterHigh` = 5 and `frameCounterLow` = `$20` the
cartridge starts its attract-mode demo, main.asm.txt:4154-4160. Not ported.)

The bursts call `setMusicOrSoundEffect` themselves, which is why they have to
run on the sound engine's machine and not a second one: the bang comes out of
the same RAM the music does and mixes by the cartridge's own priority rules.

### What the narrower screen costs

The sprites are placed in NES screen pixels, and the port's title is a
composition rather than a window — ten of the thirty rows are dropped (see
TITLE_ROW_BLOCKS) — so a sprite's row goes through `kTitleRowMap`, the same
list the artwork was cut with, and one standing on a dropped row is hidden
rather than moved. Horizontally the port keeps NES columns 2-29, so a burst
that `LACA0` has drifted far enough sideways clips at the edge. It clips on
the NES too, eight pixels later.

### CHR bank 3

None of this draws with the dancers' tiles: the title's sprites come from CHR
bank 3, which holds the cathedral overlay at `$02`-`$13`, the sparkles at
`$14`-`$17` and the firework bursts filling everything from `$90` up. The port
uploads it above the dancers' 256 tiles and installs `spritePalette1` for it,
which is the set the title itself installs (main.asm.txt:4492-4494).

## The prototype's title screen, and how it was found

Tengen made this game twice: the prototype cartridges, from while the licence
was still Nintendo's, carry a different title — "TENGEN PRESENTS / TETRIS"
over another cathedral, gold-and-blue onion domes instead of gold-and-red, and
a green fret border where the release has its blue braid. The port offers it
as a skin: L or R on the title swaps between them, announced with
`SOUND_CHIRP` ($10), one of the four effects `constants.asm.txt` marks "maybe
unused" — so the egg speaks in the game's own voice with a sound the game
itself never plays.

### These builds are simpler than the release

The release RLE-compresses its nametables and has to be RUN to unpack them
(see the note on `sendNametableToPPU`). The prototype's upload routine is a
flat four-page copy:

    sta $3C / lda TABLE,y / sta $3D / bit PPUSTATUS
    lda #$20 / sta PPUADDR / lda #$00 / sta PPUADDR
    tay / ldx #$04
  @page:
    lda ($3C),y / sta PPUDATA / iny / bne @page / inc $3D / dex / bne @page

so each screen is 960 plain bytes of nametable in the ROM. The routine's own
pointer table holds five; rendering all five identified the title at `$A3C4`.

### Everything else about it was traced, after being guessed wrong once

Two things looked settled and were not, and both showed up as a picture that
was merely *plausible* rather than right:

**The attributes are not in the blob.** The copy moves four pages, so it does
write over the attribute table at `$23C0` — but `$904D` then uploads the real
attributes there, RLE'd as (count, value) pairs terminated by a zero count,
from its own table at `$907D`. Taking the blob's last 64 bytes for attributes
gives sixty-four bytes of PROGRAM, which is what put the cathedral in green
and red stripes the first time this ran.

**The palette was picked by eye, and the eye was wrong.** There are four
16-byte sets at `$91E5`; an earlier pass rendered the title under all four and
kept the one that looked best. It looked best and it was `$9205`, index 2 —
another screen's. The real answer is not a judgement call at all, because each
screen's setup does the same three things with the SAME index (the title's is
0, at `$8C7C`-`$8C92`):

| Call | What it loads |
| --- | --- |
| `lda #0 / jsr $91C0` | palettes: `$91E5 + index*16` -> `$3F00` |
| `lda #0 / jsr $93D9` | nametable: the index'th pointer in the table at `$9403` |
| `lda #0 / jsr $904D` | attributes: the index'th entry of `$907D`, RLE |

Index 3 is not a background set at all: `$91E1,y` supplies the destination
low byte, and only index 3's is `$10`, making it the sprite palettes at
`$3F10`.

### What the port does and does not carry over

The skin is only ever the PICTURE. The cathedral overlay and the fireworks
stay on the release screen and are hidden on the prototype's, because they
ARE the release's — their sprites are placed in NES pixels over the release
cathedral, and the prototype's composition has neither the same rows nor an
empty sky to burst in. Putting them there would be inventing something neither
cartridge does.

Its frame is two columns and two rows on every side — a thin outer rule with
the fret inside it — so the two columns and ten rows the GBA lacks come off
the outer rule (all four sides, symmetrically), the five blank rows under the
cathedral, and three of the four rows of thin spires. The blank row between
PRESENTS and the logo is deliberately NOT one of them: buying it left the big
letters' ascenders sitting inside the word above.

### Only this prototype

Of the three dumps, only one stores its title flat at a findable address.
The other two have differently shaped pointer tables and a brute-force scan of
every 1024-byte window in both turned up no title screen, so getting theirs
would need a disassembly of each, and none exists. `read_proto_title` refuses
to guess: a dump without the expected first row is rejected with a reason
rather than converted into 1024 bytes of noise, and the port then builds with
`SCREEN_PROTO_AVAILABLE 0` and L/R simply have nothing to switch to.

## MUSIC MIX, and why it turns over at the level and not at the end of a tune

The same L+R that uncovers Korobeiniki uncovers a sixth entry that is not a
tune: MUSIC MIX plays the five in turn.

**When it changes is a traced decision, not a taste one.** "When the tune
ends" needs a tune length, and these tunes loop. Correlating the melody
registers (pulse 1 and 2, period and volume) over two hundred seconds of each
of them, taken off `tools/nes_cpu.py`, finds a clean loop for exactly one:

| Tune | Best period | Match |
| --- | --- | --- |
| Troika | 1969 frames (32.8 s) | 99-100% |
| Karinka | 2560 frames (42.7 s) | 44% |
| Loginska | 4381 frames (73.0 s) | 28% |
| Bradinsky | 3142 frames (52.4 s) | 17% |

Only Troika repeats. Hashing the engine's whole RAM alongside the APU finds no
exact repeat in any of them inside 150 seconds, because their vibrato and RNG
counters never come back to where they were. So a "song length" for the other
three would be a number invented here rather than one traced from the
cartridge — ground rule 2, and the reason the mix does not use one.

The LEVEL-UP is a boundary the cartridge does define. The tune already stops
there for the dancers and is started again when they finish, so the mix simply
hands that restart the next entry; a linked match, which has no interlude,
asks for it on the spot instead. It also means the music changes because you
played well, which a timer could never manage.

## Korobeiniki is not on this cartridge

Worth stating plainly, because it is the one thing in this port that is not
the ROM's. Tengen's four tunes are Loginska, Bradinsky, Karinka and Troika
(`constants.asm.txt:39-42`). Korobeiniki — the pedlars' song from the 1860s
that most people call "the Tetris theme", because Nintendo's Game Boy version
used it — is not among them, and there is no arrangement of it anywhere in
this ROM to extract.

So it is entered by hand, in `gba/korobeiniki.c`, as a fifth tune hidden
behind L+R on the selection screen. Three consequences, all deliberate:

* **It does not go through the cartridge's engine.** Feeding it one would mean
  writing new data in a music format nobody has documented and patching it
  into the ROM image. That is exactly the kind of thing this project does not
  do, so the file is a small sequencer of its own writing the GBA's PSG.
* **It shares, it does not replace.** Choosing it tells the cartridge's engine
  to play `MUSIC_SILENCE` and leaves it running, so every sound EFFECT is
  still the ROM's — and, exactly as on the cartridge, an effect briefly steals
  a pulse channel from the music and the next note takes it back.
* **PAUSE needs its own stop.** `MUSIC_SUSPEND` only reaches the cartridge's
  engine. `make gba-check --korobeiniki` measures the sound registers to prove
  the pause is real, that the tune actually changes pitch rather than sitting
  on one note, and that the ROM's engine is still sounding underneath it.

The note table is not typed by ear either: a GBA pulse channel runs at
`f = 131072 / (2048 - R)`, so `R = 2048 - 131072/f`, and the table is that
formula evaluated for equal temperament with A4 = 440 Hz. Any row of it can be
checked with a calculator.

The unlock travels over the link cable, because the lobby already exchanges
the tune and only the master's survives the handshake — so a linked player who
never found the code still hears it.

## LA035: silence first, and the cursor plays the tune

Two bugs came out of the same routine not being read closely enough.

`setMusicOrSoundEffect` ($CFB1) only QUEUES a request: a ring at $0200-$0207
with its write index at $0209 and its read index at $0208, and a full queue
drops the request. Handing the engine a new track does NOT stop the old one —
that is what `LA035` (main.asm.txt:4730-4735) is for:

    LA035:  lda #MUSIC_SILENCE / jsr setMusicOrSoundEffect
            ldy menuMusic / lda musicSelectTable,y / jmp setMusicOrSoundEffect

**Silence, then the track, every time.** Without the silence the previous
tune's channels keep running underneath the new one, which is exactly how the
title theme ended up audible on top of a match's music.

And `musicSelectTable` ($A043) is `$08, $04, $05, $06, $07` — the
disassembly's own comment reads *"silence, loginska, bradinsky, karinka,
troika"*. FIVE entries, the first of which is no music at all. The port
offered only the four tunes for several builds, quietly dropping one of the
cartridge's own choices.

`LA035` has two callers worth knowing about:

| Where | When |
| --- | --- |
| `$A00A` (main.asm.txt:4694-4696) | every cursor move while gameState is GAMESTATE_MUSIC_SELECT |
| `$976C` (main.asm.txt:3428) | when a game starts |

The first is the interesting one: **moving the cursor plays the tune under
it**, and that — not anything explicit — is what stops the title theme on the
cartridge. This port folds the ROM's separate MUSIC SELECT screen into its
level-select screen, so it previews on cursor moves there, and once on
arrival so the screen tells the truth about what is playing.

## The title is the only screen with sprites on it

The cathedral overlay and the fireworks are OAM, and nothing else in the port
ever writes OAM — so nothing else ever cleared it, and leaving the title left
sixty-three sprites standing in the middle of GAME SELECT and every screen
after. `make gba-check --leave-title` now asserts both this and the music
above, since neither would show up in any other check.

## The HUD, inside the braid

The blue rope beside the playfield is the same weave the cartridge borders its
whole 1P screen with, so it has corners and horizontal runs as well as the
vertical ones everybody notices — read straight off the border of SCREEN_1P by
`read_braid_frame`, all of it in background palette bank 2:

| Piece | Tiles |
| --- | --- |
| corners (2x2) | TL `60 61 / 65 66`, TR `9E 64 / 9F 69`, BL `87 88 / 8C 8D`, BR `8A 8B / 8F D1` |
| top / bottom run | `62 / 67` and `89 / 8E`, one column, two rows |
| left / right run | `6A 6B` and `73 74`, two columns, one row |

**Two tiles thick, and that is not adjustable**: each tile is one half of the
rope cut lengthwise (render `$6A` and `$6B` side by side and it is obvious).
That single fact decides the whole layout. The reflow is `10 | 10 | 10` — a
box of rope, the ten playable columns, another box — and each box spends its
frame on three sides only, opening at the screen's edge where the screen
already ends, which buys **eight columns and sixteen rows** of interior. See
"Eight columns, and why the panels open at the screen's edge" for why eight is
the number that matters.

What fits, and what had to give:

* **Left**: SCORE, LINES, LEVEL and HIGH SCORE, each a label row over a value
  row. No frame around each counter any more — the box IS the frame, which is
  what makes the screen read as one object rather than a stack of little
  plaques.
* **Right**: NEXT over the piece statistics, in ONE rank of seven, which is
  what the cartridge draws and what eight columns finally allow. The icons, the
  bar tiles, the palettes and the arithmetic are all the ROM's.
* **The banner does not fit at all.** It is six letters of three rows each,
  eighteen rows with no padding anywhere in it, against sixteen of interior.
  So when L+R calls for it, it takes the column instead of the box — except
  for the two rope columns nearest the board, which are redrawn as a plain
  strip, because the playfield keeps its own frame whatever the HUD is doing.
  The dancers' stage is handled the same way.

### The weave has a direction, and it is the BOX's direction

`kBraidLeft` (`6A 6B`) and `kBraidRight` (`73 74`) are the cartridge's own
columns 8-9 and 20-21 — the two sides of ITS border — and they are mirrors of
each other, as are the four corners. They only fit each other one way, and
this has now been got wrong in both directions, so it is worth writing down
which way and why.

**The tile is chosen by which side OF THE PANEL it is on**, not by which side
of the board:

| Panel | Its rope | Run | Corners |
| --- | --- | --- | --- |
| left, cols 0-9 | on its RIGHT | `kBraidRight` | TR / BR |
| right, cols 20-29 | on its LEFT | `kBraidLeft` | TL / BL |

Choosing by the board instead — the run beside the board's left edge taking
the cartridge's own left-border tiles, so each vertical run sits exactly where
the ROM has it — is tempting and wrong: the corners it then meets are its
mirror, and the weave breaks at all four of them. The box wins. These are
boxes now, and a box's own four pieces have to agree with each other before
they agree with anything else. What it costs is paid where nobody looks: the
rope beside the playfield is the mirror of the cartridge's.

**What it must never cost is the weave changing direction when the HUD does.**
L+R hands the right column to the banner, which redraws those two columns as a
plain strip, and the strip has to use the panel's own tile or the weave flips
as the box comes and goes — which is what "cambia la greca de sentido" was.
`make gba-check --braid` asks exactly that, off the SCREEN rather than the map
so a mismatched palette bank cannot slip through: the sixteen pixels beside
the board must be identical in both modes, the two panels' runs must be
mirrors of each other, and coming back from the banner must restore what was
there.

### The labels carry a piece of the grid, and it comes off exactly

SCORE, LINES, LEVEL and NEXT are lifted from the cartridge's own nametable
rather than spelled in the ASCII tileset, so they are the game's lettering.
But its 1P panel rules each counter off with a grid, and the tiles at the
START and END of each word carry a vertical fragment of it — grey pixels
hanging off the S and the E for no reason once the grid is gone.

They come off exactly, not by redrawing: **the grid is colour 3 and the
lettering colour 1**, so `strip_grid` zeroes colour 3 in those tiles and
re-encodes them, and what is left is the letter alone. The cleaned words go
into their own tile range (`HUD_LABEL_TILE_BASE`, 768 up) so the originals
stay available.

The grid itself is worth keeping, just not there: tile `$76` is four rows of
colour 3 — the ROM's own rule — and the port lays a row of it under each
counter's value, which is where the cartridge's grid ran anyway. Memorable,
and now it separates the entries instead of fraying the words.

## The fireworks are one object

`LAA41` (`main.asm.txt:5807-5820`) walks staging entries `$4C` upwards adding
the same offset to every one of their Y bytes: forty-five sprites, ONE burst,
one motion. And they are a 7x7 GRID minus its corners, forty-eight pixels
square — the ring you see is in the TILES each cell is given, not in where the
cells are.

That matters for a composition that drops rows out of the middle of the
picture. The cathedral's eighteen sprites are fixed artwork lining up with
fixed background, so one of them landing on a dropped row has nothing left to
line up with and is rightly hidden. Sending the fireworks through the same
per-sprite map deletes whichever of the forty-five happen to be crossing a
dropped row, and a burst forty pixels across is usually crossing one — a ring
with a band missing out of its middle, which is what "ya no son redondos" was.
Measured: sixteen of sixty-four sprites gone.

So the burst is mapped ONCE, by the middle of its own bounding box, and every
sprite in it moves by that one offset. Where it appears shifts by up to a
couple of tiles from where the cartridge puts it, which a firework has no
business minding, and it stays round.

## MUSIC_SILENCE does not silence anything

`$08` is an entry in `musicSelectTable` — "no tune chosen". Handing it to
`setMusicOrSoundEffect` resets the engine's state so the NEXT track starts
clean, which is why `LA035` sends it before every tune and why the port does
too. It does NOT stop what is already playing. Measured both on the port and
on the reference interpreter in `tools/nes_cpu.py`: three hundred frames after
a silence, `$4015` is still flipping bits and the title theme is still going.
That is the whole of "la musica se sigue escapando" — the port was asking it
to stop with a word that does not mean stop, and the check that was supposed
to catch it only looked at the REQUEST.

What stops it is **`MUSIC_SUSPEND` ($01)**, the half of `pauseOrUnpause`'s
pair (`main.asm.txt:7204-7211`). It silences every channel and holds them
there until `MUSIC_RESUME` ($02). Two things about it make it the right tool
for leaving a screen as well as for pausing:

* **Sound effects queued after it still play.** So the screen-switch blip is
  heard in full — all twelve frames of it — with the music gone underneath.
* **RESUME is not free when nothing is suspended.** On a cold engine an extra
  RESUME costs the first frame of the tune and the recordings drift from
  there, so the port tracks whether it suspended rather than firing one
  hopefully. `stop_music` / `resume_music` in `gba/main.c` are that pair, and
  pause, the front end and the way back to the title all go through them.
* **AND RESUME GOES LAST.** `updateAudio` takes exactly ONE request off the
  ring per frame (`$CFCC-$CFDB`), so the order they are queued in is the order
  they are heard in, a frame apart. Resuming BEFORE loading the new track —
  which is what the first version of this did — hands the suspended track a
  frame or two of the speaker before the silence meant to replace it arrives:
  the title theme turning up under the tune you are choosing on the level
  screen, and worse when the ring is busy enough to DROP the silence ($CFC3
  drops on full). Loading the new track while the engine is still frozen and
  only then letting it go has no such window.
* **And a tune that is no tune never lets it go at all.** NO MUSIC is
  `musicSelectTable`'s first entry, the silence — nothing follows it to take
  the speaker back, so it is the one menu choice that must leave the engine
  suspended. That is where a resumed title theme used to surface, and
  `make gba-check --leave-title` now listens for two seconds there.

The cartridge never needs any of this: its front end is a one-way chain and
its title theme is *meant* to carry on into the menus. This port can walk
back, so it needs a way to stop.

## The title's frame is two frames, and the screen's shape decides which

The 32x30 title has a band of gold ingots with red and green jewels set into
it, two tiles thick, and inside that a blue braid, another two tiles thick.
Eight tiles of frame on every side is more than a 30x20 screen carries
alongside the picture — VERTICALLY. Horizontally there is room for both,
because the GBA's screen is wide and the picture is not: the port keeps the
BRAID whole on all four sides, and the ingots and jewels in the two side bands
the widescreen leaves over. So the picture is framed the way the cartridge
frames it, and the bands are filled with the cartridge's own gold rather than
with black.

**Both bands are a two-tile pattern** — a jewel (tiles `00 01` / `04 05`) then
an ingot (`08 09` / `11 12`) — so every row and column kept is kept in its
PAIR. Take one row of a jewel and you get half a jewel.

| Kept | What it is |
| --- | --- |
| cols 0-1, 30-31 | the ingot band, down the two side bands |
| cols 2-3, 28-29 | the braid, framing the picture |
| rows 2-3, 26-27 | the braid's top and bottom bands |
| rows 4-5 | TENGEN |
| rows 8-11 | the TETRIS logo, ™ included |
| rows 14-23 | the cathedral, whole and 1:1 |

Twenty rows and thirty columns exactly. What it costs: the ingot band's top
and bottom rows (rows 0-1 and 28-29 — the band survives where the screen is
wide, which is the sides), PRESENTS, THE SOVIET MIND GAME, both copyright
lines (the credit moved to GAME SELECT) and the top two rows of the spire.

### The two dropped columns come one from each side, not two from one

The cartridge's picture is centred on source column 15.5: TENGEN at columns
10-21, the cathedral at 8-23, the spire at 15-16, all with the same middle,
inside a frame whose interior is columns 4-27. Thirty screen columns means
dropping two of the thirty-two, and WHICH two is not free. Taking both off the
left (which this did, dropping 4 and 5) leaves every element where it was but
pulls the frame's right half two columns in behind them, so the whole picture
ends up one column left of its own frame — small, and plainly visible once
looked for. Dropping 4 and 27 instead leaves the interior at 5-26, centred on
15.5 again: measured on the built ROM, TENGEN and the cathedral both come out
with equal margins, and the logo is half a tile right of centre because the
cartridge draws it that way.

Neither dropped column costs anything. Inside the rows this layout keeps, both
are blank in every one; column 27 carries the last letter of the copyright
line, and that row is not kept either.

### The spire is printed over the logo, and it takes TWO tiles

Dropping source rows 12-13 takes the top of the cathedral's one-tile-wide
spire with them — and the tip is the thing the eye misses. It goes back into
the logo, and the spire is three tiles stacked, so both of the dropped ones
have to come or the join shows:

| Source | Tile | What it is | Goes into |
| --- | --- | --- | --- |
| (12,16) | `$7C` | the finial: a thin pole flaring at its base | row 10, col 16 (`$1D`, blank) |
| (13,16) | `$7E` | the gold ball, which JOINS finial to roof | row 11, col 16 (`$73`) |
| (14,16) | `$7F` | the top of the red tent — already kept | — |

Printing only the finial is what "la punta de la catedral tiene un glitch
grafico" was: it floated eight pixels above the roof with black in between.
Row 10 column 16 is a genuine hole in the logo, right between its third and
fourth letters; row 11 column 16 is `$73`, which is three pixels of two
letters' bottom serif and nothing else, so the ball fits there and the spire
comes out whole on three consecutive rows exactly as the cartridge stacks
them. The ball's left neighbour, `$7D` at (13,15), is ONE pixel of its left
edge and does not come — row 11 column 15 is a solid bar of lettering.

Each overlay entry carries the tile its destination must already hold, and
`compose_title` checks it, so a different dump or a changed composition fails
loudly instead of quietly painting over a letter.

### Sprites go through the same rearrangement, in BOTH axes

`kTitleRowMap` was not enough once the composition started dropping columns as
well: the picture keeps its place and the frame's right half moves two columns
left, so there is a `kTitleColMap` too, and both are generated from the same
lists the artwork is cut with. A sprite standing on a row or column the composition
dropped is hidden rather than moved somewhere it does not belong — which is
why the cathedral overlay puts up seventeen of its eighteen sprites now. The
eighteenth belonged to a spire row that is no longer there, so it has nothing
left to overlay; `make gba-check --title` allows for that and would still
catch a map that had gone wrong.

## What the front end answers to, and it is not what a modern pad suggests

`processMenuInput` (main.asm.txt:4614-4702) is short and unambiguous, and two
of its three lines were missing from the port for a long time:

| Where | Mask | What it does |
| --- | --- | --- |
| title, `$9FA4` | `BUTTON_SELECT+BUTTON_START` | either one goes to GAME SELECT |
| menus, `$9FBC`/`$9FED` | `BUTTON_UP+BUTTON_DOWN+BUTTON_SELECT` | moves the cursor |
| menus, `$A011` | `BUTTON_START` | confirms, and nothing else does |

**SELECT moves the cursor the same way DOWN does**, which is not a guess:
`LA048` (:4730) saves the buttons, sets the carry, and adds `$FE` if UP is held
or `0` otherwise — so with the carry it is cursor−1 for UP and cursor+1 for
everything else, SELECT included. That is the whole of "SELECT does not
select".

The cartridge has **no back button** on its menus: they are a one-way chain
with an idle timer (`dec player1FallTimer` at `$9FB1`) that drops back to the
title. So B here, and A as a second confirm, are the PORT'S — the only two
buttons in `gba/main.c` that are not the ROM's, and marked as such.

## The front end's music belongs to the screen

The preview used to follow the player backwards: pick a tune, cancel out of
the cable screen, back out to GAME SELECT, back out to the title, and the tune
was still playing over the cathedral. Every transition remembered to START
music and none remembered to put the old one back.

The fix is to stop treating it as a thing transitions do. The title theme
belongs to the title AND to GAME SELECT — on the cartridge nothing changes the
music between them — and the level screen plays whichever tune the cursor is
on. `front_music()` is called by each screen every frame and does nothing when
what it is asked for is already playing, so backing out restores the theme by
construction rather than by remembering to.

The click before the tune, too: the cartridge queues `SOUND_MENU_SELECT` at
`$9FC4` and only then calls `LA035` at `$A00A`.

## The fireworks are in the audio measurement, and had to be held out of it

`make gba-check --audio` compares the emulated APU against a golden recording
frame by frame, and it started failing at frame 139 the moment the title
screen learned to set off fireworks. Nothing was wrong with either: **every
burst calls `setMusicOrSoundEffect` of its own** (`LACA0`, main.asm.txt:6104-
6109), so the title's APU carries bangs the reference interpreter never made.

Two things came out of chasing it:

* The golden is now recorded the way the port starts a tune — `MUSIC_SILENCE`
  and then the track, `LA035`'s order — because a recording of something the
  ROM never does is not a reference.
* It still has to be the TITLE theme, from a fresh engine. Recording a later
  tune and meeting it mid-session matches nothing at all: the engine carries
  state between tracks (envelope phases, vibrato counters), so only a track
  started from reset can be matched against a reference started the same way.
* The bursts are held off with the cartridge's own lever — `player2FallTimer`
  ($6B), which `LA9DE` counts down and fires a burst at zero (:5740). The
  harness keeps it away from zero for the duration. That is a fixture, never
  anything the ROM knows about, the same shape as planting completed rows for
  the line-clear check. With it, 399 of 399 frames are identical.

## On a cable, the choosing comes after the connecting

Only one of two linked players should be picking the level and the tune, and
neither console knows which one that is until the cable has told them — the
master is whichever end the hardware says it is. So 2 PLAYER now goes straight
to the lobby, and the level screen comes afterwards, on the master only.

The handshake did not have to change to allow it, because it is stop-and-wait:
`tengen_lobby_start_held` parks the master at `TENGEN_LOBBY_HELLO` and
`tengen_lobby_release` lets it run on. While parked the slave keeps echoing
HELLO, every transfer succeeds and `idle` never climbs, so parking costs
nothing — `test_the_lobby_connects_first_and_the_master_chooses_after` holds
for twice the give-up window and then completes anyway.

Two things that are easy to get wrong and were:

* **The level screen has to keep the cable turning.** A lobby that stops
  transferring looks exactly like a lobby whose cable fell out, and the guest
  would give up after ten seconds of the master reading a menu.
* **The jump to the menu has to be one-way.** Testing "connected and not
  ready" sent the master back to the level screen the frame after it chose, and
  it ping-ponged there while the guest went off and started the match alone.
  `hold` is the flag that makes it happen once.

The guest gets one of the cartridge's own cossacks in the middle of the screen,
working through the same pose table the level-up interlude uses. It has nothing
to read — the level and the tune are the master's — and a dancer that keeps
dancing is a better status light than a line of text: while he moves, the cable
is alive.

## Eight columns, and why the panels open at the screen's edge

The piece statistics are NOT seven separable icons. They are one seven-tile
picture, drawn interlocked across the tile boundaries — render `kStatsIcons`
side by side and the tetrominoes plainly straddle their tiles — so they cannot
be squeezed into six columns at any pitch, sprites or not. (The BAR tiles can:
they use six pixels of their eight. The icons are the constraint.)

A closed braid box is two tiles of rope on all four sides, so a ten-column box
leaves six, and six forces the histogram into two ranks. Opening the panel at
the screen's edge — where the screen already ends, and where the cartridge's own
HUD columns run into its screen border — leaves EIGHT, which is the seven-tile
strip in one row with a column to spare, and room above it for NEXT.

So the rope now runs along the top, the bottom and the side facing the board,
with its corners on the board side only, because that is the only side that has
one. The playfield's own frame is untouched: the panel's inner run is exactly
where the cartridge's vertical run always was.

One consequence worth writing down, because it cost a debugging session: the
left panel's content is indented one column off the screen edge, and clearing a
row with the panel's FULL interior width from that indented start runs one
column past the interior and erases the rope itself, a row at a time. `BOX_L_W`
is the interior minus that indent.

### The spare column is worth three pixels, and they need a second background

Seven tiles in eight columns leaves one spare, and there is nowhere honest to
put it: the strip's own ink is inset two pixels on its left and flush on its
right, so on the tile grid it can only ever sit 2/8 or 10/0 — visibly left of
centre either way, which is what "las fichas de las barras de stats estan
lijeramente descentradas a la izquierda" was. It wants to move three pixels.

The art cannot move with it. The icons are one interlocked picture whose seven
tiles carry three different palettes, so a three-pixel redraw fuses two banks
into the tiles either side of a palette change; and the BARS above them are
dynamic, so the same shift would have to fuse two neighbouring bars — nine fill
levels each — into every tile they share. Neither is a table anyone can build.

So the statistics ride their own background. `SCREENBLOCK_STATS` (29) holds
just that block, `REG_BG1HOFS` is 512-3, and BG1 sits at priority 0 over BG0's
1. Two other things ended up needing the same half-pixel and now share it —
see "What else rides the offset layer" below. Everywhere that map is not written it holds tile 0, which is transparent in
every pixel — verified, not assumed — so the rest of the screen is BG0 exactly
as before. Icons and bars move together, the cartridge's art is untouched, and
the strip ends up five pixels from the rope and six from the screen edge, which
is as centred as an odd width gets. Anything that takes the right panel over —
the banner, the dancers' stage, a race — has to clear that map too, not just
BG0's; `clear_stats_layer` is that call and there are four of them.

### What else rides the offset layer

The same trick, twice more, because the same arithmetic keeps coming up: art
that is centred on the TILE grid but whose INK is not centred inside its
tiles.

**The NEXT preview.** The block art has a one-pixel inset on its left, so a
piece `w` tiles wide is `8w-1` pixels of ink and centring that in the panel's
64 wants its first tile at `(65-8w)/16` — a whole number of tiles when `w` is
even, half a tile out when it is odd. The O (2 tiles) and the I (4) land
within half a pixel of centre on the main layer; the T, J, L, S and Z (3
tiles, so five pieces of seven) land three and a half pixels left, which is
what "la siguiente ficha esta alineada a la izquierda" still was after the
columns were squared up. Those five are drawn on the offset layer instead and
come out half a pixel the other side. Measured on the built ROM, all seven now
sit at 31.0 or 32.0 against an ideal of 31.5.

**TENGEN, and only TENGEN.** Nothing on the title is centred where the tile
grid says it is, and the reason took two passes to see. Measured on the built
ROM as centres of MASS — ink weighted by pixel, which is what an eye reads —
against a frame interior running 32..207 and therefore centred on 119.5:

| | unshifted |
| --- | --- |
| cathedral | 124.0 |
| TENGEN | 119.9 |
| TETRIS | 118.2 |

**The cathedral's own art leans four and a half pixels right of the middle of
its own frame**, and the cathedral is the picture. So the words are not read
against the frame at all, they are read against it — which is why TENGEN kept
looking left however carefully the columns were squared up, and why centring
the words on the frame (both at +2) did not settle it. TENGEN goes on the
offset layer at FOUR pixels, putting its mass at 123.9.

**TETRIS stays where the cartridge draws it**, two pixels left of the frame's
centre, and that is a deliberate trade. The spire's finial is printed into the
gap between its third and fourth letters while the rest of the spire is down
in the cathedral, so the letters and the pole have to agree with each other:
unshifted, the gap runs 118..131 and the pole stands at 123. Shift the logo
right and the gap goes with it while the pole does not, which is the pole
leaning against the T. A logo two pixels off centre that its own spire comes
cleanly out of beats a centred one that it does not.

The frame stays off the layer too: four pixels of braid sliding out from under
the ingots is far more visible than four pixels of lettering ever were.

### Drawing the title once per visit, not once per frame

Moving the words to a second layer doubled what `draw_title` writes — 1200 map
entries — and on top of the cartridge's own cathedral and fireworks code
running under it that was enough to miss a vblank every sixty frames, which
`make gba-check --title` caught. Nothing in those tiles changes while the
title is up (everything that moves there is a sprite), so it draws once and
`clear_screen` arms it again. Every path that reaches the title goes through
one.

## Two players over a link cable

The cartridge's 2P is a RACE: two independent 10-wide playfields, and nothing
crosses between them during play (`main.asm.txt:3545-3598` deals one player a
pile of garbage before the first piece and that is the whole of the
interaction). That is what makes a link cable simple — there is no game state
to reconcile, only inputs — and it is why the port does 2P as LOCKSTEP: both
consoles run the same core over the same seed and simulate BOTH players,
each sending only its own buttons.

Where each piece lives, and why:

| Piece | Where | Why there |
| --- | --- | --- |
| The lockstep itself and the handshake that sets up a match | `src/tengen_link.c` | Platform-independent, so `make test` can run two of them against each other and compare byte for byte. Lockstep and stop-and-wait handshakes are exactly the kind of thing that looks right and silently diverges. |
| The cable | `gba/link.c` | GBA serial multiplayer mode, driven by the serial interrupt: the handler queues each transfer and immediately loads the next word, so the send register is never stale and no transfer is ever missed. Nothing in it blocks. |
| The screens | `gba/main.c` | GAME SELECT, the link screen, and a match loop that differs from a solo game in three places only. |

Two things worth knowing before touching any of it:

- **The wire word's top bit is always zero.** A GBA reads `$FFFF` from the
  slot of a console that is not there, so no real word may look like one. The
  frame counter is therefore seven bits, not eight.
- **The SD bit is not "a cable is attached".** A GBA with nothing plugged in
  reads SD set and SI set — indistinguishable from a slave waiting for its
  parent. The only proof of a cable is a transfer that came back with a real
  word in both slots, which is what `link_connected()` reports.

`make gba-check` runs two mGBA cores with a simulated cable between them
(`tools/run_link.py`) and asserts their whole game state stays identical byte
for byte while the two players are fed opposite buttons.

## A note on frame rate

NES NTSC runs at ~60.0988 Hz; GBA runs at ~59.7275 Hz. `tengen_step` is
designed to be called once per rendered frame on either platform with no
compensation for that ~0.6% difference — it's small enough (over a 20-line
game, well under a frame of total drift) that it isn't worth the complexity
before more of the core is verified. Revisit only if playtesting shows it
matters.
