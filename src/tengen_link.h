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
    TENGEN_LOBBY_HANDICAP = 5,  /* player 1 in bits 0-3, player 2 in bits 4-7 */
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
    TENGEN_LOBBY_SKIN    = 9
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
} TengenLobby;

/* Starts a lobby. The master's seed/level/music are the ones that count; on
 * the slave they are overwritten by what arrives. */
void tengen_lobby_start(TengenLobby *lobby, uint16_t seed, uint8_t start_level,
                         uint8_t music);

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

#endif /* TENGEN_LINK_H */
