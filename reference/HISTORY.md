# How the port got its shape

The working guide is `../CLAUDE.md`. This is the record behind it: each part
of the port, what it had to get right, what was tried first and why that
was wrong. The traps listed in CLAUDE.md come from here, and so do the
reasons for the decisions it lists. Read the entry before changing the thing
it describes; most of them were paid for.

`NOTES.md`, next to this file, is the index into the cartridge's
disassembly — what the ROM does, with line numbers. This file is about the
port.

## The roadmap, item by item

Numbered as it was when it lived in CLAUDE.md; code comments and the
entries below cite these numbers ("roadmap 31", "item 19").

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
   underflow that makes an early press end it sooner than a late one. And
   each one's OWN CHOREOGRAPHY, which is the last thing here that used to be
   an approximation: the driver `LB015` is transcribed into `gba/hud.c` and
   reads the cartridge's own programs, branch tables and poses out of the PRG
   slice the sound engine already carries, rolling the cartridge's own
   `shuffleRngSeed5x` for the branches. Nothing is extracted for it.
   **THE DICE ARE SHARED, so a dancer who never comes on still costs.**
   `L8E46` zeroes the program pointer of every slot the cast does not fill
   and `LB019` skips a slot whose pointer is zero — a troupe of six rolls six
   times a frame, not eight. Giving the two spare slots a programme anyway
   drew nothing extra on screen and put every other dancer on a different
   branch; `make dance-check` is what found it, and it runs the 6502 driver
   beside the port's on one seed for both casts, six and coop's eight.
   **AND IN A RACE THEY COME ON DOWN THE TWO PANELS**, which is the port's
   arrangement of the cartridge's own show rather than the cartridge's. What
   it does in 2P and VERSUS is run the same interlude it runs in 1P —
   `showLevelBonus` sets gameState to LEVELUP, which stops both boards, and
   `L8D6B` blits `levelUpAnimationColsRows1`, four columns by eighteen rows at
   nametable (14,10), into the strip BETWEEN the two playfields. There is no
   "the player who levelled up gets them on their side": one stage, in the
   middle, and `L8D8B` counts BOTH players' triples and tetrises into one
   cast. Only the coop screen skips the blit (`bit playMode / bmi`), because
   its ledges are already drawn. The port shows ONE board of the two, so there
   is no strip between them to stand a stage in — the middle of its screen is
   the playfield. What it has is two panels with four ledges each, so in HUD
   VERSUS the troupe comes on there, the counters go for the length of the
   show, and the heights are the cartridge's own (`kDancerCoopY`; the ledge
   rows are the same in both screens). The cast is still both players' work,
   still capped at six, and the programmes are still the SOLO ones —
   positions 0-5 — because that is what a race dances on the cartridge. Only
   the floor is the port's. See `draw_race_dancers`.
   **AND IN HUD STATS THEY COME ON THROUGH THE LEFT BOX, in every mode.** The
   right box is the histogram, which is what that HUD is for, so the show
   takes the other one: its counters go for the length of the show and the
   cartridge's solo column of six walks on from the screen's open edge. It
   fits exactly — the column is six dancers 24 pixels apart and the panel is
   ruled every three rows, so the bottom one stands on the screen's edge, the
   next four on the four ledges, and the sixth on a ledge drawn for the show
   in the tall compartment where NEXT was. Solo programmes and the solo cap of
   six even on coop's board, whose pairs run down BOTH panels — capped when
   the show STARTS, not only when it is drawn, because every dancer handed a
   programme rolls the shared dice. The resident over the histogram dances it
   in place and is not one of the cast. It used to be him alone, which threw
   away the one thing the cast says: how well the level went. See
   `draw_stats_show` and `make gba-check --statsshow`.
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
   with a test; the drawing is in `gba/hud.c`.
11. ~~Where the port's own cheats live~~ — ONE CHORD, ON THE MENUS. L+R on
   GAME SELECT or on LEVEL SETTINGS uncovers the hidden tunes and the pause
   menu together, and it is a one-way door until the console is switched off.
   NOT in play: there the same chord swaps the HUD and uncovers nothing, and a
   chord that means two things depending on whether the plaque is up is a
   chord nobody can remember — it used to open the pause menu from the plaque,
   which put the one cheat that lets you LEAVE a game behind having already
   started one. The title's prototype skin is its own chord on its own screen
   (L+R there too, since the release) and opens nothing else. `unlock_cheats`
   in gba/frontend.c is the whole door; `--pausemenu` and `--skin` check both
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
   GBA frame. **THE ENGINE HAS TO BE RESET FIRST**, with track id `$00`,
   which the cartridge queues on its first frame after power-on and which
   builds the engine's free list of voice slots (`NES_AUDIO_RESET`): the
   golden could not see that it was missing, because the title theme sounds
   the same either way, but LOGINSKA in play lost the repeat of its first
   strain and ran ahead of the cartridge — heard side by side, then measured
   by `make tune-check`, which plays the cartridge itself into a game with
   each tune and lines the port up against it note by note. The interpreter
   fetches its code from a 64KB view of the address space in external WRAM
   (`Nes6502Bus.code`, one load per byte, no range check), which is what
   keeps the title screen inside its vblank with the engine doing the extra
   work the reset gives it; a fast path inlined at every fetch site instead
   grew the internal-WRAM code past the stacks, so mind `.iwram` in
   `arm-none-eabi-objdump -h` when touching `gba/nes6502.c`.
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
   asserts their game state matches byte for byte.
   **AND A DEAD BOARD GETS UP AGAIN ON A+B**, which is the cartridge's
   (`handleGameOver`, main.asm.txt:472-490) and what makes a race between two
   people playable: the loser starts again on the spot — score and lines to
   zero, the level back to the one THEY chose, the pieces the match opened
   with, the handicap dealt again — while the winner plays on. Two things
   about it. The restart runs INSIDE THE CORE off the held buttons, because
   those buttons crossed the cable and the two consoles have to do it on the
   same frame; done in the front end off the local keypad it would part the
   two simulations for good. And **A and B stop being a way out while the
   other board is still going**: there the way out is START, because a button
   that both leaves the race and starts it again is a button that does
   neither. See reference/NOTES.md.
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
   **AND GAME SELECT'S LIST IS FLUSH LEFT, which is the cartridge's and was
   not the port's.** All five entries are written at nametable column `$0C`
   whatever their length — the strings sit in the ROM's own upload stream with
   their addresses in front of them — and the arrow is two columns before them
   at `$0A` (`gameSelectArrowPpuAddrs`, `$A0AB`). Centring each entry instead
   gave the column an edge that wandered four columns as the cursor moved down
   it. The block stays where it was on the port's narrower screen: the flush
   edge is where VERSUS COMPUTER, the longest of the five, already started. A
   left-aligned line also has no middle to hit, so it rides the MAIN layer and
   never the offset one — see draw_text_left.
   **AND THE CREDITS TURN OVER EVERY FOUR SECONDS, not every one and two
   thirds.** The cartridge prints all six of its credit lines at once down a
   taller screen and never animates them, so there is no cadence to copy; the
   rotation is the port's answer to a menu box twenty-six columns wide, and a
   hundred frames was a line arriving faster than it could be read and leaving
   before it had been — unreadable and impossible to ignore at the same time.
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
   **AND THERE ARE FOUR HUDS, TWO PER MODE.** Each game mode offers the one
   it opens on and one alternative, walked with SELECT and remembered per
   mode — a choice that did not survive the next game meant picking the same
   HUD again every time, and one remembered ACROSS modes meant VERSUS handing
   WITH COMPUTER a HUD it had never been asked for, because STATS is in both
   sets. One slot each (`g_hud_choice`) settles both.

   | mode | opens on | and | 
   | --- | --- | --- |
   | 1 PLAYER | BANNER | STATS |
   | VERSUS COMPUTER | VERSUS | STATS, whose left box keeps RIVAL |
   | WITH COMPUTER | COOP | STATS, whose two cells are the board's totals once the chord is rung and your own score and lines until it is |
   | 2 PLAYER | VERSUS | — |
   | COOPERATIVE | COOP | — |

   The two CABLE modes offer one apiece on purpose: the other player is a
   person who chose to play with you, so there is nothing to hide from them
   and no reason to take their panel away. HUD VERSUS is the rival's own
   panel in the right box — their score, lines and LEVEL, laid out like
   coop's — and its fourth cell is EMPTY: it carried RIVAL for a while, which
   is a caption on a caption, since the panel is the rival's. The left box
   gives its own RIVAL cell back there and returns to the 1P panel's HIGH.
   The ledges are drawn by the panel itself rather than with the static
   screen, because it clears the column under the first shelf every frame and
   would take them with it.
   **AND THE TOP COMPARTMENT IS A COSSACK, NOT THE RIVAL'S NEXT.** It was
   their preview for one build, and in VERSUS COMPUTER that is a piece
   belonging to a board you never see: the one HUD that takes the rival's
   stack away was handing out the rival's next piece. The compartment is the
   coop panel's NEXT cell and cannot simply go empty without leaving the
   panel headless, so what stands in it is a SECOND IDLE COSSACK, the rival's
   own — the same sprite the stats panel has, on the same ledge height, and
   he celebrates when THEIR board clears rather than when yours does
   (`cossack_slot` picks the board by which panel the HUD gives the rival,
   and `cossack_watch` is called once per board per step). Two cossacks never
   appear at once: HUD VERSUS has the rival's and HUD STATS has your own. And
   he is never in YOUR colours: two figures in the same compartment of two
   HUDs in the same clothes read as one figure who changed sides. He takes the
   cartridge's dancer palette half-way round the four from yours
   (`rival_cossack_palette`) — green against the default blue and red, and
   out of reach of whatever the colour chord makes of yours.
   **AND THE COSSACK'S COLOUR CHORD ANSWERS ONLY IN HUD STATS** — anywhere
   else it changed a palette nothing on screen was using and chirped to say
   so, and in HUD VERSUS the cossack on screen is not yours to paint.
   See `hud_set` and `make gba-check --versushud`.
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
   **AND ON THE SHARED BOARD IT NOW READS ITS PARTNER, BEHIND THE CHORD.**
   `computerMove` looks at the settled field and nothing else, which is fine
   with a board to itself and is the whole problem with one to share: both
   players score the same twelve columns with the same routine, pick the same
   one, and the two pieces — solid to each other, `checkCoopCollision` — spend
   the descent shouldering. Measured over twenty-four playouts with the
   computer on both pads: 48% of every shift either of them asked for was
   REFUSED, and nine in ten of those by the partner rather than by the wall or
   the terrain. `coop_aware` is two things and each is worth about half the
   gain — the partner's piece is dropped onto the settled board and read as
   ground WHERE IT WILL LAND (not where it is: a column it is merely passing
   through is not full, and stacking against that phantom wall leaves a hole),
   and the soft drop WAITS while the piece is still short of its column,
   because a shift the partner refuses is retried eight frames later and by
   then a piece that kept dropping is out of position. Together: 908 pieces
   and 34 lines become 1637 and 197, holes fall from 903 to 666, refusals from
   48% to 27%. It costs pace, about eighty frames a piece against sixty-six,
   and it cannot hang because gravity runs whether Down is pressed or not.
   **It is OFF unless the chord has been rung**, for the same reason the pause
   menu is: WITH COMPUTER is a mode the cartridge ships and what it ships is
   the player above. `make gba-check --coopai` checks both halves of that door
   on a fixture that decides it outright — one well, three partner positions,
   and the computer takes the well every time without the chord and never with
   it — and the playouts are in `make test`.
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
   **A GAME IS WRITTEN DOWN AS IT ENDS, not at the end of the match.** L81DD
   is called from the top-out itself (main.asm.txt:600), which is what stops
   A+B's restart throwing away the game it just finished: five games are five
   rows. The cartridge keeps one typing flag per PLAYER ($74/$75) and marks
   each row's owner in the top two bits of its initials so Left and Right walk
   a player between their own rows; the port types them one after the other,
   oldest first.
   **AND OVER A CABLE BOTH PLAYERS GO ON IT, WITH THEIR OWN NAMES.** A linked
   match is lockstep — both consoles simulate both boards — so each one has
   known the rival's score and lines all along and simply never wrote them
   down: a race ended with two pages that disagreed about who had been there.
   Both players are inserted on both tables now. The one thing lockstep
   cannot hand over is the NAME the person at the other end typed, so that
   crosses on the same cable the match ran on, in the few seconds between the
   last piece and the bottom of the page: three letters each way, one per
   transfer, round and round, with a receipt bit in every word (see
   `TengenNameSwap`). It is NOT stop-and-wait like the lobby — this exchange
   is symmetric, both ends have something to say and neither is asking — and
   the LINGER at the end is not padding: the last thing each console waits
   for is the other's receipt, so one that went quiet the moment it had
   everything would leave the other with the letters and no way to learn that
   its own arrived. What it does not solve, and it is an edge rather than an
   oversight: the two tables are two consoles' own histories, so a score can
   make one and miss the other, and a player who made only the rival's sends
   the letters an untyped row carries. `tools/run_link.py` plays the whole
   road on two cores — two scores, two names typed on two consoles — and
   reads both pages back off the tilemap.
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
   **FIVE ROWS — PAUSE, THE TUNE, EXIT — AND FOURTEEN COLUMNS BECAUSE
   THIRTEEN CANNOT BE CENTRED.** It was ten rows deep, which in a twenty-row
   screen is half the board covered by a menu with three lines in it — against
   the cartridge's own PAUSE at eight columns by two (`pauseColsRows1`,
   `$B679`) and its GAME OVER plaque at six by four, that is out of proportion
   with both. Every row of it was air, and the air is what went: first the
   blank row under the heading (PAUSE is told apart from the list by its
   COLOUR, which is what a heading is for), then the word MUSIC — the tune's
   NAME is the entry, so a label over a choice that is already its own label
   says nothing — and last the blank row between the two entries. Three lines,
   three rows, a border top and bottom.
   **AND FOURTEEN IS THE FLOOR, not a preference.** The box lands on the
   board, whose middle is x=120 — the screen's own — and a box of odd width on
   an even grid cannot be put there: thirteen columns sits four and a half
   pixels left, which against a playfield you are looking straight at is not a
   subtlety. So the width goes 14, 12, 10 and never 13: one column of cursor
   plus KOROBEINIKI's eleven is twelve of interior, which is fourteen with its
   frame. Trimming the name's own trailing blank buys nothing, because the
   blank is not in the box — the interior is measured in whole tiles and
   eleven characters occupy eleven of them.
   What the even width costs is the PARITY of every line inside it, and with
   it the vertical nudge, so a gap in this box is a whole blank row or
   nothing at all.
   **TWO PIXELS INSTEAD OF THAT LAST BLANK ROW WAS TRIED AND OVERLAPS.** The
   counters' layer is the port's only sub-tile vertical nudge, so a line can
   be lifted two pixels onto it — but these glyphs are seven pixels of ink
   under ONE blank row, which is all the leading two stacked lines have, and a
   line lifted two writes through the one above it: PAUSE came out across NO
   MUSIC. The row was won by dropping the blank outright instead.
   **THE ONE PIXEL UNDER A HEADING IS IN ITS LETTERS.** The interior is three
   lines of seven-over-one exactly, with the last one already standing on the
   frame, so no layer can make room and there is no room to make. What moves
   is the ink inside the tile: PAUSE and EXIT? are drawn with copies of their
   own letters shifted up a row (`PMENU_RAISED_BASE`, nine tiles built at
   boot out of the cartridge's glyphs), which gives the heading's blank row
   to the gap beneath it. The heading meets the frame above it the way the
   last line meets the frame below, and stands two pixels clear of a list
   whose lines are one clear of each other. `tilemap_text` maps those tiles
   back to letters.
   **CENTRED MEANS AGAINST THE LINES UNDER IT, not against the frame.** One
   axis per box and the heading is on it. The column's axis is BESIDE the
   cursor's column — KOROBEINIKI leaves no other way to have an arrow — so
   PAUSE stands on that axis over its entries; centred on the whole box it
   stood half a tile left of the EXIT under it, which is what the eye
   compares. Measured off the screen, the three lines' ink centres are 123,
   122.5 and 122.
   **AND THE QUESTION HAS ITS OWN BOX, TEN WIDE**, with all three lines on the
   box's own axis. Fourteen is the column's floor, not the question's: in the
   wide box NO was centred six columns from the arrow. Eight was tried and is
   one step too narrow — EXIT? fills it, and a centred YES starts exactly where
   the arrow's tip is. In ten, EXIT?, YES and NO come out at 118.5, 118.5 and
   119.5 against a middle of 120, and an arrow fixed three pixels into the
   interior (the offset layer's column 0) is eight pixels clear of YES and
   thirteen of NO. Same rows as the column, so only the sides move, and the
   braid the wide box covered is put back through `g_repaint` on the way in
   and out. See `PQUEST_W`.
   **THE CURSOR IS AN ARROW IN A COLUMN OF ITS OWN.** It used to sit two
   columns left of the line it marked, which is where the settings screen
   puts its own — and with MUSIC gone the line it marks is the tune's name,
   eleven characters wide with nothing like two columns spare beside it. It
   is a FIXED column at the interior's left edge now (`PMENU_IN_TX`), the
   text centred in what is left; SELECT moves it, A takes a choice, B backs
   out, and START resumes from every line including EXIT and from inside the
   question.
   **AND THE BOX HAS TO BE TORN DOWN ON THREE MAPS**:
   `draw_static_screen` puts the main background back, but the lines on the
   offset and counter layers are in the middle of the board where it never
   writes, so the window vanished and the words stayed — see
   `clear_pmenu_layers`, called off the same `g_repaint`.
   **AND CENTRING IN THIS BOX STILL NEEDS TWO BACKGROUNDS.** The field left
   beside the cursor is eleven columns, so a word whose length is the wrong
   parity misses the middle by half a tile and goes on the offset layer
   (three across) to miss it by one pixel instead — see `draw_pmenu_line`.
   The question mark in EXIT? is the one glyph in the port that is not the
   cartridge's: the tile set is ASCII-indexed but $3F is a LEFT ARROW, so
   `extract_assets.py` draws one into a slot the cartridge left empty
   (`TILES_GAME_QUESTION`) and refuses to if the dump has art there. It is
   also the one glyph `tilemap_text` has to map back by hand, and until it
   did, "EXIT?" read off the tilemap as "EXIT" and the harness could not tell
   the question from the line that opens it.
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
   `piece_cell_tile` in gba/video.c does the same for the falling piece and the
   preview. Occupancy is `cell != 0` everywhere and TT_WALL is 15 either way,
   so nothing downstream notices — except one thing, found late: a clear's
   join-breaking (`kJoinBreak`, L8A85) reads cells as joined tiles, and run
   over piece ids it turned a J beside a cleared row into the wall value, an L
   into an S and a Z into a T. A board of piece ids has no joins to break, and
   `tengen_step` skips it there (`tengen_collapse_rows_joined`).
   **AND OVER THE CABLE IT IS PAINT, WORN BY BOTH.** A linked match is two
   consoles comparing state byte for byte, so what they must agree on is the
   CELL FORMAT, and the lobby settles it: a SKIN stage between HANDICAP and
   GO, in which the master offers its title's skin by a FINGERPRINT of the
   art (eleven bits of FNV over its 51 slots and its palette) and the slave
   answers in its echo whether it has the same art. The skin goes on only if
   both — and both consoles know both facts, so both reach the same answer.
   By the art and not by the index, because two builds may list their skins
   in different orders or not have the same ones; a build that predates the
   stage echoes it back empty, which reads as "no", so an old console and a
   new one fall back to the release together. The master's choice wins, as
   for the level and the tune, and on the slave it can be a skin its own
   title is not wearing (`g_board_skin`, apart from `g_title_skin`). What the
   cable does NOT carry is the prototypes' RULES: a rule the two players did
   not both choose is not a race, so a skinned linked match plays by the
   release's, and goes on the release's HIGH SCORES page, where the records
   swap needs both consoles. `tools/run_link.py` plays it on two cores: the
   master in a prototype, the slave left in the release, both screens in the
   fret, both boards piece ids, byte for byte; and a skin on the slave's
   title alone dresses nobody. **THE MENUS AND THE HIGH SCORES PAGE WEAR
   IT AS WELL**, which took three things a first pass got wrong: the frame is
   TWENTY-FOUR tiles (four corners and four runs), not the six the board
   happens to use, so replacing six left those screens two thirds blue braid
   and one third green fret; the menu frame draws out of bgPalette1's bank 2,
   so that bank needs the skin's colours too — and the NOTES had to move to a
   bank of their own (15, the one nothing else claims) or PRESS START TO PLAY
   came up in the fret's red; and the port's panels needed a top run of their
   own above tile 255, because the one tile the release shares between "the
   panel's top" and "the menu's bottom border" answers to two different
   prototype runs.
   **THE TETRIS BANNER AND THE MENU LOGO ARE THE SAME SIX LETTERS** — literally
   the same 39 tiles in the release — so they change together, and they are
   the one part of a skin that is NOT a slot swap: the release reuses a tile
   between letters where these builds use a distinct one at each place, so the
   art cannot be handed over slot by slot. The prototype's own tiles are packed
   into a window of their own (SKIN_BANNER_BASE) and drawn by its own numbers.
   The logo is SEARCHED for on each dump's menu — proto_b keeps it at rows
   12-14 and proto_c at 10-12 — and proto_a has none at all, so its menus keep
   the release's rather than a hole.
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
   **TWO OF THE THREE ARE NOW MEASURED ON THE DUMPS THEMSELVES**, which is
   what ground rule 1 asks for, since the list they came from is somebody
   else's writing. The playfield of these builds is where the release's is
   ($0600, eight bytes a row), so a row can be planted in RAM and the console
   watched:
   * **the instant clear is real.** Counting from the frame a piece touches
     the stack to the frame a completed row stops being full: the release
     takes 29 frames, and proto_a, proto_b and proto_c take 2. That 29 is its
     sweep; there is nothing of the sort in the three.
   * **the missing wall kick is real.** Pieces pinned against the right wall —
     the side the release's one-column-LEFT kick would rescue — rotate nine
     times in ten, exactly as they do a column further in, and in not ONE of
     those rotations does the piece change column. A kick is a rotation that
     displaces; these never displace. The tenth is the rotation that would
     have needed one, and it is refused.
   * **the ten-line level is real, and the start is a FLOOR.** Clearing one
     row at a time on all four dumps, from level 0 the level goes up at 10
     and 20 lines. Started at 3 (B, D) it holds until FORTY lines, and at 5
     (C) until sixty: the level is the higher of the chosen one and
     lines/10. The port had it as start + lines/10 — the same from 0 and
     wrong from anywhere else — and has the floor now, with a native test.
     This one had been written off as unmeasurable, because planting a full
     row seemed to raise SCORE and leave LINES alone. It was the experiment:
     the row went in under a stack that was already there, so the next piece
     never touched it. Emptying the field before every row, every clear
     counts. `tools/probes/proto_rules.py` does it.
   What is deliberately NOT taken is the SHAPE of their front end: only
   1 PLAYER and 2 PLAYER, a LEVEL SELECT of 0-9 on B, C and D (A's is four
   difficulty steps, drawn as art rather than text), no handicap and no music
   menu. Taking those away on a chord rung at the title
   would remove things this port has and a player chose, so they wait for a
   decision.
   **THE GAME OVER PLAQUE AND THE PIECE HISTOGRAM COME TOO**, and both had to
   be played for rather than read: each dump was taken to a real game over
   with DOWN held, and the plaque's rows and the histogram's floor read off
   the screen there. The plaque is eight box tiles ($25-$2C against the
   release's $29-$3C; the words are plain ASCII and identical in all four) and
   a palette — red in the release, BLUE in all three — and the palette needs a
   BANK OF ITS OWN rather than the release's bank 3, because that bank is also
   the HUD's and every prototype's NEXT is as red as the release's. Taking
   bank 3 wholesale turned NEXT grey; `plaque_bank()` borrows a title bank
   instead, as the menu logo and the histogram's runs do.
   The histogram is the bigger difference. The release shares ONE eight-step
   bar between its seven columns and names them with a strip of tetromino
   icons underneath; a prototype has no icon strip at all and gives each piece
   its own eight-step run drawn in THAT PIECE'S OWN BLOCK PATTERN, so what a
   bar is made of is what names it. Seven runs of eight at $5B, $63, $6B, $73,
   $7B, $83 and $8B — and which run is which piece is checked rather than
   assumed: run i's full tile is drawn in exactly the colours of block tile
   $0(i+1) in all three dumps, so TT_I..TT_Z fall straight onto them. The two
   rows the icon strip used to take become two more rows of bar.
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

## One thing that looks like a bug and is the cartridge's, and one that was not

Both were reported from playing, both were measured, and both came back
"this is what the original does". They are here so the next person to see
them does not spend the afternoon again.

- **The left panel's shelves do not touch the rope; the right panel's do.**
  There is one black pixel column between them, measured on the screen at
  the shelf rows: the shelf's last lit column is x=63 and the rope's first
  lit column is x=65. It is the cartridge's. The wall tile `$6A` — the first
  of the two the left wall is made of — has a blank leftmost pixel column,
  and the coop screen this panel is taken from puts seven ledge tiles
  straight against it: `9D 9D 9D 9D 9D 9D 9D 6A 6B ... 73 74 9D 9D ...`.
  On the right the wall's LAST tile, `$74`, is lit to its edge, so that
  junction closes. Same art, same layout, same seam. Closing it would mean
  drawing a tile the cartridge does not have — which is what the chord now
  does, and only the chord (see below).
- **Stacked pieces merging where a row was cleared** was not the cartridge's
  after all, and the note that used to stand here said it was. The blocks'
  separator lives on each tile's TOP row and LEFT column — `$01`-`$0E` — and
  which one a cell gets is decided when the piece LOCKS. A row going away
  therefore leaves lies behind it, and the cartridge does not leave them:
  `L8A85` (main.asm.txt:1589-1624) runs the row ABOVE a cleared one through
  a table that clears its downward joins and the row BELOW through one that
  clears its upward ones, and a cell left joined to nothing becomes `$0F` —
  the standalone block, the same graphic the handicap's garbage uses. The
  port had none of it. Measured on the dump before it was ported (a `$06`
  under a cleared row comes back as `$0F`), then found in the table, then
  ported into `tengen_collapse_rows` where it belongs. `make clear-check`
  plants a row in both machines and compares what the clear rewrote;
  `make trace` cannot reach this, because its button script never completes
  a row in three thousand frames.

## Melons that were opened

*(The fireworks were on this list too, as "their distance and the dithered
sky". Both halves came out different from how they were written down. The
cartridge places its bursts anywhere from x=16 to 240, over the braid
included, and draws them BEHIND the picture — attribute bit 5 on every
burst sprite — so the braid, the cathedral and the logo cover whatever
crosses them. The port drew them in front and then clamped them inward to
keep them off the braid; it honours the bit now (`TITLE_FIREWORK_PRIO`) and
the clamp is gone. And there is no dithered sky: rendered with its sprites,
the cartridge's sky is plain colour 0. See reference/NOTES.md.)*

*(`VBlankIntrWait` was on this list and is in. `vsync()` spun on VCOUNT,
which on a real console is the CPU at full clock through the whole visible
frame; it is the BIOS's SWI 5 now, which HALTS until the vertical blank's
interrupt. Measured in mGBA, a frame of play executes 11,982 instructions
where it used to execute 42,922 — the other 31,000 were the spin, about
seven tenths of every frame — and the title 18,000 against 50,000. The
wake-up is on the same scanline the spin left on, so the timing did not
move: the golden audio, the game over's 480 frames, the two-console link
(same 188-transfer handshake, same 548 transfers, still byte-identical),
and against the cartridge `make tune-check` (the four tunes note by note
over 1500 frames) and `make dance-check` all come out as they did. What it
needed is what the note said: the vector belonged to the cable. There is
ONE handler for the program now (`irq_handler`, video.c, in IWRAM, ARM),
installed at boot by `irq_init` before the first vsync; it acknowledges
every flag in IF AND in the BIOS's mirror at $03007FF8, which is what the
BIOS actually sleeps on, and hands a serial interrupt to
`link_serial_service`. The cable only switches its own source on and off.
`make gba-check --sleep` checks the vector and the enables, and that each
frame of play goes into the BIOS and stays there — 39 steps from the call to
the next frame, where a spin puts back 43,000 and fails it.)*

*(The coop HUD was on this list and is built: a panel per player, the
board's totals under the chord, and a second HUD against the computer that
hides the partner's. See draw_coop_panel and `make gba-check --coophud`.
So is the computer in coop, on the same chord: see roadmap 23. What it
still cannot do is slide a piece UNDER one already placed — and that one is
MEASURED, not pending. Teaching the height profile to report a cavity you
could reach sideways was tried twice, with a one-cell probe and with a
two-cell one, and both made it play WORSE: 1637 pieces and 197 lines became
905 and 102, then 1177 and 152. The cartridge's scorer drops pieces onto a
surface from above, and a hole it aims at but the driver cannot steer into
is a piece hung on the overhang. It stays out.)*

## Under the chord: the cartridge's bugs mended, and a computer of its own

Asked for as "bajo truco, soluciona los bugs conocidos del propio cartucho,
y mejora la IA, especialmente en cooperativo". Everything here is behind
the chord, and without it the port is the cartridge, bugs and computer
included.

**The bugs** are the three on the "looks like a bug" list, and a fourth
that was already mended (the XE mod's two off-by-ones). `TengenGame.mended`
carries the decision into the core: set from the chord alone, and over a
cable from the MASTER'S (`tengen_link_start`), so both consoles in lockstep
agree.

- *The coop deal on top of the partner* (getNextTetromino asks nothing,
  main.asm.txt:3688-3725): mended, the deal WAITS a frame at a time while
  the new piece would come up inside the partner's (`spawn_meets_partner`).
  Waiting, not shifting: the entry column is the player's side of the
  board, and a piece that came up elsewhere would be a different rule.
- *Blocks coming down into a falling piece* when the partner's rows
  collapse (INFERRED from the split between the settled buffer and the
  falling piece): mended, the falling piece is lifted, a row at a time and
  no more than it needs, out of what came down (`finish_clear`). Lifting is
  what the player would have seen anyway, the stack rising into the piece.
- *The gap between the left shelves and the rope*: a copy of `$6A` with its
  first column drawn from `$9D`'s last, on the rows `$9D` has ink, put
  wherever `$6A` stands right of a shelf (`mend_shelf_joins`, hud.c).
  Measured on the screen, closed with the chord and unchanged without.

`make fuzz` plays half its games mended and in those the two overlaps may
not happen at all; in the other half they still may.

**The computer.** The cartridge's computerMove reads a height profile and
two candidate slots; the chord had already taught it to read its coop
partner (`coop_aware`, roadmap 23). This is a second player, written the way
Tetris programs have been since Dellacherie: the board as one sixteen-bit
word a row, every placement scored by El-Tetris's six weighted features,
NEXT looked at for the best six, and nothing chosen that the pad cannot
actually do — each path walked frame by frame the way the core will play it.
It drives the same pad at the same pace (a shift every eighth frame, a
turn every sixteenth, no Down), so it is a better head, not faster hands.
What it took to get there, in the order it went wrong:

- The first version, measured with the soft drop on, looked like a giant;
  at the cartridge's pace it LOST to computerMove at level 18 (16 games in
  16 against 9). Its reachability turned first and then shifted, at one
  gravity for the whole fall; the driver does both at once and the
  fractional levels fall faster on some rows. Walked frame by frame
  (`ai_reachable`) it reaches 905 aims of 905 at level 18, and wins.
- A placement out of reach was dropped; with nothing in reach that left no
  target at all, and the piece fell where it came in. Out of reach is now a
  penalty, not a veto.
- On the shared board, reading the partner's landing as ground for the
  score (what `coop_aware` does) made it plan rows around cells that were
  not there yet: the holes were its own. It prices the columns instead.
- A cost for crossing to the partner's side measured worse with the soft
  drop and better without it: -5000 a column.
- On the ROM it spilled 140 turns of 3000 into a second frame. A shape
  table, wells counted by set bit, and a slice of eight with the first look
  charged double: none (`--aiframe`).

The table it was measured at is in tengen_ai.c. Coop at level 18 still
buries both; with a person for a partner — who does not say where they are
going — it is guessed straight down from where their piece is.

## Why the GBA layer is five files

`gba/main.c` was one file of six thousand lines and had stopped being
readable. It is five now, sharing `gba/port.h`:

- **`gba/port.h`** — the seam. Every constant that says WHERE something is on
  the screen, which palette bank it is drawn in and which of the four
  backgrounds it rides, plus the declarations of the symbols that really do
  cross between the five. Anything used in only one file stays `static` where
  it is defined, which is most of them (84 of 220 at the split).
- **`gba/video.c`** — the hardware: backgrounds and their scrolls, palettes
  and their banks, tile and sprite uploads, the keypad, the SKINS, and the
  program's one interrupt handler with the vsync that sleeps on it. The
  rest of the port draws through its primitives and never touches a register.
- **`gba/hud.c`** — what a match looks like: the two panels of rope, the
  board, the line-clear sweep, the cossacks, the piece histogram, the
  high-score table, the plaques.
- **`gba/frontend.c`** — the screens either side of one: the title and its
  fireworks, GAME SELECT, LEVEL SETTINGS, the credits, the link lobby, and
  the music the front end plays.
- **`gba/match.c`** — one frame of play: the pause menu, the computer's
  input, what a step has to announce, and the order the drawing happens in.
- **`gba/main.c`** — the state machine: which screen is up, what a button
  does on it, where it goes next.

The split was mechanical and is meant to stay honest: a symbol gets external
linkage because another file names it IN CODE, not in a comment. (Checking
that against comment-stripped text is not a detail — `main` itself came out
"shared" at first, because `main.asm.txt` is cited in half a dozen of them.)

## Why the save is also in the cartridge's flash

The first board made for the port — an AGB-E05-01, which FlashGBX knows as
"with MSP55LV128M" — ran build 0d357df perfectly and saved nothing. The
port's save code was right and had been for a year: byte accesses to
$0E000000, the wait states set, the `SRAM_V113` signature every flash cart
and emulator goes by. What was wrong was the assumption under it, that save
RAM is kept. FlashGBX's own notes say it of these boards in one line: an
SRAM chip and no battery, "not designed for unmodified ROM files". The
games sold on them are patched to copy their save into the flash chip the
game itself is in, and back at power-on.

So the port does that itself, and finds out for itself whether it has to:

- **What was read first.** FlashGBX's table for the board
  (`fc_AGB_MSP55LV128M.txt`: AMD's commands with D0 and D1 crossed, sectors
  from the chip's own CFI table) and its CFI parser, which is how the
  crossed lines are told ("QRY" reads "RQZ"); the patchers that give other
  games a save on these boards (metroid-maniac's gba-auto-batteryless-
  patcher and PocketNES's flash_write: unlock, 0xA0, the word, wait until
  it reads back; 0x80, unlock, 0x30 to the sector, wait for 0xFFFF; 0xF0),
  from RAM with the interrupts off; and gbadev's note on bootleg carts.
  The same sequences, sent the same way: the chip the port has never met
  has met them.
- **A journal, not a copy.** The patchers erase a sector and write the
  whole save at every change — fine for a game that saves at a save point,
  and a second of frozen console at every pause for one that saves a paused
  game. A change appended to a log is a few words (`src/tengen_nvlog.c`);
  the erase happens when a sector fills, which at power-on is nobody's
  wait. It was written and tested on the host first, against a chip model
  that loses its power at every word in turn, before there was a line of
  GBA code to call it.
- **Asked, not assumed.** Nothing says a board has no battery. RAM that
  kept its contents may only have been off for a second; RAM that lost
  them may be a dead battery. So the RAM is marked with a hash of what was
  written to it and believed while it adds up, and the flash is written
  regardless on a board that has one to write. A board with a battery and
  one without come to the same save by different halves.
- **What an emulator cannot say.** mGBA's cartridge ignores writes, so the
  routines ran there and proved nothing. The chip had to be stood in
  (`tools/romcheck/flashchip.py`): the port's own routines stepped an
  instruction at a time, with the model holding them to what a board would
  punish and an emulator forgives — a single instruction fetched from the
  cartridge while the chip is answering a command. Ninety-odd deliberately
  broken builds were run against it to see that it bites.
- **The red screen** is the one place the port chooses to stop. A routine
  whose chip never goes back to reading has nowhere to return to: the
  instruction after its call is in that chip. It stays in RAM, keeps
  asking, and paints the backdrop red so that what is seen is not a
  hang with a picture on it.

- **The page before the first write.** The page behind L+R+A was first
  drawn after power-on had done everything, the journal's first writing
  included — so a chip that hung on being written would have left a red
  screen and no page, on the one board where the page was needed. Power-on
  only reads now; what it owes the chip is paid at the first frame, and
  not at all while the page is up. From the page SELECT goes on without
  the chip, so a board that will not be written can still be played, and
  photographed.

None of this has met the board as this is written. The page behind L+R+A
at power-on is there for the day it does.

**The day it did.** Build 22f0bac on the board: FLASH CHIP ANSWERS, QUERY
0052 0051 005A, WIRING D0 D1, COMMANDS AMD 0002 — and CHIP 65536K 512X128K,
CHIP ID 008A 227D, which with the crossing undone is 0x89 and 0x227E: a
sixty-four megabyte chip, not the sixteen of the MSP55LV128M the flasher's
profile is named for. Nothing had assumed the size or the maker; it was
asked. JOURNAL IN USE, GENERATION 1, 85 saves. STARTED RAM AND FLASH, SAVE
RAM KEPT: the RAM had not been off long enough to lose its mark, which is
the case the mark is there to tell apart. What has still not run on a board
is an erase.

## Two cartridges on a cable, and what a console that has left sounds like

The first evening with two cartridges and no copy found six things, and
five of them were one thing seen from different sides: what a console does
when the other is no longer where it thinks.

- **A console that has left is not a quiet cable.** The port rests in
  multiplayer mode, answering 0, so that the other end's LINK CABLE screen
  can say "wait for its player" instead of "connect the cable"; a master
  on its menus sends a beacon of zeroes for the same reason. Both were
  added for the lobby, and both are exactly what a match takes for "the
  partner is slow": a word that is not a move resets the count of quiet
  frames, so LINK ISSUES and CABLE LOST never came. The loser of a race
  pressed START to play again (it leaves; A+B is what plays again, and
  nothing said so), went to the title, and the winner's console sat with a
  frozen board and a HUD that still changed. Leaving is now part of the
  game the two consoles agree on step by step, like the A+B restart and
  the pause menu's EXIT: the START is read off the wire. And for the ways
  of leaving that no button announces — the reset, the switch — the other
  console is believed when it says it is elsewhere: six transfers of its
  not playing, with this console's own word intact so that a mangled
  transfer is not taken for an answer.
- **The winner plays on.** A race is two boards and each is its player's:
  with the other gone, the cable is put away and the match carries on with
  that pad at rest. A shared board cannot, and ends as a lost cable does.
- **A move that arrives late is not a desync.** The console started over
  under a match answered, for a few transfers, with the last move its
  interrupt had loaded: a word for the step before. That had always been
  the end of the match on the spot — and so it was for any console whose
  interrupt was a transfer late, once. Both consoles see the same two
  words; when they are not for the same step neither takes them.
- **One speaker each, one board between them.** The cartridge has one
  sound engine and the port's linked match has two, each following its own
  player; the end of a shared board was reported on the player whose piece
  did not fit, and the other console's tune played on over GAME OVER.
- **The probe that the stand-in answered.** The copy went across only on
  SELECT. The probe for a console in its BIOS had been written from
  GBATEK's table and checked against a stand-in that answered whenever
  asked; the BIOS turns between three serial modes, is deaf for two frames
  after each turn, and is set back by a word at the lobby's speed. Its
  disassembly gave the model those; it did not explain a probe that never
  once got through, so the probe was not tuned to the reading but made to
  do what the send does, which works: silence first, then asking. The
  stand-in now has a knob for how hard a word of the wrong speed is on it,
  and the old probe fails at the settings the new one passes.
- **The check that read the wire.** Six mutants of the new probe — its
  silence a third as long, one question for four, the lobby never hushed —
  still found the stand-in, which is all the checks asked: the same hole
  the first probe went through. So the cable was made to keep what goes
  over it, and the check to read the round off the wire, frame by frame.
  The first reading failed on the real build: one round in six was cut
  short after a single attempt. The cable's screen looks for a Wireless
  Adapter plugged in late every five seconds, starting the cable over, and
  did it wherever the probe happened to be. It waits for the end of a
  round now. And a check of the records had been counting presses of START
  to get back to the cable: one too many was the master's, and two of its
  four meetings were held on a paused game, passing.
- **Whose table it is.** Two cartridges gave each other nothing, and the
  first mend gave each other everything. It was stopped before it shipped:
  a table is its owner's, and the friend with the monstrous scores would
  have pushed the owner's off the bottom of it. A copy's rows are another
  matter — it was sent by this cartridge an hour ago, starts from the cold
  table and is about to be switched off — and go across unasked as before.
  Between cartridges each player is asked, once, with NO under the arrow.

## Two titles, one demo — and a chord that leaves the title alone

The same evening's wish list had a joke in it: two consoles that have just
finished a game together go back to their titles at once, and their demos
start at once; "linking them would be funny". And then its limit, from the
player, before it was built: under the chord, of course — the port of the
cartridge is better left alone by now.

- **What a title says.** A console at rest answered the cable with 0, and
  that was all another console could know of it. On its title, under the
  chord, it now answers with a word that every lobby ever built still
  reads as 0 (the tag is NONE) and that says: a title, for this long, its
  demo due or not. There is no interrupt at rest, so it is read off the
  data registers, and the first question was how to tell news from what a
  console said a minute ago before it went off to a game: each word
  carries a count, and a console knows a transfer has happened when its
  own comes back with a count it has not seen.
- **Who plays.** The first plan had each console's computer play its own
  pad and send its moves like a player. But a word is loaded for the next
  step as the last one arrives, before this console has played it: a
  computer there steers by the board of two steps ago. Both computers run
  on both consoles instead, inside the step; the cable keeps time and
  carries only whether somebody has picked a console up.
- **Two bugs in one frame.** The first run: the master asked, and on that
  very frame the slave started a demo of its own. The lobby's helper had
  sent a HELLO before it could be told what the lobby was for — one word
  of a player's, where an invitation was meant — and the slave, its demo
  due, was waiting on "a title on the master's end", which the master had
  stopped being the moment it began to ask.
- **Old news, every twelve frames.** What a console hears of the other
  title is forgotten after twelve frames without a transfer. The first
  build forgot, with it, which of its own words it had last seen come
  back — and so the word still standing in the register was "a transfer
  since I last looked" again, and what a console had said before going
  off to its own demo was heard for ever. Two mutants of exactly that
  logic survived every check, which is how it was found: the check that
  would have caught them failed on the build as it stood.
- **Half a kilobyte.** The second computer's working state did not fit:
  the linker keeps four kilobytes of IWRAM for the stack and said so. It
  lives in EWRAM.
- **The chord on the title.** L+R on the title rang the chord and, since
  the title was where the prototypes had first been found, put the first
  of them on. "It should turn the cheat on, not change the skin: leftover
  code, I suppose." It was not leftover — it was written down in the
  README — but it was a title changed by a chord rung for something else.
  The skins are a list in OPTIONS; the title's chord chirps (the credit
  that changes colour with it is on GAME SELECT) and leaves the picture as
  it was.

## The demo that was shared in the emulator and nowhere else

Three lines came back from two consoles: after a cooperative game the
master is back on its title before the clone; the plaque should say A:
AGAIN and B: BACK; and waiting on the title, nothing happens. The first
and the last were one evening seen from two ends.

- **Two consoles that were two programs.** A linked match is played under
  the master's chord on both consoles — pause menu, the sheet at the end,
  the table that goes after five seconds when nobody made it. And then the
  match ended and each console was itself again: the cartridge with the
  chord, the copy it had sent without. The copy read the table for
  seventeen seconds where the cartridge read it for five, because the
  page asked its own console's chord and not the match's; and of the two
  titles that should have shared their demo, one had never been told there
  was anything to share. The chord is catching now: a console that plays
  under it has it until the power switch.
- **Cartridges only.** The demo had been compiled out of the copy, as
  things the copy can never do are — and the first two consoles anybody
  tried it on were a cartridge and its copy.
- **The hole that was the design.** With both of those mended in the
  emulator, on the cable whose transfers take a frame's worth of time and
  not none, two CARTRIDGES with the chord rung on each still played two
  demos. A title read the other's words off the data registers once a
  frame, with no interrupt, never while a transfer was passing — and the
  master's lobby starts a transfer every frame. A slave whose frame falls
  on those transfers finds one passing every time it looks. That is not a
  rare alignment: it is two consoles switched on, for however long their
  crystals agree. The check that had passed forty mutants ran on the cable
  where a transfer is over before anybody looks. So a title takes the
  port's interrupt, as the lobby and the match always have, and reads what
  the interrupt left; and the checks of anything read from the main loop
  run on the slow cable, with a copy on either end.
- **A sky to say so.** None of that could be seen on two real consoles:
  two titles and nothing happening is what every failure of it looks like.
  The title's night turns deep blue while the other title is heard, and
  violet while the two shake hands — a tell for the next evening, and one
  that can stay: it is a night all the same, under the chord.
- **One button each.** "A AND B: AGAIN / START: LEAVE" was the
  cartridge's chord and the port's way out, spelt out; the answer from the
  players it was written for was A: AGAIN, B: BACK. Under the chord A alone
  is that A and B, and B alone that START — but not on the frame it goes
  down: two fingers meant to land together land a frame apart, B first as
  often as not, and whoever had learnt A+B would have been back on the
  title for playing again. B leaves when it is let go, or after a moment
  held; an A meanwhile is AGAIN. Without the chord the pad is the
  cartridge's, and neither button alone is anything.
- **And not as the board dies.** Written and checked, the two buttons had
  a trap in them that no check had been asked about: a board is lost with
  both thumbs going, and the turn meant for the piece that did not fit
  lands a frame after it did not. With one button for each job that turn
  was AGAIN — the game gone before its player knew it was lost — or BACK,
  out of the race altogether. For three-quarters of a second after a board
  dies the two are nothing, and the plaque names them when they are
  something.
- **A cable that interrupted without being asked.** A build whose title
  never switched its port's interrupt on heard the other title all the
  same: the emulated cable raised the serial interrupt on both consoles at
  the end of every transfer, whatever SIOCNT said, and the interrupt
  enable register was all that stood in the way. The hardware requests one
  only of a console that asked. Nothing in the port had leant on it — the
  whole chain passed on the corrected cable — but the mutant that showed
  it would have shipped as "caught".
- **Thirty-two bytes.** Internal memory had exactly that to spare above
  the four kilobytes the linker keeps for the stack, and the linker's own
  comment still said ten. The sky's two bytes are locals.

## It worked — and what two consoles said next

The demo was shared on the first evening the new build saw two consoles,
and three more lines came back with the news.

- **"Take the blue sky away."** It had been put there to say which half of
  an unseen mechanism had failed; nothing failed. A tell belongs to the
  build that needs one, and the title's night is the cartridge's again.
- **"The music between the two is out of step in the demo."** One tune,
  two speakers, a frame or two apart: an echo, and heard as soon as two
  consoles sat side by side with nobody playing. Each console started the
  match's tune on the frame IT got out of the lobby, and the two never get
  out on the same one. Everything else the music does in a linked match
  was already tied to a step both consoles play together — the level-up,
  the pause, the end; the start was the one thing left to each console's
  own clock. It is on the first step now, and measured: zero frames apart,
  cartridge or copy at either end.
- **"Cartridge against cartridge, on losing, the master stays on CABLE
  LOST. Is it because I am playing against an old ROM?"** It can be, and
  the search went the long way round to say so:
  - CABLE LOST is only ever said by a match still running. So the
    master's match had not ended when the slave's had: the two consoles
    were no longer playing the same game, and the first to lose its board
    went on to the names' exchange while the other, hearing words that
    were not moves, called it gone.
  - Builds 2.20 to 2.24 were built again and put on the other end of the
    emulated cable, both ways round, coop and race, pads mashed from one
    seed for minutes, with pauses and the shared menu, with every rule on
    and hard drops raining: byte for byte the same game on both, every
    time. The first draft of this entry stopped there, at "not that I can
    show".
  - It was one build short. 2.19 and the ones before it — built too, in
    the end, back to 2.10 — are ANOTHER GAME the moment HARD DROP is on.
    The 2.20 batch was called "HARD DROP on UP held straight": a fresh
    press of UP had dropped the piece, and since then UP held three frames
    does. One core drops on the press and the other does not; from the
    first UP the two consoles share a cable and nothing else, each showing
    its own board as if it were the one. One of them is lost first. With
    2.25 on the master and 2.19 on the slave, three games in four ended
    with the slave's board lost and the master on CABLE LOST: the report,
    to the letter. (With the port's rules off they are the same game back
    to 2.10. The same file compiled against each version's core says so
    without an emulator: four numbers alike from 2.20 on, two of them
    different before.)
  - What that batch owed and nobody knew it owed: a line saying that the
    cartridges not yet flashed had become a different game over the
    cable. Two cartridges are flashed one at a time.
  - So, three things. A shared board lost on one console is lost on the
    other, as a game over and not as a cable. Each console tells the other
    which build it is, in bits of HELLO that no older build looks at, and
    a pair that differ say so on the screens their players are already
    waiting on (an older build says nothing, and is taken for older). And
    THE GAME IS PINNED in `make test`: four matches played by a small
    player that lives in the test, folded to a number each — the day a
    change to the core changes one of them, the batch finds out before
    two cartridges do.
  Whether that was the evening's cause only the other cartridge's credits
  can say. And one thing found on the way is written down in CLAUDE.md and
  not yet mended: a transfer that only one console accepts leaves the two
  a step apart for good.
- **Internal memory, again.** The interrupt needed twelve more bytes and
  there were thirty-six. The game kept for a console switched off in a
  pause — a kilobyte and a half, read twice an evening — had been sitting
  in the fastest memory the console has. It lives in EWRAM now.
- **Five hundred and twelve kilobytes, and one hundred and seventy-two
  bytes.** The program had filled its half megabyte to within 756 bytes
  without anybody counting, and this batch stepped over the line: the
  file a cart is flashed with is a megabyte from here on. Nothing in the
  game depends on the old size — the save's journal has always started at
  one megabyte — and one thing in the checks did. A file of a megabyte
  EXACTLY is, to mGBA, a Classic NES cartridge, whose boards show their
  mask ROM four times over; the flash chip the checks stand in was the
  emulator's ROM as it came, so a megabyte in, where the journal goes,
  stood a second copy of the program with its boot logo — and the port's
  own guard against a flash cart with ANOTHER GAME there refused every
  board in the list. The strict run said so within minutes of the file
  growing ("el chip no se usa, motivo 6"); the model now erases what
  follows the file, as its comment had always said it did. On a real chip
  what follows the file is what was there before: the journal.
- **Two survivors that were the checks', not the code's.** A deliberately
  broken build that left the pause menu's box over the GAME OVER of a board
  lost under a pause passed: the check looked for the box's last line,
  "EXIT GAME", and the panel drawn beside the board every frame had made
  it "DOUBLEAME0". It looks for any of the box now. And one that took the
  build's number out of the lobby passed by being SKIPPED — the link had
  thrown the number's variable away with its last use — which the strict
  chain counts as a failure and the one-check runner the mutants go
  through did not. The same search found `link_lobby_start`, a lobby that
  is not parked, which nothing had called since the choosing moved after
  the connecting: gone.

## The music that was not the music

One line came back about the build that had put the tune on the match's
first step: "The music is still out of step in the demo, cartridge against
clone. I don't understand it, because in a cooperative game it is fine."

- **The fix before this one mended what the emulator showed.** Two tunes
  a frame apart at the start, measured, mended, measured again at zero —
  on a cable whose transfers take no time and land on the edge of the
  other console's frame. Two consoles were hearing something else, and
  the sentence that said so was the second one: fine in a game.
- **What a game does not have is two computers thinking in every step.**
  A turn of a linked match looked for another move after each step it
  played — to catch up, whenever there was anything to catch up with. A
  step two players' pads make is over in a few scanlines. A step of the
  shared demo is a third of a frame and more, and a transfer takes a
  millisecond on a real cable: the one the master started as its turn
  began LANDED during the step, the look after the step found it, and the
  turn played a second step — four computers' worth of thinking, past the
  blank, a frame gone. The next turn had nothing to play, having played
  its move a frame early; the one after had two. With a transfer given
  its millisecond the emulator did it at once: 1578 steps in 1800 frames
  between a cartridge and its copy, a quarter of the master's turns
  playing no step or two — and each console's tune, which counted its
  notes one call a turn, eighteen frames behind the other's after five
  seconds and ninety after fifteen. Between two cartridges nearly half
  the master's turns, and the two tunes within five frames of each other,
  coming and going. One step a turn now; a second only for a console that
  is behind as its turn begins.
- **And the clone was losing the cable without a word.** It takes no
  interrupts; it asks after its port, and asked only while it waited for
  the blank. Its heaviest turn of the demo is a frame and an eighth: a
  transfer that landed just after one wait was looked at behind the next
  one, whose words had written over it. The cartridge had played a step
  the copy never saw; the two were a step apart for good, each counted
  six of the other's moves that were not for its step, and each called
  the other gone — in a demo, which says nothing: two consoles each
  playing on alone, seven seconds in, with the copy's frame five
  scanlines after the cartridge's transfers. It asks from inside its turn
  now. And the step itself is mended, for any cable: the console ahead
  sees the other's moves are for the step before its own and sends that
  step's move again.
- **Why nothing had seen any of it.** The rig runs one console a frame and
  then the other. A transfer the master makes during its frame found the
  slave standing on the edge of its own — the first scanline of its
  blank, the one place where a console that only asks while it waits is
  always asking, and the one moment no step is ever in the way. The slow
  cable landed its transfers as the frame ended: the same place. `_pair`
  takes a phase and a length now, and the demo is played at sixteen of
  them.
- **Five places to ask from, of which three were nothing.** The first
  draft had the copy asking before its sound, after it, as the turn
  began, between the two computers and after each step. A build without
  any one of the five passed every check — and so did the build that
  waited for its blank the old way, the one that added up the turns it
  kept a move in hand, the one that sent a move again at the first late
  word, and twelve more: sixteen deliberately broken builds of thirty-nine
  not caught. Measured, two of the five places changed nothing at all and
  a third only shortened the longest wait for an answer from a hundred
  scanlines to seventy-four, which nothing else can tell: the three went,
  and the two that are left are held to a number, half a frame (150
  scanlines with the first of them alone, 240 with neither — and a frame
  is 228). The rest were the checks': the tune's distance is now exactly
  the one frame the rig makes of it and not "within one"; not a blank
  lost; not a transfer that is not a pair; the same disturbance on an
  even step and an odd one, and three times in a match; each case held
  to the frames it has to cost. A second round of forty found four more
  — two of them a counter whose check held a console up, which begins
  the count again by itself, where the thing to do was to move one
  console's frame across the other's transfers as two crystals do — and
  the last, forty-one on the tree as it went out, none.
- **Two consoles at rest send the same word.** The build that answered
  one late word by sending its own last move again should have been
  answered in kind for ever — each console in turn a step "behind" the
  other's repeat. It passed, because with both pads at rest the two words
  of a transfer are the same sixteen bits and a console cannot tell which
  slot was its own: both sent again together and met. With LEFT held on
  one pad it lost the cable for one late word. The checks of who answers
  what hold a button down.
