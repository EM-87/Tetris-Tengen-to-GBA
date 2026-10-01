/* wireless.c — THE GAME BOY ADVANCE WIRELESS ADAPTER (AGB-015), as the
 * cable's understudy: two consoles with an adapter each play 2 PLAYER and
 * COOPERATIVE exactly as over the cable, the lobby and the lockstep
 * unchanged above link.c.
 *
 * Sources, both of which run on hardware: GBATEK ("GBA Wireless Adapter":
 * the reset, the NINTENDO login, the command framing) and gba-link-
 * connection by afska (docs/wireless_adapter.md, LinkRawWireless.hpp, which
 * reverse-engineered the commands this needs and the handshake between
 * transfers). Nothing here has met an adapter: it is checked against the
 * stand-in in tools/run_link.py (FakeAdapter), which is that documentation
 * written as a model (INFERRED until two real adapters agree).
 *
 * THREE LAYERS.
 *
 * The wire: the serial port in 32-bit normal mode on the GBA's clock
 * (256 kbit/s for the login, 2 Mbit/s after), and after every transfer of a
 * command the "ready" handshake on SO and SI (wl_ack). Every wait is on the
 * scanline counter and gives up after a few lines: no interrupt is used, so
 * the Single-Pak copy — which cannot take one — can run it too.
 *
 * The session: each console looks for a Tengen game being hosted for a
 * random while, joins it if there is one, and otherwise hosts one itself
 * (and goes back to looking, now and then, if nobody comes — two consoles
 * that started hosting together part company that way). The host is the
 * cable's master, the client its slave.
 *
 * The transport, with link.c's queue of transfers above it:
 *   - THE LOBBY (and the records swap) are stop-and-wait, so they get what
 *     the cable gives them: transfers. The host sends its word with a
 *     sequence number until the client's answer to that number comes back;
 *     the client answers each new number with the word it had loaded, as a
 *     slave does. Each answered number is one transfer, the same pair on
 *     both consoles.
 *   - THE MATCH is lockstep by frame, with the input DELAYED by
 *     WL_DELAY frames: the radio is slower than a cable and does not
 *     answer within the transfer, so each console sends its buttons for the
 *     frames ahead and a frame is played when both consoles' words for it
 *     are in. The pairs it makes are what the cable's would be — the same
 *     frame counter in both words — so the match code cannot tell. */
#include "port.h"

/* Not on the Single-Pak copy, which came over a cable and has no room to
 * spare: there the adapter is never there (stubs at the bottom). */
#ifndef TENGEN_MULTIBOOT

#define WL_SIOCNT  (*(vu16 *)0x04000128)
#define WL_SIODATA (*(vu32 *)0x04000120)
#define WL_RCNT    (*(vu16 *)0x04000134)

#define SIO_CLOCK_INTERNAL 0x0001
#define SIO_CLOCK_2MHZ     0x0002
#define SIO_SI_BIT         0x0004
#define SIO_SO_BIT         0x0008
#define SIO_START_BIT      0x0080
#define SIO_32BIT          0x1000

#define WL_PING_LINES     50       /* SD held high for the reset */
#define WL_GAP_LINES      15       /* between login transfers */
#define WL_TIMEOUT_LINES  15       /* any one wait, then give up */

#define CMD_HELLO        0x10
#define CMD_SETUP        0x17
#define CMD_BROADCAST    0x16
#define CMD_START_HOST   0x19
#define CMD_POLL_CONNS   0x1A
#define CMD_END_HOST     0x1B
#define CMD_READ_START   0x1C
#define CMD_READ_POLL    0x1D
#define CMD_READ_END     0x1E
#define CMD_CONNECT      0x1F
#define CMD_IS_CONNECTED 0x20
#define CMD_FINISH_CONN  0x21
#define CMD_SEND_DATA    0x24
#define CMD_RECEIVE_DATA 0x26
#define CMD_BYE          0x3D
#define WL_DATA_REQUEST  0x80000000u
#define WL_STILL_CONNECTING 0x01000000u

/* Ours among the broadcasts. Bit 15 would mean a wireless multiboot room. */
#define WL_GAME_ID 0x7E77u

/* ----------------------------------------------------------------------- *
 * The wire
 * ----------------------------------------------------------------------- */

/* Waits on the scanline counter: `lines` of them pass. */
static void wl_wait_lines(int lines) {
    uint16_t v = REG_VCOUNT;
    while (lines > 0) {
        uint16_t now = REG_VCOUNT;
        if (now != v) { v = now; lines--; }
    }
}

/* A wait with a deadline: true while there is time left. */
typedef struct { uint16_t v; int lines; } WlClock;
static void wl_clock(WlClock *c) { c->v = REG_VCOUNT; c->lines = 0; }
static bool wl_in_time(WlClock *c) {
    uint16_t now = REG_VCOUNT;
    if (now != c->v) { c->v = now; c->lines++; }
    return c->lines < WL_TIMEOUT_LINES;
}

static void wl_so(bool high) {
    uint16_t cnt = WL_SIOCNT;
    WL_SIOCNT = high ? (uint16_t)(cnt | SIO_SO_BIT) : (uint16_t)(cnt & ~SIO_SO_BIT);
}

static bool wl_si(void) {
    return (WL_SIOCNT & SIO_SI_BIT) != 0;
}

/* THE RESET: general purpose, SO and SD as outputs, SD high a while and low
 * again (GBATEK's 8000h, 80A0h, 80A2h, wait, 80A0h). */
static void wl_ping(void) {
    WL_RCNT = 0x8000;
    WL_RCNT = 0x80A0;
    WL_RCNT = 0x80A2;
    wl_wait_lines(WL_PING_LINES);
    WL_RCNT = 0x80A0;
}

/* Back to the serial modes, 32-bit normal on the GBA's clock, SO high (no
 * transfer wanted), at one of the two speeds. */
static void wl_spi(bool fast) {
    WL_RCNT = (uint16_t)(WL_RCNT & ~0x8000);
    WL_SIOCNT = SIO_32BIT;
    WL_SIOCNT = (uint16_t)(SIO_32BIT | SIO_SO_BIT | SIO_CLOCK_INTERNAL |
                           (fast ? SIO_CLOCK_2MHZ : 0));
}

static bool g_wl_ok;   /* the last transfer completed in time */

/* One 32-bit exchange: SO low to say "go", start, wait for the end. A
 * command's transfers keep SO low after (the handshake follows); the
 * login's take it high again. */
static uint32_t wl_transfer(uint32_t out, bool handshake) {
    WL_SIODATA = out;
    wl_so(false);
    WL_SIOCNT = (uint16_t)(WL_SIOCNT | SIO_START_BIT);
    WlClock c;
    wl_clock(&c);
    while (WL_SIOCNT & SIO_START_BIT) {
        if (!wl_in_time(&c)) {
            WL_SIOCNT = (uint16_t)(WL_SIOCNT & ~SIO_START_BIT);
            wl_so(true);
            g_wl_ok = false;
            return WL_DATA_REQUEST ^ 0xFFFFFFFFu;
        }
    }
    if (!handshake) wl_so(true);
    return WL_SIODATA;
}

/* THE "READY" HANDSHAKE after a command transfer (gba-link-connection's
 * acknowledge): SO low, the adapter answers SI high; SO high, it answers SI
 * low when it is ready for the next; SO low again. */
static bool wl_ack(void) {
    WlClock c;
    wl_clock(&c);
    wl_so(false);
    while (!wl_si()) if (!wl_in_time(&c)) return false;
    wl_so(true);
    while (wl_si()) if (!wl_in_time(&c)) return false;
    wl_so(false);
    return true;
}

static uint32_t wl_cmd_transfer(uint32_t out) {
    uint32_t in = wl_transfer(out, true);
    if (!g_wl_ok) return in;
    if (!wl_ack()) g_wl_ok = false;
    return in;
}

/* THE LOGIN: "NINTENDO" a halfword at a time, each word carrying the
 * inverse of what the adapter said last in its high half; the adapter
 * answers with the part and the inverse of the GBA's last. The first two
 * answers are junk (GBATEK; LinkRawWireless's LOGIN_JUNK_STEPS). */
static const uint16_t kLoginParts[10] = {
    0x494E, 0x494E, 0x494E, 0x544E, 0x544E,
    0x4E45, 0x4E45, 0x4F44, 0x4F44, 0x8001
};

static bool wl_login(void) {
    uint16_t prev_gba = 0, prev_adapter = 0;
    for (int i = 0; i < 10; i++) {
        wl_wait_lines(WL_GAP_LINES);
        g_wl_ok = true;
        uint32_t out = ((uint32_t)(uint16_t)~prev_adapter << 16) | kLoginParts[i];
        uint32_t in = wl_transfer(out, false);
        if (!g_wl_ok) return false;
        uint16_t adapter = (uint16_t)(in >> 16);
        if (i >= 2 && (adapter != kLoginParts[i] ||
                       (uint16_t)in != (uint16_t)~prev_gba))
            return false;
        prev_gba = kLoginParts[i];
        prev_adapter = adapter;
    }
    return true;
}

/* A COMMAND: 9966LLCCh, LL parameter words, then a request for the answer,
 * 9966RRAAh with AA = CC + 80h, and RR words of it. Each of the GBA's words
 * is answered 80000000h while the adapter is listening. Returns the number
 * of answer words kept in `out`, or -1. */
static int wl_command(uint8_t cmd, const uint32_t *params, int n,
                      uint32_t *out, int max_out) {
    g_wl_ok = true;
    uint32_t r = wl_cmd_transfer(0x99660000u | ((uint32_t)n << 8) | cmd);
    if (!g_wl_ok || r != WL_DATA_REQUEST) return -1;
    for (int i = 0; i < n; i++) {
        r = wl_cmd_transfer(params[i]);
        if (!g_wl_ok || r != WL_DATA_REQUEST) return -1;
    }
    r = wl_cmd_transfer(WL_DATA_REQUEST);
    if (!g_wl_ok || (r >> 16) != 0x9966) return -1;
    int count = (int)((r >> 8) & 0xFF);
    if ((r & 0xFF) != (uint32_t)(cmd + 0x80)) {
        if ((r & 0xFF) == 0xEE && count == 1) wl_cmd_transfer(WL_DATA_REQUEST);
        return -1;
    }
    int kept = 0;
    for (int i = 0; i < count; i++) {
        uint32_t w = wl_cmd_transfer(WL_DATA_REQUEST);
        if (!g_wl_ok) return -1;
        if (kept < max_out) out[kept++] = w;
    }
    return kept;
}

/* Reset, login, Hello at the slow clock, then the fast one and Setup:
 * two players, four transmissions, no wait timeout (LinkRawWireless's
 * setup, magic 003C0000h). */
static bool wl_start(void) {
    wl_ping();
    wl_spi(false);
    if (!wl_login()) return false;
    wl_wait_lines(WL_GAP_LINES);
    if (wl_command(CMD_HELLO, 0, 0, 0, 0) < 0) return false;
    wl_spi(true);
    uint32_t setup = 0x003C0000u | (3u << 16) | (4u << 8) | 0u;
    return wl_command(CMD_SETUP, &setup, 1, 0, 0) >= 0;
}

/* ----------------------------------------------------------------------- *
 * Is there an adapter?
 * ----------------------------------------------------------------------- */

static bool g_wl_present;

/* Asked once at power-on (main.c), before the cable is ever used: a login
 * that the adapter answers. Without one the port goes back to the cable's
 * rest (link_rest). The reset's general-purpose flutter is a one-off. */
bool wireless_detect(void) {
    g_wl_present = wl_start();
    if (g_wl_present) wl_command(CMD_BYE, 0, 0, 0, 0);
    return g_wl_present;
}

bool wireless_present(void) {
    return g_wl_present;
}

/* ----------------------------------------------------------------------- *
 * The session
 * ----------------------------------------------------------------------- */

typedef enum {
    WL_OFF, WL_SEARCH, WL_CONNECTING, WL_HOSTING, WL_LINKED
} WlState;

static WlState g_state = WL_OFF;
static bool g_host;              /* this console hosts: the cable's master */
static int g_timer;              /* frames in the state */
static int g_limit;              /* ...and how many it gets */
static uint16_t g_rand = 0x1D2B;
/* THE PAIR OUTLIVES THE RADIO. Once two consoles have found each other
 * they keep their roles until link_shutdown: if either side goes quiet
 * (an adapter knocked out, a console carried out of range) both start
 * over, the host straight to hosting and the client only to searching,
 * and the transport carries on from where it stood — the match waits
 * meanwhile with LINK ISSUES, as for a cable. */
static bool g_paired;
static int g_quiet;              /* linked frames with nothing heard */
static int g_off_wait;           /* frames before the next login try */
#define WL_LOST_FRAMES 90        /* well inside LINK_GIVEUP_FRAMES */
#define WL_RETRY_FRAMES 30       /* a login that failed is tried again */

static int wl_random(int lo, int hi) {
    g_rand = (uint16_t)(g_rand * 25173u + 13849u + REG_VCOUNT);
    return lo + (int)(g_rand % (uint16_t)(hi - lo + 1));
}

static void wl_go(WlState s) {
    g_state = s;
    g_timer = 0;
}

static void wl_search_begin(void) {
    if (wl_command(CMD_READ_START, 0, 0, 0, 0) < 0) { g_state = WL_OFF; return; }
    wl_go(WL_SEARCH);
    g_limit = wl_random(60, 150);
}

static void wl_host_begin(void);

static void wl_host_begin(void) {
    /* Game ID, "TENGEN TETRIS", "PLAYER", the way LinkRawWireless lays
     * the six words out (two characters to a halfword, low first). */
    static const char game[14] = "TENGEN TETRIS";
    static const char user[8] = "PLAYER";
    uint32_t b[6];
    b[0] = WL_GAME_ID | ((uint32_t)(uint8_t)game[0] << 16) |
           ((uint32_t)(uint8_t)game[1] << 24);
    for (int w = 1; w < 4; w++) {
        int k = 2 + (w - 1) * 4;
        b[w] = (uint32_t)(uint8_t)game[k] | ((uint32_t)(uint8_t)game[k + 1] << 8) |
               ((uint32_t)(uint8_t)game[k + 2] << 16) |
               ((uint32_t)(uint8_t)game[k + 3] << 24);
    }
    for (int w = 4; w < 6; w++) {
        int k = (w - 4) * 4;
        b[w] = (uint32_t)(uint8_t)user[k] | ((uint32_t)(uint8_t)user[k + 1] << 8) |
               ((uint32_t)(uint8_t)user[k + 2] << 16) |
               ((uint32_t)(uint8_t)user[k + 3] << 24);
    }
    if (wl_command(CMD_BROADCAST, b, 6, 0, 0) < 0 ||
        wl_command(CMD_START_HOST, 0, 0, 0, 0) < 0) {
        g_state = WL_OFF;
        return;
    }
    wl_wait_lines(WL_GAP_LINES);
    wl_go(WL_HOSTING);
    g_limit = wl_random(180, 300);
}

static void wl_transport_reset(void);

/* Linked: a first time, from scratch; again, the transport as it was. */
static void wl_linked(bool host) {
    if (!g_paired) {
        g_host = host;
        wl_transport_reset();
    }
    g_paired = true;
    g_quiet = 0;
    wl_go(WL_LINKED);
}

/* Once a frame until linked. */
static void wl_session_step(void) {
    uint32_t r[28];
    int n;
    g_timer++;
    switch (g_state) {
        case WL_OFF:
            /* No adapter answering: try again, now and then — a login that
             * fails costs most of a frame in waits. */
            if (g_off_wait > 0) { g_off_wait--; return; }
            if (!wl_start()) { g_off_wait = WL_RETRY_FRAMES; return; }
            if (g_paired && g_host) wl_host_begin();
            else wl_search_begin();
            return;
        case WL_SEARCH:
            if (g_timer % 10) return;
            n = wl_command(CMD_READ_POLL, 0, 0, r, 28);
            if (n < 0) { g_state = WL_OFF; return; }
            for (int i = 0; i + 7 <= n; i += 7) {
                uint16_t id = (uint16_t)r[i];
                uint8_t slot = (uint8_t)(r[i] >> 16);
                if ((r[i + 1] & 0x7FFF) != WL_GAME_ID || slot == 0xFF) continue;
                uint32_t param = id;
                if (wl_command(CMD_READ_END, 0, 0, 0, 0) < 0 ||
                    wl_command(CMD_CONNECT, &param, 1, 0, 0) < 0) {
                    g_state = WL_OFF;
                    return;
                }
                wl_go(WL_CONNECTING);
                return;
            }
            if (g_timer >= g_limit) {
                if (wl_command(CMD_READ_END, 0, 0, 0, 0) < 0) { g_state = WL_OFF; return; }
                /* A client that has lost its host only looks for it. */
                if (g_paired) wl_search_begin();
                else wl_host_begin();
            }
            return;
        case WL_CONNECTING:
            n = wl_command(CMD_IS_CONNECTED, 0, 0, r, 1);
            if (n < 1) { g_state = WL_OFF; return; }
            if (r[0] == WL_STILL_CONNECTING) {
                if (g_timer > 120) g_state = WL_OFF;
                return;
            }
            if (((r[0] >> 16) & 0xFF) > 3 ||
                wl_command(CMD_FINISH_CONN, 0, 0, r, 1) < 1) {
                g_state = WL_OFF;
                return;
            }
            wl_linked(false);
            return;
        case WL_HOSTING:
            n = wl_command(CMD_POLL_CONNS, 0, 0, r, 4);
            if (n < 0) { g_state = WL_OFF; return; }
            if (n > 0) {
                /* Two players is a full room: close it, keep the client. */
                wl_command(CMD_END_HOST, 0, 0, r, 4);
                wl_linked(true);
                return;
            }
            /* Nobody came: start over, looking first this time — unless
             * this is a host waiting for its client to come back. */
            if (g_timer >= g_limit && !g_paired) g_state = WL_OFF;
            return;
        case WL_LINKED:
            return;
    }
}

/* ----------------------------------------------------------------------- *
 * The transport
 * ----------------------------------------------------------------------- */

/* Packet words, the first one a header: kind in bits 30-31, then the
 * kind's own fields. LOBBY: sequence in bits 16-21, the word in 0-15.
 * MATCH: in bits 16-23 the first frame carried, in 24-25 how many (1-3),
 * in 0-7 the sender's own count of frames played (so the other side knows
 * which of its frames to send next), bit 29 if it has left the match
 * (g_tail); the words follow, two to a word. */
#define PK_LOBBY 1u
#define PK_MATCH 2u
#define PK_LEFT  (1u << 29)        /* MATCH: sent from the tail, see g_tail */
#define WL_DELAY 3                 /* frames of input delay in a match */
#define WL_RING  64

static bool g_match;               /* the transport's mode */
/* THE MATCH'S TAIL. The two consoles do not leave a match on the same
 * frame of their own: the input is delayed, and the one that reached the
 * last frame first went on to the records with the other still a few
 * frames short of it — frames only the first could send. So a console
 * that leaves a match keeps sending its frames, from where the other has
 * played, until the other is heard from the records too. */
static bool g_tail;
/* The lobby's stop-and-wait. */
static uint8_t g_seq;              /* host: the number in flight */
static uint16_t g_seq_word;        /* host: its word */
static bool g_seq_answered;        /* host: its answer came */
static uint8_t g_last_seq;         /* client: the last number answered */
static uint16_t g_reply;           /* client: what it was answered with */
/* The match's frames. */
static uint16_t g_own[WL_RING];
static uint16_t g_remote[WL_RING];
static uint8_t g_remote_have[WL_RING];
static uint32_t g_own_next;        /* own words made: frames 0..own_next-1 */
static uint32_t g_played;          /* frames both words are in for */
static uint32_t g_remote_played;   /* the other side's, last heard */

static void wl_transport_reset(void) {
    g_match = false;
    g_tail = false;
    g_seq = 1;
    g_seq_word = link_tx_word();
    g_seq_answered = false;
    g_last_seq = 0xFF;
    for (int i = 0; i < WL_RING; i++) g_remote_have[i] = 0;
    g_own_next = g_played = g_remote_played = 0;
}

/* link_play_begin, over the air: frame-indexed, the first WL_DELAY frames
 * of both consoles no buttons at all. */
void wireless_match_begin(void) {
    g_match = true;
    g_played = 0;
    g_remote_played = 0;
    g_own_next = 0;
    while (g_own_next < WL_DELAY) {
        g_own[g_own_next % WL_RING] = tengen_link_pack(0, (uint8_t)g_own_next);
        g_own_next++;
    }
}

/* link_name_start and the lobby: transfers again. */
void wireless_lobby_begin(void) {
    g_tail = g_match;          /* the ring and the counts stay for it */
    g_match = false;
    g_seq++;
    g_seq &= 0x3F;
    if (!g_seq) g_seq = 1;
    g_seq_word = link_tx_word();
    g_seq_answered = false;
    g_last_seq = 0xFF;
}

static void wl_take_match(const uint32_t *w, int n) {
    if (n < 1) return;
    uint32_t first = (w[0] >> 16) & 0xFF;
    int count = (int)((w[0] >> 24) & 3);
    uint32_t heard = w[0] & 0xFF;
    /* Eight-bit counts, unwrapped against our own. */
    uint32_t base = g_played & ~0xFFu;
    uint32_t f0 = base | first;
    if (f0 + 128 < g_played) f0 += 256;
    else if (f0 > g_played + 128 && f0 >= 256) f0 -= 256;
    uint32_t rp = base | heard;
    if (rp + 128 < g_played) rp += 256;
    else if (rp > g_played + 128 && rp >= 256) rp -= 256;
    if (rp > g_remote_played) g_remote_played = rp;
    for (int i = 0; i < count && 1 + i / 2 < n; i++) {
        uint32_t f = f0 + (uint32_t)i;
        uint16_t word = (uint16_t)(i & 1 ? w[1 + i / 2] >> 16 : w[1 + i / 2]);
        if (f >= g_played && f < g_played + WL_RING) {
            g_remote[f % WL_RING] = word;
            g_remote_have[f % WL_RING] = 1;
        }
    }
    link_heard();
}

static void wl_take(const uint32_t *w, int n) {
    if (n < 1) return;
    g_quiet = 0;
    uint32_t kind = w[0] >> 30;
    if (kind == PK_MATCH) {
        wl_take_match(w, n);
        /* Both left, each still sending the other its tail: done. */
        if (w[0] & PK_LEFT) g_tail = false;
        /* A client still in the lobby when the host's match begins: the
         * host's GO has been answered, and a match word from it is the
         * lobby's signal to go (tengen_lobby_apply, saw_go). */
        /* And the other way: a host still at GO whose answer was lost, and
         * a client gone to the match, which answers lobby words no more —
         * its first move is the echo (tengen_lobby_apply, the master at
         * GO), as on the cable. */
        if (!g_match && !g_tail && n >= 2) {
            if (g_host) link_push_pair(g_seq_word, (uint16_t)w[1]);
            else link_push_pair((uint16_t)w[1], link_tx_word());
        }
        return;
    }
    if (kind != PK_LOBBY || g_match) return;
    g_tail = false;                    /* the other has left the match too */
    uint8_t seq = (uint8_t)((w[0] >> 16) & 0x3F);
    uint16_t word = (uint16_t)w[0];
    link_heard();
    if (g_host) {
        if (seq == g_seq && !g_seq_answered) {
            g_seq_answered = true;
            link_push_pair(g_seq_word, word);
        }
    } else if (seq != g_last_seq) {
        g_last_seq = seq;
        g_reply = link_tx_word();
        link_push_pair(word, g_reply);
    }
}

/* What goes out this frame. */
static int wl_packet(uint32_t *p) {
    if (!g_match && !g_tail) {
        if (g_host) {
            if (g_seq_answered) {
                g_seq = (uint8_t)((g_seq + 1) & 0x3F);
                if (!g_seq) g_seq = 1;
                g_seq_word = link_tx_word();
                g_seq_answered = false;
            }
            p[0] = (PK_LOBBY << 30) | ((uint32_t)g_seq << 16) | g_seq_word;
        } else {
            p[0] = (PK_LOBBY << 30) | ((uint32_t)(g_last_seq & 0x3F) << 16) |
                   g_reply;
        }
        return 1;
    }
    /* The frames the other side still lacks, from where it has played:
     * the oldest first, whatever got lost on the way. */
    uint32_t from = g_remote_played;
    if (from > g_own_next) from = g_own_next;
    int count = (int)(g_own_next - from);
    if (count > 3) count = 3;
    if (count < 0) count = 0;
    p[0] = (PK_MATCH << 30) | ((uint32_t)(count & 3) << 24) |
           ((from & 0xFF) << 16) | (g_played & 0xFF) | (g_tail ? PK_LEFT : 0);
    p[1] = p[2] = 0;
    for (int i = 0; i < count; i++) {
        uint32_t w = g_own[(from + (uint32_t)i) % WL_RING];
        p[1 + i / 2] |= i & 1 ? w << 16 : w;
    }
    return 1 + (count + 1) / 2;
}

/* Frames both words are in for go to link.c as transfers; and our own
 * words, WL_DELAY ahead, are made with the buttons of now. */
static void wl_play(void) {
    while (g_played < g_own_next && g_remote_have[g_played % WL_RING]) {
        uint16_t own = g_own[g_played % WL_RING];
        uint16_t remote = g_remote[g_played % WL_RING];
        g_remote_have[g_played % WL_RING] = 0;
        if (g_host) link_push_pair(own, remote);
        else link_push_pair(remote, own);
        g_played++;
    }
    while (g_own_next < g_played + WL_DELAY) {
        g_own[g_own_next % WL_RING] =
            tengen_link_pack(link_read_buttons(), (uint8_t)(g_own_next & 31));
        g_own_next++;
    }
}

/* SendData with its header, ReceiveData and what it holds. */
static bool wl_exchange(void) {
    uint32_t p[5];
    int n = wl_packet(p + 1);
    uint32_t bytes = (uint32_t)n * 4;
    p[0] = g_host ? bytes : bytes << 8;     /* client 0's field */
    uint32_t r[24];
    if (g_host) {
        if (wl_command(CMD_SEND_DATA, p, n + 1, 0, 0) < 0) return false;
        int m = wl_command(CMD_RECEIVE_DATA, 0, 0, r, 24);
        if (m < 0) return false;
        if (m >= 2 && ((r[0] >> 8) & 0x1F)) wl_take(r + 1, m - 1);
    } else {
        int m = wl_command(CMD_RECEIVE_DATA, 0, 0, r, 24);
        if (m < 0) return false;
        if (m >= 2 && (r[0] & 0x7F)) wl_take(r + 1, m - 1);
        if (wl_command(CMD_SEND_DATA, p, n + 1, 0, 0) < 0) return false;
    }
    return true;
}

/* ----------------------------------------------------------------------- *
 * To link.c
 * ----------------------------------------------------------------------- */

static bool g_wl_on;

void wireless_open(void) {
    g_wl_on = true;
    g_state = WL_OFF;
    g_host = false;
    g_paired = false;
    g_off_wait = 0;
    wl_transport_reset();
}

void wireless_close(void) {
    if (!g_wl_on) return;
    g_wl_on = false;
    wl_command(CMD_BYE, 0, 0, 0, 0);
    g_state = WL_OFF;
}

bool wireless_on(void) {
    return g_wl_on;
}

bool wireless_linked(void) {
    return g_wl_on && g_state == WL_LINKED;
}

bool wireless_host(void) {
    return g_host;
}

bool wireless_hosting(void) {
    return g_wl_on && g_state == WL_HOSTING;
}

/* Once a frame, from link_pump, on both consoles. */
void wireless_frame(void) {
    if (!g_wl_on) return;
    if (g_state != WL_LINKED) {
        wl_session_step();
        return;
    }
    if (g_match) wl_play();
    /* The adapter stopped answering, or the other console has not been
     * heard for a while: start the session over, in the same roles. The
     * match above waits with LINK ISSUES meanwhile, as for a cable. */
    if (!wl_exchange() || ++g_quiet > WL_LOST_FRAMES) {
        wl_command(CMD_BYE, 0, 0, 0, 0);
        g_state = WL_OFF;
        g_off_wait = 0;
        return;
    }
    if (g_match) wl_play();
}

#else  /* TENGEN_MULTIBOOT */

bool wireless_detect(void) { return false; }
bool wireless_present(void) { return false; }
void wireless_open(void) {}
void wireless_close(void) {}
bool wireless_on(void) { return false; }
bool wireless_linked(void) { return false; }
bool wireless_host(void) { return false; }
bool wireless_hosting(void) { return false; }
void wireless_frame(void) {}
void wireless_match_begin(void) {}
void wireless_lobby_begin(void) {}

#endif
