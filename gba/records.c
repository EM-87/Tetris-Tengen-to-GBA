/*
 * records.c -- the HIGH SCORES tables.
 *
 * The cartridge's one table and the port's others (one per prototype, one
 * per mode), how they are kept on the battery and read back, how a game is
 * written into them as it ends, the page that shows them, the typing of the
 * initials and the name each console remembers, and the rows a Single-Pak
 * copy hands to a console with a save. Moved out of hud.c, which is what a
 * match looks like; this is what is left of it afterwards.
 */
#include "port.h"

#include <string.h>

static char leader_letter(uint8_t index) {
    return index == 0 ? ' ' : (char)('A' + index - 1);
}

/* The tables, and which one is in play. Table 0 is the release's 1 PLAYER,
 * then the prototypes', in skin order, then the release's other four modes;
 * see LEADER_TABLES. Everything below still says `g_leader`, because
 * everything below is about ONE table and which one it is is decided in
 * exactly one place (leader_select). */
static LeaderEntry g_tables[LEADER_TABLES][LEADER_ENTRIES];
static int g_table;
#define g_leader (g_tables[g_table])
/* THE LAST NAME TYPED ON THIS CONSOLE, which every row it makes after comes
 * up with: a player who plays on writes it once (SAVE_NAME_OFF). AAA, the
 * cartridge's own blank, until somebody types one. A console types for one
 * player — over the cable the other's rows are theirs to name — so there is
 * one. */
static uint8_t g_last_name[LEADER_INITIALS] = { 1, 1, 1 };
/* The save's layout holds: four skins at most below SAVE_EXTRA_OFF (proto_d
 * is not one; see Decisions in CLAUDE.md), and everything below the paused
 * game at $4000 (suspend.c). */
typedef char save_skins_fit[SAVE_TABLE_OFF(4) + 3 <= SAVE_EXTRA_OFF ? 1 : -1];
typedef char save_modes_fit[SAVE_MODE_SUMS_OFF + LEADER_MODE_TABLES <= 0x4000 ? 1 : -1];
/* THE ROWS A COPY HAS NOT HANDED ON YET. A Single-Pak copy has no save
 * (sram_write goes nowhere there), so a row it puts on a table is marked
 * here, moves with its row as rows are pushed down, falls off with it, and
 * goes across the next time the copy links up with a console that has a
 * save (TengenRecordSync, main.c). Never set on a cartridge. */
static bool g_unsent[LEADER_TABLES][LEADER_ENTRIES];

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
/* ...and what the row came up with, which is what B puts back: the last name
 * typed here (g_last_name), or AAA. */
static uint8_t g_leader_was[LEADER_INITIALS];

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
__attribute__((used, section(".rodata"), aligned(4)))
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
 * always had, the prototypes' are past all their data, and the modes' past
 * theirs. See LEADER_TABLES. */
static unsigned leader_sum_off(int table) {
    if (table == 0) return SAVE_SUM_OFF;
    if (table < LEADER_SKIN_TABLES) return SAVE_SUMS_OFF + (unsigned)(table - 1);
    return SAVE_MODE_SUMS_OFF + (unsigned)(table - LEADER_SKIN_TABLES);
}

/* ...and where its fifteen entries do. */
static unsigned leader_data_off(int table) {
    return table < LEADER_SKIN_TABLES ? SAVE_TABLE_OFF(table)
                                      : SAVE_MODE_OFF(table - LEADER_SKIN_TABLES);
}

/* A table's number ON THE WIRE (TengenRecord.table), which is not its place
 * in memory: the skins' keep the numbers they always had and the modes' are
 * 8 and up, so a copy from a build with another count of skins never puts a
 * mode's row in a prototype's table, and an older build simply refuses the
 * numbers it does not know (leader_merge). */
#define LEADER_WIRE_MODES 8
static uint8_t leader_wire_id(int table) {
    return (uint8_t)(table < LEADER_SKIN_TABLES
                         ? table : LEADER_WIRE_MODES + table - LEADER_SKIN_TABLES);
}
static int leader_from_wire(uint8_t id) {
    if (id < LEADER_SKIN_TABLES) return id;
    if (id >= LEADER_WIRE_MODES && id < LEADER_WIRE_MODES + LEADER_MODE_TABLES)
        return LEADER_SKIN_TABLES + id - LEADER_WIRE_MODES;
    return -1;
}

/* The last name's check byte: anything but three letters and this is no
 * name, and AAA stands. */
static uint8_t leader_name_check(const uint8_t *n) {
    return (uint8_t)(0x5A + n[0] * 3 + n[1] * 5 + n[2] * 7);
}

static void leader_save_name(void) {
    for (int c = 0; c < LEADER_INITIALS; c++)
        sram_write(SAVE_NAME_OFF + (unsigned)c, g_last_name[c]);
    sram_write(SAVE_NAME_OFF + LEADER_INITIALS, leader_name_check(g_last_name));
}

static void leader_load_name(void) {
    uint8_t n[LEADER_INITIALS];
    for (int c = 0; c < LEADER_INITIALS; c++) {
        n[c] = sram_read(SAVE_NAME_OFF + (unsigned)c);
        if (n[c] >= LEADER_LETTERS) return;
    }
    if (sram_read(SAVE_NAME_OFF + LEADER_INITIALS) != leader_name_check(n)) return;
    memcpy(g_last_name, n, LEADER_INITIALS);
}

static void leader_save_table(int table) {
    for (int i = 0; i < LEADER_ENTRIES; i++) {
        const LeaderEntry *e = &g_tables[table][i];
        unsigned at = leader_data_off(table) + (unsigned)i * SAVE_ENTRY_BYTES;
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
        unsigned at = leader_data_off(table) + (unsigned)i * SAVE_ENTRY_BYTES;
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
    /* The name stands on its own check byte, with or without a table. */
    leader_load_name();
    for (int i = 0; i < SAVE_MAGIC_LEN; i++)
        if (sram_read((unsigned)i) != (unsigned char)kSaveMagic[i]) return false;

    /* THE RELEASE'S TABLE DECIDES WHETHER THERE IS A SAVE AT ALL — it is the
     * one that has always been at these offsets. The prototypes' and the
     * modes' are read beside it and each simply keeps the cartridge's fifteen
     * if its own bytes do not add up, which is what a build nobody has played
     * looks like on a console that was saving before they existed. */
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

/* EVERY TABLE BACK TO THE CARTRIDGE'S COLD ONE, in memory and in the save:
 * what the erase at power-on does (erase_records_prompt), after asking
 * twice. */
void leader_erase_all(void) {
    leader_reset();
    leader_save();
    /* ...and the last name typed with them: a console wiped for somebody
     * else does not offer them the last owner's initials. */
    memset(g_last_name, 1, LEADER_INITIALS);      /* AAA */
    leader_save_name();
}

/* The one place a table is chosen, and with it the HIGH SCORE the HUD and
 * the menus print and the bar a new record is sung for (record_watch). */
static void leader_select(int table) {
    if (table < 0 || table >= LEADER_TABLES) table = 0;
    g_table = table;
    g_high_score = g_leader[0].score;
    g_record_bar = g_leader[0].score;
}

static int leader_skin_table(int skin) {
    int table = skin + 1;
    return table < 0 || table >= LEADER_SKIN_TABLES ? 0 : table;
}

/* WHICH BUILD'S TABLE IS IN PLAY, by the skin: -1 is the release and 0 and up
 * are the prototypes in their title order. Called where the skin is settled,
 * not read every frame, because switching is also what refreshes the HIGH
 * SCORE the HUD and the menus print. */
void leader_use_table(int skin) {
    leader_select(leader_skin_table(skin));
}

/* ...and a match's: the release's split by mode, a prototype's whatever the
 * mode (see LEADER_MODE_TABLES). */
void leader_use_mode(int skin, int mode) {
    int table = leader_skin_table(skin);
    if (table == 0 && mode > GAME_1P && mode < GAME_COUNT)
        table = LEADER_SKIN_TABLES + mode - 1;
    leader_select(table);
}

/* The page's LEFT and RIGHT: the title's skin's table, then the four modes',
 * round. */
void leader_browse(int skin, int step) {
    int ring[1 + LEADER_MODE_TABLES], n = 0, at = 0;
    ring[n++] = leader_skin_table(skin);
    for (int m = 0; m < LEADER_MODE_TABLES; m++) ring[n++] = LEADER_SKIN_TABLES + m;
    for (int i = 0; i < n; i++) if (ring[i] == g_table) at = i;
    leader_select(ring[(at + step + n) % n]);
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

    for (int i = LEADER_ENTRIES - 1; i > row; i--) {
        g_leader[i] = g_leader[i - 1];
        g_unsent[g_table][i] = g_unsent[g_table][i - 1];
    }
#ifdef TENGEN_MULTIBOOT
    g_unsent[g_table][row] = true;
#else
    g_unsent[g_table][row] = false;
#endif
    g_leader[row].score = score;
    /* main.asm.txt:364-377: a line count that has reached its thousands digit
     * is stored as "999" — the column is three wide and the ROM says so. */
    g_leader[row].lines = (uint16_t)(lines > 999 ? 999 : lines);
    for (int c = 0; c < LEADER_INITIALS; c++) g_leader[row].initials[c] = 1; /* 'A' */
    g_high_score = g_leader[0].score;
    return row;
}

/* The copy's rows that have not gone across yet, oldest table first; how
 * many (at most `max`). */
int leader_unsent(TengenRecord *out, int max) {
    int n = 0;
    for (int t = 0; t < LEADER_TABLES; t++)
        for (int i = 0; i < LEADER_ENTRIES && n < max; i++) {
            if (!g_unsent[t][i]) continue;
            const LeaderEntry *e = &g_tables[t][i];
            out[n].table = leader_wire_id(t);
            out[n].score = e->score;
            out[n].lines = e->lines;
            for (int c = 0; c < LEADER_INITIALS; c++) out[n].initials[c] = e->initials[c];
            n++;
        }
    return n;
}

/* ...and they have: a console with a save has them now. */
void leader_mark_sent(void) {
    for (int t = 0; t < LEADER_TABLES; t++)
        for (int i = 0; i < LEADER_ENTRIES; i++) g_unsent[t][i] = false;
}

/* A COPY'S ROWS ONTO THIS CONSOLE'S TABLES, and into its save; how many made
 * them. Each goes into the table it came from, by the cartridge's own rule
 * (leader_insert), its letters with it. A row that is already there to the
 * letter is not put in twice: two copies that played each other both carry
 * both games, and the second to link up would give them again. Checked
 * before anything is believed, as leader_load checks the save. */
int leader_merge(const TengenRecord *rows, int n) {
    int keep_table = g_table, kept = 0;
    for (int r = 0; r < n; r++) {
        const TengenRecord *in = &rows[r];
        int table = leader_from_wire(in->table);
        if (table < 0 || in->score > 999999 || in->lines > 999)
            continue;
        bool letters_ok = true;
        for (int c = 0; c < LEADER_INITIALS; c++)
            if (in->initials[c] >= LEADER_LETTERS) letters_ok = false;
        if (!letters_ok) continue;
        g_table = table;
        bool there = false;
        for (int i = 0; i < LEADER_ENTRIES && !there; i++)
            there = g_leader[i].score == in->score && g_leader[i].lines == in->lines &&
                    !memcmp(g_leader[i].initials, in->initials, LEADER_INITIALS);
        if (there) continue;
        int row = leader_insert(in->score, in->lines);
        if (row < 0) continue;
        for (int c = 0; c < LEADER_INITIALS; c++) g_leader[row].initials[c] = in->initials[c];
        kept++;
    }
    g_table = keep_table;
    g_high_score = g_leader[0].score;
    if (kept) leader_save();
    return kept;
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
    /* Leading zeros blanked, as the cartridge prints them: "17000" and a
     * lone "0" for no lines, not "017000" and "000". */
    draw_number_blank(SCREEN_LEADER_SCORE_TX, ty, e->score, 6,
                       leader_bank(SCREEN_LEADER_SCORE_TX, ty));
    draw_number_blank(SCREEN_LEADER_LINES_TX, ty, e->lines, 3,
                       leader_bank(SCREEN_LEADER_LINES_TX, ty));
}

/* WHICH TABLE THIS IS, in the heading: a mode's name in place of the
 * cartridge's HIGH SCORES, right up against its "-LINES", in its colour —
 * 1 PLAYER's table (and a prototype's) keeps the cartridge's words. The
 * heading's own row is put back first, so walking the tables never leaves a
 * longer name's head behind a shorter one. */
static void draw_leader_heading(void) {
    const int ty = SCREEN_LEADER_HEAD_TY;
    const uint8_t *row = &kScreenLeaderTiles[ty * SCREEN_LEADER_W];
    int dash = -1;
    for (int tx = 0; tx < SCREEN_LEADER_W; tx++) {
        set_map_tile(tx, ty, WITH_BANK(row[tx], leader_bank(tx, ty)));
        if (dash < 0 && row[tx] == ascii_tile('-')) dash = tx;
    }
    if (g_table < LEADER_SKIN_TABLES || dash < 11) return;
    const char *name = kGameNames[g_table - LEADER_SKIN_TABLES + 1];
    int bank = leader_bank(dash - 1, ty);
    int len = (int)strlen(name), first = dash - len;
    if (first < 2) first = 2;               /* inside the frame, whatever */
    for (int tx = 2; tx < dash; tx++)
        set_map_tile(tx, ty, WITH_BANK(ascii_tile(' '), bank));
    for (int i = 0; first + i < dash; i++)
        set_map_tile(first + i, ty, WITH_BANK(ascii_tile(name[i]), bank));
}

/* The heading and the fifteen rows: what changes between two tables. */
void draw_leader_table(void) {
    draw_leader_heading();
    for (int row = 0; row < LEADER_ENTRIES; row++) draw_leader_row(row);
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
    draw_leader_table();
}

/* WHOSE SCORES GO ON THE BOARD, AND WHEN. L81DD (main.asm.txt:315-339) is
 * called from the TOP-OUT itself (:600, the `jsr` right after the flag is
 * cleared), not from the end of the match — which is the whole reason a
 * player who starts again over A+B does not lose the game they just
 * finished: each board that dies is written down as it dies, so five games
 * are five rows. On the negative playMode — coop — it runs for the other
 * player too, because one board is two players' game. What it refuses is the
 * COMPUTER: L81EC returns without doing anything for player 2 once
 * menuGameMode has reached VERSUS. So a machine never takes a place on the
 * table, and in coop both people do.
 *
 * The cartridge keeps one flag per PLAYER for the typing ($74/$75) and marks
 * each row with its owner in the top two bits of its initials, so Left and
 * Right walk a player between their own rows. The port types them one after
 * the other instead, oldest first, which is the same set in a fixed order. */
#define LEADER_QUEUE_MAX 8
static int g_leader_queue[LEADER_QUEUE_MAX];   /* this console's, to type */
static int g_leader_queued;
/* ...and over a cable the rival's rows are not this console's to type: their
 * name arrives on the wire. See TengenNameSwap. */
static int g_leader_rivals[LEADER_QUEUE_MAX];
static int g_leader_rivals_n;
static int g_leader_own_row = -1;              /* the newest of this console's */

int leader_rival_row(void) {
    return g_leader_rivals_n ? g_leader_rivals[g_leader_rivals_n - 1] : -1;
}

/* ...and every row of theirs redrawn, once their name has landed. A rival who
 * restarted with A+B made the table once per game, leader_rival_initials
 * names all of those rows, and so all of them are redrawn: drawing only the
 * newest left the earlier ones reading AAA on screen over a name that was
 * already in memory and in the SRAM. */
void draw_rival_leader_rows(void) {
    for (int i = 0; i < g_leader_rivals_n; i++)
        draw_leader_row(g_leader_rivals[i]);
}

void leader_own_initials(uint8_t out[LEADER_INITIALS]) {
    /* A player with no row here still has a name to send, because the row
     * they have is on the OTHER console's table — the two are two consoles'
     * own histories and a score can make one and miss the other. What goes
     * across then is the name this console last typed (g_last_name), which
     * is AAA, what an untyped row carries, until somebody types one. See
     * TengenNameSwap. */
    for (int c = 0; c < LEADER_INITIALS; c++)
        out[c] = g_leader_own_row >= 0 ? g_leader[g_leader_own_row].initials[c]
                                        : g_last_name[c];
}

void leader_rival_initials(const uint8_t in[LEADER_INITIALS]) {
    /* EVERY row that is theirs, not just the last: a rival who started again
     * over A+B has one row per game they finished, and one name for all of
     * them. */
    for (int i = 0; i < g_leader_rivals_n; i++)
        for (int c = 0; c < LEADER_INITIALS; c++)
            g_leader[g_leader_rivals[i]].initials[c] =
                (uint8_t)(in[c] < LEADER_LETTERS ? in[c] : 1);
    if (g_leader_rivals_n) leader_save();
}

/* A fresh match: nothing is owed and nothing is owned. */
uint32_t g_record_bar;   /* the table's top as the match began; see record_watch */
bool g_record_sung;      /* ...and the jingle for passing it has been played */

void leader_new_match(void) {
    /* The bar itself is the match's table's top, taken as skin_begin_match
     * chooses that table (leader_use_mode), which is after this. */
    g_record_sung = false;
    g_leader_row = -1;
    g_leader_queued = 0;
    g_leader_rivals_n = 0;
    g_leader_cursor = 0;
    g_leader_own_row = -1;
}

/* One board has just died. Writes it down where it belongs and remembers
 * whose row it is; nothing is typed until the page comes up. */
static void leader_record_one(int slot) {
    const TengenPlayerState *p = &g_session.game.player[slot];
    /* L81EC's refusal: in VERSUS and WITH COMPUTER, player 2 is the machine
     * and a machine takes no place on the table. */
    if (g_ai_active && slot == TENGEN_PLAYER_2) return;

    int row = leader_insert(p->score, p->lines);
    if (row < 0) return;
    /* An entry that lands above one already remembered pushes it down a row,
     * exactly as it pushes every other entry down. */
    for (int q = 0; q < g_leader_queued; q++)
        if (g_leader_queue[q] >= row) g_leader_queue[q]++;
    for (int q = 0; q < g_leader_rivals_n; q++)
        if (g_leader_rivals[q] >= row) g_leader_rivals[q]++;
    if (g_leader_own_row >= row) g_leader_own_row++;

    /* Over a cable only this console's own player types here; in a coop game
     * on one console both pads do. */
    if (g_linked && slot != g_view) {
        if (g_leader_rivals_n < LEADER_QUEUE_MAX)
            g_leader_rivals[g_leader_rivals_n++] = row;
        return;
    }
    /* ...with the last name typed here already in it, so a player who plays
     * on takes it with START. */
    memcpy(g_leader[row].initials, g_last_name, LEADER_INITIALS);
    g_leader_own_row = row;
    if (g_leader_queued < LEADER_QUEUE_MAX)
        g_leader_queue[g_leader_queued++] = row;
}

void leader_record(int slot) {
    if (g_demo) return;                 /* nobody played that one */
    leader_record_one(slot);
    /* One board is two players' game: L81DD runs the other one as well. */
    if (g_session.game.coop) leader_record_one(slot ^ 1);
}

/* The page is coming up: open the typing on whatever was written down while
 * the match was running. */
void leader_submit(void) {
    g_leader_cursor = 0;
    if (g_leader_queued) {
        g_leader_row = g_leader_queue[0];
        g_leader_blink = 0;
        g_leader_undo_n = 0;
        memcpy(g_leader_was, g_leader[g_leader_row].initials, LEADER_INITIALS);
    } else {
        g_leader_row = -1;
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
 *   B             undo the last letter changed: back to what the row came
 *                 up with (the last name typed here, or A), cursor onto it
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
            g_leader[g_leader_row].initials[p] = g_leader_was[p];
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
        /* ...and it is the name the next row comes up with, power cycles
         * included — the next in this queue as well, which is the same
         * player's (a console types for one). */
        memcpy(g_last_name, g_leader[finished].initials, LEADER_INITIALS);
        leader_save_name();
        if (--g_leader_queued > 0) {
            g_leader_row = g_leader_queue[0];
            memcpy(g_leader[g_leader_row].initials, g_last_name, LEADER_INITIALS);
            memcpy(g_leader_was, g_last_name, LEADER_INITIALS);
        } else {
            g_leader_row = -1;
        }
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
