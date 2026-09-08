/*
 * main.c — GBA front end for the verified Tengen Tetris core.
 *
 * All the game rules live in ../src/tengen_core.c and know nothing about the
 * GBA. This file does three things and nothing else: read the keypad into
 * TengenButton bits, call tengen_step once per frame, and draw the field.
 * Keeping the split that clean is what lets `make test` verify the rules on
 * the host and this file stay small enough to eyeball.
 *
 * SCREEN LAYOUT — this is the resolution mapping the whole project rests on:
 *
 *   The playfield is 12 stored columns x 20 rows of 8x8 tiles = 96 x 160 px
 *   (10 playable columns plus the two wall columns; coop plays all 12).
 *   160 px is EXACTLY the GBA's screen height, so the field needs no scaling
 *   or cropping at all — it maps tile-for-tile, the same as on NES. That
 *   leaves 240 - 96 = 144 px, i.e. 72 px on each side, for the HUD. The NES
 *   had 176 px of surround to work with, so the HUD is the only thing that
 *   has to be redesigned for the narrower screen; the field itself is 1:1.
 *
 * GRAPHICS STATUS: the block tiles below are placeholders generated at
 * runtime, not the real game art. The genuine 8x8 CHR graphics live in the
 * original ROM (the disassembly build pulls them from `gfx/game_tileset.chr`,
 * see reference/disasm/entry.asm.txt) and are not part of this repo. The
 * renderer is deliberately built around 8x8 tiles and the core's own tile-id
 * tables so that dropping the real CHR in later is a data change, not a
 * rewrite. See ../README.md.
 */
#include "gba_hw.h"
#include "font.h"
#include "palette.h"
#include "../src/tengen_core.h"

#define CHARBLOCK   0
#define SCREENBLOCK 28  /* 28 * 2KB = 56KB in, clear of the tile data below */

#define MAP_W 32        /* a 32x32 tile background */
#define SCREEN_TW 30    /* visible tiles across */
#define SCREEN_TH 20    /* visible tiles down */

/* Where the playfield's top-left tile sits on screen, in 8x8 tiles. */
#define FIELD_ORIGIN_TX ((SCREEN_TW - TENGEN_PF_WIDTH) / 2)  /* centred: 9 */
#define FIELD_ORIGIN_TY 0                                     /* full height, no margin */

/* The two HUD panels the narrower GBA screen leaves either side of the
 * field: 9 tiles (72px) each, versus the NES's 11 (88px). */
#define PANEL_L_TX 0
#define PANEL_R_TX (FIELD_ORIGIN_TX + TENGEN_PF_WIDTH + 1)

/* Tile indices in VRAM. Tile 0 must stay blank: it's what the rest of the
 * map is filled with. */
#define TILE_BLANK 0
#define TILE_BLOCK 1        /* one shape, recoloured per piece by palette bank */
#define TILE_WALL  2
#define TILE_FONT_BASE 16   /* 10 digits then 26 letters */

/* Palette banks. Each NES palette is three colours plus a shared backdrop,
 * and seven pieces' worth doesn't fit the 16 entries of a single 4bpp
 * palette — so each piece gets its own bank and the tilemap entry selects
 * between them (bits 12-15). That's what lets one block tile shape serve
 * every piece: the colours are chosen at draw time, not baked into pixels. */
#define PAL_BANK_FOR_PIECE(piece) ((piece) - 1)  /* pieces 1..7 -> banks 0..6 */
#define PAL_UI_BANK 7
#define PAL_FIELD_BANK 8   /* settled blocks: one level-driven scheme for all */

/* Builds a tilemap entry: tile index in bits 0-9, palette bank in 12-15. */
#define WITH_BANK(tile, bank) ((uint16_t)((tile) | ((bank) << 12)))

static TengenGame g_game;

static void vsync(void) {
    /* Wait out the current vblank, then wait for the next one, so a caller
     * that arrives mid-vblank doesn't fall through both. */
    while (REG_VCOUNT >= 160) { }
    while (REG_VCOUNT < 160) { }
}

/* Reads the keypad and translates it into the core's button bits. The GBA
 * has every button the NES did, so this is a straight 1:1 remap with no
 * compromises — which is the whole reason the controls can be faithful. */
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

static void write_tile_4bpp(int tile_index, const uint8_t pixels[64]) {
    /* 4bpp tiles pack two pixels per byte, low nibble first. */
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK) + tile_index * 16;
    for (int i = 0; i < 32; i += 2) {
        uint16_t lo = (uint16_t)((pixels[i * 2 + 0] & 0xF) | ((pixels[i * 2 + 1] & 0xF) << 4));
        uint16_t hi = (uint16_t)((pixels[i * 2 + 2] & 0xF) | ((pixels[i * 2 + 3] & 0xF) << 4));
        dst[i / 2] = (uint16_t)(lo | (hi << 8));
    }
}

static void build_placeholder_tiles(void) {
    uint8_t pixels[64];

    /* Tile 0: blank. Colour 0 is the shared backdrop in every bank, so this
     * one tile works regardless of which palette bank it's drawn with. */
    for (int i = 0; i < 64; i++) pixels[i] = 0;
    write_tile_4bpp(TILE_BLANK, pixels);

    /* A single block tile serves all seven pieces: the palette bank in the
     * tilemap entry picks the colours, so the shape is shared. Colours 1-3
     * are that piece's three real Tengen colours, arranged as the bevel the
     * NES art uses — light top-left, mid body, dark bottom-right. */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint8_t colour = 2;
            if (x == 0 || y == 0) colour = 1;
            else if (x == 7 || y == 7) colour = 3;
            pixels[y * 8 + x] = colour;
        }
    }
    write_tile_4bpp(TILE_BLOCK, pixels);

    /* Wall tile, drawn in the UI bank whose colour 1 tracks the level. */
    for (int i = 0; i < 64; i++) pixels[i] = 1;
    write_tile_4bpp(TILE_WALL, pixels);

    /* Font tiles: digits then letters, one glyph per tile, so drawing text
     * later is just writing tile indices into the map — no re-uploading
     * pixels when the score changes. */
    for (int glyph = 0; glyph < 36; glyph++) {
        const uint8_t *rows = (glyph < 10) ? kFontDigits[glyph]
                                            : kFontLetters[glyph - 10];
        for (int i = 0; i < 64; i++) pixels[i] = 0;
        for (int y = 0; y < FONT_ROWS; y++) {
            for (int x = 0; x < FONT_COLS; x++) {
                if (rows[y] & (1 << (FONT_COLS - 1 - x))) {
                    pixels[(y + 1) * 8 + (x + 1)] = 3; /* UI bank's text colour */
                }
            }
        }
        write_tile_4bpp(TILE_FONT_BASE + glyph, pixels);
    }
}

static uint16_t font_tile(char c) {
    if (c >= '0' && c <= '9') return WITH_BANK(TILE_FONT_BASE + (c - '0'), PAL_UI_BANK);
    if (c >= 'A' && c <= 'Z') return WITH_BANK(TILE_FONT_BASE + 10 + (c - 'A'), PAL_UI_BANK);
    return TILE_BLANK;
}

static void build_palette(void) {
    vu16 *pal = MEM_PALETTE;

    /* One bank per piece, carrying that piece's three real Tengen colours
     * (palette.h explains the indexing) plus the shared black backdrop. */
    for (int piece = TT_I; piece <= TT_Z; piece++) {
        vu16 *bank = pal + PAL_BANK_FOR_PIECE(piece) * 16;
        bank[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
        for (int i = 0; i < 3; i++) {
            bank[1 + i] = nes_colour_to_gba(kTengenPaletteIndices[piece][i]);
        }
    }

    vu16 *ui = pal + PAL_UI_BANK * 16;
    ui[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    ui[3] = rgb15(31, 31, 31); /* text */
    /* ui[1] (the frame) is set by set_field_palette_for_level below. */
}

/* setPlayfieldPaletteFromLevel (main.asm.txt:5328-5333) recolours the field
 * using the LEVEL'S ONES DIGIT as the palette index — which is why the
 * colours cycle every ten levels rather than running out at level 9. */
static void set_field_palette_for_level(uint8_t level) {
    const uint8_t *entry = kTengenPaletteIndices[level % 10];

    /* The settled blocks. */
    vu16 *field = MEM_PALETTE + PAL_FIELD_BANK * 16;
    field[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    for (int i = 0; i < 3; i++) field[1 + i] = nes_colour_to_gba(entry[i]);

    /* The frame, in the same level scheme but the darkest tone so it reads
     * as a border rather than as more stack. */
    vu16 *ui = MEM_PALETTE + PAL_UI_BANK * 16;
    ui[1] = nes_colour_to_gba(entry[2]);
    ui[2] = nes_colour_to_gba(entry[1]);
}

static void set_map_tile(int tx, int ty, uint16_t tile) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK)[ty * MAP_W + tx] = tile;
}

/* Settled blocks all share the field palette, which tracks the level.
 *
 * That's not a simplification, it's what the ROM does: locked blocks are
 * background tiles coloured by setPlayfieldPaletteFromLevel (a $3F0x
 * background palette), while the falling piece and the next-piece preview
 * are SPRITES coloured by setPiecePalette (a $3F1x sprite palette, set once
 * per piece dealt). So the stack is monochrome-per-level and only the
 * active piece carries its own colour. See reference/NOTES.md. */
static uint16_t tile_for_cell(uint8_t cell) {
    if (cell == TT_NONE) return TILE_BLANK;
    if (cell == TT_WALL) return WITH_BANK(TILE_WALL, PAL_UI_BANK);
    return WITH_BANK(TILE_BLOCK, PAL_FIELD_BANK);
}

/* The active piece and the preview, which do get their own colours. */
static uint16_t tile_for_active_piece(uint8_t piece) {
    return WITH_BANK(TILE_BLOCK, PAL_BANK_FOR_PIECE(piece));
}

static void draw_text(int tx, int ty, const char *text) {
    for (int i = 0; text[i]; i++) set_map_tile(tx + i, ty, font_tile(text[i]));
}

/* Right-aligned, zero-padded, matching how the ROM shows its fixed-width
 * counters (its score really is six digits wide, always). */
static void draw_number(int tx, int ty, uint32_t value, int digits) {
    for (int i = digits - 1; i >= 0; i--) {
        set_map_tile(tx + i, ty, font_tile((char)('0' + (value % 10))));
        value /= 10;
    }
}

static void clear_region(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) set_map_tile(tx + x, ty + y, TILE_BLANK);
    }
}

/* The next-piece preview. Drawn from the same orientation-0 bitmap the game
 * logic uses, so it can't drift out of sync with what actually spawns. */
static void draw_next_piece(int tx, int ty) {
    clear_region(tx, ty, 4, 4);
    TengenTetromino next = g_game.player[0].piece.next;
    if (next <= TT_NONE || next >= TENGEN_TETROMINO_COUNT) return;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (tengen_piece_occupies(next, 0, r, c)) {
                set_map_tile(tx + c, ty + r, tile_for_active_piece((uint8_t)next));
            }
        }
    }
}

static void draw_hud(void) {
    const TengenPlayerState *p = &g_game.player[0];

    draw_text(PANEL_R_TX, 1, "NEXT");
    draw_next_piece(PANEL_R_TX, 3);

    draw_text(PANEL_R_TX, 8, "SCORE");
    draw_number(PANEL_R_TX, 9, p->score, 6);

    draw_text(PANEL_R_TX, 11, "LEVEL");
    draw_number(PANEL_R_TX, 12, p->level, 2);

    draw_text(PANEL_R_TX, 14, "LINES");
    draw_number(PANEL_R_TX, 15, p->lines, 4);

    /* Left panel: per-piece statistics, same information the NES 1P screen
     * shows as a bar chart. The NES had the height for bars; 9 tiles of
     * width here suit an icon-plus-count list better, so this is the one
     * place the HUD deliberately departs from the original's presentation
     * rather than its content. */
    draw_text(PANEL_L_TX + 1, 1, "STATS");
    for (int piece = TT_I; piece <= TT_Z; piece++) {
        int ty = 3 + (piece - TT_I) * 2;
        set_map_tile(PANEL_L_TX + 1, ty, tile_for_active_piece((uint8_t)piece));
        draw_number(PANEL_L_TX + 3, ty, p->piece_stats[piece], 3);
    }

    if (!p->game_active) {
        draw_text(PANEL_R_TX, 17, "GAME");
        draw_text(PANEL_R_TX, 18, "OVER");
    } else {
        clear_region(PANEL_R_TX, 17, 5, 2);
    }
}

static void draw_frame(void) {
    const TengenPlayfield *field = &g_game.field[0];

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
            set_map_tile(FIELD_ORIGIN_TX + col, FIELD_ORIGIN_TY + row,
                          tile_for_cell(field->cell[row][col]));
        }
    }

    /* The falling piece is drawn over the settled field rather than being
     * written into it — it isn't part of the field until it locks. Cells
     * still above the field (negative rows) are skipped, which is what makes
     * a piece visibly slide in from off-screen the way the ROM does. */
    TengenCell cells[4];
    int count = tengen_active_piece_cells(&g_game, TENGEN_PLAYER_1, cells);
    for (int i = 0; i < count; i++) {
        if (cells[i].row < 0) continue;
        set_map_tile(FIELD_ORIGIN_TX + cells[i].col,
                      FIELD_ORIGIN_TY + cells[i].row,
                      tile_for_active_piece((uint8_t)g_game.player[0].piece.current));
    }

    draw_hud();
}

/* Level select range, verified: computerMoveSelectTable (main.asm.txt:4819)
 * holds the wrap limit for each menu row, and row 1 — menuPlayer1StartLevel —
 * is 10, so start levels run 0..9 and wrap at both ends (main.asm.txt:4742-4763). */
#define START_LEVEL_COUNT 10

typedef enum { SCREEN_TITLE, SCREEN_PLAYING } Screen;

static void clear_screen(void) {
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < MAP_W; tx++) set_map_tile(tx, ty, TILE_BLANK);
    }
}

static void draw_title(uint8_t start_level) {
    clear_screen();
    draw_text(9, 4, "TENGEN");
    draw_text(9, 6, "TETRIS");

    draw_text(7, 10, "LEVEL");
    draw_number(14, 10, start_level, 1);
    draw_text(5, 12, "UP DOWN TO SET");
    draw_text(6, 15, "START TO PLAY");
}

int main(void) {
    build_palette();
    build_placeholder_tiles();
    set_field_palette_for_level(0);

    clear_screen();

    REG_BG0CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK);
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;

    Screen screen = SCREEN_TITLE;
    uint8_t start_level = 0;
    uint8_t shown_level = 0xFF;
    uint8_t held_last = 0;

    /* The ROM steps its RNG once per frame from the main loop
     * (main.asm.txt:49-50) and whatever state it happens to be in when you
     * press Start becomes the game's seed. Doing the same here means the
     * sequence you get genuinely depends on when you start, rather than
     * every session dealing identical pieces. */
    TengenRng seed_source;
    tengen_rng_seed(&seed_source, 0xACE1);

    for (;;) {
        uint8_t buttons = read_buttons();
        uint8_t pressed = (uint8_t)(buttons & ~held_last);
        held_last = buttons;
        tengen_rng_step(&seed_source);

        if (screen == SCREEN_TITLE) {
            if (pressed & TENGEN_BTN_UP) {
                start_level = (uint8_t)((start_level + START_LEVEL_COUNT - 1) % START_LEVEL_COUNT);
            }
            if (pressed & TENGEN_BTN_DOWN) {
                start_level = (uint8_t)((start_level + 1) % START_LEVEL_COUNT);
            }
            if (pressed & TENGEN_BTN_START) {
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                tengen_new_game(&g_game, seed, start_level, false, false);
                shown_level = 0xFF;
                screen = SCREEN_PLAYING;
                vsync();
                /* Wipe the title before the HUD takes over: draw_frame only
                 * repaints the field and the panels' own cells, so anything
                 * left elsewhere would show through. */
                clear_screen();
                continue;
            }
            vsync();
            draw_title(start_level);
            continue;
        }

        tengen_step(&g_game, TENGEN_PLAYER_1, buttons);

        if (!g_game.player[0].game_active && (pressed & TENGEN_BTN_START)) {
            screen = SCREEN_TITLE;
            vsync();
            continue;
        }

        if (g_game.player[0].level != shown_level) {
            shown_level = g_game.player[0].level;
            set_field_palette_for_level(shown_level);
        }

        vsync();
        draw_frame();
    }
}
