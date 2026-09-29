/* splash.c — THE PUBLISHER'S LOGO ON WHITE, as a GBA game opens: out of
 * white, held, and away to black, before the title. At power-on only — a
 * soft reset (A+B+START+SELECT) goes straight back to the title — and not
 * on the Single-Pak copy, which has just come through the BIOS's own logo
 * and goes straight to its lobby.
 *
 * Mode 4, the one 8-bit bitmap, drawn once: the logo (tools/make_splash.py,
 * sixteen colours with white as colour 0) unpacked into the middle of the
 * page and the rest left at colour 0. The fades are the blender's
 * brightness, over the bitmap and the backdrop both. Any button cuts the
 * hold short once the logo is up. Afterwards the video memory the bitmap
 * used is cleared and the registers left as they were found, so main lays
 * out the game's screens exactly as it did without it.
 *
 * The logo is not the port's own art, so its header is generated and not
 * committed; a build without it has no splash. */
#include "port.h"

#if __has_include("splash_logo.h") && !defined(TENGEN_MULTIBOOT)
#include "splash_logo.h"

#define REG_BLDCNT (*(vu16 *)0x04000050)
#define REG_BLDY   (*(vu16 *)0x04000054)
#define BLD_BG2_BACKDROP 0x0024      /* first target: BG2 and the backdrop */
#define BLD_BRIGHTEN     0x0080
#define BLD_DARKEN       0x00C0
#define DCNT_MODE4_BG2   (0x0004 | DCNT_BG2)

#define FADE_FRAMES 24
#define HOLD_FRAMES 120
#define HOLD_SKIPPABLE 30

/* Survives the soft reset (crt0 clears .bss, not this) and not a power
 * cycle: RAM switched off comes back as whatever it comes back as, and
 * this word is vanishingly unlikely to be it. */
#define SPLASH_SEEN 0x54454E47u      /* "TENG" */
__attribute__((section(".ewram"))) static volatile uint32_t g_splash_seen;

static void fade(uint16_t mode, int from, int to) {
    REG_BLDCNT = (uint16_t)(BLD_BG2_BACKDROP | mode);
    for (int f = 0; f <= FADE_FRAMES; f++) {
        int y = from + (to - from) * f / FADE_FRAMES;
        vsync();
        REG_BLDY = (uint16_t)y;
    }
}

void splash_show(void) {
    if (g_splash_seen == SPLASH_SEEN) return;
    g_splash_seen = SPLASH_SEEN;

    REG_DISPCNT = DCNT_FORCED_BLANK;
    for (int i = 0; i < 16; i++) MEM_PALETTE[i] = kSplashPalette[i];
    vu16 *page = (vu16 *)0x06000000;
    for (int i = 0; i < 240 * 160 / 2; i++) page[i] = 0;
    const int x0 = ((240 - SPLASH_W) / 2) & ~1;
    const int y0 = (160 - SPLASH_H) / 2;
    for (int y = 0; y < SPLASH_H; y++) {
        const uint8_t *row = kSplashPixels + y * (SPLASH_W / 2);
        vu16 *dst = page + ((y0 + y) * 240 + x0) / 2;
        for (int x = 0; x < SPLASH_W / 2; x++)
            dst[x] = (uint16_t)((row[x] & 0x0F) | ((row[x] >> 4) << 8));
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
    fade(BLD_DARKEN, 0, 16);

    /* Put everything back: nothing below expects a bitmap, a palette of
     * the logo's, or the blender on. And the button that cut it short is
     * the title's no more than the splash's. */
    REG_DISPCNT = DCNT_FORCED_BLANK;
    for (vu32 *p = (vu32 *)0x06000000; p < (vu32 *)0x06018000; p++) *p = 0;
    for (int i = 0; i < 16; i++) MEM_PALETTE[i] = 0;
    REG_BLDCNT = 0;
    REG_BLDY = 0;
    while (read_buttons()) vsync();
}
#else
void splash_show(void) {}
#endif
