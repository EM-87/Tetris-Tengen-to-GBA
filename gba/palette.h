/*
 * palette.h — Tengen Tetris's actual colours, converted from NES to GBA.
 *
 * Unlike the block shapes and the font (which are placeholders standing in
 * for CHR art this repo doesn't have), these colours ARE the game's own:
 * the NES palette indices come straight out of the disassembly's
 * piecePaletteIndex0..B table (main.asm.txt:5364-5399), verified along with
 * how the game selects between them:
 *
 *   - Each entry is three colours; the fourth is the shared backdrop.
 *   - setPiecePalette (main.asm.txt:5338) indexes the table by PIECE ID, so
 *     entry 1 is the I piece, 2 is T, 3 is O, 4 is J, 5 is L, 6 is S, 7 is Z.
 *   - setPlayfieldPaletteFromLevel (main.asm.txt:5328) indexes the SAME table
 *     by the level's ones digit, which is why the field recolours every level
 *     and repeats every ten.
 *
 * The one thing that can't come from the ROM is what those NES colour indices
 * actually look like: the master palette lives in the PPU hardware, not in
 * the cartridge, and published measurements of it genuinely disagree with
 * each other (and with individual consoles). The table below is a
 * widely-used approximation, not an exact reproduction of any particular
 * console's output.
 */
#ifndef PALETTE_H
#define PALETTE_H

#include <stdint.h>
#include "gba_hw.h"

/* NES PPU colour index -> 8-bit RGB. Commonly-cited approximation; see the
 * note above about why no single table is authoritative. Only the entries
 * the game actually uses need to be right, but the full 64 are here so the
 * CHR import path (see ../tools/) can convert anything. */
static const uint8_t kNesPaletteRGB[64][3] = {
    {84,84,84},   {0,30,116},   {8,16,144},   {48,0,136},
    {68,0,100},   {92,0,48},    {84,4,0},     {60,24,0},
    {32,42,0},    {8,58,0},     {0,64,0},     {0,60,0},
    {0,50,60},    {0,0,0},      {0,0,0},      {0,0,0},
    {152,150,152},{8,76,196},   {48,50,236},  {92,30,228},
    {136,20,176}, {160,20,100}, {152,34,32},  {120,60,0},
    {84,90,0},    {40,114,0},   {8,124,0},    {0,118,40},
    {0,102,120},  {0,0,0},      {0,0,0},      {0,0,0},
    {236,238,236},{76,154,236}, {120,124,236},{176,98,236},
    {228,84,236}, {236,88,180}, {236,106,100},{212,136,32},
    {160,170,0},  {116,196,0},  {76,208,32},  {56,204,108},
    {56,180,204}, {60,60,60},   {0,0,0},      {0,0,0},
    {236,238,236},{168,204,236},{188,188,236},{212,178,236},
    {236,174,236},{236,174,212},{236,180,176},{228,196,144},
    {204,210,120},{180,222,120},{168,226,144},{152,226,180},
    {160,214,228},{160,162,160},{0,0,0},      {0,0,0},
};

/* piecePaletteIndex0..B (main.asm.txt:5364-5399) — the piece and level
 * palettes — are no longer copied here: the port takes every palette set
 * straight off the cartridge's own tables, in gba/palettes_rom.h, which
 * tools/extract_assets.py generates. Twelve entries, indexed by piece id for
 * pieces and by the level's ones digit for the field; entry 10 is what the
 * game flashes during a line clear, entry 11 is the bonus animation. */
#define TENGEN_PALETTE_ENTRIES 12

/* The NES backdrop these palettes share ($0F is black). */
#define TENGEN_BACKDROP_INDEX 0x0F

/* THE SCREEN'S CURVE, and today it is a straight line. Games made for the
 * GBA drew their colours brighter than a television needs, because the
 * original GBA and the front-lit SP (AGS-001) flatten the dark half of every
 * channel; emulators go the other way and DARKEN to imitate those panels
 * (an LCD gamma of about 4 against a monitor's 2.2). The NES palette was
 * made for a television, and on the backlit SP (AGS-101) a brightening
 * curve (x^(1/1.6), build 822ac6c) washed everything out, so the table is
 * the identity: every colour as the cartridge names it. It stays the one
 * place to put a curve back, should a front-lit screen want one. */
static const uint8_t kLcdGamma[32] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

static inline uint16_t lcd_colour(uint16_t c) {
    return rgb15(kLcdGamma[c & 31], kLcdGamma[(c >> 5) & 31],
                 kLcdGamma[(c >> 10) & 31]);
}

static inline uint16_t nes_colour_to_gba(uint8_t nes_index) {
    const uint8_t *rgb = kNesPaletteRGB[nes_index & 0x3F];
    /* 8-bit per channel down to the GBA's 5, and through the screen's
     * curve. */
    return lcd_colour(rgb15(rgb[0] >> 3, rgb[1] >> 3, rgb[2] >> 3));
}

#endif /* PALETTE_H */
