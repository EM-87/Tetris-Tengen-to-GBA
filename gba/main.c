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
#include "../src/tengen_core.h"

#define CHARBLOCK   0
#define SCREENBLOCK 28  /* 28 * 2KB = 56KB in, clear of the tile data below */

#define MAP_W 32        /* a 32x32 tile background */

/* Where the playfield's top-left tile sits on screen, in 8x8 tiles. */
#define FIELD_ORIGIN_TX ((30 - TENGEN_PF_WIDTH) / 2)  /* centred: 9 */
#define FIELD_ORIGIN_TY 0                              /* full height, no margin */

/* Tile indices in VRAM. Tile 0 must stay blank: it's what the rest of the
 * map is filled with. */
#define TILE_BLANK 0
#define TILE_BLOCK_BASE 1   /* one tile per piece id, 1..7 */
#define TILE_WALL  9

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

    /* Tile 0: blank. */
    for (int i = 0; i < 64; i++) pixels[i] = 0;
    write_tile_4bpp(TILE_BLANK, pixels);

    /* Tiles 1..7: one solid block per piece id, with a one-pixel highlight
     * on the top/left edge so adjacent blocks stay distinguishable. The real
     * art has fifteen variants per the core's kTileIds table; this is
     * standing in for all of them. */
    for (int piece = 1; piece <= 7; piece++) {
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint8_t colour = (uint8_t)piece;
                if (x == 0 || y == 0) colour = 8;       /* highlight */
                else if (x == 7 || y == 7) colour = 9;  /* shadow */
                pixels[y * 8 + x] = colour;
            }
        }
        write_tile_4bpp(TILE_BLOCK_BASE + piece - 1, pixels);
    }

    /* Wall tile: a flat frame colour. */
    for (int i = 0; i < 64; i++) pixels[i] = 10;
    write_tile_4bpp(TILE_WALL, pixels);
}

static void build_palette(void) {
    vu16 *pal = MEM_PALETTE;
    pal[0]  = rgb15(0, 0, 0);      /* backdrop */
    pal[1]  = rgb15(0, 28, 28);    /* I - cyan */
    pal[2]  = rgb15(24, 0, 28);    /* T - purple */
    pal[3]  = rgb15(28, 28, 0);    /* O - yellow */
    pal[4]  = rgb15(0, 8, 28);     /* J - blue */
    pal[5]  = rgb15(28, 14, 0);    /* L - orange */
    pal[6]  = rgb15(0, 28, 6);     /* S - green */
    pal[7]  = rgb15(28, 0, 4);     /* Z - red */
    pal[8]  = rgb15(31, 31, 31);   /* block highlight */
    pal[9]  = rgb15(8, 8, 10);     /* block shadow */
    pal[10] = rgb15(14, 14, 18);   /* wall */
}

static void set_map_tile(int tx, int ty, uint16_t tile) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK)[ty * MAP_W + tx] = tile;
}

static uint16_t tile_for_cell(uint8_t cell) {
    if (cell == TT_NONE) return TILE_BLANK;
    if (cell == TT_WALL) return TILE_WALL;
    return (uint16_t)(TILE_BLOCK_BASE + cell - 1);
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
                      tile_for_cell((uint8_t)g_game.player[0].piece.current));
    }
}

int main(void) {
    build_palette();
    build_placeholder_tiles();

    /* Clear the map before showing it, or the first frame displays whatever
     * VRAM powered up with. */
    for (int ty = 0; ty < 32; ty++) {
        for (int tx = 0; tx < MAP_W; tx++) set_map_tile(tx, ty, TILE_BLANK);
    }

    REG_BG0CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK);
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0;

    /* The seed is arbitrary for now. The ROM seeds from a frame counter that
     * advances while the title screen waits for input, so the sequence you
     * get depends on when you press Start — worth reproducing once there's a
     * title screen to press Start on. */
    tengen_new_game(&g_game, 0xACE1, 0, false, false);

    for (;;) {
        uint8_t buttons = read_buttons();
        tengen_step(&g_game, TENGEN_PLAYER_1, buttons);

        if (!g_game.player[0].game_active && (buttons & TENGEN_BTN_START)) {
            tengen_new_game(&g_game, 0xACE1, 0, false, false);
        }

        vsync();
        draw_frame();
    }
}
