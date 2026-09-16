/*
 * test_tengen.c — native (host-compiled) sanity tests for tengen_core.
 *
 * These run with plain gcc/clang, no GBA toolchain required (`make test`).
 * The point is to pin down the verified ROM behavior in cheap, fast
 * assertions BEFORE any of it gets wired to real GBA rendering/input, so a
 * regression shows up here instead of by eyeballing an emulator.
 */
#include "../src/tengen_core.h"
#include "../src/tengen_link.h"
#include "../src/tengen_ai.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void test_rng_is_deterministic_and_never_stalls(void) {
    TengenRng a, b;
    tengen_rng_seed(&a, 0x1234);
    tengen_rng_seed(&b, 0x1234);
    for (int i = 0; i < 1000; i++) {
        uint8_t va = tengen_rng_step(&a);
        uint8_t vb = tengen_rng_step(&b);
        CHECK(va == vb); /* same seed -> same sequence */
    }

    /* Different seeds should (overwhelmingly likely) diverge. */
    TengenRng c;
    tengen_rng_seed(&c, 0xABCD);
    bool diverged = false;
    for (int i = 0; i < 16; i++) {
        if (tengen_rng_step(&a) != tengen_rng_step(&c)) { diverged = true; break; }
    }
    CHECK(diverged);
}

static void test_rng_zero_seed_does_not_lock_up(void) {
    /* eor==0 && diff==0 forces newbit=1 (main.asm.txt:3812-3819), which is
     * exactly what stops an all-zero seed from staying stuck at zero
     * forever — this is the one edge case that pseudocode comment exists
     * for. If this regresses, the RNG can degenerate to a constant 0. */
    TengenRng rng;
    tengen_rng_seed(&rng, 0x0000);
    bool saw_nonzero = false;
    for (int i = 0; i < 32; i++) {
        if (tengen_rng_step(&rng) != 0) { saw_nonzero = true; break; }
    }
    CHECK(saw_nonzero);
}

/* THE ELEVEN PIECES A REAL CARTRIDGE DEALT.
 *
 * Every other test here is the port against the DISASSEMBLY. This one is the
 * port against the ROM itself: tools/nes_cpu.py will run the whole cartridge
 * given a controller and a frame clock (a PPUSTATUS whose vblank flag clears
 * when read, and an NMI fired only while PPUCTRL bit 7 is set), so the game
 * was booted, driven through its four menus into a 1 PLAYER match, and played
 * until it topped out. It reported savedRNGSeed = $C6F0 and dealt, in order:
 *
 *     L J L T S T T S O J L      (5 4 5 2 6 2 2 6 3 4 5)
 *
 * Seeding the port's generator with that same $C6F0 deals the same eleven.
 * That is the RNG, the five steps per draw, the mask, the reroll on zero and
 * the piece numbering, all confirmed end to end against the hardware's own
 * code rather than against a reading of it.
 *
 * Only the first eleven, because the run held Down and topped out at level 0
 * in about two thousand frames. Eleven is already far past coincidence: a
 * 1-in-7 draw makes any particular eleven a 1-in-2e9 sequence, and a
 * brute-force over all 65536 seeds finds exactly four that produce it — $C6F0,
 * and the three that differ from it only in bits this many draws cannot
 * reach. */
static void test_deals_the_pieces_a_real_cartridge_dealt(void) {
    static const uint8_t kCartridgePieces[] = {
        TT_L, TT_J, TT_L, TT_T, TT_S, TT_T, TT_T, TT_S, TT_O, TT_J, TT_L
    };
    TengenGame game;
    tengen_new_game(&game, 0xC6F0, 0, false, false, false);

    /* new_game pre-rolls: `current` is the first draw and `next` the second,
     * so the first two come off the spawn and the rest off the piece after
     * it. Rather than play the match out, this walks the same generator the
     * spawn walks — the player's own lookahead RNG, in the state new_game
     * left it. */
    CHECK(game.player[0].piece.current == kCartridgePieces[0]);
    CHECK(game.player[0].piece.next == kCartridgePieces[1]);
    TengenRng rng = game.player[0].rng;
    for (unsigned i = 2; i < sizeof kCartridgePieces; i++) {
        uint8_t v;
        do {
            for (int s = 0; s < 5; s++) v = tengen_rng_step(&rng);
            v &= 7;
        } while (v == 0);
        CHECK(v == kCartridgePieces[i]);
    }
}

static void test_piece_selector_never_returns_none_and_covers_all_seven(void) {
    /* Exercise roll_next_piece indirectly via tengen_new_game/spawn, across
     * many seeds, and confirm every piece id 1..7 shows up and TT_NONE (0)
     * never does — mirrors "reroll on 0 of 8" (main.asm.txt:3700-3703). */
    bool seen[TENGEN_TETROMINO_COUNT] = {0};
    for (uint16_t seed = 1; seed < 400; seed++) {
        TengenGame game;
        tengen_new_game(&game, seed, 0, false, false, false);
        CHECK(game.player[0].piece.current != TT_NONE);
        CHECK(game.player[0].piece.next != TT_NONE);
        seen[game.player[0].piece.current] = true;
        seen[game.player[0].piece.next] = true;
    }
    for (int piece = TT_I; piece <= TT_Z; piece++) {
        CHECK(seen[piece]);
    }
}

static void test_spawn_position_matches_rom(void) {
    TengenGame game;
    tengen_new_game(&game, 42, 0, false, false, false);
    CHECK(game.player[0].piece.y == TENGEN_SPAWN_Y);
    /* 1P uses entry [2] (= 7), NOT entry [0] — the table is only indexed by
     * player in coop. main.asm.txt:3716-3720. */
    CHECK(game.player[0].piece.x == TENGEN_SPAWN_X[2]);
    CHECK(game.player[0].piece.x == 7);
    CHECK(game.player[0].piece.orientation == 0);
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));

    /* Spawn sits above the visible field, so the piece slides in. */
    CHECK(TENGEN_SPAWN_Y < TENGEN_ROM_ROW_ORIGIN);

    /* Coop spawns the two players on opposite sides of one shared field. */
    TengenGame coop;
    tengen_new_game(&coop, 42, 0, true, true, false);
    CHECK(coop.player[0].piece.x == TENGEN_SPAWN_X[0]);
    CHECK(coop.player[1].piece.x == TENGEN_SPAWN_X[1]);
}

static void test_walls_are_present_in_1p_and_absent_in_coop(void) {
    /* initPlayer1orCoopPlayfield writes solid wall nibbles in 1P/2P and
     * leaves them open in coop, widening coop to 12 columns
     * (main.asm.txt:3468-3495 and its own comment at :3483). */
    TengenGame solo;
    tengen_new_game(&solo, 5, 0, false, false, false);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        CHECK(solo.field[0].cell[row][0] == TT_WALL);
        CHECK(solo.field[0].cell[row][TENGEN_PF_WIDTH - 1] == TT_WALL);
        CHECK(solo.field[0].cell[row][1] == TT_NONE);
        CHECK(solo.field[0].cell[row][TENGEN_PF_WIDTH - 2] == TT_NONE);
    }

    TengenGame coop;
    tengen_new_game(&coop, 5, 0, true, true, false);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        CHECK(coop.field[0].cell[row][0] == TT_NONE);
        CHECK(coop.field[0].cell[row][TENGEN_PF_WIDTH - 1] == TT_NONE);
    }
}

static void test_walls_block_movement_in_1p_but_not_coop(void) {
    /* The same x that runs into a wall in 1P is playable in coop. */
    TengenGame solo;
    tengen_new_game(&solo, 6, 0, false, false, false);
    solo.player[0].piece.current = TT_O;   /* occupies local cols 0-1 */
    solo.player[0].piece.y = 10;
    solo.player[0].piece.x = TENGEN_ROM_COL_ORIGIN + 1; /* flush against the left wall */
    CHECK(tengen_position_valid(&solo, TENGEN_PLAYER_1));
    CHECK(!tengen_try_move(&solo, TENGEN_PLAYER_1, -1));

    TengenGame coop;
    tengen_new_game(&coop, 6, 0, true, true, false);
    coop.player[0].piece.current = TT_O;
    coop.player[0].piece.y = 10;
    coop.player[0].piece.x = TENGEN_ROM_COL_ORIGIN + 1;
    CHECK(tengen_try_move(&coop, TENGEN_PLAYER_1, -1)); /* the extra column exists here */
}

static void test_top_out_when_piece_rests_above_the_field(void) {
    /* main.asm.txt:588-590: a piece that comes to rest with its box top still
     * above the visible field ends the game. Fill the field solid, then let a
     * piece fall onto it. */
    TengenGame game;
    tengen_new_game(&game, 8, 0, false, false, false);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++) {
            game.field[0].cell[row][col] = TT_I;
        }
    }
    game.player[0].piece.current = TT_O;
    game.player[0].piece.x = 7;
    game.player[0].piece.y = TENGEN_SPAWN_Y;

    bool topped = false;
    for (int frame = 0; frame < 600 && !topped; frame++) {
        TengenStepResult r = tengen_step(&game, TENGEN_PLAYER_1, 0);
        topped = r.topped_out;
    }
    CHECK(topped);
    CHECK(!game.player[0].game_active);
}

static void test_score_is_awarded_per_piece_and_rewards_height(void) {
    /* Verified formula: (level+1) * ((level+1) + rows_above_floor), where
     * rows_above_floor counts from the obstruction up to the floor — so a
     * piece resting high scores MORE, not less (main.asm.txt:3874-3893). */
    TengenGame low, high;
    tengen_new_game(&low, 21, 0, false, false, false);
    tengen_new_game(&high, 21, 0, false, false, false);

    /* `high` gets a stack to land on early; `low` falls all the way down. */
    for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++) {
        high.field[0].cell[2][col] = TT_I;
    }

    for (int frame = 0; frame < 2000; frame++) {
        if (low.player[0].score == 0) tengen_step(&low, TENGEN_PLAYER_1, 0);
        if (high.player[0].score == 0) tengen_step(&high, TENGEN_PLAYER_1, 0);
    }
    CHECK(low.player[0].score > 0);
    CHECK(high.player[0].score > 0);
    CHECK(high.player[0].score > low.player[0].score);
}

static void test_o_piece_never_needs_a_kick(void) {
    /* O is the same in every orientation, at every x, so any wall-kick bug
     * that accidentally *requires* movement to succeed would show up here
     * as an always-true result with an unexpectedly shifted x. */
    TengenGame game;
    tengen_new_game(&game, 7, 0, false, false, false);
    game.player[0].piece.current = TT_O;
    int8_t x_before = game.player[0].piece.x;
    CHECK(tengen_try_rotate(&game, TENGEN_PLAYER_1, true));
    CHECK(game.player[0].piece.x == x_before); /* no kick should have been needed */
}

static void test_wall_kick_only_ever_shifts_left(void) {
    /* Force a T piece flush against the right wall in an orientation whose
     * rotation would collide with the wall, and confirm: (a) rotation still
     * succeeds via a one-column-LEFT kick, matching the documented Tengen
     * quirk quoted in reference/disasm/notes.txt ("this game will wallkick
     * one square to the left if basic rotation fails"), and (b) a
     * shifted-right kick is never attempted (there is no code path for it). */
    TengenGame game;
    tengen_new_game(&game, 3, 0, false, false, false);
    TengenPiece *piece = &game.player[0].piece;
    piece->current = TT_T;
    piece->orientation = 1; /* vertical T (main.asm.txt:1111): local cols 0-1 */
    piece->y = 10;
    /* Rightmost x where this orientation still fits: its local columns land on
     * the last two playable storage columns. */
    piece->x = TENGEN_ROM_COL_ORIGIN + TENGEN_PF_WIDTH - 3;
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));

    /* Orientation 2 is three columns wide, so rotating in place would put a
     * cell in the wall — the rotation can only succeed via the left kick. */
    int8_t x_before = piece->x;
    CHECK(tengen_try_rotate(&game, TENGEN_PLAYER_1, true));
    CHECK(piece->orientation == 2);
    CHECK(piece->x == x_before - 1); /* kicked exactly one column left */
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));
}

static void test_rotation_fails_cleanly_when_the_kick_cannot_help(void) {
    /* Against the LEFT wall the kick shifts further into the wall, so it
     * can't rescue the rotation — the ROM still only ever tries left, and
     * the piece must end up exactly as it started. */
    TengenGame game;
    tengen_new_game(&game, 4, 0, false, false, false);
    TengenPiece *piece = &game.player[0].piece;
    piece->current = TT_I;
    piece->orientation = 1; /* vertical I ($44,$44): occupies local column 1 only */
    piece->y = 10;
    piece->x = TENGEN_ROM_COL_ORIGIN; /* puts that column in storage col 1, the leftmost playable one */
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));

    int8_t x_before = piece->x;
    uint8_t orientation_before = piece->orientation;
    /* Horizontal I spans four columns starting at local col 0, which would
     * run off the left edge both in place and one column further left. */
    CHECK(!tengen_try_rotate(&game, TENGEN_PLAYER_1, true));
    CHECK(piece->x == x_before);
    CHECK(piece->orientation == orientation_before);
}

static void test_move_rejects_out_of_bounds(void) {
    TengenGame game;
    tengen_new_game(&game, 1, 0, false, false, false);
    game.player[0].piece.current = TT_O; /* occupies local columns 0-1 */
    game.player[0].piece.y = 10;
    /* Sitting flush against the left wall: legal here, illegal one further. */
    game.player[0].piece.x = TENGEN_ROM_COL_ORIGIN + 1;
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));
    CHECK(!tengen_try_move(&game, TENGEN_PLAYER_1, -1));
    CHECK(game.player[0].piece.x == TENGEN_ROM_COL_ORIGIN + 1); /* reverted */
}

static void test_line_clear_detects_and_collapses(void) {
    /* Build a 1P field: walls in the outer columns, ten playable cells. */
    TengenPlayfield field;
    memset(&field, 0, sizeof(field));
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        field.cell[row][0] = TT_WALL;
        field.cell[row][TENGEN_PF_WIDTH - 1] = TT_WALL;
    }
    /* Filling only the PLAYABLE columns must count as a full row — the wall
     * sentinels are what make that work without mode-specific bounds. */
    for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++) {
        field.cell[TENGEN_PF_HEIGHT - 1][col] = TT_I;
    }
    field.cell[TENGEN_PF_HEIGHT - 2][1] = TT_T; /* row above: a single marker cell */

    uint32_t mask = tengen_clear_full_rows(&field);
    CHECK(mask == (1u << (TENGEN_PF_HEIGHT - 1)));
    /* The marker cell should have dropped down into the now-empty bottom row. */
    CHECK(field.cell[TENGEN_PF_HEIGHT - 1][1] == TT_T);
    for (int col = 2; col < TENGEN_PF_WIDTH - 1; col++) {
        CHECK(field.cell[TENGEN_PF_HEIGHT - 1][col] == TT_NONE);
    }
    /* And the frame must survive the collapse, including on the row that was
     * vacated at the top. */
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        CHECK(field.cell[row][0] == TT_WALL);
        CHECK(field.cell[row][TENGEN_PF_WIDTH - 1] == TT_WALL);
    }
}

static void test_a_row_of_walls_alone_is_not_a_full_row(void) {
    /* Guards the sentinel trick from the obvious failure mode: an empty 1P
     * field must not read as twenty completed lines. */
    TengenPlayfield field;
    memset(&field, 0, sizeof(field));
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        field.cell[row][0] = TT_WALL;
        field.cell[row][TENGEN_PF_WIDTH - 1] = TT_WALL;
    }
    CHECK(tengen_clear_full_rows(&field) == 0);
}

static void test_level_up_thresholds_match_rom_table(void) {
    /* main.asm.txt:1473-1478, bonusLinesTable, held here as the ROM stores
     * it: ASCII digit pairs "03","06",...,"95". */
    CHECK(TENGEN_LEVEL_LINE_TENS[0] == 3);
    CHECK(TENGEN_LEVEL_LINE_TENS[1] == 6);
    CHECK(TENGEN_LEVEL_LINE_TENS[4] == 15);
    CHECK(TENGEN_LEVEL_LINE_TENS[5] == 20);  /* the table switches from +3 to +5 here */
    CHECK(TENGEN_LEVEL_LINE_TENS[20] == 95);

    /* AND THOSE PAIRS ARE HUNDREDS-AND-TENS, not tens-and-ones: both readers
     * compare them against player1LinesHundreds/player1LinesTens (:1482-1487,
     * :3145-3151). So the first level-up is at THIRTY lines, not three, and
     * the step widens from 30 to 50 at the sixth entry, not from 3 to 5. This
     * is the test that would have caught the port shipping a level every
     * three lines. */
    for (int i = 0; i < 21; i++)
        CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[i] == TENGEN_LEVEL_LINE_TENS[i] * 10u);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[0] == 30);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[4] == 150);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[5] == 200);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[20] == 950);
}

/* Drives the game until the player has cleared at least `target_lines`.
 *
 * Each round it stages a bottom row that is full except for the exact two
 * columns a centred O piece drops into, then forces the active piece to be
 * that O. Leaving a gap the piece can't actually fill is the easy way to
 * write a test that passes while exercising nothing, so the caller should
 * always assert the line count really moved. */
static void clear_lines_until(TengenGame *game, uint32_t target_lines) {
    /* An O spawned at x=7 occupies local columns 0-1, i.e. storage 5 and 6. */
    const int gap_left = 5, gap_right = 6;

    for (int guard = 0; guard < 200 && game->player[0].lines < target_lines; guard++) {
        if (!game->player[0].game_active) return;

        for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
            for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++) {
                bool bottom = (row == TENGEN_PF_HEIGHT - 1);
                bool in_gap = (col == gap_left || col == gap_right);
                game->field[0].cell[row][col] = (bottom && !in_gap) ? TT_I : TT_NONE;
            }
        }

        game->player[0].piece.current = TT_O;
        game->player[0].piece.orientation = 0;
        game->player[0].piece.x = 7;
        game->player[0].piece.y = TENGEN_SPAWN_Y;

        uint32_t lines_before = game->player[0].lines;
        for (int frame = 0; frame < 4000 && game->player[0].lines == lines_before; frame++) {
            if (!game->player[0].game_active) return;
            tengen_step(game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN);
        }
    }
}

static void test_completed_rows_wait_before_they_collapse(void) {
    /* The ROM holds the game for lineClearTimerP1 frames after finding
     * completed rows, animates them, and only then collapses
     * (main.asm.txt:1192-1197). A core that clears instantly gives a renderer
     * nothing to animate, so this pins the two phases apart. */
    TengenGame game;
    tengen_new_game(&game, 91, 0, false, false, false);

    /* Stage a bottom row that is full except where a centred O will land. */
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
            game.field[0].cell[row][col] =
                (row == TENGEN_PF_HEIGHT - 1 && col != 5 && col != 6) ? TT_I : TT_NONE;

    game.player[0].piece.current = TT_O;
    game.player[0].piece.orientation = 0;
    game.player[0].piece.x = 7;
    game.player[0].piece.y = TENGEN_SPAWN_Y;

    /* Run until the rows are found. */
    int found_at = -1;
    for (int frame = 0; frame < 4000 && found_at < 0; frame++) {
        if (tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN).lines_cleared)
            found_at = frame;
    }
    CHECK(found_at >= 0);

    /* At that moment the row is named, the timer is armed, and — the point —
     * the row is still standing. */
    CHECK(game.player[0].line_clear_timer == TENGEN_LINE_CLEAR_FRAMES);
    CHECK(game.player[0].clearing_rows == (1u << (TENGEN_PF_HEIGHT - 1)));
    CHECK(game.player[0].lines == 0);
    for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
        CHECK(game.field[0].cell[TENGEN_PF_HEIGHT - 1][col] != TENGEN_CELL_EMPTY);

    /* It collapses exactly when the timer runs out, not before. */
    bool collapsed = false;
    int frames = 0;
    for (; frames < 200 && !collapsed; frames++)
        collapsed = tengen_step(&game, TENGEN_PLAYER_1, 0).lines_collapsed;

    CHECK(collapsed);
    CHECK(frames == TENGEN_LINE_CLEAR_FRAMES);
    CHECK(game.player[0].lines == 1);
    CHECK(game.player[0].line_clear_timer == 0);
    CHECK(game.player[0].clearing_rows == 0);
}

static void test_line_clear_sweep_advances_every_other_frame(void) {
    /* stageLineClearAnimation decrements the timer every frame but only moves
     * the sweep when what's left is odd (main.asm.txt:1279-1283), so the puff
     * of smoke crosses one column per two frames. Getting this wrong is the
     * difference between an animation that fits the hold and one that either
     * races off the field or never finishes crossing it. */
    TengenGame game;
    tengen_new_game(&game, 91, 0, false, false, false);

    /* No clear running: no sweep. */
    CHECK(tengen_line_clear_step(&game, TENGEN_PLAYER_1) == 0);

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
            game.field[0].cell[row][col] =
                (row == TENGEN_PF_HEIGHT - 1 && col != 5 && col != 6) ? TT_I : TT_NONE;

    game.player[0].piece.current = TT_O;
    game.player[0].piece.orientation = 0;
    game.player[0].piece.x = 7;
    game.player[0].piece.y = TENGEN_SPAWN_Y;

    bool found = false;
    for (int frame = 0; frame < 4000 && !found; frame++)
        found = tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN).lines_cleared;
    CHECK(found);

    /* The head is staged at the row's first column on the frame the row is
     * found, and has not moved yet. */
    CHECK(tengen_line_clear_step(&game, TENGEN_PLAYER_1) == 0);

    /* Then one column per two frames, all the way to the collapse. */
    uint8_t last = 0;
    int advances = 0;
    for (int frame = 0; frame < TENGEN_LINE_CLEAR_FRAMES; frame++) {
        bool collapsed = tengen_step(&game, TENGEN_PLAYER_1, 0).lines_collapsed;
        uint8_t step = tengen_line_clear_step(&game, TENGEN_PLAYER_1);
        if (collapsed) {
            CHECK(frame == TENGEN_LINE_CLEAR_FRAMES - 1);
            CHECK(step == 0);   /* nothing left to animate */
            break;
        }
        CHECK(step == last || step == last + 1);
        if (step != last) advances++;
        last = step;
    }
    /* 29 frames of hold, one advance per two of them. */
    CHECK(advances == TENGEN_LINE_CLEAR_FRAMES / 2);

    /* And that is far enough for the head to have crossed the whole field,
     * which is what makes the sweep look like it clears the row rather than
     * stalling halfway. */
    CHECK(advances >= TENGEN_PF_WIDTH);
}

/* ----------------------------------------------------------------------- *
 * Cheat codes
 * ----------------------------------------------------------------------- */

/* Feeds one button press, as a press-then-release pair of frames, through
 * the pause/code path for player 1. Returns whatever fired. */
static TengenCheat press_code_button(TengenGame *game, uint8_t button) {
    TengenCheat fired[2];
    uint8_t presses[2] = { button, 0 };
    tengen_pause_input(game, presses, fired);
    tengen_step(game, TENGEN_PLAYER_1, button);
    uint8_t none[2] = { 0, 0 };
    tengen_pause_input(game, none, NULL);
    tengen_step(game, TENGEN_PLAYER_1, 0);
    return fired[0];
}

static TengenCheat enter_code(TengenGame *game, const uint8_t *buttons, int count) {
    TengenCheat last = TENGEN_CHEAT_NONE;
    for (int i = 0; i < count; i++) last = press_code_button(game, buttons[i]);
    return last;
}

static const uint8_t kLevelUpButtons[9] = {
    TENGEN_BTN_UP, TENGEN_BTN_DOWN, TENGEN_BTN_UP, TENGEN_BTN_DOWN,
    TENGEN_BTN_LEFT, TENGEN_BTN_RIGHT, TENGEN_BTN_B, TENGEN_BTN_B, TENGEN_BTN_A
};
static const uint8_t kLongBarButtons[8] = {
    TENGEN_BTN_DOWN, TENGEN_BTN_DOWN, TENGEN_BTN_LEFT, TENGEN_BTN_RIGHT,
    TENGEN_BTN_LEFT, TENGEN_BTN_RIGHT, TENGEN_BTN_B, TENGEN_BTN_A
};
static const uint8_t kUndoButtons[9] = {
    TENGEN_BTN_LEFT, TENGEN_BTN_DOWN, TENGEN_BTN_RIGHT, TENGEN_BTN_UP,
    TENGEN_BTN_LEFT, TENGEN_BTN_DOWN, TENGEN_BTN_RIGHT, TENGEN_BTN_B,
    TENGEN_BTN_A
};

static void pause_game(TengenGame *game) {
    uint8_t presses[2] = { TENGEN_BTN_START, 0 };
    tengen_pause_input(game, presses, NULL);
    CHECK(game->paused);
}

static void test_start_pauses_and_stops_the_game(void) {
    TengenGame game;
    tengen_new_game(&game, 7, 0, false, false, false);
    CHECK(!game.paused);

    int8_t y_before = game.player[0].piece.y;
    pause_game(&game);

    /* 200 frames of gravity and input, all of it ignored. */
    for (int i = 0; i < 200; i++) tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN);
    CHECK(game.player[0].piece.y == y_before);

    uint8_t presses[2] = { TENGEN_BTN_START, 0 };
    tengen_pause_input(&game, presses, NULL);
    CHECK(!game.paused);
    for (int i = 0; i < 200; i++) tengen_step(&game, TENGEN_PLAYER_1, 0);
    CHECK(game.player[0].piece.y != y_before);
}

static void test_codes_only_count_while_paused(void) {
    /* checkCodeInput is only reached from the paused branch of pauseOrUnpause
     * (main.asm.txt:7186-7192), so a code typed during play does nothing. */
    TengenGame game;
    tengen_new_game(&game, 7, 3, false, false, false);
    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_NONE);
    CHECK(game.player[0].level == 3);

    pause_game(&game);
    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == 4);
}

static void test_level_up_code_repeats_on_its_last_button(void) {
    /* The ROM never rewinds the match cursor when a code completes, so the
     * final A on its own fires it again (main.asm.txt:7095-7098 is only
     * reached from the partial-match paths). This is the well-known way the
     * level-up code is used to climb quickly. */
    TengenGame game;
    tengen_new_game(&game, 7, 0, false, false, false);
    pause_game(&game);

    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == 1);
    CHECK(press_code_button(&game, TENGEN_BTN_A) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == 2);
    CHECK(press_code_button(&game, TENGEN_BTN_A) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == 3);
}

static void test_level_up_code_stops_at_the_rom_cap(void) {
    TengenGame game;
    tengen_new_game(&game, 7, 9, false, false, false);
    pause_game(&game);
    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_LEVEL_UP);
    for (int i = 0; i < 40; i++) press_code_button(&game, TENGEN_BTN_A);
    CHECK(game.player[0].level == TENGEN_MAX_LEVEL);
}

static void test_long_bar_code_gives_an_i_once_per_level(void) {
    TengenGame game;
    tengen_new_game(&game, 7, 0, false, false, false);
    pause_game(&game);

    TengenTetromino next_before = game.player[0].piece.next;
    CHECK(enter_code(&game, kLongBarButtons, 8) == TENGEN_CHEAT_LONG_BAR);
    CHECK(game.player[0].piece.current == TT_I);
    /* It replaces what you were holding and does NOT consume the preview. */
    CHECK(game.player[0].piece.next == next_before);
    /* Dropped in at the top, upright, with a fresh timer. */
    CHECK(game.player[0].piece.y == TENGEN_SPAWN_Y);
    CHECK(game.player[0].piece.orientation == 0);

    /* Second time on the same level: refused. */
    game.player[0].piece.current = TT_S;
    CHECK(enter_code(&game, kLongBarButtons, 8) == TENGEN_CHEAT_NONE);
    CHECK(game.player[0].piece.current == TT_S);

    /* The cheat level-up deliberately does not hand it back... */
    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(enter_code(&game, kLongBarButtons, 8) == TENGEN_CHEAT_NONE);

    /* ...but levelling up by play does (main.asm.txt:3189-3190). */
    game.player[0].long_bar_code_used = 0;
    CHECK(enter_code(&game, kLongBarButtons, 8) == TENGEN_CHEAT_LONG_BAR);
    CHECK(game.player[0].piece.current == TT_I);
}

/* Drops one piece to the bottom of an empty field and returns the row it
 * settled on. */
/* Down until the piece plants, AND ONE FRAME MORE: a lock leaves `current`
 * at zero and the frame after it is the one that deals the replacement, the
 * way activeGamePlay does (see tengen_core.c's spawn branch). Callers here
 * want the board settled and the next piece in hand. */
static int drop_one_piece(TengenGame *game) {
    for (int frame = 0; frame < 4000; frame++) {
        if (tengen_step(game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN).piece_locked) {
            tengen_step(game, TENGEN_PLAYER_1, 0);
            return frame;
        }
    }
    return -1;
}

static int occupied_cells(const TengenPlayfield *field) {
    int n = 0;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
            if (field->cell[row][col] != TENGEN_CELL_EMPTY) n++;
    return n;
}

static void test_undo_code_takes_the_last_piece_back_once(void) {
    TengenGame game;
    tengen_new_game(&game, 23, 0, false, false, false);

    TengenTetromino dropped = game.player[0].piece.current;
    CHECK(drop_one_piece(&game) >= 0);
    CHECK(occupied_cells(&game.field[0]) == 4);
    TengenTetromino falling = game.player[0].piece.current;

    pause_game(&game);
    CHECK(enter_code(&game, kUndoButtons, 9) == TENGEN_CHEAT_UNDO);

    /* The piece comes out of the stack and back into your hand, and the one
     * you were holding goes back to being next. */
    CHECK(occupied_cells(&game.field[0]) == 0);
    CHECK(game.player[0].piece.current == dropped);
    CHECK(game.player[0].piece.next == falling);
    CHECK(game.player[0].piece.y == TENGEN_SPAWN_Y);

    /* Once per game: a second undo is refused even after another drop. */
    uint8_t unpause[2] = { TENGEN_BTN_START, 0 };
    tengen_pause_input(&game, unpause, NULL);
    CHECK(!game.paused);
    CHECK(drop_one_piece(&game) >= 0);
    pause_game(&game);
    CHECK(enter_code(&game, kUndoButtons, 9) == TENGEN_CHEAT_NONE);
    CHECK(occupied_cells(&game.field[0]) == 4);
}

static void test_undo_is_disarmed_by_a_line_clear(void) {
    /* L94E4 clears lastCurrentBlock as the rows come down
     * (main.asm.txt:3087-3092), so there is nothing to put back. */
    TengenGame game;
    tengen_new_game(&game, 91, 0, false, false, false);

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
            game.field[0].cell[row][col] =
                (row == TENGEN_PF_HEIGHT - 1 && col != 5 && col != 6) ? TT_I : TT_NONE;

    game.player[0].piece.current = TT_O;
    game.player[0].piece.orientation = 0;
    game.player[0].piece.x = 7;
    game.player[0].piece.y = TENGEN_SPAWN_Y;

    bool collapsed = false;
    for (int frame = 0; frame < 4000 && !collapsed; frame++)
        collapsed = tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN).lines_collapsed;
    CHECK(collapsed);
    CHECK(game.player[0].lines == 1);

    pause_game(&game);
    CHECK(enter_code(&game, kUndoButtons, 9) == TENGEN_CHEAT_NONE);
}

static void test_a_wrong_button_restarts_the_code(void) {
    TengenGame game;
    tengen_new_game(&game, 7, 0, false, false, false);
    pause_game(&game);

    /* Six of the nine, then a wrong one. */
    for (int i = 0; i < 6; i++) press_code_button(&game, kLevelUpButtons[i]);
    CHECK(press_code_button(&game, TENGEN_BTN_SELECT) == TENGEN_CHEAT_NONE);
    /* Finishing the code from where it was left off must do nothing... */
    for (int i = 6; i < 9; i++)
        CHECK(press_code_button(&game, kLevelUpButtons[i]) == TENGEN_CHEAT_NONE);
    CHECK(game.player[0].level == 0);
    /* ...and starting over must work. */
    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == 1);
}

static void test_codes_share_one_cursor_the_way_the_rom_does(void) {
    /* All three codes live in one table and one cursor walks it, so a first
     * press that starts two codes commits to the one the ROM tests first.
     * Down starts the long bar; Left starts the undo; Up starts level-up. */
    TengenGame game;
    tengen_new_game(&game, 7, 0, false, false, false);
    pause_game(&game);

    /* Down, then the rest of the LONG BAR code: that is what Down commits to,
     * even though Down is also the second byte of the level-up code. */
    CHECK(enter_code(&game, kLongBarButtons, 8) == TENGEN_CHEAT_LONG_BAR);

    /* And Left commits to the undo code, not to level-up's fifth byte. */
    press_code_button(&game, TENGEN_BTN_SELECT);   /* clears the cursor */
    for (int i = 0; i < 7; i++) press_code_button(&game, kUndoButtons[i]);
    CHECK(game.player[0].code_input_y == 0x13 + 7);
}

/* ----------------------------------------------------------------------- *
 * Linked two-player games
 * ----------------------------------------------------------------------- */

/* A scripted, deliberately messy button sequence, so the two machines are
 * exercised on real divergent play rather than on both sitting still. */
static uint8_t scripted_buttons(int player, int frame) {
    static const uint8_t kMoves[8] = {
        0, TENGEN_BTN_LEFT, TENGEN_BTN_RIGHT, TENGEN_BTN_A,
        TENGEN_BTN_DOWN, TENGEN_BTN_B, TENGEN_BTN_LEFT | TENGEN_BTN_DOWN, 0
    };
    return kMoves[(frame * (player ? 5 : 3) + player * 2) & 7];
}

static void test_the_walls_reach_above_the_visible_field(void) {
    /* The bug this exists for: a piece could be walked sideways INTO the wall
     * column while it was still above the field, because only the twenty
     * visible rows were checked and everything above them counted as open.
     * The first row it then descended into blocked it, so it came to rest at
     * y=5 — one short of TENGEN_TOPOUT_ROW — and the game ended. Four pieces
     * into an empty board, with nothing on screen to explain it.
     *
     * The ROM has no such gap: L89C3 (main.asm.txt:1481-1503) writes the
     * $F0/$0F wall nibbles into every row of the buffer, spawn rows included. */
    TengenGame game;
    tengen_new_game(&game, 1, 0, false, false, false);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
            game.field[0].cell[row][col] = TENGEN_CELL_EMPTY;

    /* x = 2 puts the T's leftmost cell in the wall column. It must be refused
     * at every row, not just the ones inside the field. */
    for (int y = 0; y <= 8; y++) {
        game.player[0].piece.current = TT_T;
        game.player[0].piece.orientation = 0;
        game.player[0].piece.x = 2;
        game.player[0].piece.y = (int8_t)y;
        CHECK(!tengen_position_valid(&game, TENGEN_PLAYER_1));
    }
    /* ...and one column further in is fine at every row, so the fix did not
     * simply wall off the spawn area. */
    for (int y = 0; y <= 8; y++) {
        game.player[0].piece.x = 3;
        game.player[0].piece.y = (int8_t)y;
        CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));
    }
}

/* Random play must never end a game while the board is nearly empty. This is
 * the check that would have caught the wall bug on the day it was written:
 * the rule it tests is not a ROM detail, it is "a game does not end for no
 * reason", which is exactly what a player notices and a unit test of any one
 * routine does not. */
static void test_random_play_never_tops_out_on_a_nearly_empty_board(void) {
    unsigned state = 12345u;
    for (unsigned seed = 1; seed < 300; seed++) {
        TengenGame game;
        tengen_new_game(&game, (uint16_t)seed, 0, false, false, false);
        uint8_t held = 0;
        for (int frame = 0; frame < 3000; frame++) {
            state = state * 1664525u + 1013904223u;
            unsigned r = state >> 16;
            if ((r & 15) == 0) held = (uint8_t)((r >> 4) & 0xFF);
            held = (uint8_t)(held & ~TENGEN_BTN_START);   /* never pause */
            TengenStepResult step = tengen_step(&game, TENGEN_PLAYER_1, held);
            if (!step.topped_out) continue;

            int cells = 0;
            for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
                for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
                    if (game.field[0].cell[row][col]) cells++;
            if (cells < 24) {
                printf("FAIL %s:%d: semilla %u termino en el frame %d con solo "
                        "%d celdas ocupadas\n", __FILE__, __LINE__, seed, frame, cells);
                g_failures++;
                return;
            }
            break;
        }
    }
}

static void test_two_linked_machines_stay_identical(void) {
    /* THE point of lockstep: both consoles simulate both players, so after
     * any number of frames their game state must match byte for byte. If
     * this ever fails, a linked game silently drifts into two different
     * games — which is why it is tested here and not left to a cable. */
    TengenLink master, slave;
    tengen_link_start(&master, 0x1234, 3, TENGEN_PLAYER_1, false, false);
    tengen_link_start(&slave, 0x1234, 3, TENGEN_PLAYER_2, false, false);

    CHECK(master.game.two_player && !master.game.coop);
    CHECK(memcmp(&master.game, &slave.game, sizeof(master.game)) == 0);

    for (int frame = 0; frame < 3000; frame++) {
        uint8_t p1 = scripted_buttons(0, frame);
        uint8_t p2 = scripted_buttons(1, frame);

        /* Each machine sends its own player's buttons and receives the
         * other's; neither ever sees the other's game state. */
        uint16_t from_master = tengen_link_send_word(&master, p1);
        uint16_t from_slave = tengen_link_send_word(&slave, p2);

        CHECK(tengen_link_step(&master, p1, from_slave, 0));
        CHECK(tengen_link_step(&slave, p2, from_master, 0));

        if (memcmp(&master.game, &slave.game, sizeof(master.game)) != 0) {
            printf("FAIL %s:%d: las dos maquinas divergieron en el frame %d\n",
                    __FILE__, __LINE__, frame);
            g_failures++;
            return;
        }
    }

    /* And the scripted play must have actually done something, or the
     * comparison above proved nothing. */
    CHECK(master.game.player[0].piece_stats[TT_I] == 0); /* stats are 1P only */
    CHECK(master.game.player[0].score > 0);
    CHECK(master.game.player[1].score > 0);
    CHECK(master.game.player[0].score != master.game.player[1].score);
}

static void test_a_lost_transfer_stops_the_link_rather_than_drifting(void) {
    TengenLink master;
    tengen_link_start(&master, 7, 0, TENGEN_PLAYER_1, false, false);

    /* A word from the right frame is accepted... */
    CHECK(tengen_link_step(&master, 0, tengen_link_pack(0, 0), 0));
    CHECK(!master.desynced);

    /* ...one from the wrong frame is not, and ends the session. */
    CHECK(!tengen_link_step(&master, 0, tengen_link_pack(0, 47), 0));
    CHECK(master.desynced);
    /* And it stays ended, even if the next word looks fine. */
    CHECK(!tengen_link_step(&master, 0, tengen_link_pack(0, 1), 0));
}

static void test_the_wire_word_survives_a_round_trip(void) {
    for (int buttons = 0; buttons < 256; buttons++) {
        for (int frame = 0; frame < 256; frame++) {
            uint16_t word = tengen_link_pack((uint8_t)buttons, (uint8_t)frame);
            CHECK(tengen_link_buttons(word) == (uint8_t)buttons);
            CHECK(tengen_link_frame(word) == (frame & TENGEN_LINK_FRAME_MASK));
            /* $FFFF is what a GBA reads for a console that is not on the
             * cable, so no real word may ever look like one. */
            CHECK(word != 0xFFFF);
        }
    }
}

/* Runs one transfer between two lobbies, the way the cable does: each side
 * puts a word up, the hardware hands both words to both sides. `carries` is
 * false for a transfer that did not happen at all. Returns after both have
 * seen it. */
static void lobby_transfer(TengenLobby *master, TengenLobby *slave,
                            bool carries) {
    uint16_t mw = tengen_lobby_word(master, true);
    uint16_t sw = tengen_lobby_word(slave, false);
    tengen_lobby_apply(master, true, carries, mw, sw);
    tengen_lobby_apply(slave, false, carries, mw, sw);
}

/* THE MASTER PICKS AFTER CONNECTING, NOT BEFORE.
 *
 * On a cable only one of the two players should be choosing the level and the
 * tune, and neither console knows which one that is until the cable has told
 * them. So the lobby connects first and parks: the master keeps saying HELLO
 * until tengen_lobby_release, and only then does the handshake run on.
 *
 * The parking has to be free. The handshake is stop-and-wait, so the slave
 * keeps echoing HELLO, every transfer succeeds, and neither end's give-up
 * counter moves — which this checks by holding for longer than the timeout
 * and then completing anyway. */
static void test_the_button_that_starts_a_match_does_not_pause_it(void) {
    /* Over the cable the buttons travel as raw LEVELS, a transfer behind, so
     * a START still down when the match begins arrives at the core as a fresh
     * press and pauses it on the spot. Seeding held_last_frame with
     * everything-held is what stops that; this pins it. */
    TengenLink link;
    tengen_link_start(&link, 0xACE1, 0, TENGEN_PLAYER_1, false, false);
    for (int i = 0; i < 2; i++) link.game.player[i].held_last_frame = 0xFF;

    uint16_t remote = tengen_link_pack(TENGEN_BTN_START, 0);
    for (int f = 0; f < 4; f++) {
        uint16_t local = tengen_link_pack(TENGEN_BTN_START,
                                           (uint8_t)(f & TENGEN_LINK_FRAME_MASK));
        remote = tengen_link_pack(TENGEN_BTN_START,
                                   (uint8_t)(f & TENGEN_LINK_FRAME_MASK));
        CHECK(tengen_link_step(&link, tengen_link_buttons(local), remote, NULL));
        CHECK(!link.game.paused);
    }
    /* ...and once it HAS been let go, Start still pauses, or the fix would
     * have broken the thing it was protecting. */
    for (int f = 4; f < 6; f++) {
        uint16_t w = tengen_link_pack(0, (uint8_t)(f & TENGEN_LINK_FRAME_MASK));
        CHECK(tengen_link_step(&link, tengen_link_buttons(w), w, NULL));
    }
    uint16_t press = tengen_link_pack(TENGEN_BTN_START,
                                       (uint8_t)(6 & TENGEN_LINK_FRAME_MASK));
    CHECK(tengen_link_step(&link, tengen_link_buttons(press), press, NULL));
    CHECK(link.game.paused);
}

static void test_the_lobby_connects_first_and_the_master_chooses_after(void) {
    TengenLobby master, slave;
    tengen_lobby_start_held(&master, 0x0000);
    tengen_lobby_start(&slave, 0x1111, 1, 0);

    for (int i = 0; i < 4; i++) lobby_transfer(&master, &slave, true);
    CHECK(master.linked);   /* the master should know the other end answered */
    CHECK(slave.linked);   /* and the slave should know it heard one */
    CHECK(!master.ready);   /* but nobody starts until the master has chosen */
    CHECK(!slave.ready);   /* the slave least of all */

    /* Held for twice the give-up window, and neither end gives up. */
    for (int i = 0; i < TENGEN_LOBBY_TIMEOUT * 2; i++)
        lobby_transfer(&master, &slave, true);
    CHECK(!master.failed && !slave.failed);   /* parking must not look like silence */
    CHECK(!master.ready && !slave.ready);   /* and must not start the match either */

    const uint8_t handicap[2] = { 1, 4 };
    tengen_lobby_release(&master, 0xBEEF, 7, 2, handicap, false, false);
    for (int i = 0; i < 64 && !(master.ready && slave.ready); i++)
        lobby_transfer(&master, &slave, true);

    CHECK(master.ready && slave.ready);   /* released, the handshake should finish */
    CHECK(slave.seed == 0xBEEF);   /* the slave takes the master's seed */
    CHECK(slave.start_level == 7);   /* ...and its level */
    CHECK(slave.music == 2);   /* ...and its tune */
    /* ...and how buried each of them starts, which needs all three bits of
     * each value: a handicap of 4 packed into two would come back as 0. */
    CHECK(slave.handicap[0] == 1);
    CHECK(slave.handicap[1] == 4);
}

static void test_the_lobby_agrees_on_a_game_and_both_leave_together(void) {
    TengenLobby master, slave;
    /* The two consoles arrive with different ideas of everything — which is
     * the point: the slave's own choices must lose. */
    tengen_lobby_start(&master, 0xBEEF, 7, 2);
    tengen_lobby_start(&slave, 0x1111, 1, 0);

    int transfers = 0;
    int left_apart = 0;
    while (!master.ready && !master.failed && transfers < 100) {
        lobby_transfer(&master, &slave, true);
        transfers++;
        /* NEITHER may enter the match a transfer before the other: one
         * console still in the lobby would send a tagged word that the other,
         * already playing, would read as a frame of buttons. */
        if (master.ready != slave.ready) left_apart++;
    }

    CHECK(master.ready && slave.ready);
    CHECK(left_apart == 0);
    CHECK(slave.seed == 0xBEEF);
    CHECK(slave.start_level == 7);
    CHECK(slave.music == 2);
    /* Six stages, two transfers each, and no more: a handshake that quietly
     * took twice as long as it should would still pass every check above. */
    CHECK(transfers == 12);
}

static void test_the_lobby_survives_transfers_that_do_not_arrive(void) {
    TengenLobby master, slave;
    tengen_lobby_start(&master, 0xC0DE, 4, 3);
    tengen_lobby_start(&slave, 0, 0, 0);

    /* Every third attempt is lost. Stop-and-wait means a lost transfer costs
     * a repeat, never a skipped stage — so the two still agree at the end. */
    for (int i = 0; i < 100 && !master.ready; i++)
        lobby_transfer(&master, &slave, (i % 3) != 0);

    CHECK(master.ready && slave.ready);
    CHECK(slave.seed == 0xC0DE);
    CHECK(slave.start_level == 4);
    CHECK(slave.music == 3);
}

static void test_a_lobby_with_nothing_on_the_other_end_gives_up(void) {
    TengenLobby master;
    tengen_lobby_start(&master, 1, 0, 0);
    for (int i = 0; i < TENGEN_LOBBY_TIMEOUT - 1; i++)
        tengen_lobby_apply(&master, true, false, 0, 0);
    CHECK(!master.failed);        /* ten seconds is ten seconds */
    tengen_lobby_apply(&master, true, false, 0, 0);
    CHECK(master.failed);
    CHECK(!master.ready);
}

static void test_no_lobby_word_can_look_like_an_absent_console(void) {
    /* $FFFF is what a GBA reads from the slot of a console that is not there,
     * so the handshake's words have to stay clear of it too. */
    TengenLobby lobby;
    tengen_lobby_start(&lobby, 0xFFFF, 0x0F, 0x0F);
    for (int stage = TENGEN_LOBBY_NONE; stage <= TENGEN_LOBBY_GO; stage++) {
        lobby.stage = (uint8_t)stage;
        lobby.echo = (uint8_t)stage;
        CHECK(tengen_lobby_word(&lobby, true) != 0xFFFF);
        CHECK(tengen_lobby_word(&lobby, false) != 0xFFFF);
    }
}

/* The handshake and the match are one continuous conversation over one cable,
 * so the seam between them is where a design that reads well can still fall
 * over. This plays both halves end to end. */
static void test_a_lobby_hands_straight_over_to_a_matching_pair_of_games(void) {
    TengenLobby lobby_m, lobby_s;
    tengen_lobby_start(&lobby_m, 0x51A7, 5, 1);
    tengen_lobby_start(&lobby_s, 0, 0, 0);
    for (int i = 0; i < 100 && !lobby_m.ready; i++)
        lobby_transfer(&lobby_m, &lobby_s, true);
    CHECK(lobby_m.ready && lobby_s.ready);

    TengenLink master, slave;
    tengen_link_start(&master, lobby_m.seed, lobby_m.start_level, TENGEN_PLAYER_1, false, false);
    tengen_link_start(&slave, lobby_s.seed, lobby_s.start_level, TENGEN_PLAYER_2, false, false);
    CHECK(memcmp(&master.game, &slave.game, sizeof(master.game)) == 0);

    for (int frame = 0; frame < 500; frame++) {
        uint8_t p1 = scripted_buttons(0, frame);
        uint8_t p2 = scripted_buttons(1, frame);
        uint16_t from_master = tengen_link_send_word(&master, p1);
        uint16_t from_slave = tengen_link_send_word(&slave, p2);
        CHECK(tengen_link_step(&master, p1, from_slave, 0));
        CHECK(tengen_link_step(&slave, p2, from_master, 0));
    }
    CHECK(memcmp(&master.game, &slave.game, sizeof(master.game)) == 0);
    /* And the game they are both playing is the MASTER's: its seed and its
     * start level, not the zeroes the slave walked in with. */
    CHECK(lobby_s.seed == 0x51A7 && lobby_s.start_level == 5);
    CHECK(master.game.player[0].level == 5);
    CHECK(master.game.player[0].piece.current != TT_NONE);
    CHECK(master.game.player[1].piece.current != TT_NONE);
}

/* ----------------------------------------------------------------------- *
 * The COMPUTER player
 * ----------------------------------------------------------------------- */

static void test_the_computers_piece_table_derives_from_the_bitmaps(void) {
    /* computerMoveSelectTableOffsetBy18's profile bytes are each column's
     * bottom RELATIVE TO THE PIECE'S LEFTMOST OCCUPIED COLUMN, times eight.
     * That reading is what makes the table derivable rather than transcribed,
     * so this pins the shape of what tengen_ai_profile produces: as many
     * bytes as the piece is wide, minus one, and the ROM's own values for the
     * four entries worked out by hand in reference/NOTES.md. */
    uint8_t p[3];

    /* The T flat side up is 1110 / 0100: bottoms [0,1,0], so from column zero
     * [+8, 0] — and the ROM says 08 00. */
    CHECK(tengen_ai_profile(TT_T, 0, p) == 2);
    CHECK(p[0] == 0x08 && p[1] == 0x00);
    /* The L is 1110 / 1000: bottoms [1,0,0], from column zero [-8,-8]. */
    CHECK(tengen_ai_profile(TT_L, 0, p) == 2);
    CHECK(p[0] == 0xF8 && p[1] == 0xF8);
    /* The I lying flat is four wide and level. */
    CHECK(tengen_ai_profile(TT_I, 0, p) == 3);
    CHECK(p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00);
    /* ...and standing on end is one column, so it has no profile at all —
     * which is the $80 that terminates its entry immediately. */
    CHECK(tengen_ai_profile(TT_I, 1, p) == 0);
    CHECK(tengen_ai_profile(TT_I, 3, p) == 0);

    /* Every entry's length is the piece's width minus one, for all 28. */
    for (int piece = TT_I; piece < TENGEN_TETROMINO_COUNT; piece++) {
        for (uint8_t o = 0; o < 4; o++) {
            int width = 0, seen = 0;
            for (int c = 0; c < 4; c++) {
                int any = 0;
                for (int r = 0; r < 4; r++)
                    if (tengen_piece_occupies((TengenTetromino)piece, o, r, c)) any = 1;
                if (any) { width++; seen = 1; }
                else if (seen) { /* pieces are contiguous; nothing to do */ }
            }
            CHECK(tengen_ai_profile((TengenTetromino)piece, o, p) == width - 1);
        }
    }

    /* And the bonuses are the cartridge's, signed. */
    CHECK(tengen_ai_bonus(TT_I, 1) == -8);
    CHECK(tengen_ai_bonus(TT_I, 3) == -10);
    CHECK(tengen_ai_bonus(TT_T, 0) == 9);
    CHECK(tengen_ai_bonus(TT_L, 3) == 15);
}

static void test_the_computer_reads_the_board_in_the_roms_own_units(void) {
    /* computerMove's height scan steps down in eights from $28, so its first
     * read is $30 — ROM row 6, the first visible one — and everything it does
     * downstream is in those byte units. */
    TengenGame game;
    uint8_t h[TENGEN_AI_SCRATCH_A];

    tengen_new_game(&game, 1234, 0, false, false, false);
    tengen_ai_heights(&game, TENGEN_PLAYER_1, h);

    /* An empty column runs down onto the floor the ROM lays at row 26. */
    for (int col = 1; col <= 10; col++)
        CHECK(h[col + TENGEN_ROM_COL_ORIGIN] == 26 * 8);
    /* The walls, and the padding either side of them, are solid from the top. */
    CHECK(h[2] == 0x30);
    CHECK(h[13] == 0x30);
    CHECK(h[0] == 0x30 && h[1] == 0x30);
    CHECK(h[14] == 0x30 && h[15] == 0x30);

    /* Drop one block into the middle and the height follows it: visible row
     * 15 is ROM row 21. */
    game.field[0].cell[15][5] = TT_O;
    tengen_ai_heights(&game, TENGEN_PLAYER_1, h);
    CHECK(h[5 + TENGEN_ROM_COL_ORIGIN] == 21 * 8);

    /* In coop the wall columns are playable, so they read as empty. */
    TengenGame coop;
    tengen_new_game(&coop, 1234, 0, true, true, false);
    tengen_ai_heights(&coop, TENGEN_PLAYER_2, h);
    CHECK(h[2] == 26 * 8);
    CHECK(h[13] == 26 * 8);
    CHECK(h[1] == 0x30 && h[14] == 0x30);   /* ...but the padding is not */
}

static void test_the_computer_picks_a_placement_and_walks_to_it(void) {
    /* The whole thing end to end: choose, then drive. What is checked is not
     * WHICH square it likes — that is the cartridge's taste and the tables
     * above are what pin it — but that the choice is reachable and that the
     * driver actually converges on it at the ROM's own cadence: a shift every
     * eighth frame, a rotation every sixteenth (main.asm.txt:4170). */
    TengenGame game;
    TengenAi ai;

    tengen_new_game(&game, 0x2468, 0, false, false, false);
    tengen_ai_reset(&ai);
    tengen_ai_choose(&ai, &game, TENGEN_PLAYER_1);

    CHECK(ai.target_orientation < 4);
    /* A target inside the board: the scan runs nibble columns 2..13, and the
     * I standing on end takes one back. */
    CHECK(ai.target_x >= 1 && ai.target_x <= 13);

    /* Drive it. Every eighth frame it may shift and every sixteenth rotate,
     * so a couple of hundred frames is far more than enough to arrive. */
    uint8_t frame = 0;
    for (int i = 0; i < 400; i++) {
        uint8_t buttons = tengen_ai_buttons(&ai, &game, TENGEN_PLAYER_1, frame);
        /* Nothing but the four it is allowed to press. */
        CHECK((buttons & ~(uint8_t)(TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT |
                                     TENGEN_BTN_A | TENGEN_BTN_B)) == 0);
        /* It only ever asks on the frames the ROM asks on. */
        if (frame & 0x07) CHECK((buttons & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) == 0);
        if (frame & 0x0F) CHECK((buttons & (TENGEN_BTN_A | TENGEN_BTN_B)) == 0);
        tengen_step(&game, TENGEN_PLAYER_1, buttons);
        frame++;
        if (game.player[0].piece.orientation == ai.target_orientation &&
            (uint8_t)game.player[0].piece.x == ai.target_x)
            break;
    }
    CHECK(game.player[0].piece.orientation == ai.target_orientation);
    CHECK((uint8_t)game.player[0].piece.x == ai.target_x);
}

static void test_the_computer_keeps_playing_and_does_not_bury_itself(void) {
    /* The real test of a placement chooser is a long game, so this plays
     * three out — re-choosing on every new piece the way getNextTetromino
     * does — and asks that each one lasts a good many pieces and that lines
     * get cleared along the way. A chooser that stacks blindly tops out in a
     * handful of pieces and never clears anything.
     *
     * It is not asked to be GOOD. The cartridge's computer is not: it never
     * soft-drops (its driver presses nothing but left, right, A and B), so
     * every piece takes the whole of gravity to land, and it buries itself
     * eventually. Around forty to ninety pieces and a few lines is what this
     * code does, and what the cartridge's does. */
    int total_lines = 0;

    for (int run = 0; run < 3; run++) {
        TengenGame game;
        TengenAi ai;
        TengenTetromino last = TT_NONE;
        uint8_t frame = 0;
        int pieces = 0;

        tengen_new_game(&game, (uint16_t)(0x1357 + run * 777), 0, false, false, false);
        tengen_ai_reset(&ai);

        for (int i = 0; i < 60000 && game.player[0].game_active; i++) {
            if (game.player[0].piece.current != last) {
                last = game.player[0].piece.current;
                pieces++;
                tengen_ai_choose(&ai, &game, TENGEN_PLAYER_1);
            }
            uint8_t buttons = tengen_ai_buttons(&ai, &game, TENGEN_PLAYER_1, frame);
            TengenStepResult r = tengen_step(&game, TENGEN_PLAYER_1, buttons);
            total_lines += r.lines_collapsed;
            frame++;
        }
        CHECK(pieces >= 20);
    }
    CHECK(total_lines > 0);
}

static void test_a_coop_line_clear_holds_both_players(void) {
    /* activeGamePlay (main.asm.txt:82DC-82E7) reads its own line-clear timer
     * and then, on the negative playMode that is coop, the OR of the two. One
     * board means one pause: the partner cannot keep dropping into rows that
     * are already coming down. */
    TengenGame game;
    tengen_new_game(&game, 0x4242, 0, true, true, false);

    /* Give player 1 a row to clear: everything but the column its piece is
     * not over, so the lock completes it. */
    int row = TENGEN_PF_HEIGHT - 1;
    for (int col = 0; col < TENGEN_PF_WIDTH; col++)
        game.field[0].cell[row][col] = TT_I;
    game.player[TENGEN_PLAYER_1].clearing_rows = 1u << row;
    game.player[TENGEN_PLAYER_1].line_clear_timer = 20;

    int8_t was_y = game.player[TENGEN_PLAYER_2].piece.y;
    int8_t was_x = game.player[TENGEN_PLAYER_2].piece.x;
    for (int frame = 0; frame < 10; frame++)
        tengen_step(&game, TENGEN_PLAYER_2, TENGEN_BTN_DOWN | TENGEN_BTN_LEFT);
    CHECK(game.player[TENGEN_PLAYER_2].piece.y == was_y);
    CHECK(game.player[TENGEN_PLAYER_2].piece.x == was_x);
    /* ...and the timer it is waiting on is not its own to spend. */
    CHECK(game.player[TENGEN_PLAYER_1].line_clear_timer == 20);

    /* A RACE IS TWO BOARDS AND TWO CLOCKS: the same timer on the rival holds
     * nobody here. */
    TengenGame race;
    tengen_new_game(&race, 0x4242, 0, true, false, false);
    race.player[TENGEN_PLAYER_1].line_clear_timer = 20;
    was_y = race.player[TENGEN_PLAYER_2].piece.y;
    for (int frame = 0; frame < 60; frame++)
        tengen_step(&race, TENGEN_PLAYER_2, 0);
    CHECK(race.player[TENGEN_PLAYER_2].piece.y != was_y);
}

static void test_the_computers_soft_drop_does_not_eat_its_own_shifts(void) {
    /* THE ONE THE CORE'S OWN QUIRK BREAKS. main.asm.txt:98-107 discards a
     * FRESH Left or Right outright if Down was held on the PREVIOUS frame,
     * and the driver shifts on frames where the counter is a multiple of
     * eight. A computer that holds Down through frame seven therefore has
     * most of its shifts thrown away before they reach the board: it drops
     * pieces down the column they spawned in, buries itself, and looks from
     * the outside exactly like a computer that is not playing.
     *
     * So this counts what actually happens rather than the outcome: every
     * frame the driver asks for a shift, did the piece move? */
    TengenGame game;
    TengenAi ai;
    tengen_new_game(&game, 0x31337, 0, true, false, false);
    tengen_ai_reset(&ai);
    ai.soft_drop = true;

    int asked = 0, landed = 0;
    TengenTetromino last = TT_NONE;
    for (int frame = 0; frame < 4000; frame++) {
        TengenPlayerState *p = &game.player[TENGEN_PLAYER_2];
        if (!p->game_active) break;
        if (p->piece.current != last) {
            last = p->piece.current;
            tengen_ai_choose(&ai, &game, TENGEN_PLAYER_2);
        }
        uint8_t buttons = tengen_ai_buttons(&ai, &game, TENGEN_PLAYER_2,
                                             (uint8_t)frame);
        int8_t was_x = p->piece.x;
        TengenTetromino was = p->piece.current;
        tengen_step(&game, TENGEN_PLAYER_2, buttons);
        if (buttons & (TENGEN_BTN_LEFT | TENGEN_BTN_RIGHT)) {
            asked++;
            /* A shift that was refused by the WALL is not one the input
             * handler ate, and neither is a frame the piece locked on. */
            if (p->piece.current == was && p->piece.x != was_x) landed++;
        }
    }
    CHECK(asked > 20);
    /* Every shift the driver asks for should reach the board. Allow a few
     * for the ones a wall or the terrain legitimately refuses. */
    CHECK(landed * 10 >= asked * 8);
}

static void test_a_coop_top_out_ends_the_game_for_both_players(void) {
    /* main.asm.txt:83D4-83DD. The store that clears `player1GameActive,x` is
     * preceded by a `bit playMode / bpl`, and on the negative playMode — the
     * $FF that is COOPERATIVE and WITH COMPUTER — it clears BOTH flags first.
     * One board, one game to lose.
     *
     * Leaving the partner alive is what wedged coop in the front end: the
     * board was dead, one flag said the match was still on, and nothing
     * anywhere agreed it had finished. */
    TengenGame game;
    tengen_new_game(&game, 0x1234, 0, true, true, false);
    CHECK(game.coop);
    CHECK(game.player[0].game_active && game.player[1].game_active);

    /* Bury the shared board: solid but for one column, so no row can ever
     * complete and the next piece has nowhere to go. */
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 0; col < TENGEN_PF_WIDTH; col++)
            game.field[0].cell[row][col] = (col == 5) ? TT_NONE : TT_I;

    bool topped = false;
    for (int frame = 0; frame < 600 && !topped; frame++)
        topped = tengen_step(&game, TENGEN_PLAYER_1, 0).topped_out;
    CHECK(topped);
    CHECK(!game.player[0].game_active);
    CHECK(!game.player[1].game_active);

    /* And a race is NOT that: two boards, two games, and the one that is
     * still standing keeps playing. */
    TengenGame race;
    tengen_new_game(&race, 0x1234, 0, true, false, false);
    CHECK(!race.coop);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 1; col < TENGEN_PF_WIDTH - 1; col++)
            race.field[0].cell[row][col] = (col == 5) ? TT_NONE : TT_I;
    topped = false;
    for (int frame = 0; frame < 600 && !topped; frame++)
        topped = tengen_step(&race, TENGEN_PLAYER_1, 0).topped_out;
    CHECK(topped);
    CHECK(!race.player[0].game_active);
    CHECK(race.player[1].game_active);
}

static void test_the_computer_can_be_told_to_look_first_and_to_drop(void) {
    /* Two knobs the cartridge does not have, and both OFF by default so the
     * default is still the cartridge's player. See TengenAi. */
    TengenGame game;
    TengenAi ai;
    tengen_new_game(&game, 0x2222, 0, true, false, false);
    tengen_ai_reset(&ai);
    CHECK(ai.settle == 0 && !ai.soft_drop);

    /* `settle`: nothing at all comes out of the pad until it has elapsed,
     * counted from the choice — which is made once per spawn. */
    ai.settle = 30;
    tengen_ai_choose(&ai, &game, TENGEN_PLAYER_2);
    for (int frame = 0; frame < 30; frame++)
        CHECK(tengen_ai_buttons(&ai, &game, TENGEN_PLAYER_2,
                                 (uint8_t)frame) == 0);

    /* ...and afterwards it plays: over enough frames it must press something,
     * on the ROM's own eighth-frame cadence. */
    uint8_t seen = 0;
    for (int frame = 0; frame < 64; frame++)
        seen |= tengen_ai_buttons(&ai, &game, TENGEN_PLAYER_2, (uint8_t)frame);
    CHECK(seen != 0);

    /* `soft_drop`: DOWN once the piece is over its target in the orientation
     * it wants, and never while it is still aiming. */
    TengenAi drop;
    tengen_ai_reset(&drop);
    drop.soft_drop = true;
    tengen_ai_choose(&drop, &game, TENGEN_PLAYER_2);
    game.player[TENGEN_PLAYER_2].piece.x = (int8_t)drop.target_x;
    game.player[TENGEN_PLAYER_2].piece.orientation =
        (uint8_t)(drop.target_orientation & 3);
    CHECK(tengen_ai_buttons(&drop, &game, TENGEN_PLAYER_2, 0) == TENGEN_BTN_DOWN);

    game.player[TENGEN_PLAYER_2].piece.x = (int8_t)(drop.target_x + 2);
    uint8_t aiming = tengen_ai_buttons(&drop, &game, TENGEN_PLAYER_2, 0);
    CHECK(!(aiming & TENGEN_BTN_DOWN));

    /* The ROM's player, asked the same question, keeps its hands off it. */
    TengenAi rom;
    tengen_ai_reset(&rom);
    tengen_ai_choose(&rom, &game, TENGEN_PLAYER_2);
    game.player[TENGEN_PLAYER_2].piece.x = (int8_t)rom.target_x;
    game.player[TENGEN_PLAYER_2].piece.orientation =
        (uint8_t)(rom.target_orientation & 3);
    CHECK(tengen_ai_buttons(&rom, &game, TENGEN_PLAYER_2, 0) == 0);
}

static void test_coop_is_one_twelve_wide_board_over_the_cable(void) {
    /* COOPERATIVE is the third mode the cartridge offers and the only one
     * where the two players share a field: initPlayer1orCoopPlayfield leaves
     * the wall nibbles open (main.asm.txt:3480-3489) and both play into
     * field[0]. Over a cable that is the same lockstep as a race — both
     * consoles simulate both players — so what this checks is that the
     * LOBBY carries the choice and that the shared board stays shared.
     */
    TengenLobby lobby_m, lobby_s;
    const uint8_t handicap[2] = { 0, 0 };
    tengen_lobby_start_held(&lobby_m, 0x0C0F);
    tengen_lobby_start(&lobby_s, 0, 0, 0);
    for (int i = 0; i < 40 && !lobby_m.linked; i++)
        lobby_transfer(&lobby_m, &lobby_s, true);
    tengen_lobby_release(&lobby_m, 0x0C0F, 2, 1, handicap, true, false);
    for (int i = 0; i < 100 && !(lobby_m.ready && lobby_s.ready); i++)
        lobby_transfer(&lobby_m, &lobby_s, true);
    CHECK(lobby_m.ready && lobby_s.ready);
    /* The flag rides bit 8 of the CONFIG payload and has to arrive. */
    CHECK(lobby_s.coop);

    TengenLink master, slave;
    tengen_link_start(&master, lobby_m.seed, lobby_m.start_level,
                       TENGEN_PLAYER_1, lobby_m.coop, lobby_m.xe);
    tengen_link_start(&slave, lobby_s.seed, lobby_s.start_level,
                       TENGEN_PLAYER_2, lobby_s.coop, lobby_s.xe);
    CHECK(master.game.coop && slave.game.coop);
    CHECK(memcmp(&master.game, &slave.game, sizeof(master.game)) == 0);

    /* Twelve playable columns, because the sentinels are gone. */
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        CHECK(master.game.field[0].cell[row][0] == TT_NONE);
        CHECK(master.game.field[0].cell[row][TENGEN_PF_WIDTH - 1] == TT_NONE);
    }
    /* ...and they start on opposite sides of it. */
    CHECK(master.game.player[0].piece.x == TENGEN_SPAWN_X[0]);
    CHECK(master.game.player[1].piece.x == TENGEN_SPAWN_X[1]);

    for (int frame = 0; frame < 600; frame++) {
        uint8_t p1 = scripted_buttons(0, frame);
        uint8_t p2 = scripted_buttons(1, frame);
        uint16_t from_master = tengen_link_send_word(&master, p1);
        uint16_t from_slave = tengen_link_send_word(&slave, p2);
        CHECK(tengen_link_step(&master, p1, from_slave, 0));
        CHECK(tengen_link_step(&slave, p2, from_master, 0));
    }
    CHECK(memcmp(&master.game, &slave.game, sizeof(master.game)) == 0);

    /* Both players' pieces landed in the SAME field, which is the whole of
     * what coop is: field[1] is never touched. */
    int settled = 0;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
            if (master.game.field[0].cell[row][col] != TT_NONE) settled++;
            CHECK(master.game.field[1].cell[row][col] == TT_NONE);
        }
    CHECK(settled > 0);
    /* And one level between them: tengen_step copies it across in coop. */
    CHECK(master.game.player[0].level == master.game.player[1].level);
}

static void test_either_player_can_pause_a_linked_game(void) {
    /* pauseOrUnpause ORs both controllers (main.asm.txt:7196-7198), so this
     * has to hold over the cable too. */
    TengenLink link;
    tengen_link_start(&link, 11, 0, TENGEN_PLAYER_1, false, false);
    CHECK(!link.game.paused);

    /* Player 2, the remote one, presses Start. */
    CHECK(tengen_link_step(&link, 0, tengen_link_pack(TENGEN_BTN_START, 0), 0));
    CHECK(link.game.paused);

    int8_t y_before = link.game.player[0].piece.y;
    for (int i = 0; i < 200; i++)
        tengen_link_step(&link, 0, tengen_link_pack(0, link.frame), 0);
    CHECK(link.game.player[0].piece.y == y_before);
}

static void test_level_starts_at_the_chosen_start_level(void) {
    TengenGame game;
    tengen_new_game(&game, 31, 9, false, false, false);
    CHECK(game.player[0].level == 9);
    CHECK(game.player[0].start_level == 9);
    /* And gravity should immediately reflect it, not level 0's 33 frames. */
    CHECK(tengen_frames_per_row(game.player[0].level, 0, false, false) == 6);
}

static void test_level_is_recomputed_from_the_line_total(void) {
    /* The ROM recomputes level as start_level + thresholds_passed on every
     * clear rather than incrementing (main.asm.txt:3140-3186), so a start
     * level offsets the whole curve. */
    TengenGame game;
    tengen_new_game(&game, 33, 5, false, false, false);
    clear_lines_until(&game, 29);
    CHECK(game.player[0].lines >= 29); /* the helper must actually have cleared lines */
    /* Twenty-nine lines is still short of the first threshold. A port that
     * read the table as tens-and-ones was nine levels up by here. */
    CHECK(game.player[0].level == 5);

    clear_lines_until(&game, 30);
    CHECK(game.player[0].lines >= 30);
    CHECK(game.player[0].level == 6);
}

static void test_the_dancers_cast_grows_with_triples_and_tetrises(void) {
    /* L8D8B (main.asm.txt:2050-2082): one, plus one per triple and two per
     * tetris since the last level-up, capped at six in 1P. Singles and
     * doubles buy nothing at all. */
    TengenGame game;
    tengen_new_game(&game, 11, 0, false, false, false);
    CHECK(tengen_dancer_count(&game) == 1);

    game.player[0].clear_counts[0] = 9;   /* nine singles... */
    game.player[0].clear_counts[1] = 9;   /* ...and nine doubles */
    CHECK(tengen_dancer_count(&game) == 1);

    game.player[0].clear_counts[2] = 2;   /* two triples: +2 */
    CHECK(tengen_dancer_count(&game) == 3);

    game.player[0].clear_counts[3] = 1;   /* one tetris: +2 */
    CHECK(tengen_dancer_count(&game) == 5);

    game.player[0].clear_counts[3] = 4;   /* the 1P cap is six, not eight */
    CHECK(tengen_dancer_count(&game) == 6);

    /* Coop is the mode with a second column of positions, so it uses all
     * eight. Both players' tallies count towards it. */
    TengenGame co;
    tengen_new_game(&co, 11, 0, true, true, false);
    co.player[0].clear_counts[3] = 2;
    co.player[1].clear_counts[2] = 3;
    CHECK(tengen_dancer_count(&co) == 8);

    /* And the tally is this level's, not the game's. */
    tengen_clear_bonus_counts(&game);
    CHECK(tengen_dancer_count(&game) == 1);
}

static void test_a_clear_is_tallied_by_how_many_rows_it_took(void) {
    TengenGame game;
    tengen_new_game(&game, 33, 0, false, false, false);
    clear_lines_until(&game, 1);
    CHECK(game.player[0].lines >= 1);
    /* The helper clears one row at a time, so every clear is a single. */
    CHECK(game.player[0].clear_counts[0] >= 1);
    CHECK(game.player[0].clear_counts[1] == 0);
    CHECK(game.player[0].clear_counts[2] == 0);
    CHECK(game.player[0].clear_counts[3] == 0);
    CHECK(tengen_dancer_count(&game) == 1);
}

static int handicap_count(const TengenPlayfield *f, int row) {
    int n = 0;
    for (int c = 1; c <= 10; c++) if (f->cell[row][c] != TT_NONE) n++;
    return n;
}

static void test_the_handicap_buries_three_rows_a_step(void) {
    /* garbageHeightData is $B8,$A0,$88,$70 against a field that ends at $D0
     * with eight bytes to a row (main.asm.txt:3597), so 3, 6, 9 and 12 rows.
     * Nothing above the garbage is touched. */
    for (uint8_t h = 1; h <= TENGEN_HANDICAP_MAX; h++) {
        TengenGame game;
        tengen_new_game(&game, 0x1234, 0, false, false, false);
        tengen_apply_handicap(&game, TENGEN_PLAYER_1, h);
        int rows = h * TENGEN_HANDICAP_ROWS_PER_STEP;
        for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
            int n = handicap_count(&game.field[0], row);
            if (row < TENGEN_PF_HEIGHT - rows) CHECK(n == 0);
            else CHECK(n > 0);
        }
    }
    /* And a handicap of zero is what the ROM's `bne` skips over. */
    TengenGame none;
    tengen_new_game(&none, 0x1234, 0, false, false, false);
    tengen_apply_handicap(&none, TENGEN_PLAYER_1, 0);
    CHECK(handicap_count(&none.field[0], TENGEN_PF_HEIGHT - 1) == 0);
}

static void test_every_handicap_row_has_a_way_through(void) {
    /* A row that comes out seven or more full gets one hole punched into
     * bytes 2-5 (main.asm.txt:3676-3693). Between that and the one-in-eight
     * skip, no row can arrive complete — a complete row would clear itself the
     * instant the game started. Fifty seeds' worth of rows say so. */
    int full_rows = 0, rows_seen = 0, holes = 0;
    for (uint16_t seed = 1; seed <= 50; seed++) {
        TengenGame game;
        tengen_new_game(&game, (uint16_t)(seed * 977), 0, false, false, false);
        tengen_apply_handicap(&game, TENGEN_PLAYER_1, TENGEN_HANDICAP_MAX);
        for (int row = TENGEN_PF_HEIGHT - 12; row < TENGEN_PF_HEIGHT; row++) {
            int n = handicap_count(&game.field[0], row);
            rows_seen++;
            holes += 10 - n;
            if (n == 10) full_rows++;
        }
        CHECK(tengen_find_full_rows(&game.field[0]) == 0);
    }
    CHECK(full_rows == 0);
    /* ...and it is garbage, not a wall: roughly an eighth of the cells are
     * holes, plus the punched ones. */
    CHECK(holes > rows_seen);
}

static void test_the_handicap_is_the_same_from_the_same_seed(void) {
    /* Two consoles on a cable build their own boards from one seed, so this
     * is not a nicety. */
    TengenGame a, b;
    tengen_new_game(&a, 0xBEEF, 0, true, false, false);
    tengen_new_game(&b, 0xBEEF, 0, true, false, false);
    tengen_apply_handicap(&a, TENGEN_PLAYER_2, 3);
    tengen_apply_handicap(&b, TENGEN_PLAYER_2, 3);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int c = 0; c < TENGEN_PF_WIDTH; c++)
            CHECK(a.field[1].cell[row][c] == b.field[1].cell[row][c]);
    /* ...and it went to the player it was asked for. */
    CHECK(handicap_count(&a.field[0], TENGEN_PF_HEIGHT - 1) == 0);
    CHECK(handicap_count(&a.field[1], TENGEN_PF_HEIGHT - 1) > 0);
}

static void test_coop_garbage_fills_the_two_extra_columns(void) {
    /* The ROM draws a byte at a time and only fills EMPTY nibbles, so coop —
     * whose wall nibbles are $00 — gets twelve cells a row where 1P gets ten
     * (main.asm.txt:3602-3629). */
    TengenGame game;
    tengen_new_game(&game, 0x0F0F, 0, true, true, false);
    tengen_apply_handicap(&game, TENGEN_PLAYER_1, 2);
    int edge = 0;
    for (int row = TENGEN_PF_HEIGHT - 6; row < TENGEN_PF_HEIGHT; row++) {
        if (game.field[0].cell[row][0] != TT_NONE) edge++;
        if (game.field[0].cell[row][TENGEN_PF_WIDTH - 1] != TT_NONE) edge++;
    }
    CHECK(edge > 0);
}

/* Puts both coop pieces where the caller says, with nothing settled under
 * them, so a test can talk about the two falling pieces and only those. */
static void place_coop_pair(TengenGame *game,
                             TengenTetromino a, uint8_t ao, int ax, int ay,
                             TengenTetromino b, uint8_t bo, int bx, int by) {
    tengen_new_game(game, 0x5EED, 0, true, true, false);
    game->player[0].piece.current = a;
    game->player[0].piece.orientation = ao;
    game->player[0].piece.x = (int8_t)ax;
    game->player[0].piece.y = (int8_t)ay;
    game->player[1].piece.current = b;
    game->player[1].piece.orientation = bo;
    game->player[1].piece.x = (int8_t)bx;
    game->player[1].piece.y = (int8_t)by;
}

static void test_the_two_coop_pieces_are_solid_to_each_other(void) {
    /* checkCoopCollision (main.asm.txt:1827-1924). The playfield buffer holds
     * only settled blocks, so without this routine two players on one board
     * walk through each other — which is what they did here.
     *
     * Two O pieces. The O's bitmap is $CC,$00: the top-left 2x2 of the 4x4
     * box, so two of them overlap while they are within one row and one
     * column of each other and not beyond. */
    TengenGame game;
    place_coop_pair(&game, TT_O, 0, 5, 10, TT_O, 0, 5, 10);
    CHECK(tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_1));
    CHECK(tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_2));

    /* One column apart: the right half of one meets the left half of the
     * other. Two columns apart: nothing does. */
    game.player[0].piece.x = 6;
    CHECK(tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_1));
    game.player[0].piece.x = 7;
    CHECK(!tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_1));

    /* And the same vertically, which is the shift by four rather than one. */
    game.player[0].piece.x = 5;
    game.player[0].piece.y = 11;
    CHECK(tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_1));
    game.player[0].piece.y = 12;
    CHECK(!tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_1));

    /* THE MASK EARNS ITS KEEP HERE. Shifting a 4x4 bitmap sideways wraps bits
     * out of one row into the next, so without @coopCollisionTable2 an O two
     * columns to the right would appear to touch the far side of the row
     * above. Checked with the two lined up on the SAME row, three apart —
     * which is the widest the routine even looks. */
    game.player[0].piece.y = 10;
    game.player[0].piece.x = 8;
    CHECK(!tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_1));

    /* A partner with no piece in play cannot be collided with (:8C4C). */
    place_coop_pair(&game, TT_O, 0, 5, 10, TT_O, 0, 5, 10);
    game.player[1].piece.current = TT_NONE;
    CHECK(!tengen_coop_pieces_overlap(&game, TENGEN_PLAYER_1));

    /* None of it exists outside coop: two boards, nothing to meet. */
    TengenGame race;
    tengen_new_game(&race, 0x5EED, 0, true, false, false);
    race.player[0].piece.current = race.player[1].piece.current = TT_O;
    race.player[0].piece.x = race.player[1].piece.x = 5;
    race.player[0].piece.y = race.player[1].piece.y = 10;
    CHECK(!tengen_coop_pieces_overlap(&race, TENGEN_PLAYER_1));
}

static void test_a_coop_piece_cannot_be_walked_into_its_partner(void) {
    /* checkPositionAndClearFlagsOnCarrySet asks the partner before it asks
     * the field (main.asm.txt:1017-1021), so a shift into the other player's
     * piece is refused exactly as one into a wall is — and the refusal
     * reloads the auto-repeat to $09 (:527) so the player keeps asking. */
    TengenGame game;
    place_coop_pair(&game, TT_O, 0, 5, 10, TT_O, 0, 7, 10);
    CHECK(!tengen_try_move(&game, TENGEN_PLAYER_1, 1));
    CHECK(game.player[0].piece.x == 5);
    CHECK(game.player[0].das_right == TENGEN_DAS_CHARGE_BLOCKED);
    /* ...and away from it is still free. */
    CHECK(tengen_try_move(&game, TENGEN_PLAYER_1, -1));
    CHECK(game.player[0].piece.x == 4);

    /* A rotation into the partner is refused too, kick included, and leaves
     * the piece exactly as it was (:8371-838C). */
    place_coop_pair(&game, TT_I, 1, 5, 10, TT_I, 0, 3, 10);
    uint8_t before = game.player[0].piece.orientation;
    int8_t x_before = game.player[0].piece.x;
    CHECK(!tengen_try_rotate(&game, TENGEN_PLAYER_1, true));
    CHECK(game.player[0].piece.orientation == before);
    CHECK(game.player[0].piece.x == x_before);
}

static void test_a_coop_piece_hovers_on_its_partner_instead_of_landing(void) {
    /* L840B (main.asm.txt:604-616) is NOT the lock path. A piece whose fall
     * is stopped by the partner's piece is put back, given a fall timer of 1
     * and left to try again next frame — it must not merge into the board on
     * top of a piece that is still falling. */
    TengenGame game;
    place_coop_pair(&game, TT_O, 0, 5, 10, TT_O, 0, 5, 12);
    game.player[0].fall_timer = 1;
    game.player[1].fall_timer = 200;   /* the one underneath holds still */

    for (int i = 0; i < 240; i++) {
        TengenStepResult step = tengen_step(&game, TENGEN_PLAYER_1, 0);
        CHECK(!step.piece_locked);
        game.player[1].fall_timer = 200;
    }
    /* It came to rest ON the partner and stayed there. The O fills rows
     * y and y+1, so a piece at 10 is sitting directly on one at 12. */
    CHECK(game.player[0].piece.y == 10);
    CHECK(game.player[0].piece.current == TT_O);
    /* Nothing of it reached the board. */
    int settled = 0;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        for (int col = 0; col < TENGEN_PF_WIDTH; col++)
            if (game.field[0].cell[row][col] != TT_NONE) settled++;
    CHECK(settled == 0);

    /* And once the partner goes, it falls again like anything else. */
    game.player[1].piece.current = TT_NONE;
    game.player[0].fall_timer = 1;
    tengen_step(&game, TENGEN_PLAYER_1, 0);
    CHECK(game.player[0].piece.y == 11);
}

static void test_two_coop_players_pressed_together_untangle(void) {
    /* L862E (main.asm.txt:993-1010): shoulder to shoulder neither can move,
     * so every refused shift nudges a fall timer — the HIGHER piece waits and
     * the lower one is let down, which is what stops the two from standing
     * there forever. */
    TengenGame game;
    place_coop_pair(&game, TT_O, 0, 5, 10, TT_O, 0, 7, 11);
    game.player[0].fall_timer = 40;
    game.player[1].fall_timer = 40;

    /* Player 1 is the higher of the two, so pushing into player 2 makes
     * PLAYER 1 wait rather than player 2. */
    CHECK(!tengen_try_move(&game, TENGEN_PLAYER_1, 1));
    CHECK(game.player[0].fall_timer == 42);
    CHECK(game.player[1].fall_timer == 40);

    /* ...and from the other side, the one that is lower hands the wait back
     * up to its partner. */
    place_coop_pair(&game, TT_O, 0, 5, 11, TT_O, 0, 7, 10);
    game.player[0].fall_timer = 40;
    game.player[1].fall_timer = 40;
    CHECK(!tengen_try_move(&game, TENGEN_PLAYER_1, 1));
    CHECK(game.player[0].fall_timer == 40);
    CHECK(game.player[1].fall_timer == 42);
}

static void test_level_never_passes_the_rom_cap(void) {
    TengenGame game;
    tengen_new_game(&game, 37, TENGEN_MAX_LEVEL, false, false, false);
    clear_lines_until(&game, 60);
    CHECK(game.player[0].level == TENGEN_MAX_LEVEL);
}

static void test_das_charges_before_repeating(void) {
    TengenGame game;
    tengen_new_game(&game, 9, 0, false, false, false);
    game.player[0].piece.current = TT_O;
    /* Far enough right that three repeats still have room. This used to start
     * at 5 and the third repeat reached column 2 — which is the wall, and only
     * "worked" because the walls did not yet exist above the visible field.
     * The test was quietly encoding that bug; see
     * test_the_walls_reach_above_the_visible_field. */
    game.player[0].piece.x = 8;
    game.player[0].piece.y = 0;

    int8_t start_x = game.player[0].piece.x;

    /* Frame 1 of the hold: the fresh press moves immediately, and the DAS
     * counter starts charging on this same frame (counter = 1). */
    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    CHECK(game.player[0].piece.x == start_x - 1);
    CHECK(game.player[0].das_left == 1);

    /* Frames 2..10: still charging, no further movement. */
    int8_t x_after_press = game.player[0].piece.x;
    for (int i = 0; i < TENGEN_DAS_CHARGE_FIRST - 2; i++) {
        tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    }
    CHECK(game.player[0].piece.x == x_after_press);
    CHECK(game.player[0].das_left == TENGEN_DAS_CHARGE_FIRST - 1);

    /* Frame 11 of the hold reaches the charge threshold and fires, and the
     * counter reloads to 5 rather than 0 — that reload is what makes every
     * subsequent repeat 6 frames apart instead of another 11. */
    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    CHECK(game.player[0].piece.x == x_after_press - 1);
    CHECK(game.player[0].das_left == TENGEN_DAS_CHARGE_FIRST - TENGEN_DAS_CHARGE_REPEAT);

    /* And the next repeat lands exactly 6 frames later. */
    int8_t x_after_first_repeat = game.player[0].piece.x;
    for (int i = 0; i < TENGEN_DAS_CHARGE_REPEAT - 1; i++) {
        tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    }
    CHECK(game.player[0].piece.x == x_after_first_repeat);
    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    CHECK(game.player[0].piece.x == x_after_first_repeat - 1);
}

static void test_gravity_curve_matches_rom_table(void) {
    /* possibleFallTimerTable, main.asm.txt:4016-4019. Levels 0-9 are a
     * straight lookup with no row dependence. */
    const uint8_t expected[10] = {33, 28, 24, 20, 17, 14, 11, 9, 7, 6};
    for (uint8_t level = 0; level < 10; level++) {
        for (int8_t y = 0; y < 4; y++) {
            CHECK(tengen_frames_per_row(level, y, false, false) == expected[level]);
        }
    }
    /* The ROM's own non-monotonic bump: level 15 is slower than level 14's
     * fastest case. Pinned here so nobody "fixes" the table later. */
    CHECK(tengen_frames_per_row(15, 0, false, false) == 4);
    CHECK(tengen_frames_per_row(15, 1, false, false) == 3);
}

static void test_gravity_is_fractional_above_level_ten(void) {
    /* Levels 10-17 alternate between two table entries based on the piece's
     * row, which is how the ROM gets effectively fractional speeds
     * (main.asm.txt:3985-4000). Level 10 (mask $01) should alternate 5/6. */
    CHECK(tengen_frames_per_row(10, 0, false, false) == 5);
    CHECK(tengen_frames_per_row(10, 1, false, false) == 6);
    CHECK(tengen_frames_per_row(10, 2, false, false) == 5);
    CHECK(tengen_frames_per_row(10, 3, false, false) == 6);

    /* Level 11 (mask $00) never alternates. */
    for (int8_t y = 0; y < 8; y++) {
        CHECK(tengen_frames_per_row(11, y, false, false) == 5);
    }

    /* Level 14 (mask $03) takes the fast entry only on rows divisible by 4. */
    CHECK(tengen_frames_per_row(14, 0, false, false) == 3);
    CHECK(tengen_frames_per_row(14, 1, false, false) == 4);
    CHECK(tengen_frames_per_row(14, 2, false, false) == 4);
    CHECK(tengen_frames_per_row(14, 3, false, false) == 4);

    /* Level 16 flips the polarity (bne instead of beq in the ROM): the SLOW
     * entry is the one row in four, not the fast one. */
    CHECK(tengen_frames_per_row(16, 0, false, false) == 4);
    CHECK(tengen_frames_per_row(16, 1, false, false) == 3);
}

static void test_coop_uses_its_own_gentler_curve(void) {
    /* L9B48, main.asm.txt:4021-4025 — monotonic, and never fractional. */
    const uint8_t expected[18] = {33,28,24,20,18,17,16,15,14,13,12,11,10,9,8,7,6,5};
    for (uint8_t level = 0; level <= TENGEN_MAX_LEVEL; level++) {
        for (int8_t y = 0; y < 4; y++) {
            CHECK(tengen_frames_per_row(level, y, true, false) == expected[level]);
        }
    }
}

/* ...AND THE CADENCE A REAL CARTRIDGE FELL AT.
 *
 * Same method as test_deals_the_pieces_a_real_cartridge_dealt: the ROM was
 * run in tools/nes_cpu.py, driven into a 1 PLAYER match, taken up with its
 * OWN level-up code (Up Down Up Down Left Right B B A, then A to repeat), and
 * the frames between one increment of player1TetrominoY and the next counted
 * with nothing touching the pad. What it did:
 *
 *     level 10   5 6 5 6 5 6 ...      level 14   3 4 4 4 3 4 4 4 ...
 *     level 16   4 3 3 3 4 3 3 3 ...  level 17   3 3 3 3 ...
 *
 * which is the whole fractional band, the level-16 polarity flip included —
 * at 14 the FAST entry is the one row in four and at 16 it is the SLOW one,
 * because the ROM's branch goes `beq` in one case and `bne` in the other.
 * The port is asked for the same twelve rows in the same order. */
static void test_falls_at_the_cadence_a_real_cartridge_fell_at(void) {
    static const struct { uint8_t level; uint8_t frames[12]; } kMeasured[] = {
        { 10, { 5, 6, 5, 6, 5, 6, 5, 6, 5, 6, 5, 6 } },
        { 14, { 3, 4, 4, 4, 3, 4, 4, 4, 3, 4, 4, 4 } },
        { 16, { 4, 3, 3, 3, 4, 3, 3, 3, 4, 3, 3, 3 } },
        { 17, { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 } },
    };
    for (unsigned k = 0; k < sizeof kMeasured / sizeof kMeasured[0]; k++) {
        uint8_t level = kMeasured[k].level;
        /* The cartridge was measured from a piece that had just spawned, so
         * row 0 of each run is TENGEN_SPAWN_Y and the rest follow it down. */
        for (int i = 0; i < 12; i++) {
            int8_t y = (int8_t)(TENGEN_SPAWN_Y + i);
            CHECK(tengen_frames_per_row(level, y, false, false)
                  == kMeasured[k].frames[i]);
        }
    }
}

static void test_gravity_clamps_above_max_level(void) {
    CHECK(tengen_frames_per_row(TENGEN_MAX_LEVEL, 0, false, false) ==
          tengen_frames_per_row(99, 0, false, false));
    CHECK(tengen_frames_per_row(TENGEN_MAX_LEVEL_XE, 0, false, true) ==
          tengen_frames_per_row(99, 0, false, true));
}

/* TETRIS TENGEN XE — see TENGEN_MAX_LEVEL_XE. Two things are asserted here and
 * the first matters more than the second: that turning XE on changes NOTHING
 * below level 18, which is the claim that lets it ride the same chord as the
 * tunes. The mod's replacement tables at $FED0/$FEE4 really are byte-identical
 * to the cartridge's for levels 0-17. */
static void test_xe_changes_nothing_below_eighteen(void) {
    for (uint8_t level = 0; level <= TENGEN_MAX_LEVEL; level++) {
        for (int8_t y = 0; y < 8; y++) {
            CHECK(tengen_frames_per_row(level, y, false, true) ==
                  tengen_frames_per_row(level, y, false, false));
            CHECK(tengen_frames_per_row(level, y, true, true) ==
                  tengen_frames_per_row(level, y, true, false));
        }
    }
}

static void test_xe_adds_two_levels(void) {
    /* Level 18's mask is the mod's one new mask byte ($FF00 = $01), so 18
     * alternates between entries 18 and 17 — 2 and 3 frames. */
    CHECK(tengen_frames_per_row(18, 0, false, true) == 3);
    CHECK(tengen_frames_per_row(18, 1, false, true) == 2);

    /* LEVEL 19 IS THE MOD'S OWN OFF-BY-ONE, pinned here so nobody "fixes" it.
     * Its mask would be at $FF01 and the patch stops at $FF00, so it reads the
     * cartridge's zero; the level >= 16 branch always takes the dec; and the
     * game runs on entry 18 for ever. The mod's entry 19 (1 frame) is dead. */
    for (int8_t y = 0; y < 8; y++) {
        CHECK(tengen_frames_per_row(19, y, false, true) == 2);
    }

    /* Coop's table is plain: no masks at all, 4 and 3 frames. */
    for (int8_t y = 0; y < 4; y++) {
        CHECK(tengen_frames_per_row(18, y, true, true) == 4);
        CHECK(tengen_frames_per_row(19, y, true, true) == 3);
    }
}

/* The level code stops at 17 EVEN IN XE, because the mod does not patch its
 * clamp ($B4F7) — only checkLevelUp's. Play is the only way past 17. */
static void test_xe_level_code_still_stops_at_seventeen(void) {
    TengenGame game;
    tengen_new_game(&game, 91, TENGEN_MAX_LEVEL, false, false, true);
    game.paused = true;
    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == TENGEN_MAX_LEVEL);

    /* ...but from 18, which only play reaches, the digit test lets it move. */
    tengen_new_game(&game, 91, 18, false, false, true);
    game.paused = true;
    CHECK(enter_code(&game, kLevelUpButtons, 9) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == TENGEN_MAX_LEVEL_XE);
    /* And stops at the top of the mod's tables. */
    CHECK(press_code_button(&game, TENGEN_BTN_A) == TENGEN_CHEAT_LEVEL_UP);
    CHECK(game.player[0].level == TENGEN_MAX_LEVEL_XE);
}

/* The flag has to reach the other console, and the lobby word had four bits
 * for a level that now runs to 19. */
static void test_xe_travels_over_the_cable(void) {
    TengenLobby master, slave;
    const uint8_t handicap[2] = {0, 0};
    tengen_lobby_start_held(&master, 0xC0DE);
    tengen_lobby_start_held(&slave, 0);
    tengen_lobby_release(&master, 0xC0DE, 19, 6, handicap, false, true);
    for (int i = 0; i < 64 && !(master.ready && slave.ready); i++) {
        uint16_t mw = tengen_lobby_word(&master, true);
        uint16_t sw = tengen_lobby_word(&slave, false);
        tengen_lobby_apply(&master, true, true, mw, sw);
        tengen_lobby_apply(&slave, false, true, mw, sw);
    }
    CHECK(master.ready && slave.ready);
    CHECK(slave.start_level == 19);
    CHECK(slave.music == 6);
    CHECK(slave.xe);
}

static void test_soft_drop_requires_down_alone(void) {
    /* main.asm.txt:185-188: `and #DOWN+LEFT+RIGHT; cmp #DOWN` — Down combined
     * with a direction does NOT soft drop, it resets the threshold to 5. */
    TengenGame game;
    tengen_new_game(&game, 11, 0, false, false, false);
    game.player[0].piece.current = TT_O;
    game.player[0].piece.x = 4;
    game.player[0].piece.y = 0;

    for (int i = 0; i < 30; i++) {
        tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN | TENGEN_BTN_LEFT);
    }
    CHECK(game.player[0].drop_rate_possible == TENGEN_DROP_RATE_AFTER_RELEASE);
    CHECK(game.player[0].drop_repeat == 0);
}

static void test_soft_drop_accelerates_while_held(void) {
    /* Each firing tightens the threshold by one (floored at 1), so a held
     * Down speeds up over the life of a piece (main.asm.txt:198-201). */
    TengenGame game;
    tengen_new_game(&game, 13, 0, false, false, false);
    game.player[0].piece.current = TT_O;
    game.player[0].piece.x = 4;
    game.player[0].piece.y = 0;

    uint8_t before = game.player[0].drop_rate_possible;
    CHECK(before == TENGEN_DROP_RATE_AT_SPAWN);

    /* Run enough frames for at least one soft-drop firing. */
    for (int i = 0; i < TENGEN_DROP_RATE_AT_SPAWN; i++) {
        tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN);
    }
    CHECK(game.player[0].drop_rate_possible < before);
    CHECK(game.player[0].drop_rate_possible >= 1);
}

static void test_fresh_direction_press_is_swallowed_after_soft_drop(void) {
    /* main.asm.txt:98-107: a new Left/Right press is discarded outright if
     * Down was held on the previous frame. */
    TengenGame game;
    tengen_new_game(&game, 17, 0, false, false, false);
    game.player[0].piece.current = TT_O;
    game.player[0].piece.x = 4;
    game.player[0].piece.y = 0;

    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN);
    int8_t x_before = game.player[0].piece.x;
    /* Down was held last frame, so this fresh Left press is ignored. */
    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    CHECK(game.player[0].piece.x == x_before);
    /* The frame after, with Down no longer in last frame's state, it works. */
    tengen_step(&game, TENGEN_PLAYER_1, 0);
    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    CHECK(game.player[0].piece.x == x_before - 1);
}

static void test_das_does_not_charge_while_down_is_held(void) {
    /* main.asm.txt:111-114: the DAS counter only advances when the direction
     * is held and Down is not. */
    TengenGame game;
    tengen_new_game(&game, 19, 0, false, false, false);
    game.player[0].piece.current = TT_O;
    game.player[0].piece.x = 5;
    game.player[0].piece.y = 0;

    for (int i = 0; i < 30; i++) {
        tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT | TENGEN_BTN_DOWN);
    }
    CHECK(game.player[0].das_left == 0);
}

/* Every locked cell must be reachable from the floor through other locked
 * cells. Overhangs are legal in Tetris — an S landing on a ledge leaves a
 * hole under one of its cells — but a piece always comes to rest touching
 * something, so no locked group can ever be fully detached from the stack.
 * A collision or lock bug shows up here as a floating cluster, which is
 * exactly the kind of thing that's hard to be sure about by looking at a
 * 240x160 screenshot. */
static bool every_locked_cell_is_supported(const TengenPlayfield *field) {
    bool seen[TENGEN_PF_HEIGHT][TENGEN_PF_WIDTH] = {{false}};
    /* Flood fill upward from the bottom row through occupied cells. */
    TengenCell stack[TENGEN_PF_HEIGHT * TENGEN_PF_WIDTH];
    int top = 0;

    for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
        if (field->cell[TENGEN_PF_HEIGHT - 1][col] != TT_NONE) {
            seen[TENGEN_PF_HEIGHT - 1][col] = true;
            stack[top].row = TENGEN_PF_HEIGHT - 1;
            stack[top].col = (int8_t)col;
            top++;
        }
    }

    const int dr[4] = {-1, 1, 0, 0};
    const int dc[4] = {0, 0, -1, 1};
    while (top > 0) {
        TengenCell cur = stack[--top];
        for (int d = 0; d < 4; d++) {
            int nr = cur.row + dr[d];
            int nc = cur.col + dc[d];
            if (nr < 0 || nr >= TENGEN_PF_HEIGHT || nc < 0 || nc >= TENGEN_PF_WIDTH) continue;
            if (seen[nr][nc] || field->cell[nr][nc] == TT_NONE) continue;
            seen[nr][nc] = true;
            stack[top].row = (int8_t)nr;
            stack[top].col = (int8_t)nc;
            top++;
        }
    }

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
            if (field->cell[row][col] != TT_NONE && !seen[row][col]) return false;
        }
    }
    return true;
}

static void test_locking_stores_tile_ids_not_piece_ids(void) {
    /* The ROM's playfield nibble holds a block TILE id, not the piece it came
     * from (notes.txt.txt:35). That's what lets settled blocks keep the
     * directional artwork that joins them into shapes; storing piece ids and
     * choosing tiles at draw time cannot reproduce it, because the piece
     * boundary is gone by then. */
    TengenGame game;
    tengen_new_game(&game, 77, 0, false, false, false);

    /* Drop an O straight down. Its orientation-0 bitmap is 1100/1100, so it
     * occupies four cells whose tile ids are 0x0B,0x0E,0x0D,0x0C in scan
     * order (main.asm.txt:1131-1133, tilesForO). */
    game.player[0].piece.current = TT_O;
    game.player[0].piece.orientation = 0;
    game.player[0].piece.x = 7;
    game.player[0].piece.y = TENGEN_SPAWN_Y;

    for (int frame = 0; frame < 4000; frame++) {
        if (tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_DOWN).piece_locked) break;
    }

    int col = 7 - TENGEN_ROM_COL_ORIGIN; /* storage column of the piece's left edge */
    int bottom = TENGEN_PF_HEIGHT - 1;
    CHECK(game.field[0].cell[bottom - 1][col]     == 0x0B);
    CHECK(game.field[0].cell[bottom - 1][col + 1] == 0x0E);
    CHECK(game.field[0].cell[bottom][col]         == 0x0D);
    CHECK(game.field[0].cell[bottom][col + 1]     == 0x0C);

    /* And emphatically not the piece id. */
    CHECK(game.field[0].cell[bottom][col] != TT_O);

    /* Every stored block must be a legal tile id, never a stray value. */
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        for (int c = 1; c < TENGEN_PF_WIDTH - 1; c++) {
            uint8_t v = game.field[0].cell[row][c];
            CHECK(v == TENGEN_CELL_EMPTY || TENGEN_CELL_IS_BLOCK(v));
        }
    }
}

static void test_piece_stats_count_dealt_pieces_in_1p_only(void) {
    /* main.asm.txt:3730-3797: counted as the piece is dealt, and the whole
     * routine is skipped outside 1P. */
    TengenGame solo;
    tengen_new_game(&solo, 51, 0, false, false, false);
    /* One piece has been dealt by tengen_new_game's initial spawn. */
    unsigned total = 0;
    for (int piece = TT_I; piece <= TT_Z; piece++) total += solo.player[0].piece_stats[piece];
    CHECK(total == 1);
    CHECK(solo.player[0].piece_stats[solo.player[0].piece.current] == 1);

    /* Play a while; the total must track the number of pieces dealt. */
    uint32_t rolling = 51;
    unsigned locks = 0;
    for (int frame = 0; frame < 20000 && solo.player[0].game_active; frame++) {
        rolling = rolling * 1103515245u + 12345u;
        uint8_t buttons = ((rolling >> 16) % 3) ? TENGEN_BTN_DOWN : TENGEN_BTN_LEFT;
        if (tengen_step(&solo, TENGEN_PLAYER_1, buttons).piece_locked) locks++;
    }
    total = 0;
    for (int piece = TT_I; piece <= TT_Z; piece++) total += solo.player[0].piece_stats[piece];
    CHECK(locks > 0);
    CHECK(total == locks + 1); /* every lock deals a replacement, plus the first */

    /* 2P and coop don't track stats at all. */
    TengenGame coop;
    tengen_new_game(&coop, 51, 0, true, true, false);
    unsigned coop_total = 0;
    for (int piece = TT_I; piece <= TT_Z; piece++) coop_total += coop.player[0].piece_stats[piece];
    CHECK(coop_total == 0);
}

static void test_no_piece_ever_locks_in_mid_air(void) {
    /* Play out several games' worth of pieces under varied input and check
     * the support invariant after every single lock. */
    for (uint16_t seed = 1; seed <= 40; seed++) {
        TengenGame game;
        tengen_new_game(&game, seed, 0, false, false, false);

        uint32_t rolling = seed;
        for (int frame = 0; frame < 20000; frame++) {
            if (!game.player[0].game_active) break;

            /* Cheap deterministic input churn, so pieces land all over the
             * field instead of stacking in one column. */
            rolling = rolling * 1103515245u + 12345u;
            uint8_t buttons = 0;
            switch ((rolling >> 16) % 6) {
                case 0: buttons = TENGEN_BTN_LEFT; break;
                case 1: buttons = TENGEN_BTN_RIGHT; break;
                case 2: buttons = TENGEN_BTN_A; break;
                case 3: buttons = TENGEN_BTN_B; break;
                default: buttons = TENGEN_BTN_DOWN; break;
            }

            TengenStepResult r = tengen_step(&game, TENGEN_PLAYER_1, buttons);
            if (r.piece_locked) {
                if (!every_locked_cell_is_supported(&game.field[0])) {
                    printf("  (seed %u, frame %d: a locked group is floating)\n",
                           (unsigned)seed, frame);
                    CHECK(false);
                    return;
                }
            }
        }
    }
    CHECK(true);
}

static void test_locked_cells_never_overwrite_the_walls(void) {
    /* A piece writing into a wall column would quietly turn the frame into
     * playable space and break full-row detection. */
    for (uint16_t seed = 1; seed <= 20; seed++) {
        TengenGame game;
        tengen_new_game(&game, seed, 0, false, false, false);
        uint32_t rolling = seed;
        for (int frame = 0; frame < 8000; frame++) {
            if (!game.player[0].game_active) break;
            rolling = rolling * 1103515245u + 12345u;
            uint8_t buttons = ((rolling >> 16) % 2) ? TENGEN_BTN_LEFT : TENGEN_BTN_RIGHT;
            tengen_step(&game, TENGEN_PLAYER_1, buttons);
        }
        for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
            CHECK(game.field[0].cell[row][0] == TT_WALL);
            CHECK(game.field[0].cell[row][TENGEN_PF_WIDTH - 1] == TT_WALL);
        }
    }
}

int main(void) {
    test_rng_is_deterministic_and_never_stalls();
    test_rng_zero_seed_does_not_lock_up();
    test_deals_the_pieces_a_real_cartridge_dealt();
    test_piece_selector_never_returns_none_and_covers_all_seven();
    test_spawn_position_matches_rom();
    test_o_piece_never_needs_a_kick();
    test_wall_kick_only_ever_shifts_left();
    test_rotation_fails_cleanly_when_the_kick_cannot_help();
    test_a_row_of_walls_alone_is_not_a_full_row();
    test_move_rejects_out_of_bounds();
    test_line_clear_detects_and_collapses();
    test_level_up_thresholds_match_rom_table();
    test_das_charges_before_repeating();
    test_gravity_curve_matches_rom_table();
    test_gravity_is_fractional_above_level_ten();
    test_coop_uses_its_own_gentler_curve();
    test_falls_at_the_cadence_a_real_cartridge_fell_at();
    test_gravity_clamps_above_max_level();
    test_xe_changes_nothing_below_eighteen();
    test_xe_adds_two_levels();
    test_xe_level_code_still_stops_at_seventeen();
    test_xe_travels_over_the_cable();
    test_soft_drop_requires_down_alone();
    test_soft_drop_accelerates_while_held();
    test_fresh_direction_press_is_swallowed_after_soft_drop();
    test_das_does_not_charge_while_down_is_held();
    test_walls_are_present_in_1p_and_absent_in_coop();
    test_walls_block_movement_in_1p_but_not_coop();
    test_top_out_when_piece_rests_above_the_field();
    test_score_is_awarded_per_piece_and_rewards_height();
    test_completed_rows_wait_before_they_collapse();
    test_line_clear_sweep_advances_every_other_frame();
    test_start_pauses_and_stops_the_game();
    test_codes_only_count_while_paused();
    test_level_up_code_repeats_on_its_last_button();
    test_level_up_code_stops_at_the_rom_cap();
    test_long_bar_code_gives_an_i_once_per_level();
    test_undo_code_takes_the_last_piece_back_once();
    test_undo_is_disarmed_by_a_line_clear();
    test_a_wrong_button_restarts_the_code();
    test_codes_share_one_cursor_the_way_the_rom_does();
    test_the_walls_reach_above_the_visible_field();
    test_random_play_never_tops_out_on_a_nearly_empty_board();
    test_two_linked_machines_stay_identical();
    test_a_lost_transfer_stops_the_link_rather_than_drifting();
    test_the_button_that_starts_a_match_does_not_pause_it();
    test_the_lobby_connects_first_and_the_master_chooses_after();
    test_the_lobby_agrees_on_a_game_and_both_leave_together();
    test_the_lobby_survives_transfers_that_do_not_arrive();
    test_a_lobby_with_nothing_on_the_other_end_gives_up();
    test_no_lobby_word_can_look_like_an_absent_console();
    test_a_lobby_hands_straight_over_to_a_matching_pair_of_games();
    test_the_wire_word_survives_a_round_trip();
    test_the_computers_piece_table_derives_from_the_bitmaps();
    test_the_computer_reads_the_board_in_the_roms_own_units();
    test_the_computer_picks_a_placement_and_walks_to_it();
    test_the_computer_keeps_playing_and_does_not_bury_itself();
    test_coop_is_one_twelve_wide_board_over_the_cable();
    test_a_coop_top_out_ends_the_game_for_both_players();
    test_the_computers_soft_drop_does_not_eat_its_own_shifts();
    test_a_coop_line_clear_holds_both_players();
    test_the_computer_can_be_told_to_look_first_and_to_drop();
    test_either_player_can_pause_a_linked_game();
    test_level_starts_at_the_chosen_start_level();
    test_level_is_recomputed_from_the_line_total();
    test_the_handicap_buries_three_rows_a_step();
    test_every_handicap_row_has_a_way_through();
    test_the_handicap_is_the_same_from_the_same_seed();
    test_coop_garbage_fills_the_two_extra_columns();
    test_the_two_coop_pieces_are_solid_to_each_other();
    test_a_coop_piece_cannot_be_walked_into_its_partner();
    test_a_coop_piece_hovers_on_its_partner_instead_of_landing();
    test_two_coop_players_pressed_together_untangle();
    test_the_dancers_cast_grows_with_triples_and_tetrises();
    test_a_clear_is_tallied_by_how_many_rows_it_took();
    test_level_never_passes_the_rom_cap();
    test_locking_stores_tile_ids_not_piece_ids();
    test_piece_stats_count_dealt_pieces_in_1p_only();
    test_no_piece_ever_locks_in_mid_air();
    test_locked_cells_never_overwrite_the_walls();

    if (g_failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d assertion(s) failed.\n", g_failures);
    return 1;
}
