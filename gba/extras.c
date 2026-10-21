/*
 * extras.c -- GAME SELECT's OPTIONS, the settings they keep, and the
 * console's lifetime figures.
 *
 * Under the chord GAME SELECT has a sixth line, OPTIONS, under WITH
 * COMPUTER, and it is a page of its own, as LEVEL SETTINGS is:
 *
 *   BRIGHTNESS   ▣□□□   the screen's curve, for an unlit GBA
 *   MUSIC        ▣▣▣▣   the tunes' volume
 *   EFFECTS      ▣▣▣▣   the effects'
 *   RUMBLE       ▣      on a Game Boy Player only
 *   BACKGROUND          a box: the colours behind the front end and the game
 *   PROTOTYPES          a box: the builds, by the publishers they were for
 *   STATS               a box: this console's player, every game added up
 *
 *   ERASE DATA          in the credits' grey: UP and DOWN step over it and
 *                       only SELECT, which steps down a line at a time,
 *                       reaches it; A on it asks twice, and it erases
 *                       everything kept.
 *
 * What a choice DOES — another skin, another curve, another background —
 * is done here, and the page drawn again for it (main.c's GAME SELECT hands
 * this the frame while the page is up).
 */
#include "port.h"

#include <string.h>

/* ----------------------------------------------------------------------- *
 * THE SETTINGS, kept on the battery (SAVE_SETTINGS_OFF) with a check byte,
 * and in force from power-on, chord or not: they are the console's knobs —
 * how loud, how bright, which background — and what the game IS does not
 * change with them. Only the box that sets them is behind the chord. A save
 * that does not check, or a console that never wrote one, is the cartridge
 * as it plays: both volumes full, the curve the identity, the rumble on,
 * the background black.
 *
 * NOT static: tools/romcheck reads them out of the ELF by name.
 * ----------------------------------------------------------------------- */
uint8_t g_settings[SET_COUNT] = { AUDIO_LEVEL_MAX, AUDIO_LEVEL_MAX, 0, 0, BG_BLACK };

static uint8_t settings_check(const uint8_t *s) {
    return (uint8_t)(0x5A ^ (s[0] * 3 + s[1] * 5 + s[2] * 7 + s[3] * 11 + s[4] * 13));
}

static void settings_apply(void) {
    nes_audio_set_levels(g_settings[SET_MUSIC], g_settings[SET_EFFECTS]);
}

/* At power-on, before anything is drawn: the curve and the background go in
 * without loading a palette, as none has been loaded yet. */
void settings_load(void) {
    uint8_t s[SET_COUNT];
    for (int i = 0; i < SET_COUNT; i++) s[i] = sram_read(SAVE_SETTINGS_OFF + (unsigned)i);
    bool ok = sram_read(SAVE_SETTINGS_OFF + SET_COUNT) == settings_check(s) &&
              s[SET_MUSIC] <= AUDIO_LEVEL_MAX && s[SET_EFFECTS] <= AUDIO_LEVEL_MAX &&
              s[SET_BRIGHT] < LCD_CURVES && !(s[SET_FLAGS] & ~SET_FLAGS_ALL) &&
              s[SET_BG] < BG_COUNT;
    if (ok)
        for (int i = 0; i < SET_COUNT; i++) g_settings[i] = s[i];
    g_lcd_gamma = kLcdGamma[g_settings[SET_BRIGHT]];
    settings_apply();
}

static void settings_save(void) {
    for (int i = 0; i < SET_COUNT; i++)
        sram_write(SAVE_SETTINGS_OFF + (unsigned)i, g_settings[i]);
    sram_write(SAVE_SETTINGS_OFF + SET_COUNT, settings_check(g_settings));
}

/* THE PLAYER'S RUMBLE, on a Game Boy Player: the switch is only shown on
 * one, and the rumble only ever happens on one. */
bool rumble_wanted(void) {
    return !(g_settings[SET_FLAGS] & SET_RUMBLE_OFF);
}

/* ----------------------------------------------------------------------- *
 * THE LIFETIME FIGURES: this console's player, every game added up — the
 * time the game ran for them, the games, the pieces and every kind of
 * clear, and the highest level reached. Counted off the same figures as the
 * end-of-game page (TengenStats), as they move, so a board started again
 * over A+B and a game quit through EXIT GAME count what they played: not
 * the computer's board, and nothing in the attract demo.
 *
 * On the battery at SAVE_STATS_OFF behind a magic and a check, whenever a
 * game is paused or over, and every minute of play; ERASE DATA takes them
 * with everything else. NOT static, for tools/romcheck.
 * ----------------------------------------------------------------------- */
LifetimeStats g_lifetime;
static bool g_lifetime_dirty;
static uint32_t g_lifetime_unsaved;     /* frames counted since the last save */
static const uint8_t kStatsMagic[4] = { 'S', 'T', 'A', 'T' };

static uint8_t lifetime_check(const uint8_t *b, unsigned n) {
    uint8_t sum = 0xA5;
    for (unsigned i = 0; i < n; i++) sum = (uint8_t)((sum << 1 | sum >> 7) ^ b[i]);
    return sum;
}

void lifetime_load(void) {
    uint8_t b[sizeof g_lifetime];
    for (int i = 0; i < 4; i++)
        if (sram_read(SAVE_STATS_OFF + (unsigned)i) != kStatsMagic[i]) return;
    for (unsigned i = 0; i < sizeof b; i++) b[i] = sram_read(SAVE_STATS_OFF + 4 + i);
    if (sram_read(SAVE_STATS_OFF + 4 + sizeof b) != lifetime_check(b, sizeof b)) return;
    for (unsigned i = 0; i < sizeof b; i++) ((uint8_t *)&g_lifetime)[i] = b[i];
}

static void lifetime_save(void) {
    const uint8_t *b = (const uint8_t *)&g_lifetime;
    for (int i = 0; i < 4; i++) sram_write(SAVE_STATS_OFF + (unsigned)i, kStatsMagic[i]);
    for (unsigned i = 0; i < sizeof g_lifetime; i++) sram_write(SAVE_STATS_OFF + 4 + i, b[i]);
    sram_write(SAVE_STATS_OFF + 4 + sizeof g_lifetime,
               lifetime_check(b, sizeof g_lifetime));
    g_lifetime_dirty = false;
    g_lifetime_unsaved = 0;
}

void lifetime_watch(int slot, const TengenStats *was, const TengenStats *now) {
    if (g_demo || slot != g_view || (g_ai_active && slot == g_ai_slot)) return;
    static const TengenStats kNone;
    /* A board put back on its feet starts its figures again. */
    const TengenStats *from = now->frames < was->frames ? &kNone : was;
    if (now->frames == from->frames && now->pieces == from->pieces) return;
    if (from->frames == 0 && now->frames > 0) g_lifetime.games++;
    g_lifetime.frames += now->frames - from->frames;
    g_lifetime_unsaved += now->frames - from->frames;
    if (now->pieces > from->pieces) g_lifetime.pieces += (uint32_t)(now->pieces - from->pieces);
    for (int i = 0; i < 4; i++)
        if (now->clears[i] > from->clears[i])
            g_lifetime.clears[i] += (uint32_t)(now->clears[i] - from->clears[i]);
    uint8_t level = g_session.game.player[slot].level;
    if (level > g_lifetime.top_level) g_lifetime.top_level = level;
    g_lifetime_dirty = true;
}

/* Once a frame: what was counted goes to the battery when the game is
 * still (paused, or over) or a minute's play has gone by. */
void lifetime_tick(bool still) {
    if (g_lifetime_dirty && (still || g_lifetime_unsaved >= 60u * 60u)) lifetime_save();
}

/* ----------------------------------------------------------------------- *
 * THE PAGE AND ITS BOXES
 *
 * OPTIONS is a page of its own, as LEVEL SETTINGS is: the menu frame and
 * logo, the heading, and under it a column of choices — the ones with a
 * value first, a bar of boxes (T_SQ_OFF, T_SQ_ON) or one box for a switch,
 * then the ones that open a box — and ERASE DATA a row lower in the credits'
 * grey. The names and their values are one block, CENTRED on the screen,
 * the arrow hanging before it as LEVEL SETTINGS' does.
 *
 * What a choice holds opens in a box over the page: BACKGROUND's colours,
 * PROTOTYPES' builds, STATS, and the erase's two questions, the second over
 * the first. Each in the pause box's frame (T_BOX_*, the game set's bank 3,
 * which no skin borrows), CENTRED on the screen, its heading centred in it in
 * the notes' colour, a blank row, and its lines a block centred under it,
 * the arrow before the chosen one and a value at the block's right.
 *
 * CENTRED ON A SCREEN THIRTY TILES WIDE AND TWENTY HIGH, exactly: a box is
 * an even number of tiles each way and so is its lines' block, padded where
 * they are not; a heading of an odd length rides the offset layer half a
 * tile over (MENU_TEXT_SHIFT_PX), as the menus' own headings do. The page's
 * heading is not lifted, as GAME SELECT's is, for that layer to be level.
 *
 * Drawn when something changes, not every frame: the page again when a box
 * goes (or a choice changes what it wears), and every box still up over it.
 * The three layers above the main map are cleared where a box lands.
 * ----------------------------------------------------------------------- */
enum { XP_TOP, XP_BACKGROUND, XP_PROTOS, XP_STATS, XP_SURE, XP_REALLY, XP_COUNT };
enum { TI_BRIGHT, TI_MUSIC, TI_EFFECTS, TI_RUMBLE, TI_BACKGROUND, TI_PROTOS, TI_STATS };

typedef struct { int8_t tx, ty, w, h; } XBox;

/* NOT static, for the checks (tools/romcheck): how deep it is (1, the page;
 * more, the boxes over it), which page each level is, the arrow on each,
 * and the skin each of PROTOTYPES' lines puts on. */
uint8_t g_extras_depth;
uint8_t g_extras_stack[4];
uint8_t g_extras_row[XP_COUNT];
uint8_t g_skin_rows[TITLE_SKIN_COUNT];
static XBox g_xbox[XP_COUNT];
static bool g_redraw_page, g_redraw_boxes, g_fresh;

/* THE PAGE'S BLOCK: the longest name (BRIGHTNESS, BACKGROUND, PROTOTYPES,
 * ERASE DATA), two blank columns, and the widest value, four boxes. */
#define PAGE_NAME_W   10
#define PAGE_VALUE_W  4
#define PAGE_BLOCK_W  (PAGE_NAME_W + 2 + PAGE_VALUE_W)
#define PAGE_LABEL_TX ((SCREEN_TW - PAGE_BLOCK_W) / 2)
#define PAGE_VALUE_TX (PAGE_LABEL_TX + PAGE_NAME_W + 2)
#define PAGE_ARROW_TX (PAGE_LABEL_TX - 2)
#define PAGE_TY       10       /* the first choice, a row clear of the heading */
typedef char extras_page_centred[(SCREEN_TW - PAGE_BLOCK_W) % 2 == 0 ? 1 : -1];
typedef char extras_bars_fit[LCD_CURVES <= PAGE_VALUE_W &&
                             AUDIO_LEVEL_MAX <= PAGE_VALUE_W ? 1 : -1];

/* THE PAGE'S CHOICES, the ones with a value first, RUMBLE on a Player only.
 * ERASE DATA is the line after them, a row lower, and not one of them. */
static int top_items(uint8_t *ids) {
    int n = 0;
    ids[n++] = TI_BRIGHT;
    ids[n++] = TI_MUSIC;
    ids[n++] = TI_EFFECTS;
    if (gbp_present()) ids[n++] = TI_RUMBLE;
    ids[n++] = TI_BACKGROUND;
    ids[n++] = TI_PROTOS;
    ids[n++] = TI_STATS;
    return n;
}

static const char *const kBgNames[BG_COUNT] = BG_NAMES;

/* Every build's skins fit their box (four prototypes at most: see the
 * save's layout in records.c). */
typedef char extras_skins_fit[TITLE_SKIN_COUNT <= 5 ? 1 : -1];

/* PROTOTYPES' LINES IN THE ORDER THE BUILDS WERE MADE, the release first:
 * the recipes' `order` (kSkinMenuOrder), or the dumps' own order from a
 * header extracted before there was one. */
static void skin_rows_fill(void) {
    g_skin_rows[0] = 0;
    for (int r = 1; r < TITLE_SKIN_COUNT; r++) {
#if SCREEN_PROTO_AVAILABLE && defined(SKIN_MENU_ORDER)
        g_skin_rows[r] = kSkinMenuOrder[r - 1];
#else
        g_skin_rows[r] = (uint8_t)r;
#endif
    }
}

static int row_of_skin(int skin) {
    for (int r = 0; r < TITLE_SKIN_COUNT; r++)
        if (g_skin_rows[r] == skin) return r;
    return 0;
}

/* The name the recipe gives a skin, the publisher it was made for
 * (kSkinMenuNames), or its place in the list from a header extracted before
 * there were names. */
static const char *skin_name(int skin) {
    if (skin <= 0) return "RELEASE";
#if SCREEN_PROTO_AVAILABLE && defined(SKIN_MENU_NAMES)
    return kSkinMenuNames[skin - 1];
#else
    static const char *const kNumbered[] = { "PROTO 1", "PROTO 2", "PROTO 3", "PROTO 4" };
    return kNumbered[(skin - 1) & 3];
#endif
}

#define STATS_LINES 10
#define STATS_FIGURE_W 7        /* the widest figure: a long time, h:mm */
static const char *const kStatsNames[STATS_LINES] = {
    "TIME", "GAMES", "PIECES", "LINES", "SINGLES", "DOUBLES", "TRIPLES",
    "TETRISES", "TETRIS RATE", "TOP LEVEL"
};

static int page_items(int page) {
    uint8_t ids[8];
    switch (page) {
    case XP_TOP:        return top_items(ids);
    case XP_BACKGROUND: return BG_COUNT;
    case XP_PROTOS:     return TITLE_SKIN_COUNT;
    case XP_STATS:      return STATS_LINES;
    case XP_SURE:
    case XP_REALLY:     return 2;
    default:            return 0;
    }
}

static const char *page_heading(int page) {
    static const char *const kHeading[XP_COUNT] = {
        "OPTIONS", "BACKGROUND", "PROTOTYPES", "STATS",
        "ARE YOU SURE?", "ARE YOU REALLY SURE?"
    };
    return kHeading[page];
}

static const char *line_name(int page, int i) {
    switch (page) {
    case XP_BACKGROUND: return kBgNames[i];
    case XP_PROTOS:     return skin_name(g_skin_rows[i]);
    case XP_STATS:      return kStatsNames[i];
    default:            return i ? "NO" : "YES";
    }
}

/* A box's lines as a block: the arrow and a blank column, the names, and a
 * value two columns after the longest — or, for STATS, which has no arrow,
 * the names and the figures. Even, for it to centre exactly. */
static int block_w(int page) {
    int names = 0;
    for (int i = 0; i < page_items(page); i++) {
        int n = (int)text_len(line_name(page, i));
        if (n > names) names = n;
    }
    int w = page == XP_STATS ? names + 2 + STATS_FIGURE_W
          : 3 + names + (page == XP_BACKGROUND || page == XP_PROTOS ? 2 + 1 : 0);
    return w + (w & 1);
}

/* The row of the page a choice stands on; the erase's is one lower. */
static int page_row_ty(int row) {
    uint8_t ids[8];
    return PAGE_TY + row + (row >= top_items(ids) ? 1 : 0);
}

/* THE BOX, IN THE MIDDLE OF THE SCREEN: wide enough for its heading and its
 * block with a column either side, high enough for the heading, a blank row
 * and the lines; both even. */
static XBox place_box(int page) {
    XBox b;
    int head = (int)text_len(page_heading(page));
    int bw = block_w(page);
    int w = (head > bw ? head : bw) + 4;
    int h = page_items(page) + 4;
    w += w & 1;
    h += h & 1;
    b.w = (int8_t)w;
    b.h = (int8_t)h;
    b.tx = (int8_t)((SCREEN_TW - w) / 2);
    b.ty = (int8_t)((SCREEN_TH - h) / 2);
    return b;
}

static void put_text(int tx, int ty, const char *text, int bank) {
    for (int i = 0; text[i]; i++)
        set_map_tile(tx + i, ty, WITH_BANK(ascii_tile(text[i]), bank));
}

/* ...centred over `w` columns from `tx`: on the offset layer, half a tile
 * over, when what is left over is odd. */
static void put_text_centred(int tx, int w, int ty, const char *text, int bank) {
    int len = (int)text_len(text);
    int x = tx + (w - len) / 2;
    if ((w - len) & 1) {
        for (int i = 0; i < len; i++)
            set_stats_tile(x + i, ty, WITH_BANK(ascii_tile(text[i]), bank));
    } else {
        put_text(x, ty, text, bank);
    }
}

/* A bar of `n` boxes from column `tx`, `lit` of them lit; one box alone is
 * a switch's, or a choice's. */
static void put_boxes(int tx, int ty, int n, int lit) {
    for (int i = 0; i < n; i++)
        set_map_tile(tx + i, ty, WITH_BANK(i < lit ? T_SQ_ON : T_SQ_OFF, BANK_LABEL));
}

static void box_frame(const XBox *b) {
    for (int y = 0; y < b->h; y++)
        for (int x = 0; x < b->w; x++) {
            int l = T_BLANK;
            bool top = y == 0, bottom = y == b->h - 1;
            bool left = x == 0, right = x == b->w - 1;
            if (top)         l = left ? T_BOX_TL : right ? T_BOX_TR : T_BOX_T;
            else if (bottom) l = left ? T_BOX_BL : right ? T_BOX_BR : T_BOX_B;
            else if (left)   l = T_BOX_L;
            else if (right)  l = T_BOX_R;
            set_map_tile(b->tx + x, b->ty + y, WITH_BANK(l, BANK_PAUSE));
            set_histogram_tile(b->tx + x, b->ty + y, T_BLANK);
            set_stats_tile(b->tx + x, b->ty + y, T_BLANK);
        }
    clear_panel_region(b->tx, b->ty, b->w, b->h);
}

/* The digits of `v` into `out` from `at`, ended; where they end. */
static int put_number(char *out, int at, uint32_t v) {
    char d[10];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 10);
    while (k) out[at++] = d[--k];
    out[at] = 0;
    return at;
}

/* STATS' figure for line `i`, into `v`. */
static void stats_figure(int i, char *v) {
    const LifetimeStats *s = &g_lifetime;
    uint32_t lines = s->clears[0] + 2 * s->clears[1] + 3 * s->clears[2] + 4 * s->clears[3];
    int n;
    switch (i) {
    case 0: {                                   /* hours and minutes */
        uint32_t minutes = s->frames / (60u * 60u);
        n = put_number(v, 0, minutes / 60);
        v[n++] = ':';
        v[n++] = (char)('0' + (minutes % 60) / 10);
        v[n++] = (char)('0' + minutes % 10);
        v[n] = 0;
        break;
    }
    case 1: put_number(v, 0, s->games); break;
    case 2: put_number(v, 0, s->pieces); break;
    case 3: put_number(v, 0, lines); break;
    case 4: case 5: case 6: case 7: put_number(v, 0, s->clears[i - 4]); break;
    case 8:                                     /* the lines made four at a time */
        n = put_number(v, 0, lines ? 400u * s->clears[3] / lines : 0);
        v[n++] = '%';
        v[n] = 0;
        break;
    default: put_number(v, 0, s->top_level); break;
    }
}

static int top_value(int id, int *bar) {
    *bar = PAGE_VALUE_W;
    switch (id) {
    case TI_BRIGHT:  *bar = LCD_CURVES; return g_settings[SET_BRIGHT] + 1;
    case TI_MUSIC:   *bar = AUDIO_LEVEL_MAX; return g_settings[SET_MUSIC];
    case TI_EFFECTS: *bar = AUDIO_LEVEL_MAX; return g_settings[SET_EFFECTS];
    case TI_RUMBLE:  *bar = 1; return rumble_wanted();
    default:         *bar = 0; return 0;
    }
}

/* THE PAGE: the frame, the heading, the choices, their values, the erase. */
static void draw_options_page(void) {
    static const char *const kTop[] = {
        "BRIGHTNESS", "MUSIC", "EFFECTS", "RUMBLE", "BACKGROUND", "PROTOTYPES", "STATS"
    };
    uint8_t ids[8];
    int n = top_items(ids);
    int row = g_extras_row[XP_TOP];
    /* The offset layer whole: a box's heading may have been on it. */
    for (int y = 0; y < SCREEN_TH; y++)
        for (int x = 0; x < SCREEN_TW; x++) set_stats_tile(x, y, T_BLANK);
    draw_menu_page("OPTIONS");
    for (int i = 0; i <= n; i++) {
        int ty = page_row_ty(i);
        if (i < n) {
            int bar, lit = top_value(ids[i], &bar);
            draw_menu_line(ty, PAGE_LABEL_TX, kTop[ids[i]], menu_text_bank());
            if (bar) put_boxes(PAGE_VALUE_TX, ty, bar, lit);
        } else {
            /* ...and the erase, a row clear of them, in the credits' grey. */
            draw_menu_line(ty, PAGE_LABEL_TX, "ERASE DATA", BANK_CREDIT);
        }
        set_map_tile(PAGE_ARROW_TX, ty, i == row
                     ? WITH_BANK(ascii_tile(MENU_ARROW_R), BANK_ARROW) : T_BLANK);
    }
}

static void draw_box(int page) {
    const XBox *b = &g_xbox[page];
    box_frame(b);
    put_text_centred(b->tx + 1, b->w - 2, b->ty + 1, page_heading(page), BANK_NOTE);
    int bw = block_w(page);
    int x0 = b->tx + (b->w - bw) / 2;
    int row = g_extras_row[page];
    for (int i = 0; i < page_items(page); i++) {
        int ty = b->ty + 3 + i;
        if (page == XP_STATS) {
            char v[16];
            stats_figure(i, v);
            put_text(x0, ty, kStatsNames[i], BANK_LABEL);
            put_text(x0 + bw - (int)text_len(v), ty, v, BANK_LABEL);
            continue;
        }
        if (i == row) {
            set_map_tile(x0, ty, WITH_BANK(T_ARROW_TAIL, BANK_LABEL));
            set_map_tile(x0 + 1, ty, WITH_BANK(T_ARROW_HEAD, BANK_LABEL));
        }
        put_text(x0 + 3, ty, line_name(page, i), BANK_LABEL);
        if (page == XP_BACKGROUND)
            put_boxes(x0 + bw - 1, ty, 1, g_settings[SET_BG] == i);
        else if (page == XP_PROTOS)
            put_boxes(x0 + bw - 1, ty, 1, g_title_skin == g_skin_rows[i]);
    }
}

static int top_page(void) {
    return g_extras_stack[g_extras_depth - 1];
}

static void push(int page) {
    g_xbox[page] = place_box(page);
    g_extras_stack[g_extras_depth++] = (uint8_t)page;
    /* The questions open on NO; the others on the one in force. */
    g_extras_row[page] = 0;
    if (page == XP_SURE || page == XP_REALLY) g_extras_row[page] = 1;
    if (page == XP_BACKGROUND) g_extras_row[page] = g_settings[SET_BG];
    if (page == XP_PROTOS) g_extras_row[page] = (uint8_t)row_of_skin(g_title_skin);
    g_redraw_boxes = true;
}

static void pop(void) {
    if (g_extras_depth) g_extras_depth--;
    g_redraw_page = true;
}

void extras_open(void) {
    skin_rows_fill();
    g_extras_depth = 0;
    g_extras_stack[g_extras_depth++] = XP_TOP;
    g_extras_row[XP_TOP] = 0;
    g_fresh = g_redraw_page = true;
    screen_blip();
}

bool extras_up(void) { return g_extras_depth != 0; }

/* A bar moved by `step`: LEFT and RIGHT stop at its ends, A goes round. */
static uint8_t bar_step(uint8_t v, int step, int max, bool round) {
    int n = v + step;
    if (round) return (uint8_t)((n + max + 1) % (max + 1));
    return (uint8_t)(n < 0 ? 0 : n > max ? max : n);
}

static void changed(void) {
    settings_save();
    g_redraw_page = true;
    /* The switches' bell, at the effects' volume as it now is. */
    nes_audio_play(NES_SOUND_DING);
}

/* ERASE DATA ERASES EVERYTHING the console has kept — for a console passed
 * on to somebody else: every table and the last name typed, the lifetime
 * figures, the pause menu's switches, these settings, the paused game and
 * the saved ones on the battery — and puts the release's title back. What
 * is left is the cartridge as it came, but for the chord, which is this
 * session's. */
static void erase_everything(void) {
    leader_erase_all();
    memset(&g_lifetime, 0, sizeof g_lifetime);
    lifetime_save();
    options_erase();
    static const uint8_t kFactory[SET_COUNT] = {
        AUDIO_LEVEL_MAX, AUDIO_LEVEL_MAX, 0, 0, BG_BLACK
    };
    for (int i = 0; i < SET_COUNT; i++) g_settings[i] = kFactory[i];
    settings_save();
    settings_apply();
    lcd_set_curve(0);
    bg_apply();
    suspend_clear();
    saved_clear_all();
    g_title_skin = 0;
    leader_use_table(front_skin());
}

/* One frame of the page and its boxes, the blank already waited for: their
 * buttons, then whatever has to be drawn again. False once the page has
 * gone, back to GAME SELECT. */
bool extras_frame(uint8_t pressed) {
    int page = top_page();
    int n = page_items(page);
    uint8_t *row = &g_extras_row[page];
    uint8_t ids[8];
    int regular = top_items(ids);
    bool round = (pressed & TENGEN_BTN_A) != 0;
    int step = (pressed & TENGEN_BTN_RIGHT) ? 1 : (pressed & TENGEN_BTN_LEFT) ? -1
             : round ? 1 : 0;

    if (pressed & TENGEN_BTN_START) {
        /* START: the boxes down, or off the page. */
        if (g_extras_depth > 1) {
            g_extras_depth = 1;
            g_redraw_page = true;
        } else {
            g_extras_depth = 0;
        }
        screen_blip();
    } else if (pressed & TENGEN_BTN_B) {
        pop();
        screen_blip();
    } else if (pressed & (TENGEN_BTN_UP | TENGEN_BTN_DOWN | TENGEN_BTN_SELECT)) {
        /* UP AND DOWN WALK THE CHOICES AND STEP OVER THE ERASE; SELECT goes
         * down a line at a time and goes on past the last choice onto it.
         * Off it again, DOWN and SELECT go round to the top and UP back to
         * the last choice. STATS has no arrow to move. */
        bool up = (pressed & TENGEN_BTN_UP) != 0;
        if (page == XP_TOP) {
            if (*row == regular) *row = up ? (uint8_t)(regular - 1) : 0;
            else if (up) *row = (uint8_t)((*row + regular - 1) % regular);
            else if ((pressed & TENGEN_BTN_SELECT) && *row == regular - 1) *row = (uint8_t)regular;
            else *row = (uint8_t)((*row + 1) % regular);
            g_redraw_page = true;
            cursor_blip();
        } else if (page != XP_STATS) {
            *row = (uint8_t)((*row + n + (up ? -1 : 1)) % n);
            g_redraw_boxes = true;
            cursor_blip();
        }
    } else if (pressed & (TENGEN_BTN_A | TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) {
        switch (page) {
        case XP_TOP:
            if (*row == regular) {
                if (round) {
                    push(XP_SURE);
                    screen_blip();
                }
                break;
            }
            switch (ids[*row]) {
            case TI_BRIGHT:
                g_settings[SET_BRIGHT] = bar_step(g_settings[SET_BRIGHT], step,
                                                  LCD_CURVES - 1, round);
                lcd_set_curve(g_settings[SET_BRIGHT]);
                changed();
                break;
            case TI_MUSIC:
            case TI_EFFECTS: {
                uint8_t *level = &g_settings[ids[*row] == TI_MUSIC ? SET_MUSIC : SET_EFFECTS];
                *level = bar_step(*level, step, AUDIO_LEVEL_MAX, round);
                settings_apply();
                changed();
                break;
            }
            case TI_RUMBLE:
                g_settings[SET_FLAGS] ^= SET_RUMBLE_OFF;
                if (rumble_wanted()) gbp_rumble(12);   /* what ON feels like */
                changed();
                break;
            default: {
                static const uint8_t kOpens[] = {
                    [TI_BACKGROUND] = XP_BACKGROUND, [TI_PROTOS] = XP_PROTOS,
                    [TI_STATS] = XP_STATS,
                };
                if (round) {
                    push(kOpens[ids[*row]]);
                    screen_blip();
                }
                break;
            }
            }
            break;
        case XP_BACKGROUND:
            if (!round || g_settings[SET_BG] == *row) break;
            g_settings[SET_BG] = *row;
            bg_apply();
            changed();
            break;
        case XP_PROTOS: {
            int skin = g_skin_rows[*row];
            if (!round || g_title_skin == skin) break;
            /* ANOTHER SKIN, AND THE PAGE WEARS IT AT ONCE: the menu's frame
             * and logo (draw_menu_frame), and the HIGH SCORE from its table.
             * The title takes its tiles when it is next drawn (draw_title).
             * The chord's own chirp. */
            g_title_skin = (uint8_t)skin;
            leader_use_table(front_skin());
            nes_audio_play(NES_SOUND_CHIRP);
            g_redraw_page = true;
            break;
        }
        case XP_STATS:
            if (round) {
                pop();
                screen_blip();
            }
            break;
        case XP_SURE:
            if (!round) break;
            if (*row == 0) push(XP_REALLY);
            else pop();
            screen_blip();
            break;
        case XP_REALLY:
            if (!round) break;
            if (*row == 0) {
                /* ERASED, and back to the page with the arrow off the erase,
                 * where a second A cannot find it. */
                erase_everything();
                g_extras_depth = 1;
                g_extras_row[XP_TOP] = 0;
                g_redraw_page = true;
                nes_audio_play(NES_SOUND_ALARM);
            } else {
                pop();
                pop();
                screen_blip();
            }
            break;
        }
    }
    if (!g_extras_depth) {
        clear_screen();                 /* GAME SELECT is drawn next frame */
        return false;
    }
    if (g_fresh) {
        clear_screen();
        g_fresh = false;
    }
    if (g_redraw_page) {
        draw_options_page();
        g_redraw_boxes = true;
    }
    if (g_redraw_boxes)
        for (int i = 1; i < g_extras_depth; i++) draw_box(g_extras_stack[i]);
    g_redraw_page = g_redraw_boxes = false;
    return true;
}
