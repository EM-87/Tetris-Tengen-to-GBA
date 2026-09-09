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
#include "link.h"
#include "../src/tengen_core.h"
#include "../src/tengen_link.h"

/* Generated from a cartridge dump by tools/extract_assets.py. */
#include "tiles_game.h"
#include "tiles_dancers.h"
#include "tiles_title.h"
#include "screen_1p.h"
#include "screen_title.h"
#include "screen_menu.h"
#include "palettes_rom.h"
#include "dancer_poses.h"

#define CHARBLOCK   0
#define SCREENBLOCK 28  /* 28 * 2KB = 56KB in, clear of the 8KB of tile data */

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

/* THE TWO BOXES.
 *
 * The reflow leaves eight blank columns of the cartridge's own score-panel
 * canvas on each side of the framed board — 8 | 2 | 10 | 2 | 8 — and those are
 * the two boxes the HUD lives in. Same width, same rows, mirrored about the
 * board: the screen reads the same from either edge.
 *
 * Left holds the counters, right holds the next piece and the piece
 * histogram, and during a level-up the right box is handed over to the
 * dancers, which is what the cartridge does with its banner. */
#define BOX_L_TX 0
#define BOX_R_TX 22
#define BOX_W    8

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
#define DANCER_STAGE_TX (BOX_R_TX + 2)
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

static const uint8_t kLabelScore[6] = {0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72};
static const uint8_t kLabelLines[6] = {0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F};
static const uint8_t kLabelLevel[6] = {0x7A, 0x80, 0x81, 0x82, 0x83, 0x84};
static const uint8_t kLabelNext[4]  = {0x91, 0x92, 0x93, 0x94};

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
static void draw_dancers(int elapsed) {
    int pose_step = elapsed / DANCER_POSE_FRAMES;
    int walk = elapsed / DANCER_WALK_FRAMES;

    for (int d = 0; d < DANCER_COUNT; d++) {
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

/* Paints the cartridge's own screen: the braided border, the vertical TETRIS
 * banner, every decorative tile, each with the palette the ROM's attribute
 * table assigns it. */
static void draw_static_screen(void) {
    for (int ty = 0; ty < SCREEN_TH; ty++) {
        int layout_row = ty + WINDOW_TOP;
        for (int tx = 0; tx < SCREEN_TW; tx++) {
            int i = layout_row * SCREEN_1P_W + tx;
            set_map_tile(tx, ty, WITH_BANK(kScreen1pTiles[i], kScreen1pPalettes[i]));
        }
    }
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

static void draw_tiles(int tx, int ty, const uint8_t *tiles, int count, int bank) {
    for (int i = 0; i < count; i++) set_map_tile(tx + i, ty, WITH_BANK(tiles[i], bank));
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

static void clear_region(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) set_map_tile(tx + x, ty + y, T_BLANK);
}

/* A panel, in the cartridge's own thin frame.
 *
 * The tiles are the ones the GAME OVER plaque is built from — corners, a top
 * and bottom edge, and two sides — so nothing here is drawn by hand. What was
 * here before was a "rule" made of $75/$76 with $79 as a right-hand cap, and
 * $79 IS NOT A CAP: it is an unrelated block, which is why every counter had
 * a grey stub hanging off it. The header strip's real rules on the cartridge
 * run the width of the screen and are junctions of a grid this port has no
 * room for; boxes are the honest substitute, and they are made of the ROM's
 * own frame. */
/* The interior is cleared with the frame: a box is a box, not a border laid
 * over whatever the last frame left there. Without this, swapping the left
 * column from HIGH SCORE (six digits, columns 1-6) to NEXT (a four-column
 * preview at columns 2-5) left the first and last digit of the old score
 * sitting either side of the piece. */
static void draw_box(int tx, int ty, int w, int h, int bank) {
    clear_region(tx + 1, ty + 1, w - 2, h - 2);
    for (int x = 1; x < w - 1; x++) {
        set_map_tile(tx + x, ty, WITH_BANK(T_BOX_TOP, bank));
        set_map_tile(tx + x, ty + h - 1, WITH_BANK(T_BOX_BOTTOM, bank));
    }
    for (int y = 1; y < h - 1; y++) {
        set_map_tile(tx, ty + y, WITH_BANK(T_BOX_L, bank));
        set_map_tile(tx + w - 1, ty + y, WITH_BANK(T_BOX_R, bank));
    }
    set_map_tile(tx, ty, WITH_BANK(T_BOX_TL, bank));
    set_map_tile(tx + w - 1, ty, WITH_BANK(T_BOX_TR, bank));
    set_map_tile(tx, ty + h - 1, WITH_BANK(T_BOX_BL, bank));
    set_map_tile(tx + w - 1, ty + h - 1, WITH_BANK(T_BOX_BR, bank));
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
#define BANNER_TX (BOX_R_TX + 2)
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
/* SEVEN COLUMNS DO NOT FIT INSIDE AN EIGHT-WIDE BOX. A bordered box leaves a
 * six-column interior, and there are seven pieces — so the histogram is not
 * boxed; it is headed, the way the cartridge heads it. On the NES 1P screen
 * (nametable rows 8-10 of SCREEN_1P) the word STATS sits between two grey
 * rules, the icons stand on the frame's own bottom edge, and there is nothing
 * else in the column. That is what this reproduces: rule, heading, rule, then
 * open black down to the icons. An earlier pass drew the top rule alone at row
 * 6, immediately under the NEXT box's own bottom edge, which read as two
 * borders stacked and as a grey bar belonging to nothing. */
#define STATS_TX BOX_R_TX                /* seven columns, one spare at the end */
#define STATS_HEAD_TY 6                  /* rule / STATS / rule */
#define STATS_ICON_TY 17                 /* the icons stand on the floor */
#define STATS_TOP_TY 9                   /* ...and the bars may reach this row */
#define STATS_BAR_FULL (SCREEN_1P_STATS_BAR_TILE + 7)
#define STATS_MAX_ROWS (STATS_ICON_TY - STATS_TOP_TY)
#define BANK_STATS SCREEN_1P_STATS_BAR_BANK

static void draw_stats(const TengenPlayerState *p) {
    for (int i = 0; i < SCREEN_1P_STATS_PIECES; i++) {
        int tx = STATS_TX + i;
        /* Each icon in the palette the ROM's attribute table gives it: the
         * I has its own, T/O/J/L share one, S and Z share another. */
        int icon_bank = kStatsIconBanks[i];
        set_map_tile(tx, STATS_ICON_TY, WITH_BANK(kStatsIcons[0][i], icon_bank));
        set_map_tile(tx, STATS_ICON_TY + 1, WITH_BANK(kStatsIcons[1][i], icon_bank));

        uint16_t count = p->piece_stats[TT_I + i];
        int full = count / 8;
        int part = count % 8;
        if (full > STATS_MAX_ROWS) { full = STATS_MAX_ROWS; part = 0; }

        for (int r = 0; r < STATS_MAX_ROWS; r++) {
            int ty = STATS_ICON_TY - 1 - r;
            uint16_t tile = T_BLANK;
            if (r < full) tile = STATS_BAR_FULL;
            else if (r == full && part) tile = SCREEN_1P_STATS_BAR_TILE + part - 1;
            set_map_tile(tx, ty, WITH_BANK(tile, BANK_STATS));
        }
    }
}

/* WHERE NEXT GOES WHEN SOMETHING ELSE WANTS THE RIGHT COLUMN.
 *
 * Two things claim the whole right-hand column: the vertical TETRIS banner
 * (eighteen rows) and the between-levels dancers (whose stage is the blit the
 * cartridge paints over its own banner). Both push NEXT into the left column
 * rather than taking it off the screen — a Tetris you cannot see the next
 * piece in is not a trade anybody wants to make for a decoration. */
static void draw_next_in_left_box(void) {
    draw_box(BOX_L_TX, 12, BOX_W, 6, BANK_VALUE);
    draw_tiles(BOX_L_TX + 2, 13, kLabelNext, 4, BANK_LABEL);
    draw_next_piece(BOX_L_TX + 2, 14);
    clear_region(BOX_L_TX, 18, BOX_W, 2);
}

/* One counter in its own box: label on the first row inside, value on the
 * second. Four rows tall, the full width of the panel. */
static void draw_counter(int ty, const uint8_t *label, int label_len,
                          uint32_t value, int digits, int value_indent) {
    draw_box(BOX_L_TX, ty, BOX_W, 4, BANK_VALUE);
    draw_tiles(BOX_L_TX + 1, ty + 1, label, label_len, BANK_LABEL);
    draw_number(BOX_L_TX + 1 + value_indent, ty + 2, value, digits, BANK_VALUE);
}

static void draw_panel(void) {
    const TengenPlayerState *p = &g_session.game.player[g_view];
    if (p->score > g_high_score) g_high_score = p->score;

    /* LEFT BOX: one framed counter each, in the cartridge's own multi-tile
     * lettering and its own frame. */
    draw_counter(0, kLabelScore, 6, p->score, 6, 0);
    draw_counter(4, kLabelLines, 6, p->lines, 4, 1);
    draw_counter(8, kLabelLevel, 6, p->level, 2, 2);

    if (g_session.game.two_player) {
        /* A race wants the other board's numbers where the high score would
         * be. The ROM keeps no piece histogram in 2P either, so nothing of
         * the cartridge's is being displaced. */
        const TengenPlayerState *o = &g_session.game.player[g_view ^ 1];
        draw_box(BOX_L_TX, 12, BOX_W, 8, BANK_VALUE);
        draw_text(BOX_L_TX + 2, 13, "RIVAL", BANK_LABEL);
        draw_number(BOX_L_TX + 1, 14, o->score, 6, BANK_VALUE);
        draw_text(BOX_L_TX + 1, 16, "LN", BANK_LABEL);
        draw_number(BOX_L_TX + 3, 16, o->lines, 4, BANK_VALUE);
        draw_text(BOX_L_TX + 1, 17, "LV", BANK_LABEL);
        draw_number(BOX_L_TX + 4, 17, o->level, 2, BANK_VALUE);
        if (!o->game_active) draw_text(BOX_L_TX + 2, 18, "OUT", BANK_LABEL);
        else clear_region(BOX_L_TX + 2, 18, 4, 1);
    } else {
        /* The cartridge's own 1P panel carries a HIGH SCORE beside the score
         * — "HIGH" and "SCORE" in plain ASCII at nametable row 2, and
         * highScoreHundredThousands is the seventh entry of
         * statsDataAddresses (main.asm.txt:4100-4107). Kept for the session
         * rather than saved: this cartridge has no battery either. */
        if (!g_show_banner) {
            draw_box(BOX_L_TX, 12, BOX_W, 5, BANK_VALUE);
            draw_text(BOX_L_TX + 2, 13, "HIGH", BANK_LABEL);
            draw_number(BOX_L_TX + 1, 14, g_high_score, 6, BANK_VALUE);
            clear_region(BOX_L_TX, 17, BOX_W, 3);
        }
    }

    /* NEXT: in the right-hand box normally, in the left-hand one while the
     * banner has the right column. */
    if (g_show_banner) {
        draw_next_in_left_box();
    } else {
        draw_box(BOX_R_TX, 0, BOX_W, 6, BANK_VALUE);
        draw_tiles(BOX_R_TX + 2, 1, kLabelNext, 4, BANK_LABEL);
        draw_next_piece(BOX_R_TX + 2, 2);
    }

    if (g_show_banner) {
        /* The banner is eighteen rows, which is the whole column: NEXT moves
         * into the left-hand box for the duration rather than disappearing,
         * because a Tetris you cannot see the next piece in is not a trade
         * anybody wants to make for a decoration. */
        clear_region(BOX_R_TX, 0, BOX_W, SCREEN_TH);
        draw_banner();
    } else if (!g_session.game.two_player) {
        /* Headed, not boxed; see the note beside STATS_TX. */
        for (int x = 0; x < BOX_W; x++) {
            set_map_tile(BOX_R_TX + x, STATS_HEAD_TY,
                          WITH_BANK(T_BOX_TOP, BANK_VALUE));
            set_map_tile(BOX_R_TX + x, STATS_HEAD_TY + 2,
                          WITH_BANK(T_BOX_TOP, BANK_VALUE));
        }
        clear_region(BOX_R_TX, STATS_HEAD_TY + 1, BOX_W, 1);
        draw_text(BOX_R_TX + 1, STATS_HEAD_TY + 1, "STATS", BANK_LABEL);
        clear_region(BOX_R_TX, 19, BOX_W, 1);
        draw_stats(p);
    } else {
        clear_region(BOX_R_TX, 6, BOX_W, 14);
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
#define MUSIC_COUNT 4
static const uint8_t kMusicTracks[MUSIC_COUNT] = {
    NES_MUSIC_LOGINSKA, NES_MUSIC_BRADINSKY, NES_MUSIC_KARINKA, NES_MUSIC_TROIKA
};
static const char *const kMusicNames[MUSIC_COUNT] = {
    "LOGINSKA", "BRADINSKY", "KARINKA", "TROIKA"
};

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
        for (int tx = 0; tx < MAP_W; tx++) set_map_tile(tx, ty, T_BLANK);
}

/* The cartridge's own title art, whole: the level selector has its own
 * screen after this one, the way the ROM's menus work. */
static void draw_title(void) {
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
     * says who actually wrote the game. */
    draw_text(4, 14, "2 PLAYER USES A CABLE", PAL_MENU_BASE + 3);
    /* The menu frame's black interior is columns 3-26 — twenty-four of them —
     * so the full "TETRIS BY ALEXEY PAJITNOV" (twenty-five) ran over the braid
     * at both ends. The word TETRIS is already six tiles tall above this. */
    draw_text(6, 17, "BY ALEXEY PAJITNOV", PAL_MENU_BASE + 3);
}

/* What the lobby is doing, while it does it. Two consoles reach this screen
 * independently, so it has to say which one this is and whether the other has
 * turned up — otherwise a cable that is plugged in badly looks the same as a
 * friend who has not pressed Start yet. */
static void draw_link_wait(const TengenLobby *lobby) {
    draw_menu_frame();
    draw_text(11, 8, "LINK CABLE", PAL_MENU_BASE + 3);

    clear_region(4, 11, 22, 5);
    if (lobby->failed) {
        draw_text(9, 11, "NO CABLE FOUND", BANK_HILITE);
        draw_text(8, 14, "B TO GO BACK", PAL_MENU_BASE + 3);
        return;
    }
    if (!link_connected()) {
        draw_text(6, 11, "WAITING FOR PLAYER 2", PAL_MENU_BASE + 3);
        draw_text(8, 14, "B TO GO BACK", PAL_MENU_BASE + 3);
        return;
    }
    draw_text(9, 11, link_is_master() ? "YOU ARE PLAYER 1" : "YOU ARE PLAYER 2",
               BANK_HILITE);
    /* The master's level and music are the ones that count, and on the slave
     * they change under its feet as the handshake delivers them — which is
     * exactly what the player needs to see. */
    draw_text(10, 14, "LEVEL", PAL_MENU_BASE + 3);
    draw_number(16, 14, lobby->start_level, 2, PAL_MENU_BASE + 3);
    draw_text(9, 16, kMusicNames[lobby->music < MUSIC_COUNT ? lobby->music : 0],
               PAL_MENU_BASE + 3);
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
static uint8_t g_music;          /* which of the four in-game tunes */
/* The interlude's clock, which is the ROM's player1FallTimer: `active` while
 * the show is on, `timer` counting $7C..$FF at one step every sixteen frames.
 * See the note beside DANCER_TIMER_START for why it is shaped like this. */
static bool g_dancer_active;
static uint8_t g_dancer_timer;
static uint16_t g_dancer_tick;   /* stands in for frameCounterLow & $0F */
static int g_dancer_elapsed;     /* frames since the show started, for the poses */
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
        nes_audio_play(NES_MUSIC_LEVELUP);
        if (!g_linked) {
            g_dancer_active = true;
            g_dancer_timer = DANCER_TIMER_START;
            g_dancer_tick = 0;
            g_dancer_elapsed = 0;
        }
    }
    if (step.topped_out) {
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
    if (g_link_lost) draw_text(BOX_R_TX + 2, 8, "LINK", BANK_LABEL);

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
    nes_audio_frame();
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

    REG_BG0CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK);
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

    Screen screen = SCREEN_TITLE;
    uint8_t start_level = 0;
    uint8_t game_mode = GAME_1P;
    uint8_t held_last = 0;
    bool sweeping = false;   /* true while the line-clear sweep owns the OAM */
    bool title_music = false;
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
            if (!title_music) {
                /* initializeTitleScreen ends with this (main.asm.txt:4489). */
                nes_audio_play(NES_MUSIC_TITLESCREEN);
                title_music = true;
            }
            if (pressed & TENGEN_BTN_START) {
                screen = SCREEN_GAME_SELECT;
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                nes_audio_frame();
                clear_screen();
                continue;
            }
            vsync();
            draw_title();
            nes_audio_frame();
            continue;
        }

        if (screen == SCREEN_GAME_SELECT) {
            if (pressed & TENGEN_BTN_UP)
                game_mode = (uint8_t)((game_mode + GAME_COUNT - 1) % GAME_COUNT);
            if (pressed & TENGEN_BTN_DOWN)
                game_mode = (uint8_t)((game_mode + 1) % GAME_COUNT);
            /* processMenuInput plays this on every move (main.asm.txt:4655). */
            if (pressed & (TENGEN_BTN_UP | TENGEN_BTN_DOWN))
                nes_audio_play(NES_SOUND_MENU_SELECT);
            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_TITLE;
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                nes_audio_frame();
                clear_screen();
                continue;
            }
            if (pressed & TENGEN_BTN_START) {
                screen = SCREEN_LEVEL_SELECT;
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                nes_audio_frame();
                clear_screen();
                continue;
            }
            vsync();
            draw_game_select(game_mode);
            nes_audio_frame();
            continue;
        }

        if (screen == SCREEN_LEVEL_SELECT) {
            /* Left/right wrap at both ends, the range and the wrapping the
             * ROM's own menu uses (main.asm.txt:4742-4763, 4819). */
            if (pressed & TENGEN_BTN_LEFT)
                start_level = (uint8_t)((start_level + START_LEVEL_COUNT - 1) % START_LEVEL_COUNT);
            if (pressed & TENGEN_BTN_RIGHT)
                start_level = (uint8_t)((start_level + 1) % START_LEVEL_COUNT);
            if (pressed & TENGEN_BTN_UP)
                g_music = (uint8_t)((g_music + MUSIC_COUNT - 1) % MUSIC_COUNT);
            if (pressed & TENGEN_BTN_DOWN)
                g_music = (uint8_t)((g_music + 1) % MUSIC_COUNT);
            if (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT |
                            TENGEN_BTN_UP | TENGEN_BTN_DOWN))
                nes_audio_play(NES_SOUND_MENU_SELECT);
            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_GAME_SELECT;
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                nes_audio_frame();
                clear_screen();
                continue;
            }
            if (pressed & TENGEN_BTN_START) {
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);

                if (game_mode == GAME_2P) {
                    /* Both consoles offer the seed they happen to hold; the
                     * cable decides whose counts, because only the master's
                     * survives the handshake. */
                    link_init();
                    link_lobby_start(&lobby, seed, start_level, g_music);
                    screen = SCREEN_LINK_WAIT;
                    vsync();
                    nes_audio_frame();
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
                title_music = false;
                nes_audio_play(kMusicTracks[g_music]);
                vsync();
                nes_audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }
            vsync();
            draw_level_select(start_level, g_music);
            nes_audio_frame();
            continue;
        }

        if (screen == SCREEN_LINK_WAIT) {
            /* One handshake transfer per frame until both consoles agree on a
             * seed, a level and a tune — or until the cable gives up. */
            link_lobby_step(&lobby);

            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_LEVEL_SELECT;
                link_shutdown();
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                nes_audio_frame();
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
                g_music = lobby.music < MUSIC_COUNT ? lobby.music : 0;
                tengen_link_start(&g_session, lobby.seed, lobby.start_level,
                                   link_is_master() ? TENGEN_PLAYER_1 : TENGEN_PLAYER_2);
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                set_piece_palette(g_session.game.player[g_view].piece.current);
                link_play_begin();
                screen = SCREEN_PLAYING;
                match_running = true;
                title_music = false;
                nes_audio_play(kMusicTracks[g_music]);
                vsync();
                nes_audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }

            vsync();
            draw_link_wait(&lobby);
            nes_audio_frame();
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
            if (pressed) {
                int jump = DANCER_TIMER_START - g_dancer_timer - 5;
                if (jump < DANCER_TIMER_TAIL) jump = DANCER_TIMER_TAIL;
                if (g_dancer_timer < DANCER_TIMER_TAIL) {
                    g_dancer_timer = (uint8_t)jump;
                    nes_audio_play(NES_MUSIC_SILENCE);
                }
            } else if (g_dancer_timer == DANCER_TIMER_WINDDOWN) {
                g_dancer_timer = DANCER_TIMER_TAIL;
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
                draw_static_screen();
                nes_audio_play(kMusicTracks[g_music]);
            } else {
                /* The stage gets the WHOLE column, the way the cartridge's
                 * level-up blit gets the whole banner. Painting only the
                 * stage's own tiles left the NEXT box behind it — with a
                 * dancer standing inside it — and half of the STATS heading
                 * showing between the ledges. */
                clear_region(BOX_R_TX, 0, BOX_W, SCREEN_TH);
                draw_dancer_stage();
                draw_next_in_left_box();
                draw_dancers(g_dancer_elapsed);
            }
            nes_audio_frame();
            continue;
        }

        /* L+R swaps the right-hand box between the piece histogram and the
         * cartridge's vertical TETRIS banner. */
        if (screen == SCREEN_PLAYING && shoulder_chord()) {
            g_show_banner = !g_show_banner;
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
            g_linked = false;
            g_link_lost = false;
            g_view = 0;
            oam_hide_all();
            sweeping = false;
            nes_audio_play(NES_MUSIC_SILENCE);
            vsync();
            nes_audio_frame();
            continue;
        }

        draw_match(&sweeping);
    }
}
