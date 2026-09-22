/*
 * port.h -- what the five files of the GBA front end share.
 *
 * gba/main.c was one file of six thousand lines for a long time and it
 * had stopped being readable: the layout constants, the drawing, the
 * menus and the match were all in it, and finding any of them meant
 * knowing roughly how far down it lived. It is five now --
 *
 *   video.c     the hardware: backgrounds, palettes, tiles, sprites,
 *               the skins, and the handful of primitives everything
 *               else draws through
 *   hud.c       what a MATCH looks like: the panels, the counters, the
 *               board, the dancers, the high-score table, the plaques
 *   frontend.c  the screens before and around it: the title, the menus,
 *               the credits, the lobby
 *   match.c     a frame of play: the pause menu, the computer, the
 *               order the drawing happens in
 *   main.c      the state machine that walks between them
 *
 * -- and this header is the seam. It holds the LAYOUT: every constant
 * that says where something is on the screen, which palette bank it is
 * drawn in and which of the four backgrounds it rides, because those
 * are facts about the port rather than about any one file. The
 * declarations at the foot are the symbols that really do cross between
 * the five; anything used in only one file stays static where it is
 * defined, which is most of them.
 */
#ifndef PORT_H
#define PORT_H

/*
 * main.c — GBA front end for the verified Tengen Tetris core.
 *
 * All the game rules live in ../src/tengen_core.c and know nothing about the
 * GBA. This file reads the keypad into TengenButton bits, calls tengen_step
 * once per frame, and draws. Keeping the split that clean is what lets
 * `make test` verify the rules on the host.
 *
 * GRAPHICS: every tile, palette and layout here comes from the original
 * cartridge, converted by ../tools/extract_assets.py. Nothing is redrawn.
 *
 * SCREEN LAYOUT — the whole project rests on this fitting:
 *
 *   The NES screen is 32x30 tiles; the GBA is 30x20. The playfield is 10
 *   tiles wide by 20 tall and the cartridge's braided frame adds one 2-tile
 *   column on each side, so the framed board is 112x160 px — exactly the
 *   GBA's full height. It maps tile-for-tile with no scaling, no cropping and
 *   no change of column, and the adaptation is entirely in what surrounds it:
 *
 *     horizontally: 32 columns -> 30, and the cartridge's columns are also
 *       RESEQUENCED. The NES screen is really TWO framed board areas side by
 *       side (2P uses both; 1P covers the second with its score panel), which
 *       leaves the 1P playfield well left of centre — fine on a screen that
 *       wide, lopsided on a GBA showing one player. So the runs of columns are
 *       reordered to put the playfield dead centre with the HUD split either
 *       side. Every run moves whole; nothing is scaled. Done in
 *       extract_assets.py, which lists the order.
 *
 *     vertically: 30 rows -> 20, and the field needs all 20. So the NES's
 *       header strip — the SCORE / LINES / LEVEL / NEXT labels that sit above
 *       the field — moves into the side panel, which the NES left mostly
 *       empty. Same tiles, same lettering, stacked instead of spread. That is
 *       the one deliberate rearrangement, and it is why the port keeps the
 *       original's look rather than substituting its own.
 */
#include <stddef.h>

#include "gba_hw.h"
#include "palette.h"
#include "nes_audio.h"
#include "handtunes.h"
#include "audio_prg.h"
#include "link.h"
#include "../src/tengen_core.h"
#include "../src/tengen_link.h"
#include "../src/tengen_ai.h"

/* Generated from a cartridge dump by tools/extract_assets.py. */
#include "tiles_game.h"
#include "tiles_dancers.h"
#include "tiles_title_obj.h"
#include "tiles_title.h"
#include "screen_1p.h"
#include "screen_coop.h"
#include "screen_leaderboard.h"
#include "screen_title.h"
#include "screen_proto.h"
#include "screen_menu.h"
#include "palettes_rom.h"
#include "dancer_poses.h"

#define CHARBLOCK   0
#define SCREENBLOCK 28  /* 28 * 2KB = 56KB in, clear of the 8KB of tile data */
/* A SECOND MAP, FOR THREE PIXELS. The piece statistics are a seven-tile strip
 * in an eight-column box, so on the tile grid they can only ever sit two
 * pixels from one wall and eight from the other — which is exactly the "algo
 * descentradas a la izquierda" you can see. Redrawing the strip three pixels
 * across is not an option: it is one interlocked picture whose tiles carry
 * three different palettes, and the bars above it are dynamic, so the shift
 * would have to fuse two neighbouring bars into every tile. Giving the block
 * its own background and scrolling THAT by three pixels costs one screenblock
 * and moves icons and bars together, with the cartridge's art untouched. */
#define SCREENBLOCK_STATS 29
#define STATS_SHIFT_PX 3

/* A THIRD MAP, FOR TWO PIXELS — the counters' own.
 *
 * SCORE sat one pixel under the braid while LINES, LEVEL and HIGH each had
 * three under their rule: a rule tile carries two blank pixels below its bar
 * and the label glyphs one above their ink, and the box's top braid gives
 * neither. "Baja todo un pixel para que Score tenga algo de espacio por
 * arriba" is exactly right, and two is what makes the four gaps equal.
 *
 * It cannot be done by moving anything. The counters share BG0 with the
 * PLAYFIELD, and the playfield's 160 pixels are the whole of the screen's
 * height with nothing to give at either end — scrolling BG0 would crop the
 * board, which is the one thing this port does not do. The box has no spare
 * tile row either. So the counters get a layer of their own, for the same
 * reason and at the same price as the statistics got theirs. The braid stays
 * on BG0 and does not move.
 *
 * TWO PIXELS **UP**, AND IT USED TO BE TWO DOWN. That was right while every
 * counter hung under something: SCORE's ceiling was the braid and the other
 * three had a rule, and two pixels down was what gave SCORE the same headroom
 * as the rest. It is wrong now that they sit BETWEEN shelves, which is what
 * the coop panel's compartments are: a label over a value is fifteen pixels
 * of ink and the space between two shelves is twenty, so the block wants two
 * and a half pixels at each end — and two pixels DOWN gave it six above and
 * MINUS ONE below, which is a value whose last row of pixels is drawn on the
 * shelf under it. Two up leaves two above and three below, which is the
 * nearest a whole pixel gets to the middle. */
#define SCREENBLOCK_PANEL 30
#define PANEL_SHIFT_PX (-2)

/* A FOURTH MAP, so the histogram can stay where it was.
 *
 * The counters' two pixels are the panel's, and the offset layer had to ride
 * down with them or the NEXT preview would have come apart — its label sits
 * on the panel's map and its odd-width pieces on the offset one. But the
 * PIECE HISTOGRAM is on that same offset layer, for its own three horizontal
 * pixels, and it did not want the two vertical ones: the icons stand on the
 * box's last interior row, so two pixels down put the tall I against the
 * braid with nothing between them.
 *
 * A scroll is one number for a whole background, so the only way to give the
 * histogram three pixels across and none down is to give it a background.
 * Screenblock 31 is the last one before the sprite tiles and nothing else
 * wanted it.
 *
 * Having its own scroll, it may as well use it: the icons fill their two
 * tiles to the last pixel, so even level with the grid they end one line
 * short of the braid. Two pixels UP gives them the three the rest of the HUD
 * has — the same air the counters keep above their rules. */
#define SCREENBLOCK_HISTOGRAM 31
#define STATS_LIFT_PX 2

/* THE SAME LAYER, LENT TO THE TITLE, and the two words want different things.
 *
 * Nothing on this screen is centred where the tile grid says it is. Measured
 * on the built ROM, as centres of MASS — ink weighted by pixel, which is what
 * an eye reads — against a frame whose interior runs 32..207 and is therefore
 * centred on 119.5:
 *
 *     cathedral 124.0    its own art leans right, and it is the picture
 *     TENGEN    119.9    the cartridge's lettering, unshifted
 *     TETRIS    118.2    likewise
 *
 * So the words are not measured against the frame at all: they are measured
 * against the cathedral, which sits four and a half pixels right of the
 * middle of its own frame. That is why TENGEN kept reading left however
 * carefully the columns were squared up.
 *
 * TENGEN goes on the offset layer at four pixels, which puts its mass at
 * 123.9 — the cathedral's. It is the only thing on that layer here, so the
 * scroll is simply set to what it needs and goes back to STATS_SHIFT_PX on
 * the way into a game.
 *
 * TETRIS stays where the cartridge draws it, and that is deliberate. The
 * spire's finial is printed into the gap between its third and fourth letters
 * and the rest of the spire is down in the cathedral, so the letters and the
 * pole have to agree: with the logo unshifted the gap runs 120-129 and the
 * pole stands at 123, half a pixel off its middle. Shifting the logo two
 * pixels right — as the layer once did to both words — takes the gap with it
 * and leaves the pole leaning against the T. A logo two pixels left of centre
 * that its own spire comes cleanly out of beats a centred one that it does
 * not. */
#define TITLE_LOGO_SHIFT_PX 4
#define TITLE_LOGO_TY0 2         /* TENGEN, and TENGEN only */
#define TITLE_LOGO_TY1 3
/* THE PICTURE ONLY. The frame is in these rows too and it must not move: four
 * pixels of braid sliding out from under the ingots is a great deal more
 * visible than four pixels of lettering ever were. */
#define TITLE_LOGO_TX0 4
#define TITLE_LOGO_TX1 25


#define MAP_W 32
#define SCREEN_TW 30
#define SCREEN_TH 20

/* Which 20 of the layout's 30 rows the GBA shows: the playfield's own rows.
 * Everything the NES drew above and below them is what got relocated. */
#define WINDOW_TOP SCREEN_1P_FIELD_TY

/* WHERE THE FIELD GOES, and why it is 10 columns and not 12.
 *
 * The cartridge's own screen frames the playfield with two columns of braid
 * on each side — tiles $6A $6B at columns 0-1 and $73 $74 at 12-13 — and the
 * ten columns between them are what the game plays in. Those frames are part
 * of the screen art, drawn once; the ROM never paints its playfield buffer's
 * wall cells over them. Three things confirm the columns: the nametable has
 * exactly ten blank columns there, the line-clear sweep runs from x $10 to
 * $58 (columns 2 to 11 and no further), and the pause plaque is blitted at
 * nametable column 12, right where the frame starts.
 *
 * So this draws the ten PLAYABLE cells — storage columns 1..10 of the core's
 * 12 — and leaves the frame alone. Drawing the core's wall sentinels as block
 * tiles instead would both cover the cartridge's art and shift the field a
 * column, which is exactly what an earlier pass here did. */
/* COOP IS TWELVE COLUMNS AND HAS A SCREEN OF ITS OWN. The ROM writes $00
 * where it would otherwise write the wall nibbles when playMode is coop
 * (main.asm.txt:3480-3489, and its own comment there: "to add 1 column on
 * either side"), so the two sentinel columns the core keeps become playable
 * and the board is twelve wide. The cartridge draws that mode on its own
 * nametable — screen 5, already symmetric, with a seven-column panel either
 * side and the dancers' ledges down both — so the port reflows that one for
 * coop instead of the 1P screen. See gba/screen_coop.h. */
#define FIELD_TX SCREEN_1P_FIELD_TX  /* port column of the first playable one */
#define FIELD_TY 0   /* the field starts at the top of the visible window */
#define FIELD_COL0 1              /* storage column of the first playable one */
#define FIELD_PLAYABLE (TENGEN_PF_WIDTH - 2)
#define COOP_FIELD_TX SCREEN_COOP_FIELD_TX
#define COOP_FIELD_COL0 0
#define COOP_FIELD_PLAYABLE TENGEN_PF_WIDTH

/* ...and what a twelve-wide board leaves of thirty: seven columns either
 * side. Not boxes — the cartridge's coop screen leaves them open, with the
 * dancers' ledges ruled across them. See draw_coop_panel. */
#define COOP_PANEL_W 7
/* The cartridge's own first ledge, which is the bottom of the tall
 * compartment. Screen 5 rules them three rows apart from here. */
#define COOP_LEDGE_FIRST 8
#define COOP_L_TX 0
#define COOP_R_TX (SCREEN_TW - COOP_PANEL_W)

/* THE TWO BOXES, WHICH ARE MADE OF THE BRAID ITSELF.
 *
 * The reflow is 10 | 10 | 10: the ten playable columns in the middle, and on
 * each side a closed rectangle of the blue rope that used to run down the
 * board's edges as two bare vertical strips. The rope's inner side is still
 * exactly where it was, so the playfield is framed by braid exactly as the
 * cartridge frames it — the frame simply carries on round the HUD now instead
 * of stopping.
 *
 * SIX COLUMNS INSIDE, and that is the whole story of this layout. The braid
 * is two tiles thick and cannot be thinner (each tile is half the rope cut
 * lengthwise), so ten columns minus two frames leaves six, and sixteen rows
 * of twenty. Everything below is arranged to that.
 *
 * Left holds the counters, right the next piece and the piece statistics, and
 * during a level-up the right box is handed to the dancers, which is what the
 * cartridge does with its banner. */
#define BOX_L_TX 0
#define BOX_R_TX 20
#define BOX_W    10
/* EIGHT COLUMNS INSIDE, and the eighth is the whole reason the boxes are open
 * at the screen's edge instead of closed. The piece statistics are not seven
 * separable icons: they are ONE seven-tile picture, drawn interlocked across
 * the tile boundaries (render kStatsIcons side by side and it is obvious), so
 * they cannot be squeezed into six columns at any pitch. Closing the box costs
 * two columns and the histogram has to break into two ranks; opening it at the
 * edge — where the screen already ends — keeps the rope framing the playfield
 * exactly as before and leaves room for the strip in one row, with NEXT above
 * it. */
#define BOX_IN   (BOX_W - 2)         /* eight columns of interior */
/* The left panel's interior starts at the screen's own edge, so its content
 * is indented one column: seven for the lettering and the numbers, which is
 * exactly what SCORE and a six-digit score need, and one of air against the
 * edge. The right panel gets its spare column at the other end for the same
 * reason. */
#define BOX_L_IN (BOX_L_TX + 1)      /* first content column, left panel */
/* ...and one column narrower than the interior BECAUSE of that indent. Using
 * the full width from an indented start ran one column past the interior and
 * erased the rope itself, a row at a time, wherever a counter was cleared. */
#define BOX_L_W  (BOX_IN - 1)
#define BOX_R_IN (BOX_R_TX + 2)      /* ...and right */
#define BOX_TOP_IN 2                 /* first interior row */
/* ...and the last one. Nineteen, not seventeen: the panels are inverted Ls
 * open at the bottom now, so the interior runs to the screen's own edge. The
 * histogram gets its two rows back with it — its icons stand on the last
 * interior row and its bars grow up out of them. */
#define BOX_BOT_IN (SCREEN_TH - 1)

/* PALETTE BANKS.
 *
 * The NES has four background palettes at a time and swaps the whole set per
 * screen: updatePalette installs bgPalette0 for the title, bgPalette1 for the
 * menu and bgPalette2 for the game (main.asm.txt:5268-5287, and see
 * reference/NOTES.md for which screen calls which). The GBA has sixteen banks
 * and no reason to swap, so all three sets live at once, four banks each, and
 * a screen just uses its own four. Getting this wrong is what made the title
 * monochrome for a while: it really does use all four of its palettes.
 *
 * Bank 0 is additionally rewritten per level, which is what
 * setPlayfieldPaletteFromLevel does (main.asm.txt:5328) — and only the
 * playfield's own tiles use bank 0, so nothing else changes colour with it. */
#define PAL_GAME_BASE  0   /* banks 0-3: bgPalette2 */
#define PAL_TITLE_BASE 4   /* banks 4-7: bgPalette0 */
#define PAL_MENU_BASE  8   /* banks 8-11: bgPalette1 */
#define PAL_PIECE_BANK 12  /* the falling piece's own colours */
/* AND THE PREVIEW NEEDS ITS OWN. Both were drawn out of bank 12, which holds
 * whatever setPiecePalette last wrote — the piece IN PLAY — so the NEXT
 * preview was painted in the colours of the piece already falling and changed
 * colour under you every time one locked. On the cartridge the preview is not
 * a sprite at all (see draw_next_piece), so there was nothing to copy here;
 * a second bank, loaded from the same table by the NEXT piece's id, is the
 * cheapest thing that is right. */
#define PAL_NEXT_BANK  13

/* AND COOP NEEDS A THIRD. There are two pieces falling on the one board there
 * and no reason for them to be the same tetromino, so the partner's carries
 * its own colours out of its own bank — which is what the cartridge does too,
 * by staging the two players' pieces as sprites with a palette each. */
#define PAL_PIECE2_BANK 14

/* ...AND THE PARTNER'S PREVIEW, on a coop board, which needs a fourth set of
 * piece colours: their NEXT is not their CURRENT and it is not ours either.
 * It is borrowed from the TITLE'S OWN BANK the way the plaque, the histogram
 * and the menu logo borrow the other three — no title is up while a board is,
 * install_title_palette fills all four again on every visit, and the pages
 * between a coop match and the title (the plaque, the HIGH SCORES page) draw
 * out of the menu's banks and the game's, never this one. See
 * SKIN_PLAQUE_BANK for the same argument made three times already. */
#define PAL_NEXT2_BANK PAL_TITLE_BASE

/* Anything a menu SAYS rather than offers: the handicap's unit, the line at
 * the foot. The same pale blue as a value, because on the cartridge's own
 * screens the ONLY white is the cursor and everything else is blue.
 *
 * A BANK OF ITS OWN, AND THE SKIN IS WHY. bgPalette1's bank 2 is where the
 * menu frame's braid lives, so once a skin started recolouring that bank to
 * the prototype's fret the notes went with it and PRESS START TO PLAY came up
 * in the fret's red. Bank 15 is the one background palette nothing else
 * claims — 0-3 are the game's, 4-7 the title's, 8-11 the menu's, 12-14 the
 * falling piece, the preview and the partner's piece — so the note keeps the
 * cartridge's pale blue whatever the frame is wearing. */
#define BANK_NOTE 15

/* The title screen has its own 256-tile set, uploaded above the game's so
 * both live in one charblock (512 tiles is exactly its 16KB). */
#define TITLE_TILE_BASE 256
#define PROTO_TILE_BASE 512   /* charblock 0 holds 1024 addressable tiles */
#define HUD_LABEL_TILE_BASE 768   /* the labels, with the grid stubs removed */

/* ----------------------------------------------------------------------- *
 * The between-levels dancers
 *
 * Sprites, six of them in a 1P game, each four 8x8 tiles in a 2x2 block.
 * They are stacked in one column 24 pixels apart, and the level-up blit —
 * which is what makes room for them — does more than clear the TETRIS
 * banner: it draws their STAGE into it, five ledges of tile $9D one every
 * three rows (`levelUpAnimationColsRows1` at $B7FF, tiles at `LC82C`). The
 * ledges land exactly 24 pixels apart, under the dancers' feet, which is
 * what pins the two tables to each other.
 *
 * They start 15 pixels left of the banner (x $61 against a banner at $70 on
 * the cartridge) and walk right onto it one pixel every four frames; the pose
 * advances every eight (main.asm.txt:6392-6499). Positions, stage and poses
 * are all the ROM's, from tools/extract_assets.py — the start is expressed
 * as that offset from the stage so it follows the banner to its new column.
 *
 * WHAT IS STILL NOT THE ROM'S: each dancer's individual choreography. The
 * ROM gives every dancer a pointer into a little program whose entries are
 * poses, jumps to other programs, or random branches, decided by comparing
 * the pointer against $B14D and $C8BC — traced now and written up in
 * reference/NOTES.md, but not yet wired up here, so these walk the pose
 * table from staggered starting points instead.
 * ----------------------------------------------------------------------- */
#define DANCER_COUNT DANCER_SOLO_COUNT
#define DANCER_SPRITES 4              /* four 8x8 tiles in a 2x2 per dancer */
#define DANCER_POSE_FRAMES 8          /* pose advance cadence, from the ROM */
#define DANCER_WALK_FRAMES 4          /* X advance cadence, from the ROM */
#define PAL_OBJ_DANCER 0   /* banks 0-3: spritePalette2 */

/* HOW LONG THEY DANCE, which is not a number anybody would guess.
 *
 * The interlude is driven by player1FallTimer, reused as its clock
 * (`checkLevelUp`, main.asm.txt:1956-1979). The timer advances by one every
 * SIXTEEN frames — `lda frameCounterLow / and #$0F / bne` — and the state
 * machine reads:
 *
 *   showLevelBonus (:1926)  sets the timer to 0 and silences the music
 *   timer reaches 13        L8D6B (:2034) starts the show: level-up music,
 *                           the dancers' palette, the stage blit, and the
 *                           timer is set to $7C = 124
 *   timer 124 -> 244        the dancers perform
 *   timer 244               L9035 (:2393) begins the wind-down, forcing the
 *                           timer to $F5 = 245
 *   timer wraps past 255    finishLevelUpAnimation (:2465), back to play
 *
 * So the dancing itself is (244-124) * 16 = 1920 frames, about 32 seconds,
 * and the wind-down another 11 * 16 = 176. This port had 200 frames total,
 * which is not the same thing at all.
 *
 * A button does NOT cut it short; it fast-forwards. L9035 computes
 * $7C - timer - 5 and clamps it to at least $F5, which lands you in the same
 * wind-down — so an impatient player still gets three seconds of it, exactly
 * as on the cartridge. */
#define DANCER_TICK_FRAMES 16         /* frameCounterLow & $0F */
#define DANCER_TIMER_START 0x7C       /* L8D6B */
/* How long MUSIC_LEVELUP_INTRO runs before it lets every channel go, counted
 * on the cartridge's own engine: frame 143 is the last one it holds a
 * channel for. The cartridge spends those frames on the transition between
 * the board and the show — its state counter walks $05 to $0D sixteen frames
 * at a time, and L8D6B, which plays the looping tune, is waiting at $0D. The
 * port has no transition screen: its show starts on the frame the rows
 * collapse. So the intro plays over the show's opening instead of before it,
 * and the loop takes over where the cartridge takes it over. See
 * NES_MUSIC_LEVELUP_INTRO. */
#define LEVELUP_INTRO_FRAMES 144
#define DANCER_TIMER_WINDDOWN 0xF4    /* L9035 compares against this */
#define DANCER_TIMER_TAIL 0xF5        /* ...and forces at least this */

/* The blit's own tile coordinates: 4 columns x 18 rows at nametable (14,10).
 * The banner it used to cover has no place on the port's play screen, so the
 * stage goes in the middle of the right-hand box instead — same size, same
 * rows, and the box's own contents are put back when the show ends. */
#define DANCER_STAGE_TX (BOX_R_TX + 2 + (BOX_W - 2 - DANCER_STAGE_COLS) / 2)
#define DANCER_STAGE_TY 1
#define DANCER_STAGE_TW DANCER_STAGE_COLS
#define DANCER_STAGE_TH DANCER_STAGE_ROWS

/* THE SIXTH DANCER STANDS ON THE SCREEN'S BOTTOM BORDER, NOT ON A LEDGE.
 *
 * The blit lays five ledges (kDancerStage, rows 3/6/9/12/15 of eighteen from
 * nametable row 10), so on the cartridge the ledges are at NES y 104, 128,
 * 152, 176 and 200 — and kDancerStartY puts six pairs of feet at 104, 128,
 * 152, 176, 200 and 224. The last has no ledge because at NES y 224 it is
 * standing on the border tiles at the bottom of the screen.
 *
 * The port's window is NES nametable rows 8-27, which stops one row short of
 * that border, so the sixth dancer was left treading air at the bottom edge.
 * Two things fix it and neither touches the ROM's 24-pixel spacing: the show
 * moves up one tile row, and the sixth ledge is drawn explicitly below the
 * blit — the same $9D the other five are made of, in the place the cartridge's
 * border occupies. */
#define DANCER_LIFT 8                     /* one tile row up; see above */
#define DANCER_FLOOR_TY (DANCER_STAGE_TY + DANCER_STAGE_TH)

/* The ROM's sprite coordinates are NES screen pixels; this window starts at
 * nametable row 8, so a NES y of 64 is this screen's 0. */
#define DANCER_Y_ORIGIN (SCREEN_1P_FIELD_TY * 8 + DANCER_LIFT)
/* $70 - $61 on the cartridge: how far left of its stage a dancer starts. */
#define DANCER_START_OFFSET 15

/* ----------------------------------------------------------------------- *
 * The line-clear sweep
 *
 * A puff of smoke crosses each completed row and leaves the size of the
 * clear spelled out behind it. Five sprite tiles trail one another
 * (main.asm.txt:1274-1338); the timing lives in the core, see
 * tengen_line_clear_step.
 *
 * The tiles are $5B..$5F of the cartridge's SPRITE bank — the same bank the
 * dancers come from, which is already uploaded whole, so their ids here are
 * the ROM's own. They are drawn in piecePaletteIndexA, labelled "Line
 * clears" in the disassembly and flat black in all three entries
 * (main.asm.txt:5394-5396), so the puff is a silhouette.
 * ----------------------------------------------------------------------- */
#define CLEAR_HEAD_TILE 0x5B  /* $5B is the tail; $5B+4 = $5F is the head */
#define PAL_OBJ_CLEAR 4
#define TITLE_OBJ_TILE_BASE 256   /* CHR bank 3, above the dancers' 256 */
/* ...and the ten digits above both of them. The cartridge does not need this:
 * its mapper banks CHR a kilobyte at a time, so the sprite pattern table can
 * hold the dancers' sparkles at $5B and the game bank's digits at $30 at the
 * same time. A GBA charblock is just memory, so the digits are copied in. */
#define POINTS_OBJ_TILE_BASE 512
/* ...and the tile after the ten digits, for the spire's rim. */
#define SPIRE_RIM_TILE (POINTS_OBJ_TILE_BASE + 10)
#define PAL_OBJ_TITLE 5           /* banks 5-8: spritePalette1 */

/* spritePalette0, which is the set the cartridge has installed while a game
 * is being played (main.asm.txt:A716). Only the drop-point digits use it, and
 * they use it the ROM's way: palette 3 in a one-player game, and the PLAYER'S
 * OWN palette otherwise — 0 is red and 1 is blue, so on a shared board you
 * can tell whose points just went up without reading them. */
#define PAL_OBJ_GAME 9            /* banks 9-12: spritePalette0 */
/* ...and one more for the cathedral's topmost ball, whose left rim is drawn
 * as an object over the logo rather than into it. See draw_spire_rim. */
#define PAL_OBJ_SPIRE 13

/* ----------------------------------------------------------------------- *
 * The pause box
 *
 * pauseTiles (main.asm.txt:8061-8063) is an 8x2 block of the cartridge's own
 * tiles, blitted over the screen at nametable (12,8) — the right edge of the
 * playfield and the start of the decorative divider (pausePPUAddr1 = $210C,
 * and unpausePPUAddr1/2 put the same two rows back). pauseAttrs ($EF,$BF)
 * colours it with background palette 3.
 *
 * On the cartridge those eight columns are 12..19 of 32, which puts the
 * plaque dead centre of the screen. That is the relationship worth keeping,
 * not the column number: on 30 columns it centres at (30-8)/2 = 11, and it is
 * centred vertically too rather than sitting against the top edge. Reading the
 * column number off the cartridge and using it unchanged is what left it
 * hanging off to one side.
 * ----------------------------------------------------------------------- */
#define PAUSE_W 8
#define PAUSE_H 2
#define PAUSE_TX ((SCREEN_TW - PAUSE_W) / 2)
#define PAUSE_TY ((SCREEN_TH - PAUSE_H) / 2)
#define BANK_PAUSE 3

#define WITH_BANK(tile, bank) ((uint16_t)((tile) | ((bank) << 12)))

/* Tile ids in the cartridge's own tileset, taken from the nametable it ships
 * (see reference/NOTES.md). Reusing the game's lettering is the difference
 * between the port looking like Tengen Tetris and looking like a clone. */
#define T_BLANK      0x00
#define T_RULE_LEFT  0x75
#define T_RULE_MID   0x76
#define T_RULE_RIGHT 0x79

/* ...and ONE of them on its own, for the level screen's handicap: L belongs
 * to player 1's side of the pad and R to player 2's. Each keeps its own held
 * state so neither can swallow the other's press, and the caller checks the
 * chord first so reaching for the hidden tunes buries nobody. */
#define SHOULDER_L 0
#define SHOULDER_R 1

/* Which bgPalette1 bank the MENU and HIGH SCORES frames draw their braid in —
 * measured off the generated screens, where every frame tile is in bank 2 and
 * nothing else on them is. */
#define SKIN_MENU_BRAID_BANK (PAL_MENU_BASE + 2)

/* The prototype's own banner art, into a window above everything else in the
 * charblock (a text background addresses 1024 tiles; the HUD labels end at
 * 789 and the panel's run pair at 801). Fifty-six tiles at most, 1792 bytes,
 * so unlike the title's 8KB swap this needs no blank screen to hide behind. */
#define SKIN_BANNER_BASE 832

/* THE HISTOGRAM'S SEVEN RUNS, into a window of their own above the banner's.
 *
 * The release has ONE eight-step bar tile shared by all seven columns, with a
 * strip of tetromino icons underneath to say which column is which piece. A
 * prototype has no icon strip: each piece gets its own eight-step run drawn in
 * that piece's own block pattern, and the pattern IS the label. So there is no
 * release slot to overwrite here the way the blocks and the frame are — seven
 * runs of eight is fifty-six tiles the release simply does not have — and they
 * go above the banner's window instead. 1792 bytes, well inside a vblank. */
#define SKIN_STATS_BASE 896

/* ...and the one bank they are all drawn in, which is the prototype's own
 * block palette: three colours is what tells seven patterns apart, and the
 * release's per-piece icon banks would have two of them come out the same.
 *
 * BORROWED FROM THE TITLE'S FOUR, like the menu logo's — no board and no
 * title are ever up at once, and install_title_palette fills all four again on
 * every visit, so the loan always comes back. */
#define SKIN_STATS_BANK (PAL_TITLE_BASE + 2)

/* THE GAME OVER PLAQUE. Its eight box tiles ride in kSkinTiles, straight into
 * the release's own slots — the words are plain ASCII and identical in all
 * four builds — so only its COLOURS are left, and they are a whole bank: the
 * release frames the plaque in red and every prototype in blue.
 *
 * NOT game bank 3, though, which is where the release draws it: that bank is
 * also the HUD's — NEXT, SCORE, LINES, LEVEL and their rules — and every
 * prototype's NEXT is as red as the release's, so their plaque's blue is NOT
 * in the bank their header is in. Taking bank 3 wholesale turned NEXT grey.
 * The plaque borrows a title bank instead. */
#define SKIN_PLAQUE_BANK (PAL_TITLE_BASE + 1)

/* The menus' logo gets a bank of its own, borrowed from the TITLE's four:
 * no two screens are ever up at once, and install_title_palette fills all
 * four again on every visit to the title, so the loan always comes back. It
 * cannot share the frame's — proto_b draws its logo in the fret's bank and
 * proto_c does not. */
#define SKIN_LOGO_BANK (PAL_TITLE_BASE + 3)


/* ----------------------------------------------------------------------- *
 * THE HUD HAS TWO SHAPES, AND NOW THEY HAVE NAMES
 *
 *   HUD STATS   the default: NEXT over the piece histogram, right box.
 *   HUD BANNER  L+R: the cartridge's vertical TETRIS takes the right column
 *               whole, NEXT moves down into the left box, and the level-up
 *               dancers have somewhere to come on.
 *
 * The trade is the whole point of the pair. HUD STATS keeps the statistics
 * and gives up the stage; HUD BANNER gives up the statistics and gets the
 * show — so a player who wants to see all six cossacks at a high level has a
 * reason to switch, and one who wants the histogram is not being punished for
 * it. What HUD STATS gets in exchange is ONE COSSACK, standing, in the cell
 * NEXT leaves empty at the bottom of the left box.
 *
 * HE IS AT REST, NOT DANCING, and that distinction is honest about what is
 * traced. The dancers' choreography scripts are not (see reference/NOTES.md),
 * so what "idle" looks like is the port's choice — but the POSES are the
 * cartridge's own: numbers 0 and 1 of its table are the same stance with the
 * arms a pixel apart, so alternating them slowly reads as breathing rather
 * than as a step. Nothing here invents artwork.
 * ----------------------------------------------------------------------- */
#define IDLE_OAM_BASE 124          /* four slots nothing else reaches */
/* ...and six more under him for the drop-point digits, three to a player.
 * They come out of the sweep's range rather than the cossack's because the
 * sweep is the only thing that ever fills it, and it needs twenty at most. */
#define POINTS_OAM_BASE (IDLE_OAM_BASE - 6)
#define IDLE_POSE_FRAMES 48        /* a slow sway, not the show's eight */

/* AND HE ANSWERS THE BOARD. A clear sets him dancing for as many poses as it
 * was worth — six a line, so a single is a beat of it and a TETRIS is the
 * whole figure — after which he settles back into the sway.
 *
 * WHICH poses is the port's choice and says so. The cartridge's dancers each
 * follow a little program (`LB015`, main.asm.txt:6392-6499) and those programs
 * are not traced, so there is no "the tetris dance" to copy. What is the
 * cartridge's is every pose in it: the table's first two are the standing
 * stance, and from the third on they are the figure, so the run below is the
 * table read in its own order at the ROM's own eight-frame cadence. */
#define DANCE_FIRST_POSE 2
#define DANCE_POSES_PER_LINE 6
#define DANCE_MAX_POSES (DANCE_POSES_PER_LINE * 4)   /* a tetris */
#define DANCE_POSE_FRAMES DANCER_POSE_FRAMES         /* eight, the show's own */

/* Four palettes, which is all the difference there is between the cartridge's
 * six: they share one pose table and pick among spritePalette2's four with
 * their own attribute bytes ($8E78). SELECT walks them. */
#define IDLE_PALETTE_COUNT 4

/* ----------------------------------------------------------------------- *
 * The drop-point sprites — what the piece was worth, beside the piece
 *
 * L8129 (main.asm.txt:218-274) stages three sprites the moment a piece comes
 * to rest, and stageDropPointSprites (:283-313) keeps them there for $3C
 * frames and then takes them away. What they show is the award the scoring
 * routine has just computed, in the level's own terms, with leading zeros
 * blanked; where they show it is the whole point of them, and it is worth
 * spelling out because it is not decoration:
 *
 *   THE HEIGHT IS THE SCORE. This game pays for a piece by how HIGH it comes
 *   to rest (see add_lock_score), and the ROM puts the number at that height:
 *   `$2D` — the rows between what the piece landed on and the floor — is what
 *   it turns into the sprites' Y, clamped so it never climbs past the third
 *   row ($4F, :293-296). So the number rises up the side of the board as the
 *   stack does, and a player learns what the game pays for by watching it.
 *
 *   THE SIDE IS THE PLAYER. Player 1's sit just outside the right-hand edge
 *   of its board and player 2's just outside the left of its own, which on
 *   the cartridge's two-player screen puts both in the gap down the middle;
 *   on a coop board, where there is only one board and two players dropping
 *   into it, they go one to each side of it. The ports's screen is reflowed
 *   but the relationship is not: beside the board, outside it, on that
 *   player's side. Over the braid, as they are over the cartridge's.
 * ----------------------------------------------------------------------- */
#define POINTS_FRAMES 0x3C   /* main.asm.txt:271-272 */
#define POINTS_DIGITS 3
/* The ROM's #$4F clamp, in rows: $4F is drawn at y=80, which on its screen is
 * the third row of the playfield. */
#define POINTS_TOP_ROW 2

#define BRAID_T 2                 /* the frame's thickness, in tiles */

/* THE DANCERS' LEDGE, WHICH IS ALSO A SHELF.
 *
 * $9D is the blue bar the cartridge's coop screen rules across both panels
 * every three rows, so its eight cossacks have something to stand on. It is
 * also, as it happens, the best rule in the game: it is the same blue as the
 * rope and it reads as a piece of the frame rather than as a line drawn over
 * the background, which is what the 1P panel's grey $76 always looked like.
 *
 * So the port's own boxes use it too, and take the coop screen's spacing with
 * it — see draw_braid_panel's `shelves`. */
#define T_LEDGE 0x9D

/* A panel of rope: the braid along the top, the bottom and the side facing the
 * board, and the screen's own edge closing it outward. `inner_right` says
 * which side of the panel the board is on.
 *
 * WHICH TILES, AND WHY THAT WAY ROUND — and this is the entry that was
 * WRONG for a long time, in a way that showed on every frame of every game.
 *
 * The rope's fret is chiral. $6A $6B, the cartridge's LEFT wall, has its
 * hooks opening RIGHT; $73 $74, its RIGHT wall, opens LEFT. So on the
 * cartridge BOTH walls of the playfield have their indentations pointing IN,
 * at the board. This port used to pick the tile by which side OF THE PANEL
 * the rope was on — $73 $74 for a panel with the board on its right — which
 * put the fret of both walls pointing outwards, away from the board, at the
 * HUD. The one thing in the port the board is meant to be 1:1 about, framed
 * inside out.
 *
 * It was done that way because of the CORNERS: the screen's own $9E $64 /
 * $9F $69 turns a top run down into $73 $74 and there is no mirror of it, so
 * a left panel with $6A $6B under that corner breaks the weave at the turn.
 *
 * THE CARTRIDGE HAS THE CORNERS. They are not on the screen's border, they
 * are where the header's bottom rule hangs the banner box's walls off itself
 * — kBraidHangLeft ($95 $96 / $99 $9A) turns a run coming from the LEFT down
 * into $6A $6B, kBraidHangRight ($97 $98 / $9B $9C) turns one coming from the
 * RIGHT down into $73 $74 — and the run they belong to is kBraidBottom, the
 * rule itself. The COOP screen builds exactly this port's layout out of the
 * three: a twelve-wide field walled $6A $6B and $73 $74, hung off that rule
 * at columns 8-9 and 22-23, with an open panel either side. And those rows
 * ARE the port's rows 0-1 — the window starts at nametable row 8 — so this
 * is not a lookalike assembled from spare parts, it is the cartridge's own
 * screen at the very rows the port draws.
 *
 *   left panel  (cols 0-9)   board on its RIGHT -> kBraidLeft,  HangLeft
 *   right panel (cols 20-29) board on its LEFT  -> kBraidRight, HangRight
 *
 * What it must NOT cost is the weave changing direction when the HUD does,
 * so anything that redraws these columns as a plain strip has to use the
 * panel's own tile — see draw_field_braid's callers.
 *
 * The corners are only ever drawn on the board side, because that is the only
 * side that has one — the other simply runs off the screen. */
/* SHELVES, AND WHERE THEY FALL. `shelves` opens the panel at the bottom and
 * rules the cartridge's own ledges across it — which is the coop screen's
 * frame, and the coop screen is the one the port did not design. Its panels
 * are better than the boxes were: one tall cell at the top and four short
 * ones under it, each a thing in its own compartment, with the shelf reading
 * as part of the rope instead of a grey line ruled over the black.
 *
 * The PITCH is the cartridge's, not chosen here: screen 5 rules its ledges
 * three rows apart, which leaves a tall cell at the top and pairs under it —
 * exactly five compartments, and the HUD has exactly five things to say.
 *
 * The ROWS are the cartridge's as well — 8, 11, 14 and 17 — so the port's own
 * panels and the coop screen's rule at the same heights. That puts the last
 * compartment on rows 18-19, the last two on the screen, and a six-digit
 * number there used to lose its bottom two rows of pixels off the console;
 * the counters' layer moved two pixels UP (PANEL_SHIFT_PX) and it fits with
 * room to spare. */
#define SHELF_FIRST 8
#define SHELF_STEP  3
#define SHELF_COUNT 4
/* The big cell, rows 2-7, and the four pairs at 9-10, 12-13, 15-16, 18-19. */
#define CELL_BIG_TY BOX_TOP_IN
#define CELL_BIG_H  (SHELF_FIRST - BOX_TOP_IN)
#define CELL_TY(n)  (SHELF_FIRST + (n) * SHELF_STEP + 1)

/* gameOverTiles, blitted where the cartridge blits it: nametable (4,12),
 * which is the middle of the playfield (gameOver1pPPUAddr1 = $2184,
 * gameOver1pColsRows1 = 6 columns by 4 rows, gameOverAttrs = palette 3). */
/* CENTRED ON THE BOARD, and the board is not always ten columns wide. The
 * plaque is six; `+ 2` centres it on a ten-wide field and leaves it a column
 * left of centre on coop's twelve, which is what "game over no esta centrado"
 * is. Measured off the field's own width instead. */
#define GAMEOVER_TX (field_tx() + (field_cols() - SCREEN_1P_GAMEOVER_W) / 2)
#define GAMEOVER_TY 4

/* The cartridge draws its whole header strip — SCORE, LINES, LEVEL, NEXT,
 * STATS, the rules under them and the counters themselves — in background
 * palette 3, which is read straight off its attribute table (rows 2-7 of the
 * 1P screen are solidly bank 3). Same lettering, same colours. */
#define BANK_LABEL 3
#define BANK_VALUE 3
/* THE MENU IS BLUE, and this port had it white with an orange cursor.
 *
 * bgPalette1's four banks, measured off the cartridge's own GAME SELECT with
 * its attribute table:
 *
 *   bank 0  $12  the blue EVERY line of the menu is written in — the heading,
 *                the five entries, all of it
 *   bank 1  $27  the orange, and the only thing in it is the credit line
 *                along the bottom
 *   bank 2  $31  a pale blue
 *   bank 3  $30  white
 *
 * The cartridge marks the chosen entry with a WHITE ARROW beside it and does
 * not recolour anything. This port had written the list in bank 3's white and
 * marked the choice in bank 1's orange — which is to say it used the credit's
 * colour for the cursor and never used the menu's own colour at all.
 *
 * So: the body goes back to the cartridge's blue and the arrow is white the
 * way the cartridge's is. AND NOTHING ELSE IS RECOLOURED. That was measured
 * off the running cartridge rather than reasoned about — every entry on its
 * GAME SELECT reads $12 blue to the pixel, chosen or not, and the only $30
 * white anywhere on the screen is the arrow. A pale-blue "chosen" tone stood
 * here for a while and it was still an invention: it is a second near-white
 * beside a cursor that is already saying the same thing. */
#define BANK_MENU (PAL_MENU_BASE + 0)   /* the cartridge's menu blue */
#define BANK_ARROW (PAL_MENU_BASE + 3)  /* the cursor, white like its sprite */
/* ...and the credits, in the one colour the cartridge uses for them. */
#define BANK_CREDIT (PAL_MENU_BASE + 1)

/* ----------------------------------------------------------------------- *
 * The level's BONUS tally
 *
 * A level does not just bring the cossacks on. `displayStatsP1` — index 9 of
 * gameBackgroundPatches (main.asm.txt:7554-7660) — paints the PLAYFIELD ITSELF
 * over with a scoreboard while they dance: the BONUS heading, then what this
 * level's clears were worth, category by category, and a total. The
 * multipliers are printed in the ROM's own strings, so there is nothing to
 * guess about them:
 *
 *     n SINGLES   X100=   n*100
 *     n DOUBLES   X400=   n*400
 *     n TRIPLES   X900=   n*900
 *     n TETRIS    2500=   n*2500      (no X on this one — the ROM's "X" at
 *     TOTAL               sum          statsTiles9 is never blitted)
 *
 * ...and the total is not a read-out: L8EA2 (:2189-2280) counts it up a clear
 * at a time, one every five frames, ADDING TO THE SCORE as it goes. That is
 * where a Tengen game's points actually come from at high levels, and the port
 * had none of it.
 *
 * WHERE IT GOES. The ROM's blit addresses are rows 8-27, columns 2-11 of its
 * nametable — which is the playfield, exactly — so in this port's frame that
 * is the ten columns at FIELD_TX and rows 0-19. Every row below is the ROM's
 * own, minus the eight the window drops.
 * ----------------------------------------------------------------------- */
#define BONUS_TICK_FRAMES 5      /* $0198 reloads with 5 (main.asm.txt:2244-2250) */
#define BONUS_CATEGORIES 4
#define BONUS_LABEL_DX 3         /* the labels sit at the ROM's column 5 */
#define BONUS_COUNT_DX 0         /* ...and the count, two wide, at the left edge */
#define BONUS_TOTAL_TY 18        /* $2342 */
#define BONUS_TOTAL_VALUE_TY 19  /* $236A, on the row under it */
#define BONUS_VALUE_DIGITS 5     /* what is left of the ten columns after "X100=" */

/* ----------------------------------------------------------------------- *
 * The HIGH SCORES table
 *
 * The cartridge keeps fifteen of them, with the lines that earned each one
 * and three initials, and it keeps them IN MEMORY: `reset` tests a four-byte
 * magic at $04F7 — 'L','O','G','G' — and then walks $0418-$04EF checking every
 * digit is '0'-'9' and every initial is a letter before it trusts what is
 * there (main.asm.txt:5644-5668). Survive a RESET and the table survives with
 * you; come up cold and it is rebuilt. A GBA cartridge has no equivalent of
 * that RAM, so this port rebuilds it every boot — but from the cartridge's own
 * defaults, which is why HIGH SCORE opens at 17000 rather than at nothing.
 *
 * THE FIRST ENTRY IS THE HIGH SCORE. statsDataAddresses' last entry is
 * `highScoreHundredThousands` (main.asm.txt:4107), which is the top of this
 * table — the number on the panel during play is not a separate thing.
 * ----------------------------------------------------------------------- */
#define LEADER_ENTRIES SCREEN_LEADER_ENTRIES
#define LEADER_INITIALS 3
/* The alphabet an initial is chosen from: L93DC (main.asm.txt:2957-2961) is a
 * space and then A to Z, indexed 0 to 26, which is why an initial is stored as
 * an index and not as a letter. */
#define LEADER_LETTERS 27

typedef struct {
    uint32_t score;
    uint16_t lines;
    uint8_t initials[LEADER_INITIALS];  /* indices into the alphabet */
} LeaderEntry;

#define SAVE_MAGIC_LEN 4
/* magic, then one byte of checksum, then the entries. */
#define SAVE_SUM_OFF SAVE_MAGIC_LEN
#define SAVE_DATA_OFF (SAVE_MAGIC_LEN + 1)
/* Four bytes of score, two of lines, three of initials. NINE: it was eight
 * first, and the entries wrote over each other's initials — the magic and
 * the checksum were fine, so the table simply never loaded. */
#define SAVE_ENTRY_BYTES 9

/* ONE TABLE PER BUILD. A prototype is a different game — its level climbs
 * every ten lines and its rows go the frame they complete, so a score made
 * on one is not a score made on the release, and putting them in one table
 * made the easiest build own the page. So there is a table for the release
 * and one for each prototype, chosen by the skin the match was played in.
 *
 * TABLE 0 STAYS WHERE IT WAS, bytes and offsets unchanged, so a console that
 * already has a release table keeps it; the others are written after it and
 * carry a checksum byte each, past them all. A table whose bytes do not add
 * up is not refused, it is simply the cartridge's own fifteen — which is
 * what a build nobody has played yet should look like. */
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
#define LEADER_TABLES (1 + SCREEN_PROTO_COUNT)
#else
#define LEADER_TABLES 1
#endif
#define SAVE_TABLE_BYTES ((unsigned)LEADER_ENTRIES * SAVE_ENTRY_BYTES)
#define SAVE_TABLE_OFF(t) (SAVE_DATA_OFF + (unsigned)(t) * SAVE_TABLE_BYTES)
/* ...and the extra tables' checksums, one byte each, after all the data. */
#define SAVE_SUMS_OFF SAVE_TABLE_OFF(LEADER_TABLES)

/* HOW LONG EACH OF THE TWO PAGES STANDS THERE, and both are the cartridge's
 * own, counted on its own clock — which nobody would guess at, because both
 * come out of the SAME byte being used for two things.
 *
 * GAME OVER. The top-out writes one value into two places
 * (main.asm.txt:605-607): `lda #GAMESTATE_GAMEOVER` / `sta gameState` /
 * `sta player1FallTimer` — so the fall timer starts at $F9, 249, because that
 * is what the state number happens to be. L9205 then decrements it on every
 * OTHER frame (`lsr a` / `bcs` on frameCounterLow) and jumps to the high
 * scores at zero: 498 frames, eight and a third seconds.
 *
 * HIGH SCORES. initializeLeaderboard does NOT reload that timer — it sets
 * player2FallTimer to $0A and leaves player1's alone (main.asm.txt:2963-2995),
 * and player1's is the zero the countdown above just arrived at. So L91F8's
 * first `dec` UNDERFLOWS to 255, and at one decrement every fourth frame
 * (`and #$03`) the page holds for 1020 frames, seventeen seconds.
 *
 * Measured on the cartridge to be sure, by burying the field and watching
 * gameState: $F9 at frame 48, $F8 at 546, the title at 1571 — 498 and 1025,
 * the five being where the frame counter's phase falls.
 *
 * This port used to wait for a BUTTON on the game over and hold the table for
 * three hundred frames, which is the wrong way round on both counts: the
 * plaque never left on its own and the page left too soon. A button still
 * cuts either short. */
#define GAMEOVER_HOLD_FRAMES 498
#define LEADER_HOLD_FRAMES 1020
#define LEADER_DAS 10

#define NEXT_CELL_W 4

/* The banner's own tiles and palettes, extracted on their own because the
 * reflow no longer carries NES columns 14-17. Eighteen rows, which is what
 * the right-hand box has under NEXT — the one place the arithmetic comes out
 * even. */
#define BANNER_TX (BOX_R_TX + 2 + (BOX_W - 2 - SCREEN_1P_BANNER_W) / 2)
/* Row 2, the panel's first interior row: eighteen letters' rows in the
 * eighteen the open panel has. See draw_static_screen. */
#define BANNER_TY BOX_TOP_IN

/* ----------------------------------------------------------------------- *
 * The piece histogram
 *
 * The cartridge counts every piece it hands out and draws each count as a
 * vertical bar growing up out of a little picture of that piece
 * (`L9997`, main.asm.txt:3752-3798). Its own arithmetic decides what a bar
 * looks like and this reproduces it rather than approximating:
 *
 *   tile      = $21 + (count & 7)      -> eight steps of fill inside one tile
 *   row       = base - (count >> 3)    -> every eighth piece moves up a row
 *
 * so after N pieces the bar is N/8 solid tiles with an N%8 partial on top.
 *
 * The ROM stops at 144 (`cmp #$90 / bcs`), which is exactly the 18 rows its
 * panel is tall. This box is shorter, so the same rule caps lower; the count
 * itself keeps going, only the bar stops growing.
 * ----------------------------------------------------------------------- */
/* ONE ROW OF SEVEN, which is what the cartridge draws and what an eight-column
 * interior finally allows. The icons are not seven separable pictures — they
 * are one seven-tile strip, drawn interlocked across the tile boundaries — so
 * this is the only arrangement that shows them at all without cutting the
 * strip up. The bars grow up out of them, by the ROM's own arithmetic. */
#define STATS_ICON_TY (BOX_BOT_IN - 1)                  /* rows 18-19 */
/* Straight under the box's one shelf, which is what closes the top of the
 * histogram's half of it. The shelf is the LEFT panel's first one, so the two
 * sides of the screen rule at the same height — see draw_static_screen. */
#define STATS_TOP_TY (SHELF_FIRST + 1)
#define STATS_BAR_ROWS (STATS_ICON_TY - STATS_TOP_TY)   /* ten of them */
#define STATS_BAR_FULL (SCREEN_1P_STATS_BAR_TILE + 7)
#define BANK_STATS SCREEN_1P_STATS_BAR_BANK
/* Seven tiles in eight columns, so one spare. It is not left at either end:
 * the block rides the second background, which is scrolled STATS_SHIFT_PX so
 * that the strip's own ink — inset two pixels on its left and flush on its
 * right — comes out five pixels from the rope and five from the screen edge.
 * See SCREENBLOCK_STATS. */
#define STATS_TX BOX_R_IN

/* ----------------------------------------------------------------------- *
 * THE HUD, INSIDE THE BRAID
 *
 * Two panels of rope, eight columns and sixteen rows of interior each. There
 * is no room for a frame around every counter as well — and none is wanted:
 * the panel IS the frame, which is what makes the screen read as one object
 * instead of a stack of little plaques.
 *
 *   left,  rows 2-17    SCORE / LINES / LEVEL / HIGH SCORE
 *   right, rows 2-17    NEXT, and the piece statistics in one row of seven
 *
 * NEXT is back on the right, over the statistics, which is where the cartridge
 * puts it. It still moves to the left panel for the two things that take the
 * right one over whole: the vertical TETRIS banner (L+R) and, during a
 * level-up, the dancers' stage. A Tetris you cannot see the next piece in is
 * not a trade anybody wants to make for a decoration.
 * ----------------------------------------------------------------------- */
/* One compartment each, in the order the cartridge's own 1P panel reads them.
 * The shelves between are the frame's, drawn once with it, so a counter is
 * only ever its two rows — see draw_braid_panel. */
#define ROW_SCORE  CELL_TY(0)            /*  9: label, 10: value */
#define ROW_LINES  CELL_TY(1)            /* 12, 13 */
#define ROW_LEVEL  CELL_TY(2)            /* 15, 16 */
#define ROW_HIGH   CELL_TY(3)            /* 18, 19 */
/* NEXT IN THE BIG CELL, and centred in it. The block is FOUR rows of the six
 * — the word, a row of air, and the piece over two — so it starts one row
 * down and finishes one row short, which is centred exactly. See
 * draw_next_label_and_piece for why the row of air is there. */
#define NEXT_BLOCK_H 4
#define ROW_NEXT   (CELL_BIG_TY + (CELL_BIG_H - NEXT_BLOCK_H) / 2)
/* ...and where the right box's own cell ends, in HUD Stats. Four rows for the
 * cossack and the ledge under him, with the histogram's bars starting where
 * they always did. */
#define ROW_DANCER   BOX_TOP_IN
#define ROW_DANCER_H (SHELF_FIRST - BOX_TOP_IN)

/* ----------------------------------------------------------------------- *
 * THE COOP PANEL
 *
 * Seven columns either side, which is what a twelve-wide board leaves of
 * thirty, and they are not boxes: the cartridge's coop screen leaves them
 * open, with the dancers' LEDGES ruled across them every three rows. Those
 * ledges are the layout. They sit at window rows 8, 11, 14 and 17, so the
 * panel's free space is the six rows above the first one and the pairs
 * between the rest — and the port lays its HUD into exactly that, using the
 * cartridge's own ledges where the 1P panel would draw a rule.
 *
 * WHAT GOES IN THEM was the cartridge's choice and is not any more. Its coop
 * screen carries LEVEL on the left, HIGH and SCORE on the right, and no piece
 * counts, because its coop keeps one score between the two of you. This
 * port's core keeps a score, a line count and a preview PER PLAYER on the
 * shared board, so there is a panel each: yours on the left, theirs on the
 * right, and the two figures neither panel can give you — the board's totals
 * — in the last cell of each once the chord has been rung. See
 * draw_coop_panel.
 *
 * TWO PREVIEWS, AND THIS NOTE USED TO ARGUE FOR ONE. The argument was that
 * both players' lookahead randomisers are seeded from the same number
 * (main.asm.txt:3319-3326, and tengen_new_game says so) and each steps its
 * own once per spawn, so the two SEQUENCES are identical from the first piece
 * to the last however differently the two play — and that therefore drawing
 * it twice would be drawing the same piece twice.
 *
 * The sequences are identical. The two players' POSITIONS in them are not,
 * unless they have taken exactly as many pieces as each other, which over a
 * game they never do. Measured on a WITH COMPUTER board: the two NEXT pieces
 * differ on 1528 frames out of 1800. The partner's preview is their piece,
 * not a copy of ours, and it is the one thing on a shared board you cannot
 * work out by looking at the board.
 * ----------------------------------------------------------------------- */
/* THE SAME SHAPE AS THE OTHER TWO HUDS, which is the least this panel could
 * be: it is the screen they were copied FROM. The cartridge's ledges fall on
 * window rows 8, 11, 14 and 17, so the compartments are the six rows above
 * the first and the pairs between the rest — NEXT in the tall one and a
 * counter in each of the next two, on both sides, so the four line up in
 * pairs across the board.
 *
 * NEXT had the top of the panel with LEVEL immediately under it before, which
 * is four rows for three rows of content: the word and the piece with one
 * pixel between them and nothing either side. That is "Next y la pieza se
 * solapan" even before the two of them land on layers with different
 * scrolls. */
#define COOP_NEXT_TY ROW_NEXT       /* the same row as the other two HUDs */
/* FOUR CELLS A SIDE, not two. The ledges are three rows apart from row 8, so
 * the pairs between them are 9, 12, 15 and 18 — the same four the 1P panel
 * has, because the 1P panel was copied from this screen. Two of them stood
 * empty while the cossack had the right-hand tall compartment to himself. */
#define COOP_CELL_TY(i) (COOP_LEDGE_FIRST + 1 + (i) * 3)
#define COOP_COUNTER_TY COOP_CELL_TY(0)   /*  9 */
#define COOP_LOWER_TY   COOP_CELL_TY(1)   /* 12 */
#define COOP_THIRD_TY   COOP_CELL_TY(2)   /* 15 */
#define COOP_TOTAL_TY   COOP_CELL_TY(3)   /* 18 */


/* ----------------------------------------------------------------------- *
 * Title screen
 * ----------------------------------------------------------------------- */
/* Start levels run 0..9 and wrap at both ends, which is the range the ROM's
 * own menu allows: computerMoveSelectTable (main.asm.txt:4819) holds the wrap
 * limit per menu row, and menuPlayer1StartLevel's is 10.
 *
 * ...and 0..19 once the chord has been rung, which is TETRIS TENGEN XE — see
 * TENGEN_MAX_LEVEL_XE in src/tengen_core.h for what that mod is and, just as
 * importantly, what it is not. The mod's own way in is HOLD A AND PRESS START
 * on the level screen, a second control on a screen that already has one
 * because the cartridge's list is a column of ten fixed lines. This port's
 * level is a number you wind up and down, so the extra ten simply continue
 * it, behind the same L+R as the rest of the extras. */
#define START_LEVEL_COUNT 10
#define START_LEVEL_COUNT_XE 20

/* The four in-game tunes, in the order the ROM's own music-select menu lists
 * them (constants.asm.txt:39-42). Chosen on the level-select screen with
 * up/down, which is the ROM's GAMESTATE_MUSIC_SELECT folded into the one
 * selection screen this port has. */
/* musicSelectTable (main.asm.txt:4741): $08, $04, $05, $06, $07 — and the
 * disassembly's own comment names them "silence, loginska, bradinsky, karinka,
 * troika". FIVE entries, and the first is no music at all; this port offered
 * only the four tunes for a while, which quietly dropped one of the
 * cartridge's own choices. */
#define MUSIC_COUNT 5

/* THE TWO TUNES THAT ARE NOT ON THE CARTRIDGE.
 *
 * Korobeiniki is not one of Tengen's four — the game everybody hums it at is
 * Nintendo's Game Boy version — and neither is Katyusha. There is nothing to
 * extract for either, so both are entered by hand in gba/handtunes.c, which is
 * the one file here that is not the ROM's. They stay hidden until L+R together
 * on the selection screen, and announce themselves with SOUND_CHIRP the way
 * the title's skin does; until then the menu offers the cartridge's four and
 * nothing hints that there are more.
 *
 * KALINKA IS ALREADY HERE and always was: Karinka, the cartridge's own third
 * tune, is Tengen's transliteration of it (Larionov, 1860). Nothing needed
 * adding for it, which is why the hand-entered list is two and not three.
 *
 * They are EXTRA ENTRIES, never extra ROM tracks: kMusicTracks has five, and
 * every place that starts music goes through start_music() below. */
#define MUSIC_KOROBEINIKI MUSIC_COUNT
#define MUSIC_KATIUSKA (MUSIC_COUNT + 1)
#define MUSIC_IS_HANDTUNE(m) ((m) == MUSIC_KOROBEINIKI || (m) == MUSIC_KATIUSKA)
#define MUSIC_HANDTUNE_OF(m) \
    ((uint8_t)((m) == MUSIC_KOROBEINIKI ? HANDTUNE_KOROBEINIKI \
                                        : HANDTUNE_KATIUSKA))

/* AND A LAST ENTRY THAT IS NOT A TUNE. The same L+R that uncovers the two
 * uncovers MUSIC MIX, which plays them all in turn instead of one of them over
 * and over — the cure for a long game spent listening to Loginska.
 *
 * WHEN IT CHANGES, and why it is not "when the tune ends". These tunes loop,
 * and the loop is not something this port is willing to guess at: correlating
 * the melody registers over two hundred seconds of each of them finds a clean
 * loop for exactly one — Troika, 1969 frames — and nothing better than a 17%
 * to 44% match for the others, so any "song length" for them would be a number
 * invented here rather than one traced from the cartridge, which is the thing
 * this project does not do (see CLAUDE.md, ground rule 2).
 *
 * The LEVEL-UP is a boundary the cartridge does define, and a better one
 * musically: the tune already stops there for the dancers and is started again
 * when they finish, so the mix simply hands that restart the next tune. It
 * also means the music changes because you played well, which a timer could
 * never manage. */
#define MUSIC_MIX (MUSIC_COUNT + 2)
#define MUSIC_UNLOCKED_COUNT (MUSIC_COUNT + 3)
/* Six: the cartridge's four and the two entered by hand. Spelt out rather
 * than taken with sizeof, because kMixOrder lives in frontend.c and a
 * declaration of it here would have to carry the length anyway. */
#define MIX_COUNT 6

/* MUSIC_SILENCE IS A STOP FOR ONE PRIORITY CLASS, AND THE TITLE THEME IS NOT
 * IN IT. This is the whole of a bug that took three passes to corner, so it
 * is worth setting down exactly.
 *
 * The engine keeps eleven voice slots. `$0292,y` holds each one's priority,
 * and its top bits are the song's CLASS. Measured on the reference
 * interpreter, slot by slot:
 *
 *     class 7   Loginska, Bradinsky, Karinka, Troika, the level-up jingle
 *     class 8   THE TITLE THEME, and the game-over tune
 *     class 29  the drop, the line clear, the menu click, the chirp
 *     class 62  the screen switch, the top-out
 *
 * `MUSIC_SILENCE` ($08) is not a tune at all: it dispatches to `LD040`
 * (`main.asm.txt:8462-8476`), which walks the slots and frees every one whose
 * class matches `$020B` — and `$08`'s argument is SEVEN. So it stops the four
 * in-game tunes and nothing else. Worse, `LD0E4`'s allocator refuses to evict
 * a slot held at a higher priority (`:8637-8641`), so a class-7 tune can
 * never displace the class-8 theme however it is asked.
 *
 * That is every symptom at once: the theme playing on under GAME SELECT, the
 * theme coming back at LEVEL SELECT and running to its END before the chosen
 * tune could take the slots, and a match started early carrying it along.
 * Suspending only muted it; the slots were still its.
 *
 * The fix is the cartridge's own routine with the argument it is never given:
 * `LD040` begins `sta $020B`, so calling it with 8 frees the title theme's
 * class exactly the way `$08` frees the tunes'. Both together empty every
 * music slot and `$4015` reads zero — real silence, with the effects (classes
 * 29 and 62) untouched, so the screen-switch blip is still heard in full. */
#define NES_AUDIO_STOP_ADDR   0xD040   /* LD040, main.asm.txt:8462 */
#define NES_MUSIC_CLASS_GAME  7        /* what MUSIC_SILENCE frees */
#define NES_MUSIC_CLASS_TITLE 8        /* ...and what it does not */
#define NES_AUDIO_STOP_STEPS  4000     /* eleven slots; a hang guard, not timing */

/* The ROM has a title screen and then separate selection screens, drawn in
 * its own menu frame; this follows the same shape.
 *
 * GAME SELECT is the cartridge's own first menu, and its own wording: the
 * nametable at rows 14-20 spells 1 PLAYER / 2 PLAYER / COOPERATIVE / VERSUS
 * COMPUTER / WITH COMPUTER. All five are here, and so is the screen that
 * follows a game: the cartridge goes from its GAME OVER to its HIGH SCORES
 * table and from there back to the title (somethingWithLeaderboard,
 * main.asm.txt:2643-2675). */
typedef enum {
    SCREEN_TITLE,
    SCREEN_GAME_SELECT,
    SCREEN_LEVEL_SELECT,
    SCREEN_LINK_WAIT,
    SCREEN_PLAYING,
    SCREEN_LEADERBOARD
} Screen;

/* THE CARTRIDGE'S OWN WORDING, and its own order: its GAME SELECT reads
 * 1 PLAYER / 2 PLAYER / COOPERATIVE / VERSUS COMPUTER / WITH COMPUTER at
 * nametable rows 14-20 (main.asm.txt:4742-4830). */
#define GAME_1P   0
#define GAME_2P   1
#define GAME_COOP 2
#define GAME_VS   3
#define GAME_WITH 4
#define GAME_COUNT 5

/* WHAT EACH ONE IS, from playModeTable ($9F51 = `00 01 FF 01 FF`): the five
 * entries map onto three kinds of board. 1 PLAYER is one. 2 PLAYER and
 * VERSUS COMPUTER are two separate ten-wide ones — a race. COOPERATIVE and
 * WITH COMPUTER are one twelve-wide one, shared.
 *
 * The difference between the human pairs and the computer ones is only WHO
 * presses player 2's buttons, which is why these two predicates are separate:
 * the cable modes need a second console, the computer ones need nothing. */
#define GAME_IS_COOP(m)  ((m) == GAME_COOP || (m) == GAME_WITH)
#define GAME_HAS_AI(m)   ((m) == GAME_VS || (m) == GAME_WITH)

/* Both of the cable modes go through the lobby; what they do differently is
 * what the lobby carries and what the board looks like afterwards. */
#define GAME_IS_LINKED(m) ((m) == GAME_2P || (m) == GAME_COOP)

/* ----------------------------------------------------------------------- *
 * THE TITLE, AND THE SKIN L/R SWITCHES TO
 *
 * Tengen shipped this game more than twice. Besides the release there are the
 * prototype cartridges, and they do not all carry the SAME earlier title:
 * one is "TENGEN PRESENTS / TETRIS" over St Basil's in a green fret where the
 * release has its blue braid, one replaces that logo with "THE SOVIET MIND
 * GAME", and one is older than either — "TETRIS" over a Moscow skyline, over
 * four lines ending LICENSED BY NINTENDO OF AMERICA INC., from before the
 * lawsuit. All of them are the cartridges' own art, so L+R on the title
 * cycles through however many the build was given, with SOUND_CHIRP for a
 * doorbell — one of the four effects constants.asm.txt marks "maybe unused",
 * so the egg is announced in the game's own voice by a sound the game itself
 * never plays.
 *
 * The skin is only ever the PICTURE. The cathedral overlay and the fireworks
 * stay on the release screen and are hidden on the prototype's, because they
 * are the release's: their sprites are placed in NES pixels over the release
 * cathedral, and the prototype's composition has neither the same rows nor
 * an empty sky to burst in. Putting them there would be inventing something
 * neither cartridge does.
 *
 * If the port was built without a prototype dump, SCREEN_PROTO_AVAILABLE is 0
 * and L+R has nothing to switch to. See tools/extract_assets.py.
 * ----------------------------------------------------------------------- */
#define TITLE_SKIN_COUNT (1 + SCREEN_PROTO_COUNT)

/* ----------------------------------------------------------------------- *
 * THE TITLE SCREEN'S SPRITES — the cathedral overlay and the fireworks
 *
 * These are not drawn here. They are RUN. Both are subroutines of the
 * cartridge that fill `oamStaging` ($0500) and touch nothing else, and the
 * port already carries a 6502 interpreter with the ROM's own sound engine
 * inside it (nes_audio.h, audio_prg.h) — so it calls them on that same
 * machine, every frame, and copies the sixty-four sprites they leave behind
 * into GBA OAM.
 *
 * That is not laziness, it is the only faithful reading available. The
 * cathedral's eighteen sprites come out of a table the disassembly itself
 * labels "this table is obfuscated": y and x are packed across two bytes and
 * unpacked with an ASL, two LSR/ROR pairs, an AND and a SBC
 * (main.asm.txt:6850-6890), and its only readable description is the worked
 * example in that comment. The fireworks are a little bytecode:
 * `relatedToFireworksTable0` names nine 8x6 tile blocks and drift/recolour
 * steps, picks its own random offsets and palettes, ends the show once
 * frameCounterHigh reaches 4, and fires setMusicOrSoundEffect for each burst
 * — which is why running them on the SOUND ENGINE'S machine matters: the
 * bangs come out of the same RAM the music does, and mix by the cartridge's
 * own priority rules.
 *
 * What the port has to supply is the NMI's bookkeeping, since there is no
 * NMI here: gameState (the routine branches on GAMESTATE_TITLE, both for
 * which sprite table to use and for how long the show lasts), the frame
 * counter it gates on, and the reset value of the RNG seed.
 *
 * Placing them needs one translation. The title art is a COMPOSITION, not a
 * window — ten of the NES's thirty rows are dropped so twenty fit (see
 * TITLE_ROW_BLOCKS) — so a sprite's NES row goes through kTitleRowMap, the
 * same list the artwork was cut with, and a sprite standing on a dropped row
 * is hidden rather than moved somewhere it does not belong.
 * ----------------------------------------------------------------------- */
#define TITLE_OAM_COUNT 64        /* the whole staging page: $0500-$05FF */
#define SPIRE_RIM_OAM TITLE_OAM_COUNT       /* one past it; see draw_spire_rim */

/* THE FIREWORKS ARE ONE OBJECT AND HAVE TO MOVE AS ONE.
 *
 * The per-sprite mapping above is right for the cathedral: those eighteen
 * sprites are fixed artwork lining up with fixed background, so a sprite on a
 * row the composition dropped has nothing left to line up with and goes.
 *
 * A firework is the opposite. `LAA41` (main.asm.txt:5807-5820) walks staging
 * entries $4C upwards adding the same offset to every one of their Y bytes:
 * forty-five sprites, ONE burst, one motion. Sending each of them through the
 * row map individually deletes whichever of them happen to be crossing a
 * dropped row, and since a burst is a RING about forty pixels across it is
 * usually crossing one — which is a ring with a band missing out of its
 * middle, and exactly what "ya no son redondos" is.
 *
 * So the burst is mapped ONCE, by its own centre, and every sprite in it
 * moves by that one offset. Where the burst appears shifts by up to a couple
 * of tiles from where the cartridge puts it; a firework has no business being
 * anywhere in particular, and it stays round. */
#define TITLE_FIREWORK_FIRST 19   /* oamStaging $4C, LA9F7 */
/* Half a burst at its widest, plus the sprite's own eight pixels. Measured on
 * the cartridge: 45 sprites spanning x 167-215 and y 80-128, so 48 across. */
#define TITLE_FIREWORK_R 28

/* The level selector, inside the ROM's own menu frame. The wording matches
 * the cartridge's ("LEVEL SELECT" is one of the strings it writes into this
 * same empty middle), and the digits are its font. */
/* The cartridge's menu frame, which every selection screen is drawn inside. */
/* ----------------------------------------------------------------------- *
 * CENTRING MENU TEXT, and the half tile that stops it looking centred.
 *
 * The menu frame's interior is columns 2-26, twenty-six of them, so its middle
 * is 14.5 — between two tiles. An EVEN number of characters lands on it
 * exactly; an ODD number can only sit half a tile to one side, and since these
 * screens stack odd and even lines on top of each other ("MUSIC" over
 * "LOGINSKA", "LEVEL SELECT" over the ten digits) the mismatch shows as one
 * line sitting off from the one above it. That is the whole of "ahora que miro
 * fino igual casi todo esta descentrado".
 *
 * So the odd lines ride the offset layer, which is idle on these screens, with
 * its scroll set to half a tile. Same trick as the statistics and the title's
 * TENGEN, same reason: art centred on the tile grid whose ink is not.
 * ----------------------------------------------------------------------- */
#define MENU_IN_TX 2
#define MENU_IN_W 26
#define MENU_TEXT_SHIFT_PX 4

/* Where the logo sits on the composed menu: rows 4-6, columns 3-26. That is
 * the cartridge's own rows 10-12 and columns 4-27 after MENU_ROW_BLOCKS and
 * MENU_COL_BLOCKS have taken the two columns and ten rows the GBA lacks. */
#define SKIN_LOGO_TX 3
#define SKIN_LOGO_TY 4

/* ----------------------------------------------------------------------- *
 * THE CREDITS, WHICH THIS PORT HAD BEEN DROPPING
 *
 * The cartridge puts a credit line in the bottom corner of every front-end
 * screen, and it has four of them. Pulled out of the PPU patch chains at
 * $A19B / $A20E / $A280 / $A2DF, which is where each screen's text lives:
 *
 *   GAME SELECT       LICENSED BY MIRRORSOFT LTD.      (row 24, replaced)
 *   LEVEL SELECT      CONCEPT BY ALEXEY PAZHITNOV      (row 24)
 *                     DESIGN BY VADIM GERASIMOV        (row 26)
 *   HANDICAP SELECT   PROGRAMMED BY ED LOGG            (row 24)
 *                     VIDEO GRAPHICS BY KRIS MOSER     (row 26)
 *   MUSIC SELECT      AUDIO BY BRAD FULLER             (row 25)
 *
 * THE PORT WAS SHOWING NONE OF THEM. It folded the last three screens into
 * one page, so five of the six lines had nowhere to go, and the sixth was
 * simply never drawn. What stood here instead was a line this port made up —
 * "BY ALEXEY PAJITNOV" — under a comment calling it "the credit the cartridge
 * never printed". The cartridge printed it, spelled PAZHITNOV, on its level
 * screen. Both the line and the claim are gone.
 *
 * SO THEY TAKE TURNS. Six credits, four frames' worth of screen corner, and
 * two front-end screens to put them on: one at a time, changing every hundred
 * frames or so, in the cartridge's own order and its own words. The only
 * thing here that is not the cartridge's is the LINE BREAK — the menu box's
 * black interior is twenty-six columns (MENU_IN_W) and three of the six lines
 * are twenty-seven or twenty-eight characters, so each is split into the job
 * and the name, which is uniform and loses nothing.
 * ----------------------------------------------------------------------- */
#define CREDIT_TY 16            /* ...and the name on CREDIT_TY + 1 */
#define CREDIT_FRAMES 100
#define CREDIT_COUNT (sizeof kCredits / sizeof kCredits[0])

#define MENU_ARROW_R '>'   /* tile $3E, and ASCII agrees for this one */

#define GAME_SELECT_TY 10
/* One column left of VERSUS COMPUTER, the longest entry: it is fifteen
 * characters centred in MENU_IN_W, so it starts at MENU_IN_TX + 5. */
#define GAME_SELECT_ARROW_TX (MENU_IN_TX + 3)

/* What the lobby is doing, while it does it. Two consoles reach this screen
 * independently, so it has to say which one this is and whether the other has
 * turned up — otherwise a cable that is plugged in badly looks the same as a
 * friend who has not pressed Start yet. */
/* ONE COSSACK, KICKING, WHILE THE OTHER PLAYER CHOOSES.
 *
 * The guest console has nothing to decide and nothing to read — the level and
 * the tune are the master's — so rather than a line of text it gets one of the
 * cartridge's own dancers, in the middle of the screen, going through the same
 * pose table it uses between levels. It is also the honest status light: while
 * he is dancing, the cable is alive.
 *
 * Two-by-two tiles like the rest of them, so he is drawn as four sprites, and
 * at double size so he is a figure rather than a speck. */
#define GUEST_DANCER_X ((SCREEN_TW * 8) / 2 - 8)
/* BELOW THE LINE THAT SAYS WHO YOU ARE, not through it. Row 11 is where
 * "YOU ARE PLAYER 2" is written and the cossack is sixteen pixels tall, so
 * he stands on rows 13-14 with a row of air between them. */
#define GUEST_DANCER_Y (13 * 8)

/* ----------------------------------------------------------------------- *
 * LEVEL SETTINGS — one page, three fields, a cursor
 *
 * This screen has now been all three shapes, and the third is the one that is
 * right for a GBA. Worth setting down why, because the middle one was an
 * appeal to fidelity and fidelity is not what settles it.
 *
 * The cartridge walks FOUR separate gameStates, one setting each, with START
 * between them (`processMenuInput`, main.asm.txt:4711-4726):
 *
 *   GAMESTATE_GAME_TYPE  $FC --START--> initializeLevelSelectMenu   $FD
 *   GAMESTATE_LEVEL_SELECT $FD --START--> initializeHandicapMenu    $FE
 *   GAMESTATE_HANDICAP   $FE --START--> initializeMusicSelectMenu   $FF
 *   GAMESTATE_MUSIC_SELECT $FF --START--> initializeGameMode        (play)
 *
 * and its level list is a COLUMN of ten with a cursor arrow beside it
 * (`p1levelSelectArrowPpuAddrs`, $A0B5: ten PPU addresses one row apart at
 * column 13, and p2's at column 17). Both of those are answers to a problem
 * this port does not have. That console drew to a television watched from
 * across a room: few, large, well-separated lines. This one is held at arm's
 * length, and its problem is the opposite — 240x160, where the constraint is
 * ROOM, not legibility at three metres. Copying the four screens gave three
 * pages that were nearly empty and a tune you chose two screens after you
 * started hearing it. So: one page, the three fields on it, a cursor.
 *
 * What IS kept from the ROM is everything that is a rule rather than a
 * layout. The choice counts come from the six bytes at
 * `computerMoveSelectTable` ($A0E3, main.asm.txt:4835) —
 * `$05,$0A,$0A,$05,$05,$05`, read by LA048 at $A063 as the wrap-around limit:
 * five game types, ten levels for each player, five handicaps each, five
 * tunes. SELECT moves the cursor the way DOWN does, which is LA048's
 * carry-set add ($9FBC). And the cursor arrow is the cartridge's own glyph:
 * `menuArrowTables` (main.asm.txt:4797) is annotated "$3E = right arrow,
 * $3F = left arrow", and this tile set is indexed straight off ASCII, so
 * writing '>' prints the ROM's arrow rather than punctuation.
 * ----------------------------------------------------------------------- */
/* ...but NOT for the left one, and this is a trap worth the line it costs:
 * ASCII's '?' is $3F, and ascii_tile now sends '?' to the question mark the
 * port plants for the pause menu. The left arrow is named by its tile. */
#define T_ARROW_L    0x3F  /* main.asm.txt:4797 */
#define T_ARROW_R    0x3E

#define MENU_FIELD_LEVEL    0
#define MENU_FIELD_HANDICAP 1
#define MENU_FIELD_MUSIC    2
#define MENU_FIELD_COUNT    3

/* The rows the page may write to: everything under the TETRIS logo and above
 * the frame's bottom run. */
#define MENU_BODY_TY 7
#define MENU_BODY_H  11

/* Three fields, TWO rows apart and hung off the middle one. Three rows apart
 * gave each of them two blank rows and that is a table with too much table in
 * it: the three lines read as three separate announcements rather than as one
 * block you are choosing from. Two rows still leaves a clear line of air
 * between them, and the block closes up around HANDICAP — which is where the
 * page's own middle already was, so it does not move and the other two come in
 * to meet it.
 *
 * HANDICAP KEEPS THE ROW UNDER IT. In two players its two counts do not fit
 * beside the value and take a line of their own at +1; MUSIC is at +2, so
 * that line is still free. Row 16 is the last one with air under it: the
 * frame's bottom braid starts at y=145, so a line on row 17 ends one pixel
 * short of it. */
#define MENU_FIELD_TY(f) (9 + (f) * 2)
#define MENU_FOOT_TY 16

/* TWO COLUMNS, centred on what is USUALLY in them. Labels start at one column
 * and values at another, both fixed for all three rows, so the page reads as a
 * table rather than as three sentences.
 *
 * Where to put the two columns is not the same question as how wide to make
 * them. Sized for the worst case — eleven columns, which only KOROBEINIKI
 * ever needs — the block centres on paper and reads a full tile left of centre
 * every other second, because the value there is one digit or an eight-letter
 * tune name. Measured on the built ROM, the three rows' ink spanned x 32..190
 * against an interior running 16..223: sixteen pixels left of its middle.
 * So the columns are placed for a value of EIGHT or so — MUSIC's row then
 * centres on 119.5 exactly — and the one name that is longer runs on into the
 * right margin, where there is still a tile of air before the braid. */
#define MENU_LABEL_TX  (MENU_IN_TX + 5)
/* Nine, not ten: the longest tune's name is eleven characters and the block
 * moving a column right would have run it flush against the braid. Pulling
 * the value column back the same column keeps a tile of air there and tightens
 * the two rows whose value is a single digit, which are the ones that read as
 * left-heavy however well the widest row is centred. */
#define MENU_VALUE_TX  (MENU_LABEL_TX + 9)
#define MENU_CURSOR_TX (MENU_LABEL_TX - 2)
/* ...and anything the value trails, two columns further on. */
#define MENU_TAIL_GAP 2

/* THE ATTRACT DEMO, which is the same computer playing the same game with
 * nobody watching the pad. `demoStart` (main.asm.txt:3216-3230) sets
 * GAMESTATE_DEMO, playMode 0 — one board, one player — suspends the music and
 * drops straight into the ordinary game init, so the only things that make it
 * a demo are who presses the buttons and what a press on the real pad does.
 *
 * It starts off the title's own clock: frameCounterHigh 5, frameCounterLow
 * $20 (main.asm.txt:4154-4160), which is 1312 frames of title — 288 after the
 * fireworks stop themselves at frameCounterHigh 4. The port already feeds
 * that counter to the cartridge's own firework code, so the demo can start
 * off the very same number. */
/* ...AND IT PLAYS AT LEVEL NINE, not at the level the menus are showing.
 * L955B (main.asm.txt:3156-3160) tests gameState before reading
 * menuPlayer1StartLevel: in the states whose id is negative — the title's
 * $FA and the demo's $FB — it substitutes a flat nine instead. So the
 * attract mode has always been a fast game, which is the point of it; the
 * port ran it at zero and it showed, because the pieces fell at a tenth of
 * the speed the cartridge advertises. */
#define DEMO_START_LEVEL 9
#define DEMO_START_FRAME ((5 * 256) + 0x20)
/* How long the demo lingers on its own GAME OVER before the title comes back.
 * The cartridge goes to its high-score table here and from there to the title
 * on another timer; the attract mode does NOT take that road in this port —
 * a score nobody played for has no business on the board — so it takes the
 * shorter one, three seconds, and says so. A real game's own two holds are
 * the cartridge's; see GAMEOVER_HOLD_FRAMES. */
#define DEMO_GAMEOVER_FRAMES 180

/* And how long it looks at a new piece before it touches it — see `settle` on
 * TengenAi. ZERO, which is the cartridge's: its computer shifts off
 * frameCounterLow alone (`and #$07`, main.asm.txt:4170-4176) and will yank a
 * piece sideways on the very frame it spawns.
 *
 * It was half a second for a while, on the argument that an instant twitch
 * reads as a machine rather than as somebody playing. What it actually reads
 * as is a SLOWER computer — thirty frames a piece, a good fifth of the demo's
 * pace at level 0 — and the attract mode is the one place where the port's
 * computer is put side by side with the cartridge's in a player's memory.
 * Fidelity wins; the twitch is the cartridge's twitch. */
#define DEMO_SETTLE_FRAMES 0
/* ...and the computer's own, in a game with a player in it, where it is also
 * the throttle on how fast it fills a shared board. See where it is set. */
#define AI_SETTLE_FRAMES 0
/* Which entry of musicSelectTable. It starts on LOGINSKA rather than on the
 * table's own first entry, which is SILENCE: a player who walks through the
 * menus pressing START should get music. */
/* ----------------------------------------------------------------------- *
 * WHAT THE FRONT END ANSWERS TO
 *
 * Straight out of processMenuInput (main.asm.txt:4614-4702), and not what a
 * modern pad suggests:
 *
 *   on the TITLE      `and #BUTTON_SELECT+BUTTON_START` ($9FA4) — either one
 *                     goes to GAME SELECT.
 *   on a MENU         `and #BUTTON_UP+BUTTON_DOWN+BUTTON_SELECT` ($9FBC and
 *                     $9FED) moves the cursor, and SELECT moves it the same
 *                     way DOWN does: LA048 adds 1 with the carry set unless
 *                     UP is held, in which case it adds -1. START, and only
 *                     START, confirms ($A011).
 *
 * SELECT was missing from both, which is the whole of "SELECT does not
 * select". The cartridge has no back button at all — its menus are a one-way
 * chain with an idle timer that returns to the title — so B here, and A as a
 * second confirm, are the PORT'S, added because a handheld player will try
 * them. They are the only two buttons in this file that are not the ROM's. */
#define MENU_ADVANCE (TENGEN_BTN_SELECT | TENGEN_BTN_START | TENGEN_BTN_A)
#define MENU_STEP    (TENGEN_BTN_UP | TENGEN_BTN_DOWN | TENGEN_BTN_SELECT)
#define MENU_BACKWARD (TENGEN_BTN_UP)
#define MENU_CONFIRM (TENGEN_BTN_START | TENGEN_BTN_A)

/* ----------------------------------------------------------------------- *
 * THE FRONT END'S MUSIC BELONGS TO THE SCREEN
 *
 * The title theme covers the title AND game select — on the cartridge nothing
 * changes the music between them — and the level screen plays whichever tune
 * the cursor is on. Making that a property of the screen rather than a thing
 * each transition remembers to do is what stops the preview following the
 * player back out to the title, which is what it used to do.
 * ----------------------------------------------------------------------- */
#define FRONT_TITLE_THEME 0xFE
#define FRONT_SILENCE     0xFD
#define FRONT_NOTHING     0xFF   /* not a screen's choice: "ask again" */

/* Two seconds without a transfer. Long enough that nothing short of the
 * cable actually coming out reaches it, short enough that the player is not
 * left staring at a frozen board wondering. */
#define LINK_LOST_FRAMES 120

/* Never spend a frame doing nothing but catching up. Two is all the drift
 * between two crystals can ever put in the queue at once. */
#define LINK_MAX_CATCHUP 2

/* ----------------------------------------------------------------------- *
 * The pause menu — L+R while the plaque is up
 *
 * Not the cartridge's: its PAUSE is a plaque and nothing else, and the two
 * things this offers are two things a GBA in somebody's hands wants that a
 * console in front of a television did not. It is built out of the
 * cartridge's own parts, though — the GAME OVER plaque's nine-patch frame
 * ($29/$2A/$2B over $2C/$2F over $3A/$3B/$3C, see kGameOverTiles) and the
 * tile set's own lettering — so it reads as part of the game rather than as
 * something bolted to it.
 *
 * MUSIC changes the tune where the cartridge would have made you start a new
 * game to do it, silence and the MIX included. EXIT leaves, and asks first,
 * because losing a long game to a mis-press is exactly the thing a pause menu
 * is supposed to prevent. Leaving goes out the way every finished game goes
 * out — through the HIGH SCORES page — so a score you quit on still counts.
 * ----------------------------------------------------------------------- */
/* Wide enough for the longest tune's name beside its label, and tall enough
 * to put a line of air between the two choices — the game-over plaque's own
 * four rows would have had them touching. */
/* A NARROW COLUMN, CENTRED, and narrow for a reason: the first shape of this
 * was twenty-two columns wide so a tune's name could sit beside its label,
 * and at that width it reached into both HUD boxes and cut the counters in
 * half. Stacked instead, the widest line IS the longest tune's name, eleven
 * characters — so the box is thirteen columns and lands on the board and its
 * braid, which is frame art, and leaves every counter alone.
 *
 *      PAUSE
 *
 *      MUSIC
 *      KOROBEINIKI
 *      EXIT
 *
 * PAUSE is the heading, because this box IS the pause plaque once the chord
 * has been found: see g_pause_unlocked. The line the cursor is on is picked
 * out by palette rather than by an arrow — an arrow in a centred column is a
 * character that has to come from somewhere, and it pulls the line off
 * centre. */
/* FOURTEEN COLUMNS, AND THIRTEEN WOULD NOT CENTRE. This is the whole reason
 * for the width: the box lands on the board, the board is ten columns at 10-19
 * and its middle is therefore x=120 — the screen's own middle — and a box of
 * ODD width on an even grid cannot be put there. Thirteen columns at column 8
 * spans x 64..167, whose middle is 115.5: four and a half pixels left, which
 * against a playfield you are looking straight at is not a subtlety. Fourteen
 * at the same column spans 64..175 and its middle is 120 exactly.
 *
 * What it costs is the PARITY of everything inside it — see draw_pmenu_line.
 *
 * TEN ROWS, one thing to a row and a blank between every pair:
 *
 *      row 1   -
 *      row 2   PAUSE
 *      row 3   -
 *      row 4   MUSIC
 *      row 5   KOROBEINIKI
 *      row 6   -
 *      row 7   EXIT
 *      row 8   -
 */
#define PMENU_W 14
#define PMENU_H 10
#define PMENU_TX ((SCREEN_TW - PMENU_W) / 2)
#define PMENU_TY ((SCREEN_TH - PMENU_H) / 2)
#define PMENU_IN_TX (PMENU_TX + 1)
#define PMENU_IN_W (PMENU_W - 2)

/* The frame's own tiles, out of the plaque the game over is drawn with. */
#define T_BOX_TL 0x29
#define T_BOX_T  0x2A
#define T_BOX_TR 0x2B
#define T_BOX_L  0x2C
#define T_BOX_R  0x2F
#define T_BOX_BL 0x3A
#define T_BOX_B  0x3B
#define T_BOX_BR 0x3C

/* How far left of a line its cursor sits. See draw_pmenu_line. */
#define PMENU_CURSOR_DX 2

#define PMENU_MUSIC 0
#define PMENU_EXIT  1
#define PMENU_ROWS  2

/* ----------------------------------------------------------------------- *
 * WHAT CROSSES BETWEEN THE FIVE
 *
 * Generated by reading which file names each symbol: a symbol only one
 * file uses is not here, and stays static. The comment on each says
 * where it lives, so this list reads as a map of the split.
 * ----------------------------------------------------------------------- */

/* video.c */
void set_offset_layer(int px);
void set_credit_layer(bool front_end);
extern const char *const kClearWord[5];
extern const uint8_t kPauseTiles[PAUSE_H][PAUSE_W];
uint16_t ascii_tile(char c);
extern TengenLink g_session;
int field_tx(void);
int field_col0(void);
int field_cols(void);
extern uint32_t g_high_score;
extern bool g_ai_active;
extern uint8_t g_view;
void vsync(void);
uint8_t read_buttons(void);
bool shoulder_chord(void);
bool pressed_shoulder(int which);
void upload_tiles(void);
void upload_palette_set(int base, const uint8_t *set, vu16 *memory);
void upload_palettes(void);
void set_field_palette_for_level(uint8_t level);
void set_bank_from_piece(int bank_index, TengenTetromino piece);
void set_piece_palette(TengenTetromino piece);
void set_next_palette(TengenTetromino piece);
void upload_title_tiles(void);
void upload_proto_tiles(int skin);
extern uint8_t g_title_skin;
extern bool g_title_skin_found;
void apply_skin(int skin);
void skin_begin_match(bool linked);
int front_skin(void);
int play_skin(void);
int plaque_bank(void);
void screen_blip(void);
void cursor_blip(void);
uint8_t piece_cell_tile(TengenTetromino piece, uint8_t orientation,
                                int occupied_index);
void upload_sprite_tiles(void);
void oam_set(int index, int x, int y, uint16_t tile, bool hflip, int bank);
void oam_hide_all(void);

/* hud.c */
void draw_coop_dancers(int elapsed, int count);
void draw_dancers(int elapsed, int count);
void dancers_begin(uint16_t seed, int cast);  /* the interlude's choreography; see hud.c */
void dancers_step(int frame);
extern uint8_t g_idle_palette;
extern int g_dance_frames;
void idle_cossack_celebrate(int lines);
void draw_line_clear_sweep(void);
int hud_clearing_slot(void);     /* whose rows are coming down; see hud.c */
void points_clear(void);
void note_award(int slot, TengenStepResult step);
void draw_points(void);
extern bool g_panel_layer;
void set_map_tile(int tx, int ty, uint16_t entry);
void clear_region(int tx, int ty, int w, int h);
void clear_panel_region(int tx, int ty, int w, int h);
void set_stats_tile(int tx, int ty, uint16_t entry);
void set_histogram_tile(int tx, int ty, uint16_t entry);
void clear_both(int tx, int ty, int w, int h);
extern bool g_show_banner;
void draw_field_braid(int tx, const uint8_t run[1][2]);
void draw_dancer_stage(void);
unsigned text_len(const char *s);
void draw_text(int tx, int ty, const char *text, int bank);
void draw_static_screen(void);
void draw_game_over(void);
extern bool g_bonus_showing;
extern bool g_bonus_dirty;
void draw_bonus_static(void);
void draw_bonus_numbers(void);
void bonus_begin(void);
void bonus_step(void);
void bonus_end(void);
extern int g_leader_row;
bool leader_load(void);
void leader_reset(void);
void leader_reset_table(int table);
void leader_use_table(int skin);   /* -1 release, 0.. the prototypes */
void draw_leader_row(int row);
void draw_leaderboard(void);
void leader_submit(void);
bool leader_type(uint8_t held, uint8_t pressed);
extern int g_idle_frame;
extern bool g_dancer_active;
extern int g_dancer_elapsed;
void clear_stats_layer(void);
void clear_stats_layer_at(int tx);
void draw_counter(int ty, int label_first, int label_count,
                          uint32_t value, int digits, int value_indent);
void draw_panel(void);
void draw_field(void);
void draw_pause_box(void);

/* frontend.c */
extern bool g_pause_unlocked;
extern bool g_xe;
bool unlock_cheats(void);
uint8_t start_level_choices(void);
extern const char *const kMusicNames[MUSIC_UNLOCKED_COUNT];
extern const uint8_t kMixOrder[MIX_COUNT];
extern uint8_t g_mix_step;
uint8_t mix_tune(void);
uint8_t music_choices(void);
void audio_frame(void);
void stop_music_class(uint8_t klass);
void stop_music(void);
void start_music(uint8_t music);
void clear_screen(void);
void install_title_palette(void);
void draw_title(void);
extern uint16_t g_title_frame;
void init_title_sprites(void);
void restart_title_sprites(void);
void draw_title_sprites(void);
extern const char *const kCredits[][2];
void draw_game_select(uint8_t choice);
void draw_link_wait(const TengenLobby *lobby, int elapsed);
void draw_level_settings(int chosen, uint8_t start_level, uint8_t music,
                                 const uint8_t handicap[2], bool two_player,
                                 int handicap_who);

/* match.c */
extern TengenAi g_ai;
extern uint8_t g_ai_slot;
extern TengenTetromino g_ai_last_piece;
extern TengenTetromino g_ai_last_partner;
extern uint8_t g_ai_frame;
extern bool g_demo;
extern int g_demo_over_frames;
extern bool g_linked;
extern bool g_link_lost;
extern bool g_repaint;
extern uint8_t g_front_tune;
extern uint8_t g_music;
extern uint8_t g_dancer_timer;
extern uint16_t g_dancer_tick;
extern int g_dancer_cast;
extern uint8_t g_shown_level;
extern TengenTetromino g_shown_piece;
extern TengenTetromino g_shown_piece2;
uint8_t ai_input(void);
extern bool g_pause_confirm;
void swallow_held_buttons(TengenGame *game);
bool link_play_frame(void);
void front_music(uint8_t which);
bool solo_play_frame(uint8_t buttons, uint8_t pressed, bool *quit);
void draw_match(bool *sweeping);

#endif /* PORT_H */
