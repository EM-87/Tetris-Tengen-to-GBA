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
  `link.c` (the cable), `suspend.c` (the paused game on the battery),
  `splash.c` (the logos at power-on), `gbp.c` (the Game Boy Player) and
  `wireless.c` (the Wireless Adapter). A symbol is shared only if another file names it
  in code; everything else is `static`.
- **`tools/`** — `extract_assets.py` (all the art, from a dump);
  `run_rom.py` (the command `make gba-check` runs) with its checks in
  `tools/romcheck/`, one module per family and `harness.py` for what they
  share; `run_link.py` (two cores and a cable); `run_wireless.py` (two cores, two
  stand-in Wireless Adapters and the air between them); `nes_cpu.py` /
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
| `make gba-check` | headers | before calling any change done: 66 checks on the running ROM in mGBA, eighteen of them on two consoles with a cable, and then the Wireless Adapter's (`run_wireless.py`) |
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
- **The Single-Pak slave is the same program, linked for EWRAM**
  (`gba/mb.ld`, `-DTENGEN_MULTIBOOT`, crt0's multiboot entries under
  MULTIBOOT) and carried inside the cartridge's ROM (`gba/mb_image.s`),
  which is why the ROM is 512 KB. Image plus the 6502's 64 KB must fit in
  256 KB and the link asserts it. The slave boots into the lobby following
  the master's mode (`tengen_lobby_mode_any`) and never touches save
  memory (a console booted into multiboot can have another game's
  cartridge in). What eleven rounds on two SPs taught, one line each:
  - **ON THAT HARDWARE A MULTIBOOTED IMAGE CANNOT TAKE AN INTERRUPT.** Not
    the game, not a probe of twenty instructions with a handler of its
    own: the first vertical blank taken never reached the handler (the
    CPU was in system mode, as GBATEK says; internal WRAM intact; why is
    NOT known). So the slave never sets IME (`IME_ON` is 0 there) and
    vsync() polls IF for the vertical blank and the end of a transfer and
    serves each as the handler would (`poll_interrupts`, video.c). The
    master starts one transfer a frame, so that is soon enough.
  - **The sending is all software** (`link_multiboot_send`): the BIOS's
    handshake, then GBATEK's SWI $25 pseudo-code (length, seed, each word
    encrypted in two halves, the checksum), as gba-link-connection's
    Async sender does it. Not SWI $25 itself: it keeps the CPU for the
    whole transfer (no progress bar) and nothing the BIOS does behind the
    slave's back is taken on trust. The slave can send too: ITSELF, from
    EWRAM (`single_pak_image`), so the copy makes copies.
  - crt0 switches interrupts off, stops the four DMA channels and timers
    and sets the supervisor's stack before anything else, and irq_init
    sets IE outright; none of it was the cause above, all of it is what a
    start after somebody else's code should do.
  - mGBA takes a multiboot image whose $C0 branch is exactly 28 bytes for
    a cartridge: hence the spare word after the $E0 entry.
  - The sender mutes the PSG from its first try to the end of the send —
    once: muting and unmuting around every try clicked in the speaker at
    two to four hertz while nobody answered — and waits a second after,
    before its lobby talks over the new console's start.
  - **The LINK CABLE screen says what is on the other end** (`link_peer`).
    The port rests in multiplayer mode from boot (`link_rest`), so one of
    ours anywhere in the game reads as "someone" (wait for its player). A
    console with no cartridge cannot be heard at the lobby's 38400 baud:
    the master asks for it with the send's first word, $6200 at 115200,
    every sixteenth frame while the lobby is unlinked (`link_probe`), and
    offers SELECT only when $720x comes back (INFERRED from GBATEK's
    table and gba-link-connection; the stand-in BIOS answers so). The
    probe's interrupt is kept out of IE as well as SIOCNT — the emulated
    cable raises IF regardless, and the master's lobby ate its own probe
    — and $6200 is tag GO with a payload, which a slave of ours once took
    for a GO and walked into the match alone: our GO is always empty, so
    a GO with something in it is ignored (`tengen_lobby_apply`). Nobody
    answering: on the master's end, switch the other one on; on the
    other, no transfers is as much no cable as a master in its menu, and
    the screen asks for both — or did: a console of ours in its menus now
    says it is there (`link_beacon`, a transfer of 0 every eighth frame from
    the master's end with the port at rest), so the slave's end waits for
    its player and asks for the cable only when nothing is on it. A console
    with no cartridge gets the game by itself three-quarters of a second
    after it is found (`AUTO_SEND_FRAMES`); B on the sending screen stops it
    and leaves SELECT until another comes. The stand-in BIOS answers as the
    game at rest once it has it, as the console would.
  - `poll_interrupts` serves only what IE has switched on, as the
    interrupt would: a transfer pending as the slave left the lobby was
    served from the menu, putting the lobby's word back on the wire.
  - **Open: two copies that both leave the lobby for the menu and come
    back never find each other** (after a match they do). The emulator
    does not do it in any order or timing tried. Two guesses went in:
    the IE mask above, and a slave that hears nothing for three seconds
    restarting its port through normal mode (`LINK_QUIET_RESTART`), as
    re-plugging would. What the LINK CABLE screen says on each console
    when it happens is the next clue.
  - A probe as small as the BIOS allows (784 bytes) never started on the
    SPs; one of the game's size did. Not followed up.
  `singlepak_check` plays the cartridge against the polling slave byte for
  byte; `mb_send_check` sends to a stand-in BIOS that decrypts what it
  gets: the image must come out identical, from the cartridge and from the
  copy.
- **The paused game on the battery** (`suspend.c`, SRAM from $4000, above
  the high scores) is the core's `TengenGame` and the computer's
  `TengenAi` copied whole, with what the menus chose. Structures copied
  whole are only trusted by the build that wrote them: the record carries a
  hash of that build's date and time, and another build ignores it. It is
  put back by starting the match the way LEVEL SETTINGS does (`resuming`
  confirms it) and copying the game over the one just dealt, so everything
  a match lays out is laid out. Written as the plaque goes up and when its
  tune changes; wiped off the plaque and by the soft reset.
- **The Game Boy Player is found by its logo** (`gbp.c`), shown at power-on
  before the publisher's on every GBA, as the games that supported it did:
  while it is up a Player holds all four directions one frame in three
  (KEYINPUT 030Fh, GBATEK), and only a Player can. The logo is Nintendo's,
  generated from a picture you supply (`tools/make_gbp_logo.py`, a
  lossless 240x160 capture of a game that shows it), shown in its exact
  colours — not through `kLcdGamma` — as a Mode 4 bitmap; GBATEK says
  tiles or bitmap alike. Found, the serial port is the Player's: 32-bit
  normal mode on its clock, answering GBATEK's NINTENDO handshake row by
  row (`tengen_gbp_reply`, host-tested) and then 400000yyh, rumble on or
  off — a clear, longer for more rows, the level and the top-out
  (`rumble_step`). The cable takes the port in `link_init` and gives it
  back in `link_shutdown`. The result survives a soft reset with the
  splash's word. On a Player, LEVEL SETTINGS is the cartridge's three
  pages, LEVEL, HANDICAP and MUSIC, each a column with the arrow beside
  the choice (`draw_tv_page`, `tv_page` in main.c; `tv_menu_check`): the
  shape for a television across a room that the one page gave up for a
  screen at arm's length (NOTES.md, LEVEL SETTINGS). Run only against a stand-in (`gbp_check`, `FakePlayer`):
  the mGBA the checks use does not emulate a Player.
- **The Wireless Adapter is the cable's understudy** (`wireless.c`): asked
  once at power-on (`wireless_detect`, the NINTENDO login; not on the
  Single-Pak copy), and if it answers, `link_init`, `link_pump` and the rest
  run it instead of the cable, so the lobby, the lockstep and the records
  swap above link.c do not change. Each console searches for a room a
  random while and hosts one if it finds none (the host is the master);
  the lobby and the swap are stop-and-wait over a sequence number, each
  answered number one "transfer" (`link_push_pair`) the same on both; the
  match is lockstep by frame with WL_DELAY frames of input delay, each
  packet carrying the frames the other side has not played yet, from the
  oldest (a window that started at the newest lost a frame for good and the
  guest never played one). Every wait is on the scanline counter and no
  interrupt is used. Written from GBATEK and gba-link-connection, and run
  only against `FakeAdapter` (`run_wireless.py`), which is the same
  documentation as a model: INFERRED until two real adapters agree. Booting
  takes a little longer for the login, so the seed and the deal move — a
  check that read a board once at the end failed for that (`--computer`
  watches it now).
- **The logo at power-on skips itself after a soft reset** by a word in
  external WRAM that crt0 does not clear (`g_splash_seen`). The checks skip
  it the same way, writing that word after every reset
  (`romcheck/harness.py`), because every check was written against a
  console that reaches the title at once; `suspend_check` is the one that
  looks at it.
- **Every colour goes through the screen's curve** (`kLcdGamma`,
  `lcd_colour` in gba/palette.h) on its way to palette memory, and today
  that curve is the identity. A brightening one (x^(1/1.6), build 822ac6c)
  washed everything out on the backlit SP: emulators DARKEN to imitate the
  unlit panels (an LCD gamma of about 4 against a monitor's 2.2), games of
  the day drew bright art to make up for them, and the NES palette was
  made for a television. The table is the one place to put a curve back.
  A check that names a colour passes the NES one through the same table
  (`on_screen` in romcheck/harness.py: keep the two in step).
- **Sleep waits for the keys to settle** (`wait_keys_settled`): every key
  up for four frames, both before the Stop and after the wake, and the PSG
  silenced before that wait, not after (a note held through it and cut
  was a glitch in the speaker). A, B, START or SELECT wakes it, and one bounce of SELECT on the way in woke it on the spot —
  on two SPs it took up to four tries. A solo match remembers that it is
  to sleep on the frame it pauses (`sleep_next`), so quick hands do not
  only pause it.
- **The soft reset comes back into crt0 past the header** (`_restart`,
  `soft_reset_check`, video.c), after stopping the DMA, timers and sound and
  emptying the video memory by hand. NOT through $08000000, where a flash
  cart may have put its own start-up code: on an EZ-Flash IV a jump there
  by hand came to a black screen, and so did the BIOS's SoftReset, which
  goes there too (INFERRED: the cart's code at the top of the ROM). The
  Single-Pak copy's `_restart` is in external WRAM, where it already is.
  It leaves the serial port alone for the same reason sio_reset does, and
  drops the paused game kept on the battery. **On an EZ-Flash IV it never
  runs:** with a probe that turned the screen magenta as the very first
  thing the restart did (build c81cb67), the screen went black and never
  magenta — so the cart takes A+B+START+SELECT for itself (INFERRED: its
  own soft-reset hotkey, back to its menu) before the game sees it. Not
  the game's to fix; the cart's option is.
- **EXIT on the pause menu leaves with the game still paused and the match
  still "running" for that frame** (`quit_match`; the way out is further
  down the loop). Anything that asks "is a paused game on the plaque?"
  has to ask about `quit_match` too: the battery's copy survived EXIT
  until it did (`pausemenu_check`).
- **A real cable is not the emulated one.** What two SPs taught us, one
  rule each; `run_link.py`'s cable models every one of them, and each has a
  check that the build before it fails:
  - The interrupt judges a transfer by its WORDS: two slots that are not
    $FFFF (every slot is emptied when a transfer starts). SD and the error
    bit are never acted on: judging by them threw away every
    transfer on two SPs ("G 0 B 999 R 999") while SIOCNT at leisure showed
    neither flag. SD still down when the interrupt reads it is the INFERRED
    cause, not a measured one (`jitter_check`, `Cable.sd_lags`).
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
  While the cable was being made to work, the LINK CABLE screen printed the
  build and the cable's own account (SIOCNT now and at the last interrupt,
  good/error/absent/reset counts, the last two words: `draw_link_debug` and
  `link_debug`, in the history before PR #26). Both are gone; bring them
  back from there if a console ever needs reading again. The ROM file's
  name carries the commit.
- **A coop panel stands on its player's side of the board**: player 1's
  (NEXT, score, lines) on the left, where player 1's pieces come in, on
  both consoles. "Yours on the left" put every preview over the other half
  of the board on the cable's second console, and two players kept losing
  track of whose piece was whose (`coop_check`).
- **The cold table runs 17000 down to 3000, and Tengen pays little**: a
  piece is worth (level+1) x (level+1 + rows above the floor), so a game
  that reaches level 2 is a few hundred to a couple of thousand points and
  does not make it. A report of "scores missing from the table" is this
  first; read the score before reading the code.
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
- **A linked match has no level-up show**, and the show is what brings a
  hand-entered tune back after the intro; the cartridge's own engine picks
  its tunes back up by itself. So a linked level-up asks for Korobeiniki,
  Katiuska or MUSIC MIX's next turn again when the intro ends
  (`g_levelup_resume`, `levelup_music_check`).
- **Over the cable the pause menu's tune list is the master's**, the full
  one (it is there because the master found the chord), never this
  console's own: counted from each console's chord, the two stepped through
  lists of different lengths and played different tunes
  (`menu_list_check`).
- **A linked pause is toggled inside the core**, so the music, the repaint
  and the rest of a pause going up or down live in `pause_toggled`, called
  from the solo frame and the linked one alike; the linked one used to skip
  it and a paused coop match played on.
- **The seed advances once per main-loop turn, not per frame**, and a slow
  turn takes two frames. A build that runs faster deals different pieces:
  a harness fixture must force what it needs (`--coopai` clears
  `g_ai_last_piece`) rather than rely on what was dealt.
- **What is on the screen is `field_view`'s board.** A chord pause in a race
  shows the other board, and its NEXT, colours and panels go with it — but
  not over the cable, where the other board is on the other console.
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
  two — and VERSUS COMPUTER behind the chord, the second for the
  computer's board (the port's; `handicap_cpu_check`).
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
showing the rival's board (not over the cable), with their NEXT, colours and numbers; the
computer reading its coop partner under the chord; a handicap of its own
for the computer in VERSUS COMPUTER under the chord; the pause menu over the
cable, there if the MASTER found the chord and driven by both players'
presses through lockstep (`link_match_begin`); a linked match that waits
ten seconds for a quiet cable (LINK ISSUES) before giving it up (the CABLE
LOST window, and out to the title); the XE mod's two
off-by-one bugs mended (XE only); Single-Pak (SELECT on the LINK CABLE
screen sends the game to a console with no cartridge); sleep (L+R+SELECT
anywhere but a match in play — a solo match pauses first and sleeps on its
plaque, a linked one not at all; A, B, START or SELECT wakes it) and soft
reset (A+B+START+SELECT), as commercial games had; a paused solo game kept
on the battery through a power cycle, as Tetris DX does (`suspend.c`); the
publisher's logo on white at power-on (`splash.c`, from an image you supply
through `tools/make_splash.py`, gitignored like the cartridge's art); every
colour through one table for the GBA's LCD (`kLcdGamma`, the identity
today); the Game Boy Player's logo at power-on, its rumble, and on it the
cartridge's three settings pages instead of the one (`gbp.c`,
`draw_tv_page`); 2 PLAYER and COOPERATIVE over the Wireless Adapter, found by
itself at power-on, the LINK CABLE screen saying WIRELESS (`wireless.c`);
"EXIT GAME", not "EXIT", on the pause menu (a player took it for closing the
menu); the high scores erased by L+R+B held at power-on, asked twice with NO
chosen (`erase_records_prompt`).
[8, 11, 17, 19, 20, 23, 28, 30]

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

- **Single-Pak** worked on two SPs with the cartridge sending through the
  BIOS's SWI $25 and the slave polling (build bb77e71): a coop match, the
  skins and the chord; the software sender, its bar and the copy sending
  itself on the SPs too (build bccaf9d). The block bar, the steady mute,
  the cable-state screen and the probe behind it have run only in mGBA
  (`peer_check`).
- **The sleep** has run only in mGBA, whose stand-in BIOS has no Stop:
  `system_check` checks the way in and out, the console the rest — and
  that a Stop entered with IME off (the Single-Pak copy) still wakes on
  the keypad is INFERRED.
- **The Game Boy Player** has run only against `FakePlayer`: the logo,
  the 030Fh answer, GBATEK's handshake and the rumble.
- **The Wireless Adapter** has run only against `FakeAdapter`: the login,
  the commands and their handshake, the rooms, the data and both modes.
- **The paused game through a power cycle and the logo** have run only in
  mGBA (`suspend_check`). On a flash cart the save memory has to reach the
  card for the game to survive: the EZ-Flash IV and the SuperCard each do
  that their own way, as they do for the high scores.

A new idea starts in the cartridge (`tools/nes_console.py`,
`tools/render_nes.py`), not in memory of how Tetris goes.
