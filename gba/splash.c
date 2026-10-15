/* splash.c — THE LOGOS ON WHITE, as a GBA game opens: the publisher's, then
 * the licensors' — Mirrorsoft's, whose licence the cartridge itself names on
 * GAME SELECT (ppuPatchLicensedBy, $A1EF), and the Tetris logo, as every
 * Tetris since has opened with — each out of white and held, all but the
 * last back into white and the last away to black, before the title. At
 * power-on only — a soft reset
 * (A+B+START+SELECT) goes straight back to the title — and not on the
 * Single-Pak copy, which has just come through the BIOS's own logo and goes
 * straight to its lobby.
 *
 * Mode 4, the one 8-bit bitmap, drawn once a logo: the picture
 * (tools/make_splash.py, white as colour 0 — sixteen colours for the
 * publisher's, up to 256 for the licensors' gradients) put into the
 * middle of the page and the rest left at colour 0. The fades are the
 * blender's brightness, over the bitmap and the backdrop both. Any button
 * cuts a hold short once the logo is up. Afterwards the video memory the
 * bitmap used is cleared and the registers left as they were found, so main
 * lays out the game's screens exactly as it did without it.
 *
 * The logos are not the port's own art, so their headers are generated and
 * not committed; a build without one simply goes without that splash. */
#include "port.h"

#if !defined(TENGEN_MULTIBOOT) && __has_include("splash_logo.h")
#include "splash_logo.h"
#define HAVE_TENGEN_LOGO 1
#endif
#if !defined(TENGEN_MULTIBOOT) && __has_include("splash_mirrorsoft.h")
#include "splash_mirrorsoft.h"
#define HAVE_MIRRORSOFT_LOGO 1
#endif
#if !defined(TENGEN_MULTIBOOT) && __has_include("splash_tetris.h")
#include "splash_tetris.h"
#define HAVE_TETRIS_LOGO 1
#endif

#if defined(HAVE_TENGEN_LOGO) || defined(HAVE_MIRRORSOFT_LOGO) || \
    defined(HAVE_TETRIS_LOGO)
#define REG_BLDCNT (*(vu16 *)0x04000050)
#define REG_BLDY   (*(vu16 *)0x04000054)
#define BLD_BG2_BACKDROP 0x0024      /* first target: BG2 and the backdrop */
#define BLD_BRIGHTEN     0x0080
#define BLD_DARKEN       0x00C0
#define DCNT_MODE4_BG2   (0x0004 | DCNT_BG2)

#define FADE_FRAMES 24
#define HOLD_FRAMES 120
#define HOLD_SKIPPABLE 30

static void fade(uint16_t mode, int from, int to) {
    REG_BLDCNT = (uint16_t)(BLD_BG2_BACKDROP | mode);
    for (int f = 0; f <= FADE_FRAMES; f++) {
        int y = from + (to - from) * f / FADE_FRAMES;
        vsync();
        REG_BLDY = (uint16_t)y;
    }
}

/* One logo: `pixels` a byte each, or two to a byte (`nibbles`, the left
 * one low) for a sixteen-colour one; `to_white` when another follows it. */
static void show_logo(const uint16_t *palette, int colours,
                      const uint8_t *pixels, int w, int h, bool nibbles,
                      bool to_white) {
    REG_DISPCNT = DCNT_FORCED_BLANK;
    for (int i = 0; i < colours; i++) MEM_PALETTE[i] = lcd_colour(palette[i]);
    vu16 *page = (vu16 *)0x06000000;
    for (int i = 0; i < 240 * 160 / 2; i++) page[i] = 0;
    const int x0 = ((240 - w) / 2) & ~1;
    const int y0 = (160 - h) / 2;
    for (int y = 0; y < h; y++) {
        vu16 *dst = page + ((y0 + y) * 240 + x0) / 2;
        if (nibbles) {
            const uint8_t *row = pixels + y * (w / 2);
            for (int x = 0; x < w / 2; x++)
                dst[x] = (uint16_t)((row[x] & 0x0F) | ((row[x] >> 4) << 8));
        } else {
            const uint8_t *row = pixels + y * w;
            for (int x = 0; x < w / 2; x++)
                dst[x] = (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8));
        }
    }

    /* From white — which the page already is, bar the logo. */
    REG_BLDCNT = (uint16_t)(BLD_BG2_BACKDROP | BLD_BRIGHTEN);
    REG_BLDY = 16;
    vsync();
    REG_DISPCNT = DCNT_MODE4_BG2;
    fade(BLD_BRIGHTEN, 16, 0);
    for (int f = 0; f < HOLD_FRAMES; f++) {
        vsync();
        if (f >= HOLD_SKIPPABLE && read_buttons()) break;
    }
    fade(to_white ? BLD_BRIGHTEN : BLD_DARKEN, 0, 16);

    /* Put everything back: nothing below expects a bitmap, a palette of
     * the logo's, or the blender on. And the button that cut it short is
     * the next logo's or the title's no more than this one's. */
    REG_DISPCNT = DCNT_FORCED_BLANK;
    for (vu32 *p = (vu32 *)0x06000000; p < (vu32 *)0x06018000; p++) *p = 0;
    for (int i = 0; i < colours; i++) MEM_PALETTE[i] = 0;
    REG_BLDCNT = 0;
    REG_BLDY = 0;
    while (read_buttons()) vsync();
}
#endif

/* THE PUBLISHER'S, and back into white if a licensor's comes after it. */
static void tengen_logo(void) {
#ifdef HAVE_TENGEN_LOGO
#if defined(HAVE_MIRRORSOFT_LOGO) || defined(HAVE_TETRIS_LOGO)
    const bool more = true;
#else
    const bool more = false;
#endif
    show_logo(kSplashPalette, 16, kSplashPixels, SPLASH_W, SPLASH_H, true, more);
#endif
}

/* MIRRORSOFT'S: the cartridge's licence, "LICENSED BY MIRRORSOFT LTD." on
 * its GAME SELECT, as the licensor's own logo; back into white if the
 * Tetris logo follows. */
static void mirrorsoft_logo(void) {
#ifdef HAVE_MIRRORSOFT_LOGO
#ifdef HAVE_TETRIS_LOGO
    const bool more = true;
#else
    const bool more = false;
#endif
    show_logo(kMirrorsoftLogoPalette, MIRRORSOFT_LOGO_COLOURS,
              kMirrorsoftLogoPixels, MIRRORSOFT_LOGO_W, MIRRORSOFT_LOGO_H,
              false, more);
#endif
}

/* THE TETRIS LOGO, as every Tetris since the licence has opened with: the
 * last, and to black for the title. */
static void tetris_logo(void) {
#ifdef HAVE_TETRIS_LOGO
    show_logo(kTetrisLogoPalette, TETRIS_LOGO_COLOURS, kTetrisLogoPixels,
              TETRIS_LOGO_W, TETRIS_LOGO_H, false, false);
#endif
}

/* Survives the soft reset (crt0 clears .bss, not this) and not a power
 * cycle: RAM switched off comes back as whatever it comes back as, and
 * this word is vanishingly unlikely to be it. */
#define SPLASH_SEEN 0x54454E47u      /* "TENG" */
__attribute__((section(".ewram"))) static volatile uint32_t g_splash_seen;

/* THE LOGOS AT POWER-ON: the Game Boy Player's first — which is how a
 * Player is found, gbp.c — then the publisher's, Mirrorsoft's and Tetris's,
 * each out of white and all but the last back into it. None after a soft
 * reset, nor on the Single-Pak copy. */
void splash_show(void) {
#ifdef TENGEN_MULTIBOOT
    return;
#endif
    if (g_splash_seen == SPLASH_SEEN) return;
    g_splash_seen = SPLASH_SEEN;
    gbp_show_logo();
    tengen_logo();
    mirrorsoft_logo();
    tetris_logo();
}
