/* tengen_link.c — the lockstep half of a linked two-player game.
 * See tengen_link.h for why it is shaped this way. */
#include "tengen_link.h"

#include <string.h>

void tengen_link_start(TengenLink *link, uint16_t seed, uint8_t start_level,
                        TengenPlayerSlot local_slot, bool coop, bool xe) {
    memset(link, 0, sizeof(*link));
    /* two_player either way; `coop` is what decides whether that means two
     * separate ten-wide fields or one twelve-wide one between them. The ROM's
     * 2P is a race on independent boards (see the header); its COOPERATIVE
     * leaves the wall nibbles open and shares field[0]. */
    tengen_new_game(&link->game, seed, start_level, true, coop, xe);
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
    lobby->skin_offer = -1;
    lobby->skin = -1;
}

void tengen_lobby_skins(TengenLobby *lobby, const uint16_t *prints, int count,
                         int offer) {
    if (count < 0) count = 0;
    if (count > TENGEN_SKIN_MAX) count = TENGEN_SKIN_MAX;
    for (int i = 0; i < count; i++)
        lobby->skins[i] = (uint16_t)(prints[i] & TENGEN_SKIN_PRINT_MASK);
    lobby->skin_count = (uint8_t)count;
    lobby->skin_offer = (int8_t)((offer >= 0 && offer < count) ? offer : -1);
}

uint16_t tengen_skin_fingerprint(const uint8_t *bytes, unsigned len) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return (uint16_t)((h ^ (h >> 11) ^ (h >> 22)) & TENGEN_SKIN_PRINT_MASK);
}

/* The order the master walks the stages in. SKIN is numbered after the
 * records swap's two tags but runs between HANDICAP and GO. */
static uint8_t next_stage(uint8_t stage) {
    switch (stage) {
        case TENGEN_LOBBY_HANDICAP: return TENGEN_LOBBY_SKIN;
        case TENGEN_LOBBY_SKIN:     return TENGEN_LOBBY_GO;
        default:                    return (uint8_t)(stage + 1);
    }
}

void tengen_lobby_start_held(TengenLobby *lobby, uint16_t seed) {
    tengen_lobby_start(lobby, seed, 0, 0);
    lobby->hold = true;
}

void tengen_lobby_release(TengenLobby *lobby, uint16_t seed,
                           uint8_t start_level, uint8_t music,
                           const uint8_t handicap[2], bool coop, bool xe) {
    lobby->seed = seed;
    lobby->start_level = start_level;
    lobby->music = music;
    lobby->handicap[0] = handicap ? handicap[0] : 0;
    lobby->handicap[1] = handicap ? handicap[1] : 0;
    lobby->coop = coop;
    lobby->xe = xe;
    lobby->hold = false;
}

uint16_t tengen_lobby_word(const TengenLobby *lobby, bool master) {
    if (!master)
        return tagged((TengenLobbyTag)lobby->echo,
                       lobby->echo == TENGEN_LOBBY_SKIN ? lobby->echo_payload : 0);
    switch ((TengenLobbyTag)lobby->stage) {
        case TENGEN_LOBBY_HELLO:
            return tagged(TENGEN_LOBBY_HELLO, 0);
        case TENGEN_LOBBY_SEED_HI:
            return tagged(TENGEN_LOBBY_SEED_HI, (uint16_t)(lobby->seed >> 8));
        case TENGEN_LOBBY_SEED_LO:
            return tagged(TENGEN_LOBBY_SEED_LO, (uint16_t)(lobby->seed & 0xFF));
        case TENGEN_LOBBY_CONFIG:
            /* Bits 0-4 the level, 5-8 the tune, bit 9 says whether the two
             * of them are sharing one board, and bit 10 whether this is an XE
             * game. FIVE bits for the level, not four: XE's range reaches 19,
             * and a four-bit field would have silently handed the other
             * console level 2 for level 18. */
            return tagged(TENGEN_LOBBY_CONFIG,
                           (uint16_t)((lobby->start_level & 0x1F) |
                                      ((lobby->music & 0x0F) << 5) |
                                      (lobby->coop ? 0x200u : 0u) |
                                      (lobby->xe ? 0x400u : 0u)));
        case TENGEN_LOBBY_HANDICAP:
            return tagged(TENGEN_LOBBY_HANDICAP,
                           (uint16_t)((lobby->handicap[0] & 0x0F) |
                                      ((lobby->handicap[1] & 0x0F) << 4)));
        case TENGEN_LOBBY_SKIN:
            if (lobby->skin_offer < 0) return tagged(TENGEN_LOBBY_SKIN, 0);
            return tagged(TENGEN_LOBBY_SKIN,
                           (uint16_t)(TENGEN_SKIN_OFFER |
                                      lobby->skins[lobby->skin_offer]));
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
            /* The slave's answer about the skin rides in its echo. A build
             * that predates the SKIN stage echoes the tag with nothing in
             * it, which reads as "I do not have it" — so an old console and
             * a new one fall back to the release together rather than
             * disagreeing about what a cell holds. */
            if (lobby->stage == TENGEN_LOBBY_SKIN)
                lobby->skin = (lobby->skin_offer >= 0 &&
                               (slave_word & TENGEN_SKIN_HAVE))
                                  ? lobby->skin_offer : -1;
            if (lobby->stage == TENGEN_LOBBY_GO) lobby->ready = true;
            else lobby->stage = next_stage(lobby->stage);
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
            lobby->start_level = (uint8_t)(payload & 0x1F);
            lobby->music = (uint8_t)((payload >> 5) & 0x0F);
            lobby->coop = (payload & 0x200u) != 0;
            lobby->xe = (payload & 0x400u) != 0;
            break;
        case TENGEN_LOBBY_HANDICAP:
            lobby->handicap[0] = (uint8_t)(payload & 0x0F);
            lobby->handicap[1] = (uint8_t)((payload >> 4) & 0x0F);
            break;
        case TENGEN_LOBBY_SKIN:
            /* Found by the ART, not by the index: the two builds may list
             * their skins in different orders, or not have the same ones. */
            lobby->skin = -1;
            if (payload & TENGEN_SKIN_OFFER)
                for (int i = 0; i < lobby->skin_count; i++)
                    if (lobby->skins[i] == (payload & TENGEN_SKIN_PRINT_MASK)) {
                        lobby->skin = (int8_t)i;
                        break;
                    }
            lobby->echo_payload = lobby->skin >= 0 ? TENGEN_SKIN_HAVE : 0;
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

/* ----------------------------------------------------------------------- *
 * The records, after the match
 * ----------------------------------------------------------------------- */

/* Bits 0-4 are the letter, 5-6 its place in the name, and bit 8 is the
 * receipt: "I have all three of yours". Every word carries the receipt,
 * TYPING included, so it can be given before this console has typed a thing. */
#define NAME_LETTER_MASK 0x001F
#define NAME_PLACE_SHIFT 5
#define NAME_RECEIPT     0x0100

void tengen_name_start(TengenNameSwap *swap) {
    memset(swap, 0, sizeof(*swap));
}

void tengen_name_send(TengenNameSwap *swap,
                       const uint8_t initials[TENGEN_NAME_LETTERS]) {
    for (int i = 0; i < TENGEN_NAME_LETTERS; i++)
        swap->mine[i] = (uint8_t)(initials[i] & NAME_LETTER_MASK);
    swap->sending = true;
}

uint16_t tengen_name_word(const TengenNameSwap *swap) {
    uint16_t receipt = tengen_name_have(swap) ? NAME_RECEIPT : 0u;
    if (!swap->sending) return tagged(TENGEN_LOBBY_TYPING, receipt);
    uint8_t place = (uint8_t)(swap->cursor % TENGEN_NAME_LETTERS);
    return tagged(TENGEN_LOBBY_NAME,
                   (uint16_t)((swap->mine[place] & NAME_LETTER_MASK) |
                              ((uint16_t)place << NAME_PLACE_SHIFT) | receipt));
}

void tengen_name_apply(TengenNameSwap *swap, bool got, uint16_t word) {
    if (swap->complete || swap->failed) return;

    if (!got) {
        if (++swap->idle >= TENGEN_NAME_TIMEOUT) swap->failed = true;
        return;
    }

    TengenLobbyTag tag = tag_of(word);
    /* A word that is neither of ours is the tail of something else — the
     * lobby's last GO, a frame of buttons the match left in the queue — and
     * it is not an answer. It does not reset the give-up counter either. */
    if (tag != TENGEN_LOBBY_NAME && tag != TENGEN_LOBBY_TYPING) {
        if (++swap->idle >= TENGEN_NAME_TIMEOUT) swap->failed = true;
        return;
    }
    swap->idle = 0;

    uint16_t payload = word & TENGEN_LOBBY_PAYLOAD_MASK;
    if (tag == TENGEN_LOBBY_NAME) {
        uint8_t place = (uint8_t)((payload >> NAME_PLACE_SHIFT) & 0x03);
        if (place < TENGEN_NAME_LETTERS) {
            swap->theirs[place] = (uint8_t)(payload & NAME_LETTER_MASK);
            swap->got = (uint8_t)(swap->got | (1u << place));
        }
    }
    if (payload & NAME_RECEIPT) swap->acked = true;

    /* One letter a transfer, round and round: a lost word costs one turn of
     * the wheel rather than a stall, which is what buys the lack of a
     * handshake here. */
    if (swap->sending)
        swap->cursor = (uint8_t)((swap->cursor + 1) % TENGEN_NAME_LETTERS);

    if (tengen_name_have(swap) && swap->acked) {
        if (swap->linger >= TENGEN_NAME_LINGER) swap->complete = true;
        else swap->linger++;
    }
}
