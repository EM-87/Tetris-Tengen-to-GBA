/* tengen_link.c — the lockstep half of a linked two-player game.
 * See tengen_link.h for why it is shaped this way. */
#include "tengen_link.h"

#include <string.h>

void tengen_link_start(TengenLink *link, uint16_t seed, uint8_t start_level,
                        TengenPlayerSlot local_slot) {
    memset(link, 0, sizeof(*link));
    /* two_player, not coop: two separate 10-wide fields, both walled. The
     * ROM's 2P is a race on independent boards (see the header). */
    tengen_new_game(&link->game, seed, start_level, true, false);
    link->local_slot = (uint8_t)local_slot;
    link->frame = 0;
    link->desynced = false;
}

bool tengen_link_step(TengenLink *link, uint8_t local_buttons,
                       uint16_t remote_word, TengenStepResult out[2]) {
    if (out) {
        memset(&out[0], 0, sizeof(out[0]));
        memset(&out[1], 0, sizeof(out[1]));
    }
    if (link->desynced) return false;

    /* A word from the wrong frame means a transfer was lost, and from here on
     * the two machines would be simulating different games. Stop rather than
     * drift: a desynced race is worse than an ended one. */
    if (tengen_link_frame(remote_word) != (link->frame & TENGEN_LINK_FRAME_MASK)) {
        link->desynced = true;
        return false;
    }

    uint8_t remote_buttons = tengen_link_buttons(remote_word);

    /* THE INVARIANT. Player 1's buttons come from whichever machine is player
     * 1, player 2's from the other, and both are stepped in that order on
     * both machines. Reorder this and the two simulations diverge in a way no
     * amount of network code can repair. */
    uint8_t buttons[2];
    buttons[link->local_slot] = local_buttons;
    buttons[link->local_slot ^ 1] = remote_buttons;

    /* Pause and the cheat codes run first, exactly as in a solo game, and
     * with both players' fresh presses — the ROM ORs the two controllers for
     * the Start check, so either player can pause a linked game too. */
    uint8_t presses[2];
    for (int i = 0; i < 2; i++) {
        presses[i] = (uint8_t)(buttons[i] & ~link->game.player[i].held_last_frame);
    }
    tengen_pause_input(&link->game, presses, 0);

    TengenStepResult results[2];
    results[0] = tengen_step(&link->game, TENGEN_PLAYER_1, buttons[0]);
    results[1] = tengen_step(&link->game, TENGEN_PLAYER_2, buttons[1]);
    if (out) {
        out[0] = results[0];
        out[1] = results[1];
    }

    link->frame++;
    return true;
}

/* ----------------------------------------------------------------------- *
 * The lobby
 * ----------------------------------------------------------------------- */

static uint16_t tagged(TengenLobbyTag tag, uint16_t payload) {
    return (uint16_t)(((uint16_t)tag << TENGEN_LOBBY_TAG_SHIFT) |
                       (payload & TENGEN_LOBBY_PAYLOAD_MASK));
}

static TengenLobbyTag tag_of(uint16_t word) {
    return (TengenLobbyTag)(word >> TENGEN_LOBBY_TAG_SHIFT);
}

void tengen_lobby_start(TengenLobby *lobby, uint16_t seed, uint8_t start_level,
                         uint8_t music) {
    memset(lobby, 0, sizeof(*lobby));
    lobby->seed = seed;
    lobby->start_level = start_level;
    lobby->music = music;
    lobby->stage = TENGEN_LOBBY_HELLO;
    lobby->echo = TENGEN_LOBBY_NONE;
}

void tengen_lobby_start_held(TengenLobby *lobby, uint16_t seed) {
    tengen_lobby_start(lobby, seed, 0, 0);
    lobby->hold = true;
}

void tengen_lobby_release(TengenLobby *lobby, uint16_t seed,
                           uint8_t start_level, uint8_t music,
                           const uint8_t handicap[2]) {
    lobby->seed = seed;
    lobby->start_level = start_level;
    lobby->music = music;
    lobby->handicap[0] = handicap ? handicap[0] : 0;
    lobby->handicap[1] = handicap ? handicap[1] : 0;
    lobby->hold = false;
}

uint16_t tengen_lobby_word(const TengenLobby *lobby, bool master) {
    if (!master) return tagged((TengenLobbyTag)lobby->echo, 0);
    switch ((TengenLobbyTag)lobby->stage) {
        case TENGEN_LOBBY_HELLO:
            return tagged(TENGEN_LOBBY_HELLO, 0);
        case TENGEN_LOBBY_SEED_HI:
            return tagged(TENGEN_LOBBY_SEED_HI, (uint16_t)(lobby->seed >> 8));
        case TENGEN_LOBBY_SEED_LO:
            return tagged(TENGEN_LOBBY_SEED_LO, (uint16_t)(lobby->seed & 0xFF));
        case TENGEN_LOBBY_CONFIG:
            return tagged(TENGEN_LOBBY_CONFIG,
                           (uint16_t)((lobby->start_level & 0x0F) |
                                      ((lobby->music & 0x0F) << 4)));
        case TENGEN_LOBBY_HANDICAP:
            return tagged(TENGEN_LOBBY_HANDICAP,
                           (uint16_t)((lobby->handicap[0] & 0x0F) |
                                      ((lobby->handicap[1] & 0x0F) << 4)));
        default:
            return tagged(TENGEN_LOBBY_GO, 0);
    }
}

void tengen_lobby_apply(TengenLobby *lobby, bool master, bool got,
                         uint16_t master_word, uint16_t slave_word) {
    if (lobby->ready || lobby->failed) return;

    if (!got) {
        if (++lobby->idle >= TENGEN_LOBBY_TIMEOUT) lobby->failed = true;
        return;
    }
    lobby->idle = 0;

    if (master) {
        /* Stop-and-wait: advance only when the slave echoes the tag it was
         * sent. That echo is always one transfer behind — which is what makes
         * this a handshake rather than a hope — so every stage costs two
         * transfers and a console that missed one cannot be skipped past. */
        if (tag_of(slave_word) == (TengenLobbyTag)lobby->stage) {
            lobby->linked = true;
            /* Held at HELLO until the player has chosen. The echo still comes
             * back every transfer, so neither end is anywhere near its
             * timeout; the handshake is simply parked. */
            if (lobby->hold && lobby->stage == TENGEN_LOBBY_HELLO) return;
            if (lobby->stage == TENGEN_LOBBY_GO) lobby->ready = true;
            else lobby->stage++;
        }
        return;
    }

    /* The slave takes whatever the master says and echoes the tag back. */
    TengenLobbyTag tag = tag_of(master_word);
    uint16_t payload = master_word & TENGEN_LOBBY_PAYLOAD_MASK;
    switch (tag) {
        case TENGEN_LOBBY_SEED_HI:
            lobby->seed = (uint16_t)((lobby->seed & 0x00FF) | ((payload & 0xFF) << 8));
            break;
        case TENGEN_LOBBY_SEED_LO:
            lobby->seed = (uint16_t)((lobby->seed & 0xFF00) | (payload & 0xFF));
            break;
        case TENGEN_LOBBY_CONFIG:
            lobby->start_level = (uint8_t)(payload & 0x0F);
            lobby->music = (uint8_t)((payload >> 4) & 0x0F);
            break;
        case TENGEN_LOBBY_HANDICAP:
            lobby->handicap[0] = (uint8_t)(payload & 0x0F);
            lobby->handicap[1] = (uint8_t)((payload >> 4) & 0x0F);
            break;
        case TENGEN_LOBBY_GO:
            /* NOT ready on the first GO. The master is still waiting to see
             * one come back, and that acknowledgement is the next transfer.
             * Leaving now would put the slave in the match while the master
             * was still in the lobby, and the slave would read the master's
             * last GO as a frame of buttons. One more turn round the loop and
             * the two leave together. */
            if (lobby->saw_go) lobby->ready = true;
            lobby->saw_go = true;
            break;
        default:
            break;
    }
    if (tag != TENGEN_LOBBY_NONE) {
        lobby->echo = (uint8_t)tag;
        lobby->linked = true;
    }
    lobby->stage = (uint8_t)tag;
}
