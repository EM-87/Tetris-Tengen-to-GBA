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

/* piecePaletteIndex0..B, verbatim (main.asm.txt:5364-5399). Indexed by piece
 * id for pieces and by the level's ones digit for the field; entry 10 is what
 * the game flashes during a line clear, entry 11 is the bonus animation. */
#define TENGEN_PALETTE_ENTRIES 12
static const uint8_t kTengenPaletteIndices[TENGEN_PALETTE_ENTRIES][3] = {
    {0x20, 0x10, 0x00}, /*  0  level 0            */
    {0x26, 0x16, 0x06}, /*  1  level 1 & I        */
    {0x27, 0x18, 0x08}, /*  2  level 2 & T        */
    {0x21, 0x12, 0x01}, /*  3  level 3 & O        */
    {0x37, 0x27, 0x17}, /*  4  level 4 & J        */
    {0x34, 0x24, 0x14}, /*  5  level 5 & L        */
    {0x2A, 0x1A, 0x0A}, /*  6  level 6 & S        */
    {0x2C, 0x1C, 0x0C}, /*  7  level 7 & Z        */
    {0x23, 0x13, 0x03}, /*  8  level 8            */
    {0x2B, 0x1B, 0x0B}, /*  9  level 9            */
    {0x0F, 0x0F, 0x0F}, /* 10  line clear (black) */
    {0x30, 0x16, 0x0F}, /* 11  bonus animation    */
};

/* The NES backdrop these palettes share ($0F is black). */
#define TENGEN_BACKDROP_INDEX 0x0F

static inline uint16_t nes_colour_to_gba(uint8_t nes_index) {
    const uint8_t *rgb = kNesPaletteRGB[nes_index & 0x3F];
    /* 8-bit per channel down to the GBA's 5. */
    return rgb15(rgb[0] >> 3, rgb[1] >> 3, rgb[2] >> 3);
}

#endif /* PALETTE_H */
