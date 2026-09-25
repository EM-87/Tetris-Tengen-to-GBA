/*
 * frontend.c -- the screens either side of a match.
 *
 * The title and its fireworks, GAME SELECT, LEVEL SETTINGS, the
 * credits, the link lobby, and the music the front end plays. What
 * they DO with a button belongs to main.c; this is what they look
 * like and what they sound like.
 */
#include "port.h"

static const uint8_t kMusicTracks[MUSIC_COUNT] = {
    NES_MUSIC_SILENCE, NES_MUSIC_LOGINSKA, NES_MUSIC_BRADINSKY,
    NES_MUSIC_KARINKA, NES_MUSIC_TROIKA
};
static bool g_music_unlocked;

/* THE ONE CHORD, AND IT IS ON THE MENUS.
 *
 * L+R together — the two buttons a NES pad never had, so the game proper can
 * never see it — uncovers BOTH of the port's own extras at once: the hidden
 * tunes (and the mix that plays them all) and the pause menu. It is rung on
 * GAME SELECT or on LEVEL SETTINGS, which is where a player is already
 * choosing things, and NOT during a match: in play the same chord swaps the
 * HUD, and a chord that means two things depending on whether the plaque is
 * up is a chord nobody can remember. It used to be rung on the pause plaque,
 * which meant starting a game before you could ask for the menu that lets you
 * leave one.
 *
 * A ONE-WAY DOOR, and it stays open until the console is switched off: a
 * thing you discover is not a thing you should have to remember to do at the
 * start of every game. The high scores are the only thing that outlives a
 * power cycle.
 *
 * The title's skin is NOT behind this one. See the note where it is swapped:
 * it is its own chord on its own screen and opens nothing else. */
bool g_pause_unlocked;

/* THE THIRD THING THE CHORD OPENS. See TENGEN_MAX_LEVEL_XE in
 * src/tengen_core.h: levels 18 and 19, which is the whole of Tetris Tengen
 * XE once its ten records are decoded. It rides the same chord because it is
 * the same kind of thing — something the cartridge can do and does not offer
 * — and because it costs a player who never goes past level 17 nothing at
 * all: the mod's tables are byte-identical to the cartridge's up to there. */
bool g_xe;

bool unlock_cheats(void) {
    if (g_music_unlocked && g_pause_unlocked && g_xe) return false;
    g_music_unlocked = true;
    g_pause_unlocked = true;
    g_xe = true;
    set_credit_colour();
    nes_audio_play(NES_SOUND_CHIRP);
    return true;
}

uint8_t start_level_choices(void) {
    return (uint8_t)(g_xe ? START_LEVEL_COUNT_XE : START_LEVEL_COUNT);
}

const char *const kMusicNames[MUSIC_UNLOCKED_COUNT] = {
    "NO MUSIC", "LOGINSKA", "BRADINSKY", "KARINKA", "TROIKA", "KOROBEINIKI",
    "KATIUSKA", "MUSIC MIX"
};

/* The rotation: the cartridge's four and the two hand-entered ones, which
 * have earned their place in it by the time anyone has found this. */
/* KOROBEINIKI FIRST. The mix is only on offer to somebody who found the code,
 * so the tune the code is really about opens the first level, and the rest
 * follow it. */
const uint8_t kMixOrder[MIX_COUNT] = {
    MUSIC_KOROBEINIKI, 1, 2, MUSIC_KATIUSKA, 3, 4
};
uint8_t g_mix_step;

uint8_t mix_tune(void) { return kMixOrder[g_mix_step % MIX_COUNT]; }


uint8_t music_choices(void) {
    return (uint8_t)(g_music_unlocked ? MUSIC_UNLOCKED_COUNT : MUSIC_COUNT);
}

/* One frame of sound, both engines. The cartridge's runs on every frame
 * whatever is playing, because the EFFECTS are always its; the hand-entered
 * tune does nothing unless it is the one chosen. */
void audio_frame(void) {
    nes_audio_frame();
    handtune_frame();
    nes_audio_effect_frame();
}

void stop_music_class(uint8_t klass) {
    nes_rom_call(NES_AUDIO_STOP_ADDR, klass, NES_AUDIO_STOP_STEPS);
}

void stop_music(void) {
    handtune_stop();
    stop_music_class(NES_MUSIC_CLASS_TITLE);
    stop_music_class(NES_MUSIC_CLASS_GAME);
}

/* Starts whichever tune is chosen, on whichever engine owns it. The two never
 * play at once: the cartridge's is suspended for the hand-entered one and
 * keeps running underneath, so the sound EFFECTS are the ROM's either way.
 *
 * SILENCE FIRST, ALWAYS. This is `LA035` (main.asm.txt:4730-4735), which is
 * the cartridge's own way of starting a tune:
 *
 *     lda #MUSIC_SILENCE / jsr setMusicOrSoundEffect
 *     ldy menuMusic / lda musicSelectTable,y / jmp setMusicOrSoundEffect
 *
 * and it is not decoration. setMusicOrSoundEffect only QUEUES a request
 * ($0200-$0207, a ring with its indices at $0208/$0209); handing the engine a
 * new track without silencing the old one leaves the old one's channels
 * running underneath.
 *
 * AND RESUME COMES LAST. updateAudio takes exactly ONE request off that ring
 * per frame ($CFCC-$CFDB), so the order requests are queued in is the order
 * they are heard in, a frame apart. Resuming first — which is what this did —
 * hands the suspended track a frame or two of the speaker before the silence
 * that was meant to replace it arrives: the title theme turning up under the
 * tune you are choosing, and worse if the ring is busy enough to DROP the
 * silence ($CFC3 drops on full). Loading the new track while the engine is
 * still frozen and only then letting it go has no such window, and a tune
 * that is no tune (NO MUSIC) simply never lets it go at all. */
void start_music(uint8_t music) {
    if (music == MUSIC_MIX) music = mix_tune();
    if (MUSIC_IS_HANDTUNE(music)) {
        stop_music();            /* the cartridge's engine steps aside */
        handtune_start(MUSIC_HANDTUNE_OF(music));
        return;
    }
    handtune_stop();
    uint8_t track = kMusicTracks[music < MUSIC_COUNT ? music : 0];
    if (track == NES_MUSIC_SILENCE) {
        /* musicSelectTable's first entry is no tune at all. */
        stop_music();
        return;
    }
    /* The title theme's class first, or the tune below cannot take the slots
     * off it — see stop_music_class. Free even when nothing is playing: it is
     * a walk of eleven bytes. */
    stop_music_class(NES_MUSIC_CLASS_TITLE);
    nes_audio_play(NES_MUSIC_SILENCE);
    nes_audio_play(track);
}
static const char *const kGameNames[GAME_COUNT] = {
    "1 PLAYER", "2 PLAYER", "COOPERATIVE", "VERSUS COMPUTER", "WITH COMPUTER"
};

/* Set whenever the maps are wiped, so the title knows its tiles are gone. */
static bool g_title_dirty = true;

void clear_screen(void) {
    title_window(false);
    bool was = g_panel_layer;
    g_panel_layer = false;
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < MAP_W; tx++) {
            set_map_tile(tx, ty, T_BLANK);
            set_stats_tile(tx, ty, T_BLANK);
        }
    /* ...and the counters' layer with them, or a menu reached from a game
     * would have its panel still hanging over it. The histogram's goes the
     * same way. */
    clear_panel_region(0, 0, MAP_W, 32);
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < MAP_W; tx++) set_histogram_tile(tx, ty, T_BLANK);
    g_panel_layer = was;
    g_title_dirty = true;
}

/* No two screens are ever up at once, so whichever prototype is showing puts
 * its palettes in the title's own four banks rather than asking for four
 * more. */
void install_title_palette(void) {
#if SCREEN_PROTO_AVAILABLE
    upload_palette_set(PAL_TITLE_BASE,
                        g_title_skin ? kRomPalette_bg_proto[g_title_skin - 1]
                                     : kRomPalette_bg_title,
                        MEM_PALETTE);
#else
    upload_palette_set(PAL_TITLE_BASE, kRomPalette_bg_title, MEM_PALETTE);
#endif
}

/* ONCE PER VISIT, not once per frame. The title's tiles never change while it
 * is up — the cathedral and the fireworks on top of them are sprites — and
 * since the two words moved to the offset layer this writes 1200 map entries,
 * which on top of the cartridge's own code running under it was enough to
 * miss a vblank every sixty frames. clear_screen arms it again, and every
 * path that reaches the title goes through one. */
void draw_title(void) {
    if (!g_title_dirty) return;
    g_title_dirty = false;
    /* THE TITLE TAKES ITS FOUR BANKS BACK, every visit. Three of them are on
     * loan to a skin while the front end is up — the menu logo's, the
     * histogram's and the plaque's, all borrowed from here because no title
     * and no board are ever on screen at once — and this used to run only
     * when the skin was SWAPPED. So coming back from the menus with a skin on
     * drew the title in the colours the menus had left: the grey frame of the
     * Nintendo-licensed screen came back with a blue top and a blue right
     * side. See apply_skin, which reloads the loans for the same reason. */
    install_title_palette();
#if SCREEN_PROTO_AVAILABLE
    if (g_title_skin) {
        /* Thirty columns, no padding: the bordered prototypes' frames are two
         * columns a side — a thin outer rule and the fret inside it — and the
         * two the GBA lacks come off the rule, so the decoration survives
         * whole. The unbordered one has empty columns there to spare. */
        const uint8_t *tiles = kScreenProtoTiles[g_title_skin - 1];
        const uint8_t *banks = kScreenProtoPalettes[g_title_skin - 1];
        for (int ty = 0; ty < SCREEN_PROTO_H_TILES; ty++) {
            for (int tx = 0; tx < SCREEN_PROTO_W; tx++) {
                int i = ty * SCREEN_PROTO_W + tx;
                set_map_tile(tx, ty,
                              WITH_BANK(PROTO_TILE_BASE + tiles[i],
                                        PAL_TITLE_BASE + banks[i]));
            }
        }
        return;
    }
#endif
    /* Centred: the composition is 28 columns wide (SCREEN_TITLE_W, which
     * tools/extract_assets.py cuts from the cartridge's 32 — the brick
     * border's jewels are a two-column motif and half of one is worse than
     * none), so it sits one column in from each edge. */
    const int pad = (SCREEN_TW - SCREEN_TITLE_W) / 2;
    for (int ty = 0; ty < SCREEN_TITLE_H_TILES; ty++) {
        for (int tx = 0; tx < SCREEN_TITLE_W; tx++) {
            int i = ty * SCREEN_TITLE_W + tx;
            uint16_t tile = TITLE_TILE_BASE + kScreenTitleTiles[i];
            uint16_t entry =
                WITH_BANK(tile, PAL_TITLE_BASE + kScreenTitlePalettes[i]);
            /* TENGEN RIDES THE OFFSET LAYER; see TITLE_LOGO_SHIFT_PX.
             * Everything else — the frame, the TETRIS logo and the spire that
             * comes out of it — stays on the main one. */
            bool shifted = ty >= TITLE_LOGO_TY0 && ty <= TITLE_LOGO_TY1 &&
                            tx >= TITLE_LOGO_TX0 && tx <= TITLE_LOGO_TX1;
            set_map_tile(pad + tx, ty, shifted ? T_BLANK : entry);
            set_stats_tile(pad + tx, ty, shifted ? entry : T_BLANK);
        }
    }
}
/* Columns go through their own map for the same reason rows do: the two the
 * composition drops come out of the middle, so a sprite right of the gap is
 * two columns left of where its NES x says. */

uint16_t g_title_frame;

/* Once, at boot: resetContinued seeds the RNG here and clears the page
 * (main.asm.txt:5703-5712). Zero is exactly the state the fireworks' first
 * frame expects — it finds a null script pointer, parks all 45 sprites
 * offscreen and schedules the first burst. The seed is deliberately NOT
 * touched again: it is the sound engine's too. */
void init_title_sprites(void) {
    uint8_t *ram = nes_rom_ram();
    ram[NES_RAM_RNG_SEED] = NES_RAM_RNG_SEED_VALUE;
    for (int i = 0; i < 0x100; i++) ram[NES_RAM_OAM_STAGING + i] = 0;
    g_title_frame = 0;
}

void restart_title_sprites(void);

/* Every time the title screen starts, which is what initializeTitleScreen
 * does with the frame counter (main.asm.txt:4483-4485) — and it matters:
 * the show is over once frameCounterHigh reaches 4, so without this it would
 * play only on the very first visit. The script pointer and the burst timer
 * are left alone, because the cartridge leaves them alone too.
 *
 * The cathedral is staged HERE rather than per frame. The cartridge re-runs
 * it every frame because its NMI rebuilds the whole OAM page every frame;
 * this port does not, and the routine's only inputs are a constant table and
 * ppuScrollYOffset — which only the title's hidden both-Downs scroll changes
 * (main.asm.txt:4470-4476) and this port has no scroll. Its eighteen sprites
 * are therefore the same eighteen bytes every frame, and interpreting ~500
 * 6502 instructions to arrive at them again was costing about one frame in
 * fifty-five. Nothing else writes staging entries 0-17: the fireworks' own
 * loops all start at $4C, entry 19. */
void restart_title_sprites(void) {
    g_title_frame = 0;
    nes_rom_ram()[NES_RAM_GAMESTATE] = NES_GAMESTATE_TITLE;
    nes_rom_call(NES_CATHEDRAL_ADDR, 0, 8000);
}

/* The nearest row the composition kept, for a NES row it may have dropped. */
static int title_row_near(int nrow) {
    for (int d = 0; d < 32; d++) {
        if (nrow - d >= 0 && kTitleRowMap[nrow - d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleRowMap[nrow - d];
        if (nrow + d < 30 && kTitleRowMap[nrow + d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleRowMap[nrow + d];
    }
    return -1;
}

static int title_col_near(int ncol) {
    for (int d = 0; d < 34; d++) {
        if (ncol - d >= 0 && kTitleColMap[ncol - d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleColMap[ncol - d];
        if (ncol + d < 32 && kTitleColMap[ncol + d] != SCREEN_TITLE_ROW_DROPPED)
            return kTitleColMap[ncol + d];
    }
    return -1;
}

/* THE SPIRE'S TIP, AS OBJECTS — the finial, the ball, and the ball's lit left
 * rim. See TITLE_SPIRE_SPRITES in tools/extract_assets.py for the whole of
 * why: printed into the logo, each of the three costs whatever ink its cell
 * held, and between them they were taking the second T's bottom serif and
 * three pixels of two more. As objects they cost nothing, they bring their
 * own palette — loaded from the title's BACKGROUND set, so the tip is the
 * same gold as the rest of the cathedral rather than near it — and the pixel
 * they are lowered by closes the gap the ball's blank bottom row left above
 * the tent. */
static void draw_spire_rim(void) {
    for (int i = 0; i < SCREEN_TITLE_SPIRE_COUNT; i++)
        oam_set(SPIRE_RIM_OAM + i, kTitleSpire[i].x, kTitleSpire[i].y,
                 (uint16_t)(SPIRE_RIM_TILE + i), false, PAL_OBJ_SPIRE);
}

void draw_title_sprites(void) {
    /* The release's title only: the prototypes' frames are a thin fret at
     * the screen's edge, with no brick columns for a spark to show through,
     * and their sky runs right out to it. See title_window. */
    title_window(g_title_skin == 0);
    /* THE TITLE'S CLOCK TICKS WHATEVER THE TITLE IS WEARING, and it used to
     * live three lines below this — inside the part a skin returns early
     * from. So on a prototype's screen g_title_frame never moved, and the
     * attract demo, which is nothing but that counter reaching
     * DEMO_START_FRAME, simply never came: the skinned title sat there for
     * ever while the release's started playing by itself after twenty-two
     * seconds. The counter is the screen's, not the fireworks'. */
    uint16_t frame = g_title_frame++;
    if (g_title_skin) {          /* see the note above draw_title */
        oam_hide_all();
        return;
    }
    uint8_t *ram = nes_rom_ram();
    ram[NES_RAM_GAMESTATE] = NES_GAMESTATE_TITLE;
    ram[NES_RAM_FRAME_LOW] = (uint8_t)frame;
    ram[NES_RAM_FRAME_HIGH] = (uint8_t)(frame >> 8);

    /* The step limit is a hang guard, not timing: the fireworks' worst frame
     * rewrites all 45 of their sprites twice over. */
    nes_rom_call(NES_FIREWORKS_ADDR, 0, 12000);

    const uint8_t *oam = ram + NES_RAM_OAM_STAGING;
    const int pad = (SCREEN_TW - SCREEN_TITLE_W) / 2;

    /* The burst's single offset, from the middle of its bounding box. */
    int fw_dx = 0, fw_dy = 0;
    bool fw_placed = false;
    {
        int x0 = 256, x1 = -1, y0 = 240, y1 = -1;
        for (int i = TITLE_FIREWORK_FIRST; i < TITLE_OAM_COUNT; i++) {
            int ny = oam[i * 4], nx = oam[i * 4 + 3];
            if (ny >= 240) continue;          /* parked, see below */
            if (nx < x0) x0 = nx;
            if (nx > x1) x1 = nx;
            if (ny < y0) y0 = ny;
            if (ny > y1) y1 = ny;
        }
        if (y1 >= 0) {
            int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            int col = title_col_near(cx / 8), row = title_row_near(cy / 8);
            if (col >= 0 && row >= 0) {
                fw_dx = (pad + col) * 8 + (cx & 7) - cx;
                fw_dy = row * 8 + (cy & 7) - cy;
                /* NOT CLAMPED. It was, one radius inside the frame, after a
                 * burst was seen printed over the braid — which the cartridge
                 * never shows. But the cartridge places them over the braid
                 * all the time; what it does is draw them BEHIND the picture
                 * (TITLE_FIREWORK_PRIO), so the braid covers the part that
                 * crosses it. Moving them inward was the wrong half of that,
                 * and it put every burst near an edge somewhere it is not. */
                fw_placed = true;
            }
        }
    }

    for (int i = 0; i < TITLE_OAM_COUNT; i++) {
        int ny = oam[i * 4];
        uint8_t tile = oam[i * 4 + 1];
        uint8_t attr = oam[i * 4 + 2];
        int nx = oam[i * 4 + 3];
        int x, y;
        /* The NES hides a sprite by parking it below the visible 240 lines;
         * this code uses $F7 for exactly that. */
        if (ny >= 240) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        if (i >= TITLE_FIREWORK_FIRST) {
            if (!fw_placed) { MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN; continue; }
            x = nx + fw_dx;
            y = ny + fw_dy;
            if (x < 0 || x >= SCREEN_TW * 8 || y < 0 || y >= SCREEN_TH * 8) {
                MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
                continue;
            }
            oam_set(i, x, y, (uint16_t)(TITLE_OBJ_TILE_BASE + tile), false,
                     PAL_OBJ_TITLE + (attr & 3));
            /* Bit 5 is the cartridge's "behind the background"; every burst
             * sprite has it. */
            if (attr & 0x20) MEM_OAM[i * 4 + 2] |= OBJ_ATTR2_PRIO(TITLE_FIREWORK_PRIO);
            continue;
        }
        int row = kTitleRowMap[ny / 8];
        if (row == SCREEN_TITLE_ROW_DROPPED) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        int col = (nx >= 0 && nx < 256) ? kTitleColMap[nx / 8]
                                        : SCREEN_TITLE_ROW_DROPPED;
        if (col == SCREEN_TITLE_ROW_DROPPED) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        /* ...and then the same one-column pad draw_title centres with. */
        x = (pad + col) * 8 + (nx & 7);
        if (x >= SCREEN_TW * 8) {
            MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }
        /* Low two bits of the NES attribute byte pick one of the four
         * palettes of the set the title installs, spritePalette1. */
        oam_set(i, x, row * 8 + (ny & 7),
                 (uint16_t)(TITLE_OBJ_TILE_BASE + tile), false,
                 PAL_OBJ_TITLE + (attr & 3));
    }
    /* ...and the cathedral's topmost spire keeps its tip, on the slots past
     * the staging page. See SPIRE_RIM_OAM. */
    draw_spire_rim();
    for (int i = TITLE_OAM_COUNT + SCREEN_TITLE_SPIRE_COUNT; i < 128; i++)
        MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

/* Writes `text` centred in the frame, on whichever layer makes it land on the
 * middle. Both maps are cleared first, so a name that was odd last frame and
 * is even this one leaves nothing behind. */
static void draw_text_lifted(int ty, const char *text, int bank) {
    unsigned len = text_len(text);
    int tx = MENU_IN_TX + ((int)MENU_IN_W - (int)len) / 2;
    for (int x = 0; x < MENU_IN_W; x++) set_histogram_tile(MENU_IN_TX + x, ty, T_BLANK);
    for (unsigned i = 0; i < len; i++)
        set_histogram_tile(tx + (int)i, ty, WITH_BANK(ascii_tile(text[i]), bank));
}

/* ...and the same at a column of the caller's choosing, for a list whose
 * entries line up with each other rather than each with the middle. Always
 * the MAIN layer: the offset layer exists to put an odd-length word on the
 * middle of the screen, and a left-aligned column has no middle to hit —
 * riding it would set every other entry three pixels in from the one above. */
static void draw_text_left(int ty, int tx, const char *text, int bank) {
    unsigned len = text_len(text);
    for (int x = 0; x < MENU_IN_W; x++) {
        set_map_tile(MENU_IN_TX + x, ty, T_BLANK);
        set_stats_tile(MENU_IN_TX + x, ty, T_BLANK);
    }
    for (unsigned i = 0; i < len; i++)
        set_map_tile(tx + (int)i, ty, WITH_BANK(ascii_tile(text[i]), bank));
}

static void draw_text_centred(int ty, const char *text, int bank) {
    unsigned len = text_len(text);
    int tx = MENU_IN_TX + ((int)MENU_IN_W - (int)len) / 2;
    bool offset = (len & 1u) != 0;
    for (int x = 0; x < MENU_IN_W; x++) {
        set_map_tile(MENU_IN_TX + x, ty, T_BLANK);
        set_stats_tile(MENU_IN_TX + x, ty, T_BLANK);
    }
    for (unsigned i = 0; i < len; i++) {
        uint16_t entry = WITH_BANK(ascii_tile(text[i]), bank);
        if (offset) set_stats_tile(tx + (int)i, ty, entry);
        else        set_map_tile(tx + (int)i, ty, entry);
    }
}

static void draw_menu_frame(void) {
    /* THE MENUS WEAR IT TOO. Their frame is the same twenty-four charblock
     * slots the board's is, and leaving them the release's made the chord
     * change half the game. See front_skin. */
    apply_skin(front_skin());
    set_offset_layer(MENU_TEXT_SHIFT_PX);
    for (int ty = 0; ty < SCREEN_MENU_H_TILES; ty++) {
        for (int tx = 0; tx < SCREEN_MENU_W; tx++) {
            int i = ty * SCREEN_MENU_W + tx;
            set_map_tile(tx, ty,
                          WITH_BANK(kScreenMenuTiles[i], PAL_MENU_BASE + kScreenMenuPalettes[i]));
        }
    }
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    /* ...AND THE LOGO OVER THE TOP OF IT, when the skin has one of its own.
     * The same six letters as the HUD banner, laid out the other way, so they
     * come out of the same window. proto_a's menus carry no logo at all and
     * keep the release's rather than a hole. See kSkinLogoHas. */
    int skin = front_skin();
    if (skin >= 0 && kSkinLogoHas[skin]) {
        for (int y = 0; y < SKIN_LOGO_H; y++)
            for (int x = 0; x < SKIN_LOGO_W; x++)
                set_map_tile(SKIN_LOGO_TX + x, SKIN_LOGO_TY + y,
                              WITH_BANK(SKIN_BANNER_BASE +
                                         kSkinLogoTiles[skin][y * SKIN_LOGO_W + x],
                                         SKIN_LOGO_BANK));
    }
#endif
}

/* MIRRORSOFT IS NOT ON THIS LIST, and that is the one departure from the
 * cartridge here. Its line was a licensing notice for a company that has not
 * existed since 1991, and the slot goes to whoever made THIS. The five that
 * remain are the people who made the game. */
static const char *const kCredits[][2] = {
    { "PORTED WITH CLAUDE", "BY EDUARDO MARTINEZ" },
    { "CONCEPT BY",        "ALEXEY PAZHITNOV" },
    { "DESIGN BY",         "VADIM GERASIMOV"  },
    { "PROGRAMMED BY",     "ED LOGG"          },
    { "VIDEO GRAPHICS BY", "KRIS MOSER"       },
    { "AUDIO BY",          "BRAD FULLER"      },
};
/* Here and not in port.h: sizeof needs the table's own definition in sight,
 * and nothing outside this file reads it. */
#define CREDIT_COUNT (sizeof kCredits / sizeof kCredits[0])

static uint8_t g_credit;
static uint16_t g_credit_timer;

/* One frame of it, and the drawing with it: this is called from a screen that
 * redraws itself every frame, so writing the two rows every time costs nothing
 * and needs no dirty flag. */
static void draw_credits(void) {
    if (++g_credit_timer >= CREDIT_FRAMES) {
        g_credit_timer = 0;
        g_credit = (uint8_t)((g_credit + 1) % CREDIT_COUNT);
    }
    set_credit_layer(true);
    draw_text_lifted(CREDIT_TY, kCredits[g_credit][0], BANK_CREDIT);
    draw_text_lifted(CREDIT_TY + 1, kCredits[g_credit][1], BANK_CREDIT);
}
/* THE CHORD SHOWS ON THE MENUS. Everything it uncovers is somewhere else —
 * the tunes a page on, the pause menu in a game — so a player had no way to
 * tell whether it had taken. Once it has, the menus' text turns from the
 * cartridge's blue to the cursor's white, which also reads better on an LCD.
 * The one thing that was white already, the handicap number the pad is
 * moving, takes the blue instead so it still stands out. */
static int menu_bank(void) {
    return g_pause_unlocked ? BANK_ARROW : BANK_MENU;
}
static int menu_lit_bank(void) {
    return g_pause_unlocked ? BANK_MENU : BANK_ARROW;
}

void draw_game_select(uint8_t choice) {
    draw_menu_frame();
    draw_text_centred(8, "GAME SELECT", menu_bank());
    /* FIVE ENTRIES ON CONSECUTIVE ROWS, which is the cartridge's own shape:
     * gameSelectArrowPpuAddrs ($A0AB) is $220A,$222A,$224A,$226A,$228A — five
     * addresses one nametable row apart. Two rows apart was fine for two of
     * them and does not fit five under a logo six rows tall. */
    /* ...FLUSH LEFT, which is the cartridge's: all five are written at
     * nametable column $0C whatever their length. See GAME_SELECT_TX. */
    for (int i = 0; i < GAME_COUNT; i++)
        draw_text_left(GAME_SELECT_TY + i, GAME_SELECT_TX, kGameNames[i],
                        menu_bank());
    /* AND THE ARROW, which this screen had been doing without. The cartridge
     * marks its choice here exactly as it does on LEVEL SELECT — a white
     * arrow in the column left of the list (gameSelectArrowPpuAddrs, $A0AB,
     * five addresses one row apart) — and the port was leaning on colour
     * alone. Two columns clear of the list, exactly as the cartridge has
     * it. */
    for (int ty = GAME_SELECT_TY; ty < GAME_SELECT_TY + GAME_COUNT; ty++)
        set_map_tile(GAME_SELECT_ARROW_TX, ty, T_BLANK);
    set_map_tile(GAME_SELECT_ARROW_TX, GAME_SELECT_TY + choice,
                  WITH_BANK(ascii_tile(MENU_ARROW_R), BANK_ARROW));
    draw_credits();
}

static void draw_guest_dancer(int elapsed) {
    int pose = (elapsed / DANCER_POSE_FRAMES) % DANCER_POSE_COUNT;
    const uint8_t *tiles = kDancerPoses[pose];
    for (int s = 0; s < DANCER_SPRITES; s++) {
        oam_set(s, GUEST_DANCER_X + ((s & 1) ? 8 : 0),
                 GUEST_DANCER_Y + ((s & 2) ? 8 : 0),
                 tiles[s], false, PAL_OBJ_DANCER + (kDancerAttr[0] & 3));
    }
    for (int i = DANCER_SPRITES; i < 128; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

/* THE PORT'S OWN ACCOUNT, one line under the message, for as long as the
 * cable is looking: SIOCNT in hex, then transfers that came back Good, Bad
 * ones and port Resets (each capped at 999 to fit the line). The emulated cable and a real one disagreed once
 * already, and this is what a person holding two consoles can read out. */
static unsigned append_number(char *row, unsigned n, unsigned value);

static void draw_link_debug(int ty) {
    static const char kHex[] = "0123456789ABCDEF";
    uint16_t d[LINK_DEBUG_WORDS];
    link_debug(d);
    char row[32];
    unsigned n;
#define HEX4(v) for (int sh = 12; sh >= 0; sh -= 4) row[n++] = kHex[((v) >> sh) & 0xF]
    /* SIOCNT now, and as the last interrupt found it. */
    n = 0;
    row[n++] = 'S'; row[n++] = 'I'; row[n++] = 'O'; row[n++] = ' ';
    HEX4(d[0]);
    row[n++] = ' '; row[n++] = 'I'; row[n++] = 'R'; row[n++] = 'Q'; row[n++] = ' ';
    HEX4(d[1]);
    row[n] = 0;
    draw_text_centred(ty, row, menu_bank());
    /* Good transfers, Error bits seen, transfers with a slot Absent, port
     * Resets. */
    static const char kTag[4] = { 'G', 'E', 'A', 'R' };
    n = 0;
    for (int i = 0; i < 4; i++) {
        if (i) row[n++] = ' ';
        row[n++] = kTag[i];
        row[n++] = ' ';
        n = append_number(row, n, d[2 + i] > 999 ? 999 : d[2 + i]);
    }
    row[n] = 0;
    draw_text_centred(ty + 1, row, menu_bank());
    /* ...and what the last transfer carried: the master's slot and the
     * slave's. FFFF is a console the transfer did not hear. */
    n = 0;
    row[n++] = 'M'; row[n++] = ' ';
    HEX4(d[6]);
    row[n++] = ' '; row[n++] = 'S'; row[n++] = ' ';
    HEX4(d[7]);
    row[n] = 0;
    draw_text_centred(ty + 2, row, menu_bank());
#undef HEX4
}

/* Which ROM this is — see BUILD_ID in the Makefile — in capitals, the only
 * letters the font has. */
static void draw_build_id(int ty) {
#ifndef BUILD_ID
#define BUILD_ID "DEV"
#endif
    char row[24] = "BUILD ";
    unsigned n = 6;
    for (const char *p = BUILD_ID; *p && n < sizeof(row) - 1; p++)
        if (*p != '+')                       /* no glyph; a local build only */
            row[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    row[n] = 0;
    draw_text_centred(ty, row, BANK_NOTE);
}

/* THE LOBBY SAYS WHERE IT IS, not the cable: a transfer answered by a
 * console that is on its menus is still a transfer, and "connected" by that
 * measure put YOU ARE PLAYER 2 on a master whose partner had left. */
void draw_link_wait(const TengenLobby *lobby, int elapsed) {
    draw_menu_frame();
    draw_text_centred(8, "LINK CABLE", menu_bank());

    clear_both(MENU_IN_TX, 10, MENU_IN_W, 7);
    if (!lobby->linked) {
        oam_hide_all();
        draw_text_centred(10, "WAITING FOR PLAYER 2", menu_bank());
        draw_text_centred(12, "B TO GO BACK", menu_bank());
        draw_build_id(13);
        draw_link_debug(14);
        return;
    }
    if (link_is_master()) {
        /* Chosen, and the handshake is carrying it across. */
        oam_hide_all();
        draw_text_centred(11, "STARTING", BANK_NOTE);
        return;
    }
    /* The guest: the master is choosing, and the cossack does the waiting. */
    draw_text_centred(11, "YOU ARE PLAYER 2", BANK_NOTE);
    draw_guest_dancer(elapsed);
}

/* Appends a number with no leading zeroes, however many digits it has.
 * Returns the new length. (The callers today pass a level and a handicap's
 * rows, two digits at most, but nothing here should depend on that.) */
static unsigned append_number(char *row, unsigned n, unsigned value) {
    char digits[10];
    unsigned k = 0;
    do {
        digits[k++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (k) row[n++] = digits[--k];
    return n;
}

/* One field: the cursor if it is the chosen one, then the label and the value
 * in their columns, and whatever the value trails after it — which is only
 * ever what the handicap costs, and is a note about the value rather than
 * part of it, so it is drawn in the note's colour.
 *
 * ONE STRETCH OF THE VALUE MAY BE LIT, characters [hl, hl+hl_len), and it is
 * drawn in the CURSOR'S white. That is the handicap row in a race, where the
 * value is two numbers and only one of them is the one the pad is moving: the
 * arrow says which LINE you are on and the white says which NUMBER, in the one
 * colour this page already uses to mean exactly that. hl_len of 0 lights
 * nothing, which is every other row. */
static void draw_field_row(int field, int chosen, const char *label,
                            const char *value, const char *tail,
                            int hl, int hl_len) {
    int ty = MENU_FIELD_TY(field);
    clear_both(MENU_IN_TX, ty, MENU_IN_W, 1);
    if (field == chosen)
        set_map_tile(MENU_CURSOR_TX, ty,
                      WITH_BANK(ascii_tile(MENU_ARROW_R), BANK_ARROW));
    for (int i = 0; label[i]; i++)
        set_map_tile(MENU_LABEL_TX + i, ty,
                      WITH_BANK(ascii_tile(label[i]), menu_bank()));
    int tx = MENU_VALUE_TX;
    for (int i = 0; value[i]; i++, tx++) {
        bool lit = hl_len > 0 && i >= hl && i < hl + hl_len;
        set_map_tile(tx, ty, WITH_BANK(ascii_tile(value[i]),
                                        lit ? menu_lit_bank() : menu_bank()));
    }
    if (tail) {
        tx += MENU_TAIL_GAP;
        for (int i = 0; tail[i]; i++, tx++)
            set_map_tile(tx, ty, WITH_BANK(ascii_tile(tail[i]), BANK_NOTE));
    }
}

void draw_level_settings(int chosen, uint8_t start_level, uint8_t music,
                                 const uint8_t handicap[2], bool two_player,
                                 int handicap_who) {
    draw_menu_frame();
    /* draw_menu_frame only repaints BG0; the offset layer keeps whatever the
     * screen before this one left on it. */
    clear_both(MENU_IN_TX, MENU_BODY_TY, MENU_IN_W, MENU_BODY_H);

    char value[16];
    unsigned n;

    n = append_number(value, 0, start_level);
    value[n] = '\0';
    draw_field_row(MENU_FIELD_LEVEL, chosen, "LEVEL", value, NULL, 0, 0);

    /* THE STARTING HANDICAP, the cartridge's own menuPlayer1Handicap /
     * menuPlayer2Handicap (main.asm.txt:3536-3546): how many three-row bands
     * of garbage a player starts buried under, nought to four. In two players
     * there are two of them, and the shoulder on a player's side of the pad
     * is what sets that player's.
     *
     * WHAT IT COSTS RIDES THE SAME LINE, because "2" says nothing until you
     * know it is two of the bands garbageHeightData lays down. The word
     * BURIES is what gets dropped to make it fit: "ROWS" beside the value
     * says the same thing in four columns.
     *
     * AND THE VALUE IS THE CARTRIDGE'S OWN NUMBER, not the step. Its handicap
     * screen offers 0, 3, 6, 9 and 12 ($A280's patch chain writes exactly
     * those five at column 15) — the ROWS, which is what the setting means.
     * This showed the STEP, nought to four, and kept the rows in a note
     * beside it; the step is an implementation detail of
     * `garbageHeightData`'s four entries and not something the cartridge ever
     * puts on screen. The internals still count in steps, because the ROM
     * does; only the display changed. */
    n = append_number(value, 0,
                       (unsigned)handicap[0] * TENGEN_HANDICAP_ROWS_PER_STEP);
    /* WHICH OF THE TWO NUMBERS THE PAD IS ON, in characters of the value, so
     * the white lands on the one being moved however many digits the other
     * takes. In one player there is only the one and nothing is lit. */
    int hl = 0, hl_len = 0;
    if (two_player) {
        unsigned first = n;
        value[n++] = ' ';
        unsigned second = n;
        n = append_number(value, n,
                           (unsigned)handicap[1] * TENGEN_HANDICAP_ROWS_PER_STEP);
        hl = handicap_who ? (int)second : 0;
        hl_len = handicap_who ? (int)(n - second) : (int)first;
    }
    value[n] = '\0';
    /* The unit, and only while the cursor is on the field: it is there to
     * answer the question you are asking, and the rest of the time it is one
     * more thing on the page. */
    const char *unit = chosen == MENU_FIELD_HANDICAP ? "ROWS" : NULL;
    draw_field_row(MENU_FIELD_HANDICAP, chosen, "HANDICAP", value, unit,
                    hl, hl_len);

    draw_field_row(MENU_FIELD_MUSIC, chosen, "MUSIC", kMusicNames[music],
                    NULL, 0, 0);

    /* THE LINE UNDER IT IS GONE WITH THE STEPS. It used to spell out "BURIES
     * 3 AND 6 ROWS", because "1 2" said nothing; now the row itself reads
     * "HANDICAP  3 6  ROWS" and repeating that under it would be saying the
     * same thing twice. HANDICAP still keeps the row below it free — see
     * MENU_FIELD_TY — so nothing else moved. */

    draw_text_centred(MENU_FOOT_TY, "PRESS START TO PLAY", BANK_NOTE);
}
