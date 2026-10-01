/* gbp.c — THE GAME BOY PLAYER: its logo at power-on, which is how a game
 * and the Player find each other, and its rumble.
 *
 * GBATEK ("GBA Gameboy Player"): a game shows the Player's 240x160 logo for
 * a few frames, and while it is up the Player answers on the pad — 03FFh
 * for two frames, 030Fh (left, right, up and down all held, which a pad
 * cannot do) for one. A game that sees 030Fh is on a Player. From then on
 * the Player clocks the serial port in 32-bit normal mode about once a
 * frame and the game answers each word (tengen_gbp_reply): a handshake that
 * spells NINTENDO, and then 400000yyh, the rumble on or off.
 *
 * Every GBA that boots the cartridge shows the logo, as the games that
 * supported the Player did; only a Player answers it. The logo is
 * Nintendo's art (tools/make_gbp_logo.py, from a picture you supply,
 * gitignored); a build without it shows none and never looks. Not on the
 * Single-Pak copy, which came over a cable and does not draw the boot
 * screens at all. The handshake is GBATEK's table, copied exactly, and has
 * run only against a stand-in in the checks (run_rom --gbp), not on a
 * Player (INFERRED until it has). */
#include "port.h"

#define REG_SIOCNT_GBP    (*(vu16 *)0x04000128)
#define REG_SIODATA32_GBP (*(vu32 *)0x04000120)
#define REG_RCNT_GBP      (*(vu16 *)0x04000134)
#define SIO_NORMAL32_EXT_SOHIGH 0x1008   /* 32-bit, external clock, SO high */
#define SIO_GBP_IRQ   0x4000
#define SIO_GBP_START 0x0080

/* What the boot found, kept where crt0 does not clear it so that a soft
 * reset — which skips the logos — still knows (see splash.c's word). */
#define GBP_SEEN 0x47425021u              /* "GBP!" */
__attribute__((section(".ewram"))) static volatile uint32_t g_gbp_word;

static volatile bool g_gbp_serial;        /* the port is the Player's */
static volatile uint16_t g_gbp_rumble;    /* frames of rumble still to go */

bool gbp_present(void) {
    return g_gbp_word == GBP_SEEN;
}

#if __has_include("gbp_logo.h") && !defined(TENGEN_MULTIBOOT)
#include "gbp_logo.h"

#define REG_BLDCNT_GBP (*(vu16 *)0x04000050)
#define REG_BLDY_GBP   (*(vu16 *)0x04000054)
#define GBP_FADE_FRAMES 16
#define GBP_HOLD_FRAMES 60
#define GBP_SKIPPABLE   30

/* One frame of the logo: wait for it, and look at the pad the Player
 * writes. 030Fh exactly — all four directions and nothing else. */
static bool gbp_frame_seen(void) {
    vsync();
    return (REG_KEYINPUT & 0x03FF) == 0x030F;
}

/* Out of white, held, back to white: the Tengen logo comes in from white
 * after it (splash.c). True if a Player answered. */
bool gbp_show_logo(void) {
    REG_DISPCNT = DCNT_FORCED_BLANK;
    /* The colours exactly, NOT through the screen's curve (kLcdGamma): the
     * Player is looking for this picture. */
    for (int i = 0; i < GBP_LOGO_COLOURS; i++) MEM_PALETTE[i] = kGbpLogoPalette[i];
    vu16 *page = (vu16 *)0x06000000;
    for (int i = 0; i < 240 * 160 / 2; i++) page[i] = 0;
    for (int y = 0; y < GBP_LOGO_H; y++) {
        const uint8_t *row = kGbpLogoPixels + y * GBP_LOGO_W;
        vu16 *dst = page + ((GBP_LOGO_Y + y) * 240 + GBP_LOGO_X) / 2;
        for (int x = 0; x < GBP_LOGO_W / 2; x++)
            dst[x] = (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8));
    }

    bool seen = false;
    REG_BLDCNT_GBP = 0x0024 | 0x0080;     /* BG2 and backdrop, brighten */
    REG_BLDY_GBP = 16;
    vsync();
    REG_DISPCNT = (uint16_t)(0x0004 | DCNT_BG2);
    for (int f = 0; f <= GBP_FADE_FRAMES; f++) {
        seen |= gbp_frame_seen();
        REG_BLDY_GBP = (uint16_t)(16 - f * 16 / GBP_FADE_FRAMES);
    }
    for (int f = 0; f < GBP_HOLD_FRAMES; f++) {
        seen |= gbp_frame_seen();
        /* A, B or START cuts it short once the Player has had its frames;
         * the directions are the Player's to press. */
        if (f >= GBP_SKIPPABLE && (~REG_KEYINPUT & (KEY_A | KEY_B | KEY_START)))
            break;
    }
    for (int f = 0; f <= GBP_FADE_FRAMES; f++) {
        seen |= gbp_frame_seen();
        REG_BLDY_GBP = (uint16_t)(f * 16 / GBP_FADE_FRAMES);
    }
    /* Left white and blank for the next logo; the page is emptied by it. */
    REG_DISPCNT = DCNT_FORCED_BLANK;
    REG_BLDCNT_GBP = 0;
    REG_BLDY_GBP = 0;
    g_gbp_word = seen ? GBP_SEEN : 0;
    return seen;
}
#else
bool gbp_show_logo(void) {
    g_gbp_word = 0;
    return false;
}
#endif

/* THE PORT IS THE PLAYER'S: 32-bit normal mode on its clock, the first
 * answer loaded and the start bit set, the interrupt on. Called at boot
 * once a Player has answered, and again whenever the cable gives the port
 * back (link_shutdown). */
void gbp_start(void) {
    if (!gbp_present()) return;
    REG_IME = 0;
    REG_RCNT_GBP = 0;
    REG_SIOCNT_GBP = SIO_NORMAL32_EXT_SOHIGH;
    REG_SIODATA32_GBP = 0;
    REG_IE |= IRQ_SERIAL;
    REG_IF = IRQ_SERIAL;
    REG_SIOCNT_GBP = SIO_NORMAL32_EXT_SOHIGH | SIO_GBP_IRQ | SIO_GBP_START;
    g_gbp_serial = true;
    REG_IME = IME_ON;
}

/* ...and the cable takes it (link_init). */
void gbp_release(void) {
    g_gbp_serial = false;
}

bool gbp_owns_serial(void) {
    return g_gbp_serial;
}

/* From the interrupt: the word that came, the answer for the next. */
IWRAM_CODE void gbp_serial_service(void);
void gbp_serial_service(void) {
    uint32_t got = REG_SIODATA32_GBP;
    REG_SIODATA32_GBP = tengen_gbp_reply(got, g_gbp_rumble != 0);
    REG_SIOCNT_GBP = (uint16_t)(REG_SIOCNT_GBP | SIO_GBP_START);
}

/* Rumble for this many frames (or for longer, if some is already going). */
void gbp_rumble(int frames) {
    if (!g_gbp_serial || frames <= 0) return;
    if (frames > g_gbp_rumble) g_gbp_rumble = (uint16_t)frames;
}

/* Once a frame; and none at all while the game is not being played. */
void gbp_tick(void) {
    if (g_gbp_rumble) g_gbp_rumble--;
}

void gbp_rumble_stop(void) {
    g_gbp_rumble = 0;
}
