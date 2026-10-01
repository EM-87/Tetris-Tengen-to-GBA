/*
 * link.h — the GBA half of a two-player game: the serial cable.
 *
 * The game logic of a linked match, and the handshake that sets one up, are
 * in ../src/tengen_link.c, which is platform-independent and tested on the
 * host. This file is only the wire: it puts one 16-bit word per frame across
 * a GBA link cable and hands back what the other console sent.
 *
 * Multiplayer mode is the right one here: it is the GBA's own protocol for
 * up to four consoles, every console both sends and receives on each
 * transfer, and the hardware decides who is master by which end of the cable
 * is plugged where. One transfer per frame carries a player's buttons with
 * room to spare — and it carries every console's word, including this one's,
 * which is how a machine can tell what it actually managed to send.
 *
 * WHY THE CABLE RUNS ON AN INTERRUPT.
 *
 * Two GBAs have two crystals and two LCDs, and neither is going to agree with
 * the other for long. The master starts a transfer once per frame on its own
 * clock; the slave's frame is a fraction of a percent longer or shorter, so
 * over a few minutes the two drift a whole frame apart. A slave that polls
 * for transfers misses one outright if it happens to be drawing when the
 * master starts it, and sends last frame's buttons if it has not written them
 * yet — either way lockstep is finished. Polling with a wait long enough to
 * be safe is worse still: it costs most of a frame every time nothing is
 * plugged in, so an unattached console crawls.
 *
 * So the serial interrupt drives it. The handler does two things the moment a
 * transfer completes: it queues what arrived, and — once a match is under way
 * — it reads the keypad and loads the NEXT word into the send register. Both
 * happen in microseconds, so the send register is never stale, no transfer is
 * ever missed, and nothing in this file ever blocks or waits.
 *
 * That leaves the main loop free to draw on its own console's vblank, at its
 * own pace, taking whatever the queue holds — usually one transfer per frame,
 * occasionally two or none as the clocks slide past each other. The
 * simulation advances per TRANSFER and the screen redraws per FRAME, and
 * because they are no longer the same thing, a drifting clock costs a
 * repeated or dropped picture rather than a broken game.
 *
 * WHAT COUNTS AS BEING PLUGGED IN. Not the SD bit. A GBA with nothing
 * attached reads SD set and SI set — it looks exactly like a slave waiting
 * for its parent — so believing those bits makes a lone console announce a
 * partner that is not there. The only proof of a cable is a transfer that
 * came back with a real word in both consoles' slots, and that is what
 * link_connected() reports. A console alone therefore waits, and then gives
 * up, which is the truth.
 *
 * SD DOES GATE THE START, though. Low, it is a console on the cable that is
 * not in multiplayer mode yet, and a transfer started into it fails and
 * leaves the port in error until it is started over. On the first real
 * cable the master reached the lobby first, did exactly that, and the two
 * never heard each other. See sio_reset in link.c.
 */
#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include <stdbool.h>

#include "../src/tengen_link.h"

/* A GBA reads $FFFF from the slot of a console that is not there.
 * ../src/tengen_link.h keeps its wire words below this so the two can never
 * be confused. */
#define LINK_ABSENT 0xFFFF

/* Provided by the front end: the keypad, as TengenButton bits. This is called
 * FROM THE SERIAL INTERRUPT, at the instant of a transfer, so that both
 * consoles sample their controllers at the same moment and neither can be
 * late loading the send register. It must do nothing but read the keypad. */
uint8_t link_read_buttons(void);

/* A serial transfer has completed: queue what arrived and, during a match,
 * load the next word. Called by the program's one interrupt handler
 * (irq_handler in video.c), which also owns the acknowledging. */
void link_serial_service(void);

/* Puts the serial hardware in multiplayer mode and arms the interrupt.
 * Call once, on the way into the link screen. */
void link_init(void);

/* Puts it away again. */
void link_shutdown(void);

/* Reads the SI pin, idle only, into the role link_is_master reports before
 * the first good transfer. Called once a frame by link_tick. */
void link_sample_role(void);

/* True when this console is the cable master — the one that starts each
 * transfer, and the one that plays as player 1. Which end of the cable a
 * console is plugged into decides this, not the software. A console with no
 * cable reads as a slave, which is why this alone never means "linked". */
bool link_is_master(void);

/* True when a transfer has recently come back with both consoles present.
 * This, and not the hardware's SD bit, is what "there is somebody there"
 * means — see the note at the top. */
bool link_connected(void);

/* What one completed transfer carried. Both consoles' words are here,
 * including this console's own — which is the only trustworthy account of
 * what it managed to send. */
typedef struct {
    uint16_t master;
    uint16_t slave;
} LinkFrame;

/* The master's once-per-frame job: begin a transfer if none is in flight.
 * Returns immediately — the interrupt collects the result. Does nothing on a
 * slave, which has no say in when transfers happen. */
void link_pump(void);

/* Takes the oldest transfer the interrupt has collected, if any. Call in a
 * loop each frame: there is usually one, sometimes two, sometimes none. */
bool link_pop(LinkFrame *out);

/* Frames since the last completed transfer, so a match can notice the cable
 * being unplugged rather than waiting forever for a word that is not
 * coming. link_tick() is the main loop's once-a-frame contribution to it. */
uint16_t link_starved(void);
void link_tick(void);

/* ----------------------------------------------------------------------- *
 * The lobby
 *
 * Before the first piece both consoles have to agree on the seed and the
 * start level, or their two simulations would not match. The handshake that
 * gets them there is ../src/tengen_link.c's, tested on the host; all this
 * side does is carry one word per frame for it.
 * ----------------------------------------------------------------------- */

/* Starts the handshake and loads its first word onto the wire. */
void link_lobby_start(TengenLobby *lobby, uint16_t seed, uint8_t start_level,
                       uint8_t music);

/* The same, but PARKED at the greeting until link_lobby_release: the two
 * consoles find each other before anybody picks a level or a tune, because on
 * a cable only one of them should be picking and neither knows which until the
 * cable says so. See tengen_lobby_start_held. */
void link_lobby_start_held(TengenLobby *lobby, uint16_t seed);

/* The master's choice, once it has one; lets the handshake run on. */
void link_lobby_release(TengenLobby *lobby, uint16_t seed,
                         uint8_t start_level, uint8_t music,
                         const uint8_t handicap[2], bool coop, bool xe);

/* One frame of it. Call once per frame until `ready` or `failed`. */
void link_lobby_step(TengenLobby *lobby);

/* ----------------------------------------------------------------------- *
 * The match
 * ----------------------------------------------------------------------- */

/* Hands the send register over to the interrupt, which from now on loads
 * each frame's buttons itself, and starts the frame counter at 0. Call once,
 * when the lobby reports ready. */
void link_play_begin(void);

/* Takes it back. The cable stays in multiplayer mode; use link_shutdown() to
 * put the hardware away entirely. */
void link_play_end(void);

/* ----------------------------------------------------------------------- *
 * The records
 *
 * The cable outlives the match by a few seconds, because the one thing
 * lockstep cannot give either console is the NAME the other player typed.
 * See TengenNameSwap for the exchange; this is the wire under it.
 * ----------------------------------------------------------------------- */

/* Arms the swap, takes the send register back off the interrupt, drops
 * whatever the match left in the queue, and loads the first word. */
void link_name_start(TengenNameSwap *swap);

/* One frame of it. Call once a frame until `complete` or `failed`. */
void link_name_step(TengenNameSwap *swap);

/* SINGLE-PAK: sends `image` (a multiboot image, `len` a multiple of 16) to
 * a console on the other end that has no cartridge and is waiting in its
 * BIOS. One attempt; call again on LINK_SEND_NOBODY or _RETRY, and
 * link_init afterwards whatever came back. `progress`, if given, is told
 * how far the image has got now and then, in words. */
typedef enum {
    LINK_SEND_DONE,        /* the other console is running it */
    LINK_SEND_NOBODY,      /* no console waiting answered */
    LINK_SEND_RETRY,       /* one answered and the handshake broke off */
    LINK_SEND_WRONG_END    /* this console is not on the master's end */
} LinkSendResult;
LinkSendResult link_multiboot_send(const uint8_t *image, uint32_t len,
                                    void (*progress)(uint32_t done,
                                                     uint32_t total));

/* At boot: the port in multiplayer mode, silent, as link_shutdown leaves
 * it — so that the other end can tell this console is there. */
void link_rest(void);

/* Once a frame on the menus: the master's end says it is there (see
 * link.c). */
void link_beacon(void);

/* WHAT IS ON THE OTHER END, while the lobby has found nobody to talk to —
 * for the LINK CABLE screen to say what to do next. */
typedef enum {
    LINK_PEER_NOBODY,      /* no transfers, or nothing in the other slot */
    LINK_PEER_SOMEONE,     /* a console in multiplayer mode, not (yet) ours */
    LINK_PEER_EMPTY_GBA    /* one with no cartridge, waiting in its BIOS */
} LinkPeer;

/* Once a frame on the LINK CABLE screen, before link_lobby_step, while the
 * lobby is not linked: now and then asks the cable, the way a Single-Pak
 * send starts, whether a console with no cartridge is waiting there. */
void link_probe(void);
LinkPeer link_peer(void);

/* ----------------------------------------------------------------------- *
 * The Wireless Adapter (wireless.c): the same lobby and lockstep over the
 * air. Found at power-on; from then on link_init, link_pump and the rest
 * run the adapter instead of the cable. See the note at the top of
 * wireless.c.
 * ----------------------------------------------------------------------- */

bool wireless_detect(void);     /* at power-on: is an adapter answering? */
bool wireless_present(void);
void wireless_open(void);       /* link_init's, over the air */
void wireless_close(void);      /* link_shutdown's */
bool wireless_on(void);
bool wireless_linked(void);     /* the two consoles have found each other */
bool wireless_host(void);       /* ...and this one hosts: the master */
bool wireless_hosting(void);    /* waiting, as host, for somebody to join */
void wireless_frame(void);      /* once a frame, from link_pump */
void wireless_match_begin(void);
void wireless_lobby_begin(void);

/* What wireless.c needs of link.c: the word the lobby has loaded, a
 * transfer to queue as if the cable had made it, and "the other console
 * was heard". */
uint16_t link_tx_word(void);
void link_push_pair(uint16_t master, uint16_t slave);
void link_heard(void);

#endif /* LINK_H */
