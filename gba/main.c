/*
 * main.c -- the state machine.
 *
 * Which screen is up, what a button does on it, and where it goes next.
 * All the rules live in ../src/tengen_core.c and know nothing about the
 * GBA; all the drawing lives in the other four files. This one is the
 * loop that calls both.
 */
#include "port.h"


int main(void) {
    upload_tiles();
    upload_palettes();
    /* What the battery kept, or the cartridge's cold-boot table if there is
     * nothing there to keep. See leader_load. */
    leader_reset();
    leader_load();
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

    Screen screen = SCREEN_TITLE;
    int leader_frames = 0;
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
    int link_wait_frames = 0;
    uint8_t held_last = 0;
    bool sweeping = false;   /* true while the line-clear sweep owns the OAM */
    bool match_running = false;
    TengenLobby lobby;

    /* The ROM steps its RNG once per frame from the main loop
     * (main.asm.txt:49-50), and whatever state it is in when Start is pressed
     * becomes the game's seed. Doing the same means the piece sequence
     * depends on when you start rather than being identical every session. */
    TengenRng seed_source;
    tengen_rng_seed(&seed_source, 0xACE1);

    for (;;) {
        uint8_t buttons = read_buttons();
        uint8_t pressed = (uint8_t)(buttons & ~held_last);
        held_last = buttons;
        tengen_rng_step(&seed_source);

        if (screen == SCREEN_TITLE) {
            /* initializeTitleScreen ends with this (main.asm.txt:4489). */
            front_music(FRONT_TITLE_THEME);
            set_offset_layer(TITLE_LOGO_SHIFT_PX);
            /* L+R FINDS IT; L AND R ALONE DRIVE IT AFTERWARDS.
             *
             * The chord is the discovery, and it is the only cheat on this
             * screen: it unlocks NOTHING else — a player who finds the
             * prototype title has not thereby found the hidden tunes (see
             * unlock_cheats). Until it is rung, a lone shoulder does nothing
             * at all, so the title cannot be changed by accident.
             *
             * After it, though, the chord is a poor control. There are four
             * skins and it only ever went forwards, so going back to the one
             * you just passed meant going round the other three — which is
             * exactly what was reported. So once found, R steps forward and L
             * steps back, and the chord itself stands down. */
            int skin_step = 0;
            if (TITLE_SKIN_COUNT > 1) {
                bool chord = shoulder_chord();
                bool tap_l = pressed_shoulder(SHOULDER_L);
                bool tap_r = pressed_shoulder(SHOULDER_R);
                if (!g_title_skin_found) {
                    if (chord) { g_title_skin_found = true; skin_step = 1; }
                } else if (!chord && (tap_l || tap_r)) {
                    skin_step = tap_r ? 1 : TITLE_SKIN_COUNT - 1;
                }
            }
            if (skin_step) {
                g_title_skin = (uint8_t)((g_title_skin + skin_step) %
                                          TITLE_SKIN_COUNT);
                /* ...and the HIGH SCORE the menus print is this build's, from
                 * this build's table. See LEADER_TABLES. */
                leader_use_table(front_skin());
                nes_audio_play(NES_SOUND_CHIRP);
                vsync();
                /* CLEAR FIRST, then the tiles: a skin swap rewrites the whole
                 * 256-tile prototype bank, which is far more than a vblank
                 * holds, and with the map already blank none of it is on
                 * screen while it is being written. */
                clear_screen();
#if SCREEN_PROTO_AVAILABLE
                if (g_title_skin) upload_proto_tiles(g_title_skin - 1);
#endif
                draw_title();
                oam_hide_all();
                audio_frame();
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
            /* ...and if nobody presses anything, the cartridge starts playing
             * by itself. See DEMO_START_FRAME. */
            if (g_title_frame >= DEMO_START_FRAME) {
                uint16_t seed = (uint16_t)(seed_source.lo | (seed_source.hi << 8));
                g_demo = true;
                g_linked = false;
                g_link_lost = false;
                g_view = 0;
                g_ai_active = true;
                g_ai_slot = TENGEN_PLAYER_1;   /* the demo's computer is P1 */
                tengen_new_game(&g_session.game, seed, DEMO_START_LEVEL,
                                 false, false, false);
                tengen_ai_reset(&g_ai);
                /* THE DEMO LOOKS AT A PIECE BEFORE IT MOVES IT. Nothing else
                 * separates the attract mode from a machine twitching the pad
                 * the instant a piece appears; see `settle` on TengenAi. And
                 * no soft drop here — the attract mode keeps the cartridge's
                 * pace, which is the pace it is meant to be showing off. */
                g_ai.settle = DEMO_SETTLE_FRAMES;
                g_ai_last_piece = TT_NONE;
                g_ai_last_partner = TT_NONE;
                g_ai_frame = 0;
                points_clear();
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
                over_frames = 0;
                skin_begin_match(false);
                /* MUSIC_SUSPEND, which is demoStart's own second act
                 * (main.asm.txt:3220-3221): the attract mode is silent but
                 * for the game's effects. */
                g_front_tune = FRONT_NOTHING;
                stop_music();
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
            if (shoulder_chord() && unlock_cheats()) {
                /* Nothing on this page shows for it — the tunes are on the
                 * next one and the menu is in a game — so the chirp is the
                 * whole answer, and it is the same one the next page gives. */
            }
            if (pressed & MENU_STEP) {
                game_mode = (pressed & MENU_BACKWARD)
                    ? (uint8_t)((game_mode + GAME_COUNT - 1) % GAME_COUNT)
                    : (uint8_t)((game_mode + 1) % GAME_COUNT);
                /* processMenuInput plays this on every move (:4655). */
                cursor_blip();
            }
            if (pressed & TENGEN_BTN_B) {
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
                    screen = SCREEN_LINK_WAIT;
                } else {
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
            draw_game_select(game_mode);
            audio_frame();
            continue;
        }

        if (screen == SCREEN_LEVEL_SELECT) {
            /* THE CABLE KEEPS TURNING while the master reads the menu. The
             * handshake is parked at its greeting, but it still has to happen:
             * a lobby that stops transferring looks exactly like a lobby whose
             * cable fell out, and the guest would give up after ten seconds of
             * the master thinking. */
            if (GAME_IS_LINKED(game_mode)) link_lobby_step(&lobby);

            /* ONE CALL EACH, and the results kept: these are edge detectors
             * with their own held state, so asking twice in a frame answers
             * "yes" and then "no" — which is how the handicap's first version
             * quietly ate the hidden tunes' chord. */
            bool chord = shoulder_chord();

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
            bool two_handicaps = (game_mode == GAME_2P || game_mode == GAME_VS);
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
            front_music(g_music);

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
            if (pressed & MENU_CONFIRM) {
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
                    screen = SCREEN_LINK_WAIT;
                    vsync();
                    audio_frame();
                    clear_screen();
                    continue;
                }

                g_linked = false;
                g_link_lost = false;
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
                tengen_ai_reset(&g_ai);
                /* IT DROPS ITS OWN PIECES NOW, AND LOOKS AT THEM FIRST.
                 * The ROM's computer never presses down, which costs nothing
                 * when it has a board to itself and costs the human the whole
                 * game on the shared board of WITH COMPUTER — a piece of its
                 * own took a full level-0 descent, and that is what "va un
                 * tanto lento" was. Holding down once it is lined up fixes
                 * that outright, and then overshoots: it would place ten
                 * pieces to a free-falling human's one. The settle is what
                 * buys the pace back, and it reads as thinking rather than as
                 * a handicap. Measured, on an idle board: 36 cells in three
                 * thousand frames before, 136 with both of these, 189 with
                 * the drop and no settle at all. */
                g_ai.soft_drop = true;
                g_ai.settle = AI_SETTLE_FRAMES;
                g_ai_last_piece = TT_NONE;
                g_ai_last_partner = TT_NONE;
                g_ai_frame = 0;
                points_clear();
                swallow_held_buttons(&g_session.game);
                /* endPlayfieldInit's own place for it, right after the field
                 * is laid out (main.asm.txt:3536-3546). A shared board takes
                 * one burial, not two. */
                tengen_apply_handicap(&g_session.game, TENGEN_PLAYER_1, handicap[0]);
                if (g_ai_active && !g_session.game.coop)
                    tengen_apply_handicap(&g_session.game, TENGEN_PLAYER_2,
                                           handicap[1]);
                g_mix_step = 0;      /* every game opens on the same tune */
                g_shown_level = 0xFF;
                g_shown_piece = TT_NONE;
                set_piece_palette(g_session.game.player[0].piece.current);
                oam_hide_all();
                g_idle_frame = 0;
                g_dance_frames = 0;
                screen = SCREEN_PLAYING;
                match_running = true;
                over_frames = 0;
                skin_begin_match(false);
                g_front_tune = FRONT_NOTHING;
                start_music(g_music);
                vsync();
                audio_frame();
                clear_screen();
                draw_static_screen();
                continue;
            }
            vsync();
            /* Only a RACE has two handicaps — two boards to bury. A shared
             * board, coop's or WITH COMPUTER's, takes one. */
            draw_level_settings(menu_field, start_level, g_music, handicap,
                                 game_mode == GAME_2P || game_mode == GAME_VS,
                                 handicap_who);
            audio_frame();
            continue;
        }

        if (screen == SCREEN_LINK_WAIT) {
            /* Quiet while the cable is looking for the other end; there is
             * nothing to preview yet. */
            front_music(FRONT_SILENCE);
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

            if (pressed & TENGEN_BTN_B) {
                screen = SCREEN_GAME_SELECT;
                link_shutdown();
                oam_hide_all();
                screen_blip();
                vsync();
                audio_frame();
                clear_screen();
                continue;
            }

            if (lobby.ready) {
                /* The cable master is player 1. That is not a convention this
                 * port invented; it is the one fact both consoles can agree
                 * on without asking, because the hardware sets it from which
                 * end of the cable each is plugged into. */
                g_linked = true;
                g_link_lost = false;
                g_ai_active = false;
                g_view = link_is_master() ? 0 : 1;
                /* The master's choice wins, the egg included: both consoles run
                   the same ROM, so a linked player who never found the code
                   still hears it. */
                g_music = lobby.music < MUSIC_UNLOCKED_COUNT ? lobby.music : 0;
                tengen_link_start(&g_session, lobby.seed, lobby.start_level,
                                   link_is_master() ? TENGEN_PLAYER_1 : TENGEN_PLAYER_2,
                                   lobby.coop, lobby.xe);
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
                set_piece_palette(g_session.game.player[g_view].piece.current);
                link_play_begin();
                screen = SCREEN_PLAYING;
                match_running = true;
                over_frames = 0;
                skin_begin_match(true);
                g_front_tune = FRONT_NOTHING;
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
            if (typing) {
                leader_frames = 0;
            } else if (++leader_frames >= LEADER_HOLD_FRAMES ||
                        (pressed & (TENGEN_BTN_START | TENGEN_BTN_A |
                                     TENGEN_BTN_B))) {
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
             * whole vertical blank spent on nothing. */
            if (g_leader_row >= 0) draw_leader_row(g_leader_row);
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
            if (g_dancer_elapsed == LEVELUP_INTRO_FRAMES &&
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
            } else if (!g_show_banner) {
                /* HUD STATS keeps its screen. Nothing is cleared and nothing
                 * has to be put back; the panel redraws every frame anyway,
                 * and the cossack standing in it takes the show. */
                draw_panel();
            } else {
                /* The stage gets the WHOLE column, the way the cartridge's
                 * level-up blit gets the whole banner. Painting only the
                 * stage's own tiles left the NEXT box behind it — with a
                 * dancer standing inside it — and half of the STATS heading
                 * showing between the ledges. */
                clear_region(BOX_R_TX + 2, 0, BOX_W - 2, SCREEN_TH);
                clear_stats_layer();
                draw_field_braid(BOX_R_TX, kBraidRight);
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
        /* ...AND COOP SWAPS TOO, BUT ONLY AGAINST THE MACHINE. Its other HUD
         * hides the partner's board, which against the computer is a
         * difficulty setting and against a person over the cable is just
         * less game. See draw_coop_stats_panel. */
        bool hud_swappable = screen == SCREEN_PLAYING && match_running &&
                              !g_session.game.paused &&
                              g_session.game.player[g_view].game_active &&
                              (!g_session.game.coop || (g_ai_active && !g_linked));
        if (hud_swappable && (pressed & TENGEN_BTN_SELECT)) {
            g_show_banner = !g_show_banner;
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
        if (hud_swappable && shoulder_chord()) {
            g_idle_palette = (uint8_t)((g_idle_palette + 1) % IDLE_PALETTE_COUNT);
            nes_audio_play(NES_SOUND_CHIRP);
        }

        /* IN THE DEMO THE PAD IS NOT A CONTROLLER, it is the way out. The
         * cartridge writes the computer's choice straight into
         * player1ControllerNew, so what the game sees IS the computer — and
         * what the player presses reaches processMenuInput instead, where
         * SELECT or START leaves for GAME SELECT (main.asm.txt:4633-4638). */
        if (g_demo) {
            if (pressed & (MENU_ADVANCE | TENGEN_BTN_B)) {
                g_demo = false;
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
            buttons = (match_running &&
                        g_session.game.player[TENGEN_PLAYER_1].game_active)
                ? ai_input() : 0;
            pressed = buttons;
        }

        bool quit_match = false;
        if (match_running) {
            bool keep_going = g_linked ? link_play_frame()
                                        : solo_play_frame(buttons, pressed,
                                                           &quit_match);
            if (!keep_going) {
                match_running = false;
                if (g_linked) {
                    link_play_end();
                    link_shutdown();
                }
            }
        }

        /* THE DEMO SEES ITSELF OUT. Nothing is waiting for a button here, so
         * the game over holds for a moment and the title comes back. */
        if (g_demo && !match_running) {
            if (++g_demo_over_frames >= DEMO_GAMEOVER_FRAMES) {
                g_demo = false;
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
        bool own_board_dead = !g_session.game.player[g_view].game_active;
        /* AND THE PLAQUE LEAVES ON ITS OWN, WHICH IS THE CARTRIDGE'S WAY.
         * Its game over is a countdown to the high scores, not a prompt (see
         * GAMEOVER_HOLD_FRAMES); the port sat on the plaque until somebody
         * pressed something, so a game you lost and walked away from stayed
         * lost on the screen. The counter runs only once EVERY board is dead
         * — the ROM enters $F9 on `player1GameActive ORA player2GameActive`
         * reaching zero (main.asm.txt:599-607), so in VERSUS the winner plays
         * on and nothing is counting. */
        bool over_expired = false;
        if (!g_demo && !match_running && !quit_match)
            over_expired = ++over_frames >= GAMEOVER_HOLD_FRAMES;
        /* ...and EXIT on the pause menu takes the same road, so a game you
         * quit still puts its score on the board. */
        if (!g_demo && (quit_match || over_expired ||
                         ((!match_running || own_board_dead) &&
                          (pressed & GAMEOVER_RESTART)))) {
            g_pause_confirm = false;
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
            if (quit_match) {
                screen = SCREEN_TITLE;
                restart_title_sprites();
            } else {
                leader_submit();
                screen = SCREEN_LEADERBOARD;
            }
            leader_frames = 0;
            g_linked = false;
            g_link_lost = false;
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
