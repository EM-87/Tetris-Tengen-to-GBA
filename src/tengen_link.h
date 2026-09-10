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
 * The frame counter is SEVEN bits, not eight, and the top bit of the word is
 * therefore always zero. That is not tidiness: a GBA reads $FFFF from the
 * slot of a console that is not there, so $FFFF has to stay impossible as a
 * real word or an unplugged cable would look like a player holding every
 * button. Seven bits still wrap far slower than a transfer can be late. */
#define TENGEN_LINK_BUTTON_MASK 0x00FF
#define TENGEN_LINK_FRAME_SHIFT 8
#define TENGEN_LINK_FRAME_MASK  0x7F

static inline uint16_t tengen_link_pack(uint8_t buttons, uint8_t frame) {
    return (uint16_t)(buttons | ((uint16_t)(frame & TENGEN_LINK_FRAME_MASK)
                                  << TENGEN_LINK_FRAME_SHIFT));
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
    uint8_t frame;       /* wraps at 128 on the wire; spots a lost transfer */
    bool desynced;       /* a word arrived for the wrong frame; game is over */
} TengenLink;

/* Both machines call this with IDENTICAL seed and start level — the master
 * sends them across before the first frame — and their own slot. */
void tengen_link_start(TengenLink *link, uint16_t seed, uint8_t start_level,
                        TengenPlayerSlot local_slot);

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
 * largest tagged word is $6FFF, so a lobby word can no more be mistaken for
 * an absent console's $FFFF than a match word can.
 * ----------------------------------------------------------------------- */
typedef enum {
    TENGEN_LOBBY_NONE    = 0,
    TENGEN_LOBBY_HELLO   = 1,
    TENGEN_LOBBY_SEED_HI = 2,
    TENGEN_LOBBY_SEED_LO = 3,
    TENGEN_LOBBY_CONFIG  = 4,   /* start level in bits 0-3, music in bits 4-7 */
    /* A stage of its own rather than four spare bits of CONFIG: two handicaps
     * of nought to four need six bits and CONFIG has four left. One more
     * stop-and-wait turn costs two transfers, which is two sixtieths of a
     * second on a screen the player is already sitting on. */
    TENGEN_LOBBY_HANDICAP = 5,  /* player 1 in bits 0-3, player 2 in bits 4-7 */
    TENGEN_LOBBY_GO      = 6
} TengenLobbyTag;

#define TENGEN_LOBBY_TAG_SHIFT 12
#define TENGEN_LOBBY_PAYLOAD_MASK 0x0FFF

/* How many transfer attempts a lobby tolerates without a single success
 * before giving up: ten seconds at one a frame, long enough to plug a cable
 * in after starting. */
#define TENGEN_LOBBY_TIMEOUT 600

typedef struct {
    uint16_t seed;
    uint8_t start_level;
    uint8_t music;
    uint8_t handicap[2];   /* menuPlayer1Handicap / menuPlayer2Handicap */
    bool ready;          /* the handshake finished; the match may start */
    bool failed;         /* nothing answered for long enough to give up */
    uint8_t stage;       /* master: the tag in flight. slave: the last seen */
    uint8_t echo;        /* slave: the tag it owes back */
    bool saw_go;         /* slave: GO has arrived once already */
    uint16_t idle;       /* consecutive failed transfers */
    bool linked;         /* the other end has answered at least once */
    bool hold;           /* master: stay at HELLO until the player has chosen */
} TengenLobby;

/* Starts a lobby. The master's seed/level/music are the ones that count; on
 * the slave they are overwritten by what arrives. */
void tengen_lobby_start(TengenLobby *lobby, uint16_t seed, uint8_t start_level,
                         uint8_t music);

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
                           const uint8_t handicap[2]);

/* What this machine should put on the wire next. */
uint16_t tengen_lobby_word(const TengenLobby *lobby, bool master);

/* What one transfer attempt did. `got` is false when it failed, in which case
 * the two words are ignored and only the give-up counter moves. */
void tengen_lobby_apply(TengenLobby *lobby, bool master, bool got,
                         uint16_t master_word, uint16_t slave_word);

#endif /* TENGEN_LINK_H */
