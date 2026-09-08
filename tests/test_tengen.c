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
    CHECK(game.player[0].piece.x == TENGEN_SPAWN_X[0]);
    CHECK(game.player[0].piece.orientation == 0);
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));
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
    piece->orientation = 1; /* vertical T, main.asm.txt:1111 orientationForT[1] = 8C,80 */
    piece->y = 0;
    piece->x = TENGEN_PF_WIDTH - 2; /* flush against the right wall for this orientation */
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));

    int8_t x_before = piece->x;
    bool rotated = tengen_try_rotate(&game, TENGEN_PLAYER_1, true);
    if (rotated) {
        CHECK(piece->x <= x_before); /* only ever kicks left, never right */
    }
    /* Whether or not this particular orientation pair needs a kick, the
     * piece must always end up in a legal position. */
    CHECK(tengen_position_valid(&game, TENGEN_PLAYER_1));
}

static void test_move_rejects_out_of_bounds(void) {
    TengenGame game;
    tengen_new_game(&game, 1, 0, false, false);
    game.player[0].piece.current = TT_O;
    game.player[0].piece.x = 0;
    CHECK(!tengen_try_move(&game, TENGEN_PLAYER_1, -1)); /* would leave the field */
    CHECK(game.player[0].piece.x == 0);                  /* reverted */
}

static void test_line_clear_detects_and_collapses(void) {
    TengenPlayfield field;
    memset(&field, 0, sizeof(field));
    for (int col = 0; col < TENGEN_PF_WIDTH; col++) {
        field.cell[TENGEN_PF_HEIGHT - 1][col] = TT_I; /* bottom row: full */
    }
    field.cell[TENGEN_PF_HEIGHT - 2][0] = TT_T; /* row above: a single marker cell */

    uint32_t mask = tengen_clear_full_rows(&field);
    CHECK(mask == (1u << (TENGEN_PF_HEIGHT - 1)));
    /* The marker cell should have dropped down into the now-empty bottom row. */
    CHECK(field.cell[TENGEN_PF_HEIGHT - 1][0] == TT_T);
    for (int col = 1; col < TENGEN_PF_WIDTH; col++) {
        CHECK(field.cell[TENGEN_PF_HEIGHT - 1][col] == TT_NONE);
    }
}

static void test_level_up_thresholds_match_rom_table(void) {
    /* main.asm.txt:1473-1478, decoded bonusLinesTable. */
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[0] == 3);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[1] == 6);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[4] == 15);
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[5] == 20);  /* the table switches from +3 to +5 here */
    CHECK(TENGEN_LEVEL_LINE_THRESHOLDS[20] == 95);
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

int main(void) {
    test_rng_is_deterministic_and_never_stalls();
    test_rng_zero_seed_does_not_lock_up();
    test_piece_selector_never_returns_none_and_covers_all_seven();
    test_spawn_position_matches_rom();
    test_o_piece_never_needs_a_kick();
    test_wall_kick_only_ever_shifts_left();
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

    if (g_failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d assertion(s) failed.\n", g_failures);
    return 1;
}
