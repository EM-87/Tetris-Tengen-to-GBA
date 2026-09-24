/*
 * video.c -- the hardware, and the cartridge's art on it.
 *
 * Backgrounds and their scrolls, palettes and their banks, tile and
 * sprite uploads, the keypad, and the SKINS: everything that knows what
 * a GBA register is. The rest of the port draws through the handful of
 * primitives at the foot of it and never touches a register itself.
 */
#include "port.h"


/* The offset layer's scroll. A negative scroll moves the picture the other
 * way and the field is nine bits wide, so -n is written as 512-n. */
void set_offset_layer(int px) {
    REG_BG1HOFS = (uint16_t)(512 - px);
}

/* A LINE THAT SITS TWO PIXELS HIGHER THAN THE GRID, for the credit under
 * GAME SELECT: on the tile grid it touches the frame's bottom band, and there
 * is no row to spare above it.
 *
 * It rides the HISTOGRAM'S background, which is the only one of the four
 * scrolled UP (STATS_LIFT_PX) and which carries nothing at all outside a
 * match. Its three pixels across are not wanted here — this line's length is
 * even, so it is already centred on the grid — so the scroll goes flat for
 * the front end and back to the histogram's own when a board comes up. See
 * draw_static_screen. */
void set_credit_layer(bool front_end) {
    REG_BG3HOFS = front_end ? 0 : (uint16_t)(512 - STATS_SHIFT_PX);
}

/* lineClearSingle..lineClearTetris (main.asm.txt:1548-1561), one character
 * per playfield column including the walls, exactly as the ROM stores them.
 * Indexed by how many rows are coming down. */
const char *const kClearWord[5] = {
    "",
    " SINGLE     ",
    " DOUBLE     ",
    " TRIPLE     ",
    " TETRIS     ",
};

const uint8_t kPauseTiles[PAUSE_H][PAUSE_W] = {
    {0x10, 0x11, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0x12},
    {0x13, 0x14, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0x15},
};


/* The tileset's letters and digits sit at their ASCII codes, which is how the
 * ROM's own nametable spells "HIGH SCORE" and "STATS".
 *
 * WITH ONE EXCEPTION, and it is the only character in the port that is not
 * the cartridge's. ASCII's '?' is $3F and $3F in this set is the settings
 * screen's LEFT ARROW — Tengen's Tetris never asks the player anything, so it
 * never needed a question mark. The pause menu does (EXIT / SURE?), so
 * tools/extract_assets.py draws one into a slot the cartridge left empty and
 * says where; see plant_question_mark there for the whole of why and how.
 * Mapping it here rather than at the call sites means text_len, the centring
 * and every draw_text go on working on a string with a '?' in it. */
uint16_t ascii_tile(char c) {
    if (c == '?') return TILES_GAME_QUESTION;
    return (uint16_t)(unsigned char)c;
}

/* OFFSETS THE HEADLESS CHECKS NEED, exported so the ELF is the single place
 * that knows them — the same reason kNes6502Probe exists in nes_audio.c, and
 * for the same reason it was added there: a new field anywhere in TengenGame
 * moves every one of these, and a Python constant that did not move would
 * quietly start reading a neighbour. Adding `garbage_rng` did exactly that
 * and the cheat-code check began failing three tests away from the change. */
/* `used` AND `retain`, or link-time optimisation throws it away: nothing in
 * the program reads it -- the whole point is that something OUTSIDE the
 * program does -- and LTO can see that across the whole image where a single
 * translation unit could not. */
__attribute__((used, retain))
const uint16_t kGameProbe[18] = {
    (uint16_t)offsetof(TengenGame, field),
    (uint16_t)offsetof(TengenGame, player),
    (uint16_t)sizeof(TengenPlayerState),
    (uint16_t)offsetof(TengenPlayerState, piece.current),
    (uint16_t)offsetof(TengenPlayerState, piece.y),
    (uint16_t)offsetof(TengenPlayerState, level),
    (uint16_t)offsetof(TengenPlayerState, piece_stats),
    (uint16_t)offsetof(TengenGame, paused),
    (uint16_t)offsetof(TengenPlayerState, held_last_frame),
    (uint16_t)offsetof(TengenPlayerState, piece.next),
    (uint16_t)offsetof(TengenPlayerState, game_active),
    (uint16_t)offsetof(TengenPlayerState, piece.x),
    (uint16_t)offsetof(TengenPlayerState, score),
    (uint16_t)offsetof(TengenPlayerState, lines),
    (uint16_t)offsetof(TengenPlayerState, clear_counts),
    (uint16_t)offsetof(TengenPlayerState, piece.orientation),
    /* ...and the two flags a skin sets, which the cable check reads on both
     * consoles: the cell format has to agree, the rules have to be the
     * release's. */
    (uint16_t)offsetof(TengenGame, piece_id_cells),
    (uint16_t)offsetof(TengenGame, proto_rules),
};

TengenLink g_session;

/* The board's geometry, which is the MODE'S: see COOP_FIELD_TX. Read through
 * these rather than the constants, so a coop game does not quietly draw its
 * twelve columns into ten columns' worth of screen. */
int field_tx(void) {
    return g_session.game.coop ? COOP_FIELD_TX : FIELD_TX;
}
int field_col0(void) {
    return g_session.game.coop ? COOP_FIELD_COL0 : FIELD_COL0;
}
int field_cols(void) {
    return g_session.game.coop ? COOP_FIELD_PLAYABLE : FIELD_PLAYABLE;
}
/* The best score of this session. The cartridge's own 1P panel shows one
 * (see draw_panel), and like the cartridge's it does not survive a reset. */
/* The number on the panel during a game, which is the TOP OF THE TABLE and
 * not a separate thing: statsDataAddresses' last entry is the leaderboard's
 * first score (main.asm.txt:4107). It follows the live score while the game
 * is being played and settles back onto the table's own top when the game
 * ends — which is what the cartridge shows too, because the live score is
 * written over that address as it goes. */
uint32_t g_high_score;
/* True while the COMPUTER is playing one of the two slots — VERSUS or WITH.
 * Declared up here because the high-score table has to know: a machine never
 * takes a place on it (main.asm.txt:332-339). */
bool g_ai_active;
/* Which player this console shows and plays. Always 0 in a solo game; in a
 * linked match it is the cable master that is player 1, so the two consoles
 * differ here and nowhere else. */
uint8_t g_view;

/* THE PROGRAM'S ONE INTERRUPT HANDLER. Two sources: the vertical blank,
 * which exists only to wake vsync() below, and the serial port, whose work
 * is the cable's (link_serial_service). Every flag raised is acknowledged in
 * IF and ORed into the BIOS's mirror, which is what VBlankIntrWait is
 * actually sleeping on — a handler that acknowledged the hardware and not
 * the mirror would leave it asleep for good. */
IWRAM_CODE void irq_handler(void);
void irq_handler(void) {
    /* Only the sources this program switched on: IF can latch others. */
    uint16_t flags = REG_IF & REG_IE;
    if (flags & IRQ_SERIAL) link_serial_service();
    REG_IF = flags;
    BIOS_IF_MIRROR |= flags;
}

/* Installed once, at boot, before the first vsync(). The cable adds its own
 * source when it starts (link_init) and takes it away when it stops; the
 * vertical blank's stays on for good. */
void irq_init(void) {
    REG_IME = 0;
    BIOS_IRQ_VECTOR = irq_handler;
    REG_DISPSTAT |= DSTAT_VBL_IRQ;
    REG_IE |= IRQ_VBLANK;
    REG_IF = 0xFFFF;         /* discard anything already pending */
    REG_IME = 1;
}

/* THE WAIT FOR THE NEXT FRAME SLEEPS. It used to spin on VCOUNT — first
 * out of any blank already under way, then until line 160 — which on a real
 * console is the CPU at full clock for the whole of the visible frame, and
 * the rest of every frame is most of it: battery and heat for nothing. The
 * BIOS's VBlankIntrWait (SWI 5) halts the CPU until the NEXT vertical-blank
 * interrupt instead, discarding one already flagged, so it wakes on the same
 * line the spin did and the frame's timing is unchanged — which every
 * frame-counted check in gba-check (the golden audio, the tunes, the game
 * over's 480 frames) would notice if it were not.
 *
 * SWI 5 is `swi 0x05` in Thumb and `swi 0x050000` in ARM, and this file is
 * compiled Thumb; noinline keeps link-time optimisation from folding it into
 * one of the ARM functions in IWRAM, where the Thumb encoding would be
 * wrong. */
__attribute__((noinline)) void vsync(void) {
#if defined(__thumb__)
    __asm__ volatile ("swi 0x05" ::: "r0", "r1", "r2", "r3", "memory");
#else
    __asm__ volatile ("swi 0x050000" ::: "r0", "r1", "r2", "r3", "memory");
#endif
}

/* The GBA has every button the NES did, so this is a straight 1:1 remap with
 * no compromises — which is why the controls can be faithful. */
/* IN IWRAM, because the serial interrupt calls it (link_read_buttons, at the
 * instant of a transfer) and everything that interrupt runs is meant to be
 * fetched from internal WRAM rather than over the cartridge bus — which this
 * was not: it was the one call the handler made back out into ROM. */
IWRAM_CODE uint8_t read_buttons(void);
uint8_t read_buttons(void) {
    uint16_t keys = (uint16_t)(~REG_KEYINPUT & KEY_MASK); /* KEYINPUT is active low */
    uint8_t out = 0;
    if (keys & KEY_A)      out |= TENGEN_BTN_A;
    if (keys & KEY_B)      out |= TENGEN_BTN_B;
    if (keys & KEY_SELECT) out |= TENGEN_BTN_SELECT;
    if (keys & KEY_START)  out |= TENGEN_BTN_START;
    if (keys & KEY_UP)     out |= TENGEN_BTN_UP;
    if (keys & KEY_DOWN)   out |= TENGEN_BTN_DOWN;
    if (keys & KEY_LEFT)   out |= TENGEN_BTN_LEFT;
    if (keys & KEY_RIGHT)  out |= TENGEN_BTN_RIGHT;
    return out;
}

/* The same read, for the serial interrupt to call at the instant of a
 * transfer (see link.h). Nothing else may go in here. */
/* IWRAM too: -flto inlines it today, and without that it would be a ROM
 * trampoline between two IWRAM functions. */
IWRAM_CODE uint8_t link_read_buttons(void);
uint8_t link_read_buttons(void) { return read_buttons(); }

/* L and R have no NES equivalent, so the game proper never sees them and
 * they are free for the port's own switches. This reports the two together,
 * as a fresh press. */
bool shoulder_chord(void) {
    static bool was_held;
    uint16_t keys = (uint16_t)(~REG_KEYINPUT & KEY_MASK);
    bool held = (keys & KEY_L) && (keys & KEY_R);
    bool pressed = held && !was_held;
    was_held = held;
    return pressed;
}

bool pressed_shoulder(int which) {
    static bool was_held[2];
    uint16_t keys = (uint16_t)(~REG_KEYINPUT & KEY_MASK);
    bool held = (keys & (which == SHOULDER_L ? KEY_L : KEY_R)) != 0;
    bool pressed = held && !was_held[which];
    was_held[which] = held;
    return pressed;
}

void upload_tiles(void) {
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK);
    const uint8_t *src = kGameTiles;
    for (unsigned i = 0; i < sizeof(kGameTiles); i += 2) {
        dst[i / 2] = (uint16_t)(src[i] | (src[i + 1] << 8));
    }

    /* THE PAUSE MENU'S HEADINGS, ONE PIXEL HIGHER. The cartridge's letters
     * are seven pixels of ink under one blank row, so two lines stacked on
     * the grid are one pixel apart and nothing else is; the box's interior is
     * three lines of that exactly, with the bottom line already standing on
     * the frame. There is no layer that moves one pixel, and no pixel spare
     * in the box to move INTO — so the letters move instead: the same art,
     * shifted up a row inside its tile, which gives the blank row to the gap
     * UNDER the heading. See PMENU_RAISED_BASE. */
    const char *raised = PMENU_RAISED_CHARS;
    for (int i = 0; raised[i]; i++) {
        const uint8_t *g = kGameTiles + ascii_tile(raised[i]) * 32;
        vu16 *out = dst + (PMENU_RAISED_BASE + i) * 16;
        for (int row = 0; row < 8; row++) {
            const uint8_t *from = row < 7 ? g + (row + 1) * 4 : NULL;
            out[row * 2] = from ? (uint16_t)(from[0] | (from[1] << 8)) : 0;
            out[row * 2 + 1] = from ? (uint16_t)(from[2] | (from[3] << 8)) : 0;
        }
    }
    /* ...and the arrow moved left across two tiles; see T_ARROW_TAIL. In 4bpp
     * the leftmost pixel of a row is its lowest nibble, so a row read as one
     * 32-bit word moves left by n pixels with >> 4n and right with << 4n. */
    const uint8_t *arrow = kGameTiles + T_ARROW_R * 32;
    vu16 *tail = dst + T_ARROW_TAIL * 16;
    vu16 *head = dst + T_ARROW_HEAD * 16;
    const int shift = 4 * PMENU_ARROW_GAP_PX;
    for (int row = 0; row < 8; row++) {
        const uint8_t *r = arrow + row * 4;
        uint32_t px = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
                      ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
        uint32_t t = px << (32 - shift);     /* its first pixels, at the right */
        uint32_t h = px >> shift;            /* the rest, at the left */
        tail[row * 2] = (uint16_t)t; tail[row * 2 + 1] = (uint16_t)(t >> 16);
        head[row * 2] = (uint16_t)h; head[row * 2 + 1] = (uint16_t)(h >> 16);
    }
}

uint16_t raised_tile(char c) {
    const char *raised = PMENU_RAISED_CHARS;
    for (int i = 0; raised[i]; i++)
        if (raised[i] == c) return (uint16_t)(PMENU_RAISED_BASE + i);
    return ascii_tile(c);
}

/* One updatePalette set — four palettes of four entries — into four GBA
 * banks, exactly as the cartridge stores them. */
void upload_palette_set(int base, const uint8_t *set, vu16 *memory) {
    for (int bank = 0; bank < 4; bank++) {
        vu16 *dst = memory + (base + bank) * 16;
        for (int i = 0; i < 4; i++) dst[i] = nes_colour_to_gba(set[bank * 4 + i]);
    }
}

/* THE CREDITS: the cartridge's orange ($27, bgPalette1 bank 1), and once
 * the chord has been found the NES's dark grey ($00), so that a screen whose
 * text has just turned white is not shouted over by a line of names. Bank 9
 * is the credits' alone (the menu frame uses 8, 10 and 11). See menu_bank in
 * frontend.c and BANK_CREDIT. */
/* THE FIREWORKS STAY IN THE SKY. They carry the NES's behind-the-background
 * bit, which is what lets the blue frame, being opaque, hide a burst that
 * spreads over it. The brick columns either side are NOT opaque: the gaps in
 * their bricks are the backdrop, and a big burst near the edge showed its
 * sparks through them ("se ven por detras de los lingotes"). Window 0 over
 * the frame, x 17-223 as drawn, lets sprites in there and nowhere else while
 * the release's title is up; clear_screen takes it away with the title. */
#define TITLE_WIN_L 17
#define TITLE_WIN_R 224
void title_window(bool on) {
    if (on) {
        REG_WIN0H = (uint16_t)((TITLE_WIN_L << 8) | TITLE_WIN_R);
        REG_WIN0V = (uint16_t)((0 << 8) | 160);
        REG_WININ = 0x003F;                 /* inside: everything */
        REG_WINOUT = 0x002F;                /* outside: all but the sprites */
        REG_DISPCNT |= DCNT_WIN0;
    } else {
        REG_DISPCNT &= (uint16_t)~DCNT_WIN0;
    }
}

void set_credit_colour(void) {
    MEM_PALETTE[BANK_CREDIT * 16 + 1] =
        nes_colour_to_gba(g_pause_unlocked ? 0x00 : kRomPalette_bg_menu[1 * 4 + 1]);
}

void upload_palettes(void) {
    upload_palette_set(PAL_GAME_BASE, kRomPalette_bg_game, MEM_PALETTE);
    upload_palette_set(PAL_TITLE_BASE, kRomPalette_bg_title, MEM_PALETTE);
    upload_palette_set(PAL_MENU_BASE, kRomPalette_bg_menu, MEM_PALETTE);
    set_credit_colour();
    /* The notes' own bank, filled from the menu set's bank 2 and then left
     * alone for ever — see BANK_NOTE. */
    {
        vu16 *note = MEM_PALETTE + BANK_NOTE * 16;
        for (int i = 0; i < 4; i++)
            note[i] = nes_colour_to_gba(kRomPalette_bg_menu[2 * 4 + i]);
    }
    vu16 *piece = MEM_PALETTE + PAL_PIECE_BANK * 16;
    piece[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    vu16 *next = MEM_PALETTE + PAL_NEXT_BANK * 16;
    next[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    vu16 *piece2 = MEM_PALETTE + PAL_PIECE2_BANK * 16;
    piece2[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
}

/* setPlayfieldPaletteFromLevel (main.asm.txt:5328) recolours the settled
 * field using the LEVEL'S ONES DIGIT as the index, which is why the colours
 * cycle every ten levels. It writes background palette 0, entries 1-3. */
void set_field_palette_for_level(uint8_t level) {
    const uint8_t *entry = kRomPiecePalettes[level % 10];
    vu16 *bank0 = MEM_PALETTE + PAL_GAME_BASE * 16;
    for (int i = 0; i < 3; i++) bank0[1 + i] = nes_colour_to_gba(entry[i]);
}

/* setPiecePalette (main.asm.txt:5338) indexes the same table by PIECE ID and
 * writes a sprite palette, so the falling piece carries its own colours while
 * everything settled shares the level's. */
void set_bank_from_piece(int bank_index, TengenTetromino piece) {
    if (piece <= TT_NONE || piece >= TENGEN_TETROMINO_COUNT) return;
    const uint8_t *entry = kRomPiecePalettes[piece];
    vu16 *bank = MEM_PALETTE + bank_index * 16;
    for (int i = 0; i < 3; i++) bank[1 + i] = nes_colour_to_gba(entry[i]);
}

void set_piece_palette(TengenTetromino piece) {
    set_bank_from_piece(PAL_PIECE_BANK, piece);
}

/* The preview's, from the SAME table and the same rule — only indexed by the
 * piece that is coming rather than the one that is here. */
void set_next_palette(TengenTetromino piece) {
    set_bank_from_piece(PAL_NEXT_BANK, piece);
}

void upload_title_tiles(void) {
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK) + TITLE_TILE_BASE * 16;
    for (unsigned i = 0; i < sizeof(kTitleTiles); i += 2) {
        dst[i / 2] = (uint16_t)(kTitleTiles[i] | (kTitleTiles[i + 1] << 8));
    }
}

#if SCREEN_PROTO_AVAILABLE
/* One prototype's tiles, into the bank above the release's. Tile ids in a
 * text-mode background are ten bits, so 512-767 is still addressable from
 * charblock 0, and screenblock 28 starts well past it.
 *
 * ONE SLOT, RE-FILLED — not one slot per dump. Each prototype brings a whole
 * 256-tile pattern table of its own and only one title is ever on screen, so
 * they take turns in the same 512-767 window rather than each asking for a
 * bank the charblock does not have. That is what makes the number of skins a
 * question of cartridge space instead of video memory. 8KB a swap is more
 * than a vblank's worth of writes, which is why the caller clears the map
 * first: with the map blank the tiles being rewritten are not on screen. */
void upload_proto_tiles(int skin) {
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK) + PROTO_TILE_BASE * 16;
    const uint8_t *src = kProtoTiles[skin];
    for (unsigned i = 0; i < TILES_PROTO_BYTES; i += 2) {
        dst[i / 2] = (uint16_t)(src[i] | (src[i + 1] << 8));
    }
}
#endif

#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
/* A SKIN THAT DOES NOT STOP AT THE TITLE.
 *
 * The prototypes play on screens of their own — a green fret where the
 * release has its blue braid, and blocks that are flat or striped squares
 * rather than its shaded joined ones — and the skin used to be the title art
 * and nothing else.
 *
 * WHAT MAKES THIS CHEAP is that all four builds draw a settled cell with the
 * cell's own nibble as the tile index (measured on proto_b, playfield RAM
 * against nametable; see tools/extract_assets.py). So there is no second
 * layout, no second draw path and no second set of tile numbers: a skin is
 * TWENTY-TWO TILE SLOTS re-uploaded in place. The same set_map_tile calls
 * draw the same ids and different art comes out.
 *
 * Which twenty-two: the seven blocks, the panel's top run, the two walls, the
 * two elbows over them, and the shelf. 704 bytes, well inside a vblank, so
 * unlike the title's 8KB swap this needs no blank screen to hide behind.
 *
 * Skin 0 is the release, and putting it back is the same loop reading the
 * release's own tile data — which is why kGameTiles stays around rather than
 * being uploaded once and forgotten. */
static void upload_skin_play(int skin) {
    vu16 *base = MEM_CHARBLOCK(CHARBLOCK);
    for (int i = 0; i < SKIN_SLOT_COUNT; i++) {
        unsigned slot = kSkinSlots[i];
        /* THE TWO ABOVE 255 ARE THE PANEL'S OWN TOP RUN and have no art of
         * their own in the cartridge's tile set — they take the release's
         * $89/$8E, which is what the panel used before it needed a slot to
         * itself. See SKIN_PANEL_RUN_BASE. */
        const uint8_t *src;
        if (skin >= 0) {
            src = &kSkinTiles[skin][i * 32];
        } else if (slot >= SKIN_PANEL_RUN_BASE) {
            src = &kGameTiles[kSkinPanelRunRelease[slot - SKIN_PANEL_RUN_BASE] * 32];
        } else {
            src = &kGameTiles[slot * 32];
        }
        vu16 *dst = base + slot * 16;
        for (int b = 0; b < 32; b += 2)
            dst[b / 2] = (uint16_t)(src[b] | (src[b + 1] << 8));
    }
}
static void upload_skin_banner(int skin) {
    if (skin < 0) return;          /* the release draws its own, from slot ids */
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK) + SKIN_BANNER_BASE * 16;
    const uint8_t *src = kSkinBannerArt[skin];
    for (unsigned i = 0; i < (unsigned)kSkinBannerCount[skin] * 32; i += 2)
        dst[i / 2] = (uint16_t)(src[i] | (src[i + 1] << 8));
}
static void upload_skin_stats(int skin) {
    if (skin < 0) return;          /* the release draws its own one run */
    vu16 *dst = MEM_CHARBLOCK(CHARBLOCK) + SKIN_STATS_BASE * 16;
    const uint8_t *src = kSkinStatsBars[skin];
    for (unsigned i = 0; i < SKIN_STATS_RUNS * SKIN_STATS_STEPS * 32; i += 2)
        dst[i / 2] = (uint16_t)(src[i] | (src[i + 1] << 8));
}
static void upload_skin_stats_palette(int skin) {
    if (skin < 0) return;
    vu16 *dst = MEM_PALETTE + SKIN_STATS_BANK * 16;
    for (int i = 0; i < 4; i++)
        dst[i] = nes_colour_to_gba(kSkinStatsPalette[skin][i]);
}
static void upload_skin_plaque_palette(int skin) {
    if (skin < 0) return;          /* no skin, no loan: BANK_PAUSE is bank 3 */
    vu16 *dst = MEM_PALETTE + SKIN_PLAQUE_BANK * 16;
    for (int i = 0; i < 4; i++)
        dst[i] = nes_colour_to_gba(kSkinPlaquePalette[skin][i]);
}
static void upload_skin_logo_palette(int skin) {
    if (skin < 0) return;
    vu16 *dst = MEM_PALETTE + SKIN_LOGO_BANK * 16;
    for (int i = 0; i < 4; i++)
        dst[i] = nes_colour_to_gba(kSkinLogoPalette[skin][i]);
}

/* ...and the colours the frame is drawn in. The braid's bank is the one thing
 * a skinned board needs from the prototype's palette; the blocks keep the
 * release's per-level colours, which cycle every ten levels and are the same
 * mechanism in every build.
 *
 * kRomPalette_bg_skin, NOT kRomPalette_bg_proto: a build's title palette and
 * its game palette are two separate uploads, and taking the title's here put
 * proto_b's green fret on the screen in the blue and red of its cathedral. */
static void upload_skin_frame_palette(int skin) {
    const uint8_t *set = skin < 0 ? kRomPalette_bg_game
                                  : kRomPalette_bg_skin[skin];
    int bank = skin < 0 ? BRAID_BANK - PAL_GAME_BASE : kSkinFrameBank[skin];
    vu16 *dst = MEM_PALETTE + BRAID_BANK * 16;
    for (int i = 0; i < 4; i++) dst[i] = nes_colour_to_gba(set[bank * 4 + i]);

    /* AND THE MENUS' COPY OF IT. The menu frame and the HIGH SCORES frame are
     * drawn from the same charblock slots but out of bgPalette1, bank 2 — so
     * a skinned board and an unskinned menu were the same art in two colours.
     * The release's own bank 2 goes back when the skin comes off. */
    const uint8_t *mset = skin < 0 ? kRomPalette_bg_menu
                                   : kRomPalette_bg_skin[skin];
    int mbank = skin < 0 ? SKIN_MENU_BRAID_BANK - PAL_MENU_BASE
                         : kSkinFrameBank[skin];
    vu16 *mdst = MEM_PALETTE + SKIN_MENU_BRAID_BANK * 16;
    for (int i = 0; i < 4; i++) mdst[i] = nes_colour_to_gba(mset[mbank * 4 + i]);
}
#endif

/* WHICH SKIN THE TILE SLOTS ARE HOLDING RIGHT NOW. 0 is the release; 1..N
 * index the prototypes in the order the dumps were given to
 * extract_assets.py. The title cycles it with L+R and the board follows,
 * which is the whole of "the skin does not stop at the title". */
uint8_t g_title_skin;

/* ...and whether the chord has been rung on the title yet. A shoulder on its
 * own does NOTHING until it has: the skin is a thing to find, and a player
 * who has not found it should not be able to change the title by resting a
 * finger on L. Once found, L and R alone step the list backwards and forwards
 * — with four skins, going back one used to mean going round three. */
bool g_title_skin_found;

/* Puts the board into (or back out of) a prototype's clothes. Cheap enough to
 * call on every repaint — it compares first and does nothing when the slots
 * already hold what is wanted. `skin` is -1 for the release.
 *
 * THE MENUS ASK FOR THE RELEASE BACK. Their frame is drawn out of the same
 * charblock slots as the board's, but in the MENU palette bank, so a
 * prototype's fret would be there in the cartridge's blue. The board is where
 * a skin belongs; see the callers. */
void apply_skin(int skin) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    /* -2, never a skin number: the first call must upload even the
     * release (-1), because upload_skin_play is the only writer of the
     * panel's braid run at SKIN_PANEL_RUN_BASE. Starting at -1 left the
     * release's top braid blank on a fresh boot until a prototype had
     * been entered and left. */
    static int loaded = -2;
    if (skin != loaded) {
        loaded = skin;
        upload_skin_play(skin);
        upload_skin_banner(skin);
        upload_skin_stats(skin);
        upload_skin_frame_palette(skin);
    }
    /* THE BORROWED BANKS ARE NOT MEMOISED, and that is the whole of a bug
     * worth setting down. Three of a skin's palettes are LOANS from the
     * title's four — the menu logo's, the histogram's runs and the game over
     * plaque's — and the title takes them back the moment it is drawn. So
     * "this skin is already loaded" says nothing about whether those three
     * banks still hold what the skin put there: go to the menus, back to the
     * title, and to the menus again, and the early return above left the logo
     * in the title's colours. Reloading them is twelve palette entries; the
     * tiles above are the expensive part and they really are unchanged. */
    upload_skin_logo_palette(skin);
    upload_skin_stats_palette(skin);
    upload_skin_plaque_palette(skin);
#else
    (void)skin;
#endif
}

/* WHICH SKIN THE BOARD IS WEARING, which is not always the title's.
 *
 * Alone, it is the title's choice, and it brings that build's RULES with it
 * (proto_rules). Over the cable it is whatever the lobby agreed
 * (tengen_lobby_skins): the MASTER's choice, worn only if the other console
 * has the same art, and as PAINT ONLY — both play by the release's rules,
 * because a rule the two did not both choose is not a race. What the two
 * consoles must agree on is the cell format, since a skinned board stores
 * piece ids (piece_id_cells) and lockstep compares the boards byte for byte;
 * the lobby's answer is the same on both, so they do. On the slave the index
 * can differ from its own title's — it is this build's index for the
 * master's art. */
static int8_t g_board_skin = -1;

/* Called as a match starts. `cable_skin` is the lobby's answer, an index into
 * this build's own skins or -1; it is ignored unless `linked`. */
void skin_begin_match(bool linked, int cable_skin) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    if (linked)
        g_board_skin = (int8_t)((cable_skin >= 0 && cable_skin < SCREEN_PROTO_COUNT)
                                    ? cable_skin : -1);
    else
        g_board_skin = (int8_t)(g_title_skin ? (int)g_title_skin - 1 : -1);
#else
    (void)linked;
    (void)cable_skin;
    g_board_skin = -1;
#endif
    /* ...and this match's scores go in this build's table. A prototype is a
     * different game — see LEADER_TABLES — but over the cable it is the
     * release in other clothes, and the records swap needs both consoles on
     * the same page. */
    leader_use_table(linked ? -1 : play_skin());
    g_session.game.piece_id_cells = g_board_skin >= 0;
    /* ...and their RULES with their paint, alone: the level every ten lines,
     * no wall kick, and rows that go the frame they complete. See
     * proto_rules. */
    g_session.game.proto_rules = !linked && g_board_skin >= 0;
}

/* THIS BUILD'S SKINS AS THE CABLE KNOWS THEM: a fingerprint of each one's
 * board art — the 51 slots it swaps in and its palette — which the same dump
 * produces in any build. See tengen_lobby_skins. */
int skin_prints(uint16_t *out, int max) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    int n = SCREEN_PROTO_COUNT < max ? SCREEN_PROTO_COUNT : max;
    for (int i = 0; i < n; i++) {
        uint16_t a = tengen_skin_fingerprint(kSkinTiles[i], sizeof(kSkinTiles[i]));
        uint16_t b = tengen_skin_fingerprint(kRomPalette_bg_skin[i],
                                             sizeof(kRomPalette_bg_skin[i]));
        out[i] = (uint16_t)((a ^ (uint16_t)(b * 31u)) & TENGEN_SKIN_PRINT_MASK);
    }
    return n;
#else
    (void)out;
    (void)max;
    return 0;
#endif
}

/* What the FRONT END should be wearing. Unlike the board it follows the
 * title's choice directly: a menu is not a match, so there is no cable to
 * keep in step and nothing to diverge. */
int front_skin(void) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    return g_title_skin ? (int)g_title_skin - 1 : -1;
#else
    return -1;
#endif
}

/* ...and what the board is wearing: see g_board_skin. */
int play_skin(void) {
    return g_board_skin;
}

/* WHICH PALETTE THE GAME OVER PLAQUE AND THE PAUSE BOX ARE DRAWN IN.
 *
 * The release frames both in red out of game bank 3 — which is also where the
 * HUD's labels and counters live, NEXT and SCORE and the rest. Every
 * prototype frames them in BLUE, and taking their bank 3 wholesale is not the
 * way to get that: their own NEXT is as red as the release's, so the bank
 * they draw the plaque in is not the bank they draw the header in.
 *
 * So the plaque gets a bank to itself, borrowed from the TITLE's four exactly
 * as the menu logo and the histogram's runs are: no board and no title are
 * ever up at once, and install_title_palette fills all four again on every
 * visit. Without a skin nothing is borrowed and BANK_PAUSE is bank 3, which
 * is what the cartridge uses. */
int plaque_bank(void) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    if (play_skin() >= 0) return SKIN_PLAQUE_BANK;
#endif
    return BANK_PAUSE;
}

/* THE NOISE A SCREEN MAKES WHEN IT CHANGES, and the four builds do not agree
 * on it. Logged off the real dumps, every write to $4000-$4013 while the menu
 * cursor moved:
 *
 *   release      $4004=BC $4005=DA $4006=B7 $4007=04, then BB BA B9 B7 B4 B2
 *   proto A/B/C  $4004=BE $4005=00 $4006=21 $4007=00, then BC BA B8 B6 B5 B4 B3
 *
 * Both are pulse 2 with the volume stepped down by hand. The release's is a
 * LOW note — period $4B7 is about 93Hz — with the sweep unit bending it
 * further down over eleven frames; every prototype's has no sweep, sits at
 * period $021, about 3.3kHz, and is gone in eight. A thunk against a tick,
 * and one of the plainer differences between the builds once you hear them
 * next to each other.
 *
 * The release's are the cartridge's own effects and the engine in this ROM
 * knows them by number. The prototypes' engine is not in this ROM, so theirs
 * are captured off the dumps as the registers they write and replayed through
 * the same conversion — see nes_audio_effect and read_skin_effects. */
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY && SCREEN_SKIN_FX
static bool skin_effect(int which) {
    int skin = front_skin();
    if (skin < 0) return false;
    uint8_t channel = kSkinFxChannel[skin][which];
    if (channel == SKIN_FX_SILENT) {
        /* A real answer, not a missing one: proto_a changes screen without a
         * sound. Nothing is played AND the release's is not either. */
        nes_audio_effect(0, 0, 0, SKIN_FX_SILENT);
        return true;
    }
    nes_audio_effect(&kSkinFxRegs[skin][which * SKIN_FX_FRAMES * 4],
                      kSkinFxWrite[skin][which], SKIN_FX_FRAMES, channel);
    return true;
}
#else
static bool skin_effect(int which) { (void)which; return false; }
#endif

/* WHETHER THIS GAME'S PAUSE LETS ITS TUNE PLAY ON. Only a prototype's rules
 * can, and only the prototype whose dump measured that way
 * (read_skin_pause_music): proto_a. The other three silence it, as the
 * release does. */
bool pause_keeps_music(void) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY && defined(SKIN_PAUSE_MUSIC)
    if (g_session.game.proto_rules && g_board_skin >= 0)
        return kSkinPauseKeepsMusic[g_board_skin] != 0;
#endif
    return false;
}

/* ...AND WHAT A PROTOTYPE'S LEVEL-UP SOUNDS LIKE: no jingle in any of them,
 * and in proto_c and proto_d not even the line's own sound on that clear
 * (read_skin_levelup_sound). True if this game's level-up clear should sound
 * like any other clear. */
bool levelup_clear_sound(void) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY && defined(SKIN_PAUSE_MUSIC)
    if (g_session.game.proto_rules && g_board_skin >= 0)
        return kSkinLevelUpClearSound[g_board_skin] != 0;
#endif
    return true;
}

/* The blip a screen change makes... */
void screen_blip(void) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY && SCREEN_SKIN_FX
    if (skin_effect(SKIN_FX_SCREEN)) return;
#endif
    nes_audio_play(NES_SOUND_SCREEN_SWITCH);
}

/* ...and the tick the cursor makes moving down a list. */
void cursor_blip(void) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY && SCREEN_SKIN_FX
    if (skin_effect(SKIN_FX_CURSOR)) return;
#endif
    nes_audio_play(NES_SOUND_MENU_SELECT);
}

/* WHICH TILE ONE CELL OF A PIECE IS DRAWN WITH, and it is not the same
 * question in the two builds.
 *
 * The release has fourteen block graphics and kTileIds picks one per cell so
 * that four squares read as one joined shape. The prototypes have seven, one
 * per tetromino, and draw all four cells with it — measured, by letting
 * proto_b play itself and watching the playfield: every piece that settled
 * wrote four cells of ONE value, never four of four.
 *
 * So under a skin the tile is the piece's own id. This is the falling piece
 * and the preview; the settled field is the same rule inside the core, where
 * lock_piece is what writes it (see piece_id_cells). */
uint8_t piece_cell_tile(TengenTetromino piece, uint8_t orientation,
                                int occupied_index) {
    if (play_skin() >= 0) return (uint8_t)piece;
    return tengen_tile_id_for_cell(piece, orientation, occupied_index);
}

/* The cartridge's whole sprite bank, uploaded once. Both the dancers and the
 * line-clear puff live in it, so every sprite tile id in this file is the
 * ROM's own index. */
void upload_sprite_tiles(void) {
    vu16 *dst = MEM_OBJ_TILES;
    for (unsigned i = 0; i < sizeof(kDancerTiles); i += 2) {
        dst[i / 2] = (uint16_t)(kDancerTiles[i] | (kDancerTiles[i + 1] << 8));
    }
    /* The level-up interlude installs spritePalette2 (main.asm.txt:2044), and
     * the dancers pick among its four palettes with their own attribute bytes
     * — which is why the six are not all the same colour. */
    upload_palette_set(PAL_OBJ_DANCER, kRomPalette_obj_dancers, MEM_PALETTE_OBJ);

    /* CHR bank 3, the title screen's sprite bank, above the dancers' 256:
     * the cathedral overlay at tiles $02-$13, the sparkles at $14-$17 and the
     * firework bursts from $90 up. The title installs spritePalette1 for them
     * (main.asm.txt:4492-4494). */
    vu16 *tdst = MEM_OBJ_TILES + TITLE_OBJ_TILE_BASE * 16;
    for (unsigned i = 0; i < sizeof(kTitleObjTiles); i += 2) {
        tdst[i / 2] = (uint16_t)(kTitleObjTiles[i] | (kTitleObjTiles[i + 1] << 8));
    }
    upload_palette_set(PAL_OBJ_TITLE, kRomPalette_obj_title, MEM_PALETTE_OBJ);

    /* The HUD labels, with the header grid's vertical stubs masked out of
     * them; see read_hud_labels in tools/extract_assets.py. */
    vu16 *ldst = MEM_CHARBLOCK(CHARBLOCK) + HUD_LABEL_TILE_BASE * 16;
    for (unsigned i = 0; i < sizeof(kHudLabelTiles); i += 2) {
        ldst[i / 2] = (uint16_t)(kHudLabelTiles[i] | (kHudLabelTiles[i + 1] << 8));
    }

    /* The gameplay sprite set, for the drop-point digits. See PAL_OBJ_GAME. */
    upload_palette_set(PAL_OBJ_GAME, kRomPalette_obj_game, MEM_PALETTE_OBJ);

    /* ...and their glyphs, which are the GAME bank's own '0'-'9'. The tileset
     * is ASCII-indexed (see ascii_tile), so they sit at $30 and copy straight
     * across into sprite tiles of their own. */
    vu16 *ddst = MEM_OBJ_TILES + POINTS_OBJ_TILE_BASE * 16;
    const uint8_t *digits = kGameTiles + ('0' * 32);
    for (unsigned i = 0; i < 10 * 32; i += 2)
        ddst[i / 2] = (uint16_t)(digits[i] | (digits[i + 1] << 8));

    /* The spire's rim, out of the title's own tile set, and its colours out
     * of the title's BACKGROUND palette — bank 2 of it is the cathedral's.
     * See draw_spire_rim. */
    for (int t = 0; t < SCREEN_TITLE_SPIRE_COUNT; t++) {
        vu16 *dst2 = MEM_OBJ_TILES + (SPIRE_RIM_TILE + t) * 16;
        const uint8_t *src2 = kTitleTiles + (kTitleSpire[t].tile * 32);
        for (unsigned i = 0; i < 32; i += 2)
            dst2[i / 2] = (uint16_t)(src2[i] | (src2[i + 1] << 8));
    }
    vu16 *rim_pal = MEM_PALETTE_OBJ + PAL_OBJ_SPIRE * 16;
    for (int i = 0; i < 4; i++)
        rim_pal[i] = nes_colour_to_gba(
            kRomPalette_bg_title[kTitleSpire[0].bank * 4 + i]);

    /* piecePaletteIndexA, "Line clears" (main.asm.txt:5394-5396). */
    const uint8_t *clear = kRomPiecePalettes[10];
    vu16 *cpal = MEM_PALETTE_OBJ + PAL_OBJ_CLEAR * 16;
    cpal[0] = nes_colour_to_gba(TENGEN_BACKDROP_INDEX);
    for (int i = 0; i < 3; i++) cpal[1 + i] = nes_colour_to_gba(clear[i]);
}

void oam_set(int index, int x, int y, uint16_t tile, bool hflip, int bank) {
    vu16 *entry = MEM_OAM + index * 4;
    entry[0] = (uint16_t)OBJ_ATTR0_Y(y);
    entry[1] = (uint16_t)(OBJ_ATTR1_X(x) | (hflip ? OBJ_ATTR1_HFLIP : 0));
    entry[2] = (uint16_t)(tile | OBJ_ATTR2_PAL(bank));
}

void oam_hide_all(void) {
    for (int i = 0; i < 128; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}
