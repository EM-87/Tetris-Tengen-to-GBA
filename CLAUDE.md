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

6. **Look at what you changed, on the screen.** Every change that touches
   what the player sees gets a screenshot out of the running ROM, and a
   question about pixels ("centred?", "does it touch the frame?") is
   answered by measuring the framebuffer, not by reasoning about tiles. Half
   the bugs in `reference/HISTORY.md` were visible in the first screenshot
   and invisible in the code. When the cartridge is the reference, render it
   too (`tools/render_nes.py`, sprites included) and put the two side by
   side.

   Measure the cartridge the same way. When a rule "cannot be measured",
   suspect the experiment first: the prototypes' ten-line rule was written off
   for months because the planted row sat under an existing stack.

## Where things live

- **`src/`** — the rules. `tengen_core.c` (the game), `tengen_ai.c`
  (`computerMove`), `tengen_link.c` (the cable's lockstep, lobby and records
  swap). C99, no GBA headers, tested on the host.
- **`gba/`** — the GBA layer, five files sharing `gba/port.h`:
  `video.c` the hardware (backgrounds, palettes, uploads, keypad, skins, the
  interrupt handler and `vsync`); `hud.c` what a match looks like; `frontend.c`
  the screens around a match; `match.c` one frame of play and the pause menu;
  `main.c` the state machine. Plus `nes6502.c` + `nes_audio.c` (the
  cartridge's sound engine, run), `handtunes.c` (the two hand-entered tunes)
  and `link.c` (the cable). A symbol is shared only if another file names it
  in code; everything else is `static`.
- **`tools/`** — `extract_assets.py` (all the art, from a dump);
  `run_rom.py` (the command `make gba-check` runs) with its checks in
  `tools/romcheck/`, one module per family and `harness.py` for what they
  share; `run_link.py` (two cores and a cable); `nes_cpu.py` /
  `nes_console.py` (the cartridge, run in Python); `render_nes.py` (the
  cartridge's screens as a NES draws them); `probes/` (comparisons against
  the cartridge); `trace_match.py` + `trace_core.c` (`make trace`).
- **`reference/`** — `disasm/` (the source of truth), `NOTES.md` (the index
  into it: what the ROM does, with line numbers), `HISTORY.md` (how each part
  of the port got its shape, and what was wrong before).

## Build and checks

The art and the sound engine come from a dump of the original cartridge,
which is not in the repository and must not be. Generate the headers once:
`make assets ROM=/path/to/tetris.nes [PROTO="a.nes b.nes c.nes d.nes"]`
(`PROTO` adds the prototype skins; without it the port builds the same,
minus those).

| Command | Needs the dump | Run it |
| --- | --- | --- |
| `make test` | no | always — the rules, in milliseconds |
| `make gba` | headers | to build `build/tengen.gba` |
| `make gba-check` | headers | before calling any change done: 58 checks on the running ROM in mGBA, sixteen of them on two consoles with a cable |
| `make trace ROM=... [MODE=coop\|versus\|with\|demo]` | yes | after touching `tengen_step` or `tengen_ai.c`: the port against the cartridge, iteration by iteration. The 1P and coop scripts never complete a row; `MODE="with --pad1"` plays player 1 with the port's computer and clears plenty; add `--handicap N` to any mode |
| `make tune-check ROM=...` | yes | after touching audio: the four tunes against the cartridge, note by note |
| `make dance-check ROM=...` | yes | after touching the dancers: their choreography against the cartridge's driver |
| `make clear-check ROM=...` | yes | after touching line clears: the joins a clear breaks |
| `make assets-check` | no | after touching `extract_assets.py` |
| `python3 tools/probes/proto_rules.py proto_*.nes` | yes | the prototypes' rules, measured on their dumps |

GitHub Actions (`.github/workflows/tengen-gba-port.yml`) runs everything
that needs no dump on every push: `make test` with `-Werror` and again under
ASan+UBSan, `make assets-check`, the core cross-compiled for the GBA, and the
trace harness's host build. `make gba-check` and the cartridge comparisons
stay local.

A single check runs on its own: `python3 tools/run_rom.py build/tengen.gba
--pausemenu` (the flags are listed in `tools/run_rom.py --help`). A new
check goes in the `tools/romcheck/` module of its family, gets a flag in
`run_rom.py`, and a line in the Makefile's `gba-check`.

## Traps

Each of these cost a bug before it was known. `reference/HISTORY.md` has the
story behind each; the item number is in brackets.

- **Trace what READS a table, not just the table.** `bonusLinesTable`'s digit
  pairs are hundreds and tens: the first level-up is at 30 lines, not 3. [2]
- **A scroll is one number per background.** BG0 is the grid; BG1 sits three
  pixels across (odd-width text and previews centre on it); BG2, the
  counters, two pixels UP — the only sub-tile vertical nudge, and the
  glyphs have one row of leading, so a lift can close a gap but never open
  one; BG3 is the histogram, and the lifted credit line in the front end.
  Anything whose ink is not centred in its tiles needs its own layer. A box
  drawn over the board is torn down on every layer it used. [21, 28]
- **The build.** `-flto` is what keeps the drawing inside the vertical blank.
  The C is Thumb; code that must be ARM says so per function (`IWRAM_CODE`),
  and `vsync` picks its SWI encoding by `__thumb__`. Mind `.iwram` in
  `arm-none-eabi-objdump -h`: it shares internal WRAM with the stacks. [13]
- **The sound engine is RUN, not reimplemented** (`nes6502.c`). Reset it with
  `$00` before anything else. `MUSIC_SILENCE` frees ONE priority class (7):
  the title and game-over tunes are class 8 and need `LD040` with 8.
  `MUSIC_SUSPEND` gags the whole engine, so anything that leaves a pause
  without `pauseOrUnpause` must send `MUSIC_RESUME`, first in the ring. The
  ring takes one request per frame and drops overflow. [13, 28]
- **One interrupt handler** (`irq_handler`, video.c). It must OR every flag
  into the BIOS's mirror at `$03007FF8` as well as IF, or `VBlankIntrWait`
  never wakes. The cable only switches its own source.
- **Lockstep: anything driven by buttons that crossed the wire happens inside
  the core**, on the frame both consoles agree on — the A+B restart does. The
  one thing lockstep cannot carry is what a person typed: names cross in
  `TengenNameSwap`. The lobby is stop-and-wait; its SKIN stage agrees on a
  skin by a fingerprint of the art. [14, 25, 31]
- **A real cable is not the emulated one.** What two SPs taught us, one
  rule each; `run_link.py`'s cable models every one of them, and each has a
  check that the build before it fails:
  - The interrupt judges a transfer by its WORDS: two slots that are not
    $FFFF (every slot is emptied when a transfer starts). SD and the error
    bit are counted, never acted on: judging by them threw away every
    transfer on two SPs ("G 0 B 999 R 999") while SIOCNT at leisure showed
    neither flag. SD still down when the interrupt reads it is the INFERRED
    cause, not a measured one (`jitter_check`, `Cable.sd_lags`); the
    diagnostic line's IRQ word is there to settle it.
  - Thirty transfers in a row with a slot empty restart the port
    (`sio_reset`, `mute_check`), at most once every two seconds, and
    WITHOUT going through general purpose: a port in general purpose lets go of its lines and the other
    console hears the flutter as empty transfers, so restarts on both sides
    fed each other until two SPs did nothing else ("A 999 R 999", both
    reading themselves a slave at the interrupt, the master's own slot
    empty; inferred, `storm_check`). Nothing touches RCNT once the port is
    in multiplayer mode: a restart, and every `link_init`, goes through
    NORMAL mode on an external clock with SO held high (SIOCNT $0008), a
    real change of mode that lets go of no line. The cable failed when the
    master reached the lobby first and never the other way round. The pump never resets on the error bit, which only
    a transfer rewrites (`glitch_check`). 38400 baud.
  - The master starts a transfer only with SD high, read at leisure.
  - Who is master: the ID bits of the last good transfer (checked against
    the slot this console's word landed in), and before that the SI pin
    read ONLY IDLE and held for ROLE_DEBOUNCE frames (`link_sample_role`).
    A slave's SI is the master's SO, which the master drives LOW for every
    transfer to pass the turn on (GBATEK, Transfer Protocol) and which is
    not driven high when the master's port is out of multiplayer mode; read
    at a random moment, a slave took itself for the master whenever the
    master was already pumping — on two SPs, every time the master reached
    the cable first (`race_check`, `role_check`). A lobby whose role changes
    starts again in the new one (`tengen_lobby_forget`). `link_shutdown`
    leaves the port IN multiplayer mode, answering 0.
  - The send register is written from the main loop only when the port is
    idle, and otherwise by the interrupt as the transfer ends (`tx`,
    `load_send`): with the two consoles' frames in step, a slave found the
    port busy every frame and never answered.
  - Match words carry the mark 110 in their top bits ($C000-$DFFF; the
    frame counter is five bits): the two consoles leave the lobby a
    transfer apart when the transfers fall differently in their frames. A
    lobby at GO takes a match word as the end of the handshake; a console in
    the match takes a lobby word as "not yet" and resends its first move.
    `Cable.slow` makes transfers last to the end of the frame, with the busy
    bit and the slave's SI low meanwhile.
  - The lobby never fails. It waits for a partner as long as it takes (B
    leaves), forgets one that goes quiet for TENGEN_LOBBY_LOST turns and goes
    back to waiting (the master off LEVEL SETTINGS); the slave takes the
    handshake only in order from HELLO and answers anything else with NONE,
    which sends the master back to HELLO (`churn_check`, `late_check`, and
    the host tests).
  - The lobby is per mode: HELLO carries the mode (bit 0, 1 = COOPERATIVE)
    both ways, and a HELLO for the other mode is not an answer, on either
    side. A console on 2 PLAYER and one on COOPERATIVE both wait; the
    master's choice no longer drags the slave into its mode (`mode_check`,
    `test_two_consoles_that_chose_different_modes_never_link`).
  - A match whose cable goes quiet for LINK_LOST_FRAMES waits with LINK
    ISSUES in the rival's cell — tune silenced (MUSIC_SILENCE, not SUSPEND,
    so the chime a frame later is heard) — and picks up where it was. Quiet
    for LINK_GIVEUP_FRAMES, or desynced, it is over: the CABLE LOST window
    in the middle of the screen, and START to the title without the records
    swap or the high scores (`link_wait`, `link_give_up`, `lost_check`).
  The LINK CABLE screen prints the BUILD (the commit, `BUILD_ID` in the
  Makefile — a hardware report is only as good as knowing which ROM), SIOCNT
  now and at the last interrupt, good/error/absent/reset counts and the last
  two words while it waits.
- **Coop's two falling pieces are solid to each other**, and the settled
  field cannot see it: `checkCoopCollision` runs on shifts, rotations and
  gravity. [22]
- **The dancers' dice are shared.** Every dancer given a programme rolls
  them, drawn or not, so cap the cast when the show STARTS. Positions 0-5 are
  the solo column, 6-13 coop's pairs. [8]
- **Under a skin a cell is a PIECE ID** (`piece_id_cells`), not a joined
  tile: no join-breaking on a clear, and TT_WALL is still 15. The prototypes'
  rules (`proto_rules`) come with a skin only alone; over the cable a skin is
  paint. On the slave the board's skin is not the title's (`g_board_skin`).
  [31]
- **Text is ASCII-indexed tiles**, with two exceptions `tilemap_text` maps
  back: `?` lives at `TILES_GAME_QUESTION`, and the pause menu's raised
  heading letters at `PMENU_RAISED_BASE`. [28]
- **Title sprites.** The fireworks have the NES's behind-the-background bit
  (OBJ priority 2 here), which is how the frame contains them; a burst is
  mapped through the composition by its own centre, never per sprite. The
  brick columns are NOT opaque, so window 0 keeps sprites inside the
  release's frame (`title_window`, `--fireworks`). [15]
- **A linked pause is toggled inside the core**, so the music, the repaint
  and the rest of a pause going up or down live in `pause_toggled`, called
  from the solo frame and the linked one alike; the linked one used to skip
  it and a paused coop match played on.
- **The seed advances once per main-loop turn, not per frame**, and a slow
  turn takes two frames. A build that runs faster deals different pieces:
  a harness fixture must force what it needs (`--coopai` clears
  `g_ai_last_piece`) rather than rely on what was dealt.
- **What is on the screen is `field_view`'s board.** A chord pause in a race
  shows the other board, and its NEXT, colours and panels go with it.
- **A game is written to the table as it ENDS** (L81DD at the top-out), so an
  A+B restart keeps it. Each build has its own table; a linked match uses the
  release's. [25]
- **The frame a clear's timer reaches zero also deals.** mainLoop animates
  both players before either plays, so the rows come down and the next
  piece comes up together (29 frames after the lock; 1 in the prototypes).
  In coop, player 1's step finishes a partner's clear that ends this frame.
  It dealt a frame late until a trace first cleared a row.
- **The computer plays at the cartridge's pace.** No soft drop, no pause
  before it moves: TengenAi's `soft_drop` and `settle` stay off in every
  mode. Only `coop_aware` is the port's, and it is behind the chord.
- **The NES pulse sweep is emulated** (`gba/nes_audio.c`, PulseSweep): the
  line clear is a rising sweep on pulse 2, and without it it was one low
  note. A period byte written replaces that byte of the SWEPT period, so the
  6502 core records writes (`apu_written`), not just values.
- **Against the computer there is ONE handicap**, and it buries both
  boards (`bcs @computerIsPlaying`, main.asm.txt:3539). Only 2 PLAYER has
  two.
- **Drawing must end inside the vertical blank.** `--vblank` reads the
  scanline where `draw_match` finishes: a full repaint is the heaviest frame
  (line ~220 of 227). Anything that repaints often — the pause box changing
  width did — puts back only what it uncovered instead.
- **The fall timer ticks before the moves.** L8320 decrements and reloads it,
  then shifts and turns, then drops; a shift the coop partner refuses adds
  its +2 to the timer just reloaded.
- **The cartridge's main loop does not always fit in a frame.** A heavy
  iteration waits for the NMI halfway and finishes in the next frame. Read
  at the NMI it looks a frame late (that was the "coop deals one frame
  later" we once wrote down), so the traces read per iteration.

## Decisions

The port departs from the cartridge on purpose in these places, and does
not show these things on purpose. Anything not on these lists that the
cartridge does and the port does not is a bug. The reasons are in
`reference/HISTORY.md`.

**Added or reshaped by the port**: one settings page instead of four
(level, handicap, music); four HUDs (1 PLAYER: Banner, Stats; VERSUS:
Versus; WITH: Coop; 2 PLAYER: Versus; COOPERATIVE: Coop — and under the
chord Stats as VERSUS's and WITH's second), remembered per mode; no HIGH in a
race, as on the cartridge's 2P screen; one chord (L+R on GAME SELECT or
LEVEL SETTINGS) that uncovers Korobeiniki, Katiuska, MUSIC MIX, the XE
levels 18-19, the pause menu (as wide as the tune's name) and those Stats,
and turns the menus' text from blue to white and the credits from the
cartridge's orange to dark grey to say so; the title's L+R cycling the prototype skins;
credits rotating every four seconds; the cossacks staged on the panels'
ledges where the cartridge uses a middle strip the port does not have; a
second cossack for the rival in HUD VERSUS; a paused race under the chord
showing the rival's board, with their NEXT, colours and numbers; the
computer reading its coop partner under the chord; the pause menu over the
cable, there if the MASTER found the chord and driven by both players'
presses through lockstep (`link_match_begin`); a linked match that waits
ten seconds for a quiet cable (LINK ISSUES) before giving it up (the CABLE
LOST window, and out to the title); the XE mod's two
off-by-one bugs mended (XE only). [8, 11, 17, 19, 20, 23, 28, 30]

**Knowingly not shown**: proto_c's title animation (its rows are the ones
the composition drops); a screen-change noise under proto_a (its dump has
none); the "STATS" heading and the "SCORE" of "HIGH SCORE" (no room);
proto_d as a fourth skin (its title is pixel-identical to proto_c's); a
prototype's rules over the cable; A+B restarting the whole game in 1P and
coop (there A and B are the way out); the line counter's clamp at 10000;
the prototypes' own front-end shape (two modes, their level select, no
handicap or music); the computer sliding a piece under an overhang (tried
twice, measured worse); the demo's own game over and HIGH SCORES page (the
cartridge's demo plays about 25 minutes, tops out, and shows both; the
port's holds its GAME OVER three seconds and goes back to the title, and
writes no score nobody played for); the demo's seed (the cartridge's
depends on how many times its main loop spun on the title).

**Looks like a bug, is the cartridge's**: the one-pixel gap between the left
panel's shelves and the rope — the wall tile `$6A` has a blank first column.

## Open

What has been checked only against a READING of the disassembly (host tests,
the harness) and never against the cartridge itself, which is where the
last timing bugs were found every time:

Nothing. Traced or measured since: the race and the computer
(`MODE=versus`/`with`), the handicap (`--handicap N`, all modes), the
attract demo (`--demo`), and all four prototypes' clear, level-up and pause
(`tools/probes/proto_rules.py` — proto_a draws its falling piece into the
field and walls with 8, so it is measured by letting the piece complete the
row itself: gone one frame after the lock, as in B, C and D).

And what has run only in an emulator:

- **The link cable** between two real GBAs. Tried on two SPs (EZ-Flash IV
  and SuperCard) since build 3cdac97, a round of fixes each time; see the
  trap above for what each taught.
- **The latest builds on hardware** — the Thumb code, `VBlankIntrWait`, the
  interrupt handler. mGBA is accurate on all three, but it is not the
  console.

A new idea starts in the cartridge (`tools/nes_console.py`,
`tools/render_nes.py`), not in memory of how Tetris goes.
