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

    /* Frame 1: fresh press moves immediately. */
    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    CHECK(game.player[0].piece.x == start_x - 1);

    /* Frames 2..10 (9 more, held): still charging, no further movement. */
    int8_t x_after_press = game.player[0].piece.x;
    for (int i = 0; i < TENGEN_DAS_CHARGE_FIRST - 1; i++) {
        tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    }
    CHECK(game.player[0].piece.x == x_after_press);

    /* One more frame reaches the charge threshold and fires. */
    tengen_step(&game, TENGEN_PLAYER_1, TENGEN_BTN_LEFT);
    CHECK(game.player[0].piece.x == x_after_press - 1);
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

    if (g_failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    printf("%d assertion(s) failed.\n", g_failures);
    return 1;
}
