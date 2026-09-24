#!/usr/bin/env python3
"""
run_rom.py — boot the built ROM in mGBA, drive it for a while, and report
what actually ended up on screen.

This exists because "the ROM links" and "the ROM runs" are very different
claims, and the second one is the interesting one. It runs headless, so it
works in CI or over a terminal with no display.

Usage:
    python3 tools/run_rom.py build/tengen.gba [--frames N] [--png OUT.png]
    python3 tools/run_rom.py build/tengen.gba --selftest
    python3 tools/run_rom.py build/tengen.gba --lineclear
    python3 tools/run_rom.py build/tengen.gba --pause
    python3 tools/run_rom.py build/tengen.gba --audio
    python3 tools/run_rom.py build/tengen.gba --link

--selftest checks the things a broken port would get wrong: that the screen
isn't blank, that the playfield frame is where the resolution mapping says it
should be, and that a piece actually falls.

--lineclear watches the line-clear animation happen, sprite by sprite and
tile by tile. --pause pauses the game and types in the cheat codes. Both need
`build/tengen.elf` next to the ROM (for the address of the game state) and an
`arm-none-eabi-nm` to read it with.

--link walks the two-player mode as far as one console alone can go: to the
link screen, with nothing plugged in. A linked MATCH needs two consoles, and
that is tools/run_link.py, which puts a simulated cable between two cores.

Requires: pip install pygba  (pulls in the mGBA bindings)
          plus the mGBA shared library, e.g. apt-get install libmgba0.10
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# The checks live in tools/romcheck/, one module per family (see its
# __init__.py). Everything they define stays importable from HERE as well,
# because run_link.py, the probes and scripts written against this file have
# always said `run_rom.load`, `run_rom.tilemap_text`, `run_rom.KEYS` and so
# on.
from romcheck.harness import *    # noqa: F401,F403
from romcheck.play import *       # noqa: F401,F403
from romcheck.audio import *      # noqa: F401,F403
from romcheck.pausemenu import *  # noqa: F401,F403
from romcheck.frontend import *   # noqa: F401,F403
from romcheck.hud import *        # noqa: F401,F403
from romcheck.ai import *         # noqa: F401,F403
from romcheck.records import *    # noqa: F401,F403

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--png")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--lineclear", action="store_true",
                     help="watch the line-clear animation, for 1 row and for 4")
    ap.add_argument("--pause", action="store_true",
                     help="check Start pauses and the cheat codes respond")
    ap.add_argument("--audio", action="store_true",
                     help="check the emulated sound engine against its golden recording")
    ap.add_argument("--pause-audio", action="store_true",
                     help="check that pausing actually silences the sound")
    ap.add_argument("--link", action="store_true",
                     help="check the 2-player front end with no cable attached")
    ap.add_argument("--title", action="store_true",
                     help="check the cathedral overlay and the fireworks")
    ap.add_argument("--skin", action="store_true",
                     help="check the L/R title-skin easter egg")
    ap.add_argument("--credits", action="store_true",
                     help="check the cartridge's six front-end credit lines")
    ap.add_argument("--xe", action="store_true",
                     help="check the Tetris Tengen XE level range behind the chord")
    ap.add_argument("--handtunes", "--korobeiniki", action="store_true",
                     help="check the two hidden hand-entered tunes")
    ap.add_argument("--leave-title", action="store_true",
                     help="check the title leaves no sprites or music behind")
    ap.add_argument("--braid", action="store_true",
                     help="check the braid keeps its weave in both HUD modes")
    ap.add_argument("--handicap", action="store_true",
                     help="check the starting handicap reaches the playfield")
    ap.add_argument("--panel", action="store_true",
                     help="check the four counters share one headroom")
    ap.add_argument("--next-palette", action="store_true",
                     help="check NEXT wears the next piece's colours")
    ap.add_argument("--menu", action="store_true",
                     help="check LEVEL SETTINGS fits inside its frame")
    ap.add_argument("--computer", action="store_true",
                     help="check the COMPUTER player plays VERSUS and WITH")
    ap.add_argument("--coopai", action="store_true",
                     help="check the computer reads its partner under the chord")
    ap.add_argument("--vblank", action="store_true",
                     help="check each frame's drawing ends inside the vertical blank")
    ap.add_argument("--fireworks", action="store_true",
                     help="check no title sprite shows over the brick columns")
    ap.add_argument("--idleblink", action="store_true",
                     help="check the HUD Stats cossack stays up as a clear ends")
    ap.add_argument("--proto-pause", action="store_true",
                     help="check only proto_a's pause leaves the tune playing")
    ap.add_argument("--sweep", action="store_true",
                     help="check the line clear's rising pulse sweep")
    ap.add_argument("--sleep", action="store_true",
                     help="check vsync halts the CPU instead of spinning")
    ap.add_argument("--statsshow", action="store_true",
                     help="check HUD Stats's level-up troupe, in the left box")
    ap.add_argument("--versushud", action="store_true",
                     help="check the race's third HUD and its paused view")
    ap.add_argument("--demo", action="store_true",
                     help="check the title starts playing by itself")
    ap.add_argument("--loans", action="store_true",
                    help="check the palette banks a skin borrows come back")
    ap.add_argument("--onscreen", action="store_true",
                    help="check what the tile map promises reaches the screen")
    ap.add_argument("--effects", action="store_true",
                    help="check each build's menu noises are its own")
    ap.add_argument("--stats", action="store_true",
                    help="check the histogram and the GAME OVER plaque follow "
                         "the skin")
    ap.add_argument("--gameover", action="store_true",
                     help="check every mode ends and lets go of the player")
    ap.add_argument("--counters", action="store_true",
                     help="check the counters blank their leading zeros")
    ap.add_argument("--pausemenu", action="store_true",
                     help="check the L+R pause menu")
    ap.add_argument("--quit-audio", action="store_true",
                     help="check quitting from the pause menu leaves the sound alive")
    ap.add_argument("--leaderboard", action="store_true",
                     help="check the HIGH SCORES table and its initials")
    ap.add_argument("--tables", action="store_true",
                     help="check each build keeps its own HIGH SCORES table")
    ap.add_argument("--coophud", action="store_true",
                     help="check the coop HUD gives each player a panel")
    ap.add_argument("--points", action="store_true",
                     help="check the drop-point sprites beside the piece")
    ap.add_argument("--falling", action="store_true",
                     help="check both pieces of a coop board are drawn")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest(args.rom))
    if args.lineclear:
        sys.exit(lineclear_check(args.rom, 1) or lineclear_check(args.rom, 4))
    if args.pause:
        sys.exit(pause_check(args.rom))
    if args.audio:
        sys.exit(audio_check(args.rom))
    if args.pause_audio:
        sys.exit(pause_audio_check(args.rom))
    if args.link:
        sys.exit(link_check(args.rom))
    if args.title:
        sys.exit(title_check(args.rom))
    if args.skin:
        sys.exit(skin_check(args.rom))
    if args.credits:
        sys.exit(credits_check(args.rom))
    if args.xe:
        sys.exit(xe_check(args.rom))
    if args.handtunes:
        sys.exit(handtunes_check(args.rom))
    if args.leave_title:
        sys.exit(leaving_title_check(args.rom))
    if args.braid:
        sys.exit(braid_check(args.rom))
    if args.handicap:
        sys.exit(handicap_check(args.rom))
    if args.panel:
        sys.exit(panel_check(args.rom))
    if args.next_palette:
        sys.exit(next_palette_check(args.rom))
    if args.menu:
        sys.exit(menu_check(args.rom))
    if args.computer:
        sys.exit(computer_check(args.rom))
    if args.coopai:
        sys.exit(coop_ai_check(args.rom))
    if args.versushud:
        sys.exit(versus_hud_check(args.rom))
    if args.statsshow:
        sys.exit(stats_show_check(args.rom))
    if args.sleep:
        sys.exit(sleep_check(args.rom))
    if args.sweep:
        sys.exit(sweep_check(args.rom))
    if args.vblank:
        sys.exit(vblank_check(args.rom))
    if args.fireworks:
        sys.exit(fireworks_check(args.rom))
    if args.idleblink:
        sys.exit(idle_blink_check(args.rom))
    if args.proto_pause:
        sys.exit(proto_pause_check(args.rom))
    if args.demo:
        sys.exit(demo_check(args.rom))
    if args.gameover:
        sys.exit(gameover_check(args.rom))
    if args.falling:
        sys.exit(falling_piece_check(args.rom))
    if args.points:
        sys.exit(points_check(args.rom))
    if args.leaderboard:
        sys.exit(leaderboard_check(args.rom))
    if args.tables:
        sys.exit(tables_check(args.rom))
    if args.coophud:
        sys.exit(coop_hud_check(args.rom))
    if args.quit_audio:
        sys.exit(quit_audio_check(args.rom))
    if args.loans:
        sys.exit(loans_check(args.rom))
    if args.onscreen:
        sys.exit(onscreen_check(args.rom))
    if args.effects:
        sys.exit(effects_check(args.rom))
    if args.stats:
        sys.exit(stats_check(args.rom))
    if args.pausemenu:
        sys.exit(pausemenu_check(args.rom))
    if args.counters:
        sys.exit(counters_check(args.rom))

    core, screen = load(args.rom)
    start_game(core)
    run(core, args.frames)
    describe(pixels(screen))
    if args.png:
        with open(args.png, "wb") as fh:
            screen.save_png(fh)
        print(f"guardado {args.png}")


if __name__ == "__main__":
    main()
