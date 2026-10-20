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
/* FRAMES UNTIL THE TUNE COMES BACK after a linked level-up. Alone, the
 * dancers' show is what brings it back when it ends; over the cable there is
 * no show. A cartridge tune does not need one — its engine picks the tune
 * back up by itself once the intro lets go — but a hand-entered one is
 * STOPPED for the intro, and nothing started it again: Korobeiniki or
 * Katiuska went quiet on the console whose player had gone up a level, for
 * the rest of the match. So those, and MUSIC MIX's next turn, are asked for
 * again when the intro has run its course (LEVELUP_INTRO_FRAMES), counted
 * in frames of play. */
static uint8_t g_levelup_resume;
bool g_link_lost;         /* ...and the cable stopped answering */
/* ...or it has gone quiet and the match is waiting for it to come back. */
bool g_link_waiting;
static uint8_t g_link_wait_frames;   /* for the two sounds; see link_wait */
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
/* THE COSSACK ANSWERS ONE BOARD, AND IT IS NOT ALWAYS YOURS. In HUD STATS he
 * stands in your own panel and answers your clears; in HUD VERSUS the
 * compartment he is standing in is the RIVAL'S panel, and a figure there
 * celebrating what YOU just did would be reading the wrong board out loud.
 * Counted off the mask the core reports, so a clear that took four rows gets
 * four times the figure.
 *
 * Called per PLAYER rather than from announce_step, because announce_step is
 * only ever handed one player's result: in a linked race it is the local
 * one's, so the rival's clears never reached it at all. */
static int cossack_slot(void) { return hud_versus() ? (g_view ^ 1) : g_view; }

void cossack_watch(int slot, TengenStepResult step) {
    if (slot != cossack_slot() || !step.lines_collapsed) return;
    int rows = 0;
    for (int i = 0; i < TENGEN_PF_HEIGHT; i++)
        if (step.rows_cleared_mask & (1u << i)) rows++;
    idle_cossack_celebrate(rows);
}

/* THE GAME BOY PLAYER'S RUMBLE (gbp.c), on what this console's player
 * does: a clear, longer the more rows — a TETRIS the most — the level going
 * up, and the top-out. Nothing on a GBA, which has no motor. And only under
 * the chord, the match's (match_chord), as the rest of what the port adds:
 * the cartridge's pad never shook. */
static bool pause_menu_on(void);
static void rumble_step(TengenStepResult step) {
    /* ...and not if the player has switched it off (SETTINGS, extras.c). */
    if (!pause_menu_on() || !rumble_wanted()) return;
    if (step.lines_cleared) {
        int rows = 0;
        for (uint32_t m = step.rows_cleared_mask; m; m &= m - 1) rows++;
        gbp_rumble(rows >= 4 ? 30 : 4 + 4 * rows);
    }
    if (step.leveled_up) gbp_rumble(20);
    if (step.topped_out) gbp_rumble(45);
}

/* THE GAME'S OWN FIGURES (tengen_stats.h), counted for both players from
 * the first frame, whatever the chord: it is only the page that is behind
 * it. Fed beside note_award, wherever a step is taken. */
TengenStats g_stats[2];
int8_t g_end_rise = -1;
int8_t g_end_curtain = -1;
static uint16_t g_end_frames;

void stats_begin_match(void) {
    tengen_stats_begin(&g_stats[0]);
    tengen_stats_begin(&g_stats[1]);
    g_end_frames = 0;
    g_end_rise = -1;
    g_end_curtain = -1;
}

static void stats_watch(int slot, TengenStepResult step) {
    tengen_stats_step(&g_stats[slot], &g_session.game, (TengenPlayerSlot)slot, &step);
}

/* `level_tune` is whether the LEVEL-UP'S MUSIC plays this step, which is not
 * always this player's level-up: see link_play_frame. */
static void announce(TengenStepResult step, bool level_tune) {
    rumble_step(step);
    if (step.piece_locked) nes_audio_play(NES_SOUND_DROP);
    /* ONE call per event, and the level-up is one event. A clear that also
     * raises the level used to reach setMusicOrSoundEffect(MUSIC_LEVELUP)
     * twice in the same frame — once for the clear and once for the level —
     * and the engine restarts a track every time it is handed one, so the
     * intro began, was cut off a few hundred cycles later and began again.
     * That is what a doubled tune sounds like. */
    if (step.lines_collapsed && !level_tune)
        nes_audio_play(NES_SOUND_LINECLEAR);
    /* A PROTOTYPE'S LEVEL-UP IS JUST A CLEAR. Measured on the dumps: none of
     * them asks its sound queue for anything new when the level goes up —
     * no jingle — and proto_c and proto_d do not even play the line's own
     * sound on that clear. See levelup_clear_sound. Nothing else of the
     * release's level-up follows either: no show, no music change. */
    if (level_tune && g_session.game.proto_rules && !g_linked) {
        if (levelup_clear_sound()) nes_audio_play(NES_SOUND_LINECLEAR);
        /* MUSIC MIX still turns over on a level — it is the port's, and a
         * level is its unit — and with no interlude to restart the tune
         * after, the next one starts on the spot, as over the cable. */
        if (g_music == MUSIC_MIX) {
            g_mix_step = (uint8_t)((g_mix_step + 1) % MIX_COUNT);
            start_music(g_music);
        }
        return;
    }
    if (level_tune) {
        /* The cartridge's level-up music takes over; a hand-entered tune stands
         * down and start_music() puts it back when the dancers finish. */
        handtune_stop();
        /* THE INTRO, which is what the cartridge plays at this moment; the
         * looping tune comes in when the intro runs out. See
         * NES_MUSIC_LEVELUP_INTRO. A linked match has no show to hand it
         * over to, and for it the jingle on its own is the right answer
         * anyway: it ends by itself, where the loop had to be cut off by
         * whatever came next. (A prototype's game never gets here: see
         * above.) */
        nes_audio_play(NES_MUSIC_LEVELUP_INTRO);
        /* MUSIC MIX turns over here, and here only. See MUSIC_MIX. */
        if (g_music == MUSIC_MIX)
            g_mix_step = (uint8_t)((g_mix_step + 1) % MIX_COUNT);
        /* A linked match has no interlude to bring the tune back: a
         * hand-entered one, or the mix's next, comes back when the intro
         * ends (g_levelup_resume). */
        if (g_linked && (g_music == MUSIC_MIX || MUSIC_IS_HANDTUNE(g_music)))
            g_levelup_resume = LEVELUP_INTRO_FRAMES;
        /* NO COSSACKS IN A PROTOTYPE'S GAME. Those builds go up a level and
         * carry straight on — no dancers, no BONUS tally. Measured on B, C
         * and D: gameState never leaves 0 and the dancers' programs
         * ($019A/$01A2) never start, where the release goes to 3 with the
         * piece frozen. See proto_rules. */
        if (!g_linked && !g_session.game.proto_rules) {
            /* THE TUNE STOPS FOR THE SHOW. showLevelBonus ends with
             * MUSIC_SILENCE (main.asm.txt:1953), queued right behind the
             * intro in the same frame: the intro is class 8 and survives it,
             * the match's tune is class 7 and does not. Without it the intro
             * took over only the channels it uses, and the tune's triangle
             * went on underneath — two tunes at once at every level-up, the
             * bass line of one under the jingle of the other. */
            nes_audio_play(NES_MUSIC_SILENCE);
            g_dancer_active = true;
            g_dancer_timer = DANCER_TIMER_START;
            bonus_begin();
            g_dancer_tick = 0;
            g_dancer_elapsed = 0;
            /* The cast is read HERE, before the tally is emptied: L8D8B runs
             * at the top of showLevelBonus, and how well the level went is
             * what decides how many cossacks come on. */
            g_dancer_cast = tengen_dancer_count(&g_session.game);
            /* COOP'S PAIRS ARE FOR COOP'S TWO PANELS. In HUD STATS the troupe
             * comes on through one box, on the solo column's six floors
             * (draw_stats_show), so it dances the solo programmes and has the
             * solo cap — which has to be applied HERE and not only when
             * drawing, because every dancer given a programme rolls the
             * shared dice (see dancers_begin) and a seventh and eighth that
             * were never drawn would still move the other six. */
            bool pairs = g_session.game.coop && !hud_stats();
            if (!pairs && g_dancer_cast > DANCER_COUNT)
                g_dancer_cast = DANCER_COUNT;
            /* ...and every one of them back to the head of its own
             * programme, with the match's own number for the dice. See the
             * driver in gba/hud.c. */
            dancers_begin((uint16_t)(g_session.game.player[g_view].rng.lo |
                                      (g_session.game.player[g_view].rng.hi << 8)),
                           g_dancer_cast, pairs);
        }
    }
    if (step.topped_out) {
        g_levelup_resume = 0;        /* the game-over tune, not the match's */
        handtune_stop();
        /* The game-over tune is class 8 like the title's, so the in-game
         * tune's own class has to be freed for it — which MUSIC_SILENCE does. */
        nes_audio_play(NES_MUSIC_SILENCE);
        nes_audio_play(NES_MUSIC_GAMEOVER);
    }
}

static void announce_step(TengenStepResult step) {
    announce(step, step.leveled_up);
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
    stats_watch(TENGEN_PLAYER_2, out);
    cossack_watch(TENGEN_PLAYER_2, out);
    if (out.topped_out) leader_record(TENGEN_PLAYER_2);
    announce_step(out);
}

/* The level's colours and the falling piece's, each reinstalled the frame it
 * changes — the two things the ROM rewrites its palettes for. */
static void refresh_palettes(void) {
    /* field_view, not g_view: the colours belong to the BOARD on the screen.
     * A paused race under the chord shows the other board, and with g_view
     * here its stack came up in your level's palette and its falling piece
     * in the colours of the piece YOU were holding. */
    const TengenPlayerState *p = &g_session.game.player[field_view()];
    if (p->level != g_shown_level) {
        g_shown_level = p->level;
        set_field_palette_for_level(g_shown_level);
    }
    /* THE BRICKS HAVE A BANK OF THEIR OWN (PAL_BRICK_BANK): grey while any
     * stand, the pause's wall or the end page's, and the piece's colours
     * left alone beside them. */
    if (bricks_shown()) set_brick_palette();
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
/* ...or the tunes' window, and the line its cursor is on. See PMENU_H_MAX. */
static bool g_pause_tunes;
static uint8_t g_tune_row;
/* ...or the options' window, and its line. See OPT_HARD_DROP. */
static bool g_pause_options;
static uint8_t g_option_row;

/* THE OPTIONS, as this console's player set them (OPT_HARD_DROP and the
 * rest). Kept on the battery with a check byte of their own; a byte that
 * does not check, or a console that never wrote one, is everything OFF. A
 * Single-Pak copy has no battery, so it keeps them until it is switched
 * off. */
uint8_t g_options;

static uint8_t options_check(uint8_t v) { return (uint8_t)(0xC3 ^ (v * 7)); }

void options_load(void) {
    uint8_t v = sram_read(SAVE_OPTIONS_OFF);
    g_options = (v & ~OPT_ALL) || sram_read(SAVE_OPTIONS_OFF + 1) != options_check(v)
                    ? 0 : v;
}

static void options_save(void) {
    sram_write(SAVE_OPTIONS_OFF, g_options);
    sram_write(SAVE_OPTIONS_OFF + 1, options_check(g_options));
}

/* THE PLAYER'S OWN TWO (OPT_GHOST, OPT_SWAP_AB), in force: when the menu
 * that sets them is there — this console's chord, or over the cable the
 * master's — and never in the attract demo, whose pad is the computer's. */
static bool pause_menu_on(void);
uint8_t options_mine(void) {
    if (g_demo || !pause_menu_on()) return 0;
    return (uint8_t)(g_options & ~OPT_RULES);
}

/* THE RULES (OPT_RULES) ONTO A SOLO GAME, for the one person playing it:
 * player 1. The computer beside it, when there is one, plays with the
 * cartridge's hands — and never presses UP. Without the chord, the
 * cartridge's game. */
void options_apply_solo(TengenGame *game) {
    uint8_t on = g_pause_unlocked ? g_options : 0;
    uint8_t me = 1u << TENGEN_PLAYER_1;
    game->hard_drop = (on & OPT_HARD_DROP) != 0;
    game->fast_soft = (on & OPT_FAST_DOWN) ? me : 0;
    game->fast_shift = (on & OPT_FAST_SHIFT) ? me : 0;
    game->hold = (on & OPT_HOLD) ? me : 0;
}

/* HOLD IN FORCE for this console's player (TengenGame.hold): what SELECT and
 * L+R mean in a match, and what HUD STATS shows where its cossack stood. */
bool hold_on(void) {
    return (g_session.game.hold >> g_view) & 1;
}

/* What the window says a switch is. The rules are read off the match, which
 * is what they ARE in it — over the cable the master's, whatever this
 * console has kept — and the player's own off this console. */
static bool option_value(int i) {
    const TengenGame *g = &g_session.game;
    switch (1u << i) {
    case OPT_HARD_DROP:  return g->hard_drop;
    case OPT_FAST_DOWN:  return (g->fast_soft >> g_view) & 1;
    case OPT_FAST_SHIFT: return (g->fast_shift >> g_view) & 1;
    case OPT_HOLD:       return (g->hold >> g_view) & 1;
    default:             return (g_options >> i) & 1;
    }
}

/* A switch thrown. Solo, it is kept and in force at once. Over the cable
 * both consoles throw a rule together — the menu runs on both players'
 * presses on both (see g_link_menu) — for both boards and for this match
 * only, since it was the master's to begin with; and a player's own switch
 * is thrown by that player's console alone (`here`), where it is kept. */
static void option_toggle(int i, bool here) {
    uint8_t bit = (uint8_t)(1u << i);
    TengenGame *g = &g_session.game;
    if ((bit & OPT_RULES) && g_linked) {
        uint8_t rules = 0;
        for (int r = 0; r < OPT_COUNT; r++)
            if (((1u << r) & OPT_RULES) && option_value(r)) rules |= (uint8_t)(1u << r);
        tengen_link_rules(&g_session, opt_link_rules((uint8_t)(rules ^ bit)));
        if (bit == OPT_HOLD) hud_hold_changed();
        return;
    }
    if (!here && g_linked) return;
    g_options ^= bit;
    options_save();
    if (!g_linked) options_apply_solo(g);
    if (bit == OPT_HOLD) hud_hold_changed();
}

/* A row at a time, straight into the main map: the tunes' window is eleven
 * rows of eighteen, drawn on every frame of the pause, and a call per tile
 * (with the skin's bank asked for each one) was frames past the blank. */
static void draw_box_frame(int tx, int ty, int w, int h) {
    int bank = plaque_bank();
    vu16 *row = MEM_SCREENBLOCK(SCREENBLOCK) + ty * MAP_W + tx;
    for (int y = 0; y < h; y++, row += MAP_W) {
        int l = T_BOX_L, m = T_BLANK, r = T_BOX_R;
        if (y == 0) { l = T_BOX_TL; m = T_BOX_T; r = T_BOX_TR; }
        else if (y == h - 1) { l = T_BOX_BL; m = T_BOX_B; r = T_BOX_BR; }
        row[0] = WITH_BANK(l, bank);
        for (int x = 1; x < w - 1; x++) row[x] = WITH_BANK(m, bank);
        row[w - 1] = WITH_BANK(r, bank);
    }
}

/* One line of the box, centred in its interior — and "centred" here needs
 * two of the four backgrounds, because a tile grid cannot centre everything
 * and the player can see the difference.
 *
 * ACROSS. A word whose length has the interior's parity lands on the middle
 * exactly; the other parity misses it by half a tile, four pixels. The offset
 * layer is three pixels right of the grid (STATS_SHIFT_PX), so such a line is
 * drawn THERE and comes out one pixel off instead of four — the same trick
 * draw_text_centred plays on the settings screen.
 *
 * EVERY LINE ON THE BOX'S OWN AXIS, the heading and the choices alike: that
 * is what the eye measures them against. The arrow does not get a column of
 * its own any more. It stands just before the line it marks — the same three
 * pixels before every one (T_ARROW_TAIL) — and the box is made wide enough
 * that the longest line still has room for it (pmenu_width).
 *
 * DOWNWARD, NOTHING MOVES A LINE: the only vertical nudge the port has is
 * the counters' layer, two pixels UP, and there is no pixel spare in this box
 * to move a line into anyway. The one pixel the heading has under it is in
 * its letters — see PMENU_RAISED_BASE.
 *
 * THE CURSOR IS AN ARROW, NOT A COLOUR. Picking the line out by palette was
 * this menu's first idea and it does not read: words in four colours is a
 * colour scheme, not a cursor. */
enum {
    PL_HEADING,   /* PAUSE, EXIT?: raised letters, no cursor */
    PL_CHOICE     /* the tune, EXIT, YES, NO: the arrow before it when chosen */
};

/* Where a line of `len` characters starts in an interior `in_w` wide, in
 * pixels from the interior's left edge, and whether it is on the offset
 * layer to get there. */
static int pmenu_start_px(int in_w, int len, bool *offset) {
    int spare = in_w - len;
    *offset = (spare & 1) != 0;
    return (spare / 2) * TILE_PX + (*offset ? STATS_SHIFT_PX : 0);
}

/* THE NARROWEST EVEN BOX that holds `lines`, those after the first with an
 * arrow before them: the arrow's eight pixels, its three to the word, and
 * two clear of the frame. */
#define PMENU_ARROW_ROOM_PX (TILE_PX + PMENU_ARROW_GAP_PX + 2)
static int pmenu_width(const char *const *lines, int count) {
    for (int w = PMENU_W_MIN; w <= PMENU_W_MAX; w += 2) {
        bool fits = true;
        for (int i = 0; i < count && fits; i++) {
            int len = (int)text_len(lines[i]);
            bool offset;
            if (len > w - 2) fits = false;
            else if (i > 0 &&
                     pmenu_start_px(w - 2, len, &offset) < PMENU_ARROW_ROOM_PX)
                fits = false;
        }
        if (fits) return w;
    }
    return PMENU_W_MAX;
}

static void draw_pmenu_line(int in_tx, int in_w, int ty, const char *text,
                             int bank, int kind, bool cursor) {
    int len = (int)text_len(text);
    bool offset;
    int tx = in_tx + pmenu_start_px(in_w, len, &offset) / TILE_PX;
    bool raised = kind == PL_HEADING;
    for (int i = 0; i < len; i++) {
        uint16_t tile = raised ? raised_tile(text[i]) : ascii_tile(text[i]);
        uint16_t entry = WITH_BANK(tile, bank);
        if (offset) set_stats_tile(tx + i, ty, entry);
        else set_map_tile(tx + i, ty, entry);
    }
    if (kind == PL_CHOICE && cursor) {
        uint16_t t = WITH_BANK(T_ARROW_TAIL, bank);
        uint16_t h = WITH_BANK(T_ARROW_HEAD, bank);
        if (offset) { set_stats_tile(tx - 2, ty, t); set_stats_tile(tx - 1, ty, h); }
        else { set_map_tile(tx - 2, ty, t); set_map_tile(tx - 1, ty, h); }
    }
}

/* The box drawn last, its width and height, so a change of size can clean
 * up after it. */
static int g_pmenu_drawn_w, g_pmenu_drawn_h;

/* WHAT THE BOXES COVER, on all four layers. Nothing under them changes
 * while the game is paused, so a box that gets smaller — the question after
 * the menu, the menu after the tunes' window — puts back what it uncovers
 * from here. It used to ask for a whole repaint of the static screen
 * instead, and those frames finished a scanline past the vertical blank.
 *
 * SAVED AS IT IS FIRST COVERED. What is held is the centred rectangle round
 * every box since the screen was last whole (g_pmenu_saved_w, _h); a box
 * that reaches past it saves the difference before it is drawn, and nothing
 * has covered that yet. Eleven rows of sixteen, four layers deep, on the
 * frame the pause goes up was the heaviest way to have it.
 *
 * THE FOUR LAYERS ARE FOUR SCREENBLOCKS IN A ROW — the main map, the
 * offset one, the counters' and the histogram's — so layer `l` is the main
 * map plus `l` maps, and the copies below walk them by pointer. */
typedef char pmenu_layers_in_a_row[
    SCREENBLOCK_STATS == SCREENBLOCK + 1 && SCREENBLOCK_PANEL == SCREENBLOCK + 2 &&
    SCREENBLOCK_HISTOGRAM == SCREENBLOCK + 3 ? 1 : -1];
#define PMENU_LAYER(l) (MEM_SCREENBLOCK(SCREENBLOCK) + (l) * MAP_W * 32)
#define PMENU_TX0 ((SCREEN_TW - PMENU_W_MAX) / 2)
#define PMENU_TY0 ((SCREEN_TH - PMENU_H_MAX) / 2)
/* Sixteen hundred bytes, in EWRAM: internal WRAM had less than the stack's
 * 4KB left over with them, on a cartridge and on a Single-Pak copy alike,
 * and since the 6502's view went to half (NES6502_CODE_SIZE) the copy has
 * room out there. */
__attribute__((section(".ewram")))
static uint16_t g_pmenu_under[4][PMENU_H_MAX][PMENU_W_MAX];
static int g_pmenu_saved_w, g_pmenu_saved_h;

/* Every cell of the centred box `w` by `h` that the centred box `keep_w` by
 * `keep_h` does not cover: saved, or put back. A row of it is at most two
 * runs, either side of the kept box, copied straight. */
static void pmenu_cells(int w, int h, int keep_w, int keep_h, bool back) {
    int x0 = (SCREEN_TW - w) / 2, y0 = (SCREEN_TH - h) / 2;
    int kx = (SCREEN_TW - keep_w) / 2, ky = (SCREEN_TH - keep_h) / 2;
    for (int l = 0; l < 4; l++)
        for (int ty = y0; ty < y0 + h; ty++) {
            vu16 *map = PMENU_LAYER(l) + ty * MAP_W;
            uint16_t *under = g_pmenu_under[l][ty - PMENU_TY0];
            bool kept = ty >= ky && ty < ky + keep_h;
            int run[2][2] = { { x0, kept ? kx : x0 + w },
                              { kept ? kx + keep_w : x0 + w, x0 + w } };
            for (int r = 0; r < 2; r++) {
                if (back)
                    for (int tx = run[r][0]; tx < run[r][1]; tx++)
                        map[tx] = under[tx - PMENU_TX0];
                else
                    for (int tx = run[r][0]; tx < run[r][1]; tx++)
                        under[tx - PMENU_TX0] = map[tx];
            }
        }
}

/* ...AND PUT BACK WHOLE WHEN THE PAUSE ENDS: the box's own cells, on all
 * four layers, from what was saved as it went up. That is all a pause
 * changed on the screen, and a repaint of the whole static screen to undo it
 * was the heaviest frame the game had — the one that left a pause finished
 * drawing at line 222 of 227, five lines from the next picture. */
static void pmenu_restore_box(void) {
    pmenu_cells(g_pmenu_drawn_w, g_pmenu_drawn_h, 0, 0, true);
}

/* The same for the cartridge's own plaque (no chord, no menu): it is on the
 * main background only. */
static uint16_t g_plaque_under[PAUSE_H][PAUSE_W];
static bool g_plaque_drawn;

static void plaque_draw(void) {
    if (!g_plaque_drawn) {
        vu16 *map = MEM_SCREENBLOCK(SCREENBLOCK);
        for (int y = 0; y < PAUSE_H; y++)
            for (int x = 0; x < PAUSE_W; x++)
                g_plaque_under[y][x] = map[(PAUSE_TY + y) * MAP_W + PAUSE_TX + x];
        g_plaque_drawn = true;
    }
    draw_pause_box();
}

static void plaque_restore(void) {
    vu16 *map = MEM_SCREENBLOCK(SCREENBLOCK);
    for (int y = 0; y < PAUSE_H; y++)
        for (int x = 0; x < PAUSE_W; x++)
            map[(PAUSE_TY + y) * MAP_W + PAUSE_TX + x] = g_plaque_under[y][x];
}

/* The pause is over and only its box needs taking down (see
 * pmenu_restore_box); g_repaint, the whole static screen, wins over it. */
static bool g_unpause_patch;

/* A MATCH LEFT THROUGH EXIT GAME LEAVES ITS BOX UP, and with it the record
 * of what the box covered. The next match's first box took that record for
 * its own — "drawn already, and saved" — saved nothing, and on the way down
 * put back the last match's cells: after WITH COMPUTER, the coop board's
 * twelve-wide frame across the rows of a solo board's box, there for good.
 * Forgotten as a match starts (hud_reset), with the wall that went up with
 * it. */
void pause_box_forget(void) {
    g_pmenu_drawn_w = g_pmenu_drawn_h = 0;
    g_pmenu_saved_w = g_pmenu_saved_h = 0;
    g_plaque_drawn = false;
    g_unpause_patch = false;
}

/* WHAT THE BOX LEAVES BEHIND WHEN IT GOES. The frame and the rows that centre
 * exactly are on the main background, and a repaint of the static screen
 * covers those; the rest of the menu is on the offset and counter layers,
 * which the static screen has no business in the middle of the board and
 * therefore never touches. So the window vanished and its words stayed. This
 * is the teardown, and it runs off the same g_repaint that paints the plaque
 * over — see draw_match.
 *
 * The box's own cells, centred: the box on screen, or the one before it if
 * that was bigger. Never more — the panels' counters live on these layers
 * just outside a narrow box, and wiping the widest box's worth took their
 * digits with it. */
static void clear_pmenu_layers(int w, int h) {
    int tx = (SCREEN_TW - w) / 2, ty = (SCREEN_TH - h) / 2;
    for (int l = 1; l < 4; l++)            /* all but the main map */
        for (int y = ty; y < ty + h; y++) {
            vu16 *map = PMENU_LAYER(l) + y * MAP_W;
            for (int x = tx; x < tx + w; x++) map[x] = T_BLANK;
        }
}

/* ONE WAY TO PUT UP A BOX, for the menu, its question, the tunes and the
 * lost cable: `count` lines, the heading and then the choices, with the
 * arrow before line `cursor` (none if there is no such line). */
/* Saves what a box `w` by `h` will cover that no box has covered since the
 * screen was last whole. The first box saves from scratch. */
static void pmenu_reach(int w, int h) {
    if (!g_pmenu_drawn_w) g_pmenu_saved_w = g_pmenu_saved_h = 0;
    int sw = w > g_pmenu_saved_w ? w : g_pmenu_saved_w;
    int sh = h > g_pmenu_saved_h ? h : g_pmenu_saved_h;
    if (sw != g_pmenu_saved_w || sh != g_pmenu_saved_h) {
        pmenu_cells(sw, sh, g_pmenu_saved_w, g_pmenu_saved_h, false);
        g_pmenu_saved_w = sw;
        g_pmenu_saved_h = sh;
    }
}

/* The box itself, `w` by `h` and centred, before anything is written in it. */
static void pmenu_frame(int w, int h) {
    pmenu_reach(w, h);
    /* A box of another size than the last one uncovers the board, the braid
     * and the panels' edges where the last one was: put back just those. */
    if (g_pmenu_drawn_w && (w != g_pmenu_drawn_w || h != g_pmenu_drawn_h))
        pmenu_cells(g_pmenu_drawn_w, g_pmenu_drawn_h, w, h, true);
    /* THE OTHER THREE LAYERS HAVE TO GET OUT OF THE WAY where the box lands:
     * the counters' background is drawn ABOVE the main one, and a box that
     * met one would have the panel printing straight through it. It is also
     * what wipes the line the cursor was on a frame ago. */
    clear_pmenu_layers(w, h);
    g_pmenu_drawn_w = w;
    g_pmenu_drawn_h = h;
    draw_box_frame((SCREEN_TW - w) / 2, (SCREEN_TH - h) / 2, w, h);
}

static void draw_pmenu(const char *const *lines, int count, int cursor) {
    int w = pmenu_width(lines, count), h = count + 2;
    pmenu_frame(w, h);
    int tx = (SCREEN_TW - w) / 2, ty = (SCREEN_TH - h) / 2;
    /* The heading keeps its own colour because it is not a choice, and a
     * pixel more under it than there is between the choices (see
     * PMENU_RAISED_BASE). */
    for (int i = 0; i < count; i++)
        draw_pmenu_line(tx + 1, w - 2, ty + 1 + i, lines[i],
                         i ? BANK_LABEL : BANK_NOTE, i ? PL_CHOICE : PL_HEADING,
                         i == cursor);
}

/* ...AND A LIST FLUSH LEFT: the tunes' window. Eight names of three to
 * eleven letters centred one under another made a ragged edge down both
 * sides, which reads as eight separate things and not as a list. Flush left
 * they start in one column — the options' window's, after the arrow's two
 * columns — the heading still centred over them; the box as narrow as the
 * longest name and a column clear of the frame allow, and even. */
static void draw_pmenu_list(const char *const *lines, int count, int cursor) {
    int longest = 0;
    for (int i = 1; i < count; i++) {
        int len = (int)text_len(lines[i]);
        if (len > longest) longest = len;
    }
    int w = longest + 5;                 /* frame, arrow, name, a column, frame */
    w += w & 1;
    if (w < PMENU_W_MIN) w = PMENU_W_MIN;
    if (w > PMENU_W_MAX) w = PMENU_W_MAX;
    int h = count + 2;
    pmenu_frame(w, h);
    int tx = (SCREEN_TW - w) / 2, ty = (SCREEN_TH - h) / 2;
    draw_pmenu_line(tx + 1, w - 2, ty + 1, lines[0], BANK_NOTE, PL_HEADING, false);
    for (int i = 1; i < count; i++) {
        int y = ty + 1 + i;
        for (int c = 0; lines[i][c]; c++)
            set_map_tile(tx + 3 + c, y, WITH_BANK(ascii_tile(lines[i][c]), BANK_LABEL));
        if (i == cursor) {
            set_map_tile(tx + 1, y, WITH_BANK(T_ARROW_TAIL, BANK_LABEL));
            set_map_tile(tx + 2, y, WITH_BANK(T_ARROW_HEAD, BANK_LABEL));
        }
    }
}

/* THE OPTIONS' WINDOW IS A TABLE, NOT A LIST, so its lines do not centre:
 * the switch's name stands at the left, after the arrow's two columns, and
 * its box at the right, in a column of its own (T_CHECK_OFF): empty for
 * OFF and a dot in it for ON, both in the names' white — ON in a second
 * colour said no more than the dot does, and less clearly. The box stands as far in
 * from the frame on the right as the arrow's tail does on the left.
 *
 * SIXTEEN WIDE, the menu's own width. ON and OFF were words once, and the
 * longest name with OFF after it needed eighteen: the tunes' window's,
 * which covers the panels' last column of figures. A box is one letter
 * wide and leaves FAST SHIFT its ten. */
#define OPT_W 16
static const char *const kOptionNames[OPT_COUNT] = {
    "HARD DROP", "FAST DOWN", "FAST SHIFT", "GHOST", "SWAP A-B", "HOLD"
};

static void draw_options_box(void) {
    const int w = OPT_W, h = OPT_H;
    pmenu_frame(w, h);
    int tx = (SCREEN_TW - w) / 2, ty = (SCREEN_TH - h) / 2;
    draw_pmenu_line(tx + 1, w - 2, ty + 1, "OPTIONS", BANK_NOTE, PL_HEADING, false);
    for (int i = 0; i < OPT_COUNT; i++) {
        int y = ty + 2 + i;
        const char *name = kOptionNames[i];
        for (int c = 0; name[c]; c++)
            set_map_tile(tx + 3 + c, y, WITH_BANK(ascii_tile(name[c]), BANK_LABEL));
        if (i == g_option_row) {
            set_map_tile(tx + 1, y, WITH_BANK(T_ARROW_TAIL, BANK_LABEL));
            set_map_tile(tx + 2, y, WITH_BANK(T_ARROW_HEAD, BANK_LABEL));
        }
        bool on = option_value(i);
        uint16_t box = on ? T_CHECK_ON : T_CHECK_OFF;
        set_map_tile(tx + w - 3, y, WITH_BANK(box, BANK_LABEL));
        set_map_tile(tx + w - 2, y, WITH_BANK(box + 1, BANK_LABEL));
    }
}

/* CABLE LOST, in the pause menu's own window and by its own machinery (what
 * the box covers is kept and put back): the one thing on the screen when the
 * cable is gone for good, on both consoles, where a player looks. */
static bool g_lost_over_air;   /* link_give_up: the adapter, not a cable */

static void draw_cable_lost_box(void) {
    /* Over the air there is no cable to have lost (wireless.c). */
    const char *lines[] = { g_lost_over_air ? "SIGNAL LOST" : "CABLE LOST",
                            "PRESS START", "TO LEAVE" };
    draw_pmenu(lines, 3, -1);
}

/* The tunes the window lists. THE SAME LIST ON BOTH CONSOLES: over the cable
 * the menu is the MASTER's (there because it found the chord), so its list
 * is the one with the uncovered tunes whatever this console found. Counted
 * from this console's own chord, a slave that had not rung it went through
 * five tunes while the master went through eight, and the two ended up
 * playing different ones. */
static int pause_tune_count(void) {
    return g_linked ? MUSIC_UNLOCKED_COUNT : music_choices();
}

static void draw_pause_menu(void) {
    /* THE QUESTION MARK IS WHAT MAKES IT A QUESTION. It is the one glyph here
     * that is not the cartridge's — see ascii_tile. The answers stack, one to
     * a line, so the arrow answers one of them and not the gap between. */
    /* "SURE?", not "EXIT?": the entry it answers already says EXIT GAME,
     * and the question saying it again read as a second menu. */
    static const char *const ask[] = { "SURE?", "YES", "NO" };
    /* "EXIT GAME", not "EXIT": a player took the bare word for closing the
     * menu and walked out of the game. */
    /* "MUSIC", not "CHANGE MUSIC": twelve letters and the arrow need a box
     * of sixteen, and EXIT GAME's fourteen leaves the panels' figures
     * showing with a column to spare — the score's digits, and a paused
     * race's rival numbers (`--versushud`). */
    static const char *const menu[] = { "PAUSE", "MUSIC", "OPTIONS", "EXIT GAME" };
    if (g_pause_confirm) {
        draw_pmenu(ask, 3, g_pause_yes ? 1 : 2);
    } else if (g_pause_options) {
        draw_options_box();
    } else if (g_pause_tunes) {
        const char *tunes[1 + MUSIC_UNLOCKED_COUNT] = { "MUSIC" };
        int count = pause_tune_count();
        for (int i = 0; i < count; i++) tunes[1 + i] = kMusicNames[i];
        draw_pmenu_list(tunes, 1 + count, 1 + g_tune_row);
    } else {
        draw_pmenu(menu, 1 + PMENU_ROWS, 1 + g_pause_row);
    }
}

/* THE TUNES' WINDOW PLAYS THE TUNE UNDER ITS ARROW, and the menu around it
 * plays nothing. The pause has the engine suspended (pause_toggled); the
 * window lets it go, and every move puts the tune the arrow lands on on
 * from its first bar — an audition, the way LEVEL SETTINGS gives one. NO
 * MUSIC is silence, and so is MUSIC MIX, which is a rotation that belongs to
 * the match and has no one tune to audition (see front_tune). Leaving, the
 * engine is left holding the match's tune and suspended again: the one
 * under the arrow with A, picking up on the unpause where its audition got
 * to; the one there was with B or START. On a prototype whose pause plays
 * on (pause_keeps_music) it is not suspended, as nothing else is.
 *
 * It used to be the other way round: the window was silent and A started
 * the tune in the menu behind it, which was the one place in the pause the
 * music played, and the wrong one. */
static uint8_t g_preview;        /* the tune the window has on: its arrow's */
static bool g_preview_live;      /* ...the engine let go for it */
static bool g_preview_moved;     /* ...and the arrow has left where it opened */

static void tunes_open(void) {
    g_pause_tunes = true;
    g_tune_row = g_preview = g_music;    /* on the one playing */
    g_preview_moved = false;
    /* WHERE THE PAUSE STOPPED IT, not from the top: the arrow is on the
     * match's own tune, so that is what plays — on, as an unpause would
     * have it. Not the mix's, which the arrow on MUSIC MIX does not
     * audition. */
    g_preview_live = g_music != MUSIC_MIX;
    if (!g_preview_live) return;
    nes_audio_play(NES_MUSIC_RESUME);
    if (MUSIC_IS_HANDTUNE(g_music) && !handtune_playing()) {
        if (handtune_current() == MUSIC_HANDTUNE_OF(g_music)) handtune_resume();
        else handtune_start(MUSIC_HANDTUNE_OF(g_music));
    }
}

static void tunes_move(uint8_t row) {
    g_tune_row = g_preview = row;
    g_preview_moved = true;
    if (row == MUSIC_MIX) {
        stop_music();
        return;
    }
    /* Loaded first and only then let go, if the engine is still frozen:
     * see start_music. */
    start_music(row);
    if (!g_preview_live) {
        nes_audio_play(NES_MUSIC_RESUME);
        g_preview_live = true;
    }
}

static void tunes_close(bool take) {
    g_pause_tunes = false;
    if (take && g_tune_row != g_music) {
        g_levelup_resume = 0;        /* the new tune is already on */
        g_music = g_tune_row;
        /* The mix restarts on its current turn rather than from the top. */
        g_mix_step = 0;
    }
    /* The arrow left the match's tune: it goes back on, from its first bar
     * (B or START). With A the arrow IS on it. */
    if (g_preview != g_music) {
        if (g_music == MUSIC_MIX) stop_music();
        else start_music(g_music);
    }
    /* THE MIX'S OWN TUNE WENT AT THE FIRST MOVE, and nothing restarts the
     * engine on the way out of a pause: the unpause owes the match a tune
     * (see g_mix_held). */
    if (g_music != MUSIC_MIX) g_mix_held = false;
    else if (g_preview_moved) g_mix_held = true;
    if (!pause_keeps_music()) {
        if (g_preview_live) nes_audio_play(NES_MUSIC_SUSPEND);
        handtune_suspend();
    }
}

/* THE CURSOR'S TICK UNDER THE PAUSE: the pause has the engine gagged
 * (MUSIC_SUSPEND), and the tick is one of the sounds the gag holds, so it
 * is let through for its length (nes_audio_gag_lower) — the tune stays held.
 * A prototype whose pause plays on has no gag to lower. */
static void pause_tick(void) {
    nes_audio_gag_lower(NES_GAG_TICK_FLOOR, NES_GAG_TICK_FRAMES);
    cursor_blip();
}

/* ...AND A SWITCH THROWN RINGS, which a move does not: the tick for both
 * said "something moved" twice and nothing about the switch. The engine's
 * own bell, which the cartridge never rings (NES_SOUND_DING), let through
 * the gag as the tick is. */
static void pause_ding(void) {
    nes_audio_gag_lower(NES_GAG_TICK_FLOOR, NES_GAG_TICK_FRAMES);
    nes_audio_play(NES_SOUND_DING);
}

/* One frame of it. Returns true if the menu ate the input, which is what
 * keeps the cheat codes out of it — they are entered on the pad while paused
 * too, and a Down meant for this menu is the first byte of one of them. */
static bool pause_menu_on(void);

/* `mine` is what of `pressed` this console's own player pressed: all of it
 * solo, over the cable this console's half (see option_toggle). */
/* THE B THAT LEFT THE MENU IS THE MENU'S UNTIL IT IS LET GO. Handed to the
 * board it turned the piece on the frame the game came back; taken as held
 * already it did not, but a thumb still on it was a held turn, and this game
 * turns a held B again and again (the auto-rotate). So the board does not
 * see it at all until it has been released. Per player, as over the cable
 * either can leave the menu: both consoles strip the same player's B. */
static bool g_menu_b_held[2];
static uint8_t menu_b_swallow(int player, uint8_t buttons) {
    if (!(buttons & TENGEN_BTN_B)) g_menu_b_held[player] = false;
    return g_menu_b_held[player] ? (uint8_t)(buttons & ~TENGEN_BTN_B) : buttons;
}

static bool pause_menu_input(uint8_t pressed, uint8_t mine, bool *leaving,
                             bool *resume) {
    if (!pause_menu_on() || !g_session.game.paused) return false;

    /* START IS ALWAYS RESUME, EVERYWHERE IN THIS MENU — on the EXIT line and
     * inside the question as well. It is the button that put the plaque up,
     * so it is the button that takes it down, and a menu where the meaning of
     * START depends on which line you are standing on is a menu you have to
     * remember rather than read. It is handed STRAIGHT ON to the core, which
     * is what actually unpauses; the question is dropped on the way out, so
     * the next pause opens on the column and not on a half-answered
     * "EXIT?". A takes a choice, B backs out of one. What comes off the
     * screen is pause_toggled's to decide, as the core unpauses: a repaint
     * forced here put the frame that leaves the menu at line 222 of 227.
     * The tunes' window goes the same way, and nothing in it is chosen. */
    if (pressed & TENGEN_BTN_START) {
        g_pause_confirm = false;
        g_pause_options = false;
        if (g_pause_tunes) tunes_close(false);
        return false;
    }

    /* THE OPTIONS' WINDOW: Up and Down walk it and SELECT walks it down, as
     * in the tunes'; A throws the switch under the arrow, and so do Left and
     * Right, which is how a value is changed on the settings screen; B goes
     * back to the menu. Nothing waits to be confirmed — a switch is thrown
     * as it is pressed (option_toggle). */
    /* ...AND IT TICKS as the settings screen does: the cursor's tick for a
     * move, the engine's bell for a switch thrown (pause_ding), the
     * screen's blip for B. */
    if (g_pause_options) {
        bool moved = true;
        if (pressed & TENGEN_BTN_UP)
            g_option_row = (uint8_t)((g_option_row + OPT_COUNT - 1) % OPT_COUNT);
        else if (pressed & (TENGEN_BTN_DOWN | TENGEN_BTN_SELECT))
            g_option_row = (uint8_t)((g_option_row + 1) % OPT_COUNT);
        else
            moved = false;
        if (pressed & (TENGEN_BTN_A | TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) {
            option_toggle(g_option_row,
                          (mine & (TENGEN_BTN_A | TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) != 0);
            pause_ding();
        } else if (pressed & TENGEN_BTN_B) {
            g_pause_options = false;
            screen_blip();
        } else if (moved) {
            pause_tick();
        }
        return true;
    }

    /* THE TUNES' WINDOW: Up and Down walk the list, and SELECT walks it
     * down as it does every list, playing the tune under the arrow as it
     * goes (tunes_move); A takes it and B leaves the one there was, both
     * back to the menu (tunes_close). */
    if (g_pause_tunes) {
        int count = pause_tune_count();
        if (pressed & TENGEN_BTN_UP)
            tunes_move((uint8_t)((g_tune_row + count - 1) % count));
        else if (pressed & (TENGEN_BTN_DOWN | TENGEN_BTN_SELECT))
            tunes_move((uint8_t)((g_tune_row + 1) % count));
        if (pressed & (TENGEN_BTN_A | TENGEN_BTN_B)) {
            tunes_close((pressed & TENGEN_BTN_A) != 0);
            screen_blip();
        }
        return true;
    }

    if (g_pause_confirm) {
        /* The answers stack, so Up and Down move between them as they do on
         * the column, and SELECT does too. Left and Right are kept from when
         * the tune was changed with them on the line above. */
        if (pressed & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT | TENGEN_BTN_UP |
                        TENGEN_BTN_DOWN | TENGEN_BTN_SELECT)) {
            g_pause_yes = !g_pause_yes;
            if (!(pressed & (TENGEN_BTN_A | TENGEN_BTN_B))) pause_tick();
        }
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

    /* B LEAVES THE MENU, AND THE PAUSE WITH IT, as START does: on the
     * menu's own column there is nothing to back out to but the game, and B
     * is what backs out of everything else in it. Handed on as a START
     * (`resume`): it is the core that unpauses; and the B itself is kept
     * from the board until it is let go (menu_b_swallow). */
    if (pressed & TENGEN_BTN_B) {
        *resume = true;
        return false;
    }

    /* SELECT MOVES THE CURSOR, the way it does on the settings screen — the
     * cartridge's own LA048 treats SELECT as another DOWN ($9FBC), and this
     * menu has no reason to be the one place in the port where it does not.
     * Up goes up; with two entries it was the same step as the others, with
     * three it is not. */
    /* ...with the cursor's tick, as processMenuInput plays on every move of
     * the cartridge's own menus (:4655) and this port's front end does. */
    bool moved = true;
    if (pressed & TENGEN_BTN_UP)
        g_pause_row = (uint8_t)((g_pause_row + PMENU_ROWS - 1) % PMENU_ROWS);
    else if (pressed & (TENGEN_BTN_DOWN | TENGEN_BTN_SELECT))
        g_pause_row = (uint8_t)((g_pause_row + 1) % PMENU_ROWS);
    else
        moved = false;
    if (moved && !(pressed & TENGEN_BTN_A)) pause_tick();
    /* A TAKES THE CHOICE, and only A. START used to do it here as well and it
     * cost the menu its way out — see the note at the top. Each entry opens
     * a window of its own; what the menu's box covered and the window's does
     * not comes back from under it (pmenu_cells). */
    if (pressed & TENGEN_BTN_A) {
        if (g_pause_row == PMENU_EXIT) {
            g_pause_confirm = true;
            g_pause_yes = false;   /* NO first: a pause menu does not lose games */
        } else if (g_pause_row == PMENU_OPTIONS) {
            g_pause_options = true;
            g_option_row = 0;
            /* Saved out of the blank, as the tunes' window is below. */
            pmenu_reach(OPT_W, OPT_H);
        } else {
            tunes_open();
            /* WHAT THE WINDOW WILL COVER IS SAVED NOW, out of the blank:
             * nothing under it changes between here and the frame that
             * draws it, and that frame, saving it too, finished at line 223
             * of 227 (`--vblank`). */
            pmenu_reach(PMENU_W_MAX, PMENU_H_MAX);
        }
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

static void pause_toggled(bool was_paused);

/* THE PAUSE MENU OVER THE CABLE. Both consoles have to show the same menu
 * and do the same thing with it, or a tune picked on one plays on one and an
 * EXIT leaves one console in the match alone. So it is not this console's
 * chord that decides whether there is a menu but the MASTER's, which crossed
 * in the lobby (lobby.xe — the chord is what sets XE); and it is not this
 * console's pad that drives it but both players' presses as the transfer
 * carried them, which both consoles have, in the same order. Everything the
 * menu then does — the cursor, the tune, the question, the way out —
 * happens on both on the same transfer. */
static bool g_link_menu;
static uint8_t g_link_prev[2];

void link_match_begin(bool menu) {
    g_link_menu = menu;
    g_levelup_resume = 0;
    g_link_prev[0] = g_link_prev[1] = 0xFF;   /* see swallow_held_buttons */
    g_link_waiting = false;
    g_pad_swap = false;    /* until the first frame says (link_frame_done) */
}

static bool pause_menu_on(void) {
    return g_linked ? g_link_menu : g_pause_unlocked;
}

/* The chord a match is played under: this console's, or over the cable the
 * master's — what the pause menu answers to. */
bool match_chord(void) { return pause_menu_on(); }

/* THE CABLE WENT QUIET: THE MATCH WAITS FOR IT. It used to end two seconds
 * in, which on a real cable is a match lost to a knocked plug. Lockstep can
 * wait for as long as it likes — neither console steps without the other's
 * word, and every word carries the frame it is for, so a gap is only ever a
 * gap and a mismatch is still caught (g_session.desynced, which does end
 * it). So the board freezes with LINK ISSUES in the rival's cell, the tune
 * stops on both consoles and the screen-change chime sounds, the one START
 * makes; the match picks up where it was when the transfers do. Quiet for
 * LINK_GIVEUP_FRAMES and it is over: see link_give_up. MUSIC_SILENCE rather than SUSPEND, which would gag the chime as well;
 * and one frame apart, because the engine's ring takes one request a frame.
 * A paused match is already quiet and stays so. */
static void link_wait(bool starving) {
    if (starving && !g_link_waiting) {
        g_link_waiting = true;
        g_link_wait_frames = 0;
        if (!g_session.game.paused) {
            nes_audio_play(NES_MUSIC_SILENCE);
            if (MUSIC_IS_HANDTUNE(current_tune())) handtune_suspend();
        }
    } else if (g_link_waiting && starving) {
        if (g_link_wait_frames < 255 && ++g_link_wait_frames == 1)
            nes_audio_play(NES_SOUND_SCREEN_SWITCH);
    } else if (g_link_waiting && !starving) {
        g_link_waiting = false;
        g_repaint = true;               /* the words come off the panel */
        if (!g_session.game.paused) {
            if (MUSIC_IS_HANDTUNE(current_tune())) handtune_resume();
            else start_music(g_music);
        }
    }
}

/* THE CABLE IS GONE FOR GOOD: the window goes up in the middle of the
 * screen (draw_cable_lost_box) and START leaves for the title, not the high
 * scores — a match nobody finished has no business on them, and the records
 * swap would only wait on a cable that is not there. Quiet already after a
 * wait, so the chime again to say this is different; a desync comes with
 * the tune still going, so that is stopped instead. */
static void link_give_up(void) {
    if (!g_link_waiting && !g_session.game.paused) {
        nes_audio_play(NES_MUSIC_SILENCE);
        if (MUSIC_IS_HANDTUNE(current_tune())) handtune_suspend();
    } else {
        nes_audio_play(NES_SOUND_SCREEN_SWITCH);
    }
    g_link_lost = true;
    g_link_waiting = false;
    /* Asked now: the port is put away before the window is drawn. */
    g_lost_over_air = wireless_on();
}

/* THE PAD'S A AND B FOR THE NEXT WORDS (g_pad_swap): turned while the match
 * plays on with the player's SWAP A-B on, and never once it is paused, when
 * the shared menu reads the words, or over. */
static bool link_frame_done(bool going) {
    g_pad_swap = going && !g_session.game.paused &&
                 (options_mine() & OPT_SWAP_AB) != 0;
    return going;
}

bool link_play_frame(uint8_t pressed, bool *quit) {
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

        /* The menu first, as in the solo frame, and on both players'
         * presses; if it takes them, neither board sees them. */
        uint8_t local_buttons = tengen_link_buttons(local);
        uint8_t b0 = tengen_link_buttons(f.master), b1 = tengen_link_buttons(f.slave);
        uint8_t new0 = (uint8_t)(b0 & ~g_link_prev[0]);
        uint8_t new1 = (uint8_t)(b1 & ~g_link_prev[1]);
        uint8_t menu_pressed = (uint8_t)(new0 | new1);
        g_link_prev[0] = b0;
        g_link_prev[1] = b1;
        if (g_session.game.paused && pause_menu_on()) {
            bool leaving = false, resume = false;
            if (pause_menu_input(menu_pressed, link_is_master() ? new0 : new1,
                                 &leaving, &resume)) {
                local_buttons = 0;
                remote = (uint16_t)(remote & ~TENGEN_LINK_BUTTON_MASK);
            } else if (resume) {
                /* B as START, for whichever player pressed it — the same
                 * on both consoles, which both have both presses. */
                for (int p = 0; p < 2; p++) {
                    if (!((p == 0 ? new0 : new1) & TENGEN_BTN_B)) continue;
                    if ((p == 0) == link_is_master())
                        local_buttons |= TENGEN_BTN_START;
                    else
                        remote |= TENGEN_BTN_START;
                    g_menu_b_held[p] = true;
                }
            }
            if (leaving) {
                *quit = true;
                return link_frame_done(false);
            }
        }

        /* ...and that B is the menu's until it is let go, on both. */
        {
            int me = link_is_master() ? 0 : 1;
            local_buttons = menu_b_swallow(me, local_buttons);
            remote = (uint16_t)((remote & ~TENGEN_LINK_BUTTON_MASK) |
                                menu_b_swallow(me ^ 1, tengen_link_buttons(remote)));
        }
        TengenStepResult out[2];
        bool was_paused = g_session.game.paused;
        if (!tengen_link_step(&g_session, local_buttons, remote, out))
            break;
        if (was_paused != g_session.game.paused) pause_toggled(was_paused);
        note_award(0, out[0]);
        note_award(1, out[1]);
        stats_watch(0, out[0]);
        stats_watch(1, out[1]);
        cossack_watch(0, out[0]);
        cossack_watch(1, out[1]);
        /* A GAME IS WRITTEN DOWN AS IT ENDS, not at the end of the match:
         * L81DD is called from the top-out itself (main.asm.txt:600), which
         * is what lets a player start again over A+B and keep the game they
         * just finished. Both consoles see both results, so both write the
         * same two rows on the same frame. */
        for (int i = 0; i < 2; i++) if (out[i].topped_out) leader_record(i);
        /* ...and a board put back on its feet is a new board: everything on
         * it, and every counter beside it, has to be drawn again. */
        if (out[0].restarted || out[1].restarted) g_repaint = true;
        /* THE LEVEL-UP'S MUSIC IS THE MATCH'S, NOT THE PLAYER'S. The
         * cartridge has one sound engine for both players, so either one's
         * level-up plays the intro, and here each console used to play it
         * only for its own player. In coop the level is shared: the console
         * whose player had not made the clear went on with its tune while
         * the other stopped KOROBEINIKI or KATIUSKA for the intro and started
         * it again from the top, or turned MUSIC MIX over to the next one —
         * and from there the two consoles played different music. Both see
         * both results on the same step, so both do the same thing. The
         * clears, drops and rumble stay this console's player's. */
        announce(out[g_view], out[0].leveled_up || out[1].leveled_up);
        stepped++;
    }

    (void)pressed;
    /* The intro has run out: the tune again (g_levelup_resume). Counted
     * only while the match moves, so a pause or a quiet cable holds it. */
    if (g_levelup_resume && stepped && !g_session.game.paused &&
        !g_link_waiting) {
        g_levelup_resume = (uint8_t)(g_levelup_resume > stepped
                                         ? g_levelup_resume - stepped : 0);
        if (!g_levelup_resume) start_music(g_music);
    }
    /* A DESYNC IS FINAL at once — the two consoles are no longer playing the
     * same game — and a cable quiet for LINK_GIVEUP_FRAMES is taken for gone. */
    if (g_session.desynced) {
        link_give_up();
        return link_frame_done(false);
    }
    link_wait(stepped == 0 && link_starved() > LINK_LOST_FRAMES);
    if (g_link_waiting && link_starved() > LINK_GIVEUP_FRAMES) {
        link_give_up();
        return link_frame_done(false);
    }
    return link_frame_done(!match_over());
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

/* THE PAUSE WENT UP OR CAME DOWN THIS FRAME: the music, the menu's cursor
 * and the repaint that go with it. Solo and over the cable alike — the
 * linked frame used to toggle the pause inside the core and do none of this,
 * so a paused coop match over the cable played its tune on under the
 * plaque. */
static void pause_toggled(bool was_paused) {
    gbp_rumble_stop();       /* a paused game does not shake */
    /* EVERY PAUSE OPENS ON MUSIC. The cursor used to be left where
     * the last one ended, so a player who had been to EXIT came back
     * to a menu whose START — the button that means resume
     * everywhere else on it — opened the quit question instead. */
    g_pause_row = PMENU_MUSIC;
    g_pause_confirm = false;
    g_pause_tunes = false;
    g_pause_options = false;
    /* pauseOrUnpause suspends and resumes the music
     * (main.asm.txt:7204-7211) — the same pair the front end uses to
     * go quiet, through the same two helpers so the port never loses
     * track of which state the engine is actually in. */
    /* ...EXCEPT IN PROTO_A'S GAME, where the plaque goes up and the
     * tune plays on. The list this came from said it of the
     * prototypes as a family; measured on the dumps it is proto_a's
     * alone — B, C and D go silent like the release (see
     * pause_keeps_music). Only the SUSPEND is skipped, never the
     * resume: a game that was paused before the skin was chosen, or
     * one whose engine is already gagged for any other reason, still
     * has to be let go of. */
    if (!g_session.game.paused)
        nes_audio_play(NES_MUSIC_RESUME);
    else if (!pause_keeps_music())
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
        if (g_session.game.paused && !pause_keeps_music())
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
    /* ...AND ONLY WHAT THE BOX COVERED (g_unpause_patch): the whole repaint
     * was the heaviest frame there is, line 222 of 227 (`--vblank`). The rest
     * of what a pause changes is drawn every frame anyway — the board hidden
     * under the chord, and a race's swap to the rival's board, NEXT, colours
     * and panels (field_view) — so it comes back by itself, and `--unpause`
     * holds the screen to the one a whole repaint leaves. A box never drawn
     * has nothing kept under it: that one repaints. */
    if (was_paused) {
        if (!g_pmenu_drawn_w && !g_plaque_drawn) g_repaint = true;
        else g_unpause_patch = true;
    }
}

/* A GAME PUT BACK FROM THE BATTERY (suspend.c) comes back paused: the
 * plaque, the menu on its first row and the tune suspended, as if its
 * START had just been pressed. */
void match_resumed_paused(void) {
    pause_toggled(false);
    g_repaint = true;
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
    /* A SOLO MATCH SLEEPS ON ITS PAUSE PLAQUE (system_sleep). A running
     * one is paused first, this frame, as its START would do — and sleeps
     * the next, with the keys still held, by which time main has written
     * the paused game to the battery (suspend.c): a console that runs flat
     * asleep still comes back to it. A linked match never gets here with
     * this: the other console would be left talking to nobody. */
    /* ...and the sleep is REMEMBERED from the frame that paused, so keys
     * let go of quickly still get there (they only paused it, and a second
     * try was needed). */
    static bool sleep_next;
    if (g_session.game.paused && (sleep_next || sleep_keys_held())) {
        sleep_next = false;
        system_sleep();
        return !match_over();
    }
    sleep_next = false;
    if (sleep_keys_held() && g_session.game.player[0].game_active) {
        buttons = 0;
        pressed = TENGEN_BTN_START;
        sleep_next = true;
    }
    bool resume = false;
    if (pause_menu_input(pressed, pressed, quit, &resume)) {
        buttons = 0;
        pressed = 0;
    } else if (resume) {
        pressed = (uint8_t)(pressed | TENGEN_BTN_START);
        g_menu_b_held[0] = true;
    }
    buttons = menu_b_swallow(0, buttons);
    if (g_menu_b_held[0]) pressed = (uint8_t)(pressed & ~TENGEN_BTN_B);
    if (g_session.game.player[0].game_active) {
        uint8_t presses[2] = { pressed, 0 };
        TengenCheat cheat[2];
        bool was_paused = g_session.game.paused;
        tengen_pause_input(&g_session.game, presses, cheat);
        if (was_paused != g_session.game.paused) pause_toggled(was_paused);
        /* Every applied code plays this (main.asm.txt:7089, 7127). */
        if (cheat[0] != TENGEN_CHEAT_NONE)
            screen_blip();
    }

    /* SWAP A-B (OPT_SWAP_AB): the turns the other way round. After the
     * menu, which reads the pad as it is. */
    if (options_mine() & OPT_SWAP_AB) buttons = pad_swap_ab(buttons);
    TengenStepResult local = tengen_step(&g_session.game, TENGEN_PLAYER_1, buttons);
    note_award(TENGEN_PLAYER_1, local);
    stats_watch(TENGEN_PLAYER_1, local);
    cossack_watch(TENGEN_PLAYER_1, local);
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
/* UNDER THE CHORD, A NEW RECORD IS HEARD. The match's chord: over a cable
 * the master's, as for the pause menu and the tempo below, so a console
 * that found it alone neither sings nor speeds up in a match that has
 * neither. The first frame this console's
 * player's score passes the top of the table as the match began
 * (g_record_bar), the engine's unused level-up jingle plays — once a match.
 * The cartridge's tune is back in step after it on its own (it plays on
 * underneath, measured); a hand-entered one is ducked for the jingle's
 * length with its clock running, so over a cable the two consoles' music
 * stays together. An empty table (a score of 0 at the top) is no record. */
static void record_watch(void) {
    if (!pause_menu_on() || g_record_sung || !g_record_bar || g_demo ||
        g_session.game.paused)
        return;
    if (g_session.game.player[g_view].score <= g_record_bar) return;
    g_record_sung = true;
    nes_audio_play(NES_MUSIC_UNUSED_LEVELUP);
    handtune_duck(NES_UNUSED_LEVELUP_FRAMES);
}

/* THE TEMPO BY THE STACK, under the match's chord (pause_menu_on; without
 * it the cartridge's tempo, whatever the stack): how high the settled cells on
 * this console's board reach decides how much faster the music runs, in
 * quarters of an extra step a frame (audio_frame). Twenty rows: below
 * twelve, the cartridge's tempo; from twelve, a quarter faster; from fifteen,
 * a half. The falling piece is not counted, so it does not flicker with the
 * piece passing a threshold on its way down. */
#define TEMPO_ROWS_QUARTER 12
#define TEMPO_ROWS_HALF    15
static uint8_t stack_tempo(void) {
    const TengenGame *g = &g_session.game;
    const TengenPlayfield *f = &g->field[g->coop ? 0 : g_view];
    int c0 = g->coop ? 0 : 1, c1 = g->coop ? TENGEN_PF_WIDTH : TENGEN_PF_WIDTH - 1;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = c0; col < c1; col++)
            /* Columns, not values: $0F is the wall's AND a settled block's
             * (the handicap's, or one whose joins a clear broke). */
            if (f->cell[row][col]) {
                int height = TENGEN_PF_HEIGHT - row;
                return height >= TEMPO_ROWS_HALF ? 2 :
                       height >= TEMPO_ROWS_QUARTER ? 1 : 0;
            }
    return 0;
}

/* WHAT IS LEFT OF THE FRAME GOES ON THINKING (WITH COMPUTER under the
 * chord: `deep`). The computer plans a fixed slice a frame inside
 * tengen_ai_buttons, sized for the heaviest turn there is; most turns end
 * well before the next vertical blank, and the rest of that time was spent
 * waiting in vsync. Spent on the plan instead, the choice is made sooner,
 * and sooner is what it lacked: placements a piece could reach at spawn are
 * out of reach a few rows down. So at the end of the turn it thinks on, a
 * unit at a time and the board read once (tengen_ai_think_while), while the
 * scanline is short of AI_SPARE_LINE and the frame has not turned over; the
 * turn still ends in its own frame (`--aiframe`), and a heavy frame simply
 * thinks less. MEASURED on the ROM, with the inner loops in internal WRAM
 * (TENGEN_AI_HOT): a piece's first plan done in 17 frames (median; 90% in
 * 20) where it took 63 (107). And what that is worth, on the host
 * (`ai_tune spare`, the same 3.7 times the thinking a frame, 512 games a
 * kind of partner): 4.27 +- 0.37 lines a game more, every kind of partner
 * better, 6% to 27%. */
#define AI_SPARE_LINE  136
static uint32_t g_turn_vblank;
static bool ai_time_left(void) {
    uint16_t line = REG_VCOUNT;
    if (g_vblank_count != g_turn_vblank) return false;   /* the frame is gone */
    return line >= 160 || line < AI_SPARE_LINE;
}
static void ai_think_spare(void) {
    if (!g_ai_active || !g_ai.smart || !g_ai.deep || g_session.game.paused ||
        g_ai_slot != TENGEN_PLAYER_2 ||
        !g_session.game.player[TENGEN_PLAYER_2].game_active)
        return;
    tengen_ai_think_while(&g_ai, &g_session.game, TENGEN_PLAYER_2, ai_time_left);
}

/* THE PAGE THE BOARD TURNS INTO (see g_end_rise): once the match is over
 * — when the main loop stops stepping it, match_over — under the match's
 * chord, never in the demo and not over a cable gone for good. The plaque
 * alone for END_PAGE_WAIT_FRAMES; then the figures written out once (this
 * player's; a coop board's, both players' together, as its BONUS show
 * counts them), the wall risen a row every END_PAGE_ROW_FRAMES, stood
 * whole END_PAGE_FULL_FRAMES, and let down a row every END_PAGE_ROW_FRAMES.
 * The game over's buttons are as they were, and its clock
 * (GAMEOVER_HOLD_FRAMES) runs END_PAGE_READ_FRAMES longer to read it by
 * (main.c, over_expired, on g_end_rise). Before
 * the vertical blank: the writing out is no drawing. */
static void end_page_frame(void) {
    if (g_demo || g_link_lost || !pause_menu_on() || !match_over()) {
        g_end_frames = 0;
        g_end_rise = -1;
        g_end_curtain = -1;
        return;
    }
    if (g_end_frames < 0xFFFF) g_end_frames++;
    if (g_end_frames <= END_PAGE_WAIT_FRAMES) return;
    if (g_end_rise < 0) {
        TengenStats s = g_session.game.coop
                            ? tengen_stats_sum(&g_stats[0], &g_stats[1])
                            : g_stats[g_view];
        end_stats_compose(&s);
    }
    const int rise = TENGEN_PF_HEIGHT * END_PAGE_ROW_FRAMES;
    int t = g_end_frames - END_PAGE_WAIT_FRAMES - 1;
    int risen = t / END_PAGE_ROW_FRAMES + 1;
    g_end_rise = (int8_t)(risen > TENGEN_PF_HEIGHT ? TENGEN_PF_HEIGHT : risen);
    int down = t < rise + END_PAGE_FULL_FRAMES ? 0
             : (t - rise - END_PAGE_FULL_FRAMES) / END_PAGE_ROW_FRAMES + 1;
    g_end_curtain = (int8_t)(down > TENGEN_PF_HEIGHT ? TENGEN_PF_HEIGHT : down);
}

void draw_match(bool *sweeping) {
    ai_think_spare();
    end_page_frame();
    vsync();
    g_turn_vblank = g_vblank_count;
    if (g_repaint) {
        /* THE WINDOW WENT AND THE WORDS STAYED. draw_static_screen puts the
         * board and the HUD back, which covers everything the pause menu drew
         * on the MAIN background — its frame, and the lines that centre
         * exactly. The rest of it is on the offset and counter layers, and
         * those the static screen has no business in the middle of the board
         * and so never writes: PAUSE, the tune's name and EXIT simply stayed
         * printed over the playfield. They are wiped here, where the plaque
         * is painted over, because that is the same moment — the box that was
         * drawn, at the size it was drawn. */
        if (g_pmenu_drawn_w) clear_pmenu_layers(g_pmenu_drawn_w, g_pmenu_drawn_h);
        g_pmenu_drawn_w = 0;
        g_plaque_drawn = false;
        g_unpause_patch = false;
        draw_static_screen();
        g_repaint = false;
    } else if (g_unpause_patch) {
        if (g_pmenu_drawn_w) pmenu_restore_box();
        if (g_plaque_drawn) plaque_restore();
        g_pmenu_drawn_w = 0;
        g_plaque_drawn = false;
        g_unpause_patch = false;
    } else if (g_hud_dirty && !g_session.game.paused) {
        /* The HUD changed under the pause (hud_hold_changed): drawn whole on
         * the frame AFTER the one that took the box down, not on it — the two
         * together ended at line 225 of 227. */
        g_hud_dirty = false;
        g_repaint = true;
    }
    refresh_palettes();
    /* THE SWEEP'S ONE TIDY-UP, BEFORE THE PANEL: the frame it finishes, the
     * panel's sprites — the idle cossack, a show's troupe — go down after it,
     * not under it. Done after draw_panel it wiped them, and the cossack in
     * HUD STATS blinked out for one frame at the end of every clear. */
    bool clearing =
        g_session.game.player[hud_clearing_slot()].line_clear_timer > 0;
    if (!clearing && *sweeping) {
        oam_hide_all();
        *sweeping = false;
    }
    pause_wall_step();
    draw_field();
    draw_panel();
    /* THE CABLE IS QUIET: said in the rival's cell for as long as the
     * frozen board stays up. See draw_link_issues and link_wait. */
    if (g_link_lost || g_link_waiting) draw_link_issues();

    /* The sweep's sprites. */
    if (clearing) {
        draw_line_clear_sweep();
        *sweeping = true;
    }

    /* After the sweep, and before the plaque. */
    draw_points();

    /* Last, so they sit over whatever was just drawn. */
    if (!g_session.game.player[g_view].game_active) draw_game_over();
    /* The cable gone for good covers everything else, pause included. Over
     * the cable the menu is there when the MASTER found the chord; see
     * link_match_begin. */
    if (g_link_lost) {
        draw_cable_lost_box();
    } else if (g_session.game.paused) {
        if (pause_menu_on()) draw_pause_menu();
        else plaque_draw();
    }

    /* And the sound engine afterwards, out of the blank, where it costs
     * nothing but CPU time. */
    audio_frame();

    /* Under the chord: the jingle for a new record, and the tempo for the
     * NEXT frame's audio_frame. After it, not before: anything here before
     * audio_frame is drawing time as far as the vertical blank is concerned,
     * and scanning the board put the frame that leaves a pause at line 226
     * of 227 (`--vblank`). A frame late is not something an ear hears. */
    record_watch();
    if (pause_menu_on() && !g_session.game.paused && !g_link_waiting &&
        !g_link_lost && g_session.game.player[g_view].game_active)
        g_tempo_quarters = stack_tempo();
}
