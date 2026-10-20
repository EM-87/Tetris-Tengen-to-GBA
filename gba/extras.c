/*
 * extras.c -- the title's EXTRAS, and the settings they keep.
 *
 * Under the chord, L or R on the title puts up a box the way the pause puts
 * up its menu: the prototypes to choose from, a sound test of the sounds the
 * cartridge carries and never plays, and the console's own settings — the
 * tune's volume and the effects', the screen's curve and, on a Game Boy
 * Player, its rumble. Under them, in the credits' dark grey, the high scores'
 * erase, which only SELECT reaches. What a choice DOES to the title (another
 * skin, the erase's own pages) is main.c's; this is the box and the settings.
 */
#include "port.h"

/* ----------------------------------------------------------------------- *
 * THE SETTINGS, kept on the battery (SAVE_SETTINGS_OFF) with a check byte,
 * and in force from power-on, chord or not: they are the console's knobs —
 * how loud and how bright — and what the game IS does not change with them.
 * Only the box that sets them is behind the chord. A save that does not
 * check, or a console that never wrote one, is the cartridge as it plays:
 * both volumes full, the curve the identity, the rumble on.
 *
 * NOT static: tools/romcheck reads them out of the ELF by name.
 * ----------------------------------------------------------------------- */
uint8_t g_settings[SET_COUNT] = { AUDIO_LEVEL_MAX, AUDIO_LEVEL_MAX, 0, 0 };

static uint8_t settings_check(const uint8_t *s) {
    return (uint8_t)(0x5A ^ (s[0] * 3 + s[1] * 5 + s[2] * 7 + s[3] * 11));
}

static void settings_apply(void) {
    nes_audio_set_levels(g_settings[SET_MUSIC], g_settings[SET_EFFECTS]);
}

/* At power-on, before anything is drawn: the curve goes in without loading
 * a palette again, as none has been loaded yet. */
void settings_load(void) {
    uint8_t s[SET_COUNT];
    for (int i = 0; i < SET_COUNT; i++) s[i] = sram_read(SAVE_SETTINGS_OFF + (unsigned)i);
    bool ok = sram_read(SAVE_SETTINGS_OFF + SET_COUNT) == settings_check(s) &&
              s[SET_MUSIC] <= AUDIO_LEVEL_MAX && s[SET_EFFECTS] <= AUDIO_LEVEL_MAX &&
              s[SET_BRIGHT] < LCD_CURVES && !(s[SET_FLAGS] & ~SET_FLAGS_ALL);
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
 * THE BOX
 *
 * One size for every page, so going from one to another never uncovers the
 * title: eighteen by eight, centred, a heading and five rows. The frame is
 * the pause box's (T_BOX_*, the game set's bank 3, which the title leaves
 * alone), the words its words: the heading in the notes' colour, the
 * choices in the labels', the arrow before the chosen one. Every length on
 * it even, so every line lands on the tile grid and the main map — the
 * offset layer is the title's TENGEN, shifted its own way.
 *
 * Drawn on the main map, and the three layers above it cleared where it
 * lands; the cathedral's sprites have a window hole there
 * (title_window_hole). Closing it draws the title again whole.
 * ----------------------------------------------------------------------- */
#define XB_W 18
#define XB_H 8
#define XB_TX ((SCREEN_TW - XB_W) / 2)
/* ...and two rows below the middle: centred, it cut the TETRIS logo in half
 * across its letters. Down here the title's name stands whole over it and
 * the box is in front of the cathedral, whose plinth still shows below. */
#define XB_TY ((SCREEN_TH - XB_H) / 2 + 2)
#define XB_ROWS (XB_H - 3)              /* below the heading */

enum { XP_TOP, XP_PROTOS, XP_SOUND, XP_SETTINGS, XP_COUNT };

/* NOT static, for the checks (tools/romcheck): open, the page, and the
 * arrow on each page. */
bool g_extras_open;
uint8_t g_extras_page;
uint8_t g_extras_row[XP_COUNT];

/* THE TOP PAGE'S LINES, and PROTOTYPES only on a build that has some. */
static int top_items(uint8_t *pages) {
    int n = 0;
    if (TITLE_SKIN_COUNT > 1) pages[n++] = XP_PROTOS;
    pages[n++] = XP_SOUND;
    pages[n++] = XP_SETTINGS;
    return n;
}

/* THE SOUND TEST: the five the engine carries that the cartridge never asks
 * for (constants.asm.txt:47-54, "unused" and "maybe unused"), by the number
 * the engine knows them by and the name the disassembly gives them. The port
 * puts four of them to work behind the chord — the record's jingle, the
 * chord's chirp, the switches' bell, the refusal's alarm — and SWIPE nowhere
 * but here. */
static const struct { uint8_t id; const char *name; } kSoundTest[] = {
    { NES_MUSIC_UNUSED_LEVELUP, "0C LEVEL UP" },
    { NES_SOUND_SWIPE,          "0F SWIPE" },
    { NES_SOUND_CHIRP,          "10 CHIRP" },
    { NES_SOUND_DING,           "11 DING" },
    { NES_SOUND_ALARM,          "12 ALARM" },
};
#define SOUND_TEST_COUNT ((int)(sizeof kSoundTest / sizeof kSoundTest[0]))

/* SETTINGS' LINES, and RUMBLE only on a Game Boy Player. */
enum { XS_MUSIC, XS_EFFECTS, XS_BRIGHT, XS_RUMBLE };
static int settings_items(void) {
    return gbp_present() ? 4 : 3;
}

/* Every build's skins fit the page (four prototypes at most: see the save's
 * layout in records.c). */
typedef char extras_skins_fit[TITLE_SKIN_COUNT <= XB_ROWS ? 1 : -1];

/* The name the recipe gives a skin (kSkinMenuNames), or its place in the
 * list from a header extracted before there were names. */
static const char *skin_name(int skin) {
    if (skin <= 0) return "RELEASE";
#if SCREEN_PROTO_AVAILABLE && defined(SKIN_MENU_NAMES)
    return kSkinMenuNames[skin - 1];
#else
    static const char *const kNumbered[] = { "PROTO 1", "PROTO 2", "PROTO 3", "PROTO 4" };
    return kNumbered[(skin - 1) & 3];
#endif
}

static int page_items(void) {
    uint8_t pages[XP_COUNT];
    switch (g_extras_page) {
    case XP_PROTOS:   return TITLE_SKIN_COUNT;
    case XP_SOUND:    return SOUND_TEST_COUNT;
    case XP_SETTINGS: return settings_items();
    default:          return top_items(pages);
    }
}

static void put_text(int tx, int ty, const char *text, int bank) {
    for (int i = 0; text[i]; i++)
        set_map_tile(tx + i, ty, WITH_BANK(ascii_tile(text[i]), bank));
}

/* A value at the right of its line, ending a column in from the frame. */
static void put_value(int ty, const char *text) {
    put_text(XB_TX + XB_W - 1 - (int)text_len(text), ty, text, BANK_LABEL);
}

static const char *level_text(uint8_t level) {
    static const char *const kLevel[AUDIO_LEVEL_MAX + 1] = {
        "OFF", "25%", "50%", "75%", "100%"
    };
    return kLevel[level <= AUDIO_LEVEL_MAX ? level : AUDIO_LEVEL_MAX];
}

static void draw_line(int row, const char *name, int bank) {
    int ty = XB_TY + 2 + row;
    put_text(XB_TX + 3, ty, name, bank);
    if (bank != BANK_CREDIT && row == g_extras_row[g_extras_page]) {
        set_map_tile(XB_TX + 1, ty, WITH_BANK(T_ARROW_TAIL, BANK_LABEL));
        set_map_tile(XB_TX + 2, ty, WITH_BANK(T_ARROW_HEAD, BANK_LABEL));
    }
}

static void draw_box(void) {
    for (int y = 0; y < XB_H; y++)
        for (int x = 0; x < XB_W; x++) {
            int l = T_BLANK;
            bool top = y == 0, bottom = y == XB_H - 1;
            bool left = x == 0, right = x == XB_W - 1;
            if (top)         l = left ? T_BOX_TL : right ? T_BOX_TR : T_BOX_T;
            else if (bottom) l = left ? T_BOX_BL : right ? T_BOX_BR : T_BOX_B;
            else if (left)   l = T_BOX_L;
            else if (right)  l = T_BOX_R;
            set_map_tile(XB_TX + x, XB_TY + y, WITH_BANK(l, BANK_PAUSE));
            set_stats_tile(XB_TX + x, XB_TY + y, T_BLANK);
            set_histogram_tile(XB_TX + x, XB_TY + y, T_BLANK);
        }
    clear_panel_region(XB_TX, XB_TY, XB_W, XB_H);

    static const char *const kHeading[XP_COUNT] = {
        "EXTRAS", "PROTOTYPES", "SOUND TEST", "SETTINGS"
    };
    const char *heading = kHeading[g_extras_page];
    put_text(XB_TX + 1 + (XB_W - 2 - (int)text_len(heading)) / 2, XB_TY + 1,
             heading, BANK_NOTE);

    switch (g_extras_page) {
    case XP_TOP: {
        static const char *const kPageNames[XP_COUNT] = {
            "", "PROTOTYPES", "SOUND TEST", "SETTINGS"
        };
        uint8_t pages[XP_COUNT];
        int n = top_items(pages);
        for (int i = 0; i < n; i++) draw_line(i, kPageNames[pages[i]], BANK_LABEL);
        /* ...and the erase on the last row, a row clear of them, in the
         * credits' grey: there, but not one of the choices the arrow walks
         * (extras_frame). */
        draw_line(XB_ROWS - 1, "ERASE SCORES", BANK_CREDIT);
        break;
    }
    case XP_PROTOS:
        for (int i = 0; i < TITLE_SKIN_COUNT && i < XB_ROWS; i++)
            draw_line(i, skin_name(i), BANK_LABEL);
        break;
    case XP_SOUND:
        for (int i = 0; i < SOUND_TEST_COUNT; i++)
            draw_line(i, kSoundTest[i].name, BANK_LABEL);
        break;
    case XP_SETTINGS: {
        static const char *const kBright[LCD_CURVES] = { "OFF", "LOW", "HIGH" };
        draw_line(XS_MUSIC, "MUSIC", BANK_LABEL);
        put_value(XB_TY + 2 + XS_MUSIC, level_text(g_settings[SET_MUSIC]));
        draw_line(XS_EFFECTS, "EFFECTS", BANK_LABEL);
        put_value(XB_TY + 2 + XS_EFFECTS, level_text(g_settings[SET_EFFECTS]));
        draw_line(XS_BRIGHT, "BRIGHT", BANK_LABEL);
        put_value(XB_TY + 2 + XS_BRIGHT, kBright[g_settings[SET_BRIGHT] % LCD_CURVES]);
        if (settings_items() > XS_RUMBLE) {
            int ty = XB_TY + 2 + XS_RUMBLE;
            draw_line(XS_RUMBLE, "RUMBLE", BANK_LABEL);
            uint16_t box = rumble_wanted() ? T_CHECK_ON : T_CHECK_OFF;
            set_map_tile(XB_TX + XB_W - 3, ty, WITH_BANK(box, BANK_LABEL));
            set_map_tile(XB_TX + XB_W - 2, ty, WITH_BANK(box + 1, BANK_LABEL));
        }
        break;
    }
    }
}

void extras_open(void) {
    g_extras_open = true;
    g_extras_page = XP_TOP;
    g_extras_row[XP_TOP] = 0;
    /* The fireworks stop where they are and their bursts go out — a show
     * frozen in the sky behind a box is a picture of a bug — and the
     * cathedral stays, its tower behind the box. */
    for (int i = TITLE_FIREWORK_FIRST; i < TITLE_OAM_COUNT; i++)
        MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
    title_window_hole(XB_TX * TILE_PX, XB_TY * TILE_PX, XB_W * TILE_PX, XB_H * TILE_PX);
    title_window(g_title_skin == 0);
}

static void extras_close(void) {
    g_extras_open = false;
    title_window_hole(0, 0, 0, 0);
    title_redraw();
}

bool extras_up(void) { return g_extras_open; }

/* The title's tune stands down for the sound test, so what is tested is
 * heard on its own. */
bool extras_quiet(void) {
    return g_extras_open && g_extras_page == XP_SOUND;
}

static void page_open(uint8_t page) {
    g_extras_page = page;
    g_extras_row[page] = 0;
    /* ...on the skin on the title, for the list of them. */
    if (page == XP_PROTOS) g_extras_row[page] = g_title_skin;
    screen_blip();
}

static void page_close(void) {
    /* The level-up jingle is a tune, and it outlasts the page: stopped
     * here, so the title's comes back on its own (front_music). */
    if (g_extras_page == XP_SOUND) stop_music();
    g_extras_page = XP_TOP;
    screen_blip();
}

/* A setting moved by `step` (LEFT -1, RIGHT and A +1), round. */
static void setting_step(int row, int step) {
    switch (row) {
    case XS_MUSIC:
    case XS_EFFECTS: {
        uint8_t *level = &g_settings[row == XS_MUSIC ? SET_MUSIC : SET_EFFECTS];
        *level = (uint8_t)((*level + AUDIO_LEVEL_MAX + 1 + step) % (AUDIO_LEVEL_MAX + 1));
        settings_apply();
        break;
    }
    case XS_BRIGHT:
        g_settings[SET_BRIGHT] =
            (uint8_t)((g_settings[SET_BRIGHT] + LCD_CURVES + step) % LCD_CURVES);
        lcd_set_curve(g_settings[SET_BRIGHT]);
        install_title_palette();
        break;
    case XS_RUMBLE:
        g_settings[SET_FLAGS] ^= SET_RUMBLE_OFF;
        if (rumble_wanted()) gbp_rumble(12);   /* what ON feels like */
        break;
    }
    settings_save();
    /* The switches' bell, at the effects' volume as it now is. */
    nes_audio_play(NES_SOUND_DING);
}

/* One frame of the box: its buttons, then its drawing (the caller has
 * waited for the blank). Returns what the title has to do about it. */
int extras_frame(uint8_t pressed, int *skin) {
    int result = EXTRAS_STAY;
    int n = page_items();
    uint8_t *row = &g_extras_row[g_extras_page];
    if (pressed & (TENGEN_BTN_UP | TENGEN_BTN_DOWN)) {
        *row = (uint8_t)((*row + n + ((pressed & TENGEN_BTN_UP) ? -1 : 1)) % n);
        cursor_blip();
    }
    if (pressed & TENGEN_BTN_START) {
        if (g_extras_page == XP_SOUND) stop_music();
        extras_close();
        screen_blip();
        return EXTRAS_CLOSED;
    }
    if (pressed & TENGEN_BTN_B) {
        if (g_extras_page != XP_TOP) {
            page_close();
        } else {
            extras_close();
            screen_blip();
            return EXTRAS_CLOSED;
        }
    } else if (g_extras_page == XP_TOP) {
        uint8_t pages[XP_COUNT];
        top_items(pages);
        if (pressed & TENGEN_BTN_A) page_open(pages[*row]);
        /* THE ERASE ANSWERS SELECT AND NOTHING ELSE: the arrow never lands
         * on it, so no run of UP, DOWN and A can, and the pages after it
         * ask twice with NO chosen (erase_records_prompt). */
        else if (pressed & TENGEN_BTN_SELECT) {
            extras_close();
            return EXTRAS_ERASE;
        }
    } else if (g_extras_page == XP_PROTOS) {
        if (pressed & TENGEN_BTN_A) {
            *skin = *row;
            extras_close();
            return EXTRAS_SKIN;
        }
    } else if (g_extras_page == XP_SOUND) {
        if (pressed & TENGEN_BTN_A) nes_audio_play(kSoundTest[*row].id);
    } else if (g_extras_page == XP_SETTINGS) {
        if (pressed & (TENGEN_BTN_A | TENGEN_BTN_RIGHT)) setting_step(*row, 1);
        else if (pressed & TENGEN_BTN_LEFT) setting_step(*row, -1);
    }
    if (g_extras_open) draw_box();
    return result;
}
