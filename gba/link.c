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
 *   bit 2  SI   0 on the master, 1 on a slave — the level on the SI pin,
 *               which the cable grounds for the master and wires to the
 *               master's SO for the slave. So a slave whose master is not in
 *               multiplayer mode (SO not driven) can read 0 and look like a
 *               master: see link_is_master, which trusts the ID bits once a
 *               transfer has set them.
 *   bit 3  SD   "every console is ready" — read at leisure by the pump.
 *               NOT at the instant of the interrupt: see link_serial_service.
 *   bits 4-5 ID 0 the master, 1 the slave, as the last transfer assigned it.
 *   bit 6  ERR  never acted on: see link_serial_service.
 *   bit 7  START/BUSY — the master writes 1 to begin a transfer; on every
 *               console it reads 1 while one is in progress.
 */
#define SIO_MODE_MULTI  0x2000
#define SIO_BAUD_38400  0x0001
#define SIO_SI          0x0004
#define SIO_SD          0x0008
#define SIO_ID_SHIFT    4
#define SIO_ERR         0x0040
#define SIO_START       0x0080
#define SIO_IRQ         0x4000

/* 38400 BAUD, NOT 115200. One word a frame needs neither: a transfer between
 * two consoles is about a millisecond at this rate. What the lower rate buys
 * is margin on a cheap cable, and it is the rate gba-link-connection (the
 * library people actually run on hardware) defaults to. */
#define SIO_BAUD SIO_BAUD_38400

/* How many frames the master lets a transfer stay busy before it decides the
 * hardware is wedged and starts the port over. A good one takes a millisecond. */
#define LINK_BUSY_LIMIT 3

/* ...and never more than one restart every two seconds. Restarts that moved
 * the lines the other console is listening on, four empty transfers apart,
 * fed each other on two real SPs: every interrupt on both an empty transfer
 * ("A 999 R 999", both consoles reading themselves a slave at the
 * interrupt, the master's own slot empty). */
#define LINK_RESET_COOLDOWN 120

/* A port that has heard nobody for LINK_ABSENT_LIMIT transfers running — half
 * a second — is started over, gently (sio_reset): a console that is really
 * there and stuck is worth getting back, and one that is not loses nothing.
 * It used to be four transfers, and restarts that moved the lines; together
 * those fed on each other. */
#define LINK_ABSENT_LIMIT 30

/* The interrupt hands transfers to the main loop through this. The two
 * consoles' clocks differ by parts per million, so the queue holds one entry
 * almost always and two on the rare frame where the drift puts two transfers
 * inside one of ours. SIXTEEN anyway: a word dropped on a full queue is a
 * desync and the end of the match, and sixteen is a quarter of a second of
 * this console falling behind — a stall no frame here comes near in mGBA,
 * bought for 48 bytes, on hardware that is not mGBA. It costs no latency:
 * the queue only fills when this side is behind, and LINK_MAX_CATCHUP
 * drains it. */
#define RX_QUEUE 16

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
/* What the send register is supposed to hold, so a reset can put it back. */
static volatile uint16_t g_tx_word;
static uint8_t g_busy_frames;
static volatile uint8_t g_reset_cool;     /* frames until a restart is allowed */
/* Transfers running with a slot empty; see LINK_ABSENT_LIMIT. */
static volatile uint8_t g_absent_run;
/* WHO THIS CONSOLE IS, as the hardware said on the last good transfer. */
static volatile bool g_id_known;
static volatile uint8_t g_id;
/* ...and before there has been one, as the SI pin says — read only with the
 * port idle, and only believed once it has said the same ROLE_DEBOUNCE
 * frames running. See link_sample_role. */
#define ROLE_DEBOUNCE 8
static bool g_si_master;
static uint8_t g_si_disagree;

/* THE WORD FOR THE NEXT TRANSFER, and the word the last one carried. The
 * main loop decides the first whenever it likes; it reaches the send
 * register straight away if the port is idle, and otherwise from the
 * interrupt the moment the transfer in flight ends — which is the one
 * moment the register is sure to be free. On hardware whose frames run in
 * step with the master's, a slave's lobby found the port busy EVERY frame
 * and never answered: it wrote only when idle. */
static volatile uint16_t g_sent_word;

static inline void load_send(void) {
    g_sent_word = g_tx_word;
    REG_SIOMLT_SEND = g_tx_word;
}

static inline void tx(uint16_t word) {
    g_tx_word = word;
    if (!(REG_SIOCNT & SIO_START)) load_send();
}

/* THROUGH NORMAL MODE AND BACK: a real change of mode for the serial
 * hardware — whatever a transfer left half done is dropped — that lets go of
 * no line. SIOCNT $0008 is normal mode on an EXTERNAL clock (SC stays an
 * input) with SO held high while idle (bit 3), which is where multiplayer
 * mode keeps it too; SI and SD are inputs in every mode. General purpose,
 * which gba-link-connection uses for this, releases the pins instead. */
#define SIO_NORMAL_SO_HIGH 0x0008

/* THE PORT, STARTED OVER — gently, and not often. This used to take RCNT to
 * general purpose and back, as gba-link-connection's reset does. In general
 * purpose the port lets go of its lines, and the other console, listening on
 * them, took the flutter for transfers: empty ones, which made IT restart,
 * which fluttered the lines back. Two real SPs ended up doing nothing else.
 * Through normal mode instead (SIO_NORMAL_SO_HIGH), and LINK_RESET_COOLDOWN
 * keeps it rare. */
IWRAM_CODE static void sio_reset(void);
static void sio_reset(void) {
    if (g_reset_cool) return;
    g_reset_cool = LINK_RESET_COOLDOWN;
    REG_SIOCNT = SIO_NORMAL_SO_HIGH;
    REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD | SIO_IRQ;
    load_send();
}

/* ARM, and in internal WRAM (IWRAM_CODE), for the same reason the 6502
 * interpreter is: this runs at every transfer and must finish long before
 * the next one, so none of it — the button read it calls included
 * (read_buttons, IWRAM_CODE too) — is fetched over the cartridge bus with its
 * wait states.
 *
 * It is not the interrupt handler itself any more. The program has ONE
 * (irq_handler, video.c), because the vertical blank takes an interrupt too
 * now that vsync() sleeps on it; that handler acknowledges both and calls
 * this for a serial one. */
/* THE WORDS DECIDE, NOT THE FLAGS. This used to throw away and restart the
 * port on any transfer that came back with the error bit or with SD low, as
 * gba-link-connection does. On two real SPs entering the cable together that
 * was every transfer on both — "G 0 B 999 R 999", while SIOCNT read at
 * leisure showed neither flag ($6009, $601D): read in the first
 * microseconds of the interrupt, SD is plausibly still down from the
 * transfer itself, and a reset on every transfer is its own failure. The
 * hardware empties every slot to $FFFF when a transfer STARTS, so two real
 * words in the two slots are two consoles heard in this one; that is the
 * test. The flags are not looked at. (While the cable was being made to
 * work on two SPs they were counted, with the words, for a line on the LINK
 * CABLE screen; that line is in the history, and the counters went with it.) */
IWRAM_CODE void link_serial_service(void);
void link_serial_service(void) {
    uint16_t cnt = REG_SIOCNT;
    {
        uint16_t m = REG_SIOMULTI(0);
        uint16_t s = REG_SIOMULTI(1);

        /* $FFFF is the hardware's "nobody there". A transfer missing either
         * console did not happen as far as this file is concerned: it is not
         * queued, it does not reset the starvation counter, and it does not
         * advance the frame that the next word will claim. That is what makes
         * a console with no cable — which starts transfers quite happily and
         * gets $FFFF back out of the empty slot — report itself as alone. */
        /* IN THE MATCH, BOTH WORDS HAVE TO BE THE MATCH'S. The two consoles
         * leave the lobby a transfer apart when the transfers fall
         * differently in their frames, and the one that went first hears the
         * other's GO once more: that is not a move, so it is not queued and
         * the frame is not advanced, and the same word goes out again until
         * the partner's first move arrives (see TENGEN_LINK_MATCH_MARK). */
        bool heard = m != LINK_ABSENT && s != LINK_ABSENT;
        bool lobby_word = heard && g_auto_tx &&
            !(tengen_link_is_match_word(m) && tengen_link_is_match_word(s));
        /* Any transfer that heard both consoles ends a run of empty ones —
         * the lobby's last GO in the match included, or a partner halfway
         * out of the lobby could add its empty transfers to the ones
         * before and restart the port under it. */
        if (heard) g_absent_run = 0;
        if (lobby_word) {
            g_starved = 0;                    /* the partner is there */
        } else if (!heard) {
            if (++g_absent_run >= LINK_ABSENT_LIMIT) {
                g_absent_run = 0;
                sio_reset();
            }
        } else {
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
            /* ...and who this console is, but only where the words agree:
             * ID 0 with this console's own word in the master's slot, ID 1
             * with it in the slave's. Read in the interrupt, like SD, the ID
             * bits are one register read away from a timing surprise; the
             * slot this console's word landed in is not. */
            uint8_t id = (uint8_t)((cnt >> SIO_ID_SHIFT) & 3);
            if ((id == 0 && m == g_sent_word) || (id == 1 && s == g_sent_word)) {
                g_id = id;
                g_id_known = true;
            }

            /* During a match, load the next word straight away so the send
             * register is never stale when the master starts the following
             * transfer. This is the whole reason the cable runs on an
             * interrupt. */
            if (g_auto_tx) {
                g_tx_frame++;
                g_tx_word = tengen_link_pack(link_read_buttons(), g_tx_frame);
            }
        }
    }
    /* The port is free until the master's next start: the word the main
     * loop wants goes in now (see load_send). */
    load_send();
}

void link_init(void) {
    /* RCNT bits 14-15 pick between the serial modes and the general-purpose
     * ones; zero leaves SIOCNT in charge. NOT through general purpose on the
     * way: see sio_reset for what that does to the other console. And only
     * when it is not there already: after the first session the port stays
     * in multiplayer mode (link_shutdown), and a write here would be a touch
     * on the pins for nothing. */
    if (REG_RCNT & 0xC000) REG_RCNT = 0x0000;
    /* ...and every session starts from a real change of mode, as a restart
     * does: after the first one the port is still in multiplayer mode, with
     * whatever the last session, or the other console's transfers since,
     * left in it. On two SPs the cable failed only when the master reached
     * the lobby first — its transfers running while the other console came
     * in — and never the other way round. */
    REG_SIOCNT = SIO_NORMAL_SO_HIGH;
    REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD;
    tx(0);
    g_reset_cool = 0;
    g_absent_run = 0;
    g_id_known = false;
    /* The first reading counts at once; later ones have to hold. */
    g_si_master = (REG_SIOCNT & SIO_SI) == 0;
    g_si_disagree = 0;
    g_busy_frames = 0;

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

/* ...BUT NOT OUT OF MULTIPLAYER MODE. This took the port to general purpose
 * for a while, as gba-link-connection's deactivate does, and in general
 * purpose the SO pin is not driven: the slave's SI is wired to it, so a
 * slave left waiting on the cable read SI low and took itself for the
 * master — on two real SPs a slave came up on LEVEL SETTINGS. Left in
 * multiplayer mode with its interrupt off, this console answers transfers
 * with 0, which the lobby reads as "not in the lobby" (tag NONE), and its SO
 * stays where the other console expects it. */
void link_shutdown(void) {
    REG_IME = 0;
    REG_SIOCNT &= (uint16_t)~SIO_IRQ;
    REG_IE &= (uint16_t)~IRQ_SERIAL;
    REG_SIOMLT_SEND = 0;
    g_auto_tx = false;
    g_armed = false;
    REG_IME = 1;
}

/* THE SI PIN IS NOT THE CABLE'S ANSWER WHILE A TRANSFER IS PASSING. The
 * slave's SI is wired to the master's SO, and the master drives it LOW for
 * the length of every transfer to hand the slave its turn (GBATEK, "Transfer
 * Protocol"); at rest it is HIGH. A slave that looked at SI at a random
 * moment — which this did, whenever the lobby asked — saw LOW about one read
 * in fifteen while a master was pumping, took itself for the master, "linked"
 * on its own echo and went off to LEVEL SETTINGS: on two real SPs, every
 * time the master had reached the cable first. So SI is read here, once a
 * frame, only with the port idle (the busy bit, which a slave gets for the
 * length of a transfer, clear in the same read), and a change of mind has to
 * hold for ROLE_DEBOUNCE frames. It still reads LOW on a slave whose master
 * is not in multiplayer mode at all — its SO is not driven high then — which
 * is harmless: SD is low too, nobody transfers, and the slave becomes one
 * when the master arrives. */
void link_sample_role(void) {
    uint16_t cnt = REG_SIOCNT;
    if (cnt & SIO_START) return;
    bool master = (cnt & SIO_SI) == 0;
    if (master == g_si_master) {
        g_si_disagree = 0;
    } else if (++g_si_disagree >= ROLE_DEBOUNCE) {
        g_si_master = master;
        g_si_disagree = 0;
    }
}

/* The ID bits of the last good transfer once there has been one — the
 * hardware's own answer, checked against the slot this console's word
 * landed in — and the settled SI pin before that. */
bool link_is_master(void) {
    if (g_id_known) return g_id == 0;
    return g_si_master;
}

bool link_connected(void) {
    return g_starved <= CONNECTED_GRACE;
}

/* THE MASTER STARTS A TRANSFER ONLY WHEN EVERYONE IS READY. SD low is a
 * console on the cable that is not in multiplayer mode; starting into it is
 * how the error bit gets set. And a start that is still busy frames later is
 * a port that is stuck, not a slow transfer. Both as gba-link-connection
 * does them. */
void link_pump(void) {
    if (!g_armed || !link_is_master()) return;
    uint16_t cnt = REG_SIOCNT;
    if (cnt & SIO_START) {                 /* one is still in flight */
        if (++g_busy_frames > LINK_BUSY_LIMIT) {
            REG_IME = 0;
            sio_reset();
            REG_IME = 1;
            g_busy_frames = 0;
        }
        return;
    }
    g_busy_frames = 0;
    /* NOT THE ERROR BIT. It is the verdict on the LAST transfer, and only a
     * new transfer rewrites it — starting the port over does not. This used
     * to reset instead of starting whenever it was set, and on two real SPs
     * that was a master resetting every frame for ever ("R 999", SIOCNT
     * $6049) with the error still up and not one transfer tried: a cable
     * that could never come back, mid-match (CABLE LOST) or in the lobby.
     * The interrupt judges each transfer as it lands; here only SD counts,
     * as in gba-link-connection. */
    if (!(cnt & SIO_SD)) return;
    REG_SIOCNT = cnt | SIO_START;
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
    if (g_reset_cool) g_reset_cool--;
    link_sample_role();
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

/* The role the lobby is being run in, to notice it changing. */
static bool g_lobby_master;

static void lobby_send(const TengenLobby *lobby) {
    tx(tengen_lobby_word(lobby, link_is_master()));
}

void link_lobby_start(TengenLobby *lobby, uint16_t seed, uint8_t start_level,
                       uint8_t music) {
    tengen_lobby_start(lobby, seed, start_level, music);
    g_lobby_master = link_is_master();
    lobby_send(lobby);
}

void link_lobby_start_held(TengenLobby *lobby, uint16_t seed) {
    tengen_lobby_start_held(lobby, seed);
    g_lobby_master = link_is_master();
    lobby_send(lobby);
}

void link_lobby_release(TengenLobby *lobby, uint16_t seed,
                         uint8_t start_level, uint8_t music,
                         const uint8_t handicap[2], bool coop, bool xe) {
    tengen_lobby_release(lobby, seed, start_level, music, handicap, coop, xe);
}

void link_lobby_step(TengenLobby *lobby) {
    if (lobby->ready) return;

    link_tick();
    bool master = link_is_master();
    /* A CONSOLE THAT FINDS IT IS THE OTHER ONE starts the conversation again
     * in the right role (tengen_lobby_forget): what it built up in the wrong
     * one would have it taking its own echo for its partner's. */
    if (master != g_lobby_master) {
        g_lobby_master = master;
        tengen_lobby_forget(lobby, master);
    }

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
    if (!lobby->ready) link_pump();
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
    tx(tengen_link_pack(link_read_buttons(), 0));
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
    tx(tengen_name_word(swap));
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
