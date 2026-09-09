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
static void set_piece_palette(TengenTetromino piece) {
    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return;
    const uint8_t *entry = kRomPiecePalettes[piece];
    vu16 *bank = MEM_PALETTE + PAL_PIECE_BANK * 16;
    for (int i = 0; i < 3; i++) bank[1 + i] = nes_colour_to_gba(entry[i]);
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
        /* Once a dancer walks off the far side of the stage it stops there
         * rather than wandering into the score panel. */
        int limit = (DANCER_STAGE_TX + DANCER_STAGE_TW) * 8 - 16;
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
    for (int i = used; i < 128; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

static void set_map_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK)[ty * MAP_W + tx] = entry;
}

static void clear_region(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) set_map_tile(tx + x, ty + y, T_BLANK);
}

/* The statistics layer. Tile 0 of the cartridge's set is transparent in every
 * pixel, so everywhere this map is not written the screen is simply the one
 * below it. */
static void set_stats_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK_STATS)[ty * MAP_W + tx] = entry;
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
 * screen has a plain strip here anyway. */
static void draw_field_braid(int tx, const uint8_t run[1][2]) {
    for (int y = 0; y < SCREEN_TH; y++)
        for (int dx = 0; dx < BRAID_T; dx++)
            set_map_tile(tx + dx, y, WITH_BANK(run[0][dx], BRAID_BANK));
}

/* A panel of rope: the braid along the top, the bottom and the side facing the
 * board, and the screen's own edge closing it outward. `inner_right` says
 * which side of the panel the board is on.
 *
 * WHICH TILES, AND WHY THAT WAY ROUND. The rope is not symmetrical — it is
 * woven, and the weave leans. kBraidLeft is the cartridge's LEFT screen border
 * (tiles $6A $6B): everything it frames is to its right. kBraidRight ($73 $74)
 * is the right border, framing what is to its left. The board is what these
 * runs frame, so the run with the board on ITS right takes the left-border
 * tiles, and the corners follow the same rule.
 *
 * That is also exactly what the cartridge's own screen has where the reflow
 * puts these columns — $6A $6B beside the board's left edge, $73 $74 beside
 * its right — so drawing it the other way round did not merely look odd, it
 * overwrote the ROM's art with its own mirror image. The tell was L+R: the
 * banner's strip is drawn from the ROM's tiles, so the weave flipped direction
 * as the panel came and went.
 *
 * The corners are only ever drawn on the board side, because that is the only
 * side that has one — the other simply runs off the screen. */
static void draw_braid_panel(int tx, int w, bool inner_right) {
    int ix = inner_right ? tx + w - BRAID_T : tx;   /* the inner run's column */

    for (int dy = 0; dy < BRAID_T; dy++) {
        for (int dx = 0; dx < BRAID_T; dx++) {
            set_map_tile(ix + dx, dy,
                          WITH_BANK(inner_right ? kBraidTL[dy][dx]
                                                : kBraidTR[dy][dx], BRAID_BANK));
            set_map_tile(ix + dx, SCREEN_TH - BRAID_T + dy,
                          WITH_BANK(inner_right ? kBraidBL[dy][dx]
                                                : kBraidBR[dy][dx], BRAID_BANK));
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
                          WITH_BANK(inner_right ? kBraidLeft[0][dx]
                                                : kBraidRight[0][dx], BRAID_BANK));

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

static void draw_next_piece(int tx, int ty) {
    clear_region(tx, ty, 4, 3);
    TengenTetromino next = g_session.game.player[g_view].piece.next;
    if (next <= TT_NONE || next >= TENGEN_TETROMINO_COUNT) return;
    /* Drawn from the same orientation bitmap and tile table the game logic
     * uses, so the preview cannot drift out of sync with what spawns. */
    int occupied = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!tengen_piece_occupies(next, 0, r, c)) continue;
            uint8_t tile = tengen_tile_id_for_cell(next, 0, occupied);
            occupied++;
            if (r < 3) set_map_tile(tx + c, ty + r, WITH_BANK(tile, PAL_PIECE_BANK));
        }
    }
}

/* WHAT THE RIGHT-HAND BOX SHOWS.
 *
 * The cartridge's play screen has a vertical TETRIS banner between its two
 * halves, and thirty columns cannot hold that AND two boxes wide enough to be
 * useful. So it is a choice the player makes: L+R together — the two buttons
 * a NES pad never had and this game therefore never uses — swaps the right
 * box between the piece histogram and the banner. */
static bool g_show_banner;

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
#define STATS_TOP_TY (BOX_TOP_IN + 4)                   /* bars from row 6 */
#define STATS_BAR_ROWS (STATS_ICON_TY - STATS_TOP_TY)   /* ten of them */
#define STATS_BAR_FULL (SCREEN_1P_STATS_BAR_TILE + 7)
#define BANK_STATS SCREEN_1P_STATS_BAR_BANK
/* Seven tiles in eight columns, so one spare. It is not left at either end:
 * the block rides the second background, which is scrolled STATS_SHIFT_PX so
 * that the strip's own ink — inset two pixels on its left and flush on its
 * right — comes out five pixels from the rope and five from the screen edge.
 * See SCREENBLOCK_STATS. */
#define STATS_TX BOX_R_IN

static void clear_stats_layer(void) {
    for (int y = STATS_TOP_TY; y <= STATS_ICON_TY + 1; y++)
        for (int x = 0; x < BOX_IN; x++) set_stats_tile(STATS_TX + x, y, T_BLANK);
}

static void draw_stats(const TengenPlayerState *p) {
    for (int i = 0; i < SCREEN_1P_STATS_PIECES; i++) {
        int tx = STATS_TX + i;
        /* Each icon in the palette the ROM's attribute table gives it: the
         * I has its own, T/O/J/L share one, S and Z share another. */
        int icon_bank = kStatsIconBanks[i];
        set_stats_tile(tx, STATS_ICON_TY, WITH_BANK(kStatsIcons[0][i], icon_bank));
        set_stats_tile(tx, STATS_ICON_TY + 1, WITH_BANK(kStatsIcons[1][i], icon_bank));

        uint16_t n = p->piece_stats[TT_I + i];
        int full = n / 8;
        int part = n % 8;
        if (full > STATS_BAR_ROWS) { full = STATS_BAR_ROWS; part = 0; }

        for (int r = 0; r < STATS_BAR_ROWS; r++) {
            int ty = STATS_ICON_TY - 1 - r;
            uint16_t tile = T_BLANK;
            if (r < full) tile = STATS_BAR_FULL;
            else if (r == full && part) tile = SCREEN_1P_STATS_BAR_TILE + part - 1;
            set_stats_tile(tx, ty, WITH_BANK(tile, BANK_STATS));
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
static void draw_counter(int ty, int label_first, int label_count,
                          uint32_t value, int digits, int value_indent) {
    draw_label(BOX_L_IN, ty, label_first, label_count);
    clear_region(BOX_L_IN, ty + 1, BOX_L_W, 1);
    draw_number(BOX_L_IN + value_indent, ty + 1, value, digits, BANK_VALUE);
    for (int x = 0; x < BOX_L_W; x++)
        set_map_tile(BOX_L_IN + x, ty + 2, WITH_BANK(T_GRID_RULE, BANK_LABEL));
}
/* NEXT where it belongs, at the top of the right panel over the statistics —
 * and, when something else has that panel, in the left one under the
 * counters instead. */
static void draw_next_label_and_piece(int tx, int ty) {
    draw_label(tx + 2, ty, HUD_LABEL_NEXT);
    draw_next_piece(tx + 2, ty + 1);
}

static void draw_panel(void) {
    const TengenPlayerState *p = &g_session.game.player[g_view];
    if (p->score > g_high_score) g_high_score = p->score;

    draw_counter(ROW_SCORE, HUD_LABEL_SCORE, p->score, 6, 0);
    draw_counter(ROW_LINES, HUD_LABEL_LINES, p->lines, 4, 1);
    draw_counter(ROW_LEVEL, HUD_LABEL_LEVEL, p->level, 2, 2);

    if (g_session.game.two_player) {
        /* A race wants the other board's numbers where the high score would
         * be. The ROM keeps no piece histogram in 2P either, so nothing of
         * the cartridge's is being displaced. */
        const TengenPlayerState *o = &g_session.game.player[g_view ^ 1];
        draw_text(BOX_L_IN, ROW_HIGH, "RIVAL", BANK_LABEL);
        for (int x = 0; x < BOX_L_W; x++)
            set_map_tile(BOX_L_IN + x, ROW_HIGH + 2, WITH_BANK(T_GRID_RULE, BANK_LABEL));
        clear_region(BOX_L_IN, ROW_HIGH + 1, BOX_L_W, 1);
        draw_number(BOX_L_IN, ROW_HIGH + 1, o->score, 6, BANK_VALUE);
    } else {
        /* The cartridge's own 1P panel carries a HIGH SCORE beside the score
         * — "HIGH" and "SCORE" in plain ASCII at nametable row 2, and
         * highScoreHundredThousands is the seventh entry of
         * statsDataAddresses (main.asm.txt:4100-4107). Kept for the session
         * rather than saved: this cartridge has no battery either. */
        draw_text(BOX_L_IN + 1, ROW_HIGH, "HIGH", BANK_LABEL);
        for (int x = 0; x < BOX_L_W; x++)
            set_map_tile(BOX_L_IN + x, ROW_HIGH + 2, WITH_BANK(T_GRID_RULE, BANK_LABEL));
        clear_region(BOX_L_IN, ROW_HIGH + 1, BOX_L_W, 1);
        draw_number(BOX_L_IN, ROW_HIGH + 1, g_high_score, 6, BANK_VALUE);
    }

    /* Rows 14-17 of the left panel: NEXT lodges here only when the right one
     * is taken, and is blank otherwise. */
    clear_region(BOX_L_IN, BOX_TOP_IN + 12, BOX_L_W, 4);
    if (g_show_banner) draw_next_label_and_piece(BOX_L_IN, BOX_TOP_IN + 12);

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
        clear_region(BOX_R_TX + 2, 0, BOX_W - 2, SCREEN_TH);
        clear_stats_layer();
        draw_field_braid(BOX_R_TX, kBraidRight);
        draw_banner();
    } else if (!g_session.game.two_player) {
        draw_next_label_and_piece(BOX_R_IN, ROW_NEXT);
        draw_stats(p);
    } else {
        clear_region(BOX_R_IN, BOX_TOP_IN, BOX_IN, BOX_BOT_IN - BOX_TOP_IN + 1);
        clear_stats_layer();
        if (!g_session.game.player[g_view ^ 1].game_active)
            draw_text(BOX_R_IN + 1, BOX_TOP_IN, "OUT", BANK_LABEL);
    }
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
#define MUSIC_UNLOCKED_COUNT (MUSIC_COUNT + 1)
static bool g_music_unlocked;

static const char *const kMusicNames[MUSIC_UNLOCKED_COUNT] = {
    "NO MUSIC", "LOGINSKA", "BRADINSKY", "KARINKA", "TROIKA", "KOROBEINIKI"
};

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

/* Starts whichever tune is chosen, on whichever engine owns it. The two never
 * play at once: the cartridge's is told to go silent for the hand-entered one
 * and keeps running, so the sound EFFECTS are the ROM's either way. */
/* The track half of the pair above, on its own: whoever has just queued a
 * silence calls this. Kept separate because the queue is only eight deep and
 * DROPS what does not fit ($CFC3), so a spare silence is not free — walking
 * the menus quickly used to be able to lose the one that mattered and leave
 * two tunes layered. */
static void play_after_silence(uint8_t music) {
    if (music == MUSIC_KOROBEINIKI) {
        korobeiniki_start();
        return;
    }
    korobeiniki_stop();
    uint8_t track = kMusicTracks[music < MUSIC_COUNT ? music : 0];
    /* musicSelectTable's first entry IS the silence, and it has just been
     * queued; asking for it twice would only cost a queue slot. */
    if (track != NES_MUSIC_SILENCE) nes_audio_play(track);
}

static void start_music(uint8_t music) {
    /* SILENCE FIRST, ALWAYS. This is `LA035` (main.asm.txt:4730-4735), which
     * is the cartridge's own way of starting a tune:
     *
     *     lda #MUSIC_SILENCE / jsr setMusicOrSoundEffect
     *     ldy menuMusic / lda musicSelectTable,y / jmp setMusicOrSoundEffect
     *
     * and it is not decoration. setMusicOrSoundEffect only QUEUES a request
     * ($0200-$0207, a ring with its indices at $0208/$0209); handing the
     * engine a new track without silencing the old one leaves the old one's
     * channels running underneath, which is why the title theme could still
     * be heard on top of a match's music. */
    nes_audio_play(NES_MUSIC_SILENCE);
    play_after_silence(music);
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

static void clear_screen(void) {
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < MAP_W; tx++) {
            set_map_tile(tx, ty, T_BLANK);
            set_stats_tile(tx, ty, T_BLANK);
        }
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

static void draw_title(void) {
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
            set_map_tile(pad + tx, ty,
                          WITH_BANK(tile, PAL_TITLE_BASE + kScreenTitlePalettes[i]));
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
    for (int i = 0; i < TITLE_OAM_COUNT; i++) {
        int ny = oam[i * 4];
        uint8_t tile = oam[i * 4 + 1];
        uint8_t attr = oam[i * 4 + 2];
        int nx = oam[i * 4 + 3];
        /* The NES hides a sprite by parking it below the visible 240 lines;
         * this code uses $F7 for exactly that. */
        int row = (ny >= 0 && ny < 240) ? kTitleRowMap[ny / 8] : SCREEN_TITLE_ROW_DROPPED;
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
        int x = ((SCREEN_TW - SCREEN_TITLE_W) / 2 + col) * 8 + (nx & 7);
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
static unsigned music_name_len(uint8_t music) {
    unsigned n = 0;
    while (kMusicNames[music][n]) n++;
    return n;
}

/* The cartridge's menu frame, which every selection screen is drawn inside. */
static void draw_menu_frame(void) {
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
    draw_text(10, 8, "GAME SELECT", PAL_MENU_BASE + 3);
    for (int i = 0; i < GAME_COUNT; i++)
        draw_text(11, 10 + i * 2, kGameNames[i],
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
    draw_text(6, 15, "BY ALEXEY PAJITNOV", PAL_MENU_BASE + 3);
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
#define GUEST_DANCER_Y 92

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
    draw_text(11, 8, "LINK CABLE", PAL_MENU_BASE + 3);

    clear_region(4, 11, 22, 5);
    if (lobby->failed) {
        oam_hide_all();
        draw_text(9, 11, "NO CABLE FOUND", BANK_HILITE);
        draw_text(8, 14, "B TO GO BACK", PAL_MENU_BASE + 3);
        return;
    }
    if (!link_connected()) {
        oam_hide_all();
        draw_text(6, 11, "WAITING FOR PLAYER 2", PAL_MENU_BASE + 3);
        draw_text(8, 14, "B TO GO BACK", PAL_MENU_BASE + 3);
        return;
    }
    /* Connected. The master has gone off to choose; this console is the guest,
     * so it says who it is and lets the cossack do the waiting. */
    draw_text(9, 11, "YOU ARE PLAYER 2", BANK_HILITE);
    draw_guest_dancer(elapsed);
}

static void draw_level_select(uint8_t start_level, uint8_t music) {
    draw_menu_frame();

    /* Rows 8-14 of this window are the space the cartridge's own selection
     * screens write into — its GAME SELECT list lives exactly there — so the
     * port's selection goes in the same place rather than over the frame or
     * the big TETRIS logo above it. */
    draw_text(9, 8, "LEVEL SELECT", PAL_MENU_BASE + 3);
    for (int level = 0; level < START_LEVEL_COUNT; level++) {
        /* The chosen level is picked out in a different palette, the way the
         * ROM highlights a menu selection. */
        draw_number(6 + level * 2, 10, (uint32_t)level, 1,
                     level == start_level ? BANK_HILITE : PAL_MENU_BASE + 3);
    }
    draw_text(6, 11, "LEFT RIGHT TO SET", PAL_MENU_BASE + 3);

    draw_text(12, 14, "MUSIC", PAL_MENU_BASE + 3);
    clear_region(4, 15, 22, 1);
    draw_text(15 - (int)(music_name_len(music) / 2), 15,
               kMusicNames[music], BANK_HILITE);
    draw_text(7, 16, "UP DOWN TO PICK", PAL_MENU_BASE + 3);
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

static uint8_t g_music = 1;
/* The interlude's clock, which is the ROM's player1FallTimer: `active` while
 * the show is on, `timer` counting $7C..$FF at one step every sixteen frames.
 * See the note beside DANCER_TIMER_START for why it is shaped like this. */
static bool g_dancer_active;
static uint8_t g_dancer_timer;
static uint16_t g_dancer_tick;   /* stands in for frameCounterLow & $0F */
static int g_dancer_elapsed;     /* frames since the show started, for the poses */
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
    if (step.leveled_up) {
        /* The cartridge's level-up music takes over; the fifth tune stands
         * down and start_music() puts it back when the dancers finish. */
        korobeiniki_stop();
        nes_audio_play(NES_MUSIC_LEVELUP);
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
    korobeiniki_stop();
    /* Silence first, always, for the reason LA035 does; see start_music. It
     * is also what stops the fireworks' bangs dead when the title is left:
     * they queue sound effects of their own, and only a silence clears them. */
    nes_audio_play(NES_MUSIC_SILENCE);
    if (which == FRONT_TITLE_THEME) {
        nes_audio_play(NES_MUSIC_TITLESCREEN);
    } else if (which != FRONT_SILENCE) {
        play_after_silence(which);
    }
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
             * (main.asm.txt:7204-7211). */
            nes_audio_play(g_session.game.paused ? NES_MUSIC_SUSPEND : NES_MUSIC_RESUME);
            /* MUSIC_SUSPEND only silences the cartridge's engine. The fifth
             * tune has its own channels and has to be stopped and restarted
             * with it, or PAUSE would leave it playing on its own. */
            if (g_music == MUSIC_KOROBEINIKI) {
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
    REG_BG1HOFS = (uint16_t)(512 - STATS_SHIFT_PX);
    REG_BG1VOFS = 0;
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_OBJ | DCNT_OBJ_1D;

    Screen screen = SCREEN_TITLE;
    uint8_t start_level = 0;
    uint8_t game_mode = GAME_1P;
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
            /* Left/right wrap at both ends, the range and the wrapping the
             * ROM's own menu uses (main.asm.txt:4742-4763, 4819). */
            if (pressed & TENGEN_BTN_LEFT)
                start_level = (uint8_t)((start_level + START_LEVEL_COUNT - 1) % START_LEVEL_COUNT);
            if (pressed & TENGEN_BTN_RIGHT)
                start_level = (uint8_t)((start_level + 1) % START_LEVEL_COUNT);
            if (!g_music_unlocked && shoulder_chord()) {
                /* L+R together — the two buttons a NES pad never had, so the
                 * game proper can never see this. */
                g_music_unlocked = true;
                g_music = MUSIC_KOROBEINIKI;
                nes_audio_play(NES_SOUND_CHIRP);
            }
            if (pressed & MENU_STEP) {
                g_music = (pressed & MENU_BACKWARD)
                    ? (uint8_t)((g_music + music_choices() - 1) % music_choices())
                    : (uint8_t)((g_music + 1) % music_choices());
            }
            /* The click first and the tune after it, which is the order the
             * cartridge queues them in: SOUND_MENU_SELECT at $9FC4, LA035 at
             * $A00A (main.asm.txt:4655, 4696). */
            if (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT | MENU_STEP))
                nes_audio_play(NES_SOUND_MENU_SELECT);
            /* MOVING THE CURSOR PLAYS THE TUNE. The cartridge calls LA035 from
             * `$A00A` on every cursor move while gameState is
             * GAMESTATE_MUSIC_SELECT (main.asm.txt:4694-4696), so you hear each
             * one as you pick it. front_music does nothing when the tune has
             * not changed, so this also settles the music on arrival — which
             * is what stops the title theme here. */
            front_music(g_music);
            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_GAME_SELECT;
                /* Backing out of a 2P choice drops the cable with it. */
                if (game_mode == GAME_2P) link_shutdown();
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            if (pressed & MENU_CONFIRM) {
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);

                if (game_mode == GAME_2P) {
                    /* The master has chosen. Letting the handshake go delivers
                     * the seed, the level and the tune to the other console,
                     * and both leave the lobby together. */
                    link_lobby_release(&lobby, seed, start_level, g_music);
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
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                set_piece_palette(g_session.game.player[0].piece.current);
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
            draw_level_select(start_level, g_music);
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
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                set_piece_palette(g_session.game.player[g_view].piece.current);
                link_play_begin();
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
            } else {
                /* The stage gets the WHOLE column, the way the cartridge's
                 * level-up blit gets the whole banner. Painting only the
                 * stage's own tiles left the NEXT box behind it — with a
                 * dancer standing inside it — and half of the STATS heading
                 * showing between the ledges. */
                clear_region(BOX_R_TX + 2, 0, BOX_W - 2, SCREEN_TH);
                clear_stats_layer();
                draw_field_braid(BOX_R_TX, kBraidRight);
                draw_dancer_stage();
                draw_next_label_and_piece(BOX_L_IN, BOX_TOP_IN + 12);
                draw_dancers(g_dancer_elapsed, g_dancer_cast);
            }
            audio_frame();
            continue;
        }

        /* L+R swaps the right-hand box between the piece histogram and the
         * cartridge's vertical TETRIS banner. */
        if (screen == SCREEN_PLAYING && shoulder_chord()) {
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

        /* Start goes back to the title once there is nothing left to play.
         * In a linked match this is read straight off this console's keypad
         * rather than over the cable — by now the cable is shut down, and
         * neither player should have to wait for the other to agree. */
        if (!match_running && (pressed & TENGEN_BTN_START)) {
            screen = SCREEN_TITLE;
            restart_title_sprites();
            g_linked = false;
            g_link_lost = false;
            g_view = 0;
            oam_hide_all();
            sweeping = false;
            korobeiniki_stop();
            nes_audio_play(NES_MUSIC_SILENCE);
            vsync();
            audio_frame();
            continue;
        }

        draw_match(&sweeping);
    }
}
