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
| **Attribute tables are NOT beside their nametables** | copy loop in `sendNametableToPPU`, `main.asm.txt:6941-6963`; screen addresses at `nametableAddressTable`, `:6990-6998` | A screen is 960 tile ids plus a 64-byte attribute table saying which of four palettes each 2x2 block uses, and `sendNametableToPPU` copies **1024** bytes to `$2000`. But the screens are stored **960 bytes apart**, so slicing "nametable then attributes" out of the ROM hands you the next screen's first two rows and calls them palettes. It looks almost right and is wrong everywhere: this port had the playfield's braided frame changing colour down its length and a monochrome title before the screens were read by RUNNING that routine on the 6502 interpreter instead (`read_screen` in `tools/extract_assets.py`). The truth: the 1P frame is uniformly bank 2 top to bottom, the playfield is bank 0 (which is what `setPlayfieldPaletteFromLevel` recolours), the banner is bank 3, and the whole header strip is bank 3. |
| **Which palette set each screen uses** | `updatePalette` at `main.asm.txt:5268-5287`, tables at `:5292-5321`; callers at `:2044`, `:3308-3310`, `:4492-4494`, `:4547-4549` | `updatePalette(n)` writes 16 bytes — four palettes — from `bgPalette0 + n*16` to the background palettes for n<3 and the sprite palettes for n>=3. Title: `bgPalette0` + `spritePalette1`. Menu: `bgPalette1` + `spritePalette0`. Game: `bgPalette2` + `spritePalette0`. The level-up interlude switches sprites to `spritePalette2`, and the dancers' own attribute bytes (`$8E78`) pick among its four — which is why the six are three different colours, not one. The title screen in particular uses **all four** of its palettes; assuming one covered it is what made this port's title monochrome. |
| **Screen layout** | `gameModeNametable1P` at `main.asm.txt:C028` onward | The 1P screen is 32x30 tiles and is built from TWO identical framed board areas side by side: braid at columns 0-1, ten playable columns at 2-11, braid at 12-13, the 4-column TETRIS banner at 14-17, then the same again — braid at 18-19, ten columns at 20-29, braid at 30-31. The playfield rows are 8-27, exactly 20. 2P puts a player in each area; 1P draws its score/stats panel over the second one, which is why that half is blank in the nametable. The header strip in rows 0-7 holds the SCORE / LINES / LEVEL / NEXT labels; those are multi-tile graphics, while "HIGH SCORE" and "STATS" are plain ASCII, because the tileset's letters and digits sit at their ASCII codes. |
| **The playfield's tiles come from the nibble itself** | `L8544` at `main.asm.txt:829-842` | The routine that fills the screen buffer stores the playfield nibble **directly** as the nametable tile id — no lookup, no offset. So cell value 1-14 is a block tile and the wall's `$F` is tile `$0F`, which is why walls need no special case in the renderer. `$0F` is a block graphic with transparent corners, not a solid bar; a renderer that assumes a solid wall column will look wrong. |
| **The dancers** | poses at `$C8BC-$C9FF`; positions at `$8E5C/$8E6A/$8E78`; how many, at `main.asm.txt:2085-2108`; stage blit `levelUpAnimationColsRows1` at `$B7FF` with tiles at `LC82C` ($C82C); driver `LB015` at `:6392-6499` | Sprites, four 8x8 tiles in a 2x2 each. **A 1P or 2P game shows six**, stacked in ONE column at x `$61` with y `$D0,$B8,$A0,$88,$70,$58` — 24 pixels apart; coop instead uses entries 6-13, in pairs down the two sides. How many actually appear grows with the player's bonus counters and is capped at 6 (`:2085-2108`, which also picks which range of the tables the mode uses). The level-up blit is not just clearing the TETRIS banner to make room: it **draws their stage** into it — 4 columns x 18 rows at nametable (14,10), five ledges of tile `$9D` one every three rows, i.e. 24 pixels apart, exactly under the six dancers' feet. They start just left of the banner and walk right onto it, one pixel every four frames, while the pose advances every eight. **The driver is now traced too** (`LB015`): each dancer holds a pointer into a little program (`$019A/$01A2`), advanced by one 2-byte entry every 8 frames, and what an entry MEANS is decided by comparing its value against two addresses — `>= $C8BC` is a pose (four tile ids), `>= $B14D` but below that is a jump to another program, and below `$B14D` it is a random branch: `shuffleRngSeed5x` picks one of sixteen pointers from the table the entry names. A dancer only walks while its program lies below `$B181`. **Still not wired up in the port:** the program DATA itself, so the port's dancers walk the pose table from staggered starts instead of following their own scripts. Everything else about them — art, poses, stage, positions, count, cadence — is the ROM's. |
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

A button does not cut it short, it fast-forwards: L9035 computes
`$7C - timer - 5`, clamps it to at least `$F5`, and you still get the three
seconds of wind-down.

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
