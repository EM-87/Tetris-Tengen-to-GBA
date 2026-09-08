/*
 * test_tengen.c — native (host-compiled) sanity tests for tengen_core.
 *
 * These run with plain gcc/clang, no GBA toolchain required (`make test`).
 * The point is to pin down the verified ROM behavior in cheap, fast
 * assertions BEFORE any of it gets wired to real GBA rendering/input, so a
 * regression shows up here instead of by eyeballing an emulator.
 */
#include "../src/tengen_core.h"

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

static void test_piece_selector_never_returns_none_and_covers_all_seven(void) {
    /* Exercise roll_next_piece indirectly via tengen_new_game/spawn, across
     * many seeds, and confirm every piece id 1..7 shows up and TT_NONE (0)
     * never does — mirrors "reroll on 0 of 8" (main.asm.txt:3700-3703). */
    bool seen[TENGEN_TETROMINO_COUNT] = {0};
    for (uint16_t seed = 1; seed < 400; seed++) {
        TengenGame game;
        tengen_new_game(&game, seed, 0, false, false);
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
    tengen_new_game(&game, 42, 0, false, false);
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
    tengen_new_game(&coop, 42, 0, true, true);
    CHECK(coop.player[0].piece.x == TENGEN_SPAWN_X[0]);
    CHECK(coop.player[1].piece.x == TENGEN_SPAWN_X[1]);
}

static void test_walls_are_present_in_1p_and_absent_in_coop(void) {
    /* initPlayer1orCoopPlayfield writes solid wall nibbles in 1P/2P and
     * leaves them open in coop, widening coop to 12 columns
     * (main.asm.txt:3468-3495 and its own comment at :3483). */
    TengenGame solo;
    tengen_new_game(&solo, 5, 0, false, false);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        CHECK(solo.field[0].cell[row][0] == TT_WALL);
        CHECK(solo.field[0].cell[row][TENGEN_PF_WIDTH - 1] == TT_WALL);
        CHECK(solo.field[0].cell[row][1] == TT_NONE);
        CHECK(solo.field[0].cell[row][TENGEN_PF_WIDTH - 2] == TT_NONE);
    }

    TengenGame coop;
    tengen_new_game(&coop, 5, 0, true, true);
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        CHECK(coop.field[0].cell[row][0] == TT_NONE);
        CHECK(coop.field[0].cell[row][TENGEN_PF_WIDTH - 1] == TT_NONE);
    }
}

static void test_walls_block_movement_in_1p_but_not_coop(void) {
    /* The same x that runs into a wall in 1P is playable in coop. */
    TengenGame solo;
    tengen_new_game(&solo, 6, 0, false, false);
    solo.player[0].piece.current = TT_O;   /* occupies local cols 0-1 */
    solo.player[0].piece.y = 10;
    solo.player[0].piece.x = TENGEN_ROM_COL_ORIGIN + 1; /* flush against the left wall */
    CHECK(tengen_position_valid(&solo, TENGEN_PLAYER_1));
    CHECK(!tengen_try_move(&solo, TENGEN_PLAYER_1, -1));

    TengenGame coop;
    tengen_new_game(&coop, 6, 0, true, true);
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
    tengen_new_game(&game, 8, 0, false, false);
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
    tengen_new_game(&low, 21, 0, false, false);
    tengen_new_game(&high, 21, 0, false, false);

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
    tengen_new_game(&game, 7, 0, false, false);
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
    tengen_new_game(&game, 3, 0, false, false);
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
    tengen_new_game(&game, 4, 0, false, false);
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
    tengen_new_game(&game, 1, 0, false, false);
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
    /* main.asm.txt:1473-1478, decoded bonusLinesTable. */
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[0] == 3);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[1] == 6);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[4] == 15);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[5] == 20);  /* the table switches from +3 to +5 here */
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[20] == 95);
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

static void test_level_starts_at_the_chosen_start_level(void) {
    TengenGame game;
    tengen_new_game(&game, 31, 9, false, false);
    CHECK(game.player[0].level == 9);
    CHECK(game.player[0].start_level == 9);
    /* And gravity should immediately reflect it, not level 0's 33 frames. */
    CHECK(tengen_frames_per_row(game.player[0].level, 0, false) == 6);
}

static void test_level_is_recomputed_from_the_line_total(void) {
    /* The ROM recomputes level as start_level + thresholds_passed on every
     * clear rather than incrementing (main.asm.txt:3140-3186), so a start
     * level offsets the whole curve. */
    TengenGame game;
    tengen_new_game(&game, 33, 5, false, false);
    clear_lines_until(&game, 3);
    CHECK(game.player[0].lines >= 3); /* the helper must actually have cleared lines */
    /* Three lines is the first threshold, so exactly one level above start. */
    CHECK(game.player[0].level == 6);
}

static void test_level_never_passes_the_rom_cap(void) {
    TengenGame game;
    tengen_new_game(&game, 37, TENGEN_MAX_LEVEL, false, false);
    clear_lines_until(&game, 6);
    CHECK(game.player[0].level == TENGEN_MAX_LEVEL);
}

static void test_das_charges_before_repeating(void) {
    TengenGame game;
    tengen_new_game(&game, 9, 0, false, false);
    game.player[0].piece.current = TT_O;
    game.player[0].piece.x = 5;
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
            CHECK(tengen_frames_per_row(level, y, false) == expected[level]);
        }
    }
    /* The ROM's own non-monotonic bump: level 15 is slower than level 14's
     * fastest case. Pinned here so nobody "fixes" the table later. */
    CHECK(tengen_frames_per_row(15, 0, false) == 4);
    CHECK(tengen_frames_per_row(15, 1, false) == 3);
}

static void test_gravity_is_fractional_above_level_ten(void) {
    /* Levels 10-17 alternate between two table entries based on the piece's
     * row, which is how the ROM gets effectively fractional speeds
     * (main.asm.txt:3985-4000). Level 10 (mask $01) should alternate 5/6. */
    CHECK(tengen_frames_per_row(10, 0, false) == 5);
    CHECK(tengen_frames_per_row(10, 1, false) == 6);
    CHECK(tengen_frames_per_row(10, 2, false) == 5);
    CHECK(tengen_frames_per_row(10, 3, false) == 6);

    /* Level 11 (mask $00) never alternates. */
    for (int8_t y = 0; y < 8; y++) {
        CHECK(tengen_frames_per_row(11, y, false) == 5);
    }

    /* Level 14 (mask $03) takes the fast entry only on rows divisible by 4. */
    CHECK(tengen_frames_per_row(14, 0, false) == 3);
    CHECK(tengen_frames_per_row(14, 1, false) == 4);
    CHECK(tengen_frames_per_row(14, 2, false) == 4);
    CHECK(tengen_frames_per_row(14, 3, false) == 4);

    /* Level 16 flips the polarity (bne instead of beq in the ROM): the SLOW
     * entry is the one row in four, not the fast one. */
    CHECK(tengen_frames_per_row(16, 0, false) == 4);
    CHECK(tengen_frames_per_row(16, 1, false) == 3);
}

static void test_coop_uses_its_own_gentler_curve(void) {
    /* L9B48, main.asm.txt:4021-4025 — monotonic, and never fractional. */
    const uint8_t expected[18] = {33,28,24,20,18,17,16,15,14,13,12,11,10,9,8,7,6,5};
    for (uint8_t level = 0; level <= TENGEN_MAX_LEVEL; level++) {
        for (int8_t y = 0; y < 4; y++) {
            CHECK(tengen_frames_per_row(level, y, true) == expected[level]);
        }
    }
}

static void test_gravity_clamps_above_max_level(void) {
    CHECK(tengen_frames_per_row(TENGEN_MAX_LEVEL, 0, false) ==
          tengen_frames_per_row(99, 0, false));
}

static void test_soft_drop_requires_down_alone(void) {
    /* main.asm.txt:185-188: `and #DOWN+LEFT+RIGHT; cmp #DOWN` — Down combined
     * with a direction does NOT soft drop, it resets the threshold to 5. */
    TengenGame game;
    tengen_new_game(&game, 11, 0, false, false);
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
    tengen_new_game(&game, 13, 0, false, false);
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
    tengen_new_game(&game, 17, 0, false, false);
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
    tengen_new_game(&game, 19, 0, false, false);
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

static void test_piece_stats_count_dealt_pieces_in_1p_only(void) {
    /* main.asm.txt:3730-3797: counted as the piece is dealt, and the whole
     * routine is skipped outside 1P. */
    TengenGame solo;
    tengen_new_game(&solo, 51, 0, false, false);
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
    tengen_new_game(&coop, 51, 0, true, true);
    unsigned coop_total = 0;
    for (int piece = TT_I; piece <= TT_Z; piece++) coop_total += coop.player[0].piece_stats[piece];
    CHECK(coop_total == 0);
}

static void test_no_piece_ever_locks_in_mid_air(void) {
    /* Play out several games' worth of pieces under varied input and check
     * the support invariant after every single lock. */
    for (uint16_t seed = 1; seed <= 40; seed++) {
        TengenGame game;
        tengen_new_game(&game, seed, 0, false, false);

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
        tengen_new_game(&game, seed, 0, false, false);
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
    test_gravity_clamps_above_max_level();
    test_soft_drop_requires_down_alone();
    test_soft_drop_accelerates_while_held();
    test_fresh_direction_press_is_swallowed_after_soft_drop();
    test_das_does_not_charge_while_down_is_held();
    test_walls_are_present_in_1p_and_absent_in_coop();
    test_walls_block_movement_in_1p_but_not_coop();
    test_top_out_when_piece_rests_above_the_field();
    test_score_is_awarded_per_piece_and_rewards_height();
    test_level_starts_at_the_chosen_start_level();
    test_level_is_recomputed_from_the_line_total();
    test_level_never_passes_the_rom_cap();
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
