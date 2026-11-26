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
    /* The cartridge's bugs mended on the master's chord, which is what `xe`
     * carries over the wire too: both consoles, the same game. */
    link->game.mended = xe;
    /* UP's drop and the faster hands are not the chord's any more but the
     * master's OPTIONS: off until tengen_link_rules says. */
    link->local_slot = (uint8_t)local_slot;
    link->frame = 0;
    link->desynced = false;
}

void tengen_link_rules(TengenLink *link, uint8_t rules) {
    const uint8_t both = (1u << TENGEN_PLAYER_1) | (1u << TENGEN_PLAYER_2);
    link->game.hard_drop = (rules & TENGEN_RULE_HARD_DROP) != 0;
    link->game.fast_soft = (rules & TENGEN_RULE_FAST_DOWN) ? both : 0;
    link->game.fast_shift = (rules & TENGEN_RULE_FAST_SHIFT) ? both : 0;
    link->game.hold = (rules & TENGEN_RULE_HOLD) ? both : 0;
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

void tengen_link_end(TengenLink *link, TengenStepResult out[2]) {
    memset(&out[0], 0, sizeof(out[0]));
    memset(&out[1], 0, sizeof(out[1]));
    bool said = false;
    for (int p = 0; p < 2; p++) {
        if (!link->game.player[p].game_active) continue;
        link->game.player[p].game_active = false;
        /* (One board between two players is lost once: top_out.) */
        if (!(link->game.coop && said)) out[p].topped_out = true;
        said = true;
    }
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
    lobby->stage = TENGEN_LOBBY_HELLO;   /* the master's; see tengen_lobby_apply */
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

void tengen_lobby_rules(TengenLobby *lobby, uint8_t rules) {
    lobby->rules = (uint8_t)(rules & TENGEN_RULES_ALL);
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

/* What this console's HELLO says of why it is here. */
static uint16_t hello_of(const TengenLobby *lobby) {
    return (uint16_t)((lobby->mode_coop ? TENGEN_HELLO_COOP : 0) |
                      (lobby->mode_demo ? TENGEN_HELLO_DEMO : 0));
}

/* ...and of which build it is: a player's lobby only (tengen_lobby_version). */
static uint16_t hello_word(const TengenLobby *lobby) {
    uint16_t version = lobby->mode_demo ? 0 : lobby->version;
    return (uint16_t)(hello_of(lobby) | (version << TENGEN_HELLO_VERSION_SHIFT));
}

static void hello_heard(TengenLobby *lobby, uint16_t word) {
    lobby->their_version = (uint8_t)((word & TENGEN_HELLO_VERSION_MASK) >>
                                     TENGEN_HELLO_VERSION_SHIFT);
    lobby->heard_version = true;
}

uint16_t tengen_lobby_word(const TengenLobby *lobby, bool master) {
    if (!master)
        return tagged((TengenLobbyTag)lobby->echo,
                       lobby->echo == TENGEN_LOBBY_SKIN  ? lobby->echo_payload
                     : lobby->echo == TENGEN_LOBBY_HELLO ? hello_word(lobby)
                     : 0);
    switch ((TengenLobbyTag)lobby->stage) {
        case TENGEN_LOBBY_HELLO:
            return tagged(TENGEN_LOBBY_HELLO, hello_word(lobby));
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
                                      ((lobby->handicap[1] & 0x0F) << 4) |
                                      ((lobby->rules & TENGEN_RULES_ALL) << 8)));
        case TENGEN_LOBBY_SKIN:
            if (lobby->skin_offer < 0) return tagged(TENGEN_LOBBY_SKIN, 0);
            return tagged(TENGEN_LOBBY_SKIN,
                           (uint16_t)(TENGEN_SKIN_OFFER |
                                      lobby->skins[lobby->skin_offer]));
        default:
            return tagged(TENGEN_LOBBY_GO, 0);
    }
}

void tengen_lobby_mode(TengenLobby *lobby, bool coop) {
    lobby->mode_coop = coop;
    lobby->mode_any = false;
}

void tengen_lobby_mode_any(TengenLobby *lobby) {
    lobby->mode_any = true;
}

void tengen_lobby_demo(TengenLobby *lobby) {
    lobby->mode_demo = true;
}

void tengen_lobby_version(TengenLobby *lobby, uint8_t version) {
    lobby->version = version;
}

int tengen_lobby_version_gap(const TengenLobby *lobby) {
    if (!lobby->heard_version || !lobby->version) return 0;
    return (int)lobby->their_version - (int)lobby->version;
}

/* THIS CONSOLE TURNED OUT TO BE THE OTHER ONE. Its role comes off the cable
 * (the SI pin, then the ID bits of a good transfer), and a console can find
 * out a few frames into a lobby that it is not what it took itself for. What
 * it had built up in the wrong role means nothing in the right one — a slave
 * that had "linked" as a master would take its own echo for the partner's —
 * so the conversation starts again, keeping the seed, the settings and the
 * skins: holding at HELLO for a master, having heard nothing for a slave. */
void tengen_lobby_forget(TengenLobby *lobby, bool master) {
    lobby->ready = false;
    lobby->linked = false;
    lobby->idle = 0;
    lobby->saw_go = false;
    lobby->echo = TENGEN_LOBBY_NONE;
    lobby->echo_payload = 0;
    lobby->hold = true;
    lobby->stage = master ? TENGEN_LOBBY_HELLO : TENGEN_LOBBY_NONE;
    lobby->their_version = 0;
    lobby->heard_version = false;
}

/* ...and back: which stage's echo is still legitimately in flight. */
static uint8_t prev_stage(uint8_t stage) {
    switch (stage) {
        case TENGEN_LOBBY_SKIN:  return TENGEN_LOBBY_HANDICAP;
        case TENGEN_LOBBY_GO:    return TENGEN_LOBBY_SKIN;
        case TENGEN_LOBBY_HELLO: return TENGEN_LOBBY_NONE;
        default:                 return (uint8_t)(stage - 1);
    }
}

/* THE PARTNER WENT AWAY: back to waiting, never to failing. A lobby that has
 * heard its partner and then goes TENGEN_LOBBY_LOST turns without a proper
 * answer — no transfer at all, or a word from a console that is not in a
 * lobby — forgets it. The master goes back to holding at HELLO (its player
 * chooses again when a partner turns up), the slave to having seen nothing.
 * Before the first answer nothing counts: a lobby waits as long as it
 * takes, and B is the way out. */
static void lobby_quiet_turn(TengenLobby *lobby, bool master) {
    if (!lobby->linked) return;
    if (++lobby->idle < TENGEN_LOBBY_LOST) return;
    lobby->linked = false;
    lobby->idle = 0;
    lobby->saw_go = false;
    lobby->echo = TENGEN_LOBBY_NONE;
    lobby->echo_payload = 0;
    if (master) {
        lobby->stage = TENGEN_LOBBY_HELLO;
        lobby->hold = true;
    } else {
        lobby->stage = TENGEN_LOBBY_NONE;
    }
}

void tengen_lobby_apply(TengenLobby *lobby, bool master, bool got,
                         uint16_t master_word, uint16_t slave_word) {
    if (lobby->ready) return;
    if (!got) {
        lobby_quiet_turn(lobby, master);
        return;
    }
    /* A GO WITH SOMETHING IN IT IS NOT OURS. It is the $6200 a master
     * sends, the way a Single-Pak send starts, to ask whether a console
     * with no cartridge is on the cable (link_probe) — tag GO by accident,
     * and taken for one: a slave walking in halfway through a handshake
     * went into the match on it, alone. Our GO is always empty, so the
     * probe is heard as nothing at all. */
    if (!master && tag_of(master_word) == TENGEN_LOBBY_GO &&
        (master_word & TENGEN_LOBBY_PAYLOAD_MASK))
        return;

    if (master) {
        TengenLobbyTag echo = tag_of(slave_word);
        /* Stop-and-wait: advance only when the slave echoes the tag it was
         * sent. That echo is always one transfer behind — which is what makes
         * this a handshake rather than a hope — so every stage costs two
         * transfers and a console that missed one cannot be skipped past. */
        /* THE SLAVE MAY BE IN THE MATCH ALREADY: at GO, a match word in its
         * slot is the last echo, arrived as the first move. */
        if (lobby->stage == TENGEN_LOBBY_GO &&
            (tengen_link_is_match_word(slave_word) ||
             tag_of(slave_word) == TENGEN_LOBBY_RECORDS)) {
            lobby->ready = true;
            return;
        }
        if (echo == TENGEN_LOBBY_HELLO) hello_heard(lobby, slave_word);
        /* An echo of HELLO from a console that came for the other mode is
         * not an answer (tengen_lobby_mode) — nor one from a console with a
         * player in its lobby to a demo, or the other way (tengen_lobby_demo). */
        if (echo == TENGEN_LOBBY_HELLO &&
            (slave_word & (TENGEN_HELLO_COOP | TENGEN_HELLO_DEMO)) != hello_of(lobby)) {
            lobby_quiet_turn(lobby, master);
            if (lobby->stage != TENGEN_LOBBY_HELLO) lobby->stage = TENGEN_LOBBY_HELLO;
            return;
        }
        if (echo == (TengenLobbyTag)lobby->stage) {
            lobby->idle = 0;
            lobby->linked = true;
            /* Held at HELLO until the player has chosen. The echo still comes
             * back every transfer, so the partner is never taken for gone;
             * the handshake is simply parked. */
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
            return;
        }
        /* NONE is a console that is not in a lobby, or one that has just come
         * into it: not an answer. */
        if (echo == TENGEN_LOBBY_NONE) lobby_quiet_turn(lobby, master);
        else lobby->idle = 0;
        /* The last stage's echo is the one still in flight: wait for this
         * one's. ANYTHING ELSE is a slave in another conversation — one that
         * came into the lobby halfway through this one, or left and came
         * back — and walking on from here would leave it with half of this
         * game's settings and half of its own (a seed of its own, a board
         * that is not the master's). So start again from HELLO; the settings
         * are kept, and the slave hears all of them, in order. */
        if (echo != (TengenLobbyTag)prev_stage(lobby->stage) &&
            lobby->stage != TENGEN_LOBBY_HELLO)
            lobby->stage = TENGEN_LOBBY_HELLO;
        return;
    }

    /* THE SLAVE HEARS THE HANDSHAKE IN ORDER OR NOT AT ALL. HELLO opens a
     * conversation (and forgets anything from an older one — a GO seen in a
     * session that never finished would otherwise let this console into the
     * match a turn before the master); after it, only the stage it is on or
     * the next one is taken. Anything else is answered with NONE, which
     * sends the master back to HELLO. */
    /* ...and the master may be: having seen one GO, a match word from it
     * means it took this console's echo and went. */
    /* ...or the records exchange that follows GO (TengenRecordSync). */
    if (lobby->saw_go && (tengen_link_is_match_word(master_word) ||
                          tag_of(master_word) == TENGEN_LOBBY_RECORDS)) {
        lobby->ready = true;
        return;
    }
    TengenLobbyTag tag = tag_of(master_word);
    uint16_t payload = master_word & TENGEN_LOBBY_PAYLOAD_MASK;
    /* A slave that follows takes the master's mode as its own (and says so
     * in its echo); one that chose does not follow a master that chose the
     * other. */
    if (tag == TENGEN_LOBBY_HELLO) hello_heard(lobby, payload);
    if (tag == TENGEN_LOBBY_HELLO && lobby->mode_any)
        lobby->mode_coop = (payload & TENGEN_HELLO_COOP) != 0;
    /* ...and nobody follows a demo's HELLO but a demo (tengen_lobby_demo). */
    if (tag == TENGEN_LOBBY_HELLO &&
        (payload & (TENGEN_HELLO_COOP | TENGEN_HELLO_DEMO)) != hello_of(lobby)) {
        lobby_quiet_turn(lobby, false);
        lobby->stage = TENGEN_LOBBY_NONE;
        lobby->echo = TENGEN_LOBBY_NONE;
        lobby->saw_go = false;
        return;
    }
    bool in_order =
        tag == TENGEN_LOBBY_HELLO ||
        (lobby->linked && lobby->stage != TENGEN_LOBBY_NONE &&
         (tag == (TengenLobbyTag)lobby->stage ||
          tag == (TengenLobbyTag)next_stage(lobby->stage)));
    if (!in_order) {
        if (tag == TENGEN_LOBBY_NONE) lobby_quiet_turn(lobby, false);
        else lobby->idle = 0;
        lobby->stage = TENGEN_LOBBY_NONE;
        lobby->echo = TENGEN_LOBBY_NONE;
        lobby->saw_go = false;
        return;
    }
    lobby->idle = 0;
    switch (tag) {
        case TENGEN_LOBBY_HELLO:
            if (lobby->stage != TENGEN_LOBBY_HELLO) {
                lobby->saw_go = false;
                lobby->echo_payload = 0;
            }
            break;
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
            lobby->rules = (uint8_t)((payload >> 8) & TENGEN_RULES_ALL);
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
    lobby->echo = (uint8_t)tag;
    lobby->linked = true;
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

/* GBATEK's table, "Receive / Response", row by row. The first word only
 * has to END in "NI" (494Eh: it arrives as 0000494Eh and then as whatever
 * was in the register), and anything not in the table is answered as the
 * table's last row is — the steady state — so a transfer the Player drops
 * and repeats lands back in step. */
uint32_t tengen_gbp_reply(uint32_t received, bool rumble) {
    switch (received) {
        case 0xB6B1494Eu: return 0x544EB6B1u;
        case 0xB6B1544Eu: return 0x544EABB1u;
        case 0xABB1544Eu: return 0x4E45ABB1u;
        case 0xABB14E45u: return 0x4E45B1BAu;
        case 0xB1BA4E45u: return 0x4F44B1BAu;
        case 0xB1BA4F44u: return 0x4F44B0BBu;
        case 0xB0BB4F44u: return 0x8000B0BBu;
        case 0xB0BB8002u: return 0x10000010u;
        case 0x10000010u: return 0x20000013u;
        case 0x20000013u: return 0x40000004u;
        default: break;
    }
    if ((received & 0xFFFFu) == 0x494Eu) return 0x494EB6B1u;
    return 0x40000000u | (rumble ? TENGEN_GBP_RUMBLE_ON : TENGEN_GBP_RUMBLE_OFF);
}

/* ----------------------------------------------------------------------- *
 * The records a copy made, and two cartridges' tables (TengenRecordSync)
 * ----------------------------------------------------------------------- */
#define REC_SEQ   0x100u
#define REC_ACK   0x200u
#define REC_HAVE  0x400u
#define REC_MARK  0x800u    /* this version's: every word of it carries it */
#define REC_STORAGE 0x01u   /* the message's first byte, bit 0 */

static void put_record(uint8_t *at, const TengenRecord *r) {
    at[0] = r->table;
    at[1] = (uint8_t)(r->score >> 16);
    at[2] = (uint8_t)(r->score >> 8);
    at[3] = (uint8_t)r->score;
    at[4] = (uint8_t)(r->lines >> 8);
    at[5] = (uint8_t)r->lines;
    for (int c = 0; c < 3; c++) at[6 + c] = r->initials[c];
}

static void get_record(const uint8_t *at, TengenRecord *r) {
    r->table = at[0];
    r->score = ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
    r->lines = (uint16_t)((at[4] << 8) | at[5]);
    for (int c = 0; c < 3; c++) r->initials[c] = at[6 + c];
}

/* Sixteen bits of a set of records, whatever order they are in: two builds
 * walk their tables in different orders and must still agree that they hold
 * the same rows. */
static uint16_t records_digest(const uint8_t *rows, int n) {
    uint16_t sum = 0;
    for (int i = 0; i < n; i++) {
        uint32_t h = 2166136261u;
        for (int b = 0; b < TENGEN_RECORD_BYTES; b++) {
            h ^= rows[i * TENGEN_RECORD_BYTES + b];
            h *= 16777619u;
        }
        sum = (uint16_t)(sum + (uint16_t)(h ^ (h >> 16)));
    }
    return sum;
}

void tengen_records_start(TengenRecordSync *sync, bool storage,
                          const TengenRecord *mine, int n) {
    memset(sync, 0, sizeof(*sync));
    sync->storage = storage;
    if (n < 0 || !mine) n = 0;
    if (n > TENGEN_RECORDS_MAX) n = TENGEN_RECORDS_MAX;
    sync->mine_n = (uint8_t)n;
    /* Where this version's message has them; the older form moves them. */
    for (int i = 0; i < n; i++)
        put_record(&sync->out[TENGEN_RECORDS_HEAD + i * TENGEN_RECORD_BYTES], &mine[i]);
    sync->digest = records_digest(&sync->out[TENGEN_RECORDS_HEAD], n);
    /* ...and nothing goes until a word of the other's says which form it
     * speaks (records_form). */
}

/* The message in the form the other console speaks. */
static void records_form(TengenRecordSync *sync, uint8_t version) {
    sync->version = version;
    if (version == 2) {
        sync->out[0] = sync->storage ? REC_STORAGE : 0;
        sync->out[1] = (uint8_t)(sync->digest >> 8);
        sync->out[2] = (uint8_t)sync->digest;
        sync->out_len = 3;              /* the rest as each byte is settled */
        return;
    }
    /* THE FORM BEFORE THE MARK: a save says [1, k] and gives nothing; a copy
     * says [0, n, its records]. */
    if (sync->storage) {
        sync->out[0] = 1;
        sync->out_len = 2;
        sync->out_held = true;          /* `k` once the other's are merged */
        return;
    }
    memmove(&sync->out[2], &sync->out[TENGEN_RECORDS_HEAD],
            (size_t)sync->mine_n * TENGEN_RECORD_BYTES);
    sync->out[0] = 0;
    sync->out[1] = sync->mine_n;
    sync->out_len = (uint8_t)(2 + sync->mine_n * TENGEN_RECORD_BYTES);
}

/* How many bytes of `out` may go: none before the other's form is known
 * (out_len is 0 until records_form), then all there are so far, bar a `k`
 * that is waiting for the merge — and in this version's form nothing past
 * the digest before this console's `want`, the fourth byte, is settled,
 * whatever is already written behind it (the other's `want` may come
 * first, and with it this console's `n`). */
static uint8_t records_sendable(const TengenRecordSync *sync) {
    if (sync->version == 2 && !sync->want_decided) return 3;
    return (uint8_t)(sync->out_len - (sync->out_held ? 1 : 0));
}

uint16_t tengen_records_word(const TengenRecordSync *sync) {
    uint16_t word = (uint16_t)((TENGEN_LOBBY_RECORDS << TENGEN_LOBBY_TAG_SHIFT) |
                               REC_MARK);
    if (sync->out_pos < records_sendable(sync)) {
        word |= REC_HAVE | sync->out[sync->out_pos];
        if (sync->out_pos & 1) word |= REC_SEQ;
    }
    /* The receipt: the sequence bit of the last byte taken, 1 before any. */
    if (sync->in_pos == 0 || ((sync->in_pos - 1) & 1)) word |= REC_ACK;
    return word;
}

/* Everything across both ways. Not before both consoles' players have been
 * heard, and that needs no saying here: the other's message has a length
 * only from its fifth byte, this one's `n` and `k` are added to `out` at its
 * fourth, and they do not go (records_sendable) before this console's
 * `want` is settled. */
bool tengen_records_done(const TengenRecordSync *sync) {
    return sync->version && sync->out_pos == sync->out_len && sync->in_len &&
           sync->in_pos == sync->in_len;
}

/* This console's `want` is settled: the message's fourth byte. (Its fifth
 * and the rest are the other console's to settle, and may be already.) */
static void records_want(TengenRecordSync *sync, bool want) {
    if (sync->want_decided) return;
    sync->want_decided = true;
    sync->want = want;
    sync->asking = false;
    sync->out[3] = want ? 1 : 0;
    /* ...and the message is that long, until the other's `want` says how
     * many rows follow (if it has not already). */
    if (!sync->rows_decided) sync->out_len = 4;
}

void tengen_records_answer(TengenRecordSync *sync, bool take) {
    if (sync->asking) records_want(sync, take);
}

void tengen_records_saved(TengenRecordSync *sync, uint8_t kept) {
    if (!sync->storage || !sync->out_held) return;
    sync->out[sync->out_len - 1] = kept;
    sync->out_held = false;
}

/* The other's records, out of `in` from `at`, for the caller to merge. */
static void records_theirs(TengenRecordSync *sync, int at, int n) {
    sync->theirs_n = (uint8_t)n;
    for (int i = 0; i < n; i++)
        get_record(&sync->in[at + i * TENGEN_RECORD_BYTES], &sync->theirs[i]);
    if (sync->storage) sync->theirs_ready = true;
    else sync->theirs_n = 0;            /* nowhere to keep them */
}

static void records_take(TengenRecordSync *sync, uint8_t byte) {
    if (sync->in_len && sync->in_pos >= sync->in_len) return;
    sync->in[sync->in_pos++] = byte;
    if (sync->in_pos == 1) sync->partner_storage = (byte & REC_STORAGE) != 0;

    if (sync->version == 1) {
        if (sync->in_pos == 1 && sync->partner_storage) {
            sync->in_len = 2;
            /* Two consoles with saves, one of them older: nothing to give. */
            tengen_records_saved(sync, 0);
        } else if (sync->in_pos == 2 && !sync->partner_storage) {
            uint8_t n = byte > TENGEN_RECORDS_MAX ? TENGEN_RECORDS_MAX : byte;
            sync->in_len = (uint8_t)(2 + n * TENGEN_RECORD_BYTES);
        }
        if (!sync->in_len || sync->in_pos < sync->in_len) return;
        /* All of it. */
        if (sync->partner_storage) sync->saved = sync->in[1];
        else records_theirs(sync, 2, (sync->in_len - 2) / TENGEN_RECORD_BYTES);
        return;
    }

    if (sync->in_pos == 3) {
        /* ITS DIGEST IS IN, AND WITH IT WHAT THIS CONSOLE WANTS OF IT: a
         * copy's rows, always; a save's, only if they are not this one's
         * own and its player says so (asking); nothing, if this is a copy. */
        sync->partner_digest = (uint16_t)((sync->in[1] << 8) | sync->in[2]);
        if (!sync->storage) records_want(sync, false);
        else if (!sync->partner_storage) records_want(sync, true);
        else if (sync->partner_digest == sync->digest) records_want(sync, false);
        else sync->asking = true;
    } else if (sync->in_pos == 4) {
        /* ITS `want`, AND WITH IT THIS CONSOLE'S `n`: its rows if they are
         * wanted, none if not. */
        uint8_t n = byte ? sync->mine_n : 0;
        sync->out[4] = n;
        sync->out_len = (uint8_t)(TENGEN_RECORDS_HEAD + n * TENGEN_RECORD_BYTES + 1);
        sync->out[sync->out_len - 1] = 0;
        sync->rows_decided = true;
        /* Its `k` waits for the rows this console asked for, if any come
         * (settled below, when the other's `n` is in). */
        sync->out_held = sync->storage;
    } else if (sync->in_pos == TENGEN_RECORDS_HEAD) {
        uint8_t n = byte > TENGEN_RECORDS_MAX ? TENGEN_RECORDS_MAX : byte;
        sync->in_len = (uint8_t)(TENGEN_RECORDS_HEAD + n * TENGEN_RECORD_BYTES + 1);
    }
    if (!sync->in_len) return;
    /* Its rows — or none, and then there is nothing for the caller to merge
     * and this console's `k` is 0 at once — and after them its `k`. */
    if (sync->in_pos == sync->in_len - 1) {
        int n = (sync->in_len - TENGEN_RECORDS_HEAD - 1) / TENGEN_RECORD_BYTES;
        /* ...and only rows that were asked for: nothing gets onto a table
         * because the other console chose to send it. */
        if (n && sync->want) records_theirs(sync, TENGEN_RECORDS_HEAD, n);
        if (!sync->theirs_ready) tengen_records_saved(sync, 0);
    } else if (sync->in_pos == sync->in_len) {
        sync->saved = byte;
    }
}

void tengen_records_apply(TengenRecordSync *sync, bool got, uint16_t word) {
    if (sync->complete || sync->failed) return;
    if (!got || (!tengen_link_is_match_word(word) &&
                 tag_of(word) != TENGEN_LOBBY_RECORDS)) {
        /* Nothing, or a word from before this (a GO the master is still
         * sending while it waits for its echo): not an answer. */
        if (++sync->idle >= TENGEN_RECORDS_TIMEOUT) sync->failed = true;
        return;
    }
    if (tengen_link_is_match_word(word)) {
        /* The other one finished, which it can only do with everything
         * across both ways: this one has all of it too. */
        sync->complete = true;
        return;
    }
    sync->idle = 0;
    /* Its first word says which form it speaks, and so which this console
     * answers in. */
    if (!sync->version) records_form(sync, (word & REC_MARK) ? 2 : 1);
    /* Its receipt for the byte in flight. */
    if (sync->out_pos < records_sendable(sync) &&
        (((word & REC_ACK) != 0) == ((sync->out_pos & 1) != 0)))
        sync->out_pos++;
    /* Its byte, if it is the next one. */
    if ((word & REC_HAVE) &&
        (((word & REC_SEQ) != 0) == ((sync->in_pos & 1) != 0)))
        records_take(sync, (uint8_t)(word & 0xFF));
    if (tengen_records_done(sync) && ++sync->linger > TENGEN_RECORDS_LINGER)
        sync->complete = true;
}
