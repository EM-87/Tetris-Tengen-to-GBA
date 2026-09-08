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
 *     horizontally: 32 columns -> 30. The NES screen is really TWO framed
 *       board areas side by side (2P uses both; 1P covers the second with its
 *       score panel), and the two columns that go are that second area's own
 *       frame at columns 18-19 — the one thing the 1P screen has no use for.
 *       Everything else survives at original size. Done in extract_assets.py.
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
#include "../src/tengen_core.h"

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

/* The panel the reflow leaves free, in GBA tile columns. */
#define PANEL_TX 18
#define PANEL_W  10

/* Palette banks. Banks 0-3 are the ROM's four background palettes; bank 0 is
 * additionally rewritten per level, which is what setPlayfieldPaletteFromLevel
 * does on the NES (main.asm.txt:5328). Bank 4 carries the falling piece's own
 * colours, mirroring setPiecePalette's separate sprite palette. */
#define PAL_PIECE_BANK 4
#define PAL_TITLE_BANK 5

/* The title screen has its own 256-tile set, uploaded above the game's so
 * both live in one charblock (512 tiles is exactly its 16KB). */
#define TITLE_TILE_BASE 256

/* ----------------------------------------------------------------------- *
 * The between-levels dancers
 *
 * On the NES these are sprites, eight of them, each built from four 8x8
 * tiles in a 2x2 block. A per-dancer script steps through poses every 8
 * frames while the figure walks sideways and flips horizontally
 * (main.asm.txt:6392-6499). The level-up blit clears the vertical TETRIS
 * banner to make room, which is how we know that column is their stage.
 *
 * Reproduced here with the cartridge's own dancer tiles and pose table, at
 * the same cadence and in the same place. The one thing NOT taken from the
 * ROM is each dancer's individual choreography script — those scripts have
 * branch and random-selection entries that this pass did not trace, so the
 * dancers here simply walk the pose table from staggered starting points.
 * See reference/NOTES.md.
 * ----------------------------------------------------------------------- */
#define DANCER_COUNT 8
#define DANCER_SPRITES 4              /* four 8x8 tiles in a 2x2 per dancer */
#define DANCER_POSE_FRAMES 8          /* pose advance cadence, from the ROM */
#define DANCER_WALK_FRAMES 4          /* X advance cadence, from the ROM */
#define DANCER_SHOW_FRAMES 200        /* how long the interlude lasts */
#define PAL_OBJ_DANCER 0

/* Their stage: the banner column, in the tile coordinates the level-up blit
 * clears. On the NES that blit is 4 columns x 18 rows at nametable (14,10),
 * which inside this port's window is columns 14-17, rows 2-19. */
#define DANCER_STAGE_TX 14
#define DANCER_STAGE_TY 2
#define DANCER_STAGE_TW 4
#define DANCER_STAGE_TH 18
#define DANCER_STAGE_X (DANCER_STAGE_TX * 8)
#define DANCER_STAGE_Y (DANCER_STAGE_TY * 8)

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
#define PAL_OBJ_CLEAR 1

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
 * Here it sits two columns further left, which is the same shift the divider
 * itself took to fit 32 columns into 30, so it lands in the same place
 * relative to the art around it.
 * ----------------------------------------------------------------------- */
#define PAUSE_TX 10
#define PAUSE_TY 0
#define PAUSE_W 8
#define PAUSE_H 2
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

static TengenGame g_game;

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

static void upload_tiles(void) {
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK);
    const uint8_t *src = kGameTiles;
    for (unsigned i = 0; i < sizeof(kGameTiles); i += 2) {
        dst[i / 2] = (uint16_t)(src[i] | (src[i + 1] << 8));
    }
}

/* The four background palettes exactly as the cartridge stores them. Entry 0
 * of each is the shared backdrop. */
static void upload_palettes(void) {
    for (int bank = 0; bank < 4; bank++) {
        vu16 *dst = MEM_PALETTE + bank * 16;
        for (int i = 0; i < 4; i++) {
            dst[i] = nes_colour_to_gba(kRomBgPalette[bank * 4 + i]);
        }
    }
    vu16 *piece = MEM_PALETTE + PAL_PIECE_BANK * 16;
    piece[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
}

/* setPlayfieldPaletteFromLevel (main.asm.txt:5328) recolours the settled
 * field using the LEVEL'S ONES DIGIT as the index, which is why the colours
 * cycle every ten levels. It writes background palette 0, entries 1-3. */
static void set_field_palette_for_level(uint8_t level) {
    const uint8_t *entry = kRomPiecePalettes[level % 10];
    vu16 *bank0 = MEM_PALETTE;
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
    vu16 *pal = MEM_PALETTE + PAL_TITLE_BANK * 16;
    for (int i = 0; i < 4; i++) pal[i] = nes_colour_to_gba(kTitlePalette[i]);
}

/* The cartridge's whole sprite bank, uploaded once. Both the dancers and the
 * line-clear puff live in it, so every sprite tile id in this file is the
 * ROM's own index. */
static void upload_sprite_tiles(void) {
    vu16 *dst = MEM_OBJ_TILES;
    for (unsigned i = 0; i < sizeof(kDancerTiles); i += 2) {
        dst[i / 2] = (uint16_t)(kDancerTiles[i] | (kDancerTiles[i + 1] << 8));
    }
    /* piecePaletteIndexB is labelled "bonus animation" in the disassembly
     * (main.asm.txt:5397-5399) — the level-up interlude's own colours. */
    const uint8_t *entry = kRomPiecePalettes[11];
    vu16 *pal = MEM_PALETTE_OBJ + PAL_OBJ_DANCER * 16;
    pal[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    for (int i = 0; i < 3; i++) pal[1 + i] = nes_colour_to_gba(entry[i]);

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

/* Places the eight dancers for one frame of the interlude. */
static void draw_dancers(int elapsed) {
    int pose_step = elapsed / DANCER_POSE_FRAMES;
    int walk = elapsed / DANCER_WALK_FRAMES;

    for (int d = 0; d < DANCER_COUNT; d++) {
        /* Staggered starting poses so the eight are not in lockstep. */
        int pose = (pose_step + d * 7) % DANCER_POSE_COUNT;
        const uint8_t *tiles = kDancerPoses[pose];

        /* Two columns of four, filling the banner's stage. They walk, and
         * turn around at the edges — the ROM flips them the same way. */
        int lane = d & 1;
        int row = d >> 1;
        int travel = (walk + d * 5) % 32;
        bool facing_left = travel >= 16;
        int offset = facing_left ? (31 - travel) : travel;

        /* Two lanes of four, evenly filling the 32x144 stage. */
        int x = DANCER_STAGE_X + lane * 16 + (offset >> 3);
        int y = DANCER_STAGE_Y + row * 36;

        for (int s = 0; s < DANCER_SPRITES; s++) {
            int sx = x + ((s & 1) ? 8 : 0);
            int sy = y + ((s & 2) ? 8 : 0);
            oam_set(d * DANCER_SPRITES + s, sx, sy, tiles[s], facing_left,
                     PAL_OBJ_DANCER);
        }
    }
}

/* The puff of smoke crossing each completed row: five sprites in a row, the
 * head at the column the sweep has reached and the rest trailing one column
 * apart behind it, each retiring as it leaves the field. */
static void draw_line_clear_sweep(void) {
    const TengenPlayerState *p = &g_game.player[0];
    uint8_t step = tengen_line_clear_step(&g_game, TENGEN_PLAYER_1);
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

/* The horizontal rules the NES draws under each label, built from the same
 * cap-and-middle tiles it uses. */
static void draw_rule(int tx, int ty, int width, int bank) {
    set_map_tile(tx, ty, WITH_BANK(T_RULE_LEFT, bank));
    for (int i = 1; i < width - 1; i++) set_map_tile(tx + i, ty, WITH_BANK(T_RULE_MID, bank));
    set_map_tile(tx + width - 1, ty, WITH_BANK(T_RULE_RIGHT, bank));
}

static void clear_region(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) set_map_tile(tx + x, ty + y, T_BLANK);
}

/* Palette banks for panel content, chosen to match how the NES colours the
 * same lettering in its header strip. */
#define BANK_LABEL 1   /* gold, as the ROM draws SCORE/LEVEL */
#define BANK_VALUE 2   /* blue/white, as it draws the counters */

static void draw_next_piece(int tx, int ty) {
    clear_region(tx, ty, 4, 3);
    TengenTetromino next = g_game.player[0].piece.next;
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

static void draw_panel(void) {
    const TengenPlayerState *p = &g_game.player[0];
    static const char kPieceLetter[TENGEN_TETROMINO_COUNT] = {0,'I','T','O','J','L','S','Z'};

    draw_tiles(PANEL_TX, 0, kLabelScore, 6, BANK_LABEL);
    draw_number(PANEL_TX, 1, p->score, 6, BANK_VALUE);

    draw_tiles(PANEL_TX, 2, kLabelLines, 6, BANK_LABEL);
    draw_number(PANEL_TX, 3, p->lines, 4, BANK_VALUE);

    draw_tiles(PANEL_TX, 4, kLabelLevel, 6, BANK_LABEL);
    draw_number(PANEL_TX, 5, p->level, 2, BANK_VALUE);

    draw_tiles(PANEL_TX, 6, kLabelNext, 4, BANK_LABEL);
    draw_next_piece(PANEL_TX, 7);

    draw_rule(PANEL_TX, 10, PANEL_W, BANK_VALUE);
    draw_text(PANEL_TX + 2, 11, "STATS", BANK_LABEL);
    for (int piece = TT_I; piece <= TT_Z; piece++) {
        int ty = 12 + (piece - TT_I);
        char letter[2] = { kPieceLetter[piece], 0 };
        draw_text(PANEL_TX + 1, ty, letter, BANK_LABEL);
        draw_number(PANEL_TX + 3, ty, p->piece_stats[piece], 3, BANK_VALUE);
    }

    if (!p->game_active) {
        draw_text(PANEL_TX + 2, 19, "OVER", BANK_LABEL);
    } else {
        clear_region(PANEL_TX + 2, 19, 4, 1);
    }
}

static void draw_field(void) {
    const TengenPlayfield *field = &g_game.field[0];
    const TengenPlayerState *p = &g_game.player[0];

    /* How far the line-clear sweep has crossed the completed rows, and what
     * it is writing into them as it goes. `written` is the last column the
     * trailing sprite has passed over; everything to its right still shows
     * the blocks that are about to come down. */
    uint8_t step = tengen_line_clear_step(&g_game, TENGEN_PLAYER_1);
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
    int count = tengen_active_piece_cells(&g_game, TENGEN_PLAYER_1, cells);
    TengenTetromino current = g_game.player[0].piece.current;
    for (int i = 0; i < count; i++) {
        if (cells[i].row < 0) continue;
        int col = cells[i].col - FIELD_COL0;
        if (col < 0 || col >= FIELD_PLAYABLE) continue;
        uint8_t tile = tengen_tile_id_for_cell(current, g_game.player[0].piece.orientation, i);
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
 * its own menu frame; this follows the same shape with the one selection the
 * port currently offers. */
typedef enum { SCREEN_TITLE, SCREEN_LEVEL_SELECT, SCREEN_PLAYING } Screen;

static void clear_screen(void) {
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < MAP_W; tx++) set_map_tile(tx, ty, T_BLANK);
}

/* The cartridge's own title art, whole: the level selector has its own
 * screen after this one, the way the ROM's menus work. */
static void draw_title(void) {
    for (int ty = 0; ty < SCREEN_TITLE_H_TILES; ty++) {
        for (int tx = 0; tx < SCREEN_TITLE_W; tx++) {
            uint16_t tile = TITLE_TILE_BASE + kScreenTitleTiles[ty * SCREEN_TITLE_W + tx];
            set_map_tile(tx, ty, WITH_BANK(tile, PAL_TITLE_BANK));
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

static void draw_level_select(uint8_t start_level, uint8_t music) {
    for (int ty = 0; ty < SCREEN_MENU_H_TILES; ty++) {
        for (int tx = 0; tx < SCREEN_MENU_W; tx++) {
            int i = ty * SCREEN_MENU_W + tx;
            set_map_tile(tx, ty, WITH_BANK(kScreenMenuTiles[i], kScreenMenuPalettes[i]));
        }
    }

    draw_text(9, 4, "LEVEL SELECT", BANK_LABEL);
    for (int level = 0; level < START_LEVEL_COUNT; level++) {
        /* The chosen level is picked out in the label colour, the way the
         * ROM highlights a menu selection. */
        draw_number(6 + level * 2, 9, (uint32_t)level, 1,
                     level == start_level ? BANK_LABEL : BANK_VALUE);
    }
    draw_text(6, 12, "LEFT RIGHT TO SET", BANK_VALUE);

    draw_text(10, 15, "MUSIC", BANK_LABEL);
    clear_region(6, 16, 18, 1);
    draw_text(10 - (int)(music_name_len(music) / 2) + 2, 16,
               kMusicNames[music], BANK_VALUE);
    draw_text(8, 17, "UP DOWN TO PICK", BANK_VALUE);
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
    uint8_t shown_level = 0xFF;
    TengenTetromino shown_piece = TT_NONE;
    uint8_t held_last = 0;
    int dancer_frames = 0;   /* > 0 while the level-up interlude is running */
    bool sweeping = false;   /* true while the line-clear sweep owns the OAM */
    uint8_t music = 0;       /* which of the four in-game tunes */
    bool title_music = false;

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
                screen = SCREEN_LEVEL_SELECT;
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
                vsync();
                nes_audio_frame();
                clear_screen();
                continue;
            }
            vsync();
            nes_audio_frame();
            draw_title();
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
                music = (uint8_t)((music + MUSIC_COUNT - 1) % MUSIC_COUNT);
            if (pressed & TENGEN_BTN_DOWN)
                music = (uint8_t)((music + 1) % MUSIC_COUNT);
            /* processMenuInput plays this on every move (main.asm.txt:4655). */
            if (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT |
                            TENGEN_BTN_UP | TENGEN_BTN_DOWN))
                nes_audio_play(NES_SOUND_MENU_SELECT);
            if (pressed & TENGEN_BTN_START) {
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                tengen_new_game(&g_game, seed, start_level, false, false);
                shown_level = 0xFF;
                shown_piece = TT_NONE;
                set_piece_palette(g_game.player[0].piece.current);
                screen = SCREEN_PLAYING;
                title_music = false;
                nes_audio_play(kMusicTracks[music]);
                vsync();
                nes_audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }
            vsync();
            nes_audio_frame();
            draw_level_select(start_level, music);
            continue;
        }

        /* The level-up interlude holds the game still while the dancers
         * perform, the way the ROM switches to its bonus state. Any button
         * cuts it short, which is what the original does too
         * (main.asm.txt:9037-9045). */
        if (dancer_frames > 0) {
            if (pressed) dancer_frames = 1;
            dancer_frames--;
            if (dancer_frames == 0) {
                oam_hide_all();
                draw_static_screen();
                nes_audio_play(kMusicTracks[music]);
            } else {
                clear_region(DANCER_STAGE_TX, DANCER_STAGE_TY,
                              DANCER_STAGE_TW, DANCER_STAGE_TH);
                draw_dancers(DANCER_SHOW_FRAMES - dancer_frames);
            }
            vsync();
            nes_audio_frame();
            continue;
        }

        /* Start pauses, and the cheat codes go in while paused — both are
         * the core's job (tengen_pause_input mirrors the ROM's own
         * pauseOrUnpause, which is where checkCodeInput lives). A code that
         * fires shows up on its own: a level-up through the palette check
         * below, a long bar or an undo through the current-piece check. */
        if (g_game.player[0].game_active) {
            uint8_t presses[2] = { pressed, 0 };
            TengenCheat cheat[2];
            bool was_paused = g_game.paused;
            tengen_pause_input(&g_game, presses, cheat);
            if (was_paused != g_game.paused) {
                /* pauseOrUnpause suspends and resumes the music
                 * (main.asm.txt:7204-7211). */
                nes_audio_play(g_game.paused ? NES_MUSIC_SUSPEND : NES_MUSIC_RESUME);
                if (was_paused) draw_static_screen();
            }
            /* Every applied code plays this (main.asm.txt:7089, 7127). */
            if (cheat[0] != TENGEN_CHEAT_NONE)
                nes_audio_play(NES_SOUND_SCREEN_SWITCH);
        }

        TengenStepResult step = tengen_step(&g_game, TENGEN_PLAYER_1, buttons);

        /* The ROM's own cues, each at the moment it plays them:
         *  - a piece coming to rest, L8417 (main.asm.txt:637)
         *  - rows coming down, L95C1 (:3212) — unless that clear also raised
         *    the level, in which case the intro takes its place (:3207)
         *  - the level-up interlude itself, L8D6B (:2038)
         *  - topping out, silence and then the game-over tune (:608, :620) */
        if (step.piece_locked) nes_audio_play(NES_SOUND_DROP);
        if (step.lines_collapsed)
            nes_audio_play(step.leveled_up ? NES_MUSIC_LEVELUP : NES_SOUND_LINECLEAR);
        if (step.leveled_up) {
            dancer_frames = DANCER_SHOW_FRAMES;
            nes_audio_play(NES_MUSIC_LEVELUP);
        }
        if (step.topped_out) {
            nes_audio_play(NES_MUSIC_SILENCE);
            nes_audio_play(NES_MUSIC_GAMEOVER);
        }

        if (!g_game.player[0].game_active && (pressed & TENGEN_BTN_START)) {
            screen = SCREEN_TITLE;
            oam_hide_all();
            nes_audio_play(NES_MUSIC_SILENCE);
            vsync();
            nes_audio_frame();
            continue;
        }

        if (g_game.player[0].level != shown_level) {
            shown_level = g_game.player[0].level;
            set_field_palette_for_level(shown_level);
        }
        if (g_game.player[0].piece.current != shown_piece) {
            shown_piece = g_game.player[0].piece.current;
            set_piece_palette(shown_piece);
        }

        vsync();
        nes_audio_frame();
        draw_field();
        draw_panel();

        /* The sweep's sprites, and the one tidy-up when it finishes. */
        if (g_game.player[0].line_clear_timer > 0) {
            draw_line_clear_sweep();
            sweeping = true;
        } else if (sweeping) {
            oam_hide_all();
            sweeping = false;
        }

        /* Last, so it sits over whatever was just drawn. */
        if (g_game.paused) draw_pause_box();
    }
}
