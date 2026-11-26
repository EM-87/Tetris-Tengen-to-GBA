/* link.c — GBA serial multiplayer mode. See link.h for the shape of it. */
#include "link.h"

/* The Game Boy Player shares the port (gbp.c). */
void gbp_start(void);
void gbp_release(void);
bool gbp_owns_serial(void);
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
static uint8_t g_peer_empty;     /* see link_probe */
/* Frames the lobby's own transfers stay off the cable, for link_probe's
 * sake: set by it, run down by link_tick, so a lobby it has stopped looking
 * after gets its transfers back by itself. */
static uint8_t g_hush;
static uint8_t g_probe_clock;    /* ...its clock, and the port as it left it */
static bool g_probe_fast;
#define LINK_QUIET_RESTART 180
static uint8_t g_quiet_frames;   /* a slave's frames unlinked */
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

/* THE OTHER CONSOLE IS THERE AND NOT IN THE MATCH. Once its first move has
 * arrived (g_partner_played), a transfer in which this console's own move
 * came back as sent and the other slot holds a word that is no move is a
 * console that has gone somewhere else: at rest in its menus (0, which is
 * also the master's beacon), in a lobby, restarted — or a move for a step
 * already played, from a console whose program is no longer loading them.
 * Counted in a row (g_strangers) and put to nothing by any move;
 * link_partner_left is what the match asks: six of them, a tenth of a
 * second of a console at rest on the slave's end and most of one of a
 * master's beacons. Before the first move such words are the lobby's last
 * and mean "not yet" (TENGEN_LINK_MATCH_MARK). */
#define LINK_LEFT_WORDS 6
static volatile uint8_t g_strangers;
static volatile bool g_partner_played;
/* ...and the last of those words: a console whose match is OVER says the
 * names' exchange (link_partner_over), which is not a console gone. */
static volatile uint16_t g_stranger_word;

/* A STEP THE OTHER CONSOLE NEVER GOT IS SENT AGAIN. A transfer both consoles
 * make and only one takes — a bit of the frame the cable changed for one
 * of them; a Single-Pak copy, which has to ask for its interrupts, asking
 * after the NEXT transfer had already written over this one — leaves the
 * two a step apart, and they stayed there: the one ahead sending its move
 * for the next step, the one behind its move for this, no pair of them
 * ever a pair again, until each had counted six of the other's and called
 * it gone. CABLE LOST over a whole cable, for one transfer; in the demo two
 * titles share, which says nothing, two consoles each playing on alone.
 *
 * The one ahead can tell: the other's move is for the step before its own,
 * LINK_AGAIN_AFTER transfers running (once is a console late with its
 * word, which the next transfer mends by itself). It then sends the move
 * it made for THAT step once more (g_last_word, g_again): the console
 * behind sees the pair it missed — its own word for that step is still the
 * one in its register, it has loaded nothing since — plays it, and the two
 * are on the same step again. The console ahead does not play it twice: a
 * pair is this console's to play only if it is for the step this console
 * is on (the frame its own next word carries), which the pair it sends
 * again is not. And while the other's moves are a step from this
 * console's, either way, it is not counted with the consoles that have
 * left — for long enough to send twice (LINK_APART_PATIENCE); after that
 * it is, as before. A build from before this never sends again, and takes
 * one sent to it like any pair. */
#define LINK_AGAIN_AFTER 2
#define LINK_APART_PATIENCE 8
static volatile uint16_t g_last_word;   /* this console's move in the last pair it took */
static volatile uint8_t g_behind_run;   /* transfers running with the other a step behind */
static volatile uint8_t g_apart_run;    /* ...and a step either way, since the last pair */
static volatile bool g_again;           /* ...and that move is in the register again */

/* A TITLE LISTENS (link_title_tick, far below): at rest, on its title, a
 * console has the interrupt on for one thing — to take the two words of
 * each transfer as it ends and load its own for the next — and this is
 * what the interrupt leaves for the main loop to read. The interrupt on
 * with the cable put away (g_armed) IS a title listening: nothing else
 * has it on at rest, so there is no flag to say so and none to forget. */
static volatile bool g_title_news;      /* a transfer since the loop last looked */
static volatile uint16_t g_title_m, g_title_s;
static volatile bool g_title_slave;     /* which slot was this console's (ID) */
static volatile uint16_t g_title_word;  /* what it says at rest; 0 off the title */
static void title_service(uint16_t cnt);

static inline void load_send(void) {
    g_sent_word = g_tx_word;
    REG_SIOMLT_SEND = g_tx_word;
}

static inline void tx(uint16_t word) {
    g_tx_word = word;
    if (wireless_on()) return;              /* the port is the adapter's */
    if (!(REG_SIOCNT & SIO_START)) load_send();
}

/* ----------------------------------------------------------------------- *
 * For wireless.c: the air makes the same transfers the cable would, and
 * puts them in the same queue.
 * ----------------------------------------------------------------------- */

uint16_t link_tx_word(void) {
    return g_tx_word;
}

void link_push_pair(uint16_t master, uint16_t slave) {
    uint8_t head = g_rx_head;
    uint8_t next = (uint8_t)((head + 1u) % RX_QUEUE);
    if (next != g_rx_tail) {
        g_rx[head].master = master;
        g_rx[head].slave = slave;
        g_rx_head = next;
    }
    g_starved = 0;
}

void link_heard(void) {
    g_starved = 0;
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
/* A TRANSFER IN A MATCH THAT IS NOT A PAIR OF ITS MOVES. If the other
 * console has played, it is no longer in the match — counted only where
 * this console's own word crossed whole, so that a transfer the cable
 * mangled is not taken for a console's answer — or it is in it and a step
 * behind (g_last_word). (A pair this console sent again and saw land is
 * neither, and does not come here: the other has it now, or will be found
 * a step behind again.) */
static __attribute__((noinline)) void stranger_service(uint16_t m, uint16_t s) {
    bool mine_m = m == g_sent_word && tengen_link_is_match_word(m);
    bool mine_s = s == g_sent_word && tengen_link_is_match_word(s);
    if (!g_partner_played || mine_m == mine_s) return;
    uint16_t theirs = mine_m ? s : m;
    /* Its move is for the step before this console's (1) or the step
     * after (the mask's own value, one less than nothing): the other is IN
     * the match and a step from here, which a move sent again mends. Not
     * a console gone — for LINK_APART_PATIENCE transfers, which is two of
     * those and the wait before each; one that goes on being a step away
     * after that is not taking them, and is counted like any other. */
    uint8_t ahead = (uint8_t)((tengen_link_frame(g_sent_word) -
                               tengen_link_frame(theirs)) & TENGEN_LINK_FRAME_MASK);
    bool move = tengen_link_is_match_word(theirs);
    if (move && (ahead == 1 || ahead == TENGEN_LINK_FRAME_MASK) &&
        g_apart_run < LINK_APART_PATIENCE) {
        g_apart_run++;
    } else {
        if (g_strangers < 0xFF) g_strangers++;
        g_stranger_word = theirs;
    }
    /* A step behind this console, and still: its last move goes out
     * again. */
    if (move && ahead == 1) {
        if (++g_behind_run >= LINK_AGAIN_AFTER) {
            g_behind_run = 0;
            g_again = true;
            g_tx_word = g_last_word;
        }
    } else {
        g_behind_run = 0;
    }
}

IWRAM_CODE void link_serial_service(void);
void link_serial_service(void) {
    uint16_t cnt = REG_SIOCNT;
    /* At rest on a title (not in a hurry, and not in IWRAM): its words. */
    if (!g_armed) {
        title_service(cnt);
        return;
    }
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
        /* ...AND BOTH FOR THE SAME STEP. A console whose interrupt did not
         * get to load its next move before the transfer — late once, or
         * started over under the match, its port still answering with the
         * last move it loaded — sends that one again. Queued, it was a
         * word for the wrong frame and the end of the match on the spot
         * (desynced); it is not a move, so, like the lobby's last GO, it
         * is not queued and the frame is not advanced: both consoles have
         * the same two words and both send theirs again. Once late costs a
         * transfer; a console that keeps it up is counted with the ones
         * that have left (g_strangers). */
        /* ...AND FOR THE STEP THIS CONSOLE IS ON: see g_last_word. */
        bool heard = m != LINK_ABSENT && s != LINK_ABSENT;
        bool pair = tengen_link_is_match_word(m) && tengen_link_is_match_word(s) &&
                    tengen_link_frame(m) == tengen_link_frame(s);
        bool lobby_word = heard && g_auto_tx &&
            !(pair && tengen_link_frame(m) ==
                          (g_tx_frame & TENGEN_LINK_FRAME_MASK));
        /* A move sent again was for one transfer, taken or not: the
         * register goes back to the step this console is on (`fresh`,
         * below — one reading of the pad for either reason to take one). */
        bool fresh = g_again;
        g_again = false;
        /* Any transfer that heard both consoles ends a run of empty ones —
         * the lobby's last GO in the match included, or a partner halfway
         * out of the lobby could add its empty transfers to the ones
         * before and restart the port under it. */
        if (heard) g_absent_run = 0;
        if (lobby_word) {
            g_starved = 0;                    /* the partner is there */
            /* (Not in a hurry, and not in IWRAM: a match's own pairs do
             * not come this way.) */
            if (!pair) stranger_service(m, s);
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
                g_last_word = g_sent_word;
                g_behind_run = 0;
                g_apart_run = 0;
                g_tx_frame++;
                fresh = true;
                g_partner_played = true;
                g_strangers = 0;
            }
        }
        if (fresh)
            g_tx_word = tengen_link_pack(link_read_buttons(), g_tx_frame);
    }
    /* The port is free until the master's next start: the word the main
     * loop wants goes in now (see load_send). */
    load_send();
}

void link_init(void) {
    if (wireless_present()) {
        /* Over the air: no interrupt, no multiplayer mode; the adapter is
         * run from link_pump, once a frame (wireless.c). */
        g_rx_head = 0;
        g_rx_tail = 0;
        g_starved = 0xFFFF;
        g_tx_frame = 0;
        g_auto_tx = false;
        g_peer_empty = 0;
        g_tx_word = 0;
        wireless_open();
        g_armed = true;
        return;
    }
    gbp_release();           /* the port is the cable's now, not the Player's */
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
    g_peer_empty = 0;
    g_hush = 0;
    g_probe_clock = 0;
    g_probe_fast = false;
    g_quiet_frames = 0;

    /* The vector is already irq_handler's (irq_init, at boot); the cable
     * only has to switch its own source on. */
    REG_IME = 0;
    REG_IE |= IRQ_SERIAL;
    REG_IF = IRQ_SERIAL;     /* discard anything already pending */
    REG_SIOCNT |= SIO_IRQ;
    g_armed = true;
    REG_IME = IME_ON;
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
    if (wireless_on()) {
        wireless_close();
        g_auto_tx = false;
        g_armed = false;
        g_starved = 0xFFFF;
        return;
    }
    REG_IME = 0;
    /* ...AND AT THE LOBBY'S SPEED: a probe for a console in its BIOS may
     * have left it at 115200 (link_probe), where a lobby on the other end
     * could make nothing of this one's answers or its beacon. */
    if (g_probe_fast) {
        g_probe_fast = false;
        REG_SIOCNT = SIO_NORMAL_SO_HIGH;
        REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD;
    }
    REG_SIOCNT &= (uint16_t)~SIO_IRQ;
    REG_IE &= (uint16_t)~IRQ_SERIAL;
    REG_SIOMLT_SEND = 0;
    REG_IF = IRQ_SERIAL;     /* a transfer that landed on the way out is not served */
    g_auto_tx = false;
    g_armed = false;
    /* NOBODY IS HEARD ONCE THE CABLE IS PUT AWAY. The count of frames since
     * the last transfer is only kept up while something steps the cable
     * (link_tick), so left as it stood it said "connected" for ever after
     * any session that ended with the partner still there — and the save
     * in the cartridge's flash, which waits for the other end to go before
     * it writes its journal afresh (cartsave.c, commit), would have waited
     * until the power switch. */
    g_starved = 0xFFFF;
    REG_IME = IME_ON;
    gbp_start();             /* ...and back to a Game Boy Player, if one */
}

/* THE PORT AT REST FROM THE MOMENT THE GAME STARTS: multiplayer mode, no
 * interrupt, $0000 to send — the state link_shutdown leaves it in. A
 * console whose port has never been touched is not in multiplayer mode,
 * holds SD low, and the other end cannot tell it from one switched off;
 * one at rest reads as a console that is there and not in its lobby yet,
 * and the LINK CABLE screen on the other end says to wait for its player
 * (link_peer) rather than to switch it on. */
void link_rest(void) {
    if (gbp_owns_serial() || wireless_present()) return;
    if (REG_RCNT & 0xC000) REG_RCNT = 0x0000;
    REG_SIOCNT = SIO_NORMAL_SO_HIGH;
    REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD;
    REG_SIOMLT_SEND = 0;
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
    if (wireless_on()) return;
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
    if (wireless_on()) return wireless_host();
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
    /* Over the air both consoles have their part to play every frame. */
    if (wireless_on()) {
        wireless_frame();
        return;
    }
    if (!g_armed || !link_is_master()) return;
    uint16_t cnt = REG_SIOCNT;
    if (cnt & SIO_START) {                 /* one is still in flight */
        if (++g_busy_frames > LINK_BUSY_LIMIT) {
            REG_IME = 0;
            sio_reset();
            REG_IME = IME_ON;
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

uint8_t link_queued(void) {
    return (uint8_t)((g_rx_head + RX_QUEUE - g_rx_tail) % RX_QUEUE);
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
    if (g_hush) g_hush--;
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

/* WHICH BUILD THIS IS, as the lobby says it to the other console
 * (tengen_lobby_version): the number after "2." on the credits' VERSION
 * line, one a batch. A variable, and volatile, for the checks' sake: they
 * have one build to make two different ones of, and the link would
 * otherwise fold it into the code. */
#ifndef TENGEN_VERSION_MINOR
#define TENGEN_VERSION_MINOR 0
#endif
volatile uint8_t g_link_version = (uint8_t)(TENGEN_VERSION_MINOR);

static void lobby_send(const TengenLobby *lobby) {
    tx(tengen_lobby_word(lobby, link_is_master()));
}

void link_lobby_start_held(TengenLobby *lobby, uint16_t seed) {
    tengen_lobby_start_held(lobby, seed);
    tengen_lobby_version(lobby, g_link_version);
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
    /* A SLAVE THAT HEARS NOTHING FOR SECONDS starts its port over, now and
     * then, as pulling the plug and pushing it back would. A master that
     * hears nothing does it already (LINK_ABSENT_LIMIT); a slave has no
     * transfers to count, and on two real SPs two copies of the Single-Pak
     * image that had both left the lobby for the menu and come back never
     * found each other again — a port stuck somewhere this cannot see is
     * one explanation (INFERRED; the emulator's cable does not do it). Every
     * LINK_QUIET_RESTART frames is nothing like the storms of old. */
    if (!wireless_on() && !master && !lobby->linked &&
        g_starved >= LINK_QUIET_RESTART &&
        ++g_quiet_frames >= LINK_QUIET_RESTART) {
        g_quiet_frames = 0;
        REG_IME = 0;
        g_reset_cool = 0;
        sio_reset();
        REG_IME = IME_ON;
    }
    if (g_starved < LINK_QUIET_RESTART) g_quiet_frames = 0;
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
     * handshake is done would land its GO in the match's queue. And not
     * while the cable is kept quiet for a console in its BIOS (link_probe). */
    if (!lobby->ready && !g_hush) link_pump();
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
    g_strangers = 0;
    g_partner_played = false;
    /* Frame 0's word goes in before the interrupt takes over, so the first
     * transfer of the match carries real buttons rather than the lobby's
     * last GO. */
    tx(tengen_link_pack(link_read_buttons(), 0));
    g_auto_tx = !wireless_on();   /* over the air, wireless.c makes them */
    REG_IME = IME_ON;
    if (wireless_on()) wireless_match_begin();
}

void link_play_end(void) {
    g_auto_tx = false;
}

bool link_partner_left(void) {
    return g_strangers >= LINK_LEFT_WORDS;
}

/* ...AND WHAT IT IS SAYING INSTEAD IS THE END OF A MATCH: the names'
 * exchange, which a console begins on the step its match is over and
 * nowhere else, with TYPING ("nothing to say yet") for as long as its
 * player is under the plaque. Its game has ended and this one's has not —
 * the two were no longer the same game (see tengen_link_end). Asked of a
 * partner that link_partner_left has just said is not playing. */
bool link_partner_over(void) {
    return (g_stranger_word >> TENGEN_LOBBY_TAG_SHIFT) == TENGEN_LOBBY_TYPING;
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
    if (wireless_on()) wireless_lobby_begin();
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

/* THE RECORDS A COPY MADE, across as the lobby finishes (TengenRecordSync).
 * The lobby's own transport carries on underneath: the send register is
 * already the main loop's, and over the air the lobby's stop-and-wait is
 * already running, so nothing is restarted — a restart on one console and
 * not yet on the other is exactly the gap a lobby word would fall into. */
void link_records_start(TengenRecordSync *sync, bool storage,
                        const TengenRecord *mine, int n) {
    tengen_records_start(sync, storage, mine, n);
    tx(tengen_records_word(sync));
}

void link_records_step(TengenRecordSync *sync) {
    if (sync->complete || sync->failed) return;
    bool master = link_is_master();
    link_tick();
    LinkFrame f;
    if (link_pop(&f)) {
        tengen_records_apply(sync, true, master ? f.slave : f.master);
        tx(tengen_records_word(sync));
    } else {
        tengen_records_apply(sync, false, 0);
    }
    if (!sync->complete && !sync->failed) link_pump();
}

/* ----------------------------------------------------------------------- *
 * Single-Pak: sending the game to a console with no cartridge
 *
 * A GBA switched on with no cartridge in (or with START and SELECT held)
 * sits in its BIOS waiting for a program on the cable. This is the other
 * end of that, all of it in software, a halfword at a time in multiplayer
 * mode: the handshake, then the image itself, encrypted word by word, then
 * the checksum. The steps and every constant are GBATEK's ("BIOS Multi Boot
 * (Single Game Pak)", its pseudo-code for SWI $25) as gba-link-connection's
 * LinkCableMultiboot::Async runs them on hardware, for one slave.
 *
 * NOT THROUGH THE BIOS's OWN MultiBoot (SWI $25), which did the second half
 * until the Single-Pak slave had to send too: it keeps the CPU for the
 * whole transfer, so nothing can be drawn meanwhile (no progress bar), and
 * a slave on two real SPs could take no interrupt at all (see IME_ON), so
 * nothing the BIOS does behind its back is taken on trust there.
 *
 * The cartridge sends the image it carries (kSlaveImage); the slave sends
 * ITSELF, the image still sitting in EWRAM where its BIOS put it.
 * ----------------------------------------------------------------------- */
#define SIO_BAUD_115200   0x0003    /* what the BIOS's MultiPlay expects */
#define MB_CLIENT_BIT     0x02      /* the one slave, in slot 1 */
#define MB_TRIES          16
#define MB_PALETTE        0x93      /* the logo's colour and its movement */
#define MB_SPIN_LIMIT     100000u   /* a transfer is ~150us; this is ~50ms */
#define MB_HEADER         0xC0
/* GBATEK's pseudo-code: c, x, k and m's multiplier, for MultiPlay. */
#define MB_CRC_START      0xFFF8u
#define MB_CRC_XOR        0xA517u
#define MB_DATA_XOR       0x6465646Fu
#define MB_SEED_MUL       0x6F646573u
#define MB_ROM_END        0x0065
#define MB_ROM_END_ACK    0x0075
#define MB_FINAL_CRC      0x0066

/* Waits a whole frame by the scanline counter: vsync() would serve the
 * serial port as the lobby's (poll_interrupts, on the slave's build), and
 * the port is this exchange's for now. */
static void mb_frame(void) {
    while (REG_VCOUNT >= 160) { }
    while (REG_VCOUNT < 160) { }
}

/* One halfword each way, waited for: the slave's answer, or LINK_ABSENT
 * for a transfer that did not happen. A port stuck busy is started over
 * the gentle way (through normal mode; see sio_reset). Then GBATEK's 36us
 * before the next, for the slave's BIOS to have its answer ready. */
static uint16_t mb_transfer_within(uint16_t word, uint32_t spins) {
    REG_SIOMLT_SEND = word;
    REG_SIOCNT = (uint16_t)(REG_SIOCNT | SIO_START);
    for (uint32_t spin = 0; REG_SIOCNT & SIO_START; spin++) {
        if (spin > spins) {
            REG_SIOCNT = SIO_NORMAL_SO_HIGH;
            REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD_115200;
            return LINK_ABSENT;
        }
    }
    uint16_t cnt = REG_SIOCNT;
    uint16_t got = REG_SIOMULTI(1);
    for (volatile int wait = 0; wait < 60; wait++) { }
    if (!(cnt & SIO_SD) || (cnt & SIO_ERR)) return LINK_ABSENT;
    return got;
}

static uint16_t mb_transfer(uint16_t word) {
    return mb_transfer_within(word, MB_SPIN_LIMIT);
}

/* GBATEK's checksum step: 32 bits of `data` into `crc`. */
static uint32_t mb_crc(uint32_t crc, uint32_t data) {
    for (int bit = 0; bit < 32; bit++) {
        uint32_t low = (crc ^ data) & 1u;
        data >>= 1;
        crc >>= 1;
        if (low) crc ^= MB_CRC_XOR;
    }
    return crc;
}

LinkSendResult link_multiboot_send(const uint8_t *image, uint32_t len,
                                    void (*progress)(uint32_t done,
                                                     uint32_t total)) {
    /* The lobby's interrupt-driven cable steps aside: every transfer here
     * is started and waited for by hand. link_init puts it back. */
    REG_IME = 0;
    REG_SIOCNT = (uint16_t)(REG_SIOCNT & ~SIO_IRQ);
    REG_IE = (uint16_t)(REG_IE & ~IRQ_SERIAL);
    g_armed = false;
    g_auto_tx = false;
    REG_IME = IME_ON;
    if (REG_RCNT & 0xC000) REG_RCNT = 0x0000;
    REG_SIOCNT = SIO_NORMAL_SO_HIGH;
    REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD_115200;

    /* Only the master starts transfers, and a console with no cartridge
     * can only ever be the slave: this one has to be on the master's end. */
    if (REG_SIOCNT & SIO_SI) return LINK_SEND_WRONG_END;

    /* 3. $6200 until the slave answers $7202. */
    bool found = false;
    for (int t = 0; t < MB_TRIES && !found; t++)
        found = mb_transfer(0x6200) == (0x7200 | MB_CLIENT_BIT);
    if (!found) return LINK_SEND_NOBODY;
    /* 4. Which slaves there are, $610Y; each answers $720Y. */
    if (mb_transfer(0x6100 | MB_CLIENT_BIT) != (0x7200 | MB_CLIENT_BIT))
        return LINK_SEND_RETRY;
    /* 5. The header, a halfword at a time; the slave counts them down. */
    const uint16_t *half = (const uint16_t *)image;
    for (uint16_t left = MB_HEADER / 2; left > 0; left--)
        if (mb_transfer(*half++) != (uint16_t)((left << 8) | MB_CLIENT_BIT))
            return LINK_SEND_RETRY;
    /* 6. $6200 ($000Y back), then $620Y ($720Y). */
    if (mb_transfer(0x6200) != MB_CLIENT_BIT) return LINK_SEND_RETRY;
    if (mb_transfer(0x6200 | MB_CLIENT_BIT) != (0x7200 | MB_CLIENT_BIT))
        return LINK_SEND_RETRY;
    /* 7. The palette, $63PP, until the slave answers $73CC. */
    uint8_t client = 0;
    found = false;
    for (int t = 0; t < MB_TRIES && !found; t++) {
        uint16_t v = mb_transfer(0x6300 | MB_PALETTE);
        if ((v & 0xFF00) == 0x7300) {
            client = (uint8_t)v;
            found = true;
        }
    }
    if (!found) return LINK_SEND_RETRY;
    /* 8. $11 plus the three slaves' bytes ($FF for the two not there). */
    uint8_t handshake = (uint8_t)(0x11 + client + 0xFF + 0xFF);
    if ((mb_transfer(0x6400 | handshake) & 0xFF00) != 0x7300)
        return LINK_SEND_RETRY;

    /* ...AND WHAT SWI $25 WOULD DO FROM HERE. A sixteenth of a second, then
     * the length (in words, less $34), which the slave answers with a byte
     * of its own for the checksum. */
    for (int f = 0; f < 4; f++) mb_frame();
    uint16_t reply = mb_transfer((uint16_t)((len - 0x190) / 4));
    uint32_t crc_final = handshake | ((uint32_t)(reply & 0xFF) << 8) |
                         0xFFFF0000u;
    uint32_t seed = MB_PALETTE | ((uint32_t)client << 8) | 0xFFFF0000u;
    uint32_t crc = MB_CRC_START;

    /* The image after its header, a word at a time in two halves, each
     * encrypted against its own address and a running seed; the slave
     * answers every half with where it has put it. */
    const uint32_t *word = (const uint32_t *)image;
    uint32_t words = len / 4;
    for (uint32_t i = MB_HEADER / 4; i < words; i++) {
        seed = seed * MB_SEED_MUL + 1u;
        uint32_t data = word[i] ^ (0xFE000000u - (i << 2)) ^ seed ^ MB_DATA_XOR;
        if (mb_transfer((uint16_t)data) != (uint16_t)(i << 2))
            return LINK_SEND_RETRY;
        if (mb_transfer((uint16_t)(data >> 16)) != (uint16_t)((i << 2) + 2))
            return LINK_SEND_RETRY;
        crc = mb_crc(crc, word[i]);
        if (progress && (i & 0xFF) == 0) progress(i, words);
    }
    if (progress) progress(words, words);

    /* The end: $0065 until the slave has checked it all ($0075), then
     * $0066 and the checksum, which the slave must give back the same. */
    crc = mb_crc(crc & 0xFFFFu, crc_final);
    int tries = 0;
    while (mb_transfer(MB_ROM_END) != MB_ROM_END_ACK) {
        if (++tries > 300) return LINK_SEND_RETRY;
        mb_frame();
    }
    mb_transfer(MB_FINAL_CRC);
    if (mb_transfer((uint16_t)crc) != (uint16_t)crc) return LINK_SEND_RETRY;
    return LINK_SEND_DONE;
}

/* ----------------------------------------------------------------------- *
 * What is on the other end
 * ----------------------------------------------------------------------- */

/* THE FIRST STEP OF A SEND, AND NO MORE: $6200 at the BIOS's speed, which a
 * console waiting in its BIOS answers $720x on the transfer after. The
 * lobby's own transfers run at 38400 and cannot hear that console at all.
 * The LINK CABLE screen asks only while the lobby has not linked.
 *
 * THE FIRST PROBE HERE NEVER FOUND A REAL CONSOLE. It asked twice every
 * sixteenth frame, between the lobby's transfers, and against the stand-in
 * BIOS of the checks that was enough; on two real consoles the one with no
 * cartridge was never found, and the game went across only on SELECT —
 * which goes silent first and then asks, sixteen times an attempt, an
 * attempt every eight frames. What the BIOS is doing meanwhile, read off a
 * disassembly of it (PikalaxALT/gba_bios, asm/bios.s: sub_2B34 once a
 * frame, the serial handlers from sub_2D70):
 *
 *   - it listens in THREE MODES IN TURN — JOY BUS, multiplayer at 115200
 *     (SIOCNT $6003), normal — some seven frames, thirty-one and seven; a
 *     probe in the other two finds SD low and nobody there;
 *   - a mode begins with a frame of setting up and a frame more before its
 *     interrupt is on: two frames in which a word is not heard at all;
 *   - in multiplayer mode a transfer that comes in with the error bit
 *     (_2D70) puts it back to the start of that — and a word of the
 *     lobby's, at a third of its speed, is such a transfer (INFERRED: a
 *     word at 38400 has no stop bit where one at 115200 looks for it);
 *   - its answer to $6200 is loaded for the NEXT transfer, and each $6200
 *     keeps it in multiplayer mode eleven frames more (_2DCA); anything
 *     else after one puts it back to the start with 0 to send (_2F56).
 *
 * That reading alone does not account for a probe that NEVER got through
 * (a console set back two frames by every other word of the lobby's should
 * have been caught now and then), so nothing here leans on its numbers.
 * The probe does what the send that works does, in the silence it does it
 * in: LINK_PROBE_TALK frames of the lobby's own transfers, then
 * LINK_PROBE_QUIET with none (g_hush) and an attempt every LINK_PROBE_STEP
 * of them — through normal mode to 115200 and LINK_PROBE_ASKS times $6200,
 * the port left at that speed between attempts as the send leaves it. Once
 * a console has answered, the lobby says nothing at all — there is no
 * lobby on the other end to talk to — and the attempts go on every
 * LINK_PROBE_STEP frames, inside the BIOS's eleven, so that console stays
 * as it is, listening in the one mode, until the game is sent to it (which
 * starts with this same word) or it stops answering. A console of ours on
 * the other end hears the attempts as words out of order, answers NONE,
 * and is found in the next of the lobby's turns: under half a second
 * later than it would have been. */
#define LINK_PROBE_TALK  24     /* looking: frames of the lobby's transfers... */
#define LINK_PROBE_QUIET 24     /* ...and of silence, with the attempts in it */
#define LINK_PROBE_STEP  8      /* an attempt every so many quiet frames */
#define LINK_PROBE_KEEP  3      /* found once, lost after this many misses */
#define LINK_PROBE_ASKS  4
#define LINK_PROBE_SPINS 4000u  /* two milliseconds; a transfer is a third of one */

/* g_probe_fast: the port is at the BIOS's speed with its interrupt off, as
 * an attempt leaves it; put back before the lobby next speaks. */

static void probe_port_back(void) {
    if (!g_probe_fast) return;
    g_probe_fast = false;
    REG_IME = 0;
    REG_SIOCNT = SIO_NORMAL_SO_HIGH;
    REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD;
    REG_IF = IRQ_SERIAL;
    REG_IE = (uint16_t)(REG_IE | IRQ_SERIAL);
    REG_SIOCNT = (uint16_t)(REG_SIOCNT | SIO_IRQ);
    load_send();
    REG_IME = IME_ON;
}

/* One attempt, as link_multiboot_send begins one. */
static bool probe_ask(void) {
    REG_IME = 0;
    REG_SIOCNT = (uint16_t)(REG_SIOCNT & ~SIO_IRQ);
    REG_IE = (uint16_t)(REG_IE & ~IRQ_SERIAL);
    REG_IME = IME_ON;
    REG_SIOCNT = SIO_NORMAL_SO_HIGH;
    REG_SIOCNT = SIO_MODE_MULTI | SIO_BAUD_115200;
    g_probe_fast = true;
    /* SD takes its time to come up after the change of mode (the line is
     * pulled up, not driven): a tenth of a millisecond before it is read.
     * Low, it is nobody in multiplayer mode on the other end: nothing to
     * ask, and a start would only set the error bit. */
    for (volatile int wait = 0; wait < 160; wait++) { }
    if (!(REG_SIOCNT & SIO_SD)) return false;
    for (int ask = 0; ask < LINK_PROBE_ASKS; ask++)
        if ((mb_transfer_within(0x6200, LINK_PROBE_SPINS) & 0xFFF0) == 0x7200)
            return true;
    return false;
}

void link_probe(void) {
    if (wireless_on()) return;      /* no cable to ask */
    if (!g_armed || !link_is_master()) {
        probe_port_back();
        g_peer_empty = 0;
        g_probe_clock = 0;
        return;
    }
    g_probe_clock++;
    if (!g_peer_empty && g_probe_clock <= LINK_PROBE_TALK) {
        probe_port_back();          /* the lobby's turn */
        return;
    }
    g_hush = 2;
    uint8_t quiet = g_peer_empty ? g_probe_clock
                                 : (uint8_t)(g_probe_clock - LINK_PROBE_TALK);
    if (quiet % LINK_PROBE_STEP) return;
    /* Found at once, lost only after LINK_PROBE_KEEP attempts in a row say
     * so: the screen should not flicker for one that went astray. */
    if (probe_ask()) g_peer_empty = LINK_PROBE_KEEP;
    else if (g_peer_empty) g_peer_empty--;
    if (g_peer_empty || quiet >= LINK_PROBE_QUIET) g_probe_clock = 0;
}

#ifndef TENGEN_MULTIBOOT
/* BETWEEN TWO ROUNDS OF THE PROBE, or with none running: the moment for
 * anything else that takes the port away — the search for a wireless
 * adapter plugged in late (main.c), which starts the cable over. In the
 * middle of a round it cut the silence short after one attempt and began
 * the count again with the lobby's words: one round in every six was half
 * a round (run_link.py, peer_check, read it off the wire). */
bool link_probe_resting(void) {
    return g_probe_clock == 0;
}
#endif

LinkPeer link_peer(void) {
    if (wireless_on())
        return wireless_linked() && link_connected() ? LINK_PEER_SOMEONE
                                                     : LINK_PEER_NOBODY;
    if (g_peer_empty) return LINK_PEER_EMPTY_GBA;
    return link_connected() ? LINK_PEER_SOMEONE : LINK_PEER_NOBODY;
}

/* A CONSOLE OF OURS IN ITS MENUS SAYS IT IS THERE. The slave's end of the
 * cable cannot tell "no cable" from "the console on the master's end is in
 * its menus": both are a cable with no transfers on it (link_peer). So the
 * master's end, with its port at rest (link_rest, link_shutdown) and away
 * from any lobby, starts an empty transfer now and then: a word of 0, which
 * a lobby on the other end takes for a console not in one (tag NONE) —
 * enough for its LINK CABLE screen to say "waiting for the other player"
 * rather than "connect the cable". Not on the slave's end (only the master
 * starts transfers), not with SD low (nobody in multiplayer mode there),
 * and with the port's interrupt off, as it is at rest — or on for a title,
 * which takes its two words and nothing else (title_service): nothing here
 * reaches this console's own lobby queue. */
#define LINK_BEACON_EVERY 8
static uint8_t g_beacon_clock;

/* ...AND A TITLE SAYS IT IS A TITLE. Two consoles left
 * on their titles with a cable between them start their demos as ONE
 * cooperative game, the two computers on one board (main.c, the title) —
 * and for that each has to know where the other is. So the word a console
 * at rest answers with, and the master's beacon carries, is not always 0
 * on the title:
 *
 *      0000 1000 0000 00di      tag NONE, as 0 is; bit 11 says "a title";
 *                               i: nobody has touched it for a while
 *                               (LINK_TITLE_IDLE); d: its demo is due
 *                               (LINK_TITLE_DUE)
 *
 * TAG NONE, so that every lobby there has ever been reads it as it reads
 * 0: a console that is there and not in a lobby. And nothing is asked of a
 * console that does not say it: a build from before this, a console
 * without the chord or anywhere but its title answer 0 and are left alone.
 * (A Single-Pak copy says it like a cartridge: the first build kept this
 * to cartridges, and the first two consoles it was tried on were a
 * cartridge and its copy.)
 *
 * TAKEN BY THE INTERRUPT, AS EVERY OTHER WORD ON THIS CABLE IS. The first
 * build read the data registers from the main loop, once a frame, with no
 * interrupt at rest — and never with a transfer passing, which is right
 * and which was the hole in it: a master asking for a demo starts a
 * transfer every frame, so a slave whose frame falls on those transfers
 * finds one passing every time it looks, sees nothing, cannot even load
 * its own next word, and stays that way for as long as the two consoles'
 * frames stay in step — seconds, on two crystals. (The emulated cable
 * whose transfers take the rest of the frame shows it every time; the one
 * whose transfers take no time, which that build was checked on, never.)
 * So a title has the port's interrupt on (title_listen): what a transfer
 * carried is taken as it ENDS, both words whole and the port free for the
 * next one's to be loaded, and the main loop reads what the interrupt
 * left (g_title_news). Which slot is this console's is the transfer's own
 * answer, the ID in SIOCNT.
 *
 * The master asks by simply starting the lobby of a demo
 * (tengen_lobby_demo): the slave's end sees its HELLO in the master's
 * slot, on two transfers running (link_title_invites), and starts its
 * own. */
#define LINK_TITLE_MARK   0x0800u
#define LINK_TITLE_IS     0xF800u
#define LINK_TITLE_FRESH  20      /* frames a word is news for: two beacons and a half,
                                   * so that one transfer the cable drops is not a title gone */
/* (Whole, with no build's number in it: tengen_lobby_version.) */
#define LINK_DEMO_HELLO   ((uint16_t)((TENGEN_LOBBY_HELLO << TENGEN_LOBBY_TAG_SHIFT) | \
                                      TENGEN_HELLO_COOP | TENGEN_HELLO_DEMO))
static uint8_t g_title_age = 0xFF;  /* frames since the other's word; 0xFF: none */
static int8_t g_title_other = -1;   /* the other's LINK_TITLE_*, or -1: no title */
static uint8_t g_title_invites;
static bool g_title_master;

/* The interrupt's part: the transfer's two words for the loop, and this
 * console's for the next transfer while the port is free. */
static __attribute__((noinline)) void title_service(uint16_t cnt) {
    g_title_m = REG_SIOMULTI(0);
    g_title_s = REG_SIOMULTI(1);
    g_title_slave = ((cnt >> SIO_ID_SHIFT) & 3) != 0;
    g_title_news = true;
    REG_SIOMLT_SEND = g_title_word;
}

static void title_forget(void) {
    g_title_age = 0xFF;
    g_title_other = -1;
    g_title_invites = 0;
}

/* The interrupt on for a title, or off again: only with no transfer
 * passing (a write to SIOCNT under one is a write to its start bit). False
 * if one was, and nothing was done: the next frame will do. */
static bool title_listen(bool on) {
    uint16_t cnt = REG_SIOCNT;
    if (cnt & SIO_START) return false;
    REG_IME = 0;
    g_title_news = false;
    if (on) {
        REG_SIOMLT_SEND = g_title_word;
        REG_IF = IRQ_SERIAL;            /* nothing of before is served */
        REG_IE = (uint16_t)(REG_IE | IRQ_SERIAL);
        REG_SIOCNT = (uint16_t)(cnt | SIO_IRQ);
    } else {
        REG_SIOCNT = (uint16_t)(cnt & ~SIO_IRQ);
        REG_IE = (uint16_t)(REG_IE & ~IRQ_SERIAL);
        REG_IF = IRQ_SERIAL;
        REG_SIOMLT_SEND = 0;
    }
    REG_IME = IME_ON;
    return true;
}

void link_title_tick(int flags) {
    uint16_t cnt = REG_SIOCNT;
    bool rest = !g_armed && !wireless_present() && !gbp_owns_serial() &&
                (cnt & 0x3000) == SIO_MODE_MULTI;
    bool listening = rest && (cnt & SIO_IRQ);
    if (!rest || flags < 0) {
        /* Off its title: 0 from the next transfer (the interrupt loads
         * it), and the interrupt off as soon as no transfer is passing.
         * (The port somebody's: whoever took it took the interrupt with
         * it — link_init, link_shutdown — and it is not this to touch.) */
        g_title_word = 0;
        if (listening) title_listen(false);
        title_forget();
        return;
    }
    /* THE WORD IS LOADED BY THE INTERRUPT, as each transfer ends, and by
     * the master's beacon as it starts one; never from here, where a slave
     * cannot know that a transfer is not about to start. So a change of
     * this console's word is on the wire a transfer late, a beacon's eight
     * frames at most, and nothing here is in that much of a hurry. */
    g_title_word = (uint16_t)(LINK_TITLE_MARK |
                              ((unsigned)flags & (LINK_TITLE_IDLE | LINK_TITLE_DUE)));
    if (!listening && !title_listen(true)) return;

    REG_IME = 0;
    bool news = g_title_news;
    uint16_t m = g_title_m, sl = g_title_s;
    bool slave = g_title_slave;
    g_title_news = false;
    REG_IME = IME_ON;
    uint16_t theirs = slave ? m : sl;
    if (news) {
        g_title_age = 0;
        g_title_master = !slave;
        g_title_other = (theirs & LINK_TITLE_IS) == LINK_TITLE_MARK
                            ? (int8_t)(theirs & (LINK_TITLE_IDLE | LINK_TITLE_DUE)) : -1;
        /* (Counted on either end; only the slave's is ever asked.) */
        if (theirs == LINK_DEMO_HELLO) {
            if (g_title_invites < 0xFF) g_title_invites++;
        } else {
            g_title_invites = 0;
        }
    } else if (g_title_age < 0xFF && ++g_title_age > LINK_TITLE_FRESH) {
        title_forget();
    }
}

int link_title_other(void) { return g_title_other; }
int link_title_invites(void) { return g_title_invites; }
bool link_title_master(void) { return g_title_master; }

/* The lobby of a demo, its first word a demo's: link_lobby_start_held
 * sends HELLO before a caller can say what for, and that one word would
 * be a player's. */
void link_lobby_start_demo(TengenLobby *lobby, uint16_t seed) {
    tengen_lobby_start_held(lobby, seed);
    tengen_lobby_mode(lobby, true);
    tengen_lobby_demo(lobby);
    g_lobby_master = link_is_master();
    lobby_send(lobby);
}

void link_beacon(void) {
    if (g_armed || wireless_present()) return;
    if (++g_beacon_clock % LINK_BEACON_EVERY) return;
    uint16_t cnt = REG_SIOCNT;
    if ((cnt & 0x3000) != SIO_MODE_MULTI) return;   /* not at rest */
    /* (The interrupt may be on at rest: a title's, link_title_tick.) */
    if (cnt & (SIO_SI | SIO_START)) return;
    if (!(cnt & SIO_SD)) return;
    REG_SIOMLT_SEND = g_title_word;     /* 0, but on a title */
    REG_SIOCNT = (uint16_t)(cnt | SIO_START);
}
