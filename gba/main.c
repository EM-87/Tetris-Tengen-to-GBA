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
#include "korobeiniki.h"
#include "audio_prg.h"
#include "link.h"
#include "../src/tengen_core.h"
#include "../src/tengen_link.h"

/* Generated from a cartridge dump by tools/extract_assets.py. */
#include "tiles_game.h"
#include "tiles_dancers.h"
#include "tiles_title_obj.h"
#include "tiles_title.h"
#include "screen_1p.h"
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
 * tile row either: four counters of three rows each fill rows 2 to 13 and the
 * preview takes 14 to 17. So the counters get a layer of their own, scrolled
 * two pixels down, for the same reason and at the same price as the
 * statistics got theirs. The braid stays on BG0 and does not move. */
#define SCREENBLOCK_PANEL 30
#define PANEL_SHIFT_PX 2

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

/* The offset layer's scroll. A negative scroll moves the picture the other
 * way and the field is nine bits wide, so -n is written as 512-n. */
static void set_offset_layer(int px) {
    REG_BG1HOFS = (uint16_t)(512 - px);
}

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
#define FIELD_TX SCREEN_1P_FIELD_TX  /* port column of the first playable one */
#define FIELD_TY 0   /* the field starts at the top of the visible window */
#define FIELD_COL0 1              /* storage column of the first playable one */
#define FIELD_PLAYABLE (TENGEN_PF_WIDTH - 2)

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
#define BOX_BOT_IN 17                /* last interior row */

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
#define PAL_OBJ_TITLE 5           /* banks 5-8: spritePalette1 */

/* lineClearSingle..lineClearTetris (main.asm.txt:1548-1561), one character
 * per playfield column including the walls, exactly as the ROM stores them.
 * Indexed by how many rows are coming down. */
static const char *const kClearWord[5] = {
    "",
    " SINGLE     ",
    " DOUBLE     ",
    " TRIPLE     ",
    " TETRIS     ",
};

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

static const uint8_t kPauseTiles[PAUSE_H][PAUSE_W] = {
    {0x10, 0x11, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0x12},
    {0x13, 0x14, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0x15},
};

#define WITH_BANK(tile, bank) ((uint16_t)((tile) | ((bank) << 12)))

/* Tile ids in the cartridge's own tileset, taken from the nametable it ships
 * (see reference/NOTES.md). Reusing the game's lettering is the difference
 * between the port looking like Tengen Tetris and looking like a clone. */
#define T_BLANK      0x00
#define T_RULE_LEFT  0x75
#define T_RULE_MID   0x76
#define T_RULE_RIGHT 0x79


/* The tileset's letters and digits sit at their ASCII codes, which is how the
 * ROM's own nametable spells "HIGH SCORE" and "STATS". */
static uint16_t ascii_tile(char c) { return (uint16_t)(unsigned char)c; }

/* OFFSETS THE HEADLESS CHECKS NEED, exported so the ELF is the single place
 * that knows them — the same reason kNes6502Probe exists in nes_audio.c, and
 * for the same reason it was added there: a new field anywhere in TengenGame
 * moves every one of these, and a Python constant that did not move would
 * quietly start reading a neighbour. Adding `garbage_rng` did exactly that
 * and the cheat-code check began failing three tests away from the change. */
const uint16_t kGameProbe[10] = {
    (uint16_t)offsetof(TengenGame, field),
    (uint16_t)offsetof(TengenGame, player),
    (uint16_t)sizeof(TengenPlayerState),
    (uint16_t)offsetof(TengenPlayerState, piece.current),
    (uint16_t)offsetof(TengenPlayerState, piece.y),
    (uint16_t)offsetof(TengenPlayerState, level),
    (uint16_t)offsetof(TengenPlayerState, piece_stats),
    (uint16_t)offsetof(TengenGame, paused),
    (uint16_t)offsetof(TengenPlayerState, held_last_frame),
    (uint16_t)offsetof(TengenPlayerState, piece.next),
};

static TengenLink g_session;
/* The best score of this session. The cartridge's own 1P panel shows one
 * (see draw_panel), and like the cartridge's it does not survive a reset. */
static uint32_t g_high_score;
/* Which player this console shows and plays. Always 0 in a solo game; in a
 * linked match it is the cable master that is player 1, so the two consoles
 * differ here and nowhere else. */
static uint8_t g_view;

static void vsync(void) {
    while (REG_VCOUNT >= 160) { }
    while (REG_VCOUNT < 160) { }
}

/* The GBA has every button the NES did, so this is a straight 1:1 remap with
 * no compromises — which is why the controls can be faithful. */
static uint8_t read_buttons(void) {
    uint16_t keys = (uint16_t)(~REG_KEYINPUT & KEY_MASK); /* KEYINPUT is active low */
    uint8_t out = 0;
    if (keys & KEY_A)      out |= TENGEN_BTN_A;
    if (keys & KEY_B)      out |= TENGEN_BTN_B;
    if (keys & KEY_SELECT) out |= TENGEN_BTN_SELECT;
    if (keys & KEY_START)  out |= TENGEN_BTN_START;
    if (keys & KEY_UP)     out |= TENGEN_BTN_UP;
    if (keys & KEY_DOWN)   out |= TENGEN_BTN_DOWN;
    if (keys & KEY_LEFT)   out |= TENGEN_BTN_LEFT;
    if (keys & KEY_RIGHT)  out |= TENGEN_BTN_RIGHT;
    return out;
}

/* The same read, for the serial interrupt to call at the instant of a
 * transfer (see link.h). Nothing else may go in here. */
uint8_t link_read_buttons(void) { return read_buttons(); }

/* L and R have no NES equivalent, so the game proper never sees them and
 * they are free for the port's own switches. This reports the two together,
 * as a fresh press. */
static bool shoulder_chord(void) {
    static bool was_held;
    uint16_t keys = (uint16_t)(~REG_KEYINPUT & KEY_MASK);
    bool held = (keys & KEY_L) && (keys & KEY_R);
    bool pressed = held && !was_held;
    was_held = held;
    return pressed;
}

/* ...and ONE of them on its own, for the level screen's handicap: L belongs
 * to player 1's side of the pad and R to player 2's. Each keeps its own held
 * state so neither can swallow the other's press, and the caller checks the
 * chord first so reaching for the fifth tune buries nobody. */
#define SHOULDER_L 0
#define SHOULDER_R 1

static bool pressed_shoulder(int which) {
    static bool was_held[2];
    uint16_t keys = (uint16_t)(~REG_KEYINPUT & KEY_MASK);
    bool held = (keys & (which == SHOULDER_L ? KEY_L : KEY_R)) != 0;
    bool pressed = held && !was_held[which];
    was_held[which] = held;
    return pressed;
}

/* ...and EITHER of them, for the title's skin switch — the user asked for
 * "L or R", and on the title there is no other use for them. Its own held
 * state, so a chord elsewhere cannot swallow this one's press. */
static bool shoulder_either(void) {
    static bool was_held;
    uint16_t keys = (uint16_t)(~REG_KEYINPUT & KEY_MASK);
    bool held = (keys & (KEY_L | KEY_R)) != 0;
    bool pressed = held && !was_held;
    was_held = held;
    return pressed;
}

static void upload_tiles(void) {
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK);
    const uint8_t *src = kGameTiles;
    for (unsigned i = 0; i < sizeof(kGameTiles); i += 2) {
        dst[i / 2] = (uint16_t)(src[i] | (src[i + 1] << 8));
    }
}

/* One updatePalette set — four palettes of four entries — into four GBA
 * banks, exactly as the cartridge stores them. */
static void upload_palette_set(int base, const uint8_t *set, vu16 *memory) {
    for (int bank = 0; bank < 4; bank++) {
        vu16 *dst = memory + (base + bank) * 16;
        for (int i = 0; i < 4; i++) dst[i] = nes_colour_to_gba(set[bank * 4 + i]);
    }
}

static void upload_palettes(void) {
    upload_palette_set(PAL_GAME_BASE, kRomPalette_bg_game, MEM_PALETTE);
    upload_palette_set(PAL_TITLE_BASE, kRomPalette_bg_title, MEM_PALETTE);
    upload_palette_set(PAL_MENU_BASE, kRomPalette_bg_menu, MEM_PALETTE);
    vu16 *piece = MEM_PALETTE + PAL_PIECE_BANK * 16;
    piece[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    vu16 *next = MEM_PALETTE + PAL_NEXT_BANK * 16;
    next[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
}

/* setPlayfieldPaletteFromLevel (main.asm.txt:5328) recolours the settled
 * field using the LEVEL'S ONES DIGIT as the index, which is why the colours
 * cycle every ten levels. It writes background palette 0, entries 1-3. */
static void set_field_palette_for_level(uint8_t level) {
    const uint8_t *entry = kRomPiecePalettes[level % 10];
    vu16 *bank0 = MEM_PALETTE + PAL_GAME_BASE * 16;
    for (int i = 0; i < 3; i++) bank0[1 + i] = nes_colour_to_gba(entry[i]);
}

/* setPiecePalette (main.asm.txt:5338) indexes the same table by PIECE ID and
 * writes a sprite palette, so the falling piece carries its own colours while
 * everything settled shares the level's. */
static void set_bank_from_piece(int bank_index, TengenTetromino piece) {
    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return;
    const uint8_t *entry = kRomPiecePalettes[piece];
    vu16 *bank = MEM_PALETTE + bank_index * 16;
    for (int i = 0; i < 3; i++) bank[1 + i] = nes_colour_to_gba(entry[i]);
}

static void set_piece_palette(TengenTetromino piece) {
    set_bank_from_piece(PAL_PIECE_BANK, piece);
}

/* The preview's, from the SAME table and the same rule — only indexed by the
 * piece that is coming rather than the one that is here. */
static void set_next_palette(TengenTetromino piece) {
    set_bank_from_piece(PAL_NEXT_BANK, piece);
}

static void upload_title_tiles(void) {
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK) + TITLE_TILE_BASE * 16;
    for (unsigned i = 0; i < sizeof(kTitleTiles); i += 2) {
        dst[i / 2] = (uint16_t)(kTitleTiles[i] | (kTitleTiles[i + 1] << 8));
    }
#if SCREEN_PROTO_AVAILABLE
    /* The prototype's own bank, above the release's. Tile ids in a text-mode
     * background are ten bits, so 512-767 is still addressable from
     * charblock 0, and screenblock 28 starts well past it. */
    vu16 *pdst = MEM_CHARBLOCK(CHARBLOCK) + PROTO_TILE_BASE * 16;
    for (unsigned i = 0; i < sizeof(kProtoTiles); i += 2) {
        pdst[i / 2] = (uint16_t)(kProtoTiles[i] | (kProtoTiles[i + 1] << 8));
    }
#endif
}

/* The cartridge's whole sprite bank, uploaded once. Both the dancers and the
 * line-clear puff live in it, so every sprite tile id in this file is the
 * ROM's own index. */
static void upload_sprite_tiles(void) {
    vu16 *dst = MEM_OBJ_TILES;
    for (unsigned i = 0; i < sizeof(kDancerTiles); i += 2) {
        dst[i / 2] = (uint16_t)(kDancerTiles[i] | (kDancerTiles[i + 1] << 8));
    }
    /* The level-up interlude installs spritePalette2 (main.asm.txt:2044), and
     * the dancers pick among its four palettes with their own attribute bytes
     * — which is why the six are not all the same colour. */
    upload_palette_set(PAL_OBJ_DANCER, kRomPalette_obj_dancers, MEM_PALETTE_OBJ);

    /* CHR bank 3, the title screen's sprite bank, above the dancers' 256:
     * the cathedral overlay at tiles $02-$13, the sparkles at $14-$17 and the
     * firework bursts from $90 up. The title installs spritePalette1 for them
     * (main.asm.txt:4492-4494). */
    vu16 *tdst = MEM_OBJ_TILES + TITLE_OBJ_TILE_BASE * 16;
    for (unsigned i = 0; i < sizeof(kTitleObjTiles); i += 2) {
        tdst[i / 2] = (uint16_t)(kTitleObjTiles[i] | (kTitleObjTiles[i + 1] << 8));
    }
    upload_palette_set(PAL_OBJ_TITLE, kRomPalette_obj_title, MEM_PALETTE_OBJ);

    /* The HUD labels, with the header grid's vertical stubs masked out of
     * them; see read_hud_labels in tools/extract_assets.py. */
    vu16 *ldst = MEM_CHARBLOCK(CHARBLOCK) + HUD_LABEL_TILE_BASE * 16;
    for (unsigned i = 0; i < sizeof(kHudLabelTiles); i += 2) {
        ldst[i / 2] = (uint16_t)(kHudLabelTiles[i] | (kHudLabelTiles[i + 1] << 8));
    }

    /* piecePaletteIndexA, "Line clears" (main.asm.txt:5394-5396). */
    const uint8_t *clear = kRomPiecePalettes[10];
    vu16 *cpal = MEM_PALETTE_OBJ + PAL_OBJ_CLEAR * 16;
    cpal[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    for (int i = 0; i < 3; i++) cpal[1 + i] = nes_colour_to_gba(clear[i]);
}

static void oam_set(int index, int x, int y, uint16_t tile, bool hflip, int bank) {
    vu16 *entry = MEM_OAM + index * 4;
    entry[0] = (uint16_t)OBJ_ATTR0_Y(y);
    entry[1] = (uint16_t)(OBJ_ATTR1_X(x) | (hflip ? OBJ_ATTR1_HFLIP : 0));
    entry[2] = (uint16_t)(tile | OBJ_ATTR2_PAL(bank));
}

static void oam_hide_all(void) {
    for (int i = 0; i < 128; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

/* Places the dancers for one frame of the interlude, in the ROM's own
 * positions: one column of six, 24 pixels apart, walking right off their
 * starting mark onto the ledges. */
static void draw_dancers(int elapsed, int count) {
    int pose_step = elapsed / DANCER_POSE_FRAMES;
    int walk = elapsed / DANCER_WALK_FRAMES;

    if (count > DANCER_COUNT) count = DANCER_COUNT;
    for (int d = 0; d < count; d++) {
        /* Staggered starting poses so the six are not in lockstep. This is
         * the stand-in for the per-dancer script; see the note above. */
        int pose = (pose_step + d * 7) % DANCER_POSE_COUNT;
        const uint8_t *tiles = kDancerPoses[pose];

        int x = DANCER_STAGE_TX * 8 - DANCER_START_OFFSET + walk;
        int y = (int)kDancerStartY[d] - DANCER_Y_ORIGIN;
        /* They walk on from the left and STOP IN THE MIDDLE OF THE LEDGE.
         * Where the cartridge stops them is in the choreography scripts,
         * which are not traced, so the port has to choose — and the far edge
         * it used to choose left every one of them half hanging off its own
         * ledge, and off the column the banner's letters occupy, which is the
         * same four columns (DANCER_STAGE_TX == BANNER_TX, both four wide,
         * exactly as the cartridge has them at nametable column 14). */
        int limit = DANCER_STAGE_TX * 8 + (DANCER_STAGE_TW * 8 - 16) / 2;
        if (x > limit) x = limit;

        for (int s = 0; s < DANCER_SPRITES; s++) {
            int sx = x + ((s & 1) ? 8 : 0);
            int sy = y + ((s & 2) ? 8 : 0);
            oam_set(d * DANCER_SPRITES + s, sx, sy, tiles[s], false,
                     PAL_OBJ_DANCER + (kDancerAttr[d] & 3));
        }
    }
    /* L8E42-8E53 blanks the sprites the smaller cast does not use; everything
     * else on screen during the interlude is background. */
    for (int i = count * DANCER_SPRITES; i < 128; i++)
        MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}


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
#define IDLE_POSE_FRAMES 48        /* a slow sway, not the show's eight */
static const uint8_t kIdlePoses[2] = { 0, 1 };

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

static uint8_t g_idle_palette;
static int g_dance_frames;      /* frames of the reaction still to play */
static int g_dance_length;      /* ...and how many it started with */
/* THE LEVEL-UP SHOW, DANCED ALONE. The cartridge's interlude sends a troupe
 * out onto a stage that takes the whole right-hand column — which is the
 * TETRIS banner's column, so it can only be had by giving up the statistics.
 * In HUD STATS the interlude still happens, with its own music and its own
 * traced 32 seconds, but the screen stays exactly where it was and the one
 * cossack who is already standing there dances it by himself. The troupe is
 * what HUD BANNER is FOR: it is the harder way to play, since it costs you
 * the piece histogram, and the six of them are what it pays back. */
static bool g_idle_show;

static void hide_idle_cossack(void) {
    for (int i = 0; i < DANCER_SPRITES; i++)
        MEM_OAM[(IDLE_OAM_BASE + i) * 4] = OBJ_ATTR0_HIDDEN;
}

/* Starts the reaction. `lines` is 1-4; anything else is ignored. */
static void idle_cossack_celebrate(int lines) {
    if (lines < 1) return;
    if (lines > 4) lines = 4;
    int poses = lines * DANCE_POSES_PER_LINE;
    if (poses > DANCE_MAX_POSES) poses = DANCE_MAX_POSES;
    g_dance_length = poses * DANCE_POSE_FRAMES;
    g_dance_frames = g_dance_length;
}

/* `running` false freezes him where he stands — which is what a game over
 * should look like from the wings. */
static void draw_idle_cossack(int elapsed, bool running,
                               int tx, int ty, int w, int h) {
    int pose;
    if (g_idle_show) {
        /* Round and round the figure for as long as the show lasts, at the
         * show's own eight-frame cadence. */
        int steps = DANCER_POSE_COUNT - DANCE_FIRST_POSE;
        pose = DANCE_FIRST_POSE + (elapsed / DANCE_POSE_FRAMES) % steps;
    } else if (g_dance_frames > 0) {
        int done = (g_dance_length - g_dance_frames) / DANCE_POSE_FRAMES;
        pose = DANCE_FIRST_POSE + done;
        if (pose >= DANCER_POSE_COUNT) pose = DANCER_POSE_COUNT - 1;
        if (running) g_dance_frames--;
    } else {
        pose = kIdlePoses[(elapsed / IDLE_POSE_FRAMES) & 1];
    }
    const uint8_t *tiles = kDancerPoses[pose];
    int x = tx * 8 + (w * 8 - 16) / 2;
    int y = ty * 8 + (h * 8 - 16) / 2;
    for (int s = 0; s < DANCER_SPRITES; s++)
        oam_set(IDLE_OAM_BASE + s, x + ((s & 1) ? 8 : 0), y + ((s & 2) ? 8 : 0),
                 tiles[s], false, PAL_OBJ_DANCER + g_idle_palette);
}

/* The puff of smoke crossing each completed row: five sprites in a row, the
 * head at the column the sweep has reached and the rest trailing one column
 * apart behind it, each retiring as it leaves the field. */
static void draw_line_clear_sweep(void) {
    const TengenPlayerState *p = &g_session.game.player[g_view];
    uint8_t step = tengen_line_clear_step(&g_session.game, (TengenPlayerSlot)g_view);
    int used = 0;

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        if (!(p->clearing_rows & (1u << row))) continue;
        for (int s = 0; s < TENGEN_CLEAR_SPARKS; s++) {
            int col = (int)step - TENGEN_CLEAR_TRAIL + s;
            if (col < 0 || col >= FIELD_PLAYABLE) continue;
            oam_set(used++, (FIELD_TX + col) * 8, (FIELD_TY + row) * 8,
                     (uint16_t)(CLEAR_HEAD_TILE + s), false, PAL_OBJ_CLEAR);
        }
    }
    /* ...but not the four at the top: the idle cossack lives there and this
     * runs after the panel has drawn him. */
    for (int i = used; i < IDLE_OAM_BASE; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

/* While this is on, everything that goes through set_map_tile lands on the
 * counters' layer instead of the main one. It is a switch rather than a
 * second set of drawing functions because the panel is drawn with the same
 * draw_text / draw_number / draw_rule the menus use, and those should not
 * have to know which background they are writing to. */
static bool g_panel_layer;

static void set_map_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(g_panel_layer ? SCREENBLOCK_PANEL : SCREENBLOCK)
        [ty * MAP_W + tx] = entry;
}

static void clear_region(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) set_map_tile(tx + x, ty + y, T_BLANK);
}

static void clear_panel_region(int tx, int ty, int w, int h) {
    bool was = g_panel_layer;
    g_panel_layer = true;
    clear_region(tx, ty, w, h);
    g_panel_layer = was;
}

/* The offset layer. Tile 0 of the cartridge's set is transparent in every
 * pixel, so everywhere this map is not written the screen is simply the one
 * below it. */
static void set_stats_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK_STATS)[ty * MAP_W + tx] = entry;
}

/* The histogram's own. Same three pixels across, none down. */
static void set_histogram_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK_HISTOGRAM)[ty * MAP_W + tx] = entry;
}

/* Wipes a rectangle off BOTH maps. Anywhere the offset layer might be holding
 * something has to be cleared this way or half a drawing survives. */
static void clear_both(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            set_map_tile(tx + x, ty + y, T_BLANK);
            set_stats_tile(tx + x, ty + y, T_BLANK);
        }
}

/* ----------------------------------------------------------------------- *
 * A BOX OF BRAID.
 *
 * The blue rope that runs down either side of the playfield, closed into a
 * rectangle. Its corners and horizontal runs are the ones the cartridge
 * borders its whole screen with (kBraid*, see read_braid_frame in
 * tools/extract_assets.py), so this is the game's own frame and not a
 * lookalike.
 *
 * IT IS TWO TILES THICK, and that is not adjustable: each tile is half the
 * rope cut lengthwise. So a box ten columns wide has a six-column interior,
 * and that single fact decides the whole HUD below.
 * ----------------------------------------------------------------------- */
#define BRAID_T 2                 /* the frame's thickness, in tiles */

/* The two columns of rope that frame the playfield, as a plain strip from top
 * to bottom. Used when the banner or the dancers take the rest of the column:
 * clearing around the box leaves its corners dangling, and the cartridge's own
 * screen has a plain strip here anyway.
 *
 * THE RUN MUST BE THE PANEL'S OWN. This strip replaces the right panel's left
 * side, so it is kBraidLeft — the same tile draw_braid_panel puts there. Hand
 * it the other one and the weave changes direction every time L+R is pressed,
 * which is exactly what it used to do. */
static void draw_field_braid(int tx, const uint8_t run[1][2]) {
    for (int y = 0; y < SCREEN_TH; y++)
        for (int dx = 0; dx < BRAID_T; dx++)
            set_map_tile(tx + dx, y, WITH_BANK(run[0][dx], BRAID_BANK));
}

/* A panel of rope: the braid along the top, the bottom and the side facing the
 * board, and the screen's own edge closing it outward. `inner_right` says
 * which side of the panel the board is on.
 *
 * WHICH TILES, AND WHY THAT WAY ROUND. The rope is woven and the weave leans,
 * so its four runs and four corners only fit each other one way. THE PANEL IS
 * A BOX, and the tile is chosen by which side OF THE BOX it is on, not by
 * which side of the board:
 *
 *   left panel  (cols 0-9)   rope on its RIGHT side  -> kBraidRight, TR / BR
 *   right panel (cols 20-29) rope on its LEFT side   -> kBraidLeft,  TL / BL
 *
 * Choosing by the board instead — the run beside the board's left edge taking
 * the cartridge's own left-border tiles — puts each vertical run back where
 * the ROM has it, but then the corners it meets are the mirror of it and the
 * weave visibly breaks at every one of them. The box wins: it is a box now,
 * and its own four pieces have to agree with each other.
 *
 * What that costs is one thing, and it is paid where nobody looks: the rope
 * beside the playfield is the mirror of the cartridge's. What it must NOT
 * cost is the weave changing direction when the HUD does, so anything that
 * redraws these columns as a plain strip has to use the panel's own tile —
 * see draw_field_braid's callers.
 *
 * The corners are only ever drawn on the board side, because that is the only
 * side that has one — the other simply runs off the screen. */
static void draw_braid_panel(int tx, int w, bool inner_right) {
    int ix = inner_right ? tx + w - BRAID_T : tx;   /* the inner run's column */

    for (int dy = 0; dy < BRAID_T; dy++) {
        for (int dx = 0; dx < BRAID_T; dx++) {
            set_map_tile(ix + dx, dy,
                          WITH_BANK(inner_right ? kBraidTR[dy][dx]
                                                : kBraidTL[dy][dx], BRAID_BANK));
            set_map_tile(ix + dx, SCREEN_TH - BRAID_T + dy,
                          WITH_BANK(inner_right ? kBraidBR[dy][dx]
                                                : kBraidBL[dy][dx], BRAID_BANK));
        }
    }
    for (int x = 0; x < w; x++) {
        int cx = tx + x;
        if (cx >= ix && cx < ix + BRAID_T) continue;      /* the corners */
        for (int dy = 0; dy < BRAID_T; dy++) {
            set_map_tile(cx, dy, WITH_BANK(kBraidTop[dy][0], BRAID_BANK));
            set_map_tile(cx, SCREEN_TH - BRAID_T + dy,
                          WITH_BANK(kBraidBottom[dy][0], BRAID_BANK));
        }
    }
    for (int y = BRAID_T; y < SCREEN_TH - BRAID_T; y++)
        for (int dx = 0; dx < BRAID_T; dx++)
            set_map_tile(ix + dx, y,
                          WITH_BANK(inner_right ? kBraidRight[0][dx]
                                                : kBraidLeft[0][dx], BRAID_BANK));

    clear_region(inner_right ? tx : tx + BRAID_T, BRAID_T, w - BRAID_T,
                  SCREEN_TH - 2 * BRAID_T);
}

/* The stage the level-up blit paints where the banner was. */
static void draw_dancer_stage(void) {
    for (int y = 0; y < DANCER_STAGE_TH; y++)
        for (int x = 0; x < DANCER_STAGE_TW; x++)
            set_map_tile(DANCER_STAGE_TX + x, DANCER_STAGE_TY + y,
                          WITH_BANK(kDancerStage[y][x], 1));
    /* The sixth ledge, standing in for the screen border the cartridge's
     * bottom dancer uses; see the note beside DANCER_LIFT. */
    for (int x = 0; x < DANCER_STAGE_TW; x++)
        set_map_tile(DANCER_STAGE_TX + x, DANCER_FLOOR_TY,
                      WITH_BANK(kDancerStage[3][0], 1));
}


static void draw_text(int tx, int ty, const char *text, int bank) {
    for (int i = 0; text[i]; i++) set_map_tile(tx + i, ty, WITH_BANK(ascii_tile(text[i]), bank));
}

static void draw_number(int tx, int ty, uint32_t value, int digits, int bank) {
    for (int i = digits - 1; i >= 0; i--) {
        set_map_tile(tx + i, ty, WITH_BANK(ascii_tile((char)('0' + (value % 10))), bank));
        value /= 10;
    }
}

/* Paints the cartridge's own screen: the braided border, every decorative
 * tile, each with the palette the ROM's attribute table assigns it — and then
 * closes the two bare strips of rope into panels. Their inner sides land
 * exactly where the cartridge's own vertical runs already are, so the
 * playfield keeps the frame it had; the rope simply carries on round the HUD
 * instead of stopping. */
static void draw_static_screen(void) {
    for (int ty = 0; ty < SCREEN_TH; ty++) {
        int layout_row = ty + WINDOW_TOP;
        for (int tx = 0; tx < SCREEN_TW; tx++) {
            int i = layout_row * SCREEN_1P_W + tx;
            set_map_tile(tx, ty, WITH_BANK(kScreen1pTiles[i], kScreen1pPalettes[i]));
        }
    }
    draw_braid_panel(BOX_L_TX, BOX_W, true);
    draw_braid_panel(BOX_R_TX, BOX_W, false);
    /* Back from whatever the title lent it; see set_offset_layer. */
    set_offset_layer(STATS_SHIFT_PX);
}

/* gameOverTiles, blitted where the cartridge blits it: nametable (4,12),
 * which is the middle of the playfield (gameOver1pPPUAddr1 = $2184,
 * gameOver1pColsRows1 = 6 columns by 4 rows, gameOverAttrs = palette 3). */
#define GAMEOVER_TX (FIELD_TX + 2)
#define GAMEOVER_TY 4

static void draw_game_over(void) {
    for (int y = 0; y < SCREEN_1P_GAMEOVER_H; y++)
        for (int x = 0; x < SCREEN_1P_GAMEOVER_W; x++)
            set_map_tile(GAMEOVER_TX + x, GAMEOVER_TY + y,
                          WITH_BANK(kGameOverTiles[y][x], BANK_PAUSE));
}

/* The cartridge draws its whole header strip — SCORE, LINES, LEVEL, NEXT,
 * STATS, the rules under them and the counters themselves — in background
 * palette 3, which is read straight off its attribute table (rows 2-7 of the
 * 1P screen are solidly bank 3). Same lettering, same colours. */
#define BANK_LABEL 3
#define BANK_VALUE 3
/* The menu runs on bgPalette1, whose banks are not the game's: 3 there is the
 * white lettering and 1 is an orange that reads clearly against it, which is
 * what marks the chosen entry. */
#define BANK_HILITE (PAL_MENU_BASE + 1)

#define NEXT_CELL_W 4

/* CENTRED IN ITS CELL, and the last three pixels of it come from the same
 * place the statistics' do.
 *
 * The orientation bitmaps put each piece where the PLAYFIELD wants it — an O
 * at columns 0-1, an I across all four — so drawing them at their bitmap
 * column left everything but the I against the left wall of a box that is the
 * port's own, under a NEXT that is centred. Squaring the bounding box up in
 * the four-tile cell fixes the two-tile part of that.
 *
 * What it cannot fix is the half tile. The block art has a one-pixel inset on
 * its left, so a piece w tiles wide is 8w-1 pixels of ink, and centring that
 * in the panel's 64 wants its first tile at (65-8w)/16 - an EVEN number of
 * tiles for an even w, and half a tile out for an odd one. Two tiles wide and
 * four tiles wide land within half a pixel of centre; three tiles wide - the
 * T, J, L, S and Z, so five pieces of seven - lands three and a half pixels
 * left, which is what "alineadas a la izquierda" still was after the columns
 * were right.
 *
 * Those pieces are drawn on the STATISTICS LAYER instead, which is already
 * scrolled three pixels for its own reasons (see SCREENBLOCK_STATS) and puts
 * them half a pixel the other side of centre. No new layer, no new art, and
 * the even widths stay on the main one where they are already right. */
/* True while the whole NEXT block is on the offset layer — see
 * draw_next_label_and_piece. The preview's own half-tile choice defers to it:
 * one layer cannot be in two places. */
static bool g_next_all_offset;

static void draw_next_piece(int tx, int ty) {
    clear_region(tx, ty, NEXT_CELL_W, 3);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < NEXT_CELL_W; x++) set_stats_tile(tx + x, ty + y, T_BLANK);

    TengenTetromino next = g_session.game.player[g_view].piece.next;
    if (next <= TT_NONE || next >= TENGEN_TETROMINO_COUNT) return;
    set_next_palette(next);

    int first = NEXT_CELL_W, last = -1;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (tengen_piece_occupies(next, 0, r, c)) {
                if (c < first) first = c;
                if (c > last) last = c;
            }
    if (last < first) return;
    int width = last - first + 1;
    int shift = (NEXT_CELL_W - width) / 2 - first;
    bool offset_layer = g_next_all_offset || (width & 1) != 0;

    /* Drawn from the same orientation bitmap and tile table the game logic
     * uses, so the preview cannot drift out of sync with what spawns. */
    int occupied = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!tengen_piece_occupies(next, 0, r, c)) continue;
            uint8_t tile = tengen_tile_id_for_cell(next, 0, occupied);
            occupied++;
            if (r >= 3) continue;
            uint16_t entry = WITH_BANK(tile, PAL_NEXT_BANK);
            if (offset_layer) set_stats_tile(tx + c + shift, ty + r, entry);
            else              set_map_tile(tx + c + shift, ty + r, entry);
        }
    }
}

/* WHAT THE RIGHT-HAND BOX SHOWS.
 *
 * The cartridge's play screen has a vertical TETRIS banner between its two
 * halves, and thirty columns cannot hold that AND two boxes wide enough to be
 * useful. So it is a choice the player makes: L+R together — the two buttons
 * a NES pad never had and this game therefore never uses — swaps the right
 * box between the piece histogram and the banner.
 *
 * IT STARTS ON THE BANNER, because that is the cartridge's own screen: the
 * vertical TETRIS is what a player who has seen this game remembers of it,
 * and the histogram is the thing you go and ask for. It is also the harder
 * way to play — no piece counts — which is why the level-up troupe is
 * reserved for it (see g_idle_show). */
static bool g_show_banner = true;
/* Frames the play screen has been up, for the idle cossack's slow sway. */
static int g_idle_frame;
/* True while the level-up show owns the right column; declared here because
 * the panel has to know not to put its own cossack up against the six. */
static bool g_dancer_active;
/* ...and its clock, for the same reason: in HUD STATS the panel's own cossack
 * dances the show, and he has to keep the show's time. */
static int g_dancer_elapsed;     /* frames since the show started, for the poses */

/* The banner's own tiles and palettes, extracted on their own because the
 * reflow no longer carries NES columns 14-17. Eighteen rows, which is what
 * the right-hand box has under NEXT — the one place the arithmetic comes out
 * even. */
#define BANNER_TX (BOX_R_TX + 2 + (BOX_W - 2 - SCREEN_1P_BANNER_W) / 2)
#define BANNER_TY 1

static void draw_banner(void) {
    for (int y = 0; y < SCREEN_1P_BANNER_H; y++)
        for (int x = 0; x < SCREEN_1P_BANNER_W; x++)
            set_map_tile(BANNER_TX + x, BANNER_TY + y,
                          WITH_BANK(kBannerTiles[y][x], kBannerBanks[y][x]));
}

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
#define STATS_ICON_TY (BOX_BOT_IN - 1)                  /* rows 16-17 */
/* One row later than the bars would otherwise start: row 6 is NEXT's rule,
 * which is what closes the top of the histogram's half of the box. */
#define STATS_TOP_TY (BOX_TOP_IN + 5)                   /* bars from row 7 */
#define STATS_BAR_ROWS (STATS_ICON_TY - STATS_TOP_TY)   /* ten of them */
#define STATS_BAR_FULL (SCREEN_1P_STATS_BAR_TILE + 7)
#define BANK_STATS SCREEN_1P_STATS_BAR_BANK
/* Seven tiles in eight columns, so one spare. It is not left at either end:
 * the block rides the second background, which is scrolled STATS_SHIFT_PX so
 * that the strip's own ink — inset two pixels on its left and flush on its
 * right — comes out five pixels from the rope and five from the screen edge.
 * See SCREENBLOCK_STATS. */
#define STATS_TX BOX_R_IN

/* The WHOLE right interior, not just the bars: NEXT's preview borrows this
 * layer too when its piece is an odd number of tiles wide, and it sits above
 * the statistics. Anything that takes the panel over has to take both. */
/* Both of the right box's borrowed layers: the histogram's, and the offset
 * one, which may still be holding an odd-width preview from a moment ago. */
static void clear_stats_layer(void) {
    for (int y = BOX_TOP_IN; y <= BOX_BOT_IN; y++)
        for (int x = 0; x < BOX_IN; x++) {
            set_stats_tile(STATS_TX + x, y, T_BLANK);
            set_histogram_tile(STATS_TX + x, y, T_BLANK);
        }
}

static void draw_stats(const TengenPlayerState *p) {
    for (int i = 0; i < SCREEN_1P_STATS_PIECES; i++) {
        int tx = STATS_TX + i;
        /* Each icon in the palette the ROM's attribute table gives it: the
         * I has its own, T/O/J/L share one, S and Z share another. */
        int icon_bank = kStatsIconBanks[i];
        set_histogram_tile(tx, STATS_ICON_TY, WITH_BANK(kStatsIcons[0][i], icon_bank));
        set_histogram_tile(tx, STATS_ICON_TY + 1, WITH_BANK(kStatsIcons[1][i], icon_bank));

        uint16_t n = p->piece_stats[TT_I + i];
        int full = n / 8;
        int part = n % 8;
        if (full > STATS_BAR_ROWS) { full = STATS_BAR_ROWS; part = 0; }

        for (int r = 0; r < STATS_BAR_ROWS; r++) {
            int ty = STATS_ICON_TY - 1 - r;
            uint16_t tile = T_BLANK;
            if (r < full) tile = STATS_BAR_FULL;
            else if (r == full && part) tile = SCREEN_1P_STATS_BAR_TILE + part - 1;
            set_histogram_tile(tx, ty, WITH_BANK(tile, BANK_STATS));
        }
    }
}

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
#define ROW_SCORE  (BOX_TOP_IN)          /* 2: label, 3: value */
#define ROW_LINES  (BOX_TOP_IN + 3)      /* 5, 6 */
#define ROW_LEVEL  (BOX_TOP_IN + 6)      /* 8, 9 */
#define ROW_HIGH   (BOX_TOP_IN + 9)      /* 11, 12 */
#define ROW_NEXT   (BOX_TOP_IN)          /* 2 on the right, piece on 3-5 */

/* A label on one row and its value on the next, both inside the left box. */
/* THE LABELS COME FROM A CLEANED COPY, not from the cartridge's tiles direct.
 * SCORE's first and last tiles carry a piece of the header grid's VERTICAL
 * line, and this layout has no vertical grid for it to belong to, so it read
 * as a grey stub at the start and end of every word. read_hud_labels strips
 * it — exactly, because the grid is colour 3 and the lettering colour 1 — and
 * the rule it was a fragment of goes back where the cartridge puts it, between
 * the rows. */
static void draw_label(int tx, int ty, int first, int count) {
    for (int i = 0; i < count; i++)
        set_map_tile(tx + i, ty,
                      WITH_BANK(HUD_LABEL_TILE_BASE + first + i, BANK_LABEL));
}

/* LABEL, VALUE, RULE — which is the cartridge's own shape. Its 1P panel reads
 * label, rule, value, rule down nametable rows 2-7, and the rule is tile $76.
 * The grey stubs that used to sit at the ends of each word were fragments of
 * the same grid; they are gone from the lettering and the rule they belonged
 * to is here instead. */
/* THE RULE RUNS WALL TO WALL, and that is the point of it. The cartridge's
 * grid ruled the full width of its panel; a rule stopping one column short of
 * the screen's edge left the counters floating instead of sitting in
 * something. So its span is the panel's INTERIOR — eight columns from the
 * screen edge to the rope — which is one wider than the CONTENT's, because
 * the content is indented off the edge and the rule is not. */
static void draw_rule(int tx, int ty) {
    for (int x = 0; x < BOX_IN; x++)
        set_map_tile(tx + x, ty, WITH_BANK(T_GRID_RULE, BANK_LABEL));
}

static void draw_counter(int ty, int label_first, int label_count,
                          uint32_t value, int digits, int value_indent) {
    draw_label(BOX_L_IN, ty, label_first, label_count);
    clear_region(BOX_L_IN, ty + 1, BOX_L_W, 1);
    draw_number(BOX_L_IN + value_indent, ty + 1, value, digits, BANK_VALUE);
    draw_rule(BOX_L_TX, ty + 2);
}

/* NEXT where it belongs, at the top of the right panel over the statistics —
 * and, when something else has that panel, in the left one under the
 * counters instead. `tx` is the panel's INTERIOR left column, so both the
 * word and the piece are centred in the same eight columns the rules span
 * rather than measured off the indented content column, which is what left
 * them sitting left of centre in either box. */
/* NEXT, AND FOUR ROWS THAT WANT THREE.
 *
 * The block is a label (one row) over a piece (two), and in the left panel it
 * lives in the four rows NEXT takes over when the banner does — twenty-four
 * pixels of content in thirty-two, so on the tile grid it can only sit at the
 * top with eight pixels of nothing under it. Centring it wants HALF A ROW, the
 * same half-tile problem as everything else on this screen.
 *
 * In HUD BANNER the offset layer is free: the statistics are not drawn and
 * the right panel is gone, so the whole block goes on it and the layer's
 * vertical scroll supplies the four pixels. It keeps the layer's horizontal
 * three as well, which is what the odd-width previews were already using, so
 * the label is centred for that offset rather than the main layer's. */
#define NEXT_BANNER_VOFS_PX 4

static void draw_next_label_and_piece(int tx, int ty, bool ruled, bool offset) {
    g_next_all_offset = offset;
    int label_tx = tx + (BOX_IN - HUD_LABEL_NEXT_W) / 2;
    for (int i = 0; i < HUD_LABEL_NEXT_W; i++) {
        uint16_t entry = WITH_BANK(HUD_LABEL_TILE_BASE + 18 + i, BANK_LABEL);
        if (offset) set_stats_tile(label_tx + i, ty, entry);
        else        set_map_tile(label_tx + i, ty, entry);
    }
    draw_next_piece(tx + (BOX_IN - NEXT_CELL_W) / 2, ty + 1);
    g_next_all_offset = false;
    /* THE RULE CLOSES THE CELL WHERE THE CONTENT ENDS, three rows down and
     * not four. The block is a label over a piece: at orientation 0 every one
     * of the seven is two rows tall — kOrientationBitmap's second byte is $00
     * for all of them — so the content is 23 pixels and a four-row cell left
     * ten of empty under it, which is what "NEXT esta muy arriba en su caja"
     * is. Three rows fits it exactly, and gives the same rhythm the counters
     * in the other box have: three pixels of air above the label, two below
     * the last of the ink. Centring it inside the taller cell instead would
     * want half a tile, and the offset layer is not free here — it is
     * carrying the statistics. */
    if (ruled) draw_rule(tx, ty + 3);
}

static void draw_panel(void) {
    const TengenPlayerState *p = &g_session.game.player[g_view];
    if (p->score > g_high_score) g_high_score = p->score;

    /* Everything from here down is the panel's, so it goes on the panel's
     * layer — the two exceptions, the braid and the banner, say so where they
     * are drawn. See SCREENBLOCK_PANEL. */
    g_panel_layer = true;

    draw_counter(ROW_SCORE, HUD_LABEL_SCORE, p->score, 6, 0);
    draw_counter(ROW_LINES, HUD_LABEL_LINES, p->lines, 4, 1);
    draw_counter(ROW_LEVEL, HUD_LABEL_LEVEL, p->level, 2, 2);

    if (g_session.game.two_player) {
        /* A race wants the other board's numbers where the high score would
         * be. The ROM keeps no piece histogram in 2P either, so nothing of
         * the cartridge's is being displaced. */
        const TengenPlayerState *o = &g_session.game.player[g_view ^ 1];
        draw_text(BOX_L_IN, ROW_HIGH, "RIVAL", BANK_LABEL);
        draw_rule(BOX_L_TX, ROW_HIGH + 2);
        clear_region(BOX_L_IN, ROW_HIGH + 1, BOX_L_W, 1);
        draw_number(BOX_L_IN, ROW_HIGH + 1, o->score, 6, BANK_VALUE);
    } else {
        /* The cartridge's own 1P panel carries a HIGH SCORE beside the score
         * — "HIGH" and "SCORE" in plain ASCII at nametable row 2, and
         * highScoreHundredThousands is the seventh entry of
         * statsDataAddresses (main.asm.txt:4100-4107). Kept for the session
         * rather than saved: this cartridge has no battery either. */
        draw_text(BOX_L_IN + 1, ROW_HIGH, "HIGH", BANK_LABEL);
        draw_rule(BOX_L_TX, ROW_HIGH + 2);
        clear_region(BOX_L_IN, ROW_HIGH + 1, BOX_L_W, 1);
        draw_number(BOX_L_IN, ROW_HIGH + 1, g_high_score, 6, BANK_VALUE);
    }

    /* Rows 14-17 of the left panel: NEXT lodges here only when the right one
     * is taken, and is blank otherwise.
     *
     * BOTH MAPS. The preview borrows the offset layer whenever its piece is an
     * odd number of tiles wide, so clearing only the main one left the piece
     * behind while its label went — which is a NEXT in both boxes at once, and
     * only for five pieces of seven, which is why it looked like it depended
     * on when you pressed L+R. */
    clear_both(BOX_L_TX, BOX_TOP_IN + 12, BOX_IN, 4);
    /* The offset layer carries the statistics and the odd-width previews, so
     * it rides down with the counters; the banner's NEXT block wants four
     * pixels more on top of that, and only while it is the one thing on the
     * layer. */
    REG_BG1VOFS = (uint16_t)(512 - PANEL_SHIFT_PX -
                              (g_show_banner ? NEXT_BANNER_VOFS_PX : 0));
    if (g_show_banner) {
        draw_next_label_and_piece(BOX_L_TX, BOX_TOP_IN + 12, false, true);
        hide_idle_cossack();
    } else if (g_dancer_active && g_show_banner) {
        /* The show has the real six of them out on the ledges. */
        hide_idle_cossack();
    } else {
        /* HUD STATS: one cossack where NEXT would be. See IDLE_OAM_BASE.
         *
         * He stops for two things and they are the same thing — there is
         * nothing to keep time to. A dead board freezes him where he stands,
         * and so does PAUSE: a cossack swaying behind the plaque while the
         * music is suspended is the one part of the screen that did not
         * notice the game had stopped. */
        bool alive = g_session.game.player[g_view].game_active &&
                     !g_session.game.paused;
        /* The interlude, danced solo: see g_idle_show. It runs off the show's
         * own clock so it lasts exactly as long as the show does. */
        g_idle_show = g_dancer_active;
        draw_idle_cossack(g_idle_show ? (int)g_dancer_elapsed : g_idle_frame,
                           alive || g_idle_show,
                           BOX_L_TX, BOX_TOP_IN + 12, BOX_IN, 4);
        g_idle_show = false;
        if (alive) g_idle_frame++;
    }

    /* The right box: the banner, the statistics, or — in a race, where the
     * cartridge keeps no statistics either — nothing.
     *
     * THE BANNER TAKES THE WHOLE COLUMN, BRAID AND ALL. It is six letters of
     * three rows each, eighteen rows with no padding anywhere in it, and a
     * braid box leaves sixteen — so there is no honest crop. Handing it the
     * column instead is also what the cartridge's own screen looks like:
     * a bare vertical TETRIS with nothing framing it. What it does NOT get is
     * the two columns nearest the board: those are the rope that frames the
     * playfield itself, and the playfield keeps its frame whatever the HUD is
     * doing. The box comes back when the banner goes away, through g_repaint. */
    if (g_show_banner) {
        /* THE BANNER IS ART, NOT A COUNTER: it goes on the main map, aligned
         * to the screen, with the braid that frames the board. Both maps get
         * wiped first — the panel's because the statistics box was there a
         * frame ago, the main one because the box's own tiles were. */
        g_panel_layer = false;
        clear_region(BOX_R_TX + 2, 0, BOX_W - 2, SCREEN_TH);
        clear_panel_region(BOX_R_TX + 2, 0, BOX_W - 2, SCREEN_TH);
        clear_stats_layer();
        draw_field_braid(BOX_R_TX, kBraidLeft);
        draw_banner();
    } else if (!g_session.game.two_player) {
        draw_next_label_and_piece(BOX_R_IN, ROW_NEXT, true, false);
        draw_stats(p);
    } else {
        /* A race keeps no piece histogram — the cartridge keeps none in 2P
         * either — but the PREVIEW is not statistics, it is how you plan the
         * next piece, and a player racing without one is playing a different
         * game from the one at the other end of the cable. It stays. */
        clear_region(BOX_R_IN, BOX_TOP_IN, BOX_IN, BOX_BOT_IN - BOX_TOP_IN + 1);
        clear_stats_layer();
        /* No rule under it: that line is what separates NEXT from the
         * statistics, and there are none here for it to separate. */
        draw_next_label_and_piece(BOX_R_IN, ROW_NEXT, false, false);
        /* The rival topping out is the only news this box has left to carry,
         * and it goes at the bottom of it, clear of the preview. */
        if (!g_session.game.player[g_view ^ 1].game_active)
            draw_text(BOX_R_IN + 2, BOX_BOT_IN - 1, "OUT", BANK_LABEL);
    }

    g_panel_layer = false;
}

static void draw_field(void) {
    const TengenPlayfield *field = &g_session.game.field[g_view];
    const TengenPlayerState *p = &g_session.game.player[g_view];

    /* How far the line-clear sweep has crossed the completed rows, and what
     * it is writing into them as it goes. `written` is the last column the
     * trailing sprite has passed over; everything to its right still shows
     * the blocks that are about to come down. */
    uint8_t step = tengen_line_clear_step(&g_session.game, (TengenPlayerSlot)g_view);
    int written = (int)step - TENGEN_CLEAR_TRAIL - 1;
    int rows_going = 0;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        if (p->clearing_rows & (1u << row)) rows_going++;
    const char *word = kClearWord[rows_going > 4 ? 4 : rows_going];

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        bool clearing = (p->clearing_rows & (1u << row)) != 0;
        for (int col = 0; col < FIELD_PLAYABLE; col++) {
            /* A cell's value IS its tile index — that is the whole point of
             * the ROM's nibble encoding (notes.txt.txt:35). Empty is 0, which
             * is the blank tile. All of it draws in the level's palette. */
            uint16_t entry = WITH_BANK(field->cell[row][FIELD_COL0 + col], 0);

            /* Behind the sweep the row is gone and the word is in its place,
             * one character per playable column (L89E9,
             * main.asm.txt:1508-1530). */
            if (clearing && col <= written)
                entry = WITH_BANK(ascii_tile(word[col]), 0);

            set_map_tile(FIELD_TX + col, FIELD_TY + row, entry);
        }
    }

    /* No falling piece exists while the completed rows animate. */
    if (p->line_clear_timer > 0) return;

    /* The falling piece is drawn over the settled field rather than into it,
     * and in its own palette — the ROM draws it as sprites for exactly that
     * reason. Cells above the field are skipped, which is what makes a piece
     * visibly slide in from off-screen the way the original does. */
    TengenCell cells[4];
    int count = tengen_active_piece_cells(&g_session.game, (TengenPlayerSlot)g_view, cells);
    TengenTetromino current = g_session.game.player[g_view].piece.current;
    for (int i = 0; i < count; i++) {
        if (cells[i].row < 0) continue;
        int col = cells[i].col - FIELD_COL0;
        if (col < 0 || col >= FIELD_PLAYABLE) continue;
        uint8_t tile = tengen_tile_id_for_cell(current, g_session.game.player[g_view].piece.orientation, i);
        set_map_tile(FIELD_TX + col, FIELD_TY + cells[i].row,
                      WITH_BANK(tile, PAL_PIECE_BANK));
    }
}

static void draw_pause_box(void) {
    for (int y = 0; y < PAUSE_H; y++)
        for (int x = 0; x < PAUSE_W; x++)
            set_map_tile(PAUSE_TX + x, PAUSE_TY + y,
                          WITH_BANK(kPauseTiles[y][x], BANK_PAUSE));
}

/* ----------------------------------------------------------------------- *
 * Title screen
 * ----------------------------------------------------------------------- */
/* Start levels run 0..9 and wrap at both ends, which is the range the ROM's
 * own menu allows: computerMoveSelectTable (main.asm.txt:4819) holds the wrap
 * limit per menu row, and menuPlayer1StartLevel's is 10. */
#define START_LEVEL_COUNT 10

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
static const uint8_t kMusicTracks[MUSIC_COUNT] = {
    NES_MUSIC_SILENCE, NES_MUSIC_LOGINSKA, NES_MUSIC_BRADINSKY,
    NES_MUSIC_KARINKA, NES_MUSIC_TROIKA
};

/* THE FIFTH TUNE, WHICH IS NOT ON THE CARTRIDGE.
 *
 * Korobeiniki is not one of Tengen's four — the game everybody hums it at is
 * Nintendo's Game Boy version — so there is nothing to extract and it is
 * entered by hand instead, in gba/korobeiniki.c, which is the one file here
 * that is not the ROM's. It stays hidden until L+R together on the selection
 * screen, and announces itself with SOUND_CHIRP the way the title's skin
 * does; until then the menu offers the cartridge's four and nothing hints
 * that there is a fifth.
 *
 * It is a fifth ENTRY, never a fifth ROM track: kMusicTracks has four, and
 * every place that starts music goes through start_music() below. */
#define MUSIC_KOROBEINIKI MUSIC_COUNT

/* AND A SIXTH ENTRY THAT IS NOT A TUNE. The same L+R that uncovers Korobeiniki
 * uncovers MUSIC MIX, which plays the five in turn instead of one of them over
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
#define MUSIC_MIX (MUSIC_COUNT + 1)
#define MUSIC_UNLOCKED_COUNT (MUSIC_COUNT + 2)
static bool g_music_unlocked;

static const char *const kMusicNames[MUSIC_UNLOCKED_COUNT] = {
    "NO MUSIC", "LOGINSKA", "BRADINSKY", "KARINKA", "TROIKA", "KOROBEINIKI",
    "MUSIC MIX"
};

/* The rotation: the cartridge's four and the hand-entered one, which has
 * earned its place in it by the time anyone has found this. */
/* KOROBEINIKI FIRST. The mix is only on offer to somebody who found the code,
 * so the tune the code is really about opens the first level, and the
 * cartridge's four follow it. */
static const uint8_t kMixOrder[] = { MUSIC_KOROBEINIKI, 1, 2, 3, 4 };
#define MIX_COUNT (sizeof kMixOrder / sizeof kMixOrder[0])
static uint8_t g_mix_step;

static uint8_t mix_tune(void) { return kMixOrder[g_mix_step % MIX_COUNT]; }


static uint8_t music_choices(void) {
    return (uint8_t)(g_music_unlocked ? MUSIC_UNLOCKED_COUNT : MUSIC_COUNT);
}

/* One frame of sound, both engines. The cartridge's runs on every frame
 * whatever is playing, because the EFFECTS are always its; the hand-entered
 * tune does nothing unless it is the one chosen. */
static void audio_frame(void) {
    nes_audio_frame();
    korobeiniki_frame();
}

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

static void stop_music_class(uint8_t klass) {
    nes_rom_call(NES_AUDIO_STOP_ADDR, klass, NES_AUDIO_STOP_STEPS);
}

static void stop_music(void) {
    korobeiniki_stop();
    stop_music_class(NES_MUSIC_CLASS_TITLE);
    stop_music_class(NES_MUSIC_CLASS_GAME);
}

/* Starts whichever tune is chosen, on whichever engine owns it. The two never
 * play at once: the cartridge's is suspended for the hand-entered one and
 * keeps running underneath, so the sound EFFECTS are the ROM's either way.
 *
 * SILENCE FIRST, ALWAYS. This is `LA035` (main.asm.txt:4730-4735), which is
 * the cartridge's own way of starting a tune:
 *
 *     lda #MUSIC_SILENCE / jsr setMusicOrSoundEffect
 *     ldy menuMusic / lda musicSelectTable,y / jmp setMusicOrSoundEffect
 *
 * and it is not decoration. setMusicOrSoundEffect only QUEUES a request
 * ($0200-$0207, a ring with its indices at $0208/$0209); handing the engine a
 * new track without silencing the old one leaves the old one's channels
 * running underneath.
 *
 * AND RESUME COMES LAST. updateAudio takes exactly ONE request off that ring
 * per frame ($CFCC-$CFDB), so the order requests are queued in is the order
 * they are heard in, a frame apart. Resuming first — which is what this did —
 * hands the suspended track a frame or two of the speaker before the silence
 * that was meant to replace it arrives: the title theme turning up under the
 * tune you are choosing, and worse if the ring is busy enough to DROP the
 * silence ($CFC3 drops on full). Loading the new track while the engine is
 * still frozen and only then letting it go has no such window, and a tune
 * that is no tune (NO MUSIC) simply never lets it go at all. */
static void start_music(uint8_t music) {
    if (music == MUSIC_MIX) music = mix_tune();
    if (music == MUSIC_KOROBEINIKI) {
        stop_music();            /* the cartridge's engine steps aside */
        korobeiniki_start();
        return;
    }
    korobeiniki_stop();
    uint8_t track = kMusicTracks[music < MUSIC_COUNT ? music : 0];
    if (track == NES_MUSIC_SILENCE) {
        /* musicSelectTable's first entry is no tune at all. */
        stop_music();
        return;
    }
    /* The title theme's class first, or the tune below cannot take the slots
     * off it — see stop_music_class. Free even when nothing is playing: it is
     * a walk of eleven bytes. */
    stop_music_class(NES_MUSIC_CLASS_TITLE);
    nes_audio_play(NES_MUSIC_SILENCE);
    nes_audio_play(track);
}

/* The ROM has a title screen and then separate selection screens, drawn in
 * its own menu frame; this follows the same shape.
 *
 * GAME SELECT is the cartridge's own first menu, and its own wording: the
 * nametable at rows 14-20 spells 1 PLAYER / 2 PLAYER / COOPERATIVE / VERSUS
 * COMPUTER / WITH COMPUTER. Two of those five are implemented, so two are
 * listed — an entry that does nothing would be worse than an entry that is
 * not there. The three that are missing all want the ROM's `computerMove`
 * AI or the coop front end; see CLAUDE.md's roadmap. */
typedef enum {
    SCREEN_TITLE,
    SCREEN_GAME_SELECT,
    SCREEN_LEVEL_SELECT,
    SCREEN_LINK_WAIT,
    SCREEN_PLAYING
} Screen;

#define GAME_1P   0
#define GAME_2P   1
#define GAME_COUNT 2
static const char *const kGameNames[GAME_COUNT] = { "1 PLAYER", "2 PLAYER" };

/* Set whenever the maps are wiped, so the title knows its tiles are gone. */
static bool g_title_dirty = true;

static void clear_screen(void) {
    bool was = g_panel_layer;
    g_panel_layer = false;
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < MAP_W; tx++) {
            set_map_tile(tx, ty, T_BLANK);
            set_stats_tile(tx, ty, T_BLANK);
        }
    /* ...and the counters' layer with them, or a menu reached from a game
     * would have its panel still hanging over it. The histogram's goes the
     * same way. */
    clear_panel_region(0, 0, MAP_W, 32);
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < MAP_W; tx++) set_histogram_tile(tx, ty, T_BLANK);
    g_panel_layer = was;
    g_title_dirty = true;
}

/* ----------------------------------------------------------------------- *
 * THE TITLE, AND THE SKIN L/R SWITCHES TO
 *
 * Tengen shipped this game twice. The prototype cartridges — made while the
 * licence was still Nintendo's — carry a different title screen: another
 * cathedral, another logo, and a green fret where the release has its blue
 * braid. Both are the cartridges' own art, so L or R on the title swaps
 * between them, with SOUND_CHIRP for a doorbell — one of the four effects
 * constants.asm.txt marks "maybe unused", so the egg is announced in the
 * game's own voice by a sound the game itself never plays.
 *
 * The skin is only ever the PICTURE. The cathedral overlay and the fireworks
 * stay on the release screen and are hidden on the prototype's, because they
 * are the release's: their sprites are placed in NES pixels over the release
 * cathedral, and the prototype's composition has neither the same rows nor
 * an empty sky to burst in. Putting them there would be inventing something
 * neither cartridge does.
 *
 * If the port was built without a prototype dump, SCREEN_PROTO_AVAILABLE is 0
 * and L/R have nothing to switch to. See tools/extract_assets.py.
 * ----------------------------------------------------------------------- */
#define TITLE_SKIN_COUNT (SCREEN_PROTO_AVAILABLE ? 2 : 1)

static uint8_t g_title_skin;

/* The two screens are never up at once, so the prototype's palettes go in the
 * title's own four banks rather than asking for four more. */
static void install_title_palette(void) {
#if SCREEN_PROTO_AVAILABLE
    upload_palette_set(PAL_TITLE_BASE,
                        g_title_skin ? kRomPalette_bg_proto : kRomPalette_bg_title,
                        MEM_PALETTE);
#else
    upload_palette_set(PAL_TITLE_BASE, kRomPalette_bg_title, MEM_PALETTE);
#endif
}

/* ONCE PER VISIT, not once per frame. The title's tiles never change while it
 * is up — the cathedral and the fireworks on top of them are sprites — and
 * since the two words moved to the offset layer this writes 1200 map entries,
 * which on top of the cartridge's own code running under it was enough to
 * miss a vblank every sixty frames. clear_screen arms it again, and every
 * path that reaches the title goes through one. */
static void draw_title(void) {
    if (!g_title_dirty) return;
    g_title_dirty = false;
#if SCREEN_PROTO_AVAILABLE
    if (g_title_skin) {
        /* Thirty columns, no padding: the prototype's frame is two columns a
         * side — a thin outer rule and the fret inside it — and the two the
         * GBA lacks come off the rule, so the decoration survives whole. */
        for (int ty = 0; ty < SCREEN_PROTO_H_TILES; ty++) {
            for (int tx = 0; tx < SCREEN_PROTO_W; tx++) {
                int i = ty * SCREEN_PROTO_W + tx;
                set_map_tile(tx, ty,
                              WITH_BANK(PROTO_TILE_BASE + kScreenProtoTiles[i],
                                        PAL_TITLE_BASE + kScreenProtoPalettes[i]));
            }
        }
        return;
    }
#endif
    /* Centred: the composition is 28 columns wide (see TITLE_KEEP_COLS —
     * the brick border's jewels are a two-column motif and half of one is
     * worse than none), so it sits one column in from each edge. */
    const int pad = (SCREEN_TW - SCREEN_TITLE_W) / 2;
    for (int ty = 0; ty < SCREEN_TITLE_H_TILES; ty++) {
        for (int tx = 0; tx < SCREEN_TITLE_W; tx++) {
            int i = ty * SCREEN_TITLE_W + tx;
            uint16_t tile = TITLE_TILE_BASE + kScreenTitleTiles[i];
            uint16_t entry =
                WITH_BANK(tile, PAL_TITLE_BASE + kScreenTitlePalettes[i]);
            /* TENGEN RIDES THE OFFSET LAYER; see TITLE_LOGO_SHIFT_PX.
             * Everything else — the frame, the TETRIS logo and the spire that
             * comes out of it — stays on the main one. */
            bool shifted = ty >= TITLE_LOGO_TY0 && ty <= TITLE_LOGO_TY1 &&
                            tx >= TITLE_LOGO_TX0 && tx <= TITLE_LOGO_TX1;
            set_map_tile(pad + tx, ty, shifted ? T_BLANK : entry);
            set_stats_tile(pad + tx, ty, shifted ? entry : T_BLANK);
        }
    }
}

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
/* Columns go through their own map for the same reason rows do: the two the
 * composition drops come out of the middle, so a sprite right of the gap is
 * two columns left of where its NES x says. */

static uint16_t g_title_frame;

/* Once, at boot: resetContinued seeds the RNG here and clears the page
 * (main.asm.txt:5703-5712). Zero is exactly the state the fireworks' first
 * frame expects — it finds a null script pointer, parks all 45 sprites
 * offscreen and schedules the first burst. The seed is deliberately NOT
 * touched again: it is the sound engine's too. */
static void init_title_sprites(void) {
    uint8_t *ram = nes_rom_ram();
    ram[NES_RAM_RNG_SEED] = NES_RAM_RNG_SEED_VALUE;
    for (int i = 0; i < 0x100; i++) ram[NES_RAM_OAM_STAGING + i] = 0;
    g_title_frame = 0;
}

static void restart_title_sprites(void);

/* Every time the title screen starts, which is what initializeTitleScreen
 * does with the frame counter (main.asm.txt:4483-4485) — and it matters:
 * the show is over once frameCounterHigh reaches 4, so without this it would
 * play only on the very first visit. The script pointer and the burst timer
 * are left alone, because the cartridge leaves them alone too.
 *
 * The cathedral is staged HERE rather than per frame. The cartridge re-runs
 * it every frame because its NMI rebuilds the whole OAM page every frame;
 * this port does not, and the routine's only inputs are a constant table and
 * ppuScrollYOffset — which only the title's hidden both-Downs scroll changes
 * (main.asm.txt:4470-4476) and this port has no scroll. Its eighteen sprites
 * are therefore the same eighteen bytes every frame, and interpreting ~500
 * 6502 instructions to arrive at them again was costing about one frame in
 * fifty-five. Nothing else writes staging entries 0-17: the fireworks' own
 * loops all start at $4C, entry 19. */
static void restart_title_sprites(void) {
    g_title_frame = 0;
    nes_rom_ram()[NES_RAM_GAMESTATE] = NES_GAMESTATE_TITLE;
    nes_rom_call(NES_CATHEDRAL_ADDR, 0, 8000);
}

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

/* The nearest row the composition kept, for a NES row it may have dropped. */
static int title_row_near(int nrow) {
    for (int d = 0; d < 32; d++) {
        if (nrow - d >= 0 && kTitleRowMap[nrow - d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleRowMap[nrow - d];
        if (nrow + d < 30 && kTitleRowMap[nrow + d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleRowMap[nrow + d];
    }
    return -1;
}

static int title_col_near(int ncol) {
    for (int d = 0; d < 34; d++) {
        if (ncol - d >= 0 && kTitleColMap[ncol - d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleColMap[ncol - d];
        if (ncol + d < 32 && kTitleColMap[ncol + d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleColMap[ncol + d];
    }
    return -1;
}

static void draw_title_sprites(void) {
    if (g_title_skin) {          /* see the note above draw_title */
        oam_hide_all();
        return;
    }
    uint8_t *ram = nes_rom_ram();
    ram[NES_RAM_GAMESTATE] = NES_GAMESTATE_TITLE;
    ram[NES_RAM_FRAME_LOW] = (uint8_t)g_title_frame;
    ram[NES_RAM_FRAME_HIGH] = (uint8_t)(g_title_frame >> 8);
    g_title_frame++;

    /* The step limit is a hang guard, not timing: the fireworks' worst frame
     * rewrites all 45 of their sprites twice over. */
    nes_rom_call(NES_FIREWORKS_ADDR, 0, 12000);

    const uint8_t *oam = ram + NES_RAM_OAM_STAGING;
    const int pad = (SCREEN_TW - SCREEN_TITLE_W) / 2;

    /* The burst's single offset, from the middle of its bounding box. */
    int fw_dx = 0, fw_dy = 0;
    bool fw_placed = false;
    {
        int x0 = 256, x1 = -1, y0 = 240, y1 = -1;
        for (int i = TITLE_FIREWORK_FIRST; i < TITLE_OAM_COUNT; i++) {
            int ny = oam[i * 4], nx = oam[i * 4 + 3];
            if (ny >= 240) continue;          /* parked, see below */
            if (nx < x0) x0 = nx;
            if (nx > x1) x1 = nx;
            if (ny < y0) y0 = ny;
            if (ny > y1) y1 = ny;
        }
        if (y1 >= 0) {
            int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            int col = title_col_near(cx / 8), row = title_row_near(cy / 8);
            if (col >= 0 && row >= 0) {
                fw_dx = (pad + col) * 8 + (cx & 7) - cx;
                fw_dy = row * 8 + (cy & 7) - cy;
                fw_placed = true;
            }
        }
    }

    for (int i = 0; i < TITLE_OAM_COUNT; i++) {
        int ny = oam[i * 4];
        uint8_t tile = oam[i * 4 + 1];
        uint8_t attr = oam[i * 4 + 2];
        int nx = oam[i * 4 + 3];
        int x, y;
        /* The NES hides a sprite by parking it below the visible 240 lines;
         * this code uses $F7 for exactly that. */
        if (ny >= 240) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        if (i >= TITLE_FIREWORK_FIRST) {
            if (!fw_placed) { MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN; continue; }
            x = nx + fw_dx;
            y = ny + fw_dy;
            if (x < 0 || x >= SCREEN_TW * 8 || y < 0 || y >= SCREEN_TH * 8) {
                MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
                continue;
            }
            oam_set(i, x, y, (uint16_t)(TITLE_OBJ_TILE_BASE + tile), false,
                     PAL_OBJ_TITLE + (attr & 3));
            continue;
        }
        int row = kTitleRowMap[ny / 8];
        if (row == SCREEN_TITLE_ROW_DROPPED) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        int col = (nx >= 0 && nx < 256) ? kTitleColMap[nx / 8]
                                        : SCREEN_TITLE_ROW_DROPPED;
        if (col == SCREEN_TITLE_ROW_DROPPED) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        /* ...and then the same one-column pad draw_title centres with. */
        x = (pad + col) * 8 + (nx & 7);
        if (x >= SCREEN_TW * 8) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        /* Low two bits of the NES attribute byte pick one of the four
         * palettes of the set the title installs, spritePalette1. */
        oam_set(i, x, row * 8 + (ny & 7),
                 (uint16_t)(TITLE_OBJ_TILE_BASE + tile), false,
                 PAL_OBJ_TITLE + (attr & 3));
    }
    for (int i = TITLE_OAM_COUNT; i < 128; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

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

static unsigned text_len(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

/* Writes `text` centred in the frame, on whichever layer makes it land on the
 * middle. Both maps are cleared first, so a name that was odd last frame and
 * is even this one leaves nothing behind. */
static void draw_text_centred(int ty, const char *text, int bank) {
    unsigned len = text_len(text);
    int tx = MENU_IN_TX + ((int)MENU_IN_W - (int)len) / 2;
    bool offset = (len & 1u) != 0;
    for (int x = 0; x < MENU_IN_W; x++) {
        set_map_tile(MENU_IN_TX + x, ty, T_BLANK);
        set_stats_tile(MENU_IN_TX + x, ty, T_BLANK);
    }
    for (unsigned i = 0; i < len; i++) {
        uint16_t entry = WITH_BANK(ascii_tile(text[i]), bank);
        if (offset) set_stats_tile(tx + (int)i, ty, entry);
        else        set_map_tile(tx + (int)i, ty, entry);
    }
}

static void draw_menu_frame(void) {
    set_offset_layer(MENU_TEXT_SHIFT_PX);
    for (int ty = 0; ty < SCREEN_MENU_H_TILES; ty++) {
        for (int tx = 0; tx < SCREEN_MENU_W; tx++) {
            int i = ty * SCREEN_MENU_W + tx;
            set_map_tile(tx, ty,
                          WITH_BANK(kScreenMenuTiles[i], PAL_MENU_BASE + kScreenMenuPalettes[i]));
        }
    }
}

static void draw_game_select(uint8_t choice) {
    draw_menu_frame();
    draw_text_centred(8, "GAME SELECT", PAL_MENU_BASE + 3);
    for (int i = 0; i < GAME_COUNT; i++)
        draw_text_centred(10 + i * 2, kGameNames[i],
                           i == choice ? BANK_HILITE : PAL_MENU_BASE + 3);
    /* The credit the cartridge never printed. Tengen's title screen carries
     * "(C)1987 ACADEMYSOFT-ELORG" — the Soviet institute, not the man — and
     * the licensing fight that followed is the reason this cartridge was
     * pulled from shelves. The port's title has no room for either line any
     * more (the cathedral took it), so the credit lands here instead, and
     * says who actually wrote the game.
     *
     * It sits closer to the two entries now, and alone: the line about the
     * cable that used to be between them said nothing the player could act
     * on — choosing 2 PLAYER leads to a screen that says so itself, and says
     * it when it matters.
     *
     * The menu frame's black interior is columns 3-26 — twenty-four of them —
     * so the full "TETRIS BY ALEXEY PAJITNOV" (twenty-five) ran over the braid
     * at both ends. The word TETRIS is already six tiles tall above this. */
    draw_text_centred(15, "BY ALEXEY PAJITNOV", PAL_MENU_BASE + 3);
}

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

static void draw_guest_dancer(int elapsed) {
    int pose = (elapsed / DANCER_POSE_FRAMES) % DANCER_POSE_COUNT;
    const uint8_t *tiles = kDancerPoses[pose];
    for (int s = 0; s < DANCER_SPRITES; s++) {
        oam_set(s, GUEST_DANCER_X + ((s & 1) ? 8 : 0),
                 GUEST_DANCER_Y + ((s & 2) ? 8 : 0),
                 tiles[s], false, PAL_OBJ_DANCER + (kDancerAttr[0] & 3));
    }
    for (int i = DANCER_SPRITES; i < 128; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

static void draw_link_wait(const TengenLobby *lobby, int elapsed) {
    draw_menu_frame();
    draw_text_centred(8, "LINK CABLE", PAL_MENU_BASE + 3);

    clear_both(MENU_IN_TX, 11, MENU_IN_W, 5);
    if (lobby->failed) {
        oam_hide_all();
        draw_text_centred(11, "NO CABLE FOUND", BANK_HILITE);
        draw_text_centred(14, "B TO GO BACK", PAL_MENU_BASE + 3);
        return;
    }
    if (!link_connected()) {
        oam_hide_all();
        draw_text_centred(11, "WAITING FOR PLAYER 2", PAL_MENU_BASE + 3);
        draw_text_centred(14, "B TO GO BACK", PAL_MENU_BASE + 3);
        return;
    }
    /* Connected. The master has gone off to choose; this console is the guest,
     * so it says who it is and lets the cossack do the waiting. */
    draw_text_centred(11, "YOU ARE PLAYER 2", BANK_HILITE);
    draw_guest_dancer(elapsed);
}

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
#define MENU_ARROW_L '?'   /* tile $3F — main.asm.txt:4797 */
#define MENU_ARROW_R '>'   /* tile $3E */

#define MENU_FIELD_LEVEL    0
#define MENU_FIELD_HANDICAP 1
#define MENU_FIELD_MUSIC    2
#define MENU_FIELD_COUNT    3

/* The rows the page may write to: everything under the TETRIS logo and above
 * the frame's bottom run. */
#define MENU_BODY_TY 7
#define MENU_BODY_H  11

/* Three fields, three rows apart, so each has two blank rows to itself — the
 * whole point of the exercise. Row 16 is the last one with air under it: the
 * frame's bottom braid starts at y=145, so a line on row 17 ends one pixel
 * short of it. */
#define MENU_FIELD_TY(f) (8 + (f) * 3)
#define MENU_FOOT_TY 16

/* TWO COLUMNS, centred as a block. Labels start at one column and values at
 * another, both fixed for all three rows, so the page reads as a table rather
 * than as three sentences: eight columns for the longest label (HANDICAP),
 * three of gap, eleven for the longest value (KOROBEINIKI) is twenty-two of
 * the interior's twenty-six, which leaves two either side. The cursor lives
 * in the left margin, the way a menu arrow does. */
#define MENU_LABEL_TX  (MENU_IN_TX + 2)
#define MENU_VALUE_TX  (MENU_LABEL_TX + 11)
#define MENU_CURSOR_TX MENU_IN_TX
/* ...and anything the value trails, two columns further on. */
#define MENU_TAIL_GAP 2

/* Appends a number with no leading zeroes. Returns the new length. */
static unsigned append_number(char *row, unsigned n, unsigned value) {
    if (value >= 10) row[n++] = (char)('0' + value / 10);
    row[n++] = (char)('0' + value % 10);
    return n;
}

/* Anything the screen says rather than offers: the handicap's depth, and the
 * line at the foot. Bank 2's first colour is the menu's pale cyan against
 * bank 3's white, so it reads as a note and not as another choice. */
#define BANK_NOTE (PAL_MENU_BASE + 2)

/* One field: the cursor if it is the chosen one, then the label and the value
 * in their columns, and whatever the value trails after it — which is only
 * ever what the handicap costs, and is a note about the value rather than
 * part of it, so it is drawn in the note's colour. */
static void draw_field_row(int field, int chosen, const char *label,
                            const char *value, const char *tail) {
    int ty = MENU_FIELD_TY(field);
    clear_both(MENU_IN_TX, ty, MENU_IN_W, 1);
    if (field == chosen)
        set_map_tile(MENU_CURSOR_TX, ty,
                      WITH_BANK(ascii_tile(MENU_ARROW_R), BANK_HILITE));
    for (int i = 0; label[i]; i++)
        set_map_tile(MENU_LABEL_TX + i, ty,
                      WITH_BANK(ascii_tile(label[i]), PAL_MENU_BASE + 3));
    int tx = MENU_VALUE_TX;
    for (int i = 0; value[i]; i++, tx++)
        set_map_tile(tx, ty, WITH_BANK(ascii_tile(value[i]), BANK_HILITE));
    if (tail) {
        tx += MENU_TAIL_GAP;
        for (int i = 0; tail[i]; i++, tx++)
            set_map_tile(tx, ty, WITH_BANK(ascii_tile(tail[i]), BANK_NOTE));
    }
}

static void draw_level_settings(int chosen, uint8_t start_level, uint8_t music,
                                 const uint8_t handicap[2], bool two_player) {
    draw_menu_frame();
    /* draw_menu_frame only repaints BG0; the offset layer keeps whatever the
     * screen before this one left on it. */
    clear_both(MENU_IN_TX, MENU_BODY_TY, MENU_IN_W, MENU_BODY_H);

    char value[16];
    char depth[16];
    unsigned n;

    n = append_number(value, 0, start_level);
    value[n] = '\0';
    draw_field_row(MENU_FIELD_LEVEL, chosen, "LEVEL", value, NULL);

    /* THE STARTING HANDICAP, the cartridge's own menuPlayer1Handicap /
     * menuPlayer2Handicap (main.asm.txt:3536-3546): how many three-row bands
     * of garbage a player starts buried under, nought to four. In two players
     * there are two of them, and the shoulder on a player's side of the pad
     * is what sets that player's.
     *
     * WHAT IT COSTS RIDES THE SAME LINE, because "2" says nothing until you
     * know it is two of the bands garbageHeightData lays down. The word
     * BURIES is what gets dropped to make it fit: "12 ROWS" beside the value
     * says the same thing in seven columns. In two players there are two
     * counts and no room for them, so those keep a line of their own — and
     * only while the cursor is on the field, since the rest of the time the
     * row is better empty. */
    n = append_number(value, 0, handicap[0]);
    if (two_player) {
        value[n++] = ' ';
        n = append_number(value, n, handicap[1]);
    }
    value[n] = '\0';
    if (!two_player) {
        unsigned d = append_number(depth, 0,
                                    (unsigned)handicap[0] * TENGEN_HANDICAP_ROWS_PER_STEP);
        const char *unit = " ROWS";
        while (*unit) depth[d++] = *unit++;
        depth[d] = '\0';
    }
    draw_field_row(MENU_FIELD_HANDICAP, chosen, "HANDICAP", value,
                    two_player ? NULL : depth);

    draw_field_row(MENU_FIELD_MUSIC, chosen, "MUSIC", kMusicNames[music], NULL);

    if (two_player && chosen == MENU_FIELD_HANDICAP) {
        char row[32];
        unsigned m = 0;
        const char *lead = "BURIES ";
        while (*lead) row[m++] = *lead++;
        m = append_number(row, m, (unsigned)handicap[0] * TENGEN_HANDICAP_ROWS_PER_STEP);
        const char *mid = " AND ";
        while (*mid) row[m++] = *mid++;
        m = append_number(row, m, (unsigned)handicap[1] * TENGEN_HANDICAP_ROWS_PER_STEP);
        const char *tail = " ROWS";
        while (*tail) row[m++] = *tail++;
        row[m] = '\0';
        draw_text_centred(MENU_FIELD_TY(MENU_FIELD_HANDICAP) + 1, row, BANK_NOTE);
    }

    draw_text_centred(MENU_FOOT_TY, "START TO PLAY", BANK_NOTE);
}


/* ----------------------------------------------------------------------- *
 * The match
 *
 * A solo game and a linked one share every line of the drawing and almost
 * every line of the game logic. They differ in exactly three places, which is
 * the point of keeping the lockstep in ../src/tengen_link.c:
 *
 *   - where the buttons come from: the keypad, or a completed transfer that
 *     carries BOTH consoles' buttons for one frame;
 *   - what advances the simulation: one call per frame, or one call per
 *     transfer, which is usually the same thing and occasionally is not;
 *   - whether the level-up interlude may stop the world. It may not over a
 *     cable: the two consoles would have to stop and resume on the same
 *     frame or the lockstep is over, and the cartridge does not send its
 *     dancers out during a two-player race in any case.
 * ----------------------------------------------------------------------- */

static bool g_linked;            /* this match is running over the cable */
static bool g_link_lost;         /* ...and the cable stopped answering */
static bool g_repaint;           /* the static screen needs putting back */
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
static uint8_t g_front_tune = FRONT_NOTHING;

/* NO MUSIC to start with — musicSelectTable's own first entry
 * (main.asm.txt:4741, "silence, loginska, bradinsky, karinka, troika") and
 * the quieter thing to hand somebody a handheld with. Anything else means a
 * tune starts playing the moment the settings screen comes up. */
static uint8_t g_music = 0;

/* WHAT IS ACTUALLY PLAYING, which is not always what is selected: MUSIC MIX
 * is a rotation, not a tune, so anything that has to act on the tune itself —
 * pausing the fifth one, say — has to resolve it first. */
static uint8_t current_tune(void) {
    return g_music == MUSIC_MIX ? mix_tune() : g_music;
}

/* The interlude's clock, which is the ROM's player1FallTimer: `active` while
 * the show is on, `timer` counting $7C..$FF at one step every sixteen frames.
 * See the note beside DANCER_TIMER_START for why it is shaped like this. */
static uint8_t g_dancer_timer;
static uint16_t g_dancer_tick;   /* stands in for frameCounterLow & $0F */
static int g_dancer_cast = 1;    /* how many walk on; see tengen_dancer_count */
static uint8_t g_shown_level = 0xFF;
static TengenTetromino g_shown_piece = TT_NONE;

/* Two seconds without a transfer. Long enough that nothing short of the
 * cable actually coming out reaches it, short enough that the player is not
 * left staring at a frozen board wondering. */
#define LINK_LOST_FRAMES 120

/* Never spend a frame doing nothing but catching up. Two is all the drift
 * between two crystals can ever put in the queue at once. */
#define LINK_MAX_CATCHUP 2

/* The ROM's own cues, each at the moment it plays them:
 *  - a piece coming to rest, L8417 (main.asm.txt:637)
 *  - rows coming down, L95C1 (:3212) — unless that clear also raised the
 *    level, in which case the intro takes its place (:3207)
 *  - the level-up interlude itself, L8D6B (:2038)
 *  - topping out, silence and then the game-over tune (:608, :620) */
static void announce_step(TengenStepResult step) {
    if (step.piece_locked) nes_audio_play(NES_SOUND_DROP);
    /* ONE call per event, and the level-up is one event. A clear that also
     * raises the level used to reach setMusicOrSoundEffect(MUSIC_LEVELUP)
     * twice in the same frame — once for the clear and once for the level —
     * and the engine restarts a track every time it is handed one, so the
     * intro began, was cut off a few hundred cycles later and began again.
     * That is what a doubled tune sounds like. */
    if (step.lines_collapsed && !step.leveled_up)
        nes_audio_play(NES_SOUND_LINECLEAR);
    /* THE COSSACK ANSWERS THE BOARD. Counted off the mask the core reports, so
     * a clear that took four rows gets four times the figure. */
    if (step.lines_collapsed) {
        int rows = 0;
        for (int i = 0; i < TENGEN_PF_HEIGHT; i++)
            if (step.rows_cleared_mask & (1u << i)) rows++;
        idle_cossack_celebrate(rows);
    }
    if (step.leveled_up) {
        /* The cartridge's level-up music takes over; the fifth tune stands
         * down and start_music() puts it back when the dancers finish. */
        korobeiniki_stop();
        nes_audio_play(NES_MUSIC_LEVELUP);
        /* MUSIC MIX turns over here, and here only. See MUSIC_MIX. */
        if (g_music == MUSIC_MIX) {
            g_mix_step = (uint8_t)((g_mix_step + 1) % MIX_COUNT);
            /* A linked match has no interlude to restart the tune afterwards,
             * so the mix's next one is asked for on the spot. The level-up
             * jingle was queued a moment ago and the ring is read one request
             * per frame, so it is heard first and this follows it. */
            if (g_linked) start_music(g_music);
        }
        if (!g_linked) {
            g_dancer_active = true;
            g_dancer_timer = DANCER_TIMER_START;
            g_dancer_tick = 0;
            g_dancer_elapsed = 0;
            /* The cast is read HERE, before the tally is emptied: L8D8B runs
             * at the top of showLevelBonus, and how well the level went is
             * what decides how many cossacks come on. */
            g_dancer_cast = tengen_dancer_count(&g_session.game);
        }
    }
    if (step.topped_out) {
        korobeiniki_stop();
        /* The game-over tune is class 8 like the title's, so the in-game
         * tune's own class has to be freed for it — which MUSIC_SILENCE does. */
        nes_audio_play(NES_MUSIC_SILENCE);
        nes_audio_play(NES_MUSIC_GAMEOVER);
    }
}

/* The level's colours and the falling piece's, each reinstalled the frame it
 * changes — the two things the ROM rewrites its palettes for. */
static void refresh_palettes(void) {
    const TengenPlayerState *p = &g_session.game.player[g_view];
    if (p->level != g_shown_level) {
        g_shown_level = p->level;
        set_field_palette_for_level(g_shown_level);
    }
    if (p->piece.current != g_shown_piece) {
        g_shown_piece = p->piece.current;
        set_piece_palette(g_shown_piece);
    }
}

/* True once neither board is playing, which is how a race ends. */
static bool match_over(void) {
    if (!g_session.game.two_player) return !g_session.game.player[g_view].game_active;
    return !g_session.game.player[0].game_active &&
            !g_session.game.player[1].game_active;
}

/* One frame of a linked match: pace the cable, then take whatever it brought.
 * Returns false when the match is finished — either both boards are done, or
 * the link stopped and cannot be trusted to have kept the two simulations
 * together. */
/* THE BUTTON THAT STARTED THE MATCH MUST NOT ALSO PAUSE IT.
 *
 * The core computes each player's fresh presses as `buttons & ~held_last`, and
 * a new game starts with `held_last` at zero — so a START still physically
 * down on the match's first frame reads as a press and pauses it on the spot.
 * Solo never showed it because the front end's own edge detector had already
 * eaten that press; over the cable the buttons travel as raw levels and arrive
 * a transfer later, with nothing in between to eat anything.
 *
 * Pretending everything is already held is the fix: nothing can edge until it
 * has been let go once. Both consoles do it at the same point of the same
 * code, so the lockstep is untouched. */
static void swallow_held_buttons(TengenGame *game) {
    for (int i = 0; i < 2; i++) game->player[i].held_last_frame = 0xFF;
}

static bool link_play_frame(void) {
    /* The master starts one transfer per frame off its own vblank; the slave
     * has nothing to start. Either way the interrupt does the collecting. */
    link_pump();
    link_tick();

    LinkFrame f;
    int stepped = 0;
    while (stepped < LINK_MAX_CATCHUP && link_pop(&f)) {
        uint16_t local  = link_is_master() ? f.master : f.slave;
        uint16_t remote = link_is_master() ? f.slave  : f.master;

        /* A multiplayer transfer hands back every console's word INCLUDING
         * this one's, which is the only honest account of what we actually
         * managed to send. If it is not the frame we thought we were on, the
         * other console has been fed a lie and there is nothing to do but
         * stop. */
        if (tengen_link_frame(local) !=
             (uint8_t)(g_session.frame & TENGEN_LINK_FRAME_MASK)) {
            g_session.desynced = true;
            break;
        }

        TengenStepResult out[2];
        if (!tengen_link_step(&g_session, tengen_link_buttons(local), remote, out))
            break;
        announce_step(out[g_view]);
        stepped++;
    }

    if (g_session.desynced || link_starved() > LINK_LOST_FRAMES) {
        g_link_lost = true;
        return false;
    }
    return !match_over();
}

/* Plays what this screen should be playing, and does nothing if it already
 * is — restarting a tune every frame would be a stutter, not music.
 *
 * EVERY FRONT-END SCREEN NAMES ITS TUNE, and one of the names is silence. That
 * matters more than it sounds: the title theme belongs to the TITLE and to
 * nothing else, so walking off it stops it, and walking back on starts it
 * again. An earlier pass let the theme carry on into GAME SELECT the way the
 * cartridge does, and then every path back out had to remember to put things
 * right — which is how the level screen's preview kept following the player
 * out to the title, and how a fast START could get the title theme layered
 * under a match's music.
 *
 * The cartridge does not need this because its front end is a one-way chain
 * with no way back. This port has B, so it does. */
static void front_music(uint8_t which) {
    if (g_front_tune == which) return;
    g_front_tune = which;
    /* A SCREEN THAT WANTS QUIET HAS TO SUSPEND, not silence. See stop_music:
     * MUSIC_SILENCE resets the engine for the next track and leaves the
     * current one playing, which is why the title theme used to follow the
     * player all the way to GAME SELECT. Suspending also takes the fireworks'
     * bangs down with it — they queue effects of their own on their way out —
     * while still letting the screen-switch blip that was queued a frame ago
     * be heard in full. */
    if (which == FRONT_SILENCE) {
        stop_music();
        return;
    }
    if (which == FRONT_TITLE_THEME) {
        /* FROM THE TOP, every time — and freeing its own class is what makes
         * that happen. LD0E4 refuses a class-8 song that is already in a slot
         * outright (main.asm.txt:8590-8597), so without this the theme would
         * simply carry on from wherever it was. */
        korobeiniki_stop();
        stop_music_class(NES_MUSIC_CLASS_TITLE);
        nes_audio_play(NES_MUSIC_SILENCE);
        nes_audio_play(NES_MUSIC_TITLESCREEN);
        return;
    }
    /* MUSIC MIX IS QUIET ON THE MENU, like NO MUSIC: there is no one tune to
     * preview, and the rotation belongs to the match. */
    if (which == MUSIC_MIX) {
        stop_music();
        return;
    }
    start_music(which);
}

/* One frame of a solo game: Start pauses, the cheat codes go in while paused
 * — both are the core's job (tengen_pause_input mirrors the ROM's own
 * pauseOrUnpause, which is where checkCodeInput lives). A code that fires
 * shows up on its own: a level-up through the palette check, a long bar or an
 * undo through the current-piece check. */
static bool solo_play_frame(uint8_t buttons, uint8_t pressed) {
    if (g_session.game.player[0].game_active) {
        uint8_t presses[2] = { pressed, 0 };
        TengenCheat cheat[2];
        bool was_paused = g_session.game.paused;
        tengen_pause_input(&g_session.game, presses, cheat);
        if (was_paused != g_session.game.paused) {
            /* pauseOrUnpause suspends and resumes the music
             * (main.asm.txt:7204-7211) — the same pair the front end uses to
             * go quiet, through the same two helpers so the port never loses
             * track of which state the engine is actually in. */
            nes_audio_play(g_session.game.paused ? NES_MUSIC_SUSPEND
                                                  : NES_MUSIC_RESUME);
            /* MUSIC_SUSPEND only silences the cartridge's engine. The fifth
             * tune has its own channels and has to be stopped and restarted
             * with it, or PAUSE would leave it playing on its own.
             *
             * THE MIX COUNTS. Asking `g_music == MUSIC_KOROBEINIKI` misses the
             * case where the tune playing is Korobeiniki because the MIX is on
             * its turn — and the mix OPENS on it, so it was every first level
             * of every mixed game: pause, and the fifth tune played on alone
             * over the plaque. */
            if (current_tune() == MUSIC_KOROBEINIKI) {
                if (g_session.game.paused) korobeiniki_stop();
                else korobeiniki_start();
            }
            /* The plaque has to be painted over on the way out, but this runs
             * mid-frame; six hundred tiles written into VRAM while the screen
             * is being scanned out is a visible tear. Flag it and let
             * draw_match do it inside the blank with everything else. */
            if (was_paused) g_repaint = true;
        }
        /* Every applied code plays this (main.asm.txt:7089, 7127). */
        if (cheat[0] != TENGEN_CHEAT_NONE)
            nes_audio_play(NES_SOUND_SCREEN_SWITCH);
    }

    announce_step(tengen_step(&g_session.game, TENGEN_PLAYER_1, buttons));
    return !match_over();
}

/* Everything the screen shows during a match.
 *
 * ORDER MATTERS HERE, and getting it wrong is what made the pieces flicker.
 * Everything below writes video memory — the tile map, OAM, palette RAM — and
 * on a GBA all three want to be written during the vertical blank. This used
 * to run nes_audio_frame() first, and that is not a small thing to do: it
 * steps a 6502 interpreter through a whole frame of the cartridge's sound
 * engine, which is thousands of instructions and eats the entire blank. Every
 * tile and sprite then landed while the screen was being scanned out, so a
 * falling piece could be drawn half in its old position and half in its new
 * one. So: vsync, then draw, then the audio in the time that is left. */
static void draw_match(bool *sweeping) {
    vsync();
    if (g_repaint) {
        draw_static_screen();
        g_repaint = false;
    }
    refresh_palettes();
    draw_field();
    draw_panel();
    if (g_link_lost) draw_text(BOX_R_IN + 1, BOX_TOP_IN + 2, "LINK", BANK_LABEL);

    /* The sweep's sprites, and the one tidy-up when it finishes. */
    if (g_session.game.player[g_view].line_clear_timer > 0) {
        draw_line_clear_sweep();
        *sweeping = true;
    } else if (*sweeping) {
        oam_hide_all();
        *sweeping = false;
    }

    /* Last, so they sit over whatever was just drawn. */
    if (!g_session.game.player[g_view].game_active) draw_game_over();
    if (g_session.game.paused) draw_pause_box();

    /* And the sound engine afterwards, out of the blank, where it costs
     * nothing but CPU time. */
    audio_frame();
}

int main(void) {
    upload_tiles();
    upload_palettes();
    set_field_palette_for_level(0);
    clear_screen();

    upload_title_tiles();
    upload_sprite_tiles();
    oam_hide_all();
    nes_audio_init();
    /* After nes_audio_init: this runs the cartridge's code, and the machine it
     * runs on is the sound engine's. */
    init_title_sprites();
    restart_title_sprites();

    REG_BG0CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK) | BG_PRIORITY(1);
    /* The statistics layer, three pixels to the right of the tile grid. A
     * negative scroll is what moves the picture the other way, and the field
     * is nine bits wide, so -3 is written as 512-3. */
    REG_BG1CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK_STATS) | BG_PRIORITY(0);
    set_offset_layer(STATS_SHIFT_PX);
    REG_BG1VOFS = 0;
    /* The counters' layer, two pixels below the tile grid and nothing else on
     * it — so its scroll is set once and never touched again. It shares the
     * offset layer's priority; the two never write the same cell, and where
     * priorities tie the lower-numbered background wins in any case. */
    REG_BG2CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK_PANEL) | BG_PRIORITY(0);
    REG_BG2HOFS = 0;
    REG_BG2VOFS = (uint16_t)(512 - PANEL_SHIFT_PX);
    /* The histogram's layer: the offset layer's three pixels across, and none
     * of the counters' two down. See SCREENBLOCK_HISTOGRAM. */
    REG_BG3CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK_HISTOGRAM) | BG_PRIORITY(0);
    REG_BG3HOFS = (uint16_t)(512 - STATS_SHIFT_PX);
    REG_BG3VOFS = STATS_LIFT_PX;
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_BG3 |
                   DCNT_OBJ | DCNT_OBJ_1D;

    Screen screen = SCREEN_TITLE;
    uint8_t start_level = 0;
    /* menuPlayer1Handicap / menuPlayer2Handicap ($04F3-$04F4). */
    uint8_t handicap[2] = { 0, 0 };
    uint8_t game_mode = GAME_1P;
    /* Which of the three settings the cursor is on. */
    int menu_field = MENU_FIELD_LEVEL;
    int link_wait_frames = 0;
    uint8_t held_last = 0;
    bool sweeping = false;   /* true while the line-clear sweep owns the OAM */
    bool match_running = false;
    TengenLobby lobby;

    /* The ROM steps its RNG once per frame from the main loop
     * (main.asm.txt:49-50), and whatever state it is in when Start is pressed
     * becomes the game's seed. Doing the same means the piece sequence
     * depends on when you start rather than being identical every session. */
    TengenRng seed_source;
    tengen_rng_seed(&seed_source, 0xACE1);

    for (;;) {
        uint8_t buttons = read_buttons();
        uint8_t pressed = (uint8_t)(buttons & ~held_last);
        held_last = buttons;
        tengen_rng_step(&seed_source);

        if (screen == SCREEN_TITLE) {
            /* initializeTitleScreen ends with this (main.asm.txt:4489). */
            front_music(FRONT_TITLE_THEME);
            set_offset_layer(TITLE_LOGO_SHIFT_PX);
            if (TITLE_SKIN_COUNT > 1 && shoulder_either()) {
                g_title_skin = (uint8_t)((g_title_skin + 1) % TITLE_SKIN_COUNT);
                nes_audio_play(NES_SOUND_CHIRP);
                vsync();
                install_title_palette();
                clear_screen();
                draw_title();
                oam_hide_all();
                audio_frame();
                continue;
            }
            if (pressed & MENU_ADVANCE) {
                screen = SCREEN_GAME_SELECT;
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                audio_frame();
                clear_screen();
                /* THE TITLE IS THE ONLY SCREEN WITH SPRITES ON IT. Leaving it
                 * without taking them down left the cathedral's central tower
                 * and a firework standing in the middle of GAME SELECT, LEVEL
                 * SELECT and everything after — sixty-three sprites that
                 * nothing else ever wrote to, so nothing else ever cleared. */
                oam_hide_all();
                continue;
            }
            vsync();
            draw_title();
            draw_title_sprites();
            audio_frame();
            continue;
        }

        if (screen == SCREEN_GAME_SELECT) {
            /* SILENT, and deliberately not what the cartridge does. Its title
             * theme carries on through here — but its front end is a one-way
             * chain, so the theme only ever plays forwards. This port can walk
             * back, and a theme that resumes behind you every time you press B
             * is worse than a menu that waits quietly. Leaving the cathedral
             * stops its music AND the fireworks' bangs with it. */
            front_music(FRONT_SILENCE);
            if (pressed & MENU_STEP) {
                game_mode = (pressed & MENU_BACKWARD)
                    ? (uint8_t)((game_mode + GAME_COUNT - 1) % GAME_COUNT)
                    : (uint8_t)((game_mode + 1) % GAME_COUNT);
                /* processMenuInput plays this on every move (:4655). */
                nes_audio_play(NES_SOUND_MENU_SELECT);
            }
            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_TITLE;
                restart_title_sprites();
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            if (pressed & MENU_CONFIRM) {
                /* ON A CABLE THE CHOOSING COMES AFTER THE CONNECTING. Only one
                 * of the two players should be picking a level and a tune, and
                 * neither console knows which one that is until the cable has
                 * told them — so 2 PLAYER goes straight to the lobby, and the
                 * master reaches the level screen from there. */
                if (game_mode == GAME_2P) {
                    uint16_t seed =
                        (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                    link_init();
                    link_lobby_start_held(&lobby, seed);
                    screen = SCREEN_LINK_WAIT;
                } else {
                    screen = SCREEN_LEVEL_SELECT;
                    menu_field = MENU_FIELD_LEVEL;
                }
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            vsync();
            draw_game_select(game_mode);
            audio_frame();
            continue;
        }

        if (screen == SCREEN_LEVEL_SELECT) {
            /* THE CABLE KEEPS TURNING while the master reads the menu. The
             * handshake is parked at its greeting, but it still has to happen:
             * a lobby that stops transferring looks exactly like a lobby whose
             * cable fell out, and the guest would give up after ten seconds of
             * the master thinking. */
            if (game_mode == GAME_2P) link_lobby_step(&lobby);

            /* ONE CALL EACH, and the results kept: these are edge detectors
             * with their own held state, so asking twice in a frame answers
             * "yes" and then "no" — which is how the handicap's first version
             * quietly ate the fifth tune's chord. */
            bool chord = shoulder_chord();
            bool tap_l = pressed_shoulder(SHOULDER_L);
            bool tap_r = pressed_shoulder(SHOULDER_R);

            /* UP/DOWN/SELECT MOVE THE CURSOR, LEFT/RIGHT CHANGE THE FIELD.
             * SELECT moving it the way DOWN does is the cartridge's
             * (LA048's carry-set add, $9FBC/$A063); the split between moving
             * and setting is the port's, and it is what lets three settings
             * share one page. */
            if (pressed & TENGEN_BTN_UP)
                menu_field = (menu_field + MENU_FIELD_COUNT - 1) % MENU_FIELD_COUNT;
            if (pressed & (TENGEN_BTN_DOWN | TENGEN_BTN_SELECT))
                menu_field = (menu_field + 1) % MENU_FIELD_COUNT;

            bool back = (pressed & TENGEN_BTN_LEFT) != 0;
            bool moved = (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) != 0;

            if (moved) {
                if (menu_field == MENU_FIELD_LEVEL) {
                    start_level = (uint8_t)((start_level +
                                              (back ? START_LEVEL_COUNT - 1 : 1))
                                             % START_LEVEL_COUNT);
                } else if (menu_field == MENU_FIELD_HANDICAP) {
                    /* In two players the pad reaches player 1's; the shoulders
                     * below are how player 2's is set, which is what the
                     * label says. */
                    handicap[0] = (uint8_t)((handicap[0] +
                                              (back ? TENGEN_HANDICAP_MAX : 1)) %
                                             (TENGEN_HANDICAP_MAX + 1));
                } else {
                    g_music = (uint8_t)((g_music + (back ? music_choices() - 1 : 1))
                                         % music_choices());
                }
            }

            /* One shoulder each, and only when they are NOT both down: the
             * chord is the fifth tune's, and a player reaching for it should
             * not be burying anybody on the way. They work wherever the cursor
             * is — that is the point of naming them in the label. */
            if (!chord && (tap_l || tap_r)) {
                int who = (tap_r && !tap_l && game_mode == GAME_2P) ? 1 : 0;
                handicap[who] = (uint8_t)((handicap[who] + 1) %
                                           (TENGEN_HANDICAP_MAX + 1));
                menu_field = MENU_FIELD_HANDICAP;   /* show what moved */
                moved = true;
            }

            if (!g_music_unlocked && chord) {
                /* L+R together — the two buttons a NES pad never had, so the
                 * game proper can never see this. It uncovers TWO entries:
                 * the fifth tune and the mix that plays all of them. */
                g_music_unlocked = true;
                g_music = MUSIC_KOROBEINIKI;
                menu_field = MENU_FIELD_MUSIC;   /* show what was uncovered */
                nes_audio_play(NES_SOUND_CHIRP);
            } else if (moved || (pressed & MENU_STEP)) {
                nes_audio_play(NES_SOUND_MENU_SELECT);
            }

            /* MOVING THE CURSOR PLAYS THE TUNE. The cartridge calls LA035 from
             * `$A00A` on every cursor move while gameState is
             * GAMESTATE_MUSIC_SELECT (main.asm.txt:4694-4696), so you hear each
             * one as you pick it. front_music does nothing when the tune has
             * not changed, so this also settles the music on arrival — which
             * is what stops the title theme here, and what keeps the screen
             * SILENT while NO MUSIC is the choice. */
            front_music(g_music);

            if (pressed & TENGEN_BTN_B) {
                /* The cartridge has no back button at all — its menus are a
                 * one-way chain with an idle timer — so B is the port's, for
                 * the same reason A confirms. */
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                screen = SCREEN_GAME_SELECT;
                /* Backing out of a 2P choice drops the cable with it. */
                if (game_mode == GAME_2P) link_shutdown();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            if (pressed & MENU_CONFIRM) {
                /* START, and only START, confirms on the cartridge ($A011);
                 * A is the port's second confirm, as everywhere else here. */
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);

                if (game_mode == GAME_2P) {
                    /* The master has chosen. Letting the handshake go delivers
                     * the seed, the level and the tune to the other console,
                     * and both leave the lobby together. */
                    link_lobby_release(&lobby, seed, start_level, g_music, handicap);
                    screen = SCREEN_LINK_WAIT;
                    vsync();
                    audio_frame();
                    clear_screen();
                    continue;
                }

                g_linked = false;
                g_link_lost = false;
                g_view = 0;
                tengen_new_game(&g_session.game, seed, start_level, false, false);
                swallow_held_buttons(&g_session.game);
                /* endPlayfieldInit's own place for it, right after the field
                 * is laid out (main.asm.txt:3536-3546). */
                tengen_apply_handicap(&g_session.game, TENGEN_PLAYER_1, handicap[0]);
                g_mix_step = 0;      /* every game opens on the same tune */
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                set_piece_palette(g_session.game.player[0].piece.current);
                oam_hide_all();
                g_idle_frame = 0;
                g_dance_frames = 0;
                screen = SCREEN_PLAYING;
                match_running = true;
                g_front_tune = FRONT_NOTHING;
                start_music(g_music);
                vsync();
                audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }
            vsync();
            draw_level_settings(menu_field, start_level, g_music, handicap,
                                 game_mode == GAME_2P);
            audio_frame();
            continue;
        }

        if (screen == SCREEN_LINK_WAIT) {
            /* Quiet while the cable is looking for the other end; there is
             * nothing to preview yet. */
            front_music(FRONT_SILENCE);
            /* One handshake transfer per frame until both consoles agree on a
             * seed, a level and a tune — or until the cable gives up. */
            link_lobby_step(&lobby);

            /* THE MASTER GOES OFF TO CHOOSE the moment the cable answers.
             * The guest stays here with the cossack: it has nothing to decide,
             * because the level and the tune are the master's.
             *
             * `hold` is what makes this happen once. Testing "connected and
             * not ready" instead sent the master straight back to the menu the
             * frame after it had chosen, and it ping-ponged there while the
             * other console went off and started the match alone. */
            if (lobby.hold && lobby.linked && link_is_master()) {
                screen = SCREEN_LEVEL_SELECT;
                menu_field = MENU_FIELD_LEVEL;
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                audio_frame();
                clear_screen();
                oam_hide_all();
                continue;
            }

            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_GAME_SELECT;
                link_shutdown();
                oam_hide_all();
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }

            if (lobby.ready) {
                /* The cable master is player 1. That is not a convention this
                 * port invented; it is the one fact both consoles can agree
                 * on without asking, because the hardware sets it from which
                 * end of the cable each is plugged into. */
                g_linked = true;
                g_link_lost = false;
                g_view = link_is_master() ? 0 : 1;
                /* The master's choice wins, the egg included: both consoles run
                   the same ROM, so a linked player who never found the code
                   still hears it. */
                g_music = lobby.music < MUSIC_UNLOCKED_COUNT ? lobby.music : 0;
                tengen_link_start(&g_session, lobby.seed, lobby.start_level,
                                   link_is_master() ? TENGEN_PLAYER_1 : TENGEN_PLAYER_2);
                swallow_held_buttons(&g_session.game);
                /* Both consoles bury both boards from the one seed the lobby
                 * delivered, so the two fields match without another word on
                 * the wire. */
                for (int i = 0; i < 2; i++)
                    tengen_apply_handicap(&g_session.game, (TengenPlayerSlot)i,
                                           lobby.handicap[i]);
                g_mix_step = 0;
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                set_piece_palette(g_session.game.player[g_view].piece.current);
                link_play_begin();
                screen = SCREEN_PLAYING;
                match_running = true;
                g_front_tune = FRONT_NOTHING;
                start_music(g_music);
                /* The lobby's cossack is four sprites nothing on the play
                 * screen ever writes to, so nothing there would ever have
                 * cleared him — he stood in the middle of the board. */
                oam_hide_all();
                g_idle_frame = 0;
                g_dance_frames = 0;
                vsync();
                audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }

            vsync();
            draw_link_wait(&lobby, link_wait_frames++);
            audio_frame();
            continue;
        }

        /* The level-up interlude holds the game still while the dancers
         * perform, the way the ROM switches to its bonus state. Any button
         * cuts it short, which is what the original does too
         * (main.asm.txt:9037-9045). It never runs in a linked match — see the
         * note above announce_step. */
        if (g_dancer_active) {
            /* One step of checkLevelUp: the timer advances every sixteenth
             * frame, a button jumps it to the wind-down, and the show ends
             * when it would pass $FF. */
            if (g_dancer_timer == DANCER_TIMER_WINDDOWN) {
                /* L9053, reached by `beq` BEFORE the silence: the natural end
                 * of the show does not cut the level-up music, a button does. */
                g_dancer_timer = DANCER_TIMER_TAIL;
                g_dancer_tick = 0;
            } else if (g_dancer_timer < DANCER_TIMER_WINDDOWN && pressed) {
                /* EIGHT-BIT ARITHMETIC, and the comparison is unsigned — that
                 * is the whole behaviour. `lda #$7C / sec / sbc timer / sbc #5`
                 * underflows, so an early press lands on $FB and a late one on
                 * $F5, and the clamp only catches what wrapped past it. Doing
                 * this in an int made every press give $F5, which is twice the
                 * wind-down the cartridge gives you for pressing at once. */
                uint8_t jump =
                    (uint8_t)(DANCER_TIMER_START - g_dancer_timer - 5);
                if (jump < DANCER_TIMER_TAIL) jump = DANCER_TIMER_TAIL;
                g_dancer_timer = jump;
                /* `and #$F0` on frameCounterLow: the next step starts fresh. */
                g_dancer_tick = 0;
                nes_audio_play(NES_MUSIC_SILENCE);
            }

            if (++g_dancer_tick >= DANCER_TICK_FRAMES) {
                g_dancer_tick = 0;
                if (g_dancer_timer == 0xFF) {
                    g_dancer_active = false;
                } else {
                    g_dancer_timer++;
                }
            }
            g_dancer_elapsed++;

            vsync();
            if (!g_dancer_active) {
                oam_hide_all();
                /* finishLevelUpAnimation empties the level's bonus tally on
                 * its way back to play (main.asm.txt:2476-2482), so the next
                 * level's cast is counted from zero. */
                tengen_clear_bonus_counts(&g_session.game);
                draw_static_screen();
                start_music(g_music);
            } else if (!g_show_banner) {
                /* HUD STATS keeps its screen. Nothing is cleared and nothing
                 * has to be put back; the panel redraws every frame anyway,
                 * and the cossack standing in it takes the show. */
                draw_panel();
            } else {
                /* The stage gets the WHOLE column, the way the cartridge's
                 * level-up blit gets the whole banner. Painting only the
                 * stage's own tiles left the NEXT box behind it — with a
                 * dancer standing inside it — and half of the STATS heading
                 * showing between the ledges. */
                clear_region(BOX_R_TX + 2, 0, BOX_W - 2, SCREEN_TH);
                clear_stats_layer();
                draw_field_braid(BOX_R_TX, kBraidLeft);
                draw_dancer_stage();
                draw_next_label_and_piece(BOX_L_TX, BOX_TOP_IN + 12, false, true);
                draw_dancers(g_dancer_elapsed, g_dancer_cast);
            }
            audio_frame();
            continue;
        }

        /* SELECT changes which cossack is standing in HUD STATS. The game
         * proper never reads SELECT (the cartridge's own pause and cheat
         * codes are on Start and the face buttons), so it is free, and the
         * four palettes are the only thing that separates the cartridge's six
         * dancers from each other. */
        if (screen == SCREEN_PLAYING && (pressed & TENGEN_BTN_SELECT)) {
            g_idle_palette = (uint8_t)((g_idle_palette + 1) % IDLE_PALETTE_COUNT);
        }

        /* L+R swaps the right-hand box between the piece histogram and the
         * cartridge's vertical TETRIS banner — while there is a match to swap
         * it around. Once the board is dead the only thing left to press is
         * the one that starts again. */
        if (screen == SCREEN_PLAYING && match_running && shoulder_chord()) {
            g_show_banner = !g_show_banner;
            /* Both directions need the static screen back: going TO the
             * banner erases the braid box, and coming back from it has to
             * redraw one. Without this the box's border kept whatever the
             * banner had left in it. */
            g_repaint = true;
            nes_audio_play(NES_SOUND_SCREEN_SWITCH);
        }

        if (match_running) {
            bool keep_going = g_linked ? link_play_frame()
                                        : solo_play_frame(buttons, pressed);
            if (!keep_going) {
                match_running = false;
                if (g_linked) {
                    link_play_end();
                    link_shutdown();
                }
            }
        }

        /* Any of the three goes back to the title once there is nothing left
         * to play. Start is the cartridge's own, and after a game over A and
         * B are the buttons a hand is already on.
         *
         * In a linked match this is read straight off this console's keypad
         * rather than over the cable — by now the cable is shut down, and
         * neither player should have to wait for the other to agree. */
#define GAMEOVER_RESTART (TENGEN_BTN_START | TENGEN_BTN_A | TENGEN_BTN_B)
        if (!match_running && (pressed & GAMEOVER_RESTART)) {
            screen = SCREEN_TITLE;
            restart_title_sprites();
            g_linked = false;
            g_link_lost = false;
            g_view = 0;
            oam_hide_all();
            sweeping = false;
            stop_music();
            /* THE TITLE HAS TO BE ASKED FOR AGAIN. Coming back here left the
             * match's screen underneath — including the statistics, which
             * live on their own background and so survived even a redraw of
             * the title's tiles and printed themselves over the cathedral.
             * And g_front_tune still held whatever the last menu chose, so
             * the title's own theme could decide it was already playing. */
            clear_screen();
            g_front_tune = FRONT_NOTHING;
            vsync();
            audio_frame();
            continue;
        }

        draw_match(&sweeping);
    }
}
