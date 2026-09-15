/* tengen_ai.h — the cartridge's COMPUTER player.
 *
 * This is `computerMove` (main.asm.txt:4207-4307) and the three routines it
 * leans on, transcribed rather than reinterpreted: the same height profile,
 * the same two candidate slots, the same well term, the same tie-break, and
 * the same button driver. Where the ROM's arithmetic overflows a byte, so
 * does this — the placements it picks are only the cartridge's if the
 * wrap-around is too.
 *
 * WHICH MODES USE IT. `playModeTable` ($9F51) is `00 01 FF 01 FF` for the
 * five entries of GAME SELECT, so VERSUS COMPUTER plays like 2P — two
 * separate ten-wide boards, a race — and WITH COMPUTER plays like COOP: one
 * twelve-wide board shared with it. In both, the computer is player 2
 * (main.asm.txt:3736-3749). The attract-mode demo drives player 1 with the
 * same code.
 *
 * It looks at ONE board — its own in a race, the shared one otherwise — which
 * is `playfieldPages,x` indexed by playMode ($8562: `06 07 06`), and is the
 * same field the core hands out for that slot.
 */
#ifndef TENGEN_AI_H
#define TENGEN_AI_H

#include "tengen_core.h"

/* The ROM's scratch, kept between calls because the ROM keeps it.
 *
 * `computerScratchB` is six bytes: [0] and [1] the two candidates' columns,
 * [2] and [3] their orientations, [4] and [5] their scores. computerMove
 * clears ONLY [4] and [5] (main.asm.txt:4262-4265), so the columns and
 * orientations survive from the piece before — which is what a call that
 * finds no candidate at all quietly falls back on. Reproducing that means
 * carrying the struct across pieces rather than rebuilding it.
 *
 * Slot 0 is the placement that sits FLUSH on the terrain, slot 1 the one that
 * does not; they are filled by two different routines and compared once at
 * the end. */
typedef struct {
    uint8_t scratch[6];
    uint8_t target_x;            /* compTargetX ($01CA) */
    uint8_t target_orientation;  /* compTargetOrientation ($01CB) */

    /* Two knobs the cartridge does not have, both set by the caller and both
     * zero by default — leave them alone and this is the ROM's player.
     *
     * `settle` holds the pad still for that many frames after a piece
     * appears. The ROM shifts off the GLOBAL frame counter, so a piece can be
     * yanked sideways on the very frame it spawns, and in the attract demo —
     * where there is no player to explain it — that reads as a machine, not
     * as somebody playing. A short pause first is all it takes.
     *
     * `soft_drop` presses DOWN on every frame the driver is not aiming, bar one
     * in six. The ROM never presses it at all, which is fine when the computer
     * has a board to itself; on the SHARED board of WITH COMPUTER the human
     * spends the whole game waiting on it. See tengen_ai_buttons for why it
     * starts at once and why it lets go. Off in the demo, so the attract mode
     * keeps the cartridge's pace. */
    uint8_t settle;
    bool soft_drop;

    uint8_t since_spawn;         /* frames since tengen_ai_choose was called */
} TengenAi;

/* A new game. The ROM does not clear this either — its scratch is whatever
 * the last game left — but a port that starts from uninitialised memory is a
 * port with a different bug every run, so this zeroes it. */
void tengen_ai_reset(TengenAi *ai);

/* Picks a column and an orientation for `slot`'s CURRENT piece. The ROM calls
 * this once per spawn, out of getNextTetromino (main.asm.txt:3735, 3749). */
void tengen_ai_choose(TengenAi *ai, const TengenGame *game,
                       TengenPlayerSlot slot);

/* ...and this turns that into one frame of controller input. The cadence is
 * the ROM's: a shift every eighth frame and a rotation every sixteenth
 * (main.asm.txt:4170-4202, and the comment there says so in as many words).
 * `frame_counter` stands in for frameCounterLow. */
uint8_t tengen_ai_buttons(TengenAi *ai, const TengenGame *game,
                           TengenPlayerSlot slot, uint8_t frame_counter);

/* Exposed for the tests: the sixteen column heights computerMove builds
 * before it scores anything, in the ROM's own byte units — eight to a row,
 * measured from ROM row 6 down, so a full column reads $30 and an empty one
 * reads $D0, the floor. `out` must hold TENGEN_AI_SCRATCH_A bytes. */
#define TENGEN_AI_COLUMNS 16
/* ...and three bytes of scratch the ROM keeps right behind them, which two of
 * its own reads run into. See the note by ai_well in tengen_ai.c. */
#define TENGEN_AI_SCRATCH_A 19
void tengen_ai_heights(const TengenGame *game, TengenPlayerSlot slot,
                        uint8_t out[TENGEN_AI_SCRATCH_A]);

/* The piece table's bonus byte, `computerMoveSelectTableOffsetBy18`'s first
 * byte of each entry — signed, and where the thing's taste lives. Exposed so
 * a test can show it is the ROM's. */
int8_t tengen_ai_bonus(TengenTetromino piece, uint8_t orientation);

/* The same entry's profile: each column's bottom RELATIVE TO THE PIECE'S
 * LEFTMOST OCCUPIED COLUMN, in byte units. Returns how many there are, which
 * is always the piece's width minus one. */
int tengen_ai_profile(TengenTetromino piece, uint8_t orientation,
                       uint8_t out[3]);

#endif /* TENGEN_AI_H */
