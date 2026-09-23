/* link.c — GBA serial multiplayer mode. See link.h for the shape of it. */
#include "link.h"
#include "gba_hw.h"

/* The serial registers. Declared here rather than in gba_hw.h: nothing else
 * in the port talks to the cable. The interrupt registers are shared with the
 * vertical blank and live there. */
#define REG_SIOCNT      (*(vu16 *)0x04000128)
#define REG_SIOMLT_SEND (*(vu16 *)0x0400012A)
#define REG_SIOMULTI(n) (*(vu16 *)(0x04000120 + (n) * 2))
#define REG_RCNT        (*(vu16 *)0x04000134)

/* SIOCNT, multiplayer mode.
 *
 * Bits 0-1 are the baud rate, 12-13 the mode and 14 the interrupt enable;
 * the rest are the hardware's to write, not ours:
 *   bit 2  SI   0 on the master, 1 on a slave — set by which end of the
 *               cable this console is plugged into. A console with nothing
 *               attached reads 1, i.e. it looks like a slave whose parent
 *               has not spoken yet, which is exactly how this file treats
 *               it.
 *   bit 3  SD   nominally "every console is ready", but it reads SET on a
 *               console with no cable at all, so it is not used here at all.
 *               See link_connected().
 *   bit 6  ERR  the last transfer failed.
 *   bit 7  START/BUSY — the master writes 1 to begin a transfer; on every
 *               console it reads 1 while one is in progress.
 */
#define SIO_MODE_MULTI  0x2000
#define SIO_BAUD_115200 0x0003
#define SIO_SI          0x0004
#define SIO_START       0x0080
#define SIO_IRQ         0x4000

/* The interrupt hands transfers to the main loop through this. Four is
 * plenty: the two consoles' clocks differ by parts per million, so the queue
 * holds one entry almost always and two on the rare frame where the drift
 * puts two transfers inside one of our frames. */
#define RX_QUEUE 4

/* How long after the last real transfer this console still calls itself
 * linked: half a second, many frames more than any hiccup and far less than
 * a player would spend wondering. */
#define CONNECTED_GRACE 30

static volatile LinkFrame g_rx[RX_QUEUE];
static volatile uint8_t g_rx_head;   /* written by the interrupt */
static volatile uint8_t g_rx_tail;   /* written by the main loop */
static volatile uint16_t g_starved = 0xFFFF;
static volatile uint8_t g_tx_frame;
static volatile bool g_auto_tx;      /* the interrupt loads the buttons itself */
static bool g_armed;

/* ARM, and in internal WRAM (IWRAM_CODE), for the same reason the 6502
 * interpreter is: this runs at every transfer and must finish long before
 * the next one. It also must not be fetched over the cartridge bus while
 * that bus is busy.
 *
 * It is not the interrupt handler itself any more. The program has ONE
 * (irq_handler, video.c), because the vertical blank takes an interrupt too
 * now that vsync() sleeps on it; that handler acknowledges both and calls
 * this for a serial one. */
IWRAM_CODE void link_serial_service(void);
void link_serial_service(void) {
    {
        uint16_t m = REG_SIOMULTI(0);
        uint16_t s = REG_SIOMULTI(1);

        /* $FFFF is the hardware's "nobody there". A transfer missing either
         * console did not happen as far as this file is concerned: it is not
         * queued, it does not reset the starvation counter, and it does not
         * advance the frame that the next word will claim. That is what makes
         * a console with no cable — which starts transfers quite happily and
         * gets $FFFF back out of the empty slot — report itself as alone. */
        if (m != LINK_ABSENT && s != LINK_ABSENT) {
            uint8_t head = g_rx_head;
            uint8_t next = (uint8_t)((head + 1u) % RX_QUEUE);
            /* A full queue means the main loop has stopped taking transfers.
             * Dropping the newest keeps what is kept contiguous; the frame
             * counters in the words report the gap either way. */
            if (next != g_rx_tail) {
                g_rx[head].master = m;
                g_rx[head].slave = s;
                g_rx_head = next;
            }
            g_starved = 0;

            /* During a match, load the next word straight away so the send
             * register is never stale when the master starts the following
             * transfer. This is the whole reason the cable runs on an
             * interrupt. */
            if (g_auto_tx) {
                g_tx_frame++;
                REG_SIOMLT_SEND = tengen_link_pack(link_read_buttons(), g_tx_frame);
            }
        }
    }
}

void link_init(void) {
    /* RCNT bits 14-15 pick between the serial modes and the general-purpose
     * ones; zero leaves SIOCNT in charge. */
    REG_RCNT = 0x0000;
    REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD_115200;
    REG_SIOMLT_SEND = 0;

    g_rx_head = 0;
    g_rx_tail = 0;
    g_starved = 0xFFFF;      /* nothing has ever arrived */
    g_tx_frame = 0;
    g_auto_tx = false;

    /* The vector is already irq_handler's (irq_init, at boot); the cable
     * only has to switch its own source on. */
    REG_IME = 0;
    REG_IE |= IRQ_SERIAL;
    REG_IF = IRQ_SERIAL;     /* discard anything already pending */
    REG_SIOCNT |= SIO_IRQ;
    g_armed = true;
    REG_IME = 1;
}

void link_shutdown(void) {
    REG_IME = 0;
    REG_SIOCNT &= (uint16_t)~SIO_IRQ;
    REG_IE &= (uint16_t)~IRQ_SERIAL;
    g_auto_tx = false;
    g_armed = false;
    REG_IME = 1;
}

bool link_is_master(void) {
    return (REG_SIOCNT & SIO_SI) == 0;
}

bool link_connected(void) {
    return g_starved <= CONNECTED_GRACE;
}

void link_pump(void) {
    if (!g_armed || !link_is_master()) return;
    if (REG_SIOCNT & SIO_START) return;   /* one is still in flight */
    REG_SIOCNT |= SIO_START;
}

bool link_pop(LinkFrame *out) {
    uint8_t tail = g_rx_tail;
    if (tail == g_rx_head) return false;
    out->master = g_rx[tail].master;
    out->slave = g_rx[tail].slave;
    g_rx_tail = (uint8_t)((tail + 1u) % RX_QUEUE);
    return true;
}

uint16_t link_starved(void) {
    return g_starved;
}

void link_tick(void) {
    if (g_starved < 0xFFFF) g_starved++;
}

/* ----------------------------------------------------------------------- *
 * The lobby
 *
 * The handshake itself is in ../src/tengen_link.c, where two of them can be
 * run against each other on the host. This is only the frame around it: put
 * the word the protocol asks for on the wire, and tell it what came back.
 *
 * Writing the send register from here, rather than from the interrupt, is
 * safe because the handshake is stop-and-wait: the worst a word that misses
 * its transfer can do is make the master repeat a stage.
 * ----------------------------------------------------------------------- */

static void lobby_send(const TengenLobby *lobby) {
    if (REG_SIOCNT & SIO_START) return;   /* never while one is in flight */
    REG_SIOMLT_SEND = tengen_lobby_word(lobby, link_is_master());
}

void link_lobby_start(TengenLobby *lobby, uint16_t seed, uint8_t start_level,
                       uint8_t music) {
    tengen_lobby_start(lobby, seed, start_level, music);
    lobby_send(lobby);
}

void link_lobby_start_held(TengenLobby *lobby, uint16_t seed) {
    tengen_lobby_start_held(lobby, seed);
    lobby_send(lobby);
}

void link_lobby_release(TengenLobby *lobby, uint16_t seed,
                         uint8_t start_level, uint8_t music,
                         const uint8_t handicap[2], bool coop, bool xe) {
    tengen_lobby_release(lobby, seed, start_level, music, handicap, coop, xe);
}

void link_lobby_step(TengenLobby *lobby) {
    if (lobby->ready || lobby->failed) return;

    bool master = link_is_master();
    link_tick();

    /* TAKE, ANSWER, AND ONLY THEN START THE NEXT TRANSFER. This used to pump
     * first, and on the emulated cable that was fine, because the transfer
     * there completes inside the register write and SIO_START is clear
     * again by the time lobby_send looks at it. On the hardware a transfer
     * at 115200 baud is in flight for tens of microseconds, so the master's
     * lobby_send, a few instructions after its own pump, found SIO_START set
     * every single frame and put nothing on the wire: the register kept the
     * last word and the handshake could only ever repeat its first stage.
     * Reading what arrived and loading the reply before the pump means the
     * transfer carries the reply, and the guard in lobby_send is left for
     * the case it was written for. */
    LinkFrame f;
    if (link_pop(&f)) {
        tengen_lobby_apply(lobby, master, true, f.master, f.slave);
        lobby_send(lobby);
    } else {
        tengen_lobby_apply(lobby, master, false, 0, 0);
    }
    /* ...unless that word was the last: a transfer started after the
     * handshake is done would land its GO in the match's queue. */
    if (!lobby->ready && !lobby->failed) link_pump();
}

/* ----------------------------------------------------------------------- *
 * The match
 * ----------------------------------------------------------------------- */

void link_play_begin(void) {
    REG_IME = 0;
    g_rx_head = 0;
    g_rx_tail = 0;
    g_starved = 0;
    g_tx_frame = 0;
    /* Frame 0's word goes in before the interrupt takes over, so the first
     * transfer of the match carries real buttons rather than the lobby's
     * last GO. */
    REG_SIOMLT_SEND = tengen_link_pack(link_read_buttons(), 0);
    g_auto_tx = true;
    REG_IME = 1;
}

void link_play_end(void) {
    g_auto_tx = false;
}

/* ----------------------------------------------------------------------- *
 * The records, after the match
 *
 * The same wire and the same once-a-frame shape as the lobby's; only the
 * state machine differs, and that one is in ../src/tengen_link.c where it
 * can be run against a second machine on the host. See TengenNameSwap.
 * ----------------------------------------------------------------------- */

static void name_send(const TengenNameSwap *swap) {
    if (REG_SIOCNT & SIO_START) return;   /* never while one is in flight */
    REG_SIOMLT_SEND = tengen_name_word(swap);
}

void link_name_start(TengenNameSwap *swap) {
    tengen_name_start(swap);
    /* The interrupt stops loading buttons of its own: from here the word on
     * the wire is this exchange's. */
    g_auto_tx = false;
    /* WHAT THE MATCH LEFT BEHIND GOES FIRST. The queue can still hold a
     * transfer or two of buttons, and tengen_name_apply would count each of
     * them against the give-up counter rather than as an answer. */
    LinkFrame f;
    while (link_pop(&f)) { }
    name_send(swap);
}

void link_name_step(TengenNameSwap *swap) {
    if (swap->complete || swap->failed) return;

    bool master = link_is_master();
    link_tick();
    /* Take, answer, and only then start the next transfer — see the note in
     * link_lobby_step for why that order and not the other one. */
    LinkFrame f;
    if (link_pop(&f)) {
        tengen_name_apply(swap, true, master ? f.slave : f.master);
        name_send(swap);
    } else {
        tengen_name_apply(swap, false, 0);
    }
    if (!swap->complete && !swap->failed) link_pump();
}
