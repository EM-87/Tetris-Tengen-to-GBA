/*
 * extras.c -- GAME SELECT's OPTIONS, the settings they keep, and the
 * console's lifetime figures.
 *
 * Under the chord GAME SELECT has a sixth line, OPTIONS, under WITH
 * COMPUTER, and it is a page of its own, as LEVEL SETTINGS is:
 *
 *   GAMMA        ▣□□    the screen's curve, for an unlit GBA
 *   SOUND               a box: MUSIC ▣▣▣□ and EFFECTS ▣▣▣▣, two volumes
 *   RUMBLE       ▣      on a Game Boy Player only
 *   BACKGROUND          a box: BLACK, WHITE, GREY or IVORY
 *   PROTOTYPES          a box: the builds, by the publishers they were for
 *   STATS               a box: this console's player, every game added up
 *
 *   ERASE SCORES        in the credits' grey: UP and DOWN step over it and
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
 * game is paused or over, and every minute of play; ERASE SCORES leaves
 * them, being the scores' erase. NOT static, for tools/romcheck.
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
 * logo, the heading lifted as GAME SELECT's is under the chord, the choices
 * a column at LEVEL SETTINGS' labels with its arrow before them, a value —
 * a bar of boxes, or one box for a switch (T_SQ_OFF, T_SQ_ON) — in its
 * values' column, and ERASE SCORES a row under them in the credits' grey.
 *
 * What a choice holds opens in a box over the page: SOUND's two volumes,
 * BACKGROUND's four, PROTOTYPES' builds, STATS, and the erase's two
 * questions, the second over the first. Each in the pause box's frame
 * (T_BOX_*, the game set's bank 3, which no skin borrows), a heading in the
 * notes' colour, the choices in the labels', the arrow before the chosen
 * one, a value against the frame's inside; its heading level with the line
 * it was opened from, as far as the screen lets it.
 *
 * Drawn when something changes, not every frame: the page again when a box
 * goes (or a choice changes what it wears), and every box still up over it.
 * The three layers above the main map are cleared where a box lands — the
 * heading's, lifted, one row further down.
 * ----------------------------------------------------------------------- */
enum { XP_TOP, XP_SOUND, XP_BACKGROUND, XP_PROTOS, XP_STATS, XP_SURE, XP_REALLY,
       XP_COUNT };
enum { TI_GAMMA, TI_SOUND, TI_RUMBLE, TI_BACKGROUND, TI_PROTOS, TI_STATS };

typedef struct { int8_t tx, ty, w, h; } XBox;

/* NOT static, for the checks (tools/romcheck): how deep it is (1, the page;
 * more, the boxes over it), which page each level is, and the arrow on
 * each. */
uint8_t g_extras_depth;
uint8_t g_extras_stack[4];
uint8_t g_extras_row[XP_COUNT];
static XBox g_xbox[XP_COUNT];
static bool g_redraw_page, g_redraw_boxes, g_fresh;

#define XB_W 18
#define PAGE_TY 9              /* the first choice; the heading at row 8 */
#define CASCADE_TX 4

/* THE PAGE'S CHOICES, RUMBLE on a Player only. ERASE SCORES is the line
 * after them, a row lower, and not one of them. */
static int top_items(uint8_t *ids) {
    int n = 0;
    ids[n++] = TI_GAMMA;
    ids[n++] = TI_SOUND;
    if (gbp_present()) ids[n++] = TI_RUMBLE;
    ids[n++] = TI_BACKGROUND;
    ids[n++] = TI_PROTOS;
    ids[n++] = TI_STATS;
    return n;
}

static const char *const kBgNames[BG_COUNT] = { "BLACK", "WHITE", "GREY", "IVORY" };

/* Every build's skins fit their box (four prototypes at most: see the
 * save's layout in records.c). */
typedef char extras_skins_fit[TITLE_SKIN_COUNT <= 5 ? 1 : -1];

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

static int page_items(int page) {
    uint8_t ids[8];
    switch (page) {
    case XP_TOP:        return top_items(ids);
    case XP_SOUND:      return 2;
    case XP_BACKGROUND: return BG_COUNT;
    case XP_PROTOS:     return TITLE_SKIN_COUNT;
    case XP_SURE:
    case XP_REALLY:     return 2;
    default:            return 0;
    }
}

static const char *page_heading(int page) {
    static const char *const kHeading[XP_COUNT] = {
        "OPTIONS", "SOUND", "BACKGROUND", "PROTOTYPES", "STATS",
        "ARE YOU SURE?", "ARE YOU REALLY SURE?"
    };
    return kHeading[page];
}

/* The row of the page a choice stands on; the erase's is one lower. */
static int page_row_ty(int row) {
    uint8_t ids[8];
    return PAGE_TY + row + (row >= top_items(ids) ? 1 : 0);
}

/* The box a page is drawn in. Its width holds its heading centred on the
 * grid — the parity of the two matched — and the lines; its height the
 * heading and the lines. Its heading is level with the line it was opened
 * from: the page's, or the box's under it. */
static XBox place_box(int page, int parent, int parent_row) {
    XBox b;
    int head = (int)text_len(page_heading(page));
    int w = page == XP_STATS ? 20 : XB_W;
    if (head + 2 > w - 2) w = head + 4;
    if ((w - 2 - head) & 1) w++;
    b.w = (int8_t)w;
    b.h = (int8_t)((page == XP_STATS ? STATS_LINES : page_items(page)) + 3);
    int tx, ty;
    if (parent == XP_TOP) {
        tx = MENU_LABEL_TX + CASCADE_TX;
        ty = page_row_ty(parent_row) - 1;
    } else {
        tx = g_xbox[parent].tx - CASCADE_TX;
        ty = g_xbox[parent].ty + 1 + parent_row;
    }
    /* Inside the menu's frame where it fits, as the page's words are. */
    if (tx + w > SCREEN_TW - 2) tx = SCREEN_TW - 2 - w;
    if (tx < 0) tx = 0;
    if (ty + b.h > SCREEN_TH) ty = SCREEN_TH - b.h;
    if (ty < 0) ty = 0;
    b.tx = (int8_t)tx;
    b.ty = (int8_t)ty;
    return b;
}

static void put_text(int tx, int ty, const char *text, int bank) {
    for (int i = 0; text[i]; i++)
        set_map_tile(tx + i, ty, WITH_BANK(ascii_tile(text[i]), bank));
}

/* A bar of `n` boxes from column `tx`, `lit` of them lit; one box alone is
 * a switch's, or a choice's. */
static void put_boxes(int tx, int ty, int n, int lit) {
    for (int i = 0; i < n; i++)
        set_map_tile(tx + i, ty, WITH_BANK(i < lit ? T_SQ_ON : T_SQ_OFF, BANK_LABEL));
}

/* ...and at a box's right, against its frame's inside. */
static void put_boxes_right(const XBox *b, int ty, int n, int lit) {
    put_boxes(b->tx + b->w - 1 - n, ty, n, lit);
}

/* ...and a figure there, its digits right-aligned. */
static void put_figure(const XBox *b, int ty, const char *text) {
    put_text(b->tx + b->w - 1 - (int)text_len(text), ty, text, BANK_LABEL);
}

static void put_line(const XBox *b, int row, const char *name, int bank, bool arrow) {
    int ty = b->ty + 2 + row;
    put_text(b->tx + 3, ty, name, bank);
    if (arrow) {
        set_map_tile(b->tx + 1, ty, WITH_BANK(T_ARROW_TAIL, BANK_LABEL));
        set_map_tile(b->tx + 2, ty, WITH_BANK(T_ARROW_HEAD, BANK_LABEL));
    }
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
        }
    /* The offset layer one row lower too: the page's heading rides it
     * lifted, so its row shows in the bottom of the one above. */
    for (int y = 0; y <= b->h; y++)
        for (int x = 0; x < b->w; x++) set_stats_tile(b->tx + x, b->ty + y, T_BLANK);
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

static void draw_stats(const XBox *b) {
    static const char *const kNames[STATS_LINES] = {
        "TIME", "GAMES", "PIECES", "LINES", "SINGLES", "DOUBLES", "TRIPLES",
        "TETRISES", "TETRIS RATE", "TOP LEVEL"
    };
    const LifetimeStats *s = &g_lifetime;
    uint32_t lines = s->clears[0] + 2 * s->clears[1] + 3 * s->clears[2] + 4 * s->clears[3];
    for (int i = 0; i < STATS_LINES; i++) {
        char v[16];
        int n = 0;
        switch (i) {
        case 0: {                               /* hours and minutes */
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
        case 8:                                 /* the lines made four at a time */
            n = put_number(v, 0, lines ? 400u * s->clears[3] / lines : 0);
            v[n++] = '%';
            v[n] = 0;
            break;
        default: put_number(v, 0, s->top_level); break;
        }
        put_text(b->tx + 2, b->ty + 2 + i, kNames[i], BANK_LABEL);
        put_figure(b, b->ty + 2 + i, v);
    }
}

/* THE PAGE: the frame, the heading, the choices, their values, the erase. */
static void draw_options_page(void) {
    static const char *const kTop[] = {
        "GAMMA", "SOUND", "RUMBLE", "BACKGROUND", "PROTOTYPES", "STATS"
    };
    uint8_t ids[8];
    int n = top_items(ids);
    int row = g_extras_row[XP_TOP];
    draw_menu_page("OPTIONS");
    for (int i = 0; i <= n; i++) {
        int ty = page_row_ty(i);
        if (i < n) {
            draw_menu_line(ty, MENU_LABEL_TX, kTop[ids[i]], menu_text_bank());
            if (ids[i] == TI_GAMMA)
                put_boxes(MENU_VALUE_TX, ty, LCD_CURVES, g_settings[SET_BRIGHT] + 1);
            if (ids[i] == TI_RUMBLE)
                put_boxes(MENU_VALUE_TX, ty, 1, rumble_wanted());
        } else {
            /* ...and the erase, a row clear of them, in the credits' grey. */
            draw_menu_line(ty, MENU_LABEL_TX, "ERASE SCORES", BANK_CREDIT);
        }
        set_map_tile(MENU_CURSOR_TX, ty, i == row
                     ? WITH_BANK(ascii_tile(MENU_ARROW_R), BANK_ARROW) : T_BLANK);
    }
}

static void draw_box(int page) {
    const XBox *b = &g_xbox[page];
    box_frame(b);
    const char *head = page_heading(page);
    put_text(b->tx + 1 + (b->w - 2 - (int)text_len(head)) / 2, b->ty + 1, head, BANK_NOTE);
    int row = g_extras_row[page];
    switch (page) {
    case XP_SOUND:
        put_line(b, 0, "MUSIC", BANK_LABEL, row == 0);
        put_boxes_right(b, b->ty + 2, AUDIO_LEVEL_MAX, g_settings[SET_MUSIC]);
        put_line(b, 1, "EFFECTS", BANK_LABEL, row == 1);
        put_boxes_right(b, b->ty + 3, AUDIO_LEVEL_MAX, g_settings[SET_EFFECTS]);
        break;
    case XP_BACKGROUND:
        for (int i = 0; i < BG_COUNT; i++) {
            put_line(b, i, kBgNames[i], BANK_LABEL, row == i);
            put_boxes_right(b, b->ty + 2 + i, 1, g_settings[SET_BG] == i);
        }
        break;
    case XP_PROTOS:
        for (int i = 0; i < TITLE_SKIN_COUNT; i++) {
            put_line(b, i, skin_name(i), BANK_LABEL, row == i);
            put_boxes_right(b, b->ty + 2 + i, 1, g_title_skin == i);
        }
        break;
    case XP_STATS:
        draw_stats(b);
        break;
    default:                                    /* the two questions */
        put_line(b, 0, "YES", BANK_LABEL, row == 0);
        put_line(b, 1, "NO", BANK_LABEL, row == 1);
        break;
    }
}

static int top_page(void) {
    return g_extras_stack[g_extras_depth - 1];
}

static void push(int page, int parent_row) {
    if (g_extras_depth)
        g_xbox[page] = place_box(page, top_page(), parent_row);
    g_extras_stack[g_extras_depth++] = (uint8_t)page;
    /* The questions open on NO; the others on their first line, or on the
     * one in force. */
    g_extras_row[page] = 0;
    if (page == XP_SURE || page == XP_REALLY) g_extras_row[page] = 1;
    if (page == XP_BACKGROUND) g_extras_row[page] = g_settings[SET_BG];
    if (page == XP_PROTOS) g_extras_row[page] = g_title_skin;
    g_redraw_boxes = true;
}

static void pop(void) {
    if (g_extras_depth) g_extras_depth--;
    g_redraw_page = true;
}

void extras_open(void) {
    g_extras_depth = 0;
    push(XP_TOP, 0);
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
    g_redraw_boxes = true;
    /* The switches' bell, at the effects' volume as it now is. */
    nes_audio_play(NES_SOUND_DING);
}

/* ERASE SCORES ERASES EVERYTHING the console has kept — for a console
 * passed on to somebody else: every table and the last name typed, the
 * lifetime figures, the pause menu's switches, these settings, the paused
 * game on the battery — and puts the release's title back. What is left is
 * the cartridge as it came, but for the chord, which is this session's. */
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
         * the last choice. */
        bool up = (pressed & TENGEN_BTN_UP) != 0;
        if (page == XP_TOP) {
            if (*row == regular) *row = up ? (uint8_t)(regular - 1) : 0;
            else if (up) *row = (uint8_t)((*row + regular - 1) % regular);
            else if ((pressed & TENGEN_BTN_SELECT) && *row == regular - 1) *row = (uint8_t)regular;
            else *row = (uint8_t)((*row + 1) % regular);
            g_redraw_page = true;
        } else if (n) {
            *row = (uint8_t)((*row + n + (up ? -1 : 1)) % n);
            g_redraw_boxes = true;
        }
        if (n) cursor_blip();
    } else if (pressed & (TENGEN_BTN_A | TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) {
        switch (page) {
        case XP_TOP:
            if (*row == regular) {
                if (round) {
                    push(XP_SURE, *row);
                    screen_blip();
                }
                break;
            }
            switch (ids[*row]) {
            case TI_GAMMA:
                g_settings[SET_BRIGHT] = bar_step(g_settings[SET_BRIGHT], step,
                                                  LCD_CURVES - 1, round);
                lcd_set_curve(g_settings[SET_BRIGHT]);
                g_redraw_page = true;
                changed();
                break;
            case TI_RUMBLE:
                if (!round) break;
                g_settings[SET_FLAGS] ^= SET_RUMBLE_OFF;
                if (rumble_wanted()) gbp_rumble(12);   /* what ON feels like */
                g_redraw_page = true;
                changed();
                break;
            default: {
                static const uint8_t kOpens[] = {
                    [TI_SOUND] = XP_SOUND, [TI_BACKGROUND] = XP_BACKGROUND,
                    [TI_PROTOS] = XP_PROTOS, [TI_STATS] = XP_STATS,
                };
                if (round) {
                    push(kOpens[ids[*row]], *row);
                    screen_blip();
                }
                break;
            }
            }
            break;
        case XP_SOUND: {
            uint8_t *level = &g_settings[*row ? SET_EFFECTS : SET_MUSIC];
            *level = bar_step(*level, step, AUDIO_LEVEL_MAX, round);
            settings_apply();
            changed();
            break;
        }
        case XP_BACKGROUND:
            if (!round || g_settings[SET_BG] == *row) break;
            g_settings[SET_BG] = *row;
            bg_apply();
            g_redraw_page = true;
            changed();
            break;
        case XP_PROTOS:
            if (!round || g_title_skin == *row) break;
            /* ANOTHER SKIN, AND THE PAGE WEARS IT AT ONCE: the menu's frame
             * and logo (draw_menu_frame), and the HIGH SCORE from its table.
             * The title takes its tiles when it is next drawn (draw_title).
             * The chord's own chirp. */
            g_title_skin = *row;
            leader_use_table(front_skin());
            nes_audio_play(NES_SOUND_CHIRP);
            g_redraw_page = true;
            break;
        case XP_STATS:
            if (round) {
                pop();
                screen_blip();
            }
            break;
        case XP_SURE:
            if (!round) break;
            if (*row == 0) push(XP_REALLY, 0);
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
