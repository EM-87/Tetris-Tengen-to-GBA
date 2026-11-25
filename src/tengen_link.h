/*
 * tengen_link.h — two players, two consoles, one game.
 *
 * The cartridge's 2P mode is a RACE: two independent playfields, no garbage
 * sent between them, only an optional pile of garbage dealt to one player
 * before the first piece (`initHandicapGarbage`, main.asm.txt:3545-3598).
 * Nothing crosses between the boards during play. That is what makes a link
 * cable easy: there is no game state to reconcile, only inputs.
 *
 * So this is LOCKSTEP. Both machines run the same core over the same seed and
 * simulate BOTH players; each sends only its own buttons and receives the
 * other's, then steps player 1 and player 2 in that order with those buttons.
 * Same code, same inputs, same order — same result, frame for frame, with
 * eight bits per machine per frame on the wire.
 *
 * The reason this file exists at all, rather than living in the GBA layer, is
 * that lockstep is exactly the kind of thing that looks right and silently
 * diverges. Here it compiles on the host and `make test` runs two simulated
 * machines against each other and compares their state byte for byte.
 *
 * This file knows nothing about the GBA's serial hardware; gba/link.c does
 * that part and hands the words over.
 */
#ifndef TENGEN_LINK_H
#define TENGEN_LINK_H

#include "tengen_core.h"

/* What one machine puts on the wire each frame: its buttons, plus the frame
 * it believes it is on so the other side can notice a dropped transfer. Both
 * fit in the 16 bits a GBA multiplayer transfer carries.
 *
 * THE TOP THREE BITS SAY "THIS IS A MATCH WORD": 110, which puts every
 * match word in $C000-$DFFF. No lobby or records word gets there (their tag
 * is the top nibble, and the largest is SKIN's 9), and neither does $FFFF —
 * a GBA reads that from the slot of a console that is not there, and an
 * unplugged cable must not look like a player holding every button. The mark
 * is how a console still in the lobby knows its partner has already gone
 * into the match (tengen_lobby_apply), and how one already in the match
 * knows a word is still the lobby's GO: the two consoles do not leave the
 * lobby on the same transfer when their frames and the transfers fall
 * differently, which on hardware they do. So the frame counter is FIVE
 * bits, which still wrap far slower than a transfer can be late. */
#define TENGEN_LINK_BUTTON_MASK 0x00FF
#define TENGEN_LINK_FRAME_SHIFT 8
#define TENGEN_LINK_FRAME_MASK  0x1F
#define TENGEN_LINK_MATCH_MARK  0xC000
#define TENGEN_LINK_MARK_MASK   0xE000

static inline uint16_t tengen_link_pack(uint8_t buttons, uint8_t frame) {
    return (uint16_t)(TENGEN_LINK_MATCH_MARK | buttons |
                      ((uint16_t)(frame & TENGEN_LINK_FRAME_MASK)
                       << TENGEN_LINK_FRAME_SHIFT));
}
static inline bool tengen_link_is_match_word(uint16_t word) {
    return (word & TENGEN_LINK_MARK_MASK) == TENGEN_LINK_MATCH_MARK;
}
static inline uint8_t tengen_link_buttons(uint16_t word) {
    return (uint8_t)(word & TENGEN_LINK_BUTTON_MASK);
}
static inline uint8_t tengen_link_frame(uint16_t word) {
    return (uint8_t)((word >> TENGEN_LINK_FRAME_SHIFT) & TENGEN_LINK_FRAME_MASK);
}

typedef struct {
    TengenGame game;
    uint8_t local_slot;  /* TENGEN_PLAYER_1 on the machine that is cable master */
    uint8_t frame;       /* wraps at 32 on the wire; spots a lost transfer */
    bool desynced;       /* a word arrived for the wrong frame; game is over */
} TengenLink;

/* Both machines call this with IDENTICAL seed and start level — the master
 * sends them across before the first frame — and their own slot. */
void tengen_link_start(TengenLink *link, uint16_t seed, uint8_t start_level,
                        TengenPlayerSlot local_slot, bool coop, bool xe);

/* THE MATCH'S RULES THAT ARE THE PORT'S, not the cartridge's: the pause
 * menu's OPTIONS on the master's console (TengenLobby.rules), off on a
 * console that never set them. UP's drop is the whole game's; the faster
 * hands are each player's in the core and BOTH players' here, since over the
 * cable both pads are people's. Both consoles call this with the same rules,
 * at the start and whenever the shared pause menu changes one. */
#define TENGEN_RULE_HARD_DROP  0x01   /* TengenGame.hard_drop */
#define TENGEN_RULE_FAST_DOWN  0x02   /* TengenGame.fast_soft */
#define TENGEN_RULE_FAST_SHIFT 0x04   /* TengenGame.fast_shift */
#define TENGEN_RULE_HOLD       0x08   /* TengenGame.hold */
#define TENGEN_RULES_ALL       0x0F
void tengen_link_rules(TengenLink *link, uint8_t rules);

/* One frame. `local_buttons` is what this machine's player is holding;
 * `remote_word` is what arrived from the other machine. Returns false, and
 * sets `desynced`, if the remote word is not for the frame we are on — after
 * which the session is finished, because the two simulations can no longer
 * be assumed to match.
 *
 * `out` (may be NULL) receives both players' step results, player 1 first. */
bool tengen_link_step(TengenLink *link, uint8_t local_buttons,
                       uint16_t remote_word, TengenStepResult out[2]);

/* The word this machine should send for the frame it is about to run. */
static inline uint16_t tengen_link_send_word(const TengenLink *link,
                                              uint8_t local_buttons) {
    return tengen_link_pack(local_buttons, link->frame);
}

/* ----------------------------------------------------------------------- *
 * The lobby
 *
 * Before the first piece the two machines have to agree on a seed and a start
 * level, or the lockstep above would be two different games kept carefully in
 * step. The master walks a stop-and-wait sequence — one tagged word per
 * transfer, the slave echoing back the tag it received — and both leave on the
 * same transfer.
 *
 * This is here, and not in the GBA's serial code, for the same reason the
 * lockstep is: the handshake's one subtle moment (who may leave, and when) is
 * exactly the sort of thing that looks obviously right and is not, so
 * `make test` runs two of these against each other instead of trusting a
 * reading of the code. Nothing in here touches hardware; gba/link.c carries
 * the words.
 *
 * Tags live in the top four bits, which is why they exist only here: once the
 * match starts the whole 16 bits carry buttons and a frame counter. The
 * largest tag is SKIN's 9, so the largest tagged word is $9FFF, and a lobby
 * word can no more be mistaken for an absent console's $FFFF than a match
 * word can. Tags stop short of $F for exactly that reason.
 * ----------------------------------------------------------------------- */
typedef enum {
    TENGEN_LOBBY_NONE    = 0,
    TENGEN_LOBBY_HELLO   = 1,
    TENGEN_LOBBY_SEED_HI = 2,
    TENGEN_LOBBY_SEED_LO = 3,
    TENGEN_LOBBY_CONFIG  = 4,   /* level 0-4, music 5-8, coop 9, XE 10 */
    /* A stage of its own rather than four spare bits of CONFIG: two handicaps
     * of nought to four need six bits and CONFIG has four left. One more
     * stop-and-wait turn costs two transfers, which is two sixtieths of a
     * second on a screen the player is already sitting on. */
    TENGEN_LOBBY_HANDICAP = 5,  /* player 1 in bits 0-3, player 2 in bits 4-7,
                                 * the master's rules in 8-11 (TENGEN_RULE_*) */
    TENGEN_LOBBY_GO      = 6,
    /* ...and two the match itself is over before either is used: the records
     * swap, below. They share the tag space because they share the wire and
     * the same rule about $FFFF. */
    TENGEN_LOBBY_TYPING  = 7,   /* nothing to say yet; bit 8 is the receipt */
    TENGEN_LOBBY_NAME    = 8,   /* one letter of three; see TengenNameSwap */
    /* THE MASTER'S SKIN, between HANDICAP and GO, and numbered last only
     * because it came last: the stages run in the order next_stage says.
     * Master: bit 11 set if it is offering one, bits 0-10 the skin's
     * fingerprint. Slave's echo: bit 0 set if it HAS that skin. See
     * tengen_lobby_skins. */
    TENGEN_LOBBY_SKIN    = 9,
    /* THE RECORDS A COPY MADE, right after GO: see TengenRecordSync. */
    TENGEN_LOBBY_RECORDS = 10
} TengenLobbyTag;

/* A skin's fingerprint is eleven bits of a hash of its own art, which is what
 * lets two DIFFERENT BUILDS agree on one: the same prototype dump produces the
 * same bytes whichever order a build lists its skins in, and a build without
 * it has nothing that matches. */
#define TENGEN_SKIN_OFFER       0x0800
#define TENGEN_SKIN_PRINT_MASK  0x07FF
#define TENGEN_SKIN_HAVE        0x0001
#define TENGEN_SKIN_MAX         8

#define TENGEN_LOBBY_TAG_SHIFT 12
#define TENGEN_LOBBY_PAYLOAD_MASK 0x0FFF

/* How many turns without a proper answer — no transfer, or a word from a
 * console that is not in a lobby — before a lobby forgets a partner it had
 * and goes back to waiting: a second at one a frame. Before the first answer
 * nothing counts; the other player may take minutes to get to the cable,
 * and the way out is B. A lobby never gives up. */
#define TENGEN_LOBBY_LOST 60

typedef struct {
    uint16_t seed;
    uint8_t start_level;
    uint8_t music;
    uint8_t handicap[2];   /* menuPlayer1Handicap / menuPlayer2Handicap */
    bool coop;           /* one twelve-wide board between them, not two */
    bool xe;             /* the Tetris Tengen XE level range; see tengen_core.h */
    uint8_t rules;       /* TENGEN_RULE_*: the master's, as they arrive */
    bool ready;          /* the handshake finished; the match may start */
    uint8_t stage;       /* master: the tag in flight. slave: the last taken */
    uint8_t echo;        /* slave: the tag it owes back */
    bool saw_go;         /* slave: GO has arrived once already */
    uint16_t idle;       /* turns without a proper answer, once linked */
    bool linked;         /* the other end is answering; see TENGEN_LOBBY_LOST */
    bool hold;           /* master: stay at HELLO until the player has chosen */
    /* THE SKIN. Each console lists the fingerprints of the skins IT has
     * (skins/skin_count); the master also says which of its own it is
     * offering (skin_offer, -1 for none). The result is `skin`: an index into
     * THIS console's own list, or -1, and the two consoles always agree on
     * whether it is -1 — the only thing the simulation has to agree on, since
     * a skinned board stores piece ids (piece_id_cells). See
     * tengen_lobby_skins. */
    uint16_t skins[TENGEN_SKIN_MAX];
    uint8_t skin_count;
    int8_t skin_offer;   /* master: which of its own it is offering */
    int8_t skin;         /* the agreed skin, as this console's own index */
    uint8_t echo_payload;   /* slave: what its SKIN echo says */
    /* WHICH GAME THIS CONSOLE CAME TO THE CABLE FOR: 2 PLAYER (false) or
     * COOPERATIVE (true). It rides in HELLO both ways, and two consoles that
     * chose differently never link; see tengen_lobby_mode. */
    bool mode_coop;
    /* ...unless this console came for EITHER: a Single-Pak slave, which has
     * no GAME SELECT of its own to have chosen on. It takes the mode of the
     * master's HELLO as its own; see tengen_lobby_mode_any. */
    bool mode_any;
} TengenLobby;

/* Starts a lobby. The master's seed/level/music are the ones that count; on
 * the slave they are overwritten by what arrives. */
void tengen_lobby_start(TengenLobby *lobby, uint16_t seed, uint8_t start_level,
                         uint8_t music);

/* THE MODE THIS CONSOLE CHOSE ON GAME SELECT. The lobby was one for both
 * linked modes, and the master's choice won: a player who had picked 2
 * PLAYER was taken into a COOPERATIVE game because the other had. Now each
 * says its mode in HELLO (bit 0 of the payload, master and echo alike), and
 * a HELLO or an echo of the other mode is not an answer: the two consoles
 * wait, as if the other were not there, until one of them picks the same.
 * Call after tengen_lobby_start_held; tengen_lobby_forget keeps it. */
void tengen_lobby_mode(TengenLobby *lobby, bool coop);

/* A SLAVE THAT FOLLOWS: the console booted over the cable by Single-Pak,
 * which comes to the lobby before anybody has asked it which game. It
 * answers a HELLO of either mode with that mode, so the master's choice is
 * the game. Only ever the slave's; a master with it would have nothing to
 * say in its own HELLO. Call after tengen_lobby_start_held. */
void tengen_lobby_mode_any(TengenLobby *lobby);

/* Starts the conversation again in the given role, keeping the seed, the
 * settings and the skins: for a console that has just learnt from the cable
 * that it is not the master it took itself for, or the other way round. */
void tengen_lobby_forget(TengenLobby *lobby, bool master);

/* Starts a lobby that CONNECTS AND THEN WAITS. The master keeps saying HELLO
 * until tengen_lobby_release, so the two consoles find each other before
 * anybody has chosen a level or a tune — which is the point: on a cable, only
 * one of the two players should be picking, and neither knows which one that
 * is until the cable tells them.
 *
 * Holding costs nothing, because the handshake is stop-and-wait: the slave
 * keeps echoing HELLO, every transfer succeeds, and `idle` never climbs
 * towards the timeout. */
void tengen_lobby_start_held(TengenLobby *lobby, uint16_t seed);

/* The master's choice, once it has one. Fills in the config and lets the
 * handshake run on to GO. Does nothing on a lobby that was not held. */
void tengen_lobby_release(TengenLobby *lobby, uint16_t seed,
                           uint8_t start_level, uint8_t music,
                           const uint8_t handicap[2], bool coop, bool xe);

/* ...and the master's RULES (TENGEN_RULE_*), after the release and before
 * the handshake reaches HANDICAP, which carries them. Unset, none: the
 * cartridge's game. */
void tengen_lobby_rules(TengenLobby *lobby, uint8_t rules);

/* THE PROTOTYPES AS A SKIN OVER THE CABLE. Call on both consoles after the
 * lobby starts: `prints` is this console's own skins' fingerprints
 * (tengen_skin_fingerprint), and `offer` the index of the one the master is
 * offering — the title's choice — or -1. The slave's `offer` is ignored: the
 * master's choice wins, as it does for the level and the tune.
 *
 * The skin goes on only if the master offers one AND the slave has the same
 * art; both consoles learn both facts in the SKIN stage, so both reach the
 * same answer. Otherwise both play the release. It is PAINT ONLY — over the
 * cable a skinned match plays by the release's rules. */
void tengen_lobby_skins(TengenLobby *lobby, const uint16_t *prints, int count,
                         int offer);

/* Eleven bits of FNV-1a over a skin's art. */
uint16_t tengen_skin_fingerprint(const uint8_t *bytes, unsigned len);

/* What this machine should put on the wire next. */
uint16_t tengen_lobby_word(const TengenLobby *lobby, bool master);

/* What one transfer attempt did. `got` is false when it failed, in which case
 * the two words are ignored and only the give-up counter moves. */
void tengen_lobby_apply(TengenLobby *lobby, bool master, bool got,
                         uint16_t master_word, uint16_t slave_word);

/* ----------------------------------------------------------------------- *
 * The records, after the match
 *
 * ONLY THE NAME CROSSES, and that is the whole size of this. A linked match
 * is lockstep: both consoles simulate both boards, so each one already knows
 * the other player's score and lines to the byte — `make gba-check` asserts
 * exactly that. What no console can know is what the person at the other end
 * typed into their own three letters, and without it a 2P match ends with
 * two HIGH SCORES pages that disagree about who was there.
 *
 * So both consoles put BOTH players on their own table and each types only
 * its own; the three letters go across afterwards and fill in the other row.
 *
 * NOT STOP-AND-WAIT, unlike the lobby, and for a reason: this is symmetric.
 * Both ends have something to say and neither is asking the other for it, so
 * there is no tag to echo. Each console sends its three letters round and
 * round, one per transfer, takes whatever arrives, and says in bit 8 of every
 * word whether it has all three of the other's yet. That is the receipt, and
 * it makes a lost transfer cost one turn of the wheel rather than a stall.
 *
 * THE LINGER IS NOT PADDING. A console that has everything must keep
 * answering for a few more transfers, because the last thing the other one
 * is waiting for is its receipt — and on the master, which is the only end
 * that starts transfers, going quiet would leave the slave with the letters
 * and no way to learn that its own arrived.
 *
 * What it does NOT solve, and this is a real edge rather than an oversight:
 * the two tables are two consoles' own histories, so a score can make one
 * and miss the other. A player whose score reaches the rival's table but not
 * their own is never asked to type, and sends the letters their row would
 * have carried — which is what the cartridge prints for a row nobody typed.
 * ----------------------------------------------------------------------- */
#define TENGEN_NAME_LETTERS 3
/* Ten seconds of silence, the same as the lobby's: long enough for the other
 * player to sit on their game-over plaque, short enough not to hang a page. */
#define TENGEN_NAME_TIMEOUT 600
/* Transfers to keep answering after everything has arrived. See above. */
#define TENGEN_NAME_LINGER 8

typedef struct {
    uint8_t mine[TENGEN_NAME_LETTERS];    /* what goes on the wire */
    uint8_t theirs[TENGEN_NAME_LETTERS];  /* ...and what came off it */
    uint8_t got;        /* bit per position of `theirs` that has arrived */
    uint8_t cursor;     /* which of `mine` goes next */
    uint8_t linger;
    bool sending;       /* this console has finished typing */
    bool acked;         /* the other console has all three of mine */
    bool complete;      /* everything is across, both ways; the cable may go */
    bool failed;        /* nothing answered for long enough to give up */
    uint16_t idle;
} TengenNameSwap;

/* Arms one. Nothing is sent but TYPING until tengen_name_send. */
void tengen_name_start(TengenNameSwap *swap);

/* This console's three letters are final; put them on the wire. */
void tengen_name_send(TengenNameSwap *swap,
                       const uint8_t initials[TENGEN_NAME_LETTERS]);

/* What this console should put on the wire next. */
uint16_t tengen_name_word(const TengenNameSwap *swap);

/* What one transfer carried. `got` is false when it failed, in which case
 * `word` is ignored and only the give-up counter moves. */
void tengen_name_apply(TengenNameSwap *swap, bool got, uint16_t word);

/* True once all three of the other console's letters are in `theirs`. The
 * row can be filled in from this point; `complete` is about the cable. */
static inline bool tengen_name_have(const TengenNameSwap *swap) {
    return swap->got == ((1u << TENGEN_NAME_LETTERS) - 1u);
}

/* ----------------------------------------------------------------------- *
 * The Game Boy Player's rumble
 *
 * Once a game has shown the Player's logo and been recognised, the Player
 * talks to it over the serial port in 32-bit normal mode, on its own clock,
 * about once a frame: first "NINTENDO" in 16-bit pieces with each piece's
 * bitwise inverse, then a few setup words, then 30000003h for ever, which
 * the game answers with 400000yyh — yy 26h for the motor on, 04h for off.
 * GBATEK ("GBA Gameboy Player"), table copied exactly; untested on a
 * Player. This is the answer to one received word; the serial interrupt
 * loads it for the next transfer.
 * ----------------------------------------------------------------------- */
#define TENGEN_GBP_RUMBLE_ON  0x26u
#define TENGEN_GBP_RUMBLE_OFF 0x04u
uint32_t tengen_gbp_reply(uint32_t received, bool rumble);

/* ----------------------------------------------------------------------- *
 * The records a copy made, across to a cartridge that can keep them — and
 * two cartridges' tables, each to the other
 *
 * A SINGLE-PAK COPY HAS NO SAVE. Two copies can play each other for an hour
 * with no cartridge anywhere — one copy sends the game on, the batteries of
 * a flash cart's console last five minutes and a copy's an hour — and every
 * record made that way was gone at the power switch. So the copy keeps the
 * rows it put on its tables in memory, and the next time it links up with a
 * console that has a save, they go across and that console keeps them.
 *
 * AND TWO CARTRIDGES CAN SHOW EACH OTHER THEIR BEST — IF THEIR PLAYERS SAY
 * SO. Each has a save and gave the other nothing: two friends' tables
 * stayed two tables however often they played. But a table is its owner's,
 * and a friend's monstrous scores push the owner's own off the bottom of
 * it: so nothing of a cartridge's goes onto another unasked. Each says a
 * sixteen-bit digest of what it would offer (the rows somebody made at the
 * top of its tables: the caller's choice, fifteen at most); when the two
 * differ, each console ASKS ITS PLAYER whether to add the other's
 * (`asking`, tengen_records_answer), and only a console whose player said
 * yes is sent them. When the digests are the same — two cartridges that
 * have taken each other's and have nothing new — nobody is asked and the
 * exchange is a dozen transfers. A copy is never asked about and never
 * asks: its rows go to a save unasked, as they always did (it was sent by
 * a cartridge an hour ago and is going to be switched off), and nothing is
 * sent to it.
 *
 * WHEN: as the lobby finishes, before the first piece, on every link.
 * Both consoles run one of these; it is short when there is nothing to send.
 *
 * WHAT: each console sends ONE message, a byte at a time:
 *
 *     [flags, digest high, digest low, want, n, n records of 9 bytes, k]
 *
 * flags bit 0 is "this console has a save". The first three bytes go at
 * once. `want` — "send me your rows" — waits for the other's three: a save
 * wants a copy's at once, wants nothing of a save with the same digest,
 * and of a save with another waits for its player's answer; a copy wants
 * nothing. `n` waits for the other's `want`: the rows, if they are wanted,
 * and none otherwise. `k` is how many of the OTHER's rows this console
 * kept — it waits for those rows and for the caller's merge
 * (tengen_records_saved); with none to wait for it is 0 at once.
 *
 * AND THE BUILDS BEFORE THIS ONE, which sent [0, n, records] from a copy
 * and [1, k] from a save and expect the same back: every word of this
 * version carries a mark they never set (bit 11 of the payload), and
 * nothing of the message goes until a word of the other's has arrived, so a
 * console that sees no mark answers in the old form — a copy's rows still
 * reach an older cartridge, and an older copy's still reach this one. Two
 * cartridges of which one is older give each other nothing, as before.
 *
 * HOW: both directions at once, each word carrying one byte of this
 * console's message and the receipt for the other's, alternating bit: byte
 * i goes out with sequence bit i & 1 until the receipt for it comes back,
 * and the receipt is the sequence bit of the last byte taken (1 before the
 * first). A lost or repeated transfer costs a turn, never a byte, and the
 * order the link keeps is all it needs. Payload: bits 0-7 the byte, 8 its
 * sequence bit, 9 the receipt, 10 set when there is a byte at all, 11 the
 * version's mark.
 *
 * THE END: each console lingers once it has everything, as the names swap
 * does and for the same reason (the other may still be waiting for the
 * receipt), and takes a match word in the other's slot as the other having
 * finished: it can only have finished with everything across both ways.
 * ----------------------------------------------------------------------- */
#define TENGEN_RECORDS_MAX 15
#define TENGEN_RECORD_BYTES 9
#define TENGEN_RECORDS_HEAD 5       /* flags, the digest's two bytes, want, n */
#define TENGEN_RECORDS_MSG (TENGEN_RECORDS_HEAD + TENGEN_RECORDS_MAX * TENGEN_RECORD_BYTES + 1)
#define TENGEN_RECORDS_TIMEOUT 300
#define TENGEN_RECORDS_LINGER 8

typedef struct {
    uint8_t table;          /* which build's table: 0 the release's */
    uint32_t score;         /* 0..999999 */
    uint16_t lines;         /* 0..999 */
    uint8_t initials[3];    /* letters as the table keeps them, 0..26 */
} TengenRecord;

typedef struct {
    uint8_t out[TENGEN_RECORDS_MSG];
    uint8_t out_len, out_pos;
    bool out_held;          /* `k` waits for the merge (tengen_records_saved) */
    uint8_t in[TENGEN_RECORDS_MSG];
    uint8_t in_pos, in_len; /* in_len 0: not known yet */
    bool storage;           /* this console has a save */
    bool partner_storage;   /* ...and the other one (known once in_pos > 0) */
    /* For a console with a save: the other's records, once all in, for the
     * caller to merge (tengen_records_saved says how many it kept). */
    TengenRecord theirs[TENGEN_RECORDS_MAX];
    uint8_t theirs_n;
    bool theirs_ready;
    /* How many of this console's records the other kept. */
    uint8_t saved;
    uint8_t linger;
    bool complete, failed;
    uint16_t idle;
    /* Which form the other console speaks: 0 until a word of its has come
     * (nothing is sent meanwhile), 1 the builds before the mark, 2 this. */
    uint8_t version;
    uint8_t mine_n;         /* the records given to tengen_records_start */
    uint16_t digest;        /* ...and their digest */
    uint16_t partner_digest;    /* the other's, once its three bytes are in */
    /* THE PLAYER IS TO BE ASKED whether this console takes the other
     * cartridge's rows: true from the other's digest arriving, different
     * from this one's, until tengen_records_answer. The link is kept
     * turning meanwhile; the other waits. */
    bool asking;
    bool want_decided, want;    /* this console's `want`, once settled */
    bool rows_decided;          /* ...and its `n` */
} TengenRecordSync;

/* `mine`: a copy's rows not handed on yet, or a cartridge's offer. */
void tengen_records_start(TengenRecordSync *sync, bool storage,
                          const TengenRecord *mine, int n);
uint16_t tengen_records_word(const TengenRecordSync *sync);
void tengen_records_apply(TengenRecordSync *sync, bool got, uint16_t word);
/* The player's answer while `asking`: take the other cartridge's rows, or
 * not. Does nothing at any other time. */
void tengen_records_answer(TengenRecordSync *sync, bool take);
/* A console with a save has merged `theirs`: `kept` of them made its tables. */
void tengen_records_saved(TengenRecordSync *sync, uint8_t kept);
/* Everything is across: the message both ways, receipts included. */
bool tengen_records_done(const TengenRecordSync *sync);

#endif /* TENGEN_LINK_H */
