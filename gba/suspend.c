/* suspend.c — A PAUSED SOLO GAME OUTLIVES THE POWER SWITCH, as Tetris DX's
 * does: switch off on the pause plaque and the next start comes back to the
 * same game, still paused.
 *
 * What is kept is everything a match starts from and everything it has
 * become: the menu's choices (mode, level, tune, handicaps, the skin, the
 * chord), the whole core game (g_session.game — board, pieces, score, the
 * random number generator) and the computer's state. Everything else — the
 * HUD's bookkeeping, the sprites, the sound engine — is what starting a
 * match lays out anyway, so main starts the match as the menu would and
 * then puts this back over it (suspend_apply).
 *
 * In the battery's save memory above the high scores (SUSPEND_OFF), with a
 * checksum, and tagged with the build: a record from another build is a
 * record of structures this one may lay out differently, so it is not
 * trusted. Written when the plaque goes up (and again if the tune is
 * changed under it), wiped when the game goes on, ends or is left. The
 * Single-Pak copy has no save memory of its own (sram_write does nothing
 * there), so it never finds one. */
#include "port.h"

#define SUSPEND_OFF   0x4000
static const char kSuspendMagic[4] = { 'S', 'U', 'S', 'P' };

/* __DATE__ and __TIME__ of this file's compilation, hashed: a new build
 * tells an old record apart. */
static uint32_t build_tag(void) {
    const char *s = __DATE__ " " __TIME__;
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h ^ (uint32_t)(sizeof(TengenGame) << 16) ^ (uint32_t)sizeof(TengenAi);
}

typedef struct {
    SuspendMenu menu;
    TengenGame game;
    TengenAi ai;
} SuspendBody;

static SuspendBody g_kept;

static uint32_t checksum(const uint8_t *p, unsigned n) {
    uint32_t sum = 0x5A5A5A5Au;
    for (unsigned i = 0; i < n; i++) sum = (sum << 5 | sum >> 27) ^ p[i];
    return sum;
}

static void put32(unsigned at, uint32_t v) {
    for (int b = 0; b < 4; b++) sram_write(at + (unsigned)b, (uint8_t)(v >> (8 * b)));
}

static uint32_t get32(unsigned at) {
    uint32_t v = 0;
    for (int b = 0; b < 4; b++) v |= (uint32_t)sram_read(at + (unsigned)b) << (8 * b);
    return v;
}

/* The layout: magic, build tag, checksum, then the body. The magic goes in
 * LAST, so a console switched off halfway through writing leaves no magic
 * and so no record, rather than half of one. */
#define AT_TAG  (SUSPEND_OFF + 4)
#define AT_SUM  (SUSPEND_OFF + 8)
#define AT_BODY (SUSPEND_OFF + 12)

void suspend_save(const SuspendMenu *menu) {
    g_kept.menu = *menu;
    g_kept.game = g_session.game;
    g_kept.ai = g_ai;
    const uint8_t *body = (const uint8_t *)&g_kept;
    sram_write(SUSPEND_OFF, 0);
    for (unsigned i = 0; i < sizeof g_kept; i++) sram_write(AT_BODY + i, body[i]);
    put32(AT_TAG, build_tag());
    put32(AT_SUM, checksum(body, sizeof g_kept));
    for (int i = 3; i >= 0; i--) sram_write(SUSPEND_OFF + (unsigned)i, (uint8_t)kSuspendMagic[i]);
}

void suspend_clear(void) {
    if (sram_read(SUSPEND_OFF) == (uint8_t)kSuspendMagic[0])
        sram_write(SUSPEND_OFF, 0);
}

bool suspend_load(SuspendMenu *menu) {
    for (int i = 0; i < 4; i++)
        if (sram_read(SUSPEND_OFF + (unsigned)i) != (uint8_t)kSuspendMagic[i]) return false;
    if (get32(AT_TAG) != build_tag()) return false;
    uint8_t *body = (uint8_t *)&g_kept;
    for (unsigned i = 0; i < sizeof g_kept; i++) body[i] = sram_read(AT_BODY + i);
    if (get32(AT_SUM) != checksum(body, sizeof g_kept)) return false;
    /* A record of a game that is not a paused, running solo game is not one
     * this file wrote. */
    if (!g_kept.game.paused || GAME_IS_LINKED(g_kept.menu.game_mode)) return false;
    *menu = g_kept.menu;
    return true;
}

void suspend_apply(void) {
    g_session.game = g_kept.game;
    g_ai = g_kept.ai;
}
