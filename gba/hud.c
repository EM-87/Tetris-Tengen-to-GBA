/*
 * hud.c -- what a match looks like.
 *
 * The two panels of rope and everything inside them, the board, the
 * falling piece and the sweep that clears a row, the cossacks, the piece
 * histogram, the high-score table and the plaques. Drawing only: nothing
 * here decides anything about the game.
 */
#include "port.h"


/* COOP'S EIGHT, in TWO columns, which is the other half of the same table.
 *
 * Entries 6-13 of the ROM's position tables pair the dancers off down the two
 * sides at NES x $40 and $B1 — just inside each panel — and four heights
 * whose feet land on the ledges the coop screen already carries. So there is
 * no stage to blit here: the cartridge drew it into the nametable, and the
 * port is showing that nametable.
 *
 * They walk OUTWARD, each column toward its own side, which is what attribute
 * bit 6 on the left-hand entries is for: the same sprite mirrored, facing the
 * way it is going. Where they stop is the port's choice for the same reason
 * it is in a solo game — the choreography scripts are not traced — and it is
 * the middle of the panel they are walking onto. */
/* ----------------------------------------------------------------------- *
 * THE CHOREOGRAPHY (VERIFIED, LB015 at main.asm.txt:6392-6525)
 *
 * The cossacks were the one thing in this port still doing an impression.
 * Their POSES are the cartridge's, their positions are, their cadence is,
 * their number is — but which pose each of them strikes was the port walking
 * the pose table from a staggered start, because the cartridge does not keep
 * a sequence of poses anywhere. It keeps a little PROGRAM per dancer, and
 * this is its interpreter.
 *
 * Each dancer holds a pointer ($019A low / $01A2 high on the cartridge, and
 * g_dance_pc here) into a list of 2-byte entries, advanced by one entry
 * every eighth frame. WHAT AN ENTRY MEANS is decided by comparing it against
 * two addresses, which is as close to a type tag as 1989 gets:
 *
 *   >= $C8BC   a POSE: four tile ids, one per sprite of the 2x2.
 *   >= $B14D   a JUMP: carry on reading at that address instead.
 *   otherwise  a BRANCH: the entry names a table of sixteen pointers and
 *              shuffleRngSeed5x picks one of them. This is what keeps six
 *              dancers on one stage from falling into step.
 *
 * A dancer WALKS — one pixel every fourth frame — only while its program
 * lies below $B181. That is what stops them where they stop: the port used
 * to walk them to a mark it had to choose for itself, and getting that
 * choice wrong left every one of them half off its own ledge.
 *
 * NOTHING IS EXTRACTED FOR THIS. The programs, the branch tables and the
 * poses are all inside the PRG slice the port already carries for the sound
 * engine, at the addresses the 6502 knows them by, so the driver reads them
 * straight out of it with nes_rom_peek and the dice are the cartridge's own
 * routine. The one path not modelled is LB043's wait, which tests the show's
 * counter against $70: the interlude runs that counter from $7C to $F4, so
 * on this screen it is always above it and the branch always fires.
 * ----------------------------------------------------------------------- */
#define DANCE_START_TABLE 0x8E86   /* L8E86: where each dancer's program begins */
#define DANCE_BRANCH_TOP  0xB14D   /* LB14D */
#define DANCE_POSE_BASE   0xC8BC   /* possibleUnusedData2 — the poses start here */
#define DANCE_WALK_TOP    0xB181   /* LB181: a program below this one walks */
#define NES_SHUFFLE_RNG   0x9A0D   /* shuffleRngSeed5x (main.asm.txt:3835) */
#define NES_RAM_RNG_SEED  0x0034   /* rngSeed (tetris-ram.asm.txt:37) */
#define DANCE_COOP_POSITION 6      /* coop's first position; see dancers_begin */
#define DANCE_FOLLOW_MAX  8        /* a hang guard, not a rule */

static uint16_t g_dance_pc[DANCER_COOP_COUNT];
static uint8_t g_dance_pose[DANCER_COOP_COUNT][DANCER_SPRITES];
static uint16_t g_dance_walk[DANCER_COOP_COUNT];

static uint16_t rom_peek16(uint16_t at) {
    return (uint16_t)(nes_rom_peek(at) | (nes_rom_peek((uint16_t)(at + 1)) << 8));
}

/* Follow jumps and branches until the entry under the pointer is a pose, and
 * take its four tiles. Idempotent once it lands on one, which is why the
 * cartridge can afford to do it every frame: a branch is spent the moment it
 * is taken, because what it stores is the pointer it landed on. */
static void dance_settle(int d) {
    uint16_t entry = DANCE_POSE_BASE;
    for (int hop = 0; hop < DANCE_FOLLOW_MAX; hop++) {
        entry = rom_peek16(g_dance_pc[d]);
        if (entry >= DANCE_POSE_BASE) break;
        if (entry >= DANCE_BRANCH_TOP) { g_dance_pc[d] = entry; continue; }
        nes_rom_call(NES_SHUFFLE_RNG, 0, 2000);
        g_dance_pc[d] = rom_peek16((uint16_t)(entry + (nes_rom_acc() & 0x1E)));
    }
    for (int s = 0; s < DANCER_SPRITES; s++)
        g_dance_pose[d][s] = nes_rom_peek((uint16_t)(entry + s));
}

/* The show is starting: every dancer back to the head of its own program.
 *
 * ONLY THE ONES WHO COME ON, and that is not a tidiness: L8E46 zeroes the
 * program pointer of every slot the cast does not fill and LB019 skips a
 * slot whose pointer's high byte is zero, so a troupe of six rolls the dice
 * six times a frame and not eight. Giving the two empty slots a programme
 * anyway left them rolling too, and the dice are SHARED — two extra rolls
 * moved every other dancer onto a different branch. It cost nothing on
 * screen (they were never drawn) and the whole choreography downstream. */
void dancers_begin(uint16_t seed, int cast) {
    uint8_t *ram = nes_rom_ram();
    /* The dice want a seed and the cartridge's own is not in this RAM — the
     * port runs the sound engine here, not the game. The match's is as good
     * as any and makes a replay of the same game dance the same way. */
    ram[NES_RAM_RNG_SEED] = (uint8_t)seed;
    ram[NES_RAM_RNG_SEED + 1] = (uint8_t)(seed >> 8);
    /* FOURTEEN ENTRIES, NOT EIGHT, and which six or eight of them a cast
     * gets is the POSITION each dancer stands in: L8DE4 indexes this table
     * by that, and the positions are 0-5 down the one column a solo screen
     * has and 6-13 in the pairs coop runs down both of its panels. There
     * are only two distinct programmes in the table — $B14D and $B167 —
     * but which dancer gets which is what keeps them out of step. */
    int base = g_session.game.coop ? DANCE_COOP_POSITION : 0;
    if (cast > DANCER_COOP_COUNT) cast = DANCER_COOP_COUNT;
    for (int d = 0; d < DANCER_COOP_COUNT; d++) {
        g_dance_walk[d] = 0;
        for (int s = 0; s < DANCER_SPRITES; s++) g_dance_pose[d][s] = 0;
        if (d >= cast) { g_dance_pc[d] = 0; continue; }   /* L8E46's zeroes */
        g_dance_pc[d] = rom_peek16((uint16_t)(DANCE_START_TABLE + (base + d) * 2));
        dance_settle(d);
    }
}

/* ...and one frame of it. `frame` is the show's own counter, which is what
 * the cartridge tests frameCounterLow for. */
void dancers_step(int frame) {
    for (int d = 0; d < DANCER_COOP_COUNT; d++) {
        if ((g_dance_pc[d] >> 8) == 0) continue;   /* LB019: an empty slot */
        if ((frame % DANCER_POSE_FRAMES) == 0) g_dance_pc[d] += 2;
        dance_settle(d);
        if (g_dance_pc[d] < DANCE_WALK_TOP && (frame % DANCER_WALK_FRAMES) == 0)
            g_dance_walk[d]++;
    }
}

void draw_coop_dancers(int elapsed, int count) {
    (void)elapsed;                      /* the driver keeps the clock now */
    if (count > DANCER_COOP_COUNT) count = DANCER_COOP_COUNT;
    for (int d = 0; d < count; d++) {
        const uint8_t *tiles = g_dance_pose[d];
        int walk = (int)g_dance_walk[d];
        uint8_t attr = kDancerCoopAttr[d];
        bool leftward = (attr & 0x40) != 0;   /* mirrored: walks to the left */

        /* NES pixels to the port's: one column came off the left of the
         * screen, and the window starts at the field's first row. */
        int x = (int)kDancerCoopX[d] - 8;
        int y = (int)kDancerCoopY[d] - SCREEN_COOP_FIELD_TY * 8;

        /* No mark to stop them at any more: their own programs stop them,
         * by leaving the range that walks. See the driver above. */
        x += leftward ? -walk : walk;

        for (int s = 0; s < DANCER_SPRITES; s++) {
            /* A mirrored pair swaps its own left and right halves. */
            int sx = x + (((s & 1) != 0) != leftward ? 8 : 0);
            int sy = y + ((s & 2) ? 8 : 0);
            oam_set(d * DANCER_SPRITES + s, sx, sy, tiles[s], leftward,
                     PAL_OBJ_DANCER + (attr & 3));
        }
    }
    for (int i = count * DANCER_SPRITES; i < 128; i++)
        MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

/* Places the dancers for one frame of the interlude, in the ROM's own
 * positions: one column of six, 24 pixels apart, walking right off their
 * starting mark onto the ledges. */
void draw_dancers(int elapsed, int count) {
    (void)elapsed;                      /* the driver keeps the clock now */
    if (count > DANCER_COUNT) count = DANCER_COUNT;
    for (int d = 0; d < count; d++) {
        /* Their own programs, out of the cartridge: see the driver above. */
        const uint8_t *tiles = g_dance_pose[d];

        int x = DANCER_STAGE_TX * 8 - DANCER_START_OFFSET + (int)g_dance_walk[d];
        int y = (int)kDancerStartY[d] - DANCER_Y_ORIGIN;
        /* They walk on from the left and stop where their programs stop
         * walking them, which is what the port used to have to guess at.
         * The stage is the banner's four columns (DANCER_STAGE_TX ==
         * BANNER_TX, exactly as the cartridge has them at nametable column
         * 14), so a walk that ran on would take them off it. */

        for (int s = 0; s < DANCER_SPRITES; s++) {
            int sx = x + ((s & 1) ? 8 : 0);
            int sy = y + ((s & 2) ? 8 : 0);
            oam_set(d * DANCER_SPRITES + s, sx, sy, tiles[s], false,
                     PAL_OBJ_DANCER + (kDancerAttr[d] & 3));
        }
    }
    /* L8E42-8E53 blanks the sprites the smaller cast does not use; everything
     * else on screen during the interlude is background. */
    for (int i = count * DANCER_SPRITES; i < 128; i++)
        MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}
static const uint8_t kIdlePoses[2] = { 0, 1 };

uint8_t g_idle_palette;
int g_dance_frames;      /* frames of the reaction still to play */
static int g_dance_length;      /* ...and how many it started with */
/* THE LEVEL-UP SHOW, DANCED ALONE. The cartridge's interlude sends a troupe
 * out onto a stage that takes the whole right-hand column — which is the
 * TETRIS banner's column, so it can only be had by giving up the statistics.
 * In HUD STATS the interlude still happens, with its own music and its own
 * traced 32 seconds, but the screen stays exactly where it was and the one
 * cossack who is already standing there dances it by himself. The troupe is
 * what HUD BANNER is FOR: it is the harder way to play, since it costs you
 * the piece histogram, and the six of them are what it pays back. */
static bool g_idle_show;

static void hide_idle_cossack(void) {
    for (int i = 0; i < DANCER_SPRITES; i++)
        MEM_OAM[(IDLE_OAM_BASE + i) * 4] = OBJ_ATTR0_HIDDEN;
}

/* Starts the reaction. `lines` is 1-4; anything else is ignored. */
void idle_cossack_celebrate(int lines) {
    if (lines < 1) return;
    if (lines > 4) lines = 4;
    int poses = lines * DANCE_POSES_PER_LINE;
    if (poses > DANCE_MAX_POSES) poses = DANCE_MAX_POSES;
    g_dance_length = poses * DANCE_POSE_FRAMES;
    g_dance_frames = g_dance_length;
}

/* `running` false freezes him where he stands — which is what a game over
 * should look like from the wings. */
static void draw_idle_cossack(int elapsed, bool running,
                               int tx, int ty, int w, int h) {
    int pose;
    if (g_idle_show) {
        /* Round and round the figure for as long as the show lasts, at the
         * show's own eight-frame cadence. */
        int steps = DANCER_POSE_COUNT - DANCE_FIRST_POSE;
        pose = DANCE_FIRST_POSE + (elapsed / DANCE_POSE_FRAMES) % steps;
    } else if (g_dance_frames > 0) {
        int done = (g_dance_length - g_dance_frames) / DANCE_POSE_FRAMES;
        pose = DANCE_FIRST_POSE + done;
        if (pose >= DANCER_POSE_COUNT) pose = DANCER_POSE_COUNT - 1;
        if (running) g_dance_frames--;
    } else {
        pose = kIdlePoses[(elapsed / IDLE_POSE_FRAMES) & 1];
    }
    const uint8_t *tiles = kDancerPoses[pose];
    int x = tx * 8 + (w * 8 - 16) / 2;
    int y = ty * 8 + (h * 8 - 16) / 2;
    for (int s = 0; s < DANCER_SPRITES; s++)
        oam_set(IDLE_OAM_BASE + s, x + ((s & 1) ? 8 : 0), y + ((s & 2) ? 8 : 0),
                 tiles[s], false, PAL_OBJ_DANCER + g_idle_palette);
}

/* The puff of smoke crossing each completed row: five sprites in a row, the
 * head at the column the sweep has reached and the rest trailing one column
 * apart behind it, each retiring as it leaves the field. */
/* WHOSE ROWS ARE COMING DOWN. On a race board only the viewed player's can
 * be; on the coop board either player's can, and the core holds both still
 * while they do (tengen_step's coop hold), so the sweep, the word and the
 * frozen field all have to follow whichever timer is running — which used
 * to be the viewed player's only, so the partner's clears simply happened
 * with no sweep and no word, and on the guest's console the rows vanished
 * under a piece that had never seemed to stop. */
int hud_clearing_slot(void) {
    if (g_session.game.coop &&
        g_session.game.player[g_view].line_clear_timer == 0 &&
        g_session.game.player[g_view ^ 1].line_clear_timer > 0)
        return g_view ^ 1;
    return g_view;
}

void draw_line_clear_sweep(void) {
    int slot = hud_clearing_slot();
    const TengenPlayerState *p = &g_session.game.player[slot];
    uint8_t step = tengen_line_clear_step(&g_session.game, (TengenPlayerSlot)slot);
    int used = 0;

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        if (!(p->clearing_rows & (1u << row))) continue;
        for (int s = 0; s < TENGEN_CLEAR_SPARKS; s++) {
            int col = (int)step - TENGEN_CLEAR_TRAIL + s;
            if (col < 0 || col >= field_cols()) continue;
            oam_set(used++, (field_tx() + col) * 8, (FIELD_TY + row) * 8,
                     (uint16_t)(CLEAR_HEAD_TILE + s), false, PAL_OBJ_CLEAR);
        }
    }
    /* ...but not the ones at the top: the idle cossack lives in the last four
     * and the drop-point digits in the six under him, and both are drawn
     * after this. */
    for (int i = used; i < POINTS_OAM_BASE; i++) MEM_OAM[i * 4] = OBJ_ATTR0_HIDDEN;
}

static struct {
    uint16_t value;
    uint8_t row;
    uint8_t timer;
} g_points[2];

void points_clear(void) {
    for (int i = 0; i < 2; i++) g_points[i].timer = 0;
    for (int i = 0; i < POINTS_DIGITS * 2; i++)
        MEM_OAM[(POINTS_OAM_BASE + i) * 4] = OBJ_ATTR0_HIDDEN;
}

/* One piece, come to rest, worth this much. */
void note_award(int slot, TengenStepResult step) {
    if (!step.award) return;
    int row = (TENGEN_PF_HEIGHT - 1) - (int)step.award_rows_above_floor;
    if (row < POINTS_TOP_ROW) row = POINTS_TOP_ROW;
    g_points[slot].value = step.award;
    g_points[slot].row = (uint8_t)row;
    g_points[slot].timer = POINTS_FRAMES;
}

void draw_points(void) {
    for (int slot = 0; slot < 2; slot++) {
        int base = POINTS_OAM_BASE + slot * POINTS_DIGITS;
        const TengenPlayerState *p = &g_session.game.player[slot];
        /* A board this screen is not showing has nothing to say. Only coop
         * has two players on the one board; a race shows one of them. */
        bool visible = g_session.game.coop || slot == g_view;
        /* stageDropPointSprites' first line: while rows are coming down the
         * sprites are neither drawn NOR counted down — the clock stops with
         * everything else. */
        if (visible && g_points[slot].timer && p->line_clear_timer == 0)
            g_points[slot].timer--;
        if (!visible || !g_points[slot].timer || p->line_clear_timer > 0) {
            for (int i = 0; i < POINTS_DIGITS; i++)
                MEM_OAM[(base + i) * 4] = OBJ_ATTR0_HIDDEN;
            continue;
        }

        /* The digits, most significant first, with the leading zeros simply
         * not drawn — which is what the ROM's blank tile amounts to. */
        uint8_t digit[POINTS_DIGITS];
        int count = 0;
        uint16_t v = g_points[slot].value;
        for (int i = POINTS_DIGITS - 1; i >= 0; i--) {
            digit[i] = (uint8_t)(v % 10);
            v /= 10;
            if (digit[i] || i == POINTS_DIGITS - 1) count = POINTS_DIGITS - i;
        }

        int x;
        if (!g_session.game.coop) {
            /* Outside the right-hand edge, reading away from the board. */
            x = (FIELD_TX + FIELD_PLAYABLE) * 8;
        } else if (slot == 0) {
            /* Left of the shared board, ending at its edge. */
            x = COOP_FIELD_TX * 8 - count * 8;
        } else {
            x = (COOP_FIELD_TX + TENGEN_PF_WIDTH) * 8;
        }

        int bank = PAL_OBJ_GAME + (g_session.game.two_player ? slot : 3);
        int y = (FIELD_TY + g_points[slot].row) * 8;
        for (int i = 0; i < POINTS_DIGITS; i++) {
            int d = POINTS_DIGITS - count + i;
            if (i >= count) { MEM_OAM[(base + i) * 4] = OBJ_ATTR0_HIDDEN; continue; }
            oam_set(base + i, x + i * 8, y,
                     (uint16_t)(POINTS_OBJ_TILE_BASE + digit[d]), false, bank);
        }
    }
}

/* While this is on, everything that goes through set_map_tile lands on the
 * counters' layer instead of the main one. It is a switch rather than a
 * second set of drawing functions because the panel is drawn with the same
 * draw_text / draw_number / draw_rule the menus use, and those should not
 * have to know which background they are writing to. */
bool g_panel_layer;

void set_map_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(g_panel_layer ? SCREENBLOCK_PANEL : SCREENBLOCK)
        [ty * MAP_W + tx] = entry;
}

void clear_region(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) set_map_tile(tx + x, ty + y, T_BLANK);
}

void clear_panel_region(int tx, int ty, int w, int h) {
    bool was = g_panel_layer;
    g_panel_layer = true;
    clear_region(tx, ty, w, h);
    g_panel_layer = was;
}

/* The offset layer. Tile 0 of the cartridge's set is transparent in every
 * pixel, so everywhere this map is not written the screen is simply the one
 * below it. */
void set_stats_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK_STATS)[ty * MAP_W + tx] = entry;
}

/* The histogram's own. Same three pixels across, none down. */
void set_histogram_tile(int tx, int ty, uint16_t entry) {
    if (tx < 0 || tx >= MAP_W || ty < 0 || ty >= 32) return;
    MEM_SCREENBLOCK(SCREENBLOCK_HISTOGRAM)[ty * MAP_W + tx] = entry;
}

/* Wipes a rectangle off BOTH maps. Anywhere the offset layer might be holding
 * something has to be cleared this way or half a drawing survives. */
void clear_both(int tx, int ty, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            set_map_tile(tx + x, ty + y, T_BLANK);
            set_stats_tile(tx + x, ty + y, T_BLANK);
        }
}

/* ----------------------------------------------------------------------- *
 * A BOX OF BRAID.
 *
 * The blue rope that runs down either side of the playfield, closed into a
 * rectangle. Its corners and horizontal runs are the ones the cartridge
 * borders its whole screen with (kBraid*, see read_braid_frame in
 * tools/extract_assets.py), so this is the game's own frame and not a
 * lookalike.
 *
 * IT IS TWO TILES THICK, and that is not adjustable: each tile is half the
 * rope cut lengthwise. So a box ten columns wide has a six-column interior,
 * and that single fact decides the whole HUD below.
 * ----------------------------------------------------------------------- */
/* WHICH OF THE TWO THINGS THE RIGHT PANEL IS HOLDING — the piece histogram or
 * the cartridge's vertical TETRIS banner. L+R swaps it in play; the frame is
 * the same either way and only the shelf inside it differs, which is why this
 * has to be visible to draw_static_screen. See the note above draw_banner.
 *
 * IT STARTS ON THE BANNER, because that is the cartridge's own screen: the
 * vertical TETRIS is what a player who has seen this game remembers of it,
 * and the histogram is the thing you go and ask for. It is also the harder
 * way to play — no piece counts — which is why the level-up troupe is
 * reserved for it (see g_idle_show). */
bool g_show_banner = true;

/* One shelf, `w` columns of it. */
static void draw_ledge(int tx, int ty, int w) {
    for (int x = 0; x < w; x++)
        set_map_tile(tx + x, ty, WITH_BANK(T_LEDGE, BRAID_BANK));
}

/* The two columns of rope that frame the playfield, as a plain strip from top
 * to bottom. Used when the banner or the dancers take the rest of the column:
 * clearing around the box leaves its corners dangling, and the cartridge's own
 * screen has a plain strip here anyway.
 *
 * THE RUN MUST BE THE PANEL'S OWN. This strip replaces the right panel's left
 * side, so it is kBraidRight — the same tile draw_braid_panel puts there. Hand
 * it the other one and the weave changes direction every time L+R is pressed,
 * which is exactly what it used to do. */
void draw_field_braid(int tx, const uint8_t run[1][2]) {
    for (int y = 0; y < SCREEN_TH; y++)
        for (int dx = 0; dx < BRAID_T; dx++)
            set_map_tile(tx + dx, y, WITH_BANK(run[0][dx], BRAID_BANK));
}

/* AN INVERTED L, NOT A BOX. Rope along the top and down the side facing the
 * board; the screen's own edge closes it outward and the bottom is simply
 * open, which is what the cartridge's coop panels are. It was a closed
 * rectangle either side until the panel became the coop screen's: a box
 * leaves sixteen interior rows, and sixteen is one row short of four
 * compartments and two short of the TETRIS banner's eighteen. Open, it is
 * eighteen, and both of them fit.
 *
 * `shelves` adds the four ledges. The right panel takes its one separately —
 * it depends on what that box is holding — so the flag is only about the
 * four, never about the shape. */
static void draw_braid_panel(int tx, int w, bool inner_right, bool shelves) {
    int ix = inner_right ? tx + w - BRAID_T : tx;   /* the inner run's column */

    for (int dy = 0; dy < BRAID_T; dy++)
        for (int dx = 0; dx < BRAID_T; dx++)
            set_map_tile(ix + dx, dy,
                          WITH_BANK(inner_right ? kBraidHangLeft[dy][dx]
                                                : kBraidHangRight[dy][dx],
                                     BRAID_BANK));
    for (int x = 0; x < w; x++) {
        int cx = tx + x;
        if (cx >= ix && cx < ix + BRAID_T) continue;      /* the corners */
        for (int dy = 0; dy < BRAID_T; dy++)
            /* The panel's OWN run, not kBraidBottom — see SKIN_PANEL_RUN_BASE.
             * Same art as kBraidBottom while no skin is on; a different one
             * under a skin, where the menu's bottom border and this want
             * different halves of the prototype's frame. */
            set_map_tile(cx, dy, WITH_BANK(SKIN_PANEL_RUN_BASE + dy, BRAID_BANK));
    }
    for (int y = BRAID_T; y < SCREEN_TH; y++)
        for (int dx = 0; dx < BRAID_T; dx++)
            set_map_tile(ix + dx, y,
                          WITH_BANK(inner_right ? kBraidLeft[0][dx]
                                                : kBraidRight[0][dx], BRAID_BANK));

    int in_tx = inner_right ? tx : tx + BRAID_T;
    clear_region(in_tx, BRAID_T, w - BRAID_T, SCREEN_TH - BRAID_T);
    if (!shelves) return;
    for (int i = 0; i < SHELF_COUNT; i++)
        draw_ledge(in_tx, SHELF_FIRST + i * SHELF_STEP, w - BRAID_T);
}

/* The stage the level-up blit paints where the banner was. */
void draw_dancer_stage(void) {
    for (int y = 0; y < DANCER_STAGE_TH; y++)
        for (int x = 0; x < DANCER_STAGE_TW; x++)
            set_map_tile(DANCER_STAGE_TX + x, DANCER_STAGE_TY + y,
                          WITH_BANK(kDancerStage[y][x], 1));
    /* The sixth ledge, standing in for the screen border the cartridge's
     * bottom dancer uses; see the note beside DANCER_LIFT. */
    for (int x = 0; x < DANCER_STAGE_TW; x++)
        set_map_tile(DANCER_STAGE_TX + x, DANCER_FLOOR_TY,
                      WITH_BANK(kDancerStage[3][0], 1));
}


unsigned text_len(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

void draw_text(int tx, int ty, const char *text, int bank) {
    for (int i = 0; text[i]; i++) set_map_tile(tx + i, ty, WITH_BANK(ascii_tile(text[i]), bank));
}

static void draw_number(int tx, int ty, uint32_t value, int digits, int bank) {
    for (int i = digits - 1; i >= 0; i--) {
        set_map_tile(tx + i, ty, WITH_BANK(ascii_tile((char)('0' + (value % 10))), bank));
        value /= 10;
    }
}

/* ...and the same with the leading zeros left blank. The counters keep theirs
 * — the cartridge's SCORE really does read 000000 — but the level's tally does
 * not: it prints " 1 TETRIS" and "X100=  1300", blanking everything left of
 * the first digit the way L8EA2's own staging does. */
static void draw_number_blank(int tx, int ty, uint32_t value, int digits, int bank) {
    for (int i = digits - 1; i >= 0; i--) {
        bool ink = value != 0 || i == digits - 1;
        set_map_tile(tx + i, ty,
                      WITH_BANK(ink ? ascii_tile((char)('0' + (value % 10)))
                                     : T_BLANK, bank));
        value /= 10;
    }
}

/* Paints the cartridge's own screen: the braided border, every decorative
 * tile, each with the palette the ROM's attribute table assigns it — and then
 * closes the two bare strips of rope into panels. Their inner sides land
 * exactly where the cartridge's own vertical runs already are, so the
 * playfield keeps the frame it had; the rope simply carries on round the HUD
 * instead of stopping. */
void draw_static_screen(void) {
    apply_skin(play_skin());
    if (g_session.game.coop) {
        /* COOP IS THE CARTRIDGE'S OWN SCREEN, whole. There is no reflow to do
         * and no boxes to close: screen 5 already puts a twelve-wide field in
         * the middle with a panel either side, the dancers' ledges down both,
         * and the braid running the full height as the field's walls. All the
         * port takes off it is the two columns every screen gives up to fit
         * thirty, and those come one from each end. */
        for (int ty = 0; ty < SCREEN_TH; ty++) {
            int layout_row = ty + SCREEN_COOP_FIELD_TY;
            for (int tx = 0; tx < SCREEN_TW; tx++) {
                int i = layout_row * SCREEN_COOP_W + tx;
                set_map_tile(tx, ty, WITH_BANK(kScreenCoopTiles[i],
                                                kScreenCoopPalettes[i]));
            }
        }
        /* EXCEPT THE TWO TOP CORNERS, WHICH ARE CORNERS TO NOWHERE. The rope
         * along the top of each panel ends, at the screen's own edge, in a
         * piece that turns UP — $88 on the left and $8A on the right, against
         * the plain run's $89. On the NES that is right: the rope framed the
         * whole 256x240 screen and those two are where it turned to come back
         * down the outside. Thirty columns of GBA cut that outside off, so
         * what is left is a corner with nothing round it — a little step
         * rising off the top of the screen for no reason.
         *
         * The run instead, so the rope leaves the screen the way it does in
         * the port's own boxes: straight, and off the edge. */
        for (int dy = 0; dy < BRAID_T; dy++) {
            int i = (SCREEN_COOP_FIELD_TY + dy) * SCREEN_COOP_W + 1;
            uint16_t run = WITH_BANK(kScreenCoopTiles[i], kScreenCoopPalettes[i]);
            set_map_tile(0, dy, run);
            set_map_tile(SCREEN_TW - 1, dy, run);
        }
        /* ...AND BOTH PANELS' COUNTERS COME OFF WITH IT. The two coop HUDs
         * fill different cells — the partner's panel has four and the stats
         * one has the histogram — and a counter is only overwritten by
         * another counter in the same cell, so a swap left the old HUD's
         * words standing in the cells the new one does not use. This runs
         * once per repaint, which is exactly when a swap happens. */
        clear_panel_region(COOP_L_TX, BRAID_T, COOP_PANEL_W, SCREEN_TH - BRAID_T);
        clear_panel_region(COOP_R_TX, BRAID_T, COOP_PANEL_W, SCREEN_TH - BRAID_T);
        clear_stats_layer_at(COOP_R_TX);
        set_offset_layer(STATS_SHIFT_PX);
        return;
    }
    for (int ty = 0; ty < SCREEN_TH; ty++) {
        int layout_row = ty + WINDOW_TOP;
        for (int tx = 0; tx < SCREEN_TW; tx++) {
            int i = layout_row * SCREEN_1P_W + tx;
            set_map_tile(tx, ty, WITH_BANK(kScreen1pTiles[i], kScreen1pPalettes[i]));
        }
    }
    /* BOTH PANELS ARE THE COOP SCREEN'S, and they are MIRRORS of each other:
     * rope along the top and down the side facing the board, open at the
     * bottom and at the screen's own edge. An inverted L either side of the
     * playfield, which is what the cartridge's coop screen is and what the
     * closed box on the right was not.
     *
     * Opening the right one is also what finally lets the TETRIS banner
     * inside a frame. It is six letters of three rows each, eighteen rows
     * with no padding anywhere in it; a closed box left sixteen, so the
     * banner used to take the whole column and the rope with it. Rows 2-19
     * of an open panel are eighteen exactly. */
    draw_braid_panel(BOX_L_TX, BOX_W, true, true);
    draw_braid_panel(BOX_R_TX, BOX_W, false, false);
    /* ...and ONE shelf in the right one, on the left panel's first row so the
     * two line up across the board: over it whatever that box is holding, and
     * under it the piece histogram. NOT in HUD Banner — there is nothing on
     * either side of it to separate, and a line across the middle of a
     * vertical TETRIS is a line across the middle of a vertical TETRIS. */
    if (!g_show_banner) draw_ledge(BOX_R_IN, SHELF_FIRST, BOX_IN);
    set_credit_layer(false);
    /* Back from whatever the title lent it; see set_offset_layer. */
    set_offset_layer(STATS_SHIFT_PX);
}

void draw_game_over(void) {
    for (int y = 0; y < SCREEN_1P_GAMEOVER_H; y++)
        for (int x = 0; x < SCREEN_1P_GAMEOVER_W; x++)
            set_map_tile(GAMEOVER_TX + x, GAMEOVER_TY + y,
                          WITH_BANK(kGameOverTiles[y][x], plaque_bank()));
}
/* $2165, $21E5, $2265, $22E5 — the labels — and the rows under them. */
static const uint8_t kBonusLabelTy[BONUS_CATEGORIES] = { 3, 7, 11, 15 };
static const uint8_t kBonusValueTy[BONUS_CATEGORIES] = { 4, 8, 12, 16 };
static const char *const kBonusLabel[BONUS_CATEGORIES] = {
    "SINGLES", "DOUBLES", "TRIPLES", "TETRIS"
};
static const char *const kBonusMultiplier[BONUS_CATEGORIES] = {
    "X100=", "X400=", "X900=", "2500="
};

static uint8_t g_bonus_count[BONUS_CATEGORIES];  /* this level's clears */
static uint8_t g_bonus_shown[BONUS_CATEGORIES];  /* how far the tally has got */
static uint32_t g_bonus_total;
static uint8_t g_bonus_tick;
bool g_bonus_showing;
bool g_bonus_dirty;

static int bonus_tx(void) {
    /* Coop's board is twelve wide and the panel is ten, so it sits in the
     * middle of it; everywhere else the panel IS the board. */
    return g_session.game.coop ? COOP_FIELD_TX + 1 : FIELD_TX;
}

/* One number, right-aligned against the panel's right edge the way the ROM's
 * own blit leaves room for it. */
static void draw_bonus_number(int ty, uint32_t value, int digits) {
    int tx = bonus_tx() + SCREEN_1P_BONUS_W - digits;
    draw_number_blank(tx, ty, value, digits, BANK_VALUE);
}

void draw_bonus_static(void) {
    int tx = bonus_tx();
    clear_region(tx, 0, SCREEN_1P_BONUS_W, SCREEN_TH);
    clear_panel_region(tx, 0, SCREEN_1P_BONUS_W, SCREEN_TH);
    /* THE COOP BOARD IS TWELVE WIDE AND THIS PANEL IS TEN, so a column of
     * board stood either side of it for the whole show — settled blocks
     * framing the tally. The panel is centred and cannot be widened (it is
     * the cartridge's own blit), so the two columns it does not reach are
     * wiped here. */
    if (g_session.game.coop) {
        clear_region(COOP_FIELD_TX, 0, 1, SCREEN_TH);
        clear_region(COOP_FIELD_TX + TENGEN_PF_WIDTH - 1, 0, 1, SCREEN_TH);
    }
    for (int y = 0; y < SCREEN_1P_BONUS_H; y++)
        for (int x = 0; x < SCREEN_1P_BONUS_W; x++)
            set_map_tile(tx + x, y, WITH_BANK(kBonusTiles[y][x], BANK_LABEL));
    for (int i = 0; i < BONUS_CATEGORIES; i++) {
        draw_text(tx + BONUS_LABEL_DX, kBonusLabelTy[i], kBonusLabel[i], BANK_LABEL);
        draw_text(tx, kBonusValueTy[i], kBonusMultiplier[i], BANK_LABEL);
    }
    draw_text(tx, BONUS_TOTAL_TY, "TOTAL", BANK_LABEL);
}

void draw_bonus_numbers(void) {
    int tx = bonus_tx();
    for (int i = 0; i < BONUS_CATEGORIES; i++) {
        draw_number_blank(tx + BONUS_COUNT_DX, kBonusLabelTy[i],
                           g_bonus_shown[i], 2, BANK_VALUE);
        draw_bonus_number(kBonusValueTy[i],
                           (uint32_t)g_bonus_shown[i] * TENGEN_BONUS_PER_CLEAR[i],
                           BONUS_VALUE_DIGITS);
    }
    /* THE ONE ROW THAT RIDES THE COUNTERS' LAYER, two pixels above the grid.
     * The ROM puts the total's figure at its own row 19 ($236A), and on a
     * twenty-row screen that is the last one — the number sat flush against
     * the bottom of the console with not a pixel under it. Two pixels up is
     * two pixels of air. Only this row: lifting the whole table takes the
     * same two pixels off the BONUS heading at the top, which is already
     * cropped by the window the port reads the screen through. */
    bool was = g_panel_layer;
    g_panel_layer = true;
    draw_bonus_number(BONUS_TOTAL_VALUE_TY, g_bonus_total, BONUS_VALUE_DIGITS);
    g_panel_layer = was;
}

/* The show is starting: take this level's counts and put the board away. */
void bonus_begin(void) {
    for (int i = 0; i < BONUS_CATEGORIES; i++) {
        unsigned n = g_session.game.player[g_view].clear_counts[i];
        /* One board, one tally: coop counts both people's clears into it, the
         * way the ROM's own loop walks both players (main.asm.txt:2196-2204). */
        if (g_session.game.coop)
            n += g_session.game.player[g_view ^ 1].clear_counts[i];
        if (n > 99) n = 99;   /* two columns is what the ROM prints */
        g_bonus_count[i] = (uint8_t)n;
        g_bonus_shown[i] = 0;
    }
    g_bonus_total = 0;
    g_bonus_tick = 0;
    g_bonus_showing = true;
    /* NOT DRAWN HERE. The frame a level turns over is still a playing frame:
     * draw_match runs at the end of it and draw_field paints the board back
     * over anything put on top of it. The show's own first frame draws the
     * panel — see g_bonus_dirty. */
    g_bonus_dirty = true;
}

/* ...and one frame of counting it up. Every fifth frame one more clear is
 * added, to the tally and to the score together. */
void bonus_step(void) {
    if (!g_bonus_showing) return;
    if (++g_bonus_tick < BONUS_TICK_FRAMES) return;
    g_bonus_tick = 0;

    for (int i = 0; i < BONUS_CATEGORIES; i++) {
        if (g_bonus_shown[i] >= g_bonus_count[i]) continue;
        g_bonus_shown[i]++;
        g_bonus_total += TENGEN_BONUS_PER_CLEAR[i];
        /* L8F17 adds it through the score's own routine, and in coop adds it
         * to BOTH players (`bit playMode / bpl / ldx #$01 / jsr L9A6A`,
         * main.asm.txt:2266-2271). */
        for (int slot = 0; slot < 2; slot++) {
            if (slot != g_view && !g_session.game.coop) continue;
            if (!g_session.game.player[slot].game_active) continue;
            g_session.game.player[slot].score =
                tengen_score_add(g_session.game.player[slot].score,
                                  TENGEN_BONUS_PER_CLEAR[i]);
        }
        draw_bonus_numbers();
        return;
    }
    /* Nothing left to count. */
}

/* The rival's tally is never on this screen, so it is settled in one go when
 * the show ends — the cartridge counts it up beside player 1's on its own half
 * of a two-board screen, which this port does not have. */
void bonus_end(void) {
    g_bonus_showing = false;
    /* AND THE TOTAL COMES OFF THE SCREEN WITH IT. The figure is the one row
     * of this table that rides the COUNTERS' layer, two pixels above the
     * grid, and nothing else ever writes that layer over the board: not
     * draw_field, which is the main layer, and not draw_static_screen, which
     * clears nothing. So the last total stayed printed across the bottom of
     * the playfield for the rest of the game, and you only met it on your
     * way out of the first level. draw_bonus_static clears this same
     * footprint on the way in; this is the other half of it. */
    clear_panel_region(bonus_tx(), 0, SCREEN_1P_BONUS_W, SCREEN_TH);
    /* WHATEVER THE COUNT-UP HAD NOT REACHED IS PAID NOW. The show can be cut
     * short (see the fast-forward in the level-up loop), and the bonus was
     * only ever added a clear at a time as the digits ticked: leaving early
     * left the rest of it unpaid, on the board being shown, in every mode. */
    for (int i = 0; i < BONUS_CATEGORIES; i++) {
        unsigned left = g_bonus_count[i] - g_bonus_shown[i];
        g_bonus_shown[i] = g_bonus_count[i];
        for (unsigned n = 0; n < left; n++) {
            g_bonus_total += TENGEN_BONUS_PER_CLEAR[i];
            for (int slot = 0; slot < 2; slot++) {
                if (slot != g_view && !g_session.game.coop) continue;
                if (!g_session.game.player[slot].game_active) continue;
                g_session.game.player[slot].score =
                    tengen_score_add(g_session.game.player[slot].score,
                                      TENGEN_BONUS_PER_CLEAR[i]);
            }
        }
    }
    if (g_session.game.two_player && !g_session.game.coop) {
        int other = g_view ^ 1;
        if (g_session.game.player[other].game_active) {
            g_session.game.player[other].score =
                tengen_score_add(g_session.game.player[other].score,
                                  tengen_level_bonus(&g_session.game,
                                                      (TengenPlayerSlot)other));
        }
    }
}
static char leader_letter(uint8_t index) {
    return index == 0 ? ' ' : (char)('A' + index - 1);
}

/* The tables, and which one is in play. Table 0 is the release's and the
 * rest are the prototypes', in skin order; see LEADER_TABLES. Everything
 * below still says `g_leader`, because everything below is about ONE table
 * and which one it is is decided in exactly one place. */
static LeaderEntry g_tables[LEADER_TABLES][LEADER_ENTRIES];
static int g_table;
#define g_leader (g_tables[g_table])

/* Which entry is being typed into, and which of its three letters — the
 * cartridge's $74/$75 (one per player) and the $40/$80 flags it marks the
 * initials with. See leader_submit for how two of them can be waiting at
 * once, and why they are typed one after the other. */
int g_leader_row = -1;      /* -1: nothing to type */
static int g_leader_cursor;        /* 0..2 while typing */
static uint8_t g_leader_blink;
/* Which letters have been changed, oldest first; B puts the last one
 * back. Three is the whole name, so it cannot overflow. */
static uint8_t g_leader_undo[LEADER_INITIALS];
static int g_leader_undo_n;

/* THE TABLE IS SAVED, which is the one thing the cartridge wanted and could
 * not have. Its `reset` tests a four-byte magic at $04F7 — 'L','O','G','G' —
 * and then validates every digit and initial before trusting what is in RAM
 * (main.asm.txt:5644-5668), so the scores survive a RESET and nothing else.
 * A GBA cartridge has battery-backed SRAM, so here the same magic, the same
 * validation and a checksum behind them put the table in it, and it survives
 * the power going off.
 *
 * The signature below is not decoration: an emulator or flash cart decides a
 * game has save memory by finding one of a handful of exact strings in the
 * ROM image. Without it every read comes back open bus and the table quietly
 * never persists. It is `used` so the linker cannot drop it, and it is
 * word-aligned and padded to sixteen bytes with zeros because the flash
 * carts' own patchers scan for it that way: a signature straddling a word,
 * or with whatever the linker put next to it as its tail, is the usual
 * reason a cart "does not save". */
__attribute__((used, retain, section(".rodata"), aligned(4)))
static const char kSaveSignature[16] = "SRAM_V113";
static const char kSaveMagic[SAVE_MAGIC_LEN] = { 'L', 'O', 'G', 'G' };

static uint8_t leader_checksum_of(int table) {
    uint8_t sum = 0xA5;
    for (int i = 0; i < LEADER_ENTRIES; i++) {
        const LeaderEntry *e = &g_tables[table][i];
        for (int b = 0; b < 4; b++) sum = (uint8_t)(sum + (e->score >> (8 * b)));
        sum = (uint8_t)(sum + (e->lines & 0xFF) + (e->lines >> 8));
        for (int c = 0; c < LEADER_INITIALS; c++) sum = (uint8_t)(sum + e->initials[c]);
        sum = (uint8_t)(sum * 3 + 1);
    }
    return sum;
}

/* Where a table's checksum byte lives: the release's is the one the save
 * always had, and the rest are past all the data. See LEADER_TABLES. */
static unsigned leader_sum_off(int table) {
    return table == 0 ? SAVE_SUM_OFF : SAVE_SUMS_OFF + (unsigned)(table - 1);
}

static void leader_save_table(int table) {
    for (int i = 0; i < LEADER_ENTRIES; i++) {
        const LeaderEntry *e = &g_tables[table][i];
        unsigned at = SAVE_TABLE_OFF(table) + (unsigned)i * SAVE_ENTRY_BYTES;
        for (int b = 0; b < 4; b++)
            sram_write(at + (unsigned)b, (unsigned char)(e->score >> (8 * b)));
        sram_write(at + 4, (unsigned char)(e->lines & 0xFF));
        sram_write(at + 5, (unsigned char)(e->lines >> 8));
        for (int c = 0; c < LEADER_INITIALS; c++)
            sram_write(at + 6 + (unsigned)c, e->initials[c]);
    }
    sram_write(leader_sum_off(table), leader_checksum_of(table));
}

static void leader_save(void) {
    for (int i = 0; i < SAVE_MAGIC_LEN; i++)
        sram_write((unsigned)i, (unsigned char)kSaveMagic[i]);
    /* ALL OF THEM, not just the one in play. A table nobody touched costs
     * fifteen entries of writes and keeps the save whole; writing one at a
     * time meant a console that had never played a prototype carried a
     * checksum for a table that was never written. */
    for (int t = 0; t < LEADER_TABLES; t++) leader_save_table(t);
}

/* ...and reading it back, with the cartridge's own suspicion: the magic, the
 * checksum, and then every field checked for range before any of it is
 * believed. Returns false if what is there is not a table, which is what a
 * console that has never run this game looks like. */
static bool leader_load_table(int table) {
    LeaderEntry got[LEADER_ENTRIES];
    for (int i = 0; i < LEADER_ENTRIES; i++) {
        unsigned at = SAVE_TABLE_OFF(table) + (unsigned)i * SAVE_ENTRY_BYTES;
        uint32_t score = 0;
        for (int b = 0; b < 4; b++)
            score |= (uint32_t)sram_read(at + (unsigned)b) << (8 * b);
        got[i].score = score;
        got[i].lines = (uint16_t)(sram_read(at + 4) | (sram_read(at + 5) << 8));
        for (int c = 0; c < LEADER_INITIALS; c++)
            got[i].initials[c] = sram_read(at + 6 + (unsigned)c);

        if (got[i].score > 999999 || got[i].lines > 999) return false;
        for (int c = 0; c < LEADER_INITIALS; c++)
            if (got[i].initials[c] >= LEADER_LETTERS) return false;
        /* ...and it has to be sorted, or it is not this table. */
        if (i && got[i].score > got[i - 1].score) return false;
    }

    LeaderEntry keep[LEADER_ENTRIES];
    for (int i = 0; i < LEADER_ENTRIES; i++) {
        keep[i] = g_tables[table][i];
        g_tables[table][i] = got[i];
    }
    if (leader_checksum_of(table) != sram_read(leader_sum_off(table))) {
        for (int i = 0; i < LEADER_ENTRIES; i++) g_tables[table][i] = keep[i];
        return false;
    }
    return true;
}

bool leader_load(void) {
    for (int i = 0; i < SAVE_MAGIC_LEN; i++)
        if (sram_read((unsigned)i) != (unsigned char)kSaveMagic[i]) return false;

    /* THE RELEASE'S TABLE DECIDES WHETHER THERE IS A SAVE AT ALL — it is the
     * one that has always been at these offsets. The prototypes' are read
     * beside it and each simply keeps the cartridge's fifteen if its own
     * bytes do not add up, which is what a build nobody has played looks
     * like on a console that was saving before they existed. */
    if (!leader_load_table(0)) return false;
    for (int t = 1; t < LEADER_TABLES; t++)
        if (!leader_load_table(t)) leader_reset_table(t);
    g_high_score = g_leader[0].score;
    return true;
}

/* @resetHighScores (main.asm.txt:5670-5700). Every digit '0' and every initial
 * 'A', and then the fifteen scores are built by counting DOWN from entry 0:
 * `tya; adc #$33`, which lands on 17000 for the first and 3000 for the last in
 * steps of a thousand. */
void leader_reset_table(int table) {
    for (int i = 0; i < LEADER_ENTRIES; i++) {
        g_tables[table][i].score = (uint32_t)(17000 - i * 1000);
        g_tables[table][i].lines = 0;
        for (int c = 0; c < LEADER_INITIALS; c++)
            g_tables[table][i].initials[c] = 1; /* 'A' */
    }
}

void leader_reset(void) {
    for (int t = 0; t < LEADER_TABLES; t++) leader_reset_table(t);
    g_high_score = g_leader[0].score;
}

/* WHICH BUILD'S TABLE IS IN PLAY, by the skin: -1 is the release and 0 and up
 * are the prototypes in their title order. Called where the skin is settled,
 * not read every frame, because switching is also what refreshes the HIGH
 * SCORE the HUD and the menus print. */
void leader_use_table(int skin) {
    int table = skin + 1;
    if (table < 0 || table >= LEADER_TABLES) table = 0;
    g_table = table;
    g_high_score = g_leader[0].score;
}

/* L81FF (main.asm.txt:342-378): walk the table from the BOTTOM up while the
 * new score still beats what is there, which lands on the first row it does
 * not — so an equal score goes UNDER the one already on the board. Everything
 * below that row shifts down one and the last falls off (L829A, :414-435).
 *
 * Returns the row it landed on, or -1 if the score did not make the table. */
static int leader_insert(uint32_t score, uint32_t lines) {
    int row = LEADER_ENTRIES;
    while (row > 0 && score > g_leader[row - 1].score) row--;
    if (row >= LEADER_ENTRIES) return -1;

    for (int i = LEADER_ENTRIES - 1; i > row; i--) g_leader[i] = g_leader[i - 1];
    g_leader[row].score = score;
    /* main.asm.txt:364-377: a line count that has reached its thousands digit
     * is stored as "999" — the column is three wide and the ROM says so. */
    g_leader[row].lines = (uint16_t)(lines > 999 ? 999 : lines);
    for (int c = 0; c < LEADER_INITIALS; c++) g_leader[row].initials[c] = 1; /* 'A' */
    g_high_score = g_leader[0].score;
    return row;
}

/* THE PAGE WEARS bgPalette1, which is the menu's: initializeLeaderboard ends
 * with `lda #$01 / jsr updatePalette` (main.asm.txt:3059-3060), and the set at
 * index 1 is bgPalette1 (:5297). Every cell's bank within it comes off the
 * screen's own attribute table, the runtime-written ones included — which is
 * why the text below reads its bank out of the same array as the art rather
 * than naming one. */
static int leader_bank(int tx, int ty) {
    return PAL_MENU_BASE + kScreenLeaderPalettes[ty * SCREEN_LEADER_W + tx];
}

/* The page itself, and then one row of it. */
void draw_leader_row(int row) {
    int ty = SCREEN_LEADER_FIRST_TY + row;
    const LeaderEntry *e = &g_leader[row];
    for (int c = 0; c < LEADER_INITIALS; c++) {
        char ch = leader_letter(e->initials[c]);
        /* The letter being typed blinks, the way the cartridge blinks it
         * (L932A, main.asm.txt:2855-2890). */
        if (row == g_leader_row && c == g_leader_cursor && (g_leader_blink & 0x10))
            ch = ' ';
        int tx = SCREEN_LEADER_NAME_TX + c;
        set_map_tile(tx, ty, WITH_BANK(ascii_tile(ch), leader_bank(tx, ty)));
    }
    draw_number(SCREEN_LEADER_SCORE_TX, ty, e->score, 6,
                 leader_bank(SCREEN_LEADER_SCORE_TX, ty));
    draw_number(SCREEN_LEADER_LINES_TX, ty, e->lines, 3,
                 leader_bank(SCREEN_LEADER_LINES_TX, ty));
}

void draw_leaderboard(void) {
    /* ...AND SO DOES THE HIGH SCORES PAGE, which was the visible half of this:
     * it never asked for a skin either way, so after a skinned game it came up
     * with whatever the board had left in the slots — the six the skin used to
     * cover in the prototype's fret and the other eighteen still the blue
     * braid. Half-changed. */
    apply_skin(front_skin());
    for (int ty = 0; ty < SCREEN_LEADER_H_TILES; ty++)
        for (int tx = 0; tx < SCREEN_LEADER_W; tx++) {
            int i = ty * SCREEN_LEADER_W + tx;
            set_map_tile(tx, ty, WITH_BANK(kScreenLeaderTiles[i],
                                            leader_bank(tx, ty)));
        }
    for (int row = 0; row < LEADER_ENTRIES; row++) draw_leader_row(row);
}

/* WHOSE SCORES GO ON THE BOARD. L81DD (main.asm.txt:315-339) is called with
 * the player who just topped out, and on the negative playMode — coop — it
 * runs for the other one too, because one board is two players' game. What it
 * refuses is the COMPUTER: L81EC returns without doing anything for player 2
 * once menuGameMode has reached VERSUS. So a machine never takes a place on
 * the table, and in coop both people do.
 *
 * Up to two entries can therefore be waiting to be typed into, which is what
 * the cartridge's $74 and $75 are for. They are typed one after the other
 * here, oldest first. */
static int g_leader_queue[2];
static int g_leader_queued;

void leader_submit(void) {
    g_leader_row = -1;
    g_leader_queued = 0;
    g_leader_cursor = 0;

    int slots[2];
    int n = 0;
    slots[n++] = g_view;
    if (g_session.game.coop && !g_ai_active) slots[n++] = g_view ^ 1;

    for (int i = 0; i < n; i++) {
        const TengenPlayerState *p = &g_session.game.player[slots[i]];
        int row = leader_insert(p->score, p->lines);
        if (row < 0) continue;
        /* An entry that lands above one already queued pushes it down a row,
         * exactly as it pushes every other entry down. */
        for (int q = 0; q < g_leader_queued; q++)
            if (g_leader_queue[q] >= row) g_leader_queue[q]++;
        g_leader_queue[g_leader_queued++] = row;
    }
    if (g_leader_queued) {
        g_leader_row = g_leader_queue[0];
        g_leader_blink = 0;
        g_leader_undo_n = 0;
    }
    leader_save();
}

/* One letter along the alphabet, wrapping both ways — L92BA
 * (main.asm.txt:2789-2799): $FF comes back as $1A and $1B comes back as 0, so
 * the ring is the space and the twenty-six letters. */
static void leader_letter_step(int delta) {
    uint8_t *v = &g_leader[g_leader_row].initials[g_leader_cursor];
    int next = (int)*v + delta;
    if (next < 0) next = LEADER_LETTERS - 1;
    if (next >= LEADER_LETTERS) next = 0;
    *v = (uint8_t)next;

    /* ...and remember that this is the place to put back. See leader_type's
     * UNDO. Touching a letter twice does not stack: the position moves to the
     * top of the list rather than being pushed onto it again. */
    for (int i = 0; i < g_leader_undo_n; i++) {
        if (g_leader_undo[i] != g_leader_cursor) continue;
        for (int j = i; j + 1 < g_leader_undo_n; j++)
            g_leader_undo[j] = g_leader_undo[j + 1];
        g_leader_undo_n--;
        break;
    }
    if (g_leader_undo_n < LEADER_INITIALS)
        g_leader_undo[g_leader_undo_n++] = (uint8_t)g_leader_cursor;
}

/* ONE FRAME OF TYPING, and the controls are NOT the cartridge's.
 *
 * Its own are Left and Right to walk the alphabet, A or B to take the letter
 * and move on, and SELECT — undocumented, unsignposted — to go back to the
 * first one (L9234, main.asm.txt:2709-2762). That is a menu you can only get
 * out of by finishing, where the button that fixes a mistake is one nobody
 * would find. So:
 *
 *   UP / DOWN     the letter
 *   LEFT / RIGHT  which letter, and SELECT walks the three round
 *   A / START     take the name
 *   B             undo the last letter changed: back to A, cursor onto it
 *
 * ...and B with nothing to undo is an ALARM rather than nothing happening,
 * because a button that is silent is a button you cannot tell from a broken
 * one. SOUND_ALARM is the cartridge's own, and one it never plays.
 *
 * Returns true while there is still something to type. THE BUTTON THAT TAKES
 * THE NAME DOES NOT ALSO LEAVE THE PAGE: A and START mean "done" here and
 * "away with you" out there, so the frame that spends one reports itself as
 * still typing and the caller's clock starts on the next. */
bool leader_type(uint8_t held, uint8_t pressed) {
    static uint8_t das_u, das_d;
    bool was_typing = g_leader_row >= 0;
    if (!was_typing) return false;
    g_leader_blink++;

    if (pressed & TENGEN_BTN_UP) { leader_letter_step(1); das_u = 0; }
    else if (held & TENGEN_BTN_UP) {
        if (++das_u >= LEADER_DAS) { leader_letter_step(1); das_u = 0; }
    } else das_u = 0;

    if (pressed & TENGEN_BTN_DOWN) { leader_letter_step(-1); das_d = 0; }
    else if (held & TENGEN_BTN_DOWN) {
        if (++das_d >= LEADER_DAS) { leader_letter_step(-1); das_d = 0; }
    } else das_d = 0;

    if ((pressed & TENGEN_BTN_LEFT) && g_leader_cursor > 0) g_leader_cursor--;
    if ((pressed & TENGEN_BTN_RIGHT) && g_leader_cursor < LEADER_INITIALS - 1)
        g_leader_cursor++;
    /* SELECT as a cursor button is the cartridge's own idiom — it is what
     * moves the cursor on the settings screen too — and here it walks the
     * three round rather than stopping at the end. */
    if (pressed & TENGEN_BTN_SELECT)
        g_leader_cursor = (g_leader_cursor + 1) % LEADER_INITIALS;

    if (pressed & TENGEN_BTN_B) {
        if (g_leader_undo_n == 0) {
            nes_audio_play(NES_SOUND_ALARM);
        } else {
            int p = g_leader_undo[--g_leader_undo_n];
            g_leader[g_leader_row].initials[p] = 1;   /* 'A' */
            g_leader_cursor = p;
            screen_blip();
        }
    }

    /* A WALKS, START FINISHES. A used to mean "done" wherever the cursor
     * was, which put the end of the name one button away from its first
     * letter: press A to accept the letter you just chose, the way every
     * other entry field in the world works, and the whole name was taken
     * with two of its three letters still an A. So A moves to the next
     * letter and only finishes on the last one, and START is the way out
     * from anywhere — which is what it already meant here and everywhere
     * else on this page. B still puts a letter back. */
    if ((pressed & TENGEN_BTN_A) && !(pressed & TENGEN_BTN_START) &&
        g_leader_cursor < LEADER_INITIALS - 1) {
        g_leader_cursor++;
        cursor_blip();
    } else if (pressed & (TENGEN_BTN_A | TENGEN_BTN_START)) {
        screen_blip();
        g_leader_cursor = 0;
        g_leader_undo_n = 0;
        /* Done with this one; the next person who made the table, if there is
         * one, types theirs. */
        for (int q = 1; q < g_leader_queued; q++)
            g_leader_queue[q - 1] = g_leader_queue[q];
        int finished = g_leader_row;
        if (--g_leader_queued > 0) g_leader_row = g_leader_queue[0];
        else g_leader_row = -1;
        /* A name is not a name until it is finished, so this is where it is
         * written down — and THE ROW IS REDRAWN ONE LAST TIME, unblinking.
         * Without that the letter under the cursor kept whichever half of the
         * blink it was in when the button landed, and a name taken on the
         * wrong frame lost its last letter for good: nothing redraws a row
         * that is no longer being typed into. */
        leader_save();
        draw_leader_row(finished);
    }
    return was_typing;
}

/* CENTRED IN ITS CELL, and the last three pixels of it come from the same
 * place the statistics' do.
 *
 * The orientation bitmaps put each piece where the PLAYFIELD wants it — an O
 * at columns 0-1, an I across all four — so drawing them at their bitmap
 * column left everything but the I against the left wall of a box that is the
 * port's own, under a NEXT that is centred. Squaring the bounding box up in
 * the four-tile cell fixes the two-tile part of that.
 *
 * What it cannot fix is the half tile. The block art has a one-pixel inset on
 * its left, so a piece w tiles wide is 8w-1 pixels of ink, and centring that
 * in the panel's 64 wants its first tile at (65-8w)/16 - an EVEN number of
 * tiles for an even w, and half a tile out for an odd one. Two tiles wide and
 * four tiles wide land within half a pixel of centre; three tiles wide - the
 * T, J, L, S and Z, so five pieces of seven - lands three and a half pixels
 * left, which is what "alineadas a la izquierda" still was after the columns
 * were right.
 *
 * Those pieces are drawn on the STATISTICS LAYER instead, which is already
 * scrolled three pixels for its own reasons (see SCREENBLOCK_STATS) and puts
 * them half a pixel the other side of centre. No new layer, no new art, and
 * the even widths stay on the main one where they are already right. */
static void draw_next_piece(int tx, int ty, int slot, int bank) {
    clear_region(tx, ty, NEXT_CELL_W, 3);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < NEXT_CELL_W; x++) set_stats_tile(tx + x, ty + y, T_BLANK);

    TengenTetromino next = g_session.game.player[slot].piece.next;
    if (next <= TT_NONE || next >= TENGEN_TETROMINO_COUNT) return;
    set_bank_from_piece(bank, next);

    int first = NEXT_CELL_W, last = -1;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (tengen_piece_occupies(next, 0, r, c)) {
                if (c < first) first = c;
                if (c > last) last = c;
            }
    if (last < first) return;
    int width = last - first + 1;
    int shift = (NEXT_CELL_W - width) / 2 - first;
    bool offset_layer = (width & 1) != 0;

    /* Drawn from the same orientation bitmap and tile table the game logic
     * uses, so the preview cannot drift out of sync with what spawns. */
    int occupied = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!tengen_piece_occupies(next, 0, r, c)) continue;
            uint8_t tile = piece_cell_tile(next, 0, occupied);
            occupied++;
            if (r >= 3) continue;
            uint16_t entry = WITH_BANK(tile, bank);
            if (offset_layer) set_stats_tile(tx + c + shift, ty + r, entry);
            else              set_map_tile(tx + c + shift, ty + r, entry);
        }
    }
}

/* WHAT THE RIGHT-HAND BOX SHOWS.
 *
 * The cartridge's play screen has a vertical TETRIS banner between its two
 * halves, and thirty columns cannot hold that AND two boxes wide enough to be
 * useful. So it is a choice the player makes: L+R together — the two buttons
 * a NES pad never had and this game therefore never uses — swaps the right
 * box between the piece histogram and the banner.
 *
 * IT STARTS ON THE BANNER, because that is the cartridge's own screen: the
 * vertical TETRIS is what a player who has seen this game remembers of it,
 * and the histogram is the thing you go and ask for. It is also the harder
 * way to play — no piece counts — which is why the level-up troupe is
 * reserved for it (see g_idle_show). */
/* (Declared up with draw_static_screen, which has to know which shape the
 * right panel is before the panel itself is drawn.) */
/* Frames the play screen has been up, for the idle cossack's slow sway. */
int g_idle_frame;
/* True while the level-up show owns the right column; declared here because
 * the panel has to know not to put its own cossack up against the six. */
bool g_dancer_active;
/* ...and its clock, for the same reason: in HUD STATS the panel's own cossack
 * dances the show, and he has to keep the show's time. */
int g_dancer_elapsed;     /* frames since the show started, for the poses */

static void draw_banner(void) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    /* THE PROTOTYPE'S OWN LETTERS, out of their own window. Not the release's
     * slots recoloured: these builds draw each letter with tiles of their own
     * where the release reuses one between letters, so the art cannot be
     * handed over slot by slot. It goes in the FRAME's palette bank, which is
     * where all three dumps put their banner. See upload_skin_banner. */
    int skin = play_skin();
    if (skin >= 0) {
        for (int y = 0; y < SKIN_BANNER_H; y++)
            for (int x = 0; x < SKIN_BANNER_W; x++)
                set_map_tile(BANNER_TX + x, BANNER_TY + y,
                              WITH_BANK(SKIN_BANNER_BASE +
                                         kSkinBannerTiles[skin][y * SKIN_BANNER_W + x],
                                         BRAID_BANK));
        return;
    }
#endif
    for (int y = 0; y < SCREEN_1P_BANNER_H; y++)
        for (int x = 0; x < SCREEN_1P_BANNER_W; x++)
            set_map_tile(BANNER_TX + x, BANNER_TY + y,
                          WITH_BANK(kBannerTiles[y][x], kBannerBanks[y][x]));
}

/* The WHOLE right interior, not just the bars: NEXT's preview borrows this
 * layer too when its piece is an odd number of tiles wide, and it sits above
 * the statistics. Anything that takes the panel over has to take both. */
/* Both of the right box's borrowed layers: the histogram's, and the offset
 * one, which may still be holding an odd-width preview from a moment ago. */
void clear_stats_layer_at(int tx) {
    for (int y = BOX_TOP_IN; y <= BOX_BOT_IN; y++)
        for (int x = 0; x < BOX_IN; x++) {
            set_stats_tile(tx + x, y, T_BLANK);
            set_histogram_tile(tx + x, y, T_BLANK);
        }
}

void clear_stats_layer(void) { clear_stats_layer_at(STATS_TX); }

/* A PROTOTYPE COUNTS ITS PIECES DIFFERENTLY, and it is worth saying how,
 * because it is one of the clearer differences between the builds.
 *
 * The release draws one bar graphic for all seven columns and puts a strip of
 * little tetromino ICONS under them to say which column is whose. A prototype
 * draws each piece's bar in that PIECE'S OWN BLOCK PATTERN — solid, striped,
 * hatched — and has no icon strip at all: what the bar is made of is what
 * names it. Seven runs of eight steps, in the pieces' own order, uploaded to
 * SKIN_STATS_BASE and drawn in the blocks' bank.
 *
 * The ARITHMETIC does not change. Which step of the run a bar is on, and when
 * it climbs a row, is the cartridge's own rule in both cases; only the
 * pictures the rule picks are different. */
static void draw_stats(const TengenPlayerState *p, int base_tx) {
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
    int skin = play_skin();
#else
    int skin = -1;
#endif
    /* THE ICON STRIP'S TWO ROWS GO TO THE BARS under a skin, which is the
     * only sensible thing to do with them: a prototype's bars stand on the
     * floor of the box, and leaving the strip's rows blank would have them
     * hanging two rows above it with nothing underneath. So the bars start
     * one row lower and get two rows taller. */
    int floor_ty = skin < 0 ? STATS_ICON_TY - 1 : STATS_ICON_TY + 1;
    int bar_rows = floor_ty - STATS_TOP_TY + 1;

    for (int i = 0; i < SCREEN_1P_STATS_PIECES; i++) {
        int tx = base_tx + i;
        if (skin < 0) {
            /* Each icon in the palette the ROM's attribute table gives it: the
             * I has its own, T/O/J/L share one, S and Z share another. */
            int icon_bank = kStatsIconBanks[i];
            set_histogram_tile(tx, STATS_ICON_TY,
                                WITH_BANK(kStatsIcons[0][i], icon_bank));
            set_histogram_tile(tx, STATS_ICON_TY + 1,
                                WITH_BANK(kStatsIcons[1][i], icon_bank));
        }

        uint16_t n = p->piece_stats[TT_I + i];
        int full = n / 8;
        int part = n % 8;
        if (full > bar_rows) { full = bar_rows; part = 0; }

        for (int r = 0; r < bar_rows; r++) {
            int ty = floor_ty - r;
            uint16_t tile = T_BLANK;
            int bank = BANK_STATS;
#if SCREEN_PROTO_AVAILABLE && SCREEN_SKIN_PLAY
            if (skin >= 0) {
                unsigned run = SKIN_STATS_BASE + (unsigned)i * SKIN_STATS_STEPS;
                bank = SKIN_STATS_BANK;
                if (r < full) tile = (uint16_t)(run + SKIN_STATS_STEPS - 1);
                else if (r == full && part) tile = (uint16_t)(run + part - 1);
                set_histogram_tile(tx, ty, WITH_BANK(tile, bank));
                continue;
            }
#endif
            if (r < full) tile = STATS_BAR_FULL;
            else if (r == full && part) tile = SCREEN_1P_STATS_BAR_TILE + part - 1;
            set_histogram_tile(tx, ty, WITH_BANK(tile, bank));
        }
    }
}

/* A label on one row and its value on the next, both inside the left box. */
/* THE LABELS COME FROM A CLEANED COPY, not from the cartridge's tiles direct.
 * SCORE's first and last tiles carry a piece of the header grid's VERTICAL
 * line, and this layout has no vertical grid for it to belong to, so it read
 * as a grey stub at the start and end of every word. read_hud_labels strips
 * it — exactly, because the grid is colour 3 and the lettering colour 1 — and
 * the rule it was a fragment of goes back where the cartridge puts it, between
 * the rows. */
static void draw_label(int tx, int ty, int first, int count) {
    for (int i = 0; i < count; i++)
        set_map_tile(tx + i, ty,
                      WITH_BANK(HUD_LABEL_TILE_BASE + first + i, BANK_LABEL));
}

/* NO LEADING ZEROS, which is the cartridge's own and was missed here for a
 * long time: renderStatistics walks each counter's digits from the top and,
 * while it finds a '0', shortens the run AND advances the write position
 * (main.asm.txt:4067-4082) — so the number keeps its place and the zeros in
 * front of it are simply never drawn. A screen of 000000 / 0000 / 00 is not
 * what this game looks like; it looks like 8294 / 30 / 2.
 *
 * It is player 1's counters that are pushed right like this. Player 2's are
 * not (`lda generalCounter38 / lsr a / bcs @noIncrement` — the odd indices are
 * its own), and neither is coop's, which is the one case this port would have
 * to mirror if its coop panel were the cartridge's shape. It is not: coop's
 * counters are centred in their own panels here, so they take the same
 * treatment as everything else. */
void draw_counter(int ty, int label_first, int label_count,
                          uint32_t value, int digits, int value_indent) {
    draw_label(BOX_L_IN, ty, label_first, label_count);
    clear_region(BOX_L_IN, ty + 1, BOX_L_W, 1);
    draw_number_blank(BOX_L_IN + value_indent, ty + 1, value, digits, BANK_VALUE);
}

/* NEXT, IN ITS OWN COMPARTMENT AND CENTRED IN IT.
 *
 * A ROW OF AIR BETWEEN THE WORD AND THE PIECE, and it is not decoration. At
 * orientation 0 every one of the seven pieces is two rows tall
 * (kOrientationBitmap's second byte is $00 for all of them) and the art fills
 * its tiles to the top edge, so a piece drawn straight under the label has
 * the label's baseline and the block's first row of pixels on consecutive
 * scanlines — the word and the piece touching, which is what "Next y la pieza
 * se solapan" is once you are looking for it. One blank row is eight pixels
 * and it makes the two read as two things.
 *
 * Four rows of content in the compartment's six then centres exactly, which
 * three never did.
 *
 * `tx` is the panel's INTERIOR left column, so both the word and the piece
 * are centred in the same columns the shelves span rather than measured off
 * the indented content column, which is what left them sitting left of
 * centre; `w` is that interior's width, because the coop panel's is seven
 * where the port's own boxes are eight. */
static void draw_next_label_and_piece(int tx, int ty, int w, int slot, int bank) {
    int label_tx = tx + (w - HUD_LABEL_NEXT_W) / 2;
    for (int i = 0; i < HUD_LABEL_NEXT_W; i++)
        set_map_tile(label_tx + i, ty,
                      WITH_BANK(HUD_LABEL_TILE_BASE + 18 + i, BANK_LABEL));
    draw_next_piece(tx + (w - NEXT_CELL_W) / 2, ty + 2, slot, bank);
}

/* One counter in a seven-column panel: label, value, and no rule — the ledge
 * under it is the cartridge's own. */
static void draw_coop_counter(int tx, int ty, int label_first, int label_count,
                               uint32_t value, int digits) {
    draw_label(tx + (COOP_PANEL_W - label_count) / 2, ty, label_first, label_count);
    clear_region(tx, ty + 1, COOP_PANEL_W, 1);
    draw_number_blank(tx + (COOP_PANEL_W - digits) / 2, ty + 1, value, digits, BANK_VALUE);
}

/* The same, for HIGH — which the cartridge sets in plain ASCII rather than in
 * the drawn lettering the other three use, on its coop screen as on its 1P
 * one. */
static void draw_coop_text_counter(int tx, int ty, const char *label,
                                    uint32_t value, int digits) {
    clear_region(tx, ty, COOP_PANEL_W, 2);
    draw_text(tx + (COOP_PANEL_W - (int)text_len(label)) / 2, ty, label, BANK_LABEL);
    draw_number_blank(tx + (COOP_PANEL_W - digits) / 2, ty + 1, value, digits, BANK_VALUE);
}

/* ON THE MAIN LAYER, like the other two HUDs, and that is the whole of why
 * "NEXT y la pieza se solapan en ocasiones" — in coop and only in coop, and
 * only for five pieces of seven. The label rides whatever layer the panel is
 * on; the PIECE picks its own by width, because a preview an odd number of
 * tiles wide is centred with the offset layer's three pixels (see
 * draw_next_piece). Those two layers do not have the same vertical scroll, so
 * the piece landed two pixels off its own word — which with one pixel between
 * them is the word and the piece touching. Both on the grid, and there is no
 * second scroll to disagree with. */
static void draw_coop_next(int tx, int slot, int bank) {
    bool was = g_panel_layer;
    g_panel_layer = false;
    clear_both(tx, BRAID_T, COOP_PANEL_W, COOP_LEDGE_FIRST - BRAID_T);
    clear_panel_region(tx, BRAID_T, COOP_PANEL_W, COOP_LEDGE_FIRST - BRAID_T);
    draw_next_label_and_piece(tx, COOP_NEXT_TY, COOP_PANEL_W, slot, bank);
    g_panel_layer = was;
}

/* Both boards' numbers added up, which is the only figure a shared board
 * cannot read off either panel. Clamped where the counters are: six digits of
 * score and four of lines is what every other counter on this screen shows,
 * and a number wider than its cell is worse than a number that stops. */
static uint32_t coop_total_score(void) {
    uint32_t t = g_session.game.player[0].score + g_session.game.player[1].score;
    return t > 999999 ? 999999 : t;
}
static uint32_t coop_total_lines(void) {
    uint32_t t = g_session.game.player[0].lines + g_session.game.player[1].lines;
    return t > 9999 ? 9999 : t;
}

/* THE COOP PANELS, ONE PLAYER EACH.
 *
 * The cartridge's coop screen prints LEVEL on the left and HIGH and SCORE on
 * the right and nothing else, because its own coop has nothing else to say:
 * one board, one score between the two of you. This port's does not work
 * that way — the core keeps a score, a line count and a preview PER PLAYER
 * on the shared board, and every one of those was being thrown away. The
 * left panel is yours and the right one is theirs, laid into the four cells
 * the cartridge's own ledges make on each side:
 *
 *      left                    right
 *      NEXT   (yours)          NEXT   (theirs)
 *      SCORE  (yours)          SCORE  (theirs)
 *      LINES  (yours)          LINES  (theirs)
 *      LEVEL  (shared)         HIGH   (this build's table)
 *      -- free --              -- free --
 *
 * LEVEL IS ONE FIELD because there is one level: tengen_step hands the new
 * one to the partner on a shared board, the way the cartridge's own level-up
 * routine stores it twice. HIGH is the table the match is being played into,
 * which on a cable is each console's own — see LEADER_TABLES.
 *
 * TWO PREVIEWS, NOT ONE, and that is worth setting down because this file
 * used to say the opposite. The two lookahead randomisers are seeded from
 * the same number and step once per spawn each, so the SEQUENCES are
 * identical — but the two players are not at the same POINT in them unless
 * they have taken exactly as many pieces as each other, which over a game
 * they never do. Measured on a WITH COMPUTER board: the two NEXT pieces
 * differ on 1528 frames out of 1800. It is their piece, not a copy of ours.
 *
 * AND THE LAST CELL EACH IS THE CHEAT'S. Coop plays more like a race than
 * like a team, so the two figures nobody can work out from the panels — the
 * board's total lines and total score — go there once the chord has been
 * rung, and stand empty until it has. */
/* THE OTHER COOP HUD, and it is a difficulty setting. Against the COMPUTER
 * the partner's panel tells you what the machine is holding and how it is
 * doing, which is information the cartridge would never have given you; this
 * takes it away again. The left panel goes back to the shape the 1P one has
 * — NEXT, two counters, LEVEL, HIGH — except that the two counters are the
 * BOARD'S totals rather than yours, because on one field your own score
 * without your partner's says less than the pair does. The right panel is
 * the histogram and the cossack over it, exactly as in 1P; it is seven
 * columns wide and so is the panel, which is the whole reason this fits.
 *
 * NOT OVER THE CABLE. There the partner is a person who chose to play with
 * you, and hiding their board from you is not a difficulty setting, it is
 * just less game. See the swap's own condition in main.c. */
static void draw_coop_stats_panel(const TengenPlayerState *me) {
    draw_coop_next(COOP_L_TX, g_view, PAL_NEXT_BANK);

    g_panel_layer = true;
    draw_coop_text_counter(COOP_L_TX, COOP_COUNTER_TY, "T.SCORE",
                            coop_total_score(), 6);
    draw_coop_text_counter(COOP_L_TX, COOP_LOWER_TY, "T.LINES",
                            coop_total_lines(), 4);
    draw_coop_counter(COOP_L_TX, COOP_THIRD_TY, HUD_LABEL_LEVEL, me->level, 2);
    draw_coop_text_counter(COOP_L_TX, COOP_TOTAL_TY, "HIGH", g_high_score, 6);
    g_panel_layer = false;

    /* The cossack gets the right panel's tall compartment back, on the same
     * terms he has in HUD Stats: he stops when the board dies and when the
     * plaque goes up, and he stands down for the level-up show because that
     * show has the real eight of them out on these very ledges. */
    if (g_dancer_active) {
        hide_idle_cossack();
    } else {
        bool alive = me->game_active && !g_session.game.paused;
        draw_idle_cossack(g_idle_frame, alive, COOP_R_TX, BRAID_T,
                           COOP_PANEL_W, COOP_LEDGE_FIRST - BRAID_T);
        if (alive) g_idle_frame++;
    }
    /* AND THE THREE LEDGES UNDER HIS FEET COME OFF. The cartridge rules this
     * panel every three rows and the histogram stands on all of it, so the
     * bars grew out of three blue lines that had nothing to do with them.
     * The first ledge stays: it is the floor of the cossack's compartment
     * and the top of the histogram's, which is what it is for in HUD Stats
     * too. Cheap and idempotent, so it runs with the panel rather than
     * being another thing a repaint has to remember; going back to the other
     * HUD repaints the screen and the cartridge's own ledges come with it. */
    for (int i = 1; i < 4; i++)
        clear_region(COOP_R_TX, COOP_LEDGE_FIRST + i * 3, COOP_PANEL_W, 1);
    draw_stats(me, COOP_R_TX);
}

static void draw_coop_panel(void) {
    const TengenPlayerState *me = &g_session.game.player[g_view];
    const TengenPlayerState *them = &g_session.game.player[g_view ^ 1];
    /* EITHER OF THEM CAN BEAT THE TABLE, and both are offered it when the
     * board dies — see leader_submit. */
    if (me->score > g_high_score) g_high_score = me->score;
    if (them->score > g_high_score) g_high_score = them->score;

    if (!g_show_banner) { draw_coop_stats_panel(me); return; }

    draw_coop_next(COOP_L_TX, g_view, PAL_NEXT_BANK);
    draw_coop_next(COOP_R_TX, g_view ^ 1, PAL_NEXT2_BANK);

    g_panel_layer = true;
    draw_coop_counter(COOP_L_TX, COOP_COUNTER_TY, HUD_LABEL_SCORE, me->score, 6);
    draw_coop_counter(COOP_L_TX, COOP_LOWER_TY, HUD_LABEL_LINES, me->lines, 4);
    draw_coop_counter(COOP_L_TX, COOP_THIRD_TY, HUD_LABEL_LEVEL, me->level, 2);

    draw_coop_counter(COOP_R_TX, COOP_COUNTER_TY, HUD_LABEL_SCORE, them->score, 6);
    draw_coop_counter(COOP_R_TX, COOP_LOWER_TY, HUD_LABEL_LINES, them->lines, 4);
    draw_coop_text_counter(COOP_R_TX, COOP_THIRD_TY, "HIGH", g_high_score, 6);

    if (g_pause_unlocked) {
        draw_coop_text_counter(COOP_L_TX, COOP_TOTAL_TY, "T.LINES",
                                coop_total_lines(), 4);
        draw_coop_text_counter(COOP_R_TX, COOP_TOTAL_TY, "T.SCORE",
                                coop_total_score(), 6);
    } else {
        clear_region(COOP_L_TX, COOP_TOTAL_TY, COOP_PANEL_W, 2);
        clear_region(COOP_R_TX, COOP_TOTAL_TY, COOP_PANEL_W, 2);
    }
    g_panel_layer = false;

    /* NO COSSACK HERE ANY MORE. He had the right panel's tall compartment
     * while it was empty; the partner's preview is what that compartment is
     * for now, and he keeps the one HUD Stats gives him. */
    hide_idle_cossack();
}

void draw_panel(void) {
    if (g_session.game.coop) { draw_coop_panel(); return; }

    const TengenPlayerState *p = &g_session.game.player[g_view];
    if (p->score > g_high_score) g_high_score = p->score;

    /* Everything from here down is the panel's, so it goes on the panel's
     * layer — the two exceptions, the braid and the banner, say so where they
     * are drawn. See SCREENBLOCK_PANEL. */
    g_panel_layer = true;

    /* THE LEFT PANEL, TOP TO BOTTOM: the preview in the big compartment and
     * the four counters in the short ones. NEXT used to live over the
     * histogram in the right box, which is where the cartridge puts it, and
     * moved to the left one only when the banner took the right one over —
     * two homes for one thing, and the emptier of the two boxes was always
     * the one you were looking at. One home now, in both HUDs, and it is the
     * compartment that was built for it. */
    clear_both(BOX_L_TX, CELL_BIG_TY, BOX_IN, CELL_BIG_H);
    clear_panel_region(BOX_L_TX, CELL_BIG_TY, BOX_IN, CELL_BIG_H);
    /* ON THE MAIN LAYER, WHICH IS WHAT CENTRES IT. Everything else in this
     * panel rides the counters' layer two pixels below the grid — that is
     * where SCORE's headroom under the braid comes from — but NEXT is not a
     * counter under a shelf, it is a block of content in a cell, and those
     * two pixels are the difference between nine pixels of air above it and
     * eight below (which is centred, as near as a 23-pixel block in a
     * 40-pixel cell can be) and eleven against six. The preview's own layer
     * is held at the grid to match; see the note by REG_BG1VOFS below. */
    g_panel_layer = false;
    draw_next_label_and_piece(BOX_L_TX, ROW_NEXT, BOX_IN, g_view, PAL_NEXT_BANK);
    g_panel_layer = true;

    draw_counter(ROW_SCORE, HUD_LABEL_SCORE, p->score, 6, 0);
    draw_counter(ROW_LINES, HUD_LABEL_LINES, p->lines, 4, 1);
    draw_counter(ROW_LEVEL, HUD_LABEL_LEVEL, p->level, 2, 2);

    if (g_session.game.two_player) {
        /* A race wants the other board's numbers where the high score would
         * be. The ROM keeps no piece histogram in 2P either, so nothing of
         * the cartridge's is being displaced. */
        const TengenPlayerState *o = &g_session.game.player[g_view ^ 1];
        /* ...AND THE WORD OVER IT SAYS WHETHER THEY ARE STILL IN IT. The
         * notice used to be at the bottom of the right box, which is the row
         * the histogram's icon strip stands on — so the box could carry the
         * notice or the statistics and not both, and a race got no
         * statistics at all. Here it costs nothing: the number under it is
         * their score either way, and frozen is what OUT means. */
        clear_region(BOX_L_IN, ROW_HIGH, BOX_L_W, 1);
        draw_text(BOX_L_IN, ROW_HIGH,
                   o->game_active ? "RIVAL" : " OUT", BANK_LABEL);
        clear_region(BOX_L_IN, ROW_HIGH + 1, BOX_L_W, 1);
        draw_number_blank(BOX_L_IN, ROW_HIGH + 1, o->score, 6, BANK_VALUE);
    } else {
        /* The cartridge's own 1P panel carries a HIGH SCORE beside the score
         * — "HIGH" and "SCORE" in plain ASCII at nametable row 2, and
         * highScoreHundredThousands is the seventh entry of
         * statsDataAddresses (main.asm.txt:4100-4107), which is the top of
         * the HIGH SCORES table — see leader_reset. */
        draw_text(BOX_L_IN + 1, ROW_HIGH, "HIGH", BANK_LABEL);
        clear_region(BOX_L_IN, ROW_HIGH + 1, BOX_L_W, 1);
        draw_number_blank(BOX_L_IN, ROW_HIGH + 1, g_high_score, 6, BANK_VALUE);
    }

    /* AND THE OFFSET LAYER STAYS ON THE GRID. It carries exactly one thing in
     * a match — the preview, on the frames its piece is an odd number of
     * tiles wide — and the preview's label is on the main layer, so a layer
     * two pixels down meant the piece sat two pixels below its own word every
     * other piece. It used to ride with the counters because NEXT used to be
     * drawn with them; it is not any more. */
    REG_BG1VOFS = 0;

    /* The right box: the banner, the statistics, or — in a race, where the
     * cartridge keeps no statistics either — nothing.
     *
     * THE BANNER TAKES THE WHOLE COLUMN, BRAID AND ALL. It is six letters of
     * three rows each, eighteen rows with no padding anywhere in it, and a
     * braid box leaves sixteen — so there is no honest crop. Handing it the
     * column instead is also what the cartridge's own screen looks like:
     * a bare vertical TETRIS with nothing framing it. What it does NOT get is
     * the two columns nearest the board: those are the rope that frames the
     * playfield itself, and the playfield keeps its frame whatever the HUD is
     * doing. The box comes back when the banner goes away, through g_repaint. */
    if (g_show_banner) {
        /* THE BANNER IS ART, NOT A COUNTER: it goes on the main map, aligned
         * to the screen. Both borrowed maps are wiped first — the panel's
         * because the statistics box was there a frame ago, the offset one
         * because a preview may have been. The FRAME is not touched: it is
         * the same inverted L in both HUDs now, drawn once with the screen. */
        g_panel_layer = false;
        clear_region(BOX_R_IN, BOX_TOP_IN, BOX_IN, SCREEN_TH - BOX_TOP_IN);
        clear_panel_region(BOX_R_IN, BOX_TOP_IN, BOX_IN, SCREEN_TH - BOX_TOP_IN);
        clear_stats_layer();
        draw_banner();
        /* No shelf for him in HUD Banner: the column is the banner's. */
        hide_idle_cossack();
    } else {
        /* HUD STATS: THE COSSACK TAKES THE TOP OF THE RIGHT BOX, which is the
         * cell NEXT used to have, and the histogram keeps the rest of it.
         * He was tucked into the bottom of the left panel before, under four
         * counters, where he had nothing round him and nothing to do with
         * what was over him; here he has a compartment, a ledge under his
         * feet and the piece counts below — which is a figure standing on a
         * shelf watching the game, rather than a sprite parked in a gap.
         *
         * He stops for two things and they are the same thing — there is
         * nothing to keep time to. A dead board freezes him where he stands,
         * and so does PAUSE: a cossack swaying behind the plaque while the
         * music is suspended is the one part of the screen that did not
         * notice the game had stopped. */
        bool alive = g_session.game.player[g_view].game_active &&
                     !g_session.game.paused;
        /* The interlude, danced solo: see g_idle_show. It runs off the show's
         * own clock so it lasts exactly as long as the show does. */
        g_idle_show = g_dancer_active;
        draw_idle_cossack(g_idle_show ? (int)g_dancer_elapsed : g_idle_frame,
                           alive || g_idle_show,
                           BOX_R_IN, ROW_DANCER, BOX_IN, ROW_DANCER_H);
        g_idle_show = false;
        if (alive) g_idle_frame++;

        clear_both(BOX_R_IN, ROW_DANCER, BOX_IN, ROW_DANCER_H);
        /* THE HISTOGRAM IN EVERY MODE, a race included. The cartridge keeps
         * none in 2P, which is why this box used to be empty there — but the
         * cartridge has no VERSUS COMPUTER either, and the counts it would
         * be refusing to show are the ones the port already keeps per player.
         * What it was really showing instead was the rival's OUT notice, and
         * that has moved to the left panel, over their score, where it reads
         * better and costs the box nothing. */
        draw_stats(p, STATS_TX);
    }

    g_panel_layer = false;
}

void draw_field(void) {
    /* THE COOP BOARD IS field[0] FOR BOTH PLAYERS — the core shares it the
     * way the cartridge does (tengen_core.c, `game->coop ? 0 : slot`) — and
     * the guest views slot 1, so indexing by view showed the guest the
     * second field, which coop never writes: an empty board with two pieces
     * falling through it. */
    const TengenPlayfield *field =
        &g_session.game.field[g_session.game.coop ? 0 : g_view];
    int clearing_slot = hud_clearing_slot();
    const TengenPlayerState *p = &g_session.game.player[clearing_slot];

    /* How far the line-clear sweep has crossed the completed rows, and what
     * it is writing into them as it goes. `written` is the last column the
     * trailing sprite has passed over; everything to its right still shows
     * the blocks that are about to come down. */
    uint8_t step = tengen_line_clear_step(&g_session.game, (TengenPlayerSlot)clearing_slot);
    int written = (int)step - TENGEN_CLEAR_TRAIL - 1;
    int rows_going = 0;
    for (int row = 0; row < TENGEN_PF_HEIGHT; row++)
        if (p->clearing_rows & (1u << row)) rows_going++;
    const char *word = kClearWord[rows_going > 4 ? 4 : rows_going];

    for (int row = 0; row < TENGEN_PF_HEIGHT; row++) {
        bool clearing = (p->clearing_rows & (1u << row)) != 0;
        for (int col = 0; col < field_cols(); col++) {
            /* A cell's value IS its tile index — that is the whole point of
             * the ROM's nibble encoding (notes.txt.txt:35). Empty is 0, which
             * is the blank tile. All of it draws in the level's palette. */
            uint16_t entry = WITH_BANK(field->cell[row][field_col0() + col], 0);

            /* Behind the sweep the row is gone and the word is in its place,
             * one character per playable column (L89E9,
             * main.asm.txt:1508-1530). */
            if (clearing && col <= written)
                entry = WITH_BANK(ascii_tile(word[col]), 0);

            set_map_tile(field_tx() + col, FIELD_TY + row, entry);
        }
    }

    /* No falling piece exists while the completed rows animate — the ROM
     * blanks player1TetrominoCurrent as the piece locks. That is per player:
     * on a coop board the partner's piece is still standing there, frozen
     * with everything else, and the loop below skips only the one that is
     * actually clearing. */

    /* The falling piece is drawn over the settled field rather than into it,
     * and in its own palette — the ROM draws it as sprites for exactly that
     * reason. Cells above the field are skipped, which is what makes a piece
     * visibly slide in from off-screen the way the original does.
     *
     * TWO PIECES FALL ON A COOP BOARD AND BOTH OF THEM HAVE TO BE DRAWN. This
     * used to draw only the viewed player's, and since coop is one shared
     * field the partner's piece — the computer's, in WITH COMPUTER — simply
     * was not there: nothing fell, and then a piece appeared on the floor out
     * of nowhere when it locked and joined the settled cells. The partner
     * goes down first so the local piece wins any overlap, which is the one
     * the player is steering. */
    for (int pass = 0; pass < 2; pass++) {
        int slot = pass == 0 ? (g_view ^ 1) : g_view;
        if (pass == 0 && !g_session.game.coop) continue;
        const TengenPlayerState *sp = &g_session.game.player[slot];
        if (!sp->game_active || sp->line_clear_timer > 0) continue;

        TengenCell cells[4];
        int count = tengen_active_piece_cells(&g_session.game,
                                               (TengenPlayerSlot)slot, cells);
        TengenTetromino current = sp->piece.current;
        int bank = pass == 0 ? PAL_PIECE2_BANK : PAL_PIECE_BANK;
        for (int i = 0; i < count; i++) {
            if (cells[i].row < 0) continue;
            int col = cells[i].col - field_col0();
            if (col < 0 || col >= field_cols()) continue;
            uint8_t tile = piece_cell_tile(current, sp->piece.orientation, i);
            set_map_tile(field_tx() + col, FIELD_TY + cells[i].row,
                          WITH_BANK(tile, bank));
        }
    }
}

void draw_pause_box(void) {
    for (int y = 0; y < PAUSE_H; y++)
        for (int x = 0; x < PAUSE_W; x++)
            set_map_tile(PAUSE_TX + x, PAUSE_TY + y,
                          WITH_BANK(kPauseTiles[y][x], plaque_bank()));
}
