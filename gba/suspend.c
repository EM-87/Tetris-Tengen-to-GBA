/* suspend.c — A PAUSED SOLO GAME OUTLIVES THE POWER SWITCH, as Tetris DX's
 * does: switch off on the pause plaque and the next start comes back to the
 * same game, still paused.
 *
 * What is kept is everything a match starts from and everything it has
 * become: the menu's choices (mode, level, tune, handicaps, the skin, the
 * chord), the whole core game (g_session.game — board, pieces, score, the
 * random number generator), the computer's state and the game's own
 * figures so far (g_stats). Everything else — the
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
    TengenStats stats[2];     /* the game's own figures so far (g_stats) */
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
#define REC_TAG  4
#define REC_SUM  8
#define REC_BODY 12

static void record_write(unsigned at, const char *magic) {
    const uint8_t *body = (const uint8_t *)&g_kept;
    sram_write(at, 0);
    for (unsigned i = 0; i < sizeof g_kept; i++) sram_write(at + REC_BODY + i, body[i]);
    put32(at + REC_TAG, build_tag());
    put32(at + REC_SUM, checksum(body, sizeof g_kept));
    for (int i = 3; i >= 0; i--) sram_write(at + (unsigned)i, (uint8_t)magic[i]);
}

static void record_clear(unsigned at, const char *magic) {
    if (sram_read(at) == (uint8_t)magic[0]) sram_write(at, 0);
}

/* A record there, of this build, whole — read straight off the battery, so
 * that asking does not disturb g_kept. */
static bool record_there(unsigned at, const char *magic) {
    for (int i = 0; i < 4; i++)
        if (sram_read(at + (unsigned)i) != (uint8_t)magic[i]) return false;
    if (get32(at + REC_TAG) != build_tag()) return false;
    uint32_t sum = 0x5A5A5A5Au;
    for (unsigned i = 0; i < sizeof g_kept; i++)
        sum = (sum << 5 | sum >> 27) ^ sram_read(at + REC_BODY + i);
    return sum == get32(at + REC_SUM);
}

/* ...and into g_kept. A record of a game that is not a paused, running game
 * on this one console is not one this file wrote. */
static bool record_read(unsigned at, const char *magic) {
    if (!record_there(at, magic)) return false;
    uint8_t *body = (uint8_t *)&g_kept;
    for (unsigned i = 0; i < sizeof g_kept; i++) body[i] = sram_read(at + REC_BODY + i);
    return g_kept.game.paused && !GAME_IS_LINKED(g_kept.menu.game_mode);
}

static void keep(const SuspendMenu *menu) {
    g_kept.menu = *menu;
    g_kept.game = g_session.game;
    g_kept.ai = g_ai;
    g_kept.stats[0] = g_stats[0];
    g_kept.stats[1] = g_stats[1];
}

void suspend_save(const SuspendMenu *menu) {
    keep(menu);
    record_write(SUSPEND_OFF, kSuspendMagic);
}

void suspend_clear(void) {
    record_clear(SUSPEND_OFF, kSuspendMagic);
}

bool suspend_load(SuspendMenu *menu) {
    if (!record_read(SUSPEND_OFF, kSuspendMagic)) return false;
    *menu = g_kept.menu;
    return true;
}

void suspend_apply(void) {
    g_session.game = g_kept.game;
    g_ai = g_kept.ai;
    g_stats[0] = g_kept.stats[0];
    g_stats[1] = g_kept.stats[1];
}

/* ----------------------------------------------------------------------- *
 * THE SAVED GAMES: EXIT GAME's SAVE, one for each mode a console plays on
 * its own — 1 PLAYER, VERSUS COMPUTER, WITH COMPUTER; the cable's two are
 * half on the other console. The same record as the suspend's, each in a
 * place of its own, and kept until it is played: choosing its mode on GAME
 * SELECT goes straight back into it, paused, and takes it off the battery
 * (to keep it, SAVE it again). Unlike the suspend, nothing brings one back
 * by itself at power-on. GAME SELECT marks the modes that have one, from
 * g_saved_mask, which is read off the battery once and kept as it changes.
 * ----------------------------------------------------------------------- */
#define SAVED_OFF(slot) (0x4800u + (unsigned)(slot) * 0x800u)
static const char kSavedMagic[4] = { 'S', 'A', 'V', 'E' };
typedef char saved_fit[REC_BODY + sizeof(SuspendBody) <= 0x800 &&
                       SAVED_OFF(SAVED_SLOTS) <= 0x8000 &&
                       SUSPEND_OFF + REC_BODY + sizeof(SuspendBody) <= SAVED_OFF(0) ? 1 : -1];

/* NOT static, for tools/romcheck: bit `slot` set, a game saved there; bit
 * 7, read already. */
uint8_t g_saved_mask;
#define SAVED_READ 0x80

int saved_slot(uint8_t game_mode) {
    switch (game_mode) {
    case GAME_1P:   return 0;
    case GAME_VS:   return 1;
    case GAME_WITH: return 2;
    default:        return -1;
    }
}

bool saved_has(uint8_t game_mode) {
    int slot = saved_slot(game_mode);
    if (slot < 0) return false;
    if (!(g_saved_mask & SAVED_READ)) {
        g_saved_mask = SAVED_READ;
        for (int i = 0; i < SAVED_SLOTS; i++)
            if (record_there(SAVED_OFF(i), kSavedMagic)) g_saved_mask |= (uint8_t)(1u << i);
    }
    return (g_saved_mask >> slot) & 1;
}

void saved_write(const SuspendMenu *menu) {
    int slot = saved_slot(menu->game_mode);
    if (slot < 0) return;
    keep(menu);
    record_write(SAVED_OFF(slot), kSavedMagic);
    saved_has(menu->game_mode);              /* the mask read, if it was not */
    g_saved_mask |= (uint8_t)(1u << slot);
}

bool saved_load(uint8_t game_mode, SuspendMenu *menu) {
    int slot = saved_slot(game_mode);
    if (slot < 0 || !record_read(SAVED_OFF(slot), kSavedMagic) ||
        g_kept.menu.game_mode != game_mode)
        return false;
    *menu = g_kept.menu;
    record_clear(SAVED_OFF(slot), kSavedMagic);
    g_saved_mask &= (uint8_t)~(1u << slot);
    return true;
}

void saved_clear_all(void) {
    for (int i = 0; i < SAVED_SLOTS; i++) record_clear(SAVED_OFF(i), kSavedMagic);
    g_saved_mask = SAVED_READ;
}
