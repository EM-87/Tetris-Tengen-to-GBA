/*
 * match.c -- one frame of play.
 *
 * The pause menu, the computer player's input, what a step of the core
 * has to announce, and the order the drawing happens in -- which is not
 * a detail here: vsync, then draw, then the audio in the time that is
 * left, or the falling piece tears.
 */
#include "port.h"



/* ----------------------------------------------------------------------- *
 * The match
 *
 * A solo game and a linked one share every line of the drawing and almost
 * every line of the game logic. They differ in exactly three places, which is
 * the point of keeping the lockstep in ../src/tengen_link.c:
 *
 *   - where the buttons come from: the keypad, or a completed transfer that
 *     carries BOTH consoles' buttons for one frame;
 *   - what advances the simulation: one call per frame, or one call per
 *     transfer, which is usually the same thing and occasionally is not;
 *   - whether the level-up interlude may stop the world. It may not over a
 *     cable: the two consoles would have to stop and resume on the same
 *     frame or the lockstep is over, and the cartridge does not send its
 *     dancers out during a two-player race in any case.
 * ----------------------------------------------------------------------- */

/* THE COMPUTER PLAYER, which needs neither a second console nor a cable: it
 * is player 2 in VERSUS and WITH, exactly as in the cartridge
 * (main.asm.txt:3736-3749), and it is handed the same TengenGame the human
 * is playing in. `g_ai_frame` stands in for frameCounterLow, whose low bits
 * are the whole of its cadence. */
TengenAi g_ai;
uint8_t g_ai_slot = TENGEN_PLAYER_2;
TengenTetromino g_ai_last_piece;
/* ...and the OTHER player's, which in WITH COMPUTER is a reason to think
 * again. See ai_input. */
TengenTetromino g_ai_last_partner;
uint8_t g_ai_frame;
bool g_demo;
int g_demo_over_frames;

/* One frame of the computer's play. It re-chooses on every new piece, which
 * is where getNextTetromino calls computerMove, and presses whatever the
 * driver says the rest of the time. */
static void ai_play_frame(void);

bool g_linked;            /* this match is running over the cable */
bool g_link_lost;         /* ...and the cable stopped answering */
bool g_repaint;           /* the static screen needs putting back */
uint8_t g_front_tune = FRONT_NOTHING;

/* NO MUSIC to start with — musicSelectTable's own first entry
 * (main.asm.txt:4741, "silence, loginska, bradinsky, karinka, troika") and
 * the quieter thing to hand somebody a handheld with. Anything else means a
 * tune starts playing the moment the settings screen comes up. */
uint8_t g_music = 0;

/* WHAT IS ACTUALLY PLAYING, which is not always what is selected: MUSIC MIX
 * is a rotation, not a tune, so anything that has to act on the tune itself —
 * pausing the fifth one, say — has to resolve it first. */
static uint8_t current_tune(void) {
    return g_music == MUSIC_MIX ? mix_tune() : g_music;
}

/* The interlude's clock, which is the ROM's player1FallTimer: `active` while
 * the show is on, `timer` counting $7C..$FF at one step every sixteen frames.
 * See the note beside DANCER_TIMER_START for why it is shaped like this. */
uint8_t g_dancer_timer;
uint16_t g_dancer_tick;   /* stands in for frameCounterLow & $0F */
int g_dancer_cast = 1;    /* how many walk on; see tengen_dancer_count */
uint8_t g_shown_level = 0xFF;
TengenTetromino g_shown_piece = TT_NONE;
TengenTetromino g_shown_piece2 = TT_NONE;  /* coop: the partner's */

/* The ROM's own cues, each at the moment it plays them:
 *  - a piece coming to rest, L8417 (main.asm.txt:637)
 *  - rows coming down, L95C1 (:3212) — unless that clear also raised the
 *    level, in which case the intro takes its place (:3207)
 *  - the level-up interlude itself, L8D6B (:2038)
 *  - topping out, silence and then the game-over tune (:608, :620) */
static void announce_step(TengenStepResult step) {
    if (step.piece_locked) nes_audio_play(NES_SOUND_DROP);
    /* ONE call per event, and the level-up is one event. A clear that also
     * raises the level used to reach setMusicOrSoundEffect(MUSIC_LEVELUP)
     * twice in the same frame — once for the clear and once for the level —
     * and the engine restarts a track every time it is handed one, so the
     * intro began, was cut off a few hundred cycles later and began again.
     * That is what a doubled tune sounds like. */
    if (step.lines_collapsed && !step.leveled_up)
        nes_audio_play(NES_SOUND_LINECLEAR);
    /* THE COSSACK ANSWERS THE BOARD. Counted off the mask the core reports, so
     * a clear that took four rows gets four times the figure. */
    if (step.lines_collapsed) {
        int rows = 0;
        for (int i = 0; i < TENGEN_PF_HEIGHT; i++)
            if (step.rows_cleared_mask & (1u << i)) rows++;
        idle_cossack_celebrate(rows);
    }
    if (step.leveled_up) {
        /* The cartridge's level-up music takes over; a hand-entered tune stands
         * down and start_music() puts it back when the dancers finish. */
        handtune_stop();
        /* THE INTRO, which is what the cartridge plays at this moment; the
         * looping tune comes in when the intro runs out. See
         * NES_MUSIC_LEVELUP_INTRO. A linked match and a prototype's game have
         * no show to hand it over to, and for them the jingle on its own is
         * the right answer anyway: it ends by itself, where the loop had to
         * be cut off by whatever came next. */
        nes_audio_play(NES_MUSIC_LEVELUP_INTRO);
        /* MUSIC MIX turns over here, and here only. See MUSIC_MIX. */
        if (g_music == MUSIC_MIX) {
            g_mix_step = (uint8_t)((g_mix_step + 1) % MIX_COUNT);
            /* A linked match has no interlude to restart the tune afterwards,
             * so the mix's next one is asked for on the spot. The level-up
             * jingle was queued a moment ago and the ring is read one request
             * per frame, so it is heard first and this follows it. */
            if (g_linked) start_music(g_music);
        }
        /* NO COSSACKS IN A PROTOTYPE'S GAME. Those builds go up a level and
         * carry straight on — no dancers, no BONUS tally — and the interlude
         * is one of the release's later additions. The level-up jingle above
         * still plays: it is a sound, not a show, and their level-up is not
         * silent. See proto_rules. */
        if (!g_linked && !g_session.game.proto_rules) {
            g_dancer_active = true;
            g_dancer_timer = DANCER_TIMER_START;
            bonus_begin();
            g_dancer_tick = 0;
            g_dancer_elapsed = 0;
            /* The cast is read HERE, before the tally is emptied: L8D8B runs
             * at the top of showLevelBonus, and how well the level went is
             * what decides how many cossacks come on. */
            g_dancer_cast = tengen_dancer_count(&g_session.game);
            /* ...and every one of them back to the head of its own
             * programme, with the match's own number for the dice. See the
             * driver in gba/hud.c. */
            dancers_begin((uint16_t)(g_session.game.player[g_view].rng.lo |
                                      (g_session.game.player[g_view].rng.hi << 8)),
                           g_dancer_cast);
        }
    }
    if (step.topped_out) {
        handtune_stop();
        /* The game-over tune is class 8 like the title's, so the in-game
         * tune's own class has to be freed for it — which MUSIC_SILENCE does. */
        nes_audio_play(NES_MUSIC_SILENCE);
        nes_audio_play(NES_MUSIC_GAMEOVER);
    }
}

/* One frame of the computer's input, for whichever player it is. It
 * re-chooses on every new piece, which is where getNextTetromino calls
 * computerMove (main.asm.txt:3735, 3749).
 *
 * AND IN "WITH COMPUTER" IT THINKS AGAIN WHEN THE HUMAN DOES, WHICH IS THE
 * WHOLE OF ITS COOP MANNERS.
 *
 * Those two call sites are not symmetrical, and the difference is one `txa` /
 * `beq`. getNextTetromino runs for whichever player just got a piece, and at
 * its end (main.asm.txt:3740-3749):
 *
 *      lda menuGameMode
 *      cmp #MENU_GAMEMODE_VS      ; $03
 *      bcc return                 ; 2 PLAYER, COOPERATIVE: no computer
 *      bne L9992                  ; $04 WITH COMPUTER: always
 *      txa                        ; $03 VERSUS...
 *      beq return                 ; ...only when the computer itself spawned
 *  L9992:
 *      ldx #$01
 *      jmp computerMove
 *
 * So in VERSUS — two separate boards — the computer plans once per piece of
 * its own and the human's spawns are none of its business. In WITH COMPUTER,
 * where both play into ONE twelve-wide field, EVERY spawn calls computerMove
 * for player 2, the human's included: the moment a human piece locks and the
 * next appears, the computer re-reads the board and re-picks a column for the
 * piece it is still holding. That is how the cartridge notices that the hole
 * it was aiming for has just been filled in, which is what this port did not
 * do — it committed on spawn and shoved its piece down on top of whatever had
 * arrived in the meantime.
 *
 * The re-plan goes through tengen_ai_rechoose, which is the same choice with
 * the settle clock left running: a piece halfway down that stops dead for a
 * quarter of a second reads as a hang, not as a decision. */
uint8_t ai_input(void) {
    TengenPlayerSlot slot = (TengenPlayerSlot)g_ai_slot;
    TengenPlayerSlot other = (TengenPlayerSlot)(g_ai_slot ^ 1);
    TengenTetromino mine = g_session.game.player[slot].piece.current;
    TengenTetromino theirs = g_session.game.player[other].piece.current;

    if (mine != g_ai_last_piece) {
        tengen_ai_choose(&g_ai, &g_session.game, slot);
    } else if (g_session.game.coop && theirs != g_ai_last_partner &&
                theirs != TT_NONE && mine != TT_NONE) {
        /* The shared board, and only there. `coop` is what playModeTable
         * makes of WITH COMPUTER; a race is two boards and what lands on the
         * other one is not news. */
        tengen_ai_rechoose(&g_ai, &g_session.game, slot);
    }
    g_ai_last_piece = mine;
    g_ai_last_partner = theirs;
    return tengen_ai_buttons(&g_ai, &g_session.game, slot, g_ai_frame++);
}

static void ai_play_frame(void) {
    if (!g_ai_active || g_ai_slot != TENGEN_PLAYER_2) return;
    if (g_session.game.paused) return;
    if (!g_session.game.player[TENGEN_PLAYER_2].game_active) return;

    uint8_t buttons = ai_input();
    /* Its clears and its top-out are heard: one screen, one speaker. In WITH
     * the level is shared, so a level-up it earns brings the dancers out for
     * both of them, which is what announce_step already does. */
    TengenStepResult out = tengen_step(&g_session.game, TENGEN_PLAYER_2, buttons);
    note_award(TENGEN_PLAYER_2, out);
    if (out.topped_out) leader_record(TENGEN_PLAYER_2);
    announce_step(out);
}

/* The level's colours and the falling piece's, each reinstalled the frame it
 * changes — the two things the ROM rewrites its palettes for. */
static void refresh_palettes(void) {
    const TengenPlayerState *p = &g_session.game.player[g_view];
    if (p->level != g_shown_level) {
        g_shown_level = p->level;
        set_field_palette_for_level(g_shown_level);
    }
    if (p->piece.current != g_shown_piece) {
        g_shown_piece = p->piece.current;
        set_piece_palette(g_shown_piece);
    }
    /* The partner's falling piece has a bank of its own on a coop board. */
    if (g_session.game.coop) {
        TengenTetromino other = g_session.game.player[g_view ^ 1].piece.current;
        if (other != g_shown_piece2) {
            g_shown_piece2 = other;
            set_bank_from_piece(PAL_PIECE2_BANK, other);
        }
    }
}

static uint8_t g_pause_row;    /* which line the cursor is on */
/* MUSIC MIX was chosen on the pause menu and left deliberately silent there,
 * so the unpause owes the match a tune. See the note in pause_menu_input. */
static bool g_mix_held;
bool g_pause_confirm;   /* ...and the SURE? question over the top of it */
static bool g_pause_yes;

static void draw_box_frame(int tx, int ty, int w, int h) {
    for (int x = 0; x < w; x++) {
        set_map_tile(tx + x, ty,
                      WITH_BANK(x == 0 ? T_BOX_TL : x == w - 1 ? T_BOX_TR : T_BOX_T,
                                 plaque_bank()));
        set_map_tile(tx + x, ty + h - 1,
                      WITH_BANK(x == 0 ? T_BOX_BL : x == w - 1 ? T_BOX_BR : T_BOX_B,
                                 plaque_bank()));
    }
    for (int y = 1; y < h - 1; y++) {
        set_map_tile(tx, ty + y, WITH_BANK(T_BOX_L, plaque_bank()));
        set_map_tile(tx + w - 1, ty + y, WITH_BANK(T_BOX_R, plaque_bank()));
        for (int x = 1; x < w - 1; x++)
            set_map_tile(tx + x, ty + y, WITH_BANK(T_BLANK, plaque_bank()));
    }
}

/* One line of the column, centred in the box's interior — and "centred" here
 * needs two of the four backgrounds, because a tile grid cannot centre
 * everything and the player can see the difference.
 *
 * ACROSS. The interior is TWELVE columns, an even number, so a word of EVEN
 * length lands on the middle exactly and a word of ODD length misses it by
 * half a tile — four pixels, and four pixels is what "sigue sin estar bien
 * centrado" looks like with MUSIC over LOGINSKA over EXIT. The offset layer
 * is three pixels right of the grid (STATS_SHIFT_PX), which is one pixel
 * short of that half tile, so an odd-length line is drawn THERE and comes out
 * one pixel off centre instead of four. It is the same trick
 * draw_text_centred plays on the settings screen, and the same way round now
 * that both frames have an even interior.
 *
 * (It was the other way round while the box was thirteen columns wide. It is
 * fourteen because thirteen could not be centred on the board at all; see
 * PMENU_W. Nothing here is free.)
 *
 * NOTHING SUB-TILE DOWNWARD, which is why every gap in this box is a whole
 * blank row. The only vertical nudge the port has is the counters' layer and
 * that is two pixels UP (PANEL_SHIFT_PX), which under a box's ceiling is the
 * wrong direction; and it cannot be combined with the three across anyway, so
 * the lines that would want it are exactly the ones that cannot have it. */
static void draw_pmenu_line(int ty, const char *text, int bank,
                             bool cursor) {
    unsigned len = text_len(text);
    int tx = PMENU_IN_TX + ((int)PMENU_IN_W - (int)len) / 2;
    bool offset = (len & 1u) != 0;

    /* THE CURSOR IS AN ARROW, NOT A COLOUR. Picking the line out by palette
     * was this menu's first idea and it does not read: four words in four
     * colours is a colour scheme, not a cursor, and nothing on screen says
     * which colour means "here". The arrow is the cartridge's own $3E, the
     * one its settings screen uses, and it sits immediately left of the line
     * it marks so it moves with the words rather than standing in a column of
     * its own.
     *
     * It rides whichever layer the line does, or it would sit three pixels
     * off the word it belongs to. And it is only ever asked for on the two
     * LABEL rows: the longest thing this box prints is KOROBEINIKI, which is
     * eleven characters in an eleven-column interior with no room beside it —
     * but that is the second line of the MUSIC entry, and the arrow marks the
     * entry at its label. */
    /* TWO COLUMNS LEFT, which is where the settings screen puts its cursor
     * (MENU_CURSOR_TX) and for the same reason: $3E's shaft runs the full
     * width of its tile and the letters start at the edge of theirs, so an
     * arrow one column left of a word is an arrow welded to it. */
    int first = cursor ? tx - PMENU_CURSOR_DX : tx;
    for (int i = first; i < tx + (int)len; i++) {
        uint16_t entry = (i == first && cursor) ? WITH_BANK(T_ARROW_R, bank)
                        : (i < tx) ? WITH_BANK(T_BLANK, bank)
                        : WITH_BANK(ascii_tile(text[i - tx]), bank);
        if (offset) set_stats_tile(i, ty, entry);
        else        set_map_tile(i, ty, entry);
    }
}

/* WHAT THE BOX LEAVES BEHIND WHEN IT GOES. The frame and the rows that centre
 * exactly are on the main background, and a repaint of the static screen
 * covers those; the rest of the menu is on the offset and counter layers,
 * which the static screen has no business in the middle of the board and
 * therefore never touches. So the window vanished and its words stayed. This
 * is the teardown, and it runs off the same g_repaint that paints the plaque
 * over — see draw_match. */
static void clear_pmenu_layers(void) {
    for (int y = 0; y < PMENU_H; y++)
        for (int x = 0; x < PMENU_W; x++) {
            clear_panel_region(PMENU_TX + x, PMENU_TY + y, 1, 1);
            set_stats_tile(PMENU_TX + x, PMENU_TY + y, T_BLANK);
            set_histogram_tile(PMENU_TX + x, PMENU_TY + y, T_BLANK);
        }
}

static void draw_pause_menu(void) {
    /* THE OTHER THREE LAYERS HAVE TO GET OUT OF THE WAY where the box lands.
     * Thirteen columns centred is the board and its braid, so in practice
     * this only clears frame art — but the counters' background is drawn
     * ABOVE the main one, and a box that met one would have the panel
     * printing straight through it. It is also what wipes the line the cursor
     * was on a frame ago, now that the lines move between layers. */
    clear_pmenu_layers();
    draw_box_frame(PMENU_TX, PMENU_TY, PMENU_W, PMENU_H);

    if (g_pause_confirm) {
        /* THE QUESTION MARK IS THE POINT OF THE SECOND LINE. It is not in the
         * cartridge's tile set at all — see ascii_tile — and without it the
         * two words read as a label rather than as a question.
         *
         * A blank row keeps EXIT off the box's ceiling, and SURE? rides two
         * pixels lower than its own row so the two words are spaced without
         * eight pixels of nothing between them. Both of them can only sit
         * where they do because EXIT is four letters and SURE? is five: see
         * draw_pmenu_line for why the parity decides which layer each gets. */
        draw_pmenu_line(PMENU_TY + 2, "EXIT", BANK_LABEL, false);
        draw_pmenu_line(PMENU_TY + 3, "SURE?", BANK_LABEL, false);
        /* THE TWO ANSWERS STACK, like everything else in this box. Side by
         * side they had the arrow sitting exactly between them — as far from
         * YES as from NO, which is an arrow that answers nothing. One to a
         * line each gets its own, in the same column as the cursor upstairs,
         * and there is no reading it wrong.
         *
         * (No lowercase in this tile set either — $61 up are the braid and
         * the border, which is why 'yes' came out as two stray marks — so
         * capitals and an arrow are all there is to say it with.) */
        draw_pmenu_line(PMENU_TY + 6, "YES", BANK_LABEL, g_pause_yes);
        draw_pmenu_line(PMENU_TY + 7, "NO", BANK_LABEL, !g_pause_yes);
        return;
    }
    /* PAUSE keeps its own colour because it is the heading and not a choice;
     * the three lines under it are all one colour now, and the arrow is what
     * says where you are. */
    draw_pmenu_line(PMENU_TY + 2, "PAUSE", BANK_NOTE, false);
    draw_pmenu_line(PMENU_TY + 4, "MUSIC", BANK_LABEL,
                     g_pause_row == PMENU_MUSIC);
    draw_pmenu_line(PMENU_TY + 5, kMusicNames[g_music], BANK_LABEL, false);
    draw_pmenu_line(PMENU_TY + 7, "EXIT", BANK_LABEL,
                     g_pause_row == PMENU_EXIT);
}

/* One frame of it. Returns true if the menu ate the input, which is what
 * keeps the cheat codes out of it — they are entered on the pad while paused
 * too, and a Down meant for this menu is the first byte of one of them. */
static bool pause_menu_input(uint8_t pressed, bool *leaving) {
    if (!g_pause_unlocked || !g_session.game.paused) return false;

    /* START IS ALWAYS RESUME, EVERYWHERE IN THIS MENU — on the EXIT line and
     * inside the question as well. It is the button that put the plaque up,
     * so it is the button that takes it down, and a menu where the meaning of
     * START depends on which line you are standing on is a menu you have to
     * remember rather than read. It is handed STRAIGHT ON to the core, which
     * is what actually unpauses; the question is dropped on the way out, so
     * the next pause opens on the column and not on a half-answered
     * "SURE?". A takes a choice, B backs out of one. */
    if (pressed & TENGEN_BTN_START) {
        g_pause_confirm = false;
        g_repaint = true;
        return false;
    }

    if (g_pause_confirm) {
        /* The answers stack, so Up and Down move between them as they do on
         * the column, and SELECT does too. Left and Right are kept because a
         * hand that has just been changing the tune with them is already
         * there. */
        if (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT | TENGEN_BTN_UP |
                        TENGEN_BTN_DOWN | TENGEN_BTN_SELECT))
            g_pause_yes = !g_pause_yes;
        if (pressed & TENGEN_BTN_B) {
            g_pause_confirm = false;
            screen_blip();
        } else if (pressed & TENGEN_BTN_A) {
            if (g_pause_yes) {
                /* QUITTING IS NOT AN UNPAUSE, AND THE ENGINE ONLY KNOWS ABOUT
                 * UNPAUSING. This is the whole of a bug that killed every
                 * sound in the machine for the rest of the session.
                 *
                 * Pausing sends the cartridge's MUSIC_SUSPEND, which is a gag
                 * on the WHOLE engine and not on the music alone: it silences
                 * the effects too, and the only thing that lifts it is
                 * MUSIC_RESUME. Every other way out of a pause goes back
                 * through pauseOrUnpause, which sends exactly that. This one
                 * did not — it tore the match down from under the plaque, and
                 * stop_music() only frees voice slots, it does not ungag
                 * anything — so the engine was left suspended with no pause
                 * left to lift it. After that the piece never landed with a
                 * sound, the game-over jingle never played, the menus lost
                 * their blip and the title came back silent, exactly as
                 * reported, until the console was switched off.
                 *
                 * FIRST IN THE RING, TOO. updateAudio takes one request per
                 * frame ($CFCC-$CFDB), so a resume queued after the blip
                 * below would let the blip be swallowed by the gag it is
                 * meant to lift. */
                nes_audio_play(NES_MUSIC_RESUME);
                *leaving = true;
            } else {
                g_pause_confirm = false;
            }
            screen_blip();
        }
        return true;
    }

    /* SELECT MOVES THE CURSOR, the way it does on the settings screen — the
     * cartridge's own LA048 treats SELECT as another DOWN ($9FBC), and this
     * menu has no reason to be the one place in the port where it does not.
     * Up and Down still do it too; with two entries all three are the same
     * step. */
    if (pressed & (TENGEN_BTN_UP | TENGEN_BTN_DOWN | TENGEN_BTN_SELECT))
        g_pause_row = (uint8_t)((g_pause_row + 1) % PMENU_ROWS);
    if (g_pause_row == PMENU_MUSIC &&
        (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT))) {
        int count = music_choices();
        int step = (pressed & TENGEN_BTN_RIGHT) ? 1 : count - 1;
        g_music = (uint8_t)((g_music + step) % count);
        /* Heard at once, which is the whole point of putting it here. The mix
         * restarts on its current turn rather than from the top. */
        g_mix_step = 0;
        /* HEARD AT ONCE, which is the whole point of putting it here — and
         * the game is PAUSED, so the engine has been suspended and a track
         * handed to it now would sit there silent. Resume, then start. The
         * pause's own suspend goes back on when the menu is left, because
         * tengen_pause_input resumes on the way out either way.
         *
         * EXCEPT MUSIC MIX, WHICH HAS NOTHING TO PREVIEW. The selection
         * screen already says so in as many words (see front_tune): the mix
         * is a rotation that belongs to the match, not one tune to audition,
         * so it goes quiet there like NO MUSIC does. This menu was starting
         * whichever tune the rotation happened to be on, which is the one
         * place in the port where the same choice sounded like two different
         * things depending on which screen you made it from.
         *
         * Going quiet here means the match would come back silent, though —
         * nothing restarts the engine on the way out of a pause — so the
         * silence is remembered and spent on the unpause below. */
        nes_audio_play(NES_MUSIC_RESUME);
        if (g_music == MUSIC_MIX) {
            stop_music();
            g_mix_held = true;
        } else {
            start_music(g_music);
            g_mix_held = false;
        }
    }
    /* A TAKES THE CHOICE, and only A. START used to do it here as well and it
     * cost the menu its way out — see the note at the top. */
    if (g_pause_row == PMENU_EXIT && (pressed & TENGEN_BTN_A)) {
        g_pause_confirm = true;
        g_pause_yes = false;   /* NO first: a pause menu does not lose games */
        screen_blip();
    }
    return true;
}

/* True once there is nothing left to play, which each mode decides its own
 * way.
 *
 * COOP IS ONE GAME, so one top-out finishes it — the core kills both players
 * at once there, the way the cartridge does (main.asm.txt:83D4-83DD), and
 * this only has to read either flag.
 *
 * AGAINST THE COMPUTER, THE RACE ENDS WITH THE PLAYER. The cartridge lets the
 * computer play on over a dead board, because its own way out is A+B held,
 * which restarts on the spot from handleGameOver (main.asm.txt:82F3-830F) —
 * so nobody ever sat and watched it. This port's way out is the GAME OVER
 * plaque and Start, and leaving the thing playing behind that plaque is what
 * it looked like: a game that would not end. It ends. */
static bool match_over(void) {
    if (!g_session.game.two_player || g_session.game.coop)
        return !g_session.game.player[g_view].game_active;
    if (g_ai_active)
        return !g_session.game.player[TENGEN_PLAYER_1].game_active;
    return !g_session.game.player[0].game_active &&
            !g_session.game.player[1].game_active;
}

/* One frame of a linked match: pace the cable, then take whatever it brought.
 * Returns false when the match is finished — either both boards are done, or
 * the link stopped and cannot be trusted to have kept the two simulations
 * together. */
/* THE BUTTON THAT STARTED THE MATCH MUST NOT ALSO PAUSE IT.
 *
 * The core computes each player's fresh presses as `buttons & ~held_last`, and
 * a new game starts with `held_last` at zero — so a START still physically
 * down on the match's first frame reads as a press and pauses it on the spot.
 * Solo never showed it because the front end's own edge detector had already
 * eaten that press; over the cable the buttons travel as raw levels and arrive
 * a transfer later, with nothing in between to eat anything.
 *
 * Pretending everything is already held is the fix: nothing can edge until it
 * has been let go once. Both consoles do it at the same point of the same
 * code, so the lockstep is untouched. */
void swallow_held_buttons(TengenGame *game) {
    for (int i = 0; i < 2; i++) game->player[i].held_last_frame = 0xFF;
}

bool link_play_frame(void) {
    /* The master starts one transfer per frame off its own vblank; the slave
     * has nothing to start. Either way the interrupt does the collecting. */
    link_pump();
    link_tick();

    LinkFrame f;
    int stepped = 0;
    while (stepped < LINK_MAX_CATCHUP && link_pop(&f)) {
        uint16_t local  = link_is_master() ? f.master : f.slave;
        uint16_t remote = link_is_master() ? f.slave  : f.master;

        /* A multiplayer transfer hands back every console's word INCLUDING
         * this one's, which is the only honest account of what we actually
         * managed to send. If it is not the frame we thought we were on, the
         * other console has been fed a lie and there is nothing to do but
         * stop. */
        if (tengen_link_frame(local) !=
             (uint8_t)(g_session.frame & TENGEN_LINK_FRAME_MASK)) {
            g_session.desynced = true;
            break;
        }

        TengenStepResult out[2];
        if (!tengen_link_step(&g_session, tengen_link_buttons(local), remote, out))
            break;
        note_award(0, out[0]);
        note_award(1, out[1]);
        /* A GAME IS WRITTEN DOWN AS IT ENDS, not at the end of the match:
         * L81DD is called from the top-out itself (main.asm.txt:600), which
         * is what lets a player start again over A+B and keep the game they
         * just finished. Both consoles see both results, so both write the
         * same two rows on the same frame. */
        for (int i = 0; i < 2; i++) if (out[i].topped_out) leader_record(i);
        /* ...and a board put back on its feet is a new board: everything on
         * it, and every counter beside it, has to be drawn again. */
        if (out[0].restarted || out[1].restarted) g_repaint = true;
        announce_step(out[g_view]);
        stepped++;
    }

    if (g_session.desynced || link_starved() > LINK_LOST_FRAMES) {
        g_link_lost = true;
        return false;
    }
    return !match_over();
}

/* Plays what this screen should be playing, and does nothing if it already
 * is — restarting a tune every frame would be a stutter, not music.
 *
 * EVERY FRONT-END SCREEN NAMES ITS TUNE, and one of the names is silence. That
 * matters more than it sounds: the title theme belongs to the TITLE and to
 * nothing else, so walking off it stops it, and walking back on starts it
 * again. An earlier pass let the theme carry on into GAME SELECT the way the
 * cartridge does, and then every path back out had to remember to put things
 * right — which is how the level screen's preview kept following the player
 * out to the title, and how a fast START could get the title theme layered
 * under a match's music.
 *
 * The cartridge does not need this because its front end is a one-way chain
 * with no way back. This port has B, so it does. */
void front_music(uint8_t which) {
    if (g_front_tune == which) return;
    g_front_tune = which;
    /* A SCREEN THAT WANTS QUIET HAS TO SUSPEND, not silence. See stop_music:
     * MUSIC_SILENCE resets the engine for the next track and leaves the
     * current one playing, which is why the title theme used to follow the
     * player all the way to GAME SELECT. Suspending also takes the fireworks'
     * bangs down with it — they queue effects of their own on their way out —
     * while still letting the screen-switch blip that was queued a frame ago
     * be heard in full. */
    if (which == FRONT_SILENCE) {
        stop_music();
        return;
    }
    if (which == FRONT_TITLE_THEME) {
        /* FROM THE TOP, every time — and freeing its own class is what makes
         * that happen. LD0E4 refuses a class-8 song that is already in a slot
         * outright (main.asm.txt:8590-8597), so without this the theme would
         * simply carry on from wherever it was. */
        handtune_stop();
        stop_music_class(NES_MUSIC_CLASS_TITLE);
        nes_audio_play(NES_MUSIC_SILENCE);
        nes_audio_play(NES_MUSIC_TITLESCREEN);
        return;
    }
    /* MUSIC MIX IS QUIET ON THE MENU, like NO MUSIC: there is no one tune to
     * preview, and the rotation belongs to the match. */
    if (which == MUSIC_MIX) {
        stop_music();
        return;
    }
    start_music(which);
}

/* One frame of a solo game: Start pauses, the cheat codes go in while paused
 * — both are the core's job (tengen_pause_input mirrors the ROM's own
 * pauseOrUnpause, which is where checkCodeInput lives). A code that fires
 * shows up on its own: a level-up through the palette check, a long bar or an
 * undo through the current-piece check. */
bool solo_play_frame(uint8_t buttons, uint8_t pressed, bool *quit) {
    /* THE MENU EATS THE PAD WHILE IT IS OPEN, and it has to: the cheat codes
     * are typed on the pad while paused too, so a Down meant for this menu is
     * the first byte of one of them. */
    if (pause_menu_input(pressed, quit)) { buttons = 0; pressed = 0; }
    if (g_session.game.player[0].game_active) {
        uint8_t presses[2] = { pressed, 0 };
        TengenCheat cheat[2];
        bool was_paused = g_session.game.paused;
        tengen_pause_input(&g_session.game, presses, cheat);
        if (was_paused != g_session.game.paused) {
            /* EVERY PAUSE OPENS ON MUSIC. The cursor used to be left where
             * the last one ended, so a player who had been to EXIT came back
             * to a menu whose START — the button that means resume
             * everywhere else on it — opened the quit question instead. */
            g_pause_row = PMENU_MUSIC;
            g_pause_confirm = false;
            /* pauseOrUnpause suspends and resumes the music
             * (main.asm.txt:7204-7211) — the same pair the front end uses to
             * go quiet, through the same two helpers so the port never loses
             * track of which state the engine is actually in. */
            /* ...EXCEPT IN A PROTOTYPE'S GAME, where "pausing in-game doesn't
             * mute the music" — the plaque goes up and the tune plays on.
             * Only the SUSPEND is skipped, never the resume: a game that was
             * paused before the skin was chosen, or one whose engine is
             * already gagged for any other reason, still has to be let go of.
             * See proto_rules. */
            if (!g_session.game.paused)
                nes_audio_play(NES_MUSIC_RESUME);
            else if (!g_session.game.proto_rules)
                nes_audio_play(NES_MUSIC_SUSPEND);
            /* ...and the mix, if the pause menu left it silent, starts here:
             * this is the frame the match comes back. See g_mix_held. */
            if (!g_session.game.paused && g_mix_held) {
                g_mix_held = false;
                start_music(g_music);
            }
            /* MUSIC_SUSPEND only silences the cartridge's engine. The
             * hand-entered tunes have their own channels and have to be
             * stopped and restarted with it, or PAUSE would leave one playing
             * on its own.
             *
             * THE MIX COUNTS. Asking `g_music == MUSIC_KOROBEINIKI` missed the
             * case where the tune playing is a hand-entered one because the
             * MIX is on its turn — and the mix OPENS on Korobeiniki, so it was
             * every first level of every mixed game: pause, and the tune
             * played on alone over the plaque. */
            /* A PAUSE SUSPENDS A TUNE; IT DOES NOT REWIND IT. This used to
             * call handtune_start on the way out, which resets both voices to
             * the first bar — so Korobeiniki and Katiuska began again from the
             * top after every pause, and after every visit to the pause menu's
             * MUSIC line, while the cartridge's own tracks came back exactly
             * where MUSIC_SUSPEND had left them. Suspend and resume are that
             * pair for the hand-entered ones.
             *
             * START is still start, mind: if the tune that should be playing
             * is not the one loaded — the menu chose another, or the mix has
             * turned over — it begins properly, from its first bar. */
            uint8_t tune = current_tune();
            if (MUSIC_IS_HANDTUNE(tune)) {
                /* ...and they follow the engine, including into a prototype's
                 * pause, where it is not silenced at all: a hand tune stopping
                 * over a plaque the cartridge's own tune plays through would
                 * be the two halves of the machine disagreeing. */
                if (g_session.game.paused && !g_session.game.proto_rules)
                    handtune_suspend();
                else if (!g_session.game.paused) {
                    if (handtune_current() == MUSIC_HANDTUNE_OF(tune))
                        handtune_resume();
                    else
                        handtune_start(MUSIC_HANDTUNE_OF(tune));
                }
            }
            /* The plaque has to be painted over on the way out, but this runs
             * mid-frame; six hundred tiles written into VRAM while the screen
             * is being scanned out is a visible tear. Flag it and let
             * draw_match do it inside the blank with everything else. */
            if (was_paused) g_repaint = true;
        }
        /* Every applied code plays this (main.asm.txt:7089, 7127). */
        if (cheat[0] != TENGEN_CHEAT_NONE)
            screen_blip();
    }

    TengenStepResult local = tengen_step(&g_session.game, TENGEN_PLAYER_1, buttons);
    note_award(TENGEN_PLAYER_1, local);
    if (local.topped_out) leader_record(TENGEN_PLAYER_1);
    announce_step(local);
    ai_play_frame();
    return !match_over();
}

/* Everything the screen shows during a match.
 *
 * ORDER MATTERS HERE, and getting it wrong is what made the pieces flicker.
 * Everything below writes video memory — the tile map, OAM, palette RAM — and
 * on a GBA all three want to be written during the vertical blank. This used
 * to run nes_audio_frame() first, and that is not a small thing to do: it
 * steps a 6502 interpreter through a whole frame of the cartridge's sound
 * engine, which is thousands of instructions and eats the entire blank. Every
 * tile and sprite then landed while the screen was being scanned out, so a
 * falling piece could be drawn half in its old position and half in its new
 * one. So: vsync, then draw, then the audio in the time that is left. */
void draw_match(bool *sweeping) {
    vsync();
    if (g_repaint) {
        /* THE WINDOW WENT AND THE WORDS STAYED. draw_static_screen puts the
         * board and the HUD back, which covers everything the pause menu drew
         * on the MAIN background — its frame, and the lines that centre
         * exactly. The rest of it is on the offset and counter layers, and
         * those the static screen has no business in the middle of the board
         * and so never writes: PAUSE, the tune's name and EXIT simply stayed
         * printed over the playfield. They are wiped here, where the plaque
         * is painted over, because that is the same moment. */
        clear_pmenu_layers();
        draw_static_screen();
        g_repaint = false;
    }
    refresh_palettes();
    draw_field();
    draw_panel();
    if (g_link_lost) draw_text(BOX_R_IN + 1, BOX_TOP_IN + 2, "LINK", BANK_LABEL);

    /* The sweep's sprites, and the one tidy-up when it finishes. */
    if (g_session.game.player[hud_clearing_slot()].line_clear_timer > 0) {
        draw_line_clear_sweep();
        *sweeping = true;
    } else if (*sweeping) {
        oam_hide_all();
        *sweeping = false;
    }

    /* After the sweep, which hides everything below its own range when it
     * finishes, and before the plaque. */
    draw_points();

    /* Last, so they sit over whatever was just drawn. */
    if (!g_session.game.player[g_view].game_active) draw_game_over();
    if (g_session.game.paused) {
        if (g_pause_unlocked) draw_pause_menu();
        else draw_pause_box();
    }

    /* And the sound engine afterwards, out of the blank, where it costs
     * nothing but CPU time. */
    audio_frame();
}
