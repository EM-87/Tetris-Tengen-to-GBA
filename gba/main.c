/*
 * main.c -- the state machine.
 *
 * Which screen is up, what a button does on it, and where it goes next.
 * All the rules live in ../src/tengen_core.c and know nothing about the
 * GBA; all the drawing lives in the other four files. This one is the
 * loop that calls both.
 */
#include "port.h"


#ifdef TENGEN_MULTIBOOT
/* The image as it landed in external WRAM: its first sixteen bytes are the
 * header the sender gave it (wireless_multiboot_send patches them). */
static bool booted_over_air(void) {
    static const char kTag[9] = { 'R', 'F', 'U', '-', 'M', 'B', 'O', 'O', 'T' };
    const volatile uint8_t *at = (const volatile uint8_t *)0x02000004;
    for (int i = 0; i < 9; i++)
        if (at[i] != (uint8_t)kTag[i]) return false;
    return true;
}
#endif

/* One frame of the send over the air: the screen, the sound, and B. */
static bool air_send_frame(int stage, uint32_t done, uint32_t total) {
    vsync();
    draw_air_sending(stage, done, total);
    audio_frame();
    return (read_buttons() & TENGEN_BTN_B) != 0;
}

/* THE DEMO'S MUSIC MIX (under the chord) borrows g_music, which is also
 * what LEVEL SETTINGS shows as the player's choice: it goes back as the
 * demo ends, either way out. */
static bool g_demo_mix;
static uint8_t g_demo_music_kept;
static void demo_music_back(void) {
    if (!g_demo_mix) return;
    g_music = g_demo_music_kept;
    g_demo_mix = false;
}

/* ...and GAME SELECT's line, which the demo two titles share takes for its
 * own game (COOPERATIVE) while it plays: given back with the tune. */
static int g_demo_mode_kept = -1;
static void demo_mode_back(uint8_t *game_mode) {
    if (g_demo_mode_kept < 0) return;
    *game_mode = (uint8_t)g_demo_mode_kept;
    g_demo_mode_kept = -1;
}

#ifndef TENGEN_MULTIBOOT
/* WHAT THE MENUS CHOSE, for a game going to the battery (suspend.c): the
 * plaque's suspend and EXIT GAME's SAVE keep the same. */
static SuspendMenu kept_menu(uint8_t game_mode, uint8_t start_level,
                             const uint8_t *handicap) {
    SuspendMenu m = {
        .game_mode = game_mode, .start_level = start_level,
        .music = g_music,
        .handicap = { handicap[0], handicap[1] },
        .title_skin = g_title_skin,
        .unlocked = g_pause_unlocked, .mix_step = g_mix_step,
        .hud = g_hud, .idle_palette = g_idle_palette,
        .ai_last_piece = g_ai_last_piece,
        .ai_last_partner = g_ai_last_partner,
        .ai_frame = g_ai_frame,
    };
    return m;
}
#endif

int main(void) {
    /* WHAT THIS CARTRIDGE CAN KEEP A SAVE IN, asked before anything reads
     * one: its save RAM, or on a board with no battery its own flash too
     * (cartsave.h). Before the interrupts as well — while the chip is
     * being asked it is not a ROM, and nothing of the cartridge's may run. */
    cartsave_init();
    /* Every screen from here on waits for its frame in vsync(), and
     * vsync() sleeps until an interrupt that this is what switches on. */
    irq_init();
    /* A Wireless Adapter in the port, asked once (wireless.c): if one
     * answers, 2 PLAYER and COOPERATIVE go over the air. The Single-Pak copy
     * asks only if it came over the air itself — the adapter's loader wants
     * RFU-MBOOT in bytes 4-15 of what it boots, so the copy finds those in
     * its own image — and never if it came over a cable, where the login's
     * trip through general purpose would be a flutter on the sender's lobby. */
#ifdef TENGEN_MULTIBOOT
    wireless_client_only();
    if (booted_over_air()) wireless_detect();
#else
    wireless_detect();
#endif
    /* ...and otherwise the cable answers from the start, wherever the
     * player is: see link_rest. */
    link_rest();
    /* THE CONSOLE'S SETTINGS before the first thing is drawn or heard: the
     * screen's curve the splash's colours go through, and the volumes. See
     * g_settings (extras.c). */
    settings_load();
    lifetime_load();
    /* The publisher's logo, at power-on only; see splash.c. Not on the way
     * to the page behind L+R+A, which is asked for to be read at once
     * (cartsave_page, below). */
    if (!cartsave_page_asked()) splash_show();
    /* A Game Boy Player answered its logo — now, or at power-on before a
     * soft reset: the serial port is its, for the rumble (gbp.c). */
    gbp_start();

    upload_tiles();
    upload_palettes();
    /* What the battery kept, or the cartridge's cold-boot table if there is
     * nothing there to keep. See leader_load. */
    leader_reset();
    leader_load();
    options_load();
    set_field_palette_for_level(0);
    clear_screen();

    upload_title_tiles();
    upload_sprite_tiles();
    oam_hide_all();
    nes_audio_init();
    /* After nes_audio_init: this runs the cartridge's code, and the machine it
     * runs on is the sound engine's. */
    init_title_sprites();
    restart_title_sprites();

    REG_BG0CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK) | BG_PRIORITY(1);
    /* The statistics layer, three pixels to the right of the tile grid. A
     * negative scroll is what moves the picture the other way, and the field
     * is nine bits wide, so -3 is written as 512-3. */
    REG_BG1CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK_STATS) | BG_PRIORITY(0);
    set_offset_layer(STATS_SHIFT_PX);
    REG_BG1VOFS = 0;
    /* The counters' layer, two pixels below the tile grid and nothing else on
     * it — so its scroll is set once and never touched again. It shares the
     * offset layer's priority; the two never write the same cell, and where
     * priorities tie the lower-numbered background wins in any case. */
    REG_BG2CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK_PANEL) | BG_PRIORITY(0);
    REG_BG2HOFS = 0;
    REG_BG2VOFS = (uint16_t)((512 - PANEL_SHIFT_PX) & 511);
    /* The histogram's layer: the offset layer's three pixels across, and none
     * of the counters' two down. See SCREENBLOCK_HISTOGRAM. */
    REG_BG3CNT = BG_4BPP | BG_SIZE_32x32 | BG_CHARBLOCK(CHARBLOCK) |
                  BG_SCREENBLOCK(SCREENBLOCK_HISTOGRAM) | BG_PRIORITY(0);
    REG_BG3HOFS = (uint16_t)(512 - STATS_SHIFT_PX);
    REG_BG3VOFS = STATS_LIFT_PX;
    REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1 | DCNT_BG2 | DCNT_BG3 |
                   DCNT_OBJ | DCNT_OBJ_1D;
    /* L+R+A HELD AT POWER-ON: what this cartridge turned out to be and
     * where its save is going, before the title (cartsave_page). */
    if (cartsave_page_asked()) cartsave_page();

    Screen screen = SCREEN_TITLE;
    int leader_frames = 0;
    /* The page came off the title's SELECT, so LEFT and RIGHT walk the
     * tables (leader_browse); after a game it is that game's table. */
    bool leader_browsing = false;
    /* ...and the game it follows put nobody on it from here, under the
     * chord: it holds LEADER_NOTHING_NEW_FRAMES. */
    bool leader_nothing_new = false;
    int over_frames = 0;
    uint8_t start_level = 0;
    /* menuPlayer1Handicap / menuPlayer2Handicap ($04F3-$04F4). */
    uint8_t handicap[2] = { 0, 0 };
    /* ...and which of the two the pad is setting, in a race. SELECT swaps it
     * and the value's white says which. See the note by the cursor keys. */
    int handicap_who = 0;
    uint8_t game_mode = GAME_1P;
    /* Which of the three settings the cursor is on. */
    int menu_field = MENU_FIELD_LEVEL;
    /* ...and which of the three pages, on a Game Boy Player (draw_tv_page). */
    int tv_page = TV_PAGE_LEVEL;
    int link_wait_frames = 0;
    /* The Single-Pak send that starts by itself; see the LINK CABLE screen. */
#define AUTO_SEND_FRAMES 45
    /* How long RECORDS SAVED stays up before the match. */
#define RECORDS_SAVED_FRAMES 120
    int auto_send_wait = 0;
#ifndef TENGEN_MULTIBOOT
    /* The adapter asked for again from the cable's screen; see there. */
#define RADIO_FIRST_TRY 180
#define RADIO_RETRY 300
    int radio_wait = 0;
#endif
    bool auto_send_refused = false;
#define AUTO_SEND_GONE_FRAMES 120
    int auto_send_gone = 0;
#ifndef TENGEN_MULTIBOOT
    /* The other cartridge's records this player has been asked about
     * (TengenRecordSync.partner_digest): not asked again for the same. */
    bool records_answered = false, records_took = false;
    uint16_t records_answered_for = 0;
#endif
    /* TWO TITLES, ONE DEMO (the title, below; link.c, link_title_tick).
     * How long a title has to have stood untouched before the other
     * console's demo may take it along; how long the slave's end, its own
     * demo due, waits to be asked; and how long a handshake gets. The last
     * two are a real cable's: in the emulator the master has asked within
     * two beacons — sixteen frames; a title's word is on the wire a
     * transfer after it changes — and the lobby is through in fifteen, and
     * a real one drops a transfer now and then and takes its time over a
     * port started afresh: a second to be asked in, three to shake hands. */
#define TITLE_IDLE_FRAMES 300
#define DEMO_LINK_WAIT 60
#define DEMO_LINK_FRAMES 180
#define SKY_HOLD_FRAMES 20
    bool demo_link = false;         /* the two titles are shaking hands */
    int demo_link_frames = 0;
    bool demo_waiting = false;      /* the slave's end, its demo due: to be asked */
    uint8_t sky = TITLE_SKY_NIGHT, sky_dark = 0;    /* the title's (title_sky) */
    uint8_t held_last = 0;
    bool sweeping = false;   /* true while the line-clear sweep owns the OAM */
    bool match_running = false;
    bool gs_options = false;     /* GAME SELECT's cursor on OPTIONS */
    TengenLobby lobby;
    /* THE CABLE OUTLIVES THE MATCH BY A FEW SECONDS. Lockstep gives both
     * consoles the rival's score and lines for nothing; the one thing it
     * cannot give is the name the other player typed, so the wire stays up
     * from the last piece to the bottom of the HIGH SCORES page while the
     * two exchange three letters each. See TengenNameSwap. */
    TengenNameSwap name_swap;
    bool swapping = false;
    bool swap_sent = false;
    bool swap_landed = false;   /* the rival's letters are in the table */
    bool swap_drawn = false;    /* ...and on the screen */

    /* The ROM steps its RNG once per frame from the main loop
     * (main.asm.txt:49-50), and whatever state it is in when Start is pressed
     * becomes the game's seed. Doing the same means the piece sequence
     * depends on when you start rather than being identical every session. */
    TengenRng seed_source;
    tengen_rng_seed(&seed_source, 0xACE1);

#ifdef TENGEN_MULTIBOOT
    /* THE SINGLE-PAK SLAVE WAKES UP ON THE CABLE. It came over it, from a
     * console that is sitting in its own lobby waiting for it, so it goes
     * straight there and plays whichever of the two linked games that
     * console chose (tengen_lobby_mode_any). B still leaves for GAME
     * SELECT: the whole game came across, and it can be played alone. */
    {
        link_init();
        link_lobby_start_held(&lobby, 0);
        uint16_t prints[TENGEN_SKIN_MAX];
        int count = skin_prints(prints, TENGEN_SKIN_MAX);
        tengen_lobby_skins(&lobby, prints, count, -1);
        tengen_lobby_mode_any(&lobby);
        game_mode = GAME_2P;
        screen = SCREEN_LINK_WAIT;
        oam_hide_all();
        clear_screen();
    }
#endif

#ifndef TENGEN_MULTIBOOT
    /* A PAUSED GAME LEFT ON THE BATTERY (suspend.c) comes back: the menu's
     * choices first, then LEVEL SETTINGS confirms itself (`resuming`), the
     * match starts as it always does, and the game is put back over it. */
    SuspendMenu resume = { 0 };
    bool resuming = suspend_load(&resume);
    if (resuming) {
        game_mode = resume.game_mode;
        start_level = resume.start_level;
        g_music = resume.music;
        handicap[0] = resume.handicap[0];
        handicap[1] = resume.handicap[1];
        g_title_skin = resume.title_skin;
        if (resume.unlocked) cheats_restore();
        screen = SCREEN_LEVEL_SELECT;
    }
    bool suspended = false;          /* the battery holds this match */
    uint8_t suspended_music = 0;
#else
    const bool resuming = false;
#endif

    for (;;) {
        /* Asleep from any screen but a match in play; see system_sleep. */
        g_sleep_blocked = screen == SCREEN_PLAYING && match_running;
        /* UNDER THE CHORD, the cable says this console is on its title and
         * how long it has been (link_title_tick): two of them start their
         * demos as one game. Without the chord this console says 0 as it
         * always has, hears nothing, and its demo is the cartridge's.
         * BEFORE THE BEACON, which carries the word this decides on: the
         * frame after a title is left, a beacon first would still say it. */
        {
            int title = -1;
            if (screen == SCREEN_TITLE && g_pause_unlocked && !demo_link) {
                title = 0;
                if (g_title_frame >= TITLE_IDLE_FRAMES) title |= LINK_TITLE_IDLE;
                if (g_title_frame >= DEMO_START_FRAME) title |= LINK_TITLE_DUE;
            }
            link_title_tick(title);
        }
        /* On the menus, the cable says this console is there (link_beacon):
         * a console waiting at the other end then knows to wait. */
        if (screen == SCREEN_TITLE || screen == SCREEN_GAME_SELECT ||
            screen == SCREEN_LEADERBOARD ||
            (screen == SCREEN_LEVEL_SELECT && !GAME_IS_LINKED(game_mode)))
            link_beacon();
        uint8_t buttons = read_buttons();
        uint8_t pressed = (uint8_t)(buttons & ~held_last);
        held_last = buttons;
        tengen_rng_step(&seed_source);
        /* The lifetime figures to the battery, when the game is still (see
         * lifetime_tick). */
        lifetime_tick(screen != SCREEN_PLAYING || !match_running ||
                      g_session.game.paused);

        /* THE SWAP RUNS UNDER WHATEVER IS ON THE SCREEN, which is why it is
         * up here and not in one screen's arm of the loop: the plaque comes
         * first, then the table, and each console reaches them in its own
         * time because one player sits on the game over and the other does
         * not. Starting it the moment the match ends, rather than when the
         * page comes up, is what keeps either end from waiting on the
         * other's button.
         *
         * This console's own three letters go on the wire as soon as there
         * is nothing left to type — which for a player who did not make this
         * table is at once, with the letters an untyped row carries. See
         * TengenNameSwap for why that case exists at all. */
        if (swapping) {
            if (screen == SCREEN_TITLE) {
                /* Every road out of the table ends here, and a cable left
                 * live under the title is one the next lobby would find
                 * halfway through somebody else's conversation. A rival still
                 * waiting sees this as silence and gives up on its own clock,
                 * keeping whatever letters did arrive. */
                link_shutdown();
                swapping = false;
            } else {
                /* NOT BEFORE THE TABLE IS UP. `g_leader_row` is -1 from
                 * the moment the match ends until leader_submit runs, so a
                 * send gated on that alone went out on the game-over plaque
                 * with the letters of a row nobody had been offered yet —
                 * and the rival's row came up AAA however carefully its
                 * owner typed. The page is where there is a name to send. */
                if (!swap_sent && screen == SCREEN_LEADERBOARD &&
                     g_leader_row < 0) {
                    uint8_t mine[LEADER_INITIALS];
                    leader_own_initials(mine);
                    tengen_name_send(&name_swap, mine);
                    swap_sent = true;
                }
                link_name_step(&name_swap);
                /* The row is filled in as soon as the three letters are here;
                 * DRAWING it waits for the page's own vertical blank below,
                 * which is the only place this file writes video memory. */
                if (tengen_name_have(&name_swap) && !swap_landed) {
                    leader_rival_initials(name_swap.theirs);
                    swap_landed = true;
                }
                if (name_swap.complete || name_swap.failed) {
                    link_shutdown();
                    swapping = false;
                }
            }
        }

        if (screen == SCREEN_TITLE) {
            /* initializeTitleScreen ends with this (main.asm.txt:4489). */
            front_music(FRONT_TITLE_THEME);
            set_offset_layer(TITLE_LOGO_SHIFT_PX);
            /* L+R HERE is the game's own chord as much as on GAME SELECT or
             * LEVEL SETTINGS (unlock_cheats): rung here it opens the tunes,
             * the pause menu and the rest — AND THAT IS ALL IT DOES. It
             * used to put the first prototype's title on as well, which was
             * a title changed by a chord rung for something else: the
             * skins are a list in GAME SELECT's OPTIONS (extras.c), and
             * that is where one is put on. Until the chord is rung,
             * anywhere, a lone shoulder does nothing at all; after it the
             * shoulders do nothing here either. What says it took is the
             * chirp (and on GAME SELECT, where the credits are, the last
             * one's colour: set_credit_colour). */
            if (shoulder_chord())             /* every frame: an edge */
                unlock_cheats();
            /* The title left while two of them shake hands (below): the
             * cable is put away with it. */
            if (demo_link && (pressed & MENU_ADVANCE)) {
                link_shutdown();
                demo_link = false;
            }
            /* ...AND THE SKY SAYS WHAT THE CABLE DOES (title_sky): the
             * cathedral's night turns deep blue while the other console's
             * title is heard, and violet while the two shake hands. Nothing
             * else on a title could say it — and on a real cable, where
             * none of this had been seen to work when it was written, it
             * is the only way to tell which half did not. And a sky that
             * was lit stays lit SKY_HOLD_FRAMES with nothing to light it:
             * a master starting its port over to ask says a word or two
             * that are neither a title's nor an invitation, and the
             * slave's sky went blue, black, violet. */
            {
                int now = demo_link ? TITLE_SKY_HANDS
                          : link_title_other() >= 0 ? TITLE_SKY_HEARD
                          : TITLE_SKY_NIGHT;
                if (now == TITLE_SKY_NIGHT && sky != TITLE_SKY_NIGHT &&
                    ++sky_dark <= SKY_HOLD_FRAMES) {
                    now = sky;
                } else {
                    sky = (uint8_t)now;
                    sky_dark = 0;
                }
                title_sky(now);
            }
            /* SELECT SHOWS THE HIGH SCORES, which otherwise only a lost
             * game could. The page as a game over leaves it, with nothing
             * to type, and back to the title as it goes back after one: a
             * button or LEADER_HOLD_FRAMES. The table is this title's
             * build's (leader_use_table).
             *
             * UNDER THE CHORD, as the rest of what the port adds is: without
             * it SELECT is the cartridge's, another START ($9FA4), which is
             * MENU_ADVANCE below. */
            if (g_pause_unlocked && (pressed & TENGEN_BTN_SELECT)) {
                g_leader_row = -1;
                leader_frames = 0;
                leader_browsing = true;
                leader_nothing_new = false;
                /* Opening on the title's own table, 1 PLAYER under its skin:
                 * the last match's (VERSUS, say) is still the one chosen. */
                leader_use_table(front_skin());
                screen = SCREEN_LEADERBOARD;
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                oam_hide_all();
                draw_leaderboard();
                continue;
            }
            if (pressed & MENU_ADVANCE) {
                screen = SCREEN_GAME_SELECT;
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                /* THE TITLE IS THE ONLY SCREEN WITH SPRITES ON IT. Leaving it
                 * without taking them down left the cathedral's central tower
                 * and a firework standing in the middle of GAME SELECT, LEVEL
                 * SELECT and everything after — sixty-three sprites that
                 * nothing else ever wrote to, so nothing else ever cleared. */
                oam_hide_all();
                continue;
            }
            /* TWO CONSOLES LEFT ON THEIR TITLES, A CABLE BETWEEN THEM,
             * THE CHORD RUNG ON BOTH: their demos start together, as ONE
             * cooperative game the two computers play (link_demo_begin).
             * It happens by itself after any game two players shared: both
             * go back to the title at once and both demos come due at
             * once. Nothing the cartridge does, so nothing a console does
             * without the chord — which says nothing on the cable and is
             * asked nothing (link_title_tick).
             *
             * The master's end asks, being the one that can start a
             * transfer: when its demo is due and the other title has stood
             * untouched TITLE_IDLE_FRAMES, or the other's is due and its
             * own has. It asks by starting the lobby of a demo
             * (tengen_lobby_demo), the settings a demo's; the slave's end
             * sees that and starts its own. A slave whose demo is due
             * first waits DEMO_LINK_WAIT frames to be asked — once it has
             * a title on the master's end to ask it, it waits them out
             * whatever the cable carries meanwhile (demo_waiting): the
             * master's first words as it starts its port over are not
             * always words. The handshake is the lobby's, with the title
             * still on the screen; if it is not done in DEMO_LINK_FRAMES,
             * or either title is left meanwhile, each console plays its
             * own demo as ever.
             *
             * (No word of the chord here: a console without it has heard
             * of no title and been asked nothing, having said nothing. And
             * a Single-Pak copy is a console like any other: it has the
             * chord if its player rang it, or from the first match it
             * played under its cartridge's — the chord is catching, see
             * the start of a linked match.) */
            bool demo_hold = false;
            if (g_title_frame < DEMO_START_FRAME) demo_waiting = false;
            if (!demo_link) {
                int other = link_title_other();
                bool idle = g_title_frame >= TITLE_IDLE_FRAMES;
                bool due = g_title_frame >= DEMO_START_FRAME;
                bool begin = false;
                if (link_title_master())
                    begin = other >= 0 &&
                            ((due && (other & LINK_TITLE_IDLE)) ||
                             (idle && (other & LINK_TITLE_DUE)));
                else if (link_title_invites() >= 2)
                    begin = true;
                else if (due && g_title_frame < DEMO_START_FRAME + DEMO_LINK_WAIT &&
                         (demo_waiting || link_title_invites() ||
                          (other >= 0 && (other & LINK_TITLE_IDLE)))) {
                    demo_waiting = true;
                    demo_hold = true;
                }
                if (begin) {
                    uint16_t seed =
                        (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                    link_init();
                    link_lobby_start_demo(&lobby, seed);
                    uint16_t prints[TENGEN_SKIN_MAX];
                    int count = skin_prints(prints, TENGEN_SKIN_MAX);
                    tengen_lobby_skins(&lobby, prints, count, front_skin());
                    if (link_is_master()) {
                        static const uint8_t kNoHandicap[2] = { 0, 0 };
                        link_lobby_release(&lobby, seed, DEMO_START_LEVEL, 0,
                                           kNoHandicap, true, g_xe);
                        tengen_lobby_rules(&lobby,
                                           opt_link_rules(g_options & OPT_RULES));
                    }
                    demo_link = true;
                    demo_link_frames = 0;
                }
            }
            if (demo_link) {
                link_lobby_step(&lobby);
                if (lobby.ready) {
                    /* ...and the match starts where a lobby's does (the
                     * LINK CABLE screen, which this never shows). */
                    screen = SCREEN_LINK_WAIT;
                    continue;
                }
                if (++demo_link_frames >= DEMO_LINK_FRAMES) {
                    link_shutdown();
                    demo_link = false;
                } else {
                    demo_hold = true;
                }
            }
            /* ...and if nobody presses anything, the cartridge starts playing
             * by itself. See DEMO_START_FRAME. */
            if (g_title_frame >= DEMO_START_FRAME && !demo_hold) {
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                g_demo = true;
                g_linked = false;
                g_link_lost = false;
                g_link_waiting = false;
                g_view = 0;
                g_ai_active = true;
                g_ai_slot = TENGEN_PLAYER_1;   /* the demo's computer is P1 */
                /* ...UP TO XE'S 19 UNDER THE CHORD, as any game under it:
                 * the port's computer does not top out at 17, and a demo
                 * held there played on for ever. */
                tengen_new_game(&g_session.game, seed, DEMO_START_LEVEL,
                                 false, false, g_xe);
                tengen_ai_reset(&g_ai);
                /* The cartridge's computer as it ships: TengenAi's knobs
                 * all left off — EXCEPT UNDER THE CHORD, where the computer
                 * of every match is the port's (`smart`, as in VERSUS and
                 * WITH COMPUTER) and the demo shows that one. Same pad and
                 * same pace; nobody beside it to read or keep up with, so
                 * `coop_aware`, `adaptive` and `deep` stay off. And the
                 * cartridge's bugs mended, as in any match under it. */
                g_ai.smart = g_pause_unlocked;
                g_session.game.mended = g_pause_unlocked;
                /* ...AND THE PLAYER'S OPTIONS, the rules on player 1's board,
                 * which is the computer's here (options_apply_solo: nothing
                 * without the chord), and the ghost (options_mine). */
                options_apply_solo(&g_session.game);
                g_ai_last_piece = TT_NONE;
                g_ai_last_partner = TT_NONE;
                g_ai_frame = 0;
                points_clear();
                leader_new_match();
                stats_begin_match();
                /* ...and the HUD this mode opens on. See hud_set. UNDER THE
                 * CHORD, HUD STATS: the port's computer playing to a
                 * histogram that scales, rather than to the cartridge's
                 * banner, which says nothing about how it is playing. */
                hud_reset();
                if (g_pause_unlocked) g_hud = HUD_STATS;
                g_demo_over_frames = 0;
                g_mix_step = 0;
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                g_shown_piece2 = TT_NONE;
                set_piece_palette(g_session.game.player[0].piece.current);
                oam_hide_all();
                g_idle_frame = 0;
                g_dance_frames = 0;
                screen = SCREEN_PLAYING;
                match_running = true;
                g_pause_can_save = false;
                over_frames = 0;
                skin_begin_match(false, -1);
                /* MUSIC_SUSPEND, which is demoStart's own second act
                 * (main.asm.txt:3220-3221): the attract mode is silent but
                 * for the game's effects. */
                g_front_tune = FRONT_NOTHING;
                /* ...EXCEPT UNDER THE CHORD, where it plays MUSIC MIX: the
                 * tunes the chord uncovered, taking turns at its level-ups
                 * as in a game. g_music is the match's tune, and the
                 * player's choice on LEVEL SETTINGS: kept, and given back
                 * as the demo ends (demo_music_back). */
                if (g_pause_unlocked) {
                    g_demo_music_kept = g_music;
                    g_demo_mix = true;
                    g_music = MUSIC_MIX;
                    start_music(MUSIC_MIX);
                } else {
                    stop_music();
                }
                vsync();
                audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }

            vsync();
            draw_title();
            draw_title_sprites();
            audio_frame();
            continue;
        }

        if (screen == SCREEN_GAME_SELECT) {
            /* SILENT, and deliberately not what the cartridge does. Its title
             * theme carries on through here — but its front end is a one-way
             * chain, so the theme only ever plays forwards. This port can walk
             * back, and a theme that resumes behind you every time you press B
             * is worse than a menu that waits quietly. Leaving the cathedral
             * stops its music AND the fireworks' bangs with it. */
            front_music(FRONT_SILENCE);
            bool chord = shoulder_chord();    /* every frame: an edge */
            /* OPTIONS' BOXES, while one is up, have the buttons and the
             * frame (extras.c); the list waits under them on OPTIONS. */
            if (extras_up()) {
                vsync();
                extras_frame(pressed);
                audio_frame();
                continue;
            }
            if (chord && unlock_cheats()) {
                /* Nothing else on this page shows for it — the tunes are on
                 * the next one and the menu is in a game — but OPTIONS, the
                 * list's sixth line; the chirp is the rest of the answer, and
                 * it is the same one the next page gives. */
            }
            if (pressed & MENU_STEP) {
                /* SIX LINES UNDER THE CHORD, the sixth OPTIONS, which is not
                 * a mode: the cursor stands on it (gs_options) and game_mode
                 * keeps the last mode it left. */
                int lines = g_pause_unlocked ? GAME_COUNT + 1 : GAME_COUNT;
                int at = gs_options ? GAME_COUNT : game_mode;
                at = (pressed & MENU_BACKWARD) ? (at + lines - 1) % lines
                                               : (at + 1) % lines;
                gs_options = at == GAME_COUNT;
                if (!gs_options) game_mode = (uint8_t)at;
                /* processMenuInput plays this on every move (:4655). */
                cursor_blip();
            }
            if (gs_options && (pressed & MENU_CONFIRM)) {
                extras_open();
                vsync();
                extras_frame(0);
                audio_frame();
                continue;
            }
            if (pressed & TENGEN_BTN_B) {
                gs_options = false;
                screen = SCREEN_TITLE;
                restart_title_sprites();
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            if (pressed & MENU_CONFIRM) {
                /* ON A CABLE THE CHOOSING COMES AFTER THE CONNECTING. Only one
                 * of the two players should be picking a level and a tune, and
                 * neither console knows which one that is until the cable has
                 * told them — so 2 PLAYER goes straight to the lobby, and the
                 * master reaches the level screen from there. */
                if (GAME_IS_LINKED(game_mode)) {
                    uint16_t seed =
                        (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                    link_init();
                    link_lobby_start_held(&lobby, seed);
                    /* THE PROTOTYPES AS A SKIN OVER THE CABLE: this build's
                     * skins, and the title's choice as the offer — which only
                     * counts if this console turns out to be the master. See
                     * tengen_lobby_skins. */
                    uint16_t prints[TENGEN_SKIN_MAX];
                    int count = skin_prints(prints, TENGEN_SKIN_MAX);
                    tengen_lobby_skins(&lobby, prints, count, front_skin());
                    /* ...and which of the two linked games this console came
                     * for: a partner who chose the other is not one. */
                    tengen_lobby_mode(&lobby, game_mode == GAME_COOP);
                    screen = SCREEN_LINK_WAIT;
                }
#ifndef TENGEN_MULTIBOOT
                else if (saved_has(game_mode) && saved_load(game_mode, &resume)) {
                    /* A GAME OF THIS MODE SAVED (EXIT GAME's SAVE): straight
                     * back into it, paused, as a game left on the plaque
                     * comes back at power-on — LEVEL SETTINGS confirms
                     * itself (`resuming`) — and off the battery with it.
                     * WITH THE CHORD OR WITHOUT IT. Only a console that has
                     * found it can SAVE, so a game saved is one found; and
                     * a power cycle forgets the chord, which hid the game
                     * until something rang it again — a game put back from
                     * the plaque at power-on, say, and lost — and then the
                     * save seemed to come from nowhere. It comes back with
                     * the game, as the plaque's does. */
                    if (resume.unlocked) cheats_restore();
                    start_level = resume.start_level;
                    g_music = resume.music;
                    handicap[0] = resume.handicap[0];
                    handicap[1] = resume.handicap[1];
                    g_title_skin = resume.title_skin;
                    resuming = true;
                    screen = SCREEN_LEVEL_SELECT;
                }
#endif
                else {
                    screen = SCREEN_LEVEL_SELECT;
                    menu_field = MENU_FIELD_LEVEL;
                }
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            vsync();
            draw_game_select(gs_options ? GAME_COUNT : game_mode);
            audio_frame();
            continue;
        }

        if (screen == SCREEN_LEVEL_SELECT) {
            /* THE CABLE KEEPS TURNING while the master reads the menu. The
             * handshake is parked at its greeting, but it still has to happen:
             * a lobby that stops transferring looks exactly like a lobby whose
             * cable fell out, and the guest would give up after ten seconds of
             * the master thinking. */
            if (GAME_IS_LINKED(game_mode)) {
                link_lobby_step(&lobby);
                /* THE PARTNER WENT, OR THIS IS NOT THE MASTER AFTER ALL: back
                 * to the cable screen. A lobby that loses its partner goes
                 * back to holding (TENGEN_LOBBY_LOST), and one transfer's ID
                 * bits can tell a console that read its SI pin wrong that it
                 * is the slave — on two real SPs a slave had come up here. */
                if (!lobby.linked || !link_is_master()) {
                    screen = SCREEN_LINK_WAIT;
                    vsync();
                    audio_frame();
                    clear_screen();
                    continue;
                }
            }

            /* ONE CALL EACH, and the results kept: these are edge detectors
             * with their own held state, so asking twice in a frame answers
             * "yes" and then "no" — which is how the handicap's first version
             * quietly ate the hidden tunes' chord. */
            bool chord = shoulder_chord();
            /* A GAME COMING BACK goes straight through: a button held from
             * power-on is not a choice on a page that is never seen — B would
             * turn it back, LEFT move the level, the shoulders' chord change
             * the tune. */
            if (resuming) {
                pressed = 0;
                chord = false;
            }

            /* UP/DOWN/SELECT MOVE THE CURSOR, LEFT/RIGHT CHANGE THE FIELD.
             * SELECT moving it the way DOWN does is the cartridge's
             * (LA048's carry-set add, $9FBC/$A063); the split between moving
             * and setting is the port's, and it is what lets three settings
             * share one page.
             *
             * EXCEPT ON THE HANDICAP LINE OF A RACE, where SELECT switches
             * between the two numbers instead of leaving the line.
             *
             * This used to be L and R — a shoulder on each player's side of
             * the pad — and a Game Boy Advance has those where a Nintendo
             * Entertainment System controller does not. The cartridge's own
             * handicap screen is driven by the four it had, so the port should
             * be too: Up and Down are the cursor's and Left and Right are the
             * value's, which leaves exactly SELECT, and SELECT already means
             * "the other one" everywhere else on this page. Down still leaves
             * the line, so nothing is trapped there. */
            /* ONLY 2 PLAYER HAS TWO. Against the computer the cartridge's
             * handicap screen has one cursor, and endPlayfieldInit buries
             * both boards under that one number (`bcs @computerIsPlaying`,
             * main.asm.txt:3539-3542). Traced: MODE=versus --handicap.
             *
             * EXCEPT BEHIND THE CHORD, where VERSUS COMPUTER gets the second
             * number too, for the computer's board: the port's, like the
             * rest of what the chord uncovers. Without it the cartridge's
             * one number stands, burying both. (WITH COMPUTER shares one
             * board, so one number is all it can have.) */
            bool two_handicaps = game_mode == GAME_2P ||
                                 (game_mode == GAME_VS && g_pause_unlocked);

            /* ON A GAME BOY PLAYER, THE CARTRIDGE'S THREE PAGES (draw_tv_page):
             * UP and DOWN move down the column, LEFT and RIGHT pick the
             * player's column on a race's HANDICAP, START or A turns the
             * page and plays from the last, B turns it back. The values and
             * everything they do after are the one page's, below. */
#ifdef TENGEN_MULTIBOOT
            /* Never a Player on the copy (gbp_present), and said here as
             * well, where the link can see it: the pages it would draw are
             * a kilobyte its image does not have. */
            const bool tv = false;
#else
            bool tv = gbp_present() && !resuming;
#endif
            if (tv) {
                bool up = (pressed & TENGEN_BTN_UP) != 0;
                bool down = (pressed & (TENGEN_BTN_DOWN | TENGEN_BTN_SELECT)) != 0;
                bool moved = up || down;
                if (tv_page == TV_PAGE_LEVEL && moved) {
                    uint8_t levels = start_level_choices();
                    start_level = (uint8_t)((start_level +
                                              (up ? levels - 1 : 1)) % levels);
                } else if (tv_page == TV_PAGE_HANDICAP) {
                    if (two_handicaps &&
                        (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT))) {
                        handicap_who ^= 1;
                        moved = true;
                    }
                    if (up || down) {
                        int who = two_handicaps ? handicap_who : 0;
                        handicap[who] = (uint8_t)((handicap[who] +
                                                   (up ? TENGEN_HANDICAP_MAX : 1)) %
                                                  (TENGEN_HANDICAP_MAX + 1));
                    }
                } else if (tv_page == TV_PAGE_MUSIC && moved) {
                    g_music = (uint8_t)((g_music + (up ? music_choices() - 1 : 1))
                                         % music_choices());
                }
                if (chord && unlock_cheats()) {
                    g_music = MUSIC_KOROBEINIKI;
                    tv_page = TV_PAGE_MUSIC;
                    moved = true;
                } else if (moved) {
                    cursor_blip();
                }
                /* Each page silent but the tunes', where the cursor plays
                 * them as the cartridge's music screen does (:4694-4696). */
                front_music(tv_page == TV_PAGE_MUSIC ? g_music : FRONT_SILENCE);
                bool turn = (pressed & MENU_CONFIRM) && tv_page + 1 < TV_PAGE_COUNT;
                bool turn_back = (pressed & TENGEN_BTN_B) && tv_page > TV_PAGE_LEVEL;
                if (turn || turn_back) {
                    tv_page += turn ? 1 : -1;
                    screen_blip();
                    vsync();
                    audio_frame();
                    clear_screen();
                    continue;
                }
                /* The last page's START and the first page's B go on to
                 * what the one page does with them, below. */
                if (pressed & (MENU_CONFIRM | TENGEN_BTN_B)) tv_page = TV_PAGE_LEVEL;
                pressed &= (uint8_t)(MENU_CONFIRM | TENGEN_BTN_B);
                chord = false;
            }

            bool pick_side = (pressed & TENGEN_BTN_SELECT) && two_handicaps &&
                              menu_field == MENU_FIELD_HANDICAP;
            if (pick_side) handicap_who ^= 1;
            if (pressed & TENGEN_BTN_UP)
                menu_field = (menu_field + MENU_FIELD_COUNT - 1) % MENU_FIELD_COUNT;
            if ((pressed & TENGEN_BTN_DOWN) ||
                ((pressed & TENGEN_BTN_SELECT) && !pick_side))
                menu_field = (menu_field + 1) % MENU_FIELD_COUNT;

            bool back = (pressed & TENGEN_BTN_LEFT) != 0;
            bool moved = (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) != 0;

            if (moved) {
                if (menu_field == MENU_FIELD_LEVEL) {
                    uint8_t levels = start_level_choices();
                    start_level = (uint8_t)((start_level +
                                              (back ? levels - 1 : 1)) % levels);
                } else if (menu_field == MENU_FIELD_HANDICAP) {
                    /* Whichever of the two the white is on — the pad sets one
                     * number at a time and SELECT says which. */
                    int who = two_handicaps ? handicap_who : 0;
                    handicap[who] = (uint8_t)((handicap[who] +
                                               (back ? TENGEN_HANDICAP_MAX : 1)) %
                                              (TENGEN_HANDICAP_MAX + 1));
                } else {
                    g_music = (uint8_t)((g_music + (back ? music_choices() - 1 : 1))
                                         % music_choices());
                }
            }

            /* The shoulders set no handicap here any more — see SELECT above.
             * The chord is all they are for on this page, and it is checked
             * below. */
            if (pick_side) moved = true;   /* repaint: the white has moved */

            if (chord && unlock_cheats()) {
                /* This page can SHOW what was uncovered, so it does: the
                 * cursor goes to MUSIC and the list opens on the first of the
                 * hidden tunes. */
                g_music = MUSIC_KOROBEINIKI;
                menu_field = MENU_FIELD_MUSIC;
            } else if (moved || (pressed & MENU_STEP)) {
                cursor_blip();
            }

            /* MOVING THE CURSOR PLAYS THE TUNE. The cartridge calls LA035 from
             * `$A00A` on every cursor move while gameState is
             * GAMESTATE_MUSIC_SELECT (main.asm.txt:4694-4696), so you hear each
             * one as you pick it. front_music does nothing when the tune has
             * not changed, so this also settles the music on arrival — which
             * is what stops the title theme here, and what keeps the screen
             * SILENT while NO MUSIC is the choice. */
            if (!tv) front_music(g_music);

            if (pressed & TENGEN_BTN_B) {
                /* The cartridge has no back button at all — its menus are a
                 * one-way chain with an idle timer — so B is the port's, for
                 * the same reason A confirms. */
                screen_blip();
                screen = SCREEN_GAME_SELECT;
                /* Backing out of a 2P choice drops the cable with it. */
                if (GAME_IS_LINKED(game_mode)) link_shutdown();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            if ((pressed & MENU_CONFIRM) || resuming) {
                /* START, and only START, confirms on the cartridge ($A011);
                 * A is the port's second confirm, as everywhere else here. */
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                screen_blip();

                if (GAME_IS_LINKED(game_mode)) {
                    /* The master has chosen. Letting the handshake go delivers
                     * the seed, the level and the tune to the other console,
                     * and both leave the lobby together. */
                    link_lobby_release(&lobby, seed, start_level, g_music, handicap,
                                        game_mode == GAME_COOP, g_xe);
                    /* ...and the rules in this console's OPTIONS, which are
                     * the match's (OPT_RULES): the chord's, like the menu. */
                    tengen_lobby_rules(&lobby, g_pause_unlocked
                                                   ? opt_link_rules(g_options & OPT_RULES) : 0);
                    screen = SCREEN_LINK_WAIT;
                    vsync();
                    audio_frame();
                    clear_screen();
                    continue;
                }

                g_linked = false;
                g_link_lost = false;
                g_link_waiting = false;
                g_view = 0;
                /* VERSUS and WITH are the two-player modes that need no second
                 * console: the board is a race's or a coop's, and the computer
                 * presses player 2's buttons. playModeTable ($9F51) is what
                 * says which board — `00 01 FF 01 FF`. */
                g_ai_active = GAME_HAS_AI(game_mode);
                /* AND THE COMPUTER IS PLAYER 2 AGAIN. The attract demo puts
                 * it on PLAYER 1 — that is the cartridge's own arrangement,
                 * the VS and WITH paths `inx` first and the demo does not
                 * (main.asm.txt:4145-4157) — and nothing here ever put it
                 * back. So a console that had been left alone long enough for
                 * the demo to run once started VERSUS and WITH COMPUTER with
                 * ai_play_frame bailing out on its first line, and the
                 * computer simply never played. Every headless check drove
                 * the menus faster than the demo's own clock, so every one of
                 * them passed. */
                g_ai_slot = TENGEN_PLAYER_2;
                tengen_new_game(&g_session.game, seed, start_level,
                                 g_ai_active, GAME_IS_COOP(game_mode), g_xe);
                /* The cartridge's own bugs mended, behind the chord (see
                 * TengenGame.mended; over a cable the master's chord). */
                g_session.game.mended = g_pause_unlocked;
                /* ...and the pause menu's OPTIONS, which the same chord
                 * puts there: UP's drop and the faster hands, as the player
                 * left them (options_apply_solo). */
                options_apply_solo(&g_session.game);
                tengen_ai_reset(&g_ai);
                /* AT THE CARTRIDGE'S PACE. The port once had it hold Down
                 * and look at each piece first ("va un tanto lento"); that
                 * was the port asking for the cartridge's speed, not for a
                 * faster computer than the cartridge's. So neither knob is
                 * set: it never presses Down and moves from the frame a
                 * piece appears, as computerMove does. */
                /* AND IT READS ITS PARTNER, BEHIND THE CHORD. This one is
                 * not a pace knob, it is a different player: the cartridge's
                 * computer sees the settled board and not the other falling
                 * piece, so on the shared field of WITH COMPUTER the two of
                 * them pick the same column and shoulder each other down it —
                 * 48% of every shift either asked for was refused, and nine
                 * in ten of those by the partner. `coop_aware` reads the
                 * partner's landing as ground and waits its turn before
                 * dropping, and it is worth 908 pieces and 34 lines against
                 * 1637 and 197 over twenty-four playouts.
                 *
                 * It goes behind `unlock_cheats` for the same reason the
                 * pause menu does: WITH COMPUTER is a mode the cartridge
                 * ships, and what it ships is the player above. The chord
                 * that already turns the coop panel's last cells into the
                 * board's totals turns this on with them. */
                g_ai.coop_aware = g_pause_unlocked && GAME_IS_COOP(game_mode);
                /* ...and under the same chord, not the cartridge's computer
                 * at all but the port's (`smart`, tengen_ai.c): same pad, same
                 * pace, a better head. A person for a partner never says where
                 * they are going, so `partner_known` stays off. */
                g_ai.smart = g_pause_unlocked;
                /* ...with a person's pace and lead to go by (`adaptive`):
                 * it drops its pieces when the player drops theirs, and to
                 * catch up when four lines behind. */
                g_ai.adaptive = g_pause_unlocked;
                /* ...and on the shared board, where it is watched, with the
                 * time a slow fall leaves it: slides under a ledge once the
                 * plan is made (`deep`). VERSUS keeps its computer as it is:
                 * nobody sees that board. */
                g_ai.deep = g_pause_unlocked && GAME_IS_COOP(game_mode);
                g_ai_last_piece = TT_NONE;
                g_ai_last_partner = TT_NONE;
                g_ai_frame = 0;
                points_clear();
                leader_new_match();
                stats_begin_match();
                /* ...and the HUD this mode opens on. See hud_set. */
                hud_reset();
                swallow_held_buttons(&g_session.game);
                /* endPlayfieldInit's own place for it, right after the field
                 * is laid out (main.asm.txt:3536-3546). A shared board takes
                 * one burial, not two; and the computer's own board takes
                 * PLAYER 1'S — the cartridge offers no second number against
                 * it — unless the chord gave it one (see two_handicaps). */
                tengen_apply_handicap(&g_session.game, TENGEN_PLAYER_1, handicap[0]);
                if (g_ai_active && !g_session.game.coop)
                    tengen_apply_handicap(&g_session.game, TENGEN_PLAYER_2,
                                           handicap[two_handicaps ? 1 : 0]);
                g_mix_step = 0;      /* every game opens on the same tune */
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                set_piece_palette(g_session.game.player[0].piece.current);
                oam_hide_all();
                g_idle_frame = 0;
                g_dance_frames = 0;
                screen = SCREEN_PLAYING;
                match_running = true;
                /* EXIT GAME offers SAVE in the modes a console plays alone. */
                g_pause_can_save = saved_slot(game_mode) >= 0;
                over_frames = 0;
                skin_begin_match(false, -1);
                g_front_tune = FRONT_NOTHING;
#ifndef TENGEN_MULTIBOOT
                if (resuming) {
                    /* ...and the game itself, over the one just dealt. */
                    resuming = false;
                    suspend_apply();
                    /* ...with the OPTIONS as they are now: they are kept
                     * on their own (SAVE_OPTIONS_OFF), and may have been
                     * changed under the plaque after it was written. */
                    options_apply_solo(&g_session.game);
                    g_mix_step = resume.mix_step;
                    g_hud = resume.hud;
                    g_idle_palette = resume.idle_palette;
                    g_ai_last_piece = resume.ai_last_piece;
                    g_ai_last_partner = resume.ai_last_partner;
                    g_ai_frame = resume.ai_frame;
                    set_piece_palette(g_session.game.player[0].piece.current);
                    start_music(g_music);
                    match_resumed_paused();
                } else
#endif
                start_music(g_music);
                vsync();
                audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }
            vsync();
            /* Only a race has two handicaps, and against the computer only
             * behind the chord: see two_handicaps. */
            if (tv)
                draw_tv_page(tv_page, start_level, start_level_choices(),
                             g_music, music_choices(), handicap,
                             two_handicaps, handicap_who);
            else
                draw_level_settings(menu_field, start_level, g_music, handicap,
                                     two_handicaps, handicap_who);
            audio_frame();
            continue;
        }

        if (screen == SCREEN_LINK_WAIT) {
            /* Quiet while the cable is looking for the other end; there is
             * nothing to preview yet. */
            front_music(FRONT_SILENCE);
            /* WHO IS THERE, for the screen to say: a console with no
             * cartridge is asked for separately (link_probe), and only
             * until somebody has answered the lobby. */
            if (!lobby.linked) link_probe();
            /* One handshake transfer per frame until both consoles agree on a
             * seed, a level and a tune — or until the cable gives up. */
            link_lobby_step(&lobby);

            /* THE MASTER GOES OFF TO CHOOSE the moment the cable answers.
             * The guest stays here with the cossack: it has nothing to decide,
             * because the level and the tune are the master's.
             *
             * `hold` is what makes this happen once. Testing "connected and
             * not ready" instead sent the master straight back to the menu the
             * frame after it had chosen, and it ping-ponged there while the
             * other console went off and started the match alone. */
            if (lobby.hold && lobby.linked && link_is_master()) {
                screen = SCREEN_LEVEL_SELECT;
                menu_field = MENU_FIELD_LEVEL;
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                oam_hide_all();
                continue;
            }

#ifndef TENGEN_MULTIBOOT
            /* THE ADAPTER, PLUGGED IN AFTER POWER-ON. It is asked for at
             * power-on (wireless_detect), and a player who plugs it in later
             * finds the cable's screen. So the cable's screen asks too —
             * only when nobody is on the cable at all (link_peer): the
             * login takes the port through general purpose, which a
             * console of ours on the other end would hear as a flutter, and
             * one in its BIOS is found by the probe. Every few seconds,
             * while nobody comes; found, the screen is the WIRELESS one. */
            /* (Not on a Game Boy Player, whose port the cable borrows.) */
            if (!lobby.linked && !wireless_on() && !gbp_present() &&
                link_peer() == LINK_PEER_NOBODY) {
                /* ...and between two rounds of the probe, not across one
                 * (link_probe_resting). */
                if (++radio_wait >= RADIO_FIRST_TRY && link_probe_resting()) {
                    radio_wait = RADIO_FIRST_TRY - RADIO_RETRY;
                    link_shutdown();
                    wireless_detect();
                    link_rest();
                    link_init();     /* over the air, if it answered */
                    tengen_lobby_forget(&lobby, link_is_master());
                }
            } else {
                radio_wait = 0;
            }
            /* ...and the other way: one that stops answering before anybody
             * was found over it (wireless.c gives up on it) hands the
             * screen back to the cable. */
            if (wireless_on() && !wireless_present() && !lobby.linked) {
                link_shutdown();
                link_rest();
                link_init();
                tengen_lobby_forget(&lobby, link_is_master());
            }
#endif

            /* SINGLE-PAK: SELECT, while nobody has answered, sends the game
             * to a console with no cartridge (link_multiboot_send) — from
             * the cartridge, the image it carries; from a console that got
             * the game that way, ITSELF, still in EWRAM where its BIOS put
             * it, so the copy can make a copy. It keeps trying — the other
             * console may not be switched on yet — until the game is
             * across or B stops it, and then the cable is this lobby's
             * again, which is where the other console's copy comes
             * looking for it. */
            /* ...AND BY ITSELF, ONCE THE CABLE HAS FOUND ONE (link_peer): a
             * console with no cartridge waiting in its BIOS has no use for
             * anything else. Three-quarters of a second of the screen
             * saying so first; B on the sending screen stops it, and then
             * it waits for SELECT until that console goes away and another
             * comes. */
            /* ...GOES AWAY, not "is not seen for a frame": coming back from
             * the sending screen the cable is started over and that console
             * has to be found again, which used to count as its having
             * gone — B stopped the send, and a second later it began again
             * by itself. Two seconds of not finding it is gone. */
            if (!lobby.linked && link_peer() == LINK_PEER_EMPTY_GBA) {
                if (auto_send_wait < AUTO_SEND_FRAMES) auto_send_wait++;
                auto_send_gone = 0;
            } else {
                auto_send_wait = 0;
                if (auto_send_gone < AUTO_SEND_GONE_FRAMES) auto_send_gone++;
                else auto_send_refused = false;
            }
            bool auto_send = auto_send_wait >= AUTO_SEND_FRAMES &&
                             !auto_send_refused;
            /* ...AND OVER THE AIR: SELECT on the WIRELESS screen while
             * nobody is found opens a room the adapter's loader on a GBA
             * with no cartridge can pick (wireless_multiboot_send). Once
             * the game is across, both look for each other as usual. */
            if (!lobby.linked && wireless_on() && (pressed & TENGEN_BTN_SELECT)) {
                screen_blip();
                link_shutdown();
                WlSendResult r = wireless_multiboot_send(single_pak_image(),
                                                         single_pak_length(),
                                                         air_send_frame);
                if (r == WL_SEND_DONE)
                    for (int wait = 0; wait < 60; wait++) {
                        vsync();
                        audio_frame();
                    }
                held_last = read_buttons();   /* B is not the lobby's */
                link_init();
                tengen_lobby_forget(&lobby, link_is_master());
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            if (!lobby.linked && !wireless_on() &&
                ((pressed & TENGEN_BTN_SELECT) || auto_send)) {
                const uint8_t *img = single_pak_image();
                uint32_t img_len = single_pak_length();
                screen_blip();
                bool wrong_end = false;
                bool muted = false;
                for (int frame = 0;; frame++) {
                    vsync();
                    draw_link_sending(wrong_end);
                    audio_frame();
                    if (read_buttons() & TENGEN_BTN_B) {
                        auto_send_refused = true;
                        break;
                    }
                    /* A few frames between tries, as gba-link-connection
                     * waits: a BIOS that has just been switched on needs
                     * them to be listening. */
                    if (frame % 8 != 7) continue;
                    /* NOTHING SOUNDS WHILE IT SENDS. The sound engine
                     * does not run meanwhile, and whatever note was on
                     * would hold for the whole of it: a long beep. Muted
                     * from the first try to the end, once — the blip has
                     * had its eight frames by then — and not around each
                     * try: with nobody answering, the volume going off and
                     * on again with every try clicked in the speaker two
                     * to four times a second. */
                    if (!muted) {
                        psg_mute(true);
                        muted = true;
                    }
                    LinkSendResult r = link_multiboot_send(img, img_len,
                                                           draw_send_progress);
                    if (r == LINK_SEND_DONE) {
                        /* A SECOND OF QUIET before the lobby starts talking:
                         * the other console is starting the image now. */
                        for (int wait = 0; wait < 60; wait++) {
                            vsync();
                            audio_frame();
                        }
                        break;
                    }
                    wrong_end = r == LINK_SEND_WRONG_END;
                }
                if (muted) psg_mute(false);
                held_last = read_buttons();   /* B is not the lobby's */
                link_init();
                tengen_lobby_forget(&lobby, link_is_master());
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }
            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_GAME_SELECT;
                link_shutdown();
                demo_link = false;      /* ...a demo's lobby included */
                oam_hide_all();
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }

            if (lobby.ready) {
                /* THE RECORDS GO ACROSS FIRST (TengenRecordSync): a
                 * Single-Pak copy, which has no save, hands the rows it put
                 * on its tables to a console that has one, every time they
                 * link, unasked; and two cartridges whose tables differ
                 * each ASK THEIR PLAYER whether to add the other's best
                 * (draw_records_question) — once for what that other has,
                 * until the power is cycled, so a player who said no is not
                 * asked again before every match. Short when there is
                 * nothing to send. If any were kept, both consoles say so
                 * for two seconds, keeping the link turning meanwhile (over
                 * the air, a second and a half of silence is a lost
                 * partner). */
                /* (Not for the demo two titles share: nobody is there to
                 * ask, and nothing of anybody's is at stake.) */
                const bool demo_start = demo_link;
                const uint8_t music_was = g_music, mode_was = game_mode;
                demo_link = false;
                /* ...and a demo from here on, for everything the start of a
                 * match asks (as the title's own demo is before its). */
                if (demo_start) g_demo = true;
                if (!demo_start)
                {
                    TengenRecordSync sync;
                    TengenRecord mine[TENGEN_RECORDS_MAX];
#ifdef TENGEN_MULTIBOOT
                    const bool storage = false;
                    int n = leader_unsent(mine, TENGEN_RECORDS_MAX);
#else
                    const bool storage = true;
                    int n = leader_offer(mine, TENGEN_RECORDS_MAX);
#endif
                    int kept = 0;
                    link_records_start(&sync, storage, mine, n);
#ifndef TENGEN_MULTIBOOT
                    int ask = -1;               /* not on the screen yet */
                    uint8_t ask_held = 0xFF;
#endif
                    while (!sync.complete && !sync.failed) {
                        link_records_step(&sync);
#ifndef TENGEN_MULTIBOOT
                        if (sync.asking && records_answered &&
                            (!records_took ||
                             sync.partner_digest == records_answered_for)) {
                            /* ASKED ONCE. A player who said no is not asked
                             * again before every match — nor when the other
                             * console's tables change, which they do the
                             * moment its own player says yes. One who said
                             * yes is asked again only when the other has
                             * something it did not have then. */
                            tengen_records_answer(&sync, false);
                        } else if (sync.asking) {
                            uint8_t held = read_buttons();
                            uint8_t hit = (uint8_t)(held & ~ask_held);
                            ask_held = held;
                            bool answer = false, take = false;
                            if (ask < 0) {
                                ask = RECORDS_ASK_NO;
                                screen_blip();
                                draw_records_question(ask);
                            } else if (hit & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) {
                                ask = ask == RECORDS_ASK_NO ? RECORDS_ASK_YES
                                                            : RECORDS_ASK_NO;
                                cursor_blip();
                                draw_records_question(ask);
                            } else if (hit & TENGEN_BTN_B) {
                                answer = true;
                            } else if (hit & (TENGEN_BTN_A | TENGEN_BTN_START)) {
                                answer = true;
                                take = ask == RECORDS_ASK_YES;
                            }
                            if (answer) {
                                tengen_records_answer(&sync, take);
                                records_answered = true;
                                records_took = take;
                                records_answered_for = sync.partner_digest;
                                screen_blip();
                                draw_records_question(RECORDS_ASK_ANSWERED);
                            }
                        }
#endif
                        if (sync.theirs_ready && sync.out_held) {
                            kept = leader_merge(sync.theirs, sync.theirs_n);
                            tengen_records_saved(&sync, (uint8_t)kept);
                        }
                        vsync();
                        audio_frame();
                    }
                    /* What the other kept of this console's: a copy's rows
                     * are handed on and need not go again; a cartridge's
                     * are simply on two tables now. */
                    int given = sync.complete && sync.partner_storage ? sync.saved : 0;
                    if (!storage && sync.complete && sync.partner_storage)
                        leader_mark_sent();
                    if (sync.complete && (kept > 0 || given > 0)) {
                        if (kept > 0) draw_records_saved(kept, RECORDS_FROM_OTHER);
                        else draw_records_saved(given, storage ? RECORDS_ON_OTHER
                                                               : RECORDS_ON_CARTRIDGE);
                        nes_audio_play(NES_SOUND_MENU_SELECT);
                        for (int wait = 0; wait < RECORDS_SAVED_FRAMES; wait++) {
                            link_tick();
                            LinkFrame f;
                            while (link_pop(&f)) { }
                            link_pump();
                            vsync();
                            audio_frame();
                        }
                    }
                }
                /* The game the lobby agreed on, which on a Single-Pak slave
                 * is the one the master chose (and on any other console the
                 * one this one chose too). */
                game_mode = lobby.coop ? GAME_COOP : GAME_2P;
                /* The cable master is player 1. That is not a convention this
                 * port invented; it is the one fact both consoles can agree
                 * on without asking, because the hardware sets it from which
                 * end of the cable each is plugged into. */
                g_linked = true;
                g_link_lost = false;
                g_link_waiting = false;
                g_ai_active = false;
                g_view = link_is_master() ? 0 : 1;
                /* The master's choice wins, the egg included: both consoles run
                   the same ROM, so a linked player who never found the code
                   still hears it. */
                g_music = lobby.music < MUSIC_UNLOCKED_COUNT ? lobby.music : 0;
                /* ...AND THE CHORD IS CATCHING. A match is played under the
                 * master's (lobby.xe; match_chord), pause menu and all, on
                 * both consoles; and until now the other console came out
                 * of it as it went in, without. So a cartridge with the
                 * chord and the copy it had sent finished a game together
                 * and were two different programs again: the copy sat on
                 * the HIGH SCORES page seventeen seconds where the
                 * cartridge sat five, and two titles that should have
                 * shared their demo (below) could not, one of them never
                 * having been told. A console that has played under the
                 * chord has it, until the power switch. */
                if (lobby.xe && !g_pause_unlocked) cheats_restore();
                tengen_link_start(&g_session, lobby.seed, lobby.start_level,
                                   link_is_master() ? TENGEN_PLAYER_1 : TENGEN_PLAYER_2,
                                   lobby.coop, lobby.xe);
                tengen_link_rules(&g_session, lobby.rules);
                swallow_held_buttons(&g_session.game);
                /* Both consoles bury both boards from the one seed the lobby
                 * delivered, so the two fields match without another word on
                 * the wire. Coop has ONE board, so only player 1's handicap
                 * means anything there — burying the shared field twice would
                 * be twice the garbage nobody asked for. */
                for (int i = 0; i < (lobby.coop ? 1 : 2); i++)
                    tengen_apply_handicap(&g_session.game, (TengenPlayerSlot)i,
                                           lobby.handicap[i]);
                g_mix_step = 0;
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                g_shown_piece2 = TT_NONE;
                points_clear();
                leader_new_match();
                stats_begin_match();
                /* ...and the HUD this mode opens on. See hud_set. */
                hud_reset();
                set_piece_palette(g_session.game.player[g_view].piece.current);
                link_play_begin();
                link_match_begin(lobby.xe);
                screen = SCREEN_PLAYING;
                match_running = true;
                g_pause_can_save = false;    /* half the game is on the other console */
                over_frames = 0;
                skin_begin_match(true, lobby.skin);
                g_front_tune = FRONT_NOTHING;
                if (demo_start) {
                    /* ...AND NOBODY IS PLAYING IT: the demo two titles
                     * share. A computer on each pad, both on both consoles
                     * (link_demo_begin); MUSIC MIX, as the demo under the
                     * chord plays, and the tune and GAME SELECT's line the
                     * player left given back as it ends (demo_music_back,
                     * demo_mode_back); nothing written down
                     * (leader_record), nobody's name to swap. */
                    g_demo_over_frames = 0;
                    g_demo_mode_kept = mode_was;
                    link_demo_begin(lobby.xe);
                    g_demo_music_kept = music_was;
                    g_demo_mix = true;
                    g_music = MUSIC_MIX;
                }
                start_music(g_music);
                /* The lobby's cossack is four sprites nothing on the play
                 * screen ever writes to, so nothing there would ever have
                 * cleared him — he stood in the middle of the board. */
                oam_hide_all();
                g_idle_frame = 0;
                g_dance_frames = 0;
                vsync();
                audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }

            vsync();
            draw_link_wait(&lobby, link_wait_frames++);
            audio_frame();
            continue;
        }

        /* THE HIGH SCORES PAGE, which is what a finished game leads to. It
         * sits here until the entries that made the board have been typed
         * into, and then until either a button or its own clock — the ROM's
         * own way out of it is a countdown on player1FallTimer to
         * initializeTitleScreen (main.asm.txt:2653-2657). */
        if (screen == SCREEN_LEADERBOARD) {
            bool typing = leader_type(buttons, pressed);
            /* ONE TABLE PER MODE, and the page off the title walks them:
             * the title's skin's, then 2 PLAYER, COOPERATIVE, VERSUS and
             * WITH COMPUTER, round, the heading naming each. */
            bool browsed = false;
            if (leader_browsing && (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT))) {
                leader_browse(front_skin(), (pressed & TENGEN_BTN_RIGHT) ? 1 : -1);
                cursor_blip();
                leader_frames = 0;
                browsed = true;
            }
            if (typing) {
                leader_frames = 0;
            } else if (++leader_frames >= (leader_nothing_new
                                                ? LEADER_NOTHING_NEW_FRAMES
                                                : LEADER_HOLD_FRAMES) ||
                        (pressed & (TENGEN_BTN_START | TENGEN_BTN_A |
                                     TENGEN_BTN_B))) {
                /* ...and the title's HIGH SCORE is its own table's again. */
                if (leader_browsing) leader_use_table(front_skin());
                leader_browsing = false;
                /* THE DEMO'S TABLE GOES BACK AS IT WAS, and the demo ends
                 * here, as it does on its own without the chord. */
                if (g_demo) {
                    leader_demo_end();
                    g_demo = false;
                    demo_music_back();
                    demo_mode_back(&game_mode);
                    g_ai_active = false;
                }
                screen = SCREEN_TITLE;
                restart_title_sprites();
                g_front_tune = FRONT_NOTHING;
                vsync();
                audio_frame();
                clear_screen();
                oam_hide_all();
                continue;
            }
            vsync();
            /* Only the row being typed into changes, so only it is redrawn —
             * six hundred tiles a frame for a blinking letter would be a
             * whole vertical blank spent on nothing. Another table is the
             * heading and the rows, not the frame round them. */
            if (browsed) draw_leader_table();
            if (g_leader_row >= 0) draw_leader_row(g_leader_row);
            /* ...and the rival's rows once, on the frame their name lands —
             * ALL of them: one per game, if they restarted with A+B. */
            if (swap_landed && !swap_drawn && leader_rival_row() >= 0) {
                draw_rival_leader_rows();
                swap_drawn = true;
            }
            audio_frame();
            continue;
        }

        /* The level-up interlude holds the game still while the dancers
         * perform, the way the ROM switches to its bonus state. Any button
         * cuts it short, which is what the original does too
         * (main.asm.txt:9037-9045). It never runs in a linked match — see the
         * note above announce_step. */
        if (g_dancer_active) {
            /* One step of checkLevelUp: the timer advances every sixteenth
             * frame, a button jumps it to the wind-down, and the show ends
             * when it would pass $FF. */
            if (g_dancer_timer == DANCER_TIMER_WINDDOWN) {
                /* L9053, reached by `beq` BEFORE the silence: the natural end
                 * of the show does not cut the level-up music, a button does. */
                g_dancer_timer = DANCER_TIMER_TAIL;
                g_dancer_tick = 0;
            } else if (g_dancer_timer < DANCER_TIMER_WINDDOWN && pressed) {
                /* EIGHT-BIT ARITHMETIC, and the comparison is unsigned — that
                 * is the whole behaviour. `lda #$7C / sec / sbc timer / sbc #5`
                 * underflows, so an early press lands on $FB and a late one on
                 * $F5, and the clamp only catches what wrapped past it. Doing
                 * this in an int made every press give $F5, which is twice the
                 * wind-down the cartridge gives you for pressing at once. */
                uint8_t jump =
                    (uint8_t)(DANCER_TIMER_START - g_dancer_timer - 5);
                if (jump < DANCER_TIMER_TAIL) jump = DANCER_TIMER_TAIL;
                g_dancer_timer = jump;
                /* `and #$F0` on frameCounterLow: the next step starts fresh. */
                g_dancer_tick = 0;
                nes_audio_play(NES_MUSIC_SILENCE);
            }

            if (++g_dancer_tick >= DANCER_TICK_FRAMES) {
                g_dancer_tick = 0;
                if (g_dancer_timer == 0xFF) {
                    g_dancer_active = false;
                } else {
                    g_dancer_timer++;
                }
            }
            /* ...and where the intro runs out, the show's own tune takes
             * over, which is L8D6B's half of the level-up music. Not if the
             * player has already cut the show short: that path silences the
             * music on purpose and starting a looping tune into the last few
             * frames of a wind-down would be a stutter, not music. */
            if (g_dancer_elapsed == LEVELUP_LOOP_FRAMES &&
                g_dancer_timer < DANCER_TIMER_TAIL) {
                nes_audio_play(NES_MUSIC_LEVELUP);
            }
            g_dancer_elapsed++;
            /* One frame of the choreography: see the driver in gba/hud.c. */
            dancers_step(g_dancer_elapsed);
            bonus_step();

            vsync();
            if (g_bonus_dirty) {
                draw_bonus_static();
                draw_bonus_numbers();
                g_bonus_dirty = false;
            }
            /* THE SCORE HAS TO BE SEEN CLIMBING. That is what the tally is —
             * L8EA2 adds to it a clear at a time — and draw_panel does not run
             * during the interlude, so the counter would sit at what it read
             * when the level turned over and jump when the board came back. */
            if (!g_session.game.coop && g_bonus_showing) {
                g_panel_layer = true;
                draw_counter(ROW_SCORE, HUD_LABEL_SCORE,
                              g_session.game.player[g_view].score, 6, 0);
                g_panel_layer = false;
            }
            if (!g_dancer_active) {
                bonus_end();
                oam_hide_all();
                /* finishLevelUpAnimation empties the level's bonus tally on
                 * its way back to play (main.asm.txt:2476-2482), so the next
                 * level's cast is counted from zero. */
                tengen_clear_bonus_counts(&g_session.game);
                draw_static_screen();
                start_music(g_music);
            } else if (hud_stats()) {
                /* HUD STATS GIVES THE SHOW ITS LEFT BOX, on either screen:
                 * the histogram in the right one is what the HUD is for and
                 * stays, the counters on the left go for the length of the
                 * show, and the troupe walks on there from the screen's edge.
                 * The resident over the histogram dances it in place. See
                 * draw_stats_show. */
                draw_stats_show(g_dancer_elapsed, g_dancer_cast);
            } else if (g_session.game.coop) {
                /* COOP HAS THE STAGE ALREADY: the ledges are part of the
                 * cartridge's own screen, four of them down each panel, and
                 * the eight dancers' feet land on them. All that has to go is
                 * the HUD text sharing those panels — the dancers walk
                 * straight through where LINES is printed. */
                clear_panel_region(COOP_L_TX, 0, COOP_PANEL_W, SCREEN_TH);
                clear_panel_region(COOP_R_TX, 0, COOP_PANEL_W, SCREEN_TH);
                /* THE WHOLE TOP COMPARTMENT, not the three rows NEXT's label
                 * and piece were assumed to fit in. It is six rows deep — the
                 * braid to the first ledge — and a piece that reached past the
                 * third left its bottom row standing behind the dancers. This
                 * is draw_coop_next's own clear, which is the guarantee that
                 * it cannot reach the ledges they stand on. */
                clear_both(COOP_L_TX, BRAID_T, COOP_PANEL_W,
                            COOP_LEDGE_FIRST - BRAID_T);
                clear_both(COOP_R_TX, BRAID_T, COOP_PANEL_W,
                            COOP_LEDGE_FIRST - BRAID_T);
                draw_coop_dancers(g_dancer_elapsed, g_dancer_cast);
            } else if (hud_versus()) {
                /* HUD VERSUS HAS TWO PANELS AND FOUR LEDGES A SIDE, which is
                 * coop's shape, so the troupe comes on there rather than on a
                 * stage blitted over the right column — and the counters go
                 * for the length of the show, exactly as they do in coop. See
                 * draw_race_dancers for what the cartridge does instead and
                 * why the port cannot. */
                clear_panel_region(BOX_L_TX, 0, BOX_W, SCREEN_TH);
                clear_panel_region(BOX_R_TX, 0, BOX_W, SCREEN_TH);
                /* THE INTERIOR, EIGHT WIDE, and not the box's ten: the two
                 * past the interior are the left box's inner rope, and the
                 * show left a hole in it from the braid to the first ledge
                 * (`--showframe`). */
                clear_both(BOX_L_TX, BRAID_T, BOX_IN, SHELF_FIRST - BRAID_T);
                clear_both(BOX_R_IN, BRAID_T, BOX_IN, SHELF_FIRST - BRAID_T);
                clear_stats_layer();
                draw_race_dancers(g_dancer_elapsed, g_dancer_cast);
            } else {
                /* The stage gets the WHOLE INTERIOR of the box, the way the
                 * cartridge's level-up blit gets the whole banner. Painting
                 * only the stage's own tiles left the NEXT box behind it —
                 * with a dancer standing inside it — and half of the STATS
                 * heading showing between the ledges.
                 *
                 * THE INTERIOR AND NOT THE COLUMN. It used to wipe the column
                 * from the screen's top and put back a plain strip of rope
                 * down the board's side, which is what this screen had before
                 * the boxes: for the length of every show the box lost its
                 * top run and its corner, and the rope by the board turned
                 * into the old one (`--showframe`). The banner lives inside
                 * the box, and so does the stage that takes its place. */
                clear_region(BOX_R_IN, BOX_TOP_IN, BOX_IN,
                             SCREEN_TH - BOX_TOP_IN);
                clear_stats_layer();
                draw_dancer_stage();
                /* Nothing here redraws NEXT any more and nothing has to: it
                 * lives in the LEFT panel's top compartment in both HUDs now,
                 * and the stage only ever touches the right column. It used to
                 * be lodged in the rows the stage was about to take. */
                draw_dancers(g_dancer_elapsed, g_dancer_cast);
            }
            audio_frame();
            continue;
        }

        /* THE HUD IS THE FEATURE AND THE COSSACK IS THE EXTRA, so the plain
         * button is the HUD's and the chord is the cossack's. It was the
         * other way round: SELECT cycled the dancer's palette and L+R swapped
         * the box, which put the thing you change once in a session on the
         * easy button and the thing you actually use on a two-hand chord.
         *
         * SELECT swaps the right-hand box between the piece histogram and the
         * cartridge's vertical TETRIS banner — while there is a match to swap
         * it around. Once the board is dead the only thing left to press is
         * the one that starts again, and there is nothing to swap on a coop
         * screen: it has no boxes, and the banner's column is the middle of
         * the board. The game proper never reads SELECT — the cartridge's own
         * pause and cheat codes are on Start and the face buttons — so it is
         * free. */
        /* ...AND COOP SWAPS TOO, BUT ONLY AGAINST THE MACHINE AND ONLY UNDER
         * THE CHORD. Its other HUD hides the partner's board, which against
         * the computer is a difficulty setting and against a person over the
         * cable is just less game — and it carries T.SCORE and T.LINES, the
         * board's totals, which the panel itself only prints once the cheats
         * have been found. A screen reachable with SELECT that shows what the
         * chord is supposed to unlock is not a locked door. See
         * draw_coop_stats_panel and g_pause_unlocked. */
        int hud_count = 1;
        (void)hud_set(&hud_count);
        bool hud_swappable = screen == SCREEN_PLAYING && match_running &&
                              !g_session.game.paused && !g_link_waiting &&
                              g_session.game.player[g_view].game_active &&
                              hud_count > 1;
        /* ...AND WITH HOLD ON, SELECT IS HOLD'S (OPT_HOLD, the core reads
         * it) and L+R does this instead: the cossack whose colours they
         * changed is not in HUD STATS any more. */
        bool hud_key = hold_on() ? shoulder_chord()
                                 : (pressed & TENGEN_BTN_SELECT) != 0;
        if (hud_swappable && hud_key) {
            {
                int n;
                const uint8_t *set = hud_set(&n);
                int at = 0;
                for (int i = 0; i < n; i++) if (set[i] == g_hud) at = i;
                g_hud = set[(at + 1) % n];
                hud_remember();
            }
            /* Both directions need the static screen back: going TO the
             * banner erases the braid box, and coming back from it has to
             * redraw one. Without this the box's border kept whatever the
             * banner had left in it. */
            g_repaint = true;
            screen_blip();
        }

        /* ...and the chord changes which cossack is standing in HUD STATS,
         * which is the only thing separating the cartridge's six dancers from
         * each other. It answers on the SAME terms as the swap above — not
         * over a dead board and not under a pause — because two doors into
         * the same screen that disagree about when they are open is worse
         * than either rule. The chirp is its own: the screen has not changed,
         * so the screen's blip would be a lie. */
        /* ...AND ONLY WHERE THERE IS A COSSACK TO RECOLOUR, which is HUD
         * STATS and nowhere else. On any other HUD the chord changed a
         * palette nothing on screen was using and chirped to say so, which is
         * a control that answers and does nothing. And only under the
         * match's chord: a cossack of another colour is the port's toy, not
         * the cartridge's. */
        if (hud_swappable && hud_stats() && !hold_on() && shoulder_chord() &&
            match_chord()) {
            g_idle_palette = (uint8_t)((g_idle_palette + 1) % IDLE_PALETTE_COUNT);
            nes_audio_play(NES_SOUND_CHIRP);
        }

        /* IN THE DEMO THE PAD IS NOT A CONTROLLER, it is the way out. The
         * cartridge writes the computer's choice straight into
         * player1ControllerNew, so what the game sees IS the computer — and
         * what the player presses reaches processMenuInput instead, where
         * SELECT or START leaves for GAME SELECT (main.asm.txt:4633-4638). */
        /* ...THE DEMO TWO TITLES SHARE THE SAME, but off the wire while it
         * plays: the press goes across like any other, and on the step both
         * consoles have it this one leaves and the other plays on alone
         * (link_play_frame, g_link_walked) — left off its own keypad, the
         * other console's demo stood still until it worked out it had been
         * left. Its computers are not this pad's either (link_demo_moves). */
        bool demo_out = false;
        if (g_demo) {
            bool shared = g_linked && match_running;
            if (!shared && (pressed & (MENU_ADVANCE | TENGEN_BTN_B))) {
                demo_out = true;
            } else if (!g_linked) {
                buttons = (match_running &&
                            g_session.game.player[TENGEN_PLAYER_1].game_active)
                    ? ai_input() : 0;
                pressed = buttons;
            }
        }

        bool quit_match = false;
        if (match_running && !demo_out) {
            bool keep_going = g_linked ? link_play_frame(pressed, &quit_match)
                                        : solo_play_frame(buttons, pressed,
                                                           &quit_match);
            if (!keep_going) {
                match_running = false;
                /* EXIT on a linked pause menu: both consoles took it on the
                 * same transfer, so both put the cable away here and go the
                 * way a solo EXIT goes. */
                if (g_linked && g_link_alone) {
                    /* The other player left this race a while ago and the
                     * cable was put away then (link_go_alone): nobody to
                     * swap a name with. */
                } else if (g_linked && (quit_match || g_link_lost ||
                                        g_link_walked || g_demo)) {
                    /* ...and a cable gone for good takes nobody's name
                     * across: there is no cable to take it (link_give_up).
                     * Nor does a player who leaves a race the other is
                     * still playing (g_link_walked): that console has gone
                     * on alone, its cable put away on the same step. Nor
                     * do a demo's two computers, which have none. */
                    link_play_end();
                    link_shutdown();
                } else if (g_linked) {
                    /* THE HARDWARE DOES NOT GO AWAY HERE ANY MORE. It used
                     * to, and that is why the rival's name could not cross:
                     * by the time there was a name to send, the cable was
                     * already put away. link_name_start takes the send
                     * register back off the interrupt, which is what
                     * link_play_end did on its own before. */
                    link_name_start(&name_swap);
                    swapping = true;
                    swap_sent = false;
                    swap_landed = false;
                    swap_drawn = false;
                }
            }
        }
        if (g_demo && g_link_walked) {
            g_link_walked = false;
            demo_out = true;
        }
        if (demo_out) {
            g_demo = false;
            demo_music_back();
            demo_mode_back(&game_mode);
            g_ai_active = false;
            match_running = false;
            screen = SCREEN_GAME_SELECT;
            screen_blip();
            vsync();
            audio_frame();
            clear_screen();
            oam_hide_all();
            continue;
        }

#ifndef TENGEN_MULTIBOOT
        /* ON THE PAUSE PLAQUE THE GAME GOES TO THE BATTERY (suspend.c), and
         * again if the tune is changed under it; off the plaque — played
         * on, over, or left — it is wiped. LEFT is `quit_match`: EXIT on the
         * pause menu leaves with the game still paused and still "running"
         * this frame (the way out is below), and was kept. */
        {
            bool keep = match_running && !quit_match && !g_linked &&
                        !g_demo && g_session.game.paused;
            if (keep && (!suspended || g_music != suspended_music)) {
                SuspendMenu m = kept_menu(game_mode, start_level, handicap);
                suspend_save(&m);
                suspended = true;
                suspended_music = g_music;
            } else if (!keep && suspended) {
                suspend_clear();
                suspended = false;
            }
        }
#endif

        /* THE DEMO SEES ITSELF OUT. Nothing is waiting for a button here, so
         * the game over holds for a moment and the title comes back. */
        /* ...EXCEPT UNDER THE CHORD, where the demo's game ends as a game
         * does: the plaque and the game's page on the board, for as long as
         * a game's (GAMEOVER_HOLD_FRAMES and END_PAGE_READ_FRAMES), and then
         * the HIGH SCORES with its row on them as the machine's, if it made
         * the table — never typed, never saved (leader_demo_show) — and the
         * title after them. */
        if (g_demo && !match_running && g_pause_unlocked) {
            if (++g_demo_over_frames >= GAMEOVER_HOLD_FRAMES +
                    (g_end_rise >= 0 ? END_PAGE_READ_FRAMES : 0)) {
                leader_demo_show();
                leader_frames = 0;
                leader_browsing = false;
                leader_nothing_new = true;
                screen = SCREEN_LEADERBOARD;
                oam_hide_all();
                vsync();
                audio_frame();
                clear_screen();
                draw_leaderboard();
                continue;
            }
        } else if (g_demo && !match_running) {
            /* (Never the demo two titles share, which is under the chord
             * and ends above: nothing of its to give back here.) */
            if (++g_demo_over_frames >= DEMO_GAMEOVER_FRAMES) {
                g_demo = false;
                demo_music_back();
                g_ai_active = false;
                screen = SCREEN_TITLE;
                g_front_tune = FRONT_NOTHING;
                vsync();
                audio_frame();
                clear_screen();
                oam_hide_all();
                restart_title_sprites();
                continue;
            }
        }

        /* Any of the three goes back to the title once there is nothing left
         * to play. Start is the cartridge's own, and after a game over A and
         * B are the buttons a hand is already on.
         *
         * In a linked match this is read straight off this console's keypad
         * rather than over the cable — by now the cable is shut down, and
         * neither player should have to wait for the other to agree. */
#define GAMEOVER_RESTART (TENGEN_BTN_START | TENGEN_BTN_A | TENGEN_BTN_B)
        /* ...EXCEPT WHILE THE OTHER BOARD IS STILL GOING, where A and B have
         * a job of their own: held together they put this one back on its
         * feet (handleGameOver, main.asm.txt:472-478, and the note on
         * tengen_restart_player). A button that both leaves the race and
         * starts it again is a button that does neither reliably, so in that
         * one state the way out is START and nothing else. */
        /* THE BUTTON BELONGS TO THE BOARD IN FRONT OF THE PLAYER, not to the
         * match. The cartridge reads it per player — handleGameOver is called
         * with x on the dead side and restarts from there even while the other
         * board is still going (main.asm.txt:82F3-830F) — and a race where the
         * loser has to sit and wait for the winner is a race nobody can leave.
         * So: a dead board here is a way out here. */
        /* NOT IN THE DEMO, and the whole condition has to say so rather than
         * just the dead-board half of it. There the pad is not a controller:
         * the computer's choice is what lands in `pressed`, so its own A or B
         * would read as a player asking to leave — and on the one frame the
         * demo's board dies, `pressed` still holds the computer's last press
         * while `match_running` has just gone false. That put the attract mode
         * on the HIGH SCORES page with a score nobody played for. The demo
         * sees itself out above. */
        /* ...AND THAT START IS TAKEN OFF THE CABLE, not off this keypad
         * (link_play_frame, g_link_walked): the two consoles have to agree
         * on the step it happened, or the one left behind waits for moves
         * that are not coming. Off the keypad only while the cable is
         * quiet (LINK ISSUES), when no word of it would get across. */
        bool own_board_dead = !g_session.game.player[g_view].game_active;
        uint8_t leave_buttons = (match_running && own_board_dead)
                                 ? (g_link_waiting ? TENGEN_BTN_START : 0)
                                 : GAMEOVER_RESTART;
        bool walked = g_link_walked;
        g_link_walked = false;
        /* AND THE PLAQUE LEAVES ON ITS OWN, WHICH IS THE CARTRIDGE'S WAY.
         * Its game over is a countdown to the high scores, not a prompt (see
         * GAMEOVER_HOLD_FRAMES); the port sat on the plaque until somebody
         * pressed something, so a game you lost and walked away from stayed
         * lost on the screen. The counter runs only once EVERY board is dead
         * — the ROM enters $F9 on `player1GameActive ORA player2GameActive`
         * reaching zero (main.asm.txt:599-607), so in VERSUS the winner plays
         * on and nothing is counting. */
        /* ...LONGER WITH THE GAME'S PAGE ON THE BOARD (END_PAGE_READ_FRAMES),
         * which only the chord puts there. */
        bool over_expired = false;
        if (!g_demo && !match_running && !quit_match && !g_link_lost)
            over_expired = ++over_frames >= GAMEOVER_HOLD_FRAMES +
                                            (g_end_rise >= 0 ? END_PAGE_READ_FRAMES : 0);
        /* ...and EXIT on the pause menu takes the same road, so a game you
         * quit still puts its score on the board. */
        if (!g_demo && (quit_match || over_expired || walked ||
                         ((!match_running || own_board_dead) &&
                          (pressed & leave_buttons)))) {
            g_pause_confirm = false;
#ifndef TENGEN_MULTIBOOT
            /* EXIT GAME's SAVE: the game to the battery as it stands, paused,
             * before it is torn down; GAME SELECT brings it back. */
            if (quit_match && g_pause_saving) {
                SuspendMenu m = kept_menu(game_mode, start_level, handicap);
                saved_write(&m);
            }
#endif
            g_pause_saving = false;
            /* Quitting out from under a match still running on the other side
             * of the cable: the cable has to be told, and put away, exactly as
             * it would have been had both boards died. */
            if (match_running) {
                match_running = false;
                if (g_linked) {
                    link_play_end();
                    link_shutdown();
                }
            }
            /* AND THE TABLE COMES NEXT — unless you QUIT, in which case it
             * does not. The cartridge's own road out of a finished game runs
             * through its HIGH SCORES page (the game over counts down and
             * jumps to initializeLeaderboard, main.asm.txt:2643-2675), and a
             * game that ENDED takes it. A game you walked out of has not
             * ended: you killed it, and a score you abandoned has no business
             * on the board. */
            /* NOTHING IS LEFT OF THE TORN-DOWN GAME, THE HOLD INCLUDED — and
             * the hold is two things, not one. The flag is this port's, and
             * leaving it lying about is a paused game handed to the next
             * screen; the SUSPEND is the cartridge engine's, and it gags the
             * EFFECTS as well as the music, so a game abandoned under the
             * plaque and never resumed leaves the whole machine mute. See the
             * note by MUSIC_RESUME in pause_menu_input: the way out through
             * EXIT sends its own resume first, so that the blip it plays on
             * the way is heard rather than swallowed. This is for the roads
             * that do not — a cable pulled out from under a held match. */
            if (g_session.game.paused && !quit_match)
                nes_audio_play(NES_MUSIC_RESUME);
            g_session.game.paused = false;
            /* A cable gone for good leaves the way EXIT does: to the title,
             * with nothing on the high scores for a match nobody finished. */
            if (quit_match || g_link_lost) {
                screen = SCREEN_TITLE;
                restart_title_sprites();
            } else {
                leader_submit();
                /* (The chord the MATCH was under, which over the cable is
                 * the master's: the two consoles leave the page on the same
                 * clock, whatever each had rung.) */
                leader_nothing_new = match_chord() && g_leader_row < 0;
                screen = SCREEN_LEADERBOARD;
            }
            leader_frames = 0;
            g_linked = false;
            g_link_lost = false;
            g_link_waiting = false;
            g_view = 0;
            oam_hide_all();
            sweeping = false;
            /* THE GAME-OVER TUNE IS NOT CUT OFF ON ITS WAY TO THE TABLE.
             * initializeLeaderboard sets no music at all (main.asm.txt:2963
             * onward): whatever was playing carries on, which for a game that
             * just ended is its own jingle. Silencing it here meant a quick
             * hand on Start heard the plaque's music stop dead. The title,
             * which does want its own theme, still gets the silence. */
            if (screen != SCREEN_LEADERBOARD) stop_music();
            /* THE TITLE HAS TO BE ASKED FOR AGAIN. Coming back here left the
             * match's screen underneath — including the statistics, which
             * live on their own background and so survived even a redraw of
             * the title's tiles and printed themselves over the cathedral.
             * And g_front_tune still held whatever the last menu chose, so
             * the title's own theme could decide it was already playing. */
            clear_screen();
            g_front_tune = FRONT_NOTHING;
            vsync();
            audio_frame();
            /* ONLY IF THAT IS WHERE WE ARE GOING. This drew the page whatever
             * the destination was, so quitting out through the pause menu put
             * one frame of HIGH SCORES on the screen on its way to the title
             * — a flash, and a flash of exactly the thing quitting is
             * supposed not to do. */
            if (screen == SCREEN_LEADERBOARD) draw_leaderboard();
            continue;
        }

        draw_match(&sweeping);
    }
}
