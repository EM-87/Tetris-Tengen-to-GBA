# Working in tengen_gba_port/

Port of Tetris (NES, Tengen) to GBA. Goal: 1:1 gameplay and graphics for the
playfield itself; the surrounding HUD is redesigned to fit the GBA's narrower
screen (see `reference/NOTES.md`'s resolution-mapping section for the exact
numbers — short version: the framed 96×160px playfield fits the GBA's 160px
height exactly with zero scaling, and `make gba-check` asserts that against a
running ROM; only the side panels need reflowing).

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

2. **Never silently upgrade a guess to VERIFIED.** There are currently no
   placeholders left in the core — `reference/NOTES.md`'s PLACEHOLDER section
   reads "Nothing", and keeping it that way is the point. If something new
   can't be traced to the ROM, mark it `TODO(verify)` at its point of use AND
   list it there; when it does get traced, update the comment, the value and
   that table in the same change. Never present an untraced value as
   confirmed — several rules here looked plausible and were wrong.

3. **Core stays platform-independent.** `src/tengen_core.{h,c}` must keep
   compiling with plain `gcc -std=c99`, no GBA headers, no `#ifdef GBA`
   branches. All hardware-specific code (tile/palette upload, REG_KEYINPUT
   reads, sound) belongs in a `gba/` layer that calls into this API — that's
   what keeps `make test` fast and keeps the rules honestly testable. If a
   rule seems to require touching hardware state directly, that's a sign the
   API needs a new return value or callback, not a leak.

4. **Every new rule gets a native test before it gets GBA integration.**
   `tests/test_tengen.c` runs in milliseconds with `make test`. Adding a rule
   (say, the 2P/coop front end from the roadmap below) without a test for
   its documented behavior is how a subtle 6502-carry-flag misreading turns
   into a silent gameplay bug — see how `tengen_try_rotate`
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
   "Nothing". One warning from that work, because it survived a long time
   looking verified: `bonusLinesTable`'s ASCII digit pairs are HUNDREDS and
   TENS, so the first level-up is at 30 lines and not 3. Tracing a table is
   not enough — trace what READS it.
3. ~~Stand up `gba/`~~ — done. It builds with a stock `arm-none-eabi-gcc`
   (no devkitARM required), boots in mGBA, and `make gba-check` verifies it
   renders and plays rather than merely links.
4. ~~HUD and title screen~~ — done, and laid out as `10 | 10 | 10`: the blue
   braid that used to run down the board's edges as two bare strips is now
   two BOXES of the same rope, one either side, with the HUD inside them. The
   braid is two tiles thick and cannot be thinner, so each box frames three
   sides and opens at the screen's edge — which is what buys an eight-column
   interior, and eight is what the seven-tile statistics strip needs to stay
   in one rank. The counters lose their individual frames (the box is the
   frame) and are ruled off with the cartridge's own ledge (item 19 below —
   it was the grey `$76` for a long time and that was the wrong line); the eighteen-row
   TETRIS banner does not fit at all and takes the column instead when L+R
   asks for it. A SECOND BACKGROUND, scrolled a few pixels, carries everything
   that is centred on the tile grid but whose ink is not centred inside its
   tiles: the seven-tile statistics strip, the odd-width NEXT previews, and on
   the title the words TENGEN and TETRIS. See reference/NOTES.md.
5. ~~Palettes~~ — done, and these are the game's real colours rather than
   placeholders: the palette tables ARE in the disassembly even though the
   tile art isn't. Tracing them also caught a fidelity bug worth remembering:
   settled blocks are background tiles sharing one level-wide palette, and
   only the falling piece and preview are sprites with their own colours.
6. ~~Real graphics~~ — done. `tools/extract_assets.py` pulls the tiles,
   palettes, screen layout and dancer poses out of a cartridge dump
   (`make assets ROM=...`); nothing in the port is drawn by hand. The
   generated headers stay out of version control.
7. ~~Real-hardware boot~~ — done, via devkitPro's `gbafix` vendored in
   `tools/gbafix/`, with `tools/check_header.py` verifying the result.
8. ~~The between-levels dancers~~ — done, with the ROM's own poses, cadence
   and CAST: how many walk on is `L8D8B`'s one-plus-one-per-triple-plus-two-
   per-tetris since the last level-up, capped at six. The show is a fixed 32
   seconds and a button does not skip it, it fast-forwards to a wind-down of
   one to three seconds — both the cartridge's, including the eight-bit
   underflow that makes an early press end it sooner than a late one. Their
   individual choreography scripts are still untraced (noted in NOTES.md).
9. ~~Title and menu screens~~ — done, from the cartridge's own title art and
   menu frame. The title's frame is TWO frames and the screen's shape decides
   which: the port keeps the blue BRAID whole on all four sides and fills the
   widescreen's side bands with the outer band of gold ingots and jewels, so
   nothing is drawn by the port and nothing is black. Both bands are a
   two-tile pattern, so every row and column is kept in its pair or you get
   half a jewel; the two columns the 30-wide screen has to drop come ONE FROM
   EACH SIDE, or the picture ends up a column left of its own frame. What the
   picture gives up: PRESENTS, THE SOVIET MIND GAME, both copyright lines (the
   credit moved to GAME SELECT) and the top two rows of the cathedral's
   one-tile-wide spire — its tip is printed back over the logo's blank cell
   and comes out between the T and the Я. See reference/NOTES.md.
10. ~~The line-clear animation~~ — done, and it turned out to be one of the
   game's signatures rather than a pause: a black puff of smoke crosses each
   completed row and leaves SINGLE / DOUBLE / TRIPLE / TETRIS written where
   the blocks were. Timing (one column every other frame) lives in the core
   with a test; the drawing is in `gba/main.c`.
11. ~~Where the port's own cheats live~~ — ONE CHORD, ON THE MENUS. L+R on
   GAME SELECT or on LEVEL SETTINGS uncovers the hidden tunes and the pause
   menu together, and it is a one-way door until the console is switched off.
   NOT in play: there the same chord swaps the HUD and uncovers nothing, and a
   chord that means two things depending on whether the plaque is up is a
   chord nobody can remember — it used to open the pause menu from the plaque,
   which put the one cheat that lets you LEAVE a game behind having already
   started one. The title's prototype skin is its own chord on its own screen
   (L+R there too, since the release) and opens nothing else. `unlock_cheats`
   in gba/main.c is the whole door; `--pausemenu` and `--skin` check both
   halves, including that the game screen does NOT open it.
12. ~~Pause and the long-bar/undo/level-up cheat codes~~ — done, traced in
   full. The three codes share one table and one cursor in the ROM, which is
   where their odd behaviours come from (a broken sequence swallows the press
   that broke it; a completed code re-fires on its last button); all of it is
   reproduced and tested rather than tidied up.
13. ~~Audio~~ — done, and not the way the roadmap assumed. One thing about it
   is worth knowing before touching any of it: **MUSIC_SILENCE ($08) stops
   ONE PRIORITY CLASS, and the title theme is not in it.** The engine keeps
   eleven voice slots; the four in-game tunes hold class 7, which is what `$08`
   frees, while the title theme and the game-over tune hold class 8 — and a
   class-7 tune can never evict a class-8 one. The stop for those is the
   cartridge's own `LD040` called with 8. See reference/NOTES.md, which also
   records what was tried before that and why a mute was not a stop. The port doesn't
   reimplement the sound engine, it RUNS it: `gba/nes6502.c` is a small 6502
   interpreter and `gba/audio_prg.h` is the slice of the cartridge holding
   the engine and its music. That was the only way to get the music, the
   effects and their priority mixing without guessing at a format nobody has
   documented. `make gba-check` compares the emulated APU against a golden
   recording made by the reference interpreter in `tools/nes_cpu.py`, frame
   by frame and byte for byte, and checks the whole thing still fits in a
   GBA frame.
14. ~~2P~~ — done, over a LINK CABLE, and the CHOOSING COMES AFTER THE
   CONNECTING: 2 PLAYER goes straight to the lobby, the master reaches the
   level screen from there and the guest waits with a dancing cossack. The
   handshake parks at its greeting to allow it (`tengen_lobby_start_held` /
   `_release`), which is free because it is stop-and-wait. Lockstep is what
   the cartridge's 2P wants: it is a race on two independent boards with
   nothing crossing between them, so the port runs lockstep (both consoles simulate both players from one
   seed and exchange only buttons). The rules and the handshake are in
   `src/tengen_link.c` and tested on the host; the cable is `gba/link.c`,
   interrupt-driven so neither console can miss a transfer or send a stale
   word. `make gba-check` runs two mGBA cores with a cable between them and
   asserts their game state matches byte for byte. See reference/NOTES.md.
15. ~~The title screen's cathedral overlay and fireworks~~ — done, and by the
   same method as the audio: they are RUN, not reimplemented. Both are ROM
   subroutines that fill `oamStaging` and touch nothing else, so they execute
   on the sound engine's own 6502 interpreter — which they have to, because
   the bursts call `setMusicOrSoundEffect`. See reference/NOTES.md.
16. ~~The prototype title skins~~ — done, from prototype dumps: L+R on the
   title cycles the release's screen and one screen per dump given, each with
   its own art, tiles and palettes. Optional and unlimited:
   `make assets ROM=... PROTO="a.nes b.nes c.nes"`; with none of them the port
   builds with `SCREEN_PROTO_AVAILABLE 0` and L+R does nothing. **THEY ARE
   CAPTURED, NOT READ**: `boot_prototype` boots each dump on `nes_cpu` with a
   PPU behind it and an NMI every 30000 instructions, then reads the
   nametable, the attributes and the palette back — so there is no address to
   guess and no storage format to recognise, and a dump this file has never
   seen still gives up its screen. The first pass instead read one screen out
   of a fixed address found by rendering an upload table by hand; it worked
   for one dump, hid the other two (see item 29), and even for its own dump it
   missed the two copyright lines, which are written by a separate text
   routine and are not in the blob. What is still per-dump is the COMPOSITION
   — which ten rows go, to fit 32x30 into 30x20 — and that is keyed to the
   MD5 of the captured screen, with a blank-row fallback for an unknown one.
   All three skins fit one 256-tile window (512-767): they take turns in it,
   re-uploaded on each swap, so the count is bounded by cartridge space rather
   than by video memory.
17. ~~Korobeiniki, Katiuska and MUSIC MIX~~ — done. These two are THE EXCEPTION
   to ground rule 1: neither is on this cartridge (Tengen's four are Loginska,
   Bradinsky, Karinka and Troika), so `gba/handtunes.c` is the one file here
   entered by hand rather than extracted. It runs its own sequencer over the
   GBA's PSG, never through the ROM's engine, and shares the chip with it so
   the effects stay the cartridge's. **KALINKA NEEDED NOTHING**: Karinka is
   Tengen's transliteration of it and was always there. **KATIUSKA IS NOT
   PUBLIC DOMAIN** — Korobeiniki (1860s) and Kalinka (Larionov, 1860) are, but
   Katyusha is Blanter, 1938, and he died in 1990, so in Russia and the EU the
   melody is in copyright until 2061. It is in because it was asked for, and
   `handtunes.h` says so where anyone touching the file will read it. Its
   notes are not from memory either: two independent public transcriptions
   (thesession.org 14315 and John Chambers' Musica Viva posting) agree on it,
   and the plainer reading is the one entered. All hidden behind L+R on the
   selection screen, which uncovers a third entry with them: MUSIC MIX,
   playing the six in turn and turning over at every level-up. NOT at the end
   of a tune — only one of the four has a loop these measurements can find, so
   a "song length" for the rest would be invented. See reference/NOTES.md.
18. ~~The starting handicap~~ — done, `initHandicapGarbage` traced in full:
   three rows a step, each cell filled seven times in eight, and a guaranteed
   hole punched into the middle eight columns of any row that came out seven
   or more full. Two values on the HANDICAP screen (L for player 1, R for
   player 2), a lobby stage of their own on the cable, and the garbage drawn
   from a per-game RNG so one seed buries both consoles identically.
19. ~~The settings screen~~ — done, and it has been all three shapes. The
   cartridge walks FOUR menu gameStates, one setting each, with START between
   them, and its level list is a vertical column of ten with a cursor. Both
   are answers to "a television across a room"; a GBA's problem is 240x160 of
   room, so the port keeps ONE page with three fields and a cursor
   (up/down/select move it, left/right set). What is kept from the ROM is the
   rules: the choice counts (`computerMoveSelectTable` — misnamed, only
   `$A0EB` on is the AI's), SELECT as a cursor button, and the two menu arrows,
   which are PRINTABLE because the tile set is ASCII-indexed. It has no
   parentheses, though — `$28`/`$29` are border art — so the handicap's note
   is separated by palette instead. Defaults: NO MUSIC and HUD Banner.
20. ~~The HUD is the COOP screen's panel now~~ — and this is the shape to
   know before moving anything in it. The port's left box used to be a closed
   rectangle of rope with the cartridge's grey header rule between the
   counters; it is the coop screen's panel instead, which is a better object
   and is the cartridge's own: open at the bottom, ruled across with the blue
   DANCERS' LEDGE ($9D) every three rows, which makes one tall compartment at
   the top and four short ones under it. Five compartments, and the HUD has
   exactly five things to say — NEXT, SCORE, LINES, LEVEL, HIGH — so NEXT has
   one home in both HUDs instead of moving between the boxes. The right box
   keeps one shelf on the same row, so the two sides rule at the same height,
   and in HUD Stats the cossack stands on it with the histogram below.
   BOTH PANELS ARE INVERTED Ls, mirrors of each other: rope along the top and
   down the side facing the board, open at the bottom and at the screen's own
   edge. That is the coop screen's shape, and opening the right one is what
   finally puts the eighteen-row TETRIS banner inside a frame — a closed box
   left sixteen interior rows, which is why the banner used to take the whole
   column and the rope with it. The only difference between the two HUDs is
   that shelf: HUD Stats has one, HUD Banner has none.
   THREE THINGS THAT LOOK LIKE CONSTANTS AND ARE NOT: the counters' layer is
   two pixels **UP** (it was two down while every counter hung under a rule;
   between shelves that put the value's last row of pixels ON the shelf); NEXT
   alone is drawn on the MAIN layer, because the piece picks its own layer by
   width and a preview two pixels off its own word is what "Next y la pieza se
   solapan" was; and the NEXT block is FOUR rows, not three — a blank one
   between the word and the piece, because the block art fills its tiles to
   the top edge and a piece drawn straight under the label touches it.
   `make gba-check --panel` measures all of it off the framebuffer: four blue
   shelves found by colour, equal air under each, NEXT centred, and both ropes
   reaching the last scanline.
21. ~~FOUR backgrounds, each for a scroll the others cannot share~~ — done,
   and this is the shape to keep in mind before adding anything to the HUD: a
   scroll is one number per background, so anything whose ink is not centred
   in its tiles needs a layer of its own or it drags its neighbours with it.
   BG0 is the playfield and the cartridge's art at the grid; BG1 is three
   pixels across (odd-width previews, menu text, the title's words) and the
   panel's two down; BG2 is the counters, two down; BG3 is the piece
   histogram, three across and two UP so its icons clear the braid. SCORE's
   missing headroom and the histogram touching the frame were the same bug
   twice. `make gba-check --panel` measures both gaps off the framebuffer.
22. ~~Coop~~ — and the thing to know before touching anything on a shared
   board: **THE TWO FALLING PIECES ARE SOLID TO EACH OTHER, and the ordinary
   collision check cannot see that.** The playfield buffer holds settled
   blocks only, so the partner's falling piece is invisible to it and the two
   walked through each other like ghosts. The cartridge has a whole routine
   for it, `checkCoopCollision`, which shifts one piece's 4x4 bitmap into the
   other's frame and ANDs them; it is called from every shift, every rotation
   AND the gravity step, where a partner underneath makes the piece HOVER
   rather than lock, and a refused shift runs a fall-timer stagger so two
   players pressed together untangle instead of deadlocking. All of it is
   traced in reference/NOTES.md and tested both natively and off the running
   ROM. The layout question answered itself: the cartridge
   ships a coop SCREEN (screen 5) already laid out symmetrically, so the port
   reflows that one instead of the 1P screen and the only change is the two
   columns every screen gives up, taken one from each end. Twelve-wide field
   dead centre, seven columns of panel either side, the dancers' ledges
   already drawn, and the ROM's own eight coop dancer positions to stand on
   them. It goes over the cable like 2P, with the choice riding bit 8 of the
   lobby's CONFIG word. See reference/NOTES.md.
23. ~~The COMPUTER player~~ — done, and with it all five of the cartridge's
   GAME SELECT entries. `computerMove` is `src/tengen_ai.c`, transcribed byte
   for byte including the arithmetic that wraps; only the 28 bonus bytes are
   copied, since the profiles derive from the core's own bitmaps and a test
   checks that they do. `playModeTable` is what says which board each mode
   uses: VERSUS is a race like 2P, WITH COMPUTER is coop's shared twelve-wide
   board. Neither needs a cable. See reference/NOTES.md.
24. ~~The attract demo~~ — done, and it is the same computer playing the same
   game: `demoStart` is playMode 0 with the music suspended, reached off the
   title's own clock at frameCounterHigh 5 / low $20, with the computer on
   PLAYER 1 (the VS and WITH paths `inx` first, the demo does not). A press is
   the way out rather than a move. What ends it is the port's: the cartridge
   goes to a high-score table this port does not have, so the game over holds
   three seconds and the title comes back.
25. ~~The HIGH SCORES table~~ — done, and SAVED: fifteen entries with their
   lines and three initials, inserted the ROM's way (bottom up, equal scores
   go under), typed into with Left/Right and A, and kept in the GBA's
   battery-backed SRAM under the cartridge's own 'LOGG' magic — which on the
   NES only carried it across a RESET. A cold table is @resetHighScores', 17000
   down to 3000 in thousands, which is why HIGH SCORE opens at 017000.
26. ~~The level's BONUS tally~~ — done. displayStatsP1 paints the playfield
   over with it while the cossacks dance, and L8EA2 counts it up one clear at
   a time ADDING TO THE SCORE: singles x100, doubles x400, triples x900,
   tetris x2500, all printed in the ROM's own strings.
27. ~~The drop-point sprites~~ — done. Three digits beside the piece the moment
   it lands, for $3C frames, at the HEIGHT it landed — which is the whole
   point, because this game pays by how high a piece comes to rest.
28. ~~The pause menu~~ — the port's own, behind L+R on the plaque: the tune
   (silence and the MIX included) and a way out that asks first. Built from
   the game-over plaque's nine-patch frame so it reads as part of the game.
   Two things it taught, both of them traps for anything added near it.
   **A PAUSE THAT IS TORN DOWN HAS TO BE RESUMED.** `MUSIC_SUSPEND` is a gag
   on the whole engine, not a stop for the music — under it only the two
   class-62 effects are still heard and every tune and every class-29 effect
   is silent — so EXIT, which pulls the match out from under the plaque
   without going back through `pauseOrUnpause`, left the machine mute for the
   rest of the session: no piece landing, no game-over jingle, no menu blip,
   no title theme. It sends the RESUME itself now, FIRST in the ring so its
   own blip is not swallowed, and `make gba-check --quit-audio` walks that
   road and listens at every screen. See reference/NOTES.md.
   **AND FOURTEEN COLUMNS WIDE, BECAUSE THIRTEEN CANNOT BE CENTRED.** The box
   lands on the board, whose middle is x=120 — the screen's own — and a box of
   odd width on an even grid cannot be put there: thirteen columns sits four
   and a half pixels left, which against a playfield you are looking straight
   at is not a subtlety. What that costs is the PARITY of every line inside
   it, and with it the vertical nudge, so every gap in the box is a whole
   blank row.
   **THE CURSOR IS AN ARROW AND START ALWAYS LEAVES.** Picking the line out
   by palette read as a colour scheme rather than as a cursor, so it is the
   cartridge's own `$3E` two columns left of the line, where the settings
   screen puts its own; SELECT moves it as it does there, A takes a choice, B
   backs out, and START resumes from every line including EXIT and from
   inside the question. **AND THE BOX HAS TO BE TORN DOWN ON THREE MAPS**:
   `draw_static_screen` puts the main background back, but the lines on the
   offset and counter layers are in the middle of the board where it never
   writes, so the window vanished and the words stayed — see
   `clear_pmenu_layers`, called off the same `g_repaint`.
   **AND CENTRING IN THIS BOX NEEDS THREE OF THE FOUR BACKGROUNDS.** Its
   interior is eleven columns, so an even-length word misses the middle by
   half a tile and goes on the offset layer (three across) to miss it by one
   pixel instead; the heading rides the counters' layer, two pixels down,
   which is the only sub-tile nudge downwards this port has. A line can have
   one or the other, never both — see `draw_pmenu_line`. The question mark in
   EXIT / SURE? is the one glyph in the port that is not the cartridge's: the
   tile set is ASCII-indexed but $3F is a LEFT ARROW, so `extract_assets.py`
   draws one into a slot the cartridge left empty and refuses to if the dump
   has art there.
29. **WHAT THE THREE DUMPS ACTUALLY HAVE** — measured by booting each one,
   which is the correction to what stood here before. This entry used to say
   "only `proto_b` carries the title screen this port can read" and that the
   other two hid theirs in a format nothing recognised. Wrong, and wrong by
   method: it had searched the ROMs for a flat nametable instead of asking the
   ROMs to draw. All three have complete, distinct title screens —
   `proto_a` is "TETRIS" over a Moscow skyline with LICENSED BY NINTENDO OF
   AMERICA INC., from before the lawsuit; `proto_b` is "TENGEN PRESENTS /
   TETRIS" over St Basil's in the green fret; `proto_c` is the same screen
   with the logo replaced by "THE SOVIET MIND GAME". All three draw out of
   CHR bank 1, and all three now ship. The charblock does not cap it either:
   they share one window and are re-uploaded on the swap.
31. ~~AND THE SKIN DOES NOT STOP AT THE TITLE~~ — done, and it cost no new
   draw path at all. Each prototype plays on a screen of its own: a GREEN
   FRET where the release has its blue braid, and blocks that are flat or
   striped squares rather than shaded joined ones. Both come across as
   TWENTY-TWO TILE SLOTS re-uploaded in place (704 bytes, well inside a
   vblank) plus one palette bank, so the same `set_map_tile` calls draw the
   same tile ids and different art comes out. The frame is lifted off each
   dump's play screen BY POSITION — the three number their tiles quite
   differently, $93-$9A in two of them and $08-$17 in the third, but all
   three put the same parts in the same places — and the two elbows the
   port's panels need come from where a prototype hangs its banner box off
   the header rule, exactly as the release's do.
   **THE ONE THING THAT IS NOT COSMETIC IS THE CELL ENCODING**, and it is
   measured rather than assumed: the release has fourteen joined-block
   graphics at $01-$0E and `kTileIds` picks one per cell; the prototypes have
   SEVEN, one per tetromino, and write all four of a piece's cells with it.
   Proto_b was left playing itself and its playfield watched — every piece
   that settled wrote four cells of ONE value, never four of four. So
   `piece_id_cells` on TengenGame makes `lock_piece` store the piece's id, and
   `piece_cell_tile` in gba/main.c does the same for the falling piece and the
   preview. Occupancy is `cell != 0` everywhere and TT_WALL is 15 either way,
   so nothing downstream notices. **NOT OVER THE CABLE**: a linked match is
   two consoles comparing state byte for byte, and one of them in a
   prototype's clothes would diverge in the playfield itself, so
   `skin_begin_match` refuses there. The MENUS ask for the release back too —
   their frame comes out of the same slots in the cartridge's blue.
   **AND THE SKIN CARRIES RULES, not only paint** (`proto_rules`). The three
   differences that are gameplay rather than art, documented per build and
   agreeing across A, B and C: the level goes up every TEN lines instead of on
   the release's 30/60/90/120-then-every-50 curve; there is NO WALL KICK, which
   is what "blocks often cannot be turned when they are pressed against the
   wall" describes (the release kicks one column left, these do not kick at
   all); and a completed row goes the frame it completes, with no sweep across
   it and no SINGLE / DOUBLE / TRIPLE / TETRIS written where it was. Two more
   live in the front end: no cossacks and no BONUS tally at a level-up — those
   builds carry straight on — and PAUSE does not silence the music.
   What is deliberately NOT taken is the SHAPE of their front end: only
   1 PLAYER and 2 PLAYER, four difficulty steps instead of ten levels, no
   handicap and no music menu. Taking those away on a chord rung at the title
   would remove things this port has and a player chose, so they wait for a
   decision. Likewise their GAME OVER plaque's blue border, which is a palette
   this port has not captured.
30. ~~Tetris Tengen XE~~ — done, behind the same L+R chord: levels 0-19 on the
   settings screen, the mod's longer fall-timer and mask tables, and the cap
   at 19 instead of 17. **AND THAT IS ALL THE MOD IS.** Its ten IPS records
   were decoded one by one (reference/NOTES.md) and six of them are the
   machinery for putting a two-digit level on a menu that is a fixed column of
   ten lines — machinery this port does not need, because its level is a
   number you wind. Levels 0-17 are BYTE-IDENTICAL in both of its replacement
   fall-timer tables, so the "drop speed adjustment" it is described as having
   is the two new levels and nothing else; it patches nothing near the OAM or
   sprite code, so there is no glitch fix in it; and it does not touch the
   soft drop at all. **Two of its own mistakes ARE MENDED, and this is the one
   place in the project where something deliberately does not behave as its
   source does.** They are not the cartridge's quirks — they are a patch that
   stops one address short, and each one breaks the only thing the mod exists
   to do: level 19's mask byte falls outside the patch, so 19 ran on level
   18's entry with the mod's own last table byte dead; and the level-up CODE
   stopped at 17, because the mod raises checkLevelUp's clamp and not the
   cheat's own at $B4F7. Both fixes are `xe`-only — with the flag off the
   cartridge is untouched — and both are pinned by tests, with what the mod
   actually does written down beside them.

## What the port knowingly does NOT show

Kept as a list rather than as scattered comments, because the last sweep found
six credit lines that had gone missing without anyone noticing. Everything
here is a decision; anything not on it that the cartridge shows and the port
does not is a bug.

- **"STATS", the heading over the 1P histogram** (row 6 of
  `gameModeNametable1P`). The port relocated the cartridge's header strip into
  the two boxes, and the right box's interior is eight columns by ten rows —
  all of which the bars grow into. A heading would cost a row off every bar.
  The seven piece icons under them say what it is.
- **"HIGH SCORE" reads "HIGH"** in the 1P panel, for the same reason: the cell
  is eight tiles wide and the phrase is ten. The number under it is the number
  the cartridge puts there.
- **Each dancer's choreography script.** Traced as far as the driver (`LB015`)
  and no further; the port's dancers walk the pose table from staggered starts
  instead of following their own programs. See reference/NOTES.md.
- **A skinned board over the LINK CABLE.** A skin changes what a settled cell
  holds (roadmap 31), and a linked match is two consoles comparing state byte
  for byte, so one of them wearing a prototype's clothes would be a real
  divergence. The title's skin still cycles; a 2P board stays the release's.
- **The line counter's digit clamp** at 10000 (`main.asm.txt:3129-3133`). The
  score's wrap at 999999 IS reproduced; reaching ten thousand lines in one
  game is not a scenario worth carrying a bug for.
- **One frame at the start of a COOPERATIVE match.** The cartridge's coop deal
  frame decrements both fall timers and the port's does not — measured, and
  written up with the PC that does it in reference/NOTES.md. One frame, once,
  in one mode; every frame after it matches.

## Build

- `make test` — native core tests, gcc only. The everyday loop.
- `make gba` — cross-compiles `build/tengen.gba`.
- `make assets ROM=/path/to/tetris.nes [PROTO="a.nes b.nes c.nes"]` —
  regenerates the graphics headers from a cartridge dump. Required once before
  `make gba` on a fresh clone. `PROTO` is optional and adds the title-skin
  easter egg; without it the port builds and runs the same, minus that.
- `make assets-check` — verifies the asset conversion without needing a ROM.
- `make trace ROM=/path/to/tetris.nes [FRAMES=3000]` — **THE PORT AGAINST THE
  CARTRIDGE, NOT AGAINST THE DISASSEMBLY.** Boots an original dump in
  `tools/nes_console.py`, walks its menus into a 1 PLAYER game, reads its
  `savedRNGSeed`, seeds `src/tengen_core.c` with the same number, feeds both
  the same button script and diffs a line a frame — piece, position, fall
  timer, counters and all two hundred playable cells. It is what found the
  port's last three timing bugs, none of which a reading of the disassembly
  had caught. `--coop` does the same for COOPERATIVE, both pads and the
  twelve-wide shared board. Both run to the end of a match and match it frame
  for frame. Slow (an interpreter: about a minute a thousand frames), so it is
  NOT in `gba-check`; run it after touching `tengen_step`.
- `make gba-check` — boots the ROM headlessly in mGBA and asserts it draws
  the field where the resolution mapping says it should, that a piece
  actually falls, that the line-clear sweep crosses the row and writes the
  right word, that Start pauses and the cheat codes respond, that the blue
  braid keeps its weave when L+R swaps the right-hand box, that the title's
  theme actually stops when you leave it, and that the
  emulated sound engine matches a golden recording of the reference
  interpreter frame for frame while still leaving the game running at full
  speed. The line-clear check plants completed rows straight into the game's
  playfield through the emulator, so it doesn't need a bot that can stack;
  that's a fixture in the harness, never anything the ROM knows about.

Run `make test` and `make gba-check` before considering a change done. The
first catches rule regressions; the second catches the ones that only appear
once the code runs on ARM (alignment, the `-nostartfiles` build, VRAM
layout), which the host tests structurally cannot.
