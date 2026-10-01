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
static bool wl_start_tx(unsigned max_tx) {
    wl_ping();
    wl_spi(false);
    if (!wl_login()) return false;
    wl_wait_lines(WL_GAP_LINES);
    if (wl_command(CMD_HELLO, 0, 0, 0, 0) < 0) return false;
    wl_spi(true);
    uint32_t setup = 0x003C0000u | (3u << 16) | (max_tx << 8) | 0u;
    return wl_command(CMD_SETUP, &setup, 1, 0, 0) >= 0;
}

static bool wl_start(void) {
    return wl_start_tx(4);
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
    if (g_wl_present) {
        wl_command(CMD_BYE, 0, 0, 0, 0);
    } else {
        /* Nobody answered: the port as the cable expects to find it, the
         * general-purpose bits the reset left behind cleared too. */
        WL_RCNT = 0;
        WL_SIOCNT = 0;
    }
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
/* THE SINGLE-PAK COPY ONLY EVER JOINS: its lobby follows the other
 * console's mode (tengen_lobby_mode_any), which a host — the master — has
 * nobody to follow for. So it looks for a room and never opens one. */
static bool g_client_only;

void wireless_client_only(void) {
    g_client_only = true;
}
static int g_quiet;              /* linked frames with nothing heard */
static int g_off_wait;           /* frames before the next login try */
static int g_login_fails;        /* logins in a row nobody answered */
#define WL_GONE_LOGINS 3         /* ...and unpaired, the adapter is gone */
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

/* The room's six words: game ID, "TENGEN TETRIS", "PLAYER", the way
 * LinkRawWireless lays them out (two characters to a halfword, low first);
 * then the room opened. */
static bool wl_open_room(uint16_t game_id) {
    static const char game[14] = "TENGEN TETRIS";
    static const char user[8] = "PLAYER";
    uint32_t b[6];
    b[0] = game_id | ((uint32_t)(uint8_t)game[0] << 16) |
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
    return wl_command(CMD_BROADCAST, b, 6, 0, 0) >= 0 &&
           wl_command(CMD_START_HOST, 0, 0, 0, 0) >= 0;
}

static void wl_host_begin(void) {
    if (!wl_open_room(WL_GAME_ID)) {
        g_state = WL_OFF;
        return;
    }
    wl_wait_lines(WL_GAP_LINES);
    wl_go(WL_HOSTING);
    g_limit = wl_random(180, 300);
}

static void wl_transport_reset(void);
static void wl_transport_relink(void);

/* Linked: a first time, from scratch; again, the transport as it was. */
static void wl_linked(bool host) {
    if (!g_paired) {
        g_host = host;
        wl_transport_reset();
    } else {
        wl_transport_relink();
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
            if (!wl_start()) {
                g_off_wait = WL_RETRY_FRAMES;
                /* Pulled out before anybody was found over it: the cable
                 * may have the port back (main.c, the LINK CABLE screen).
                 * A pair keeps trying — the match waits for its partner. */
                if (++g_login_fails >= WL_GONE_LOGINS && !g_paired) {
                    g_wl_present = false;
                    WL_RCNT = 0;
                    WL_SIOCNT = 0;
                }
                return;
            }
            g_login_fails = 0;
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
                /* All sixteen bits: with bit 15 it is a room sending the game
                 * (wireless_multiboot_send), not one to play in. */
                if ((r[i + 1] & 0xFFFF) != WL_GAME_ID || slot == 0xFF) continue;
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
                if (g_paired || g_client_only) wl_search_begin();
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
static bool g_played_match;        /* this session has had its match */
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
    g_played_match = false;
    g_seq = 1;
    g_seq_word = link_tx_word();
    g_seq_answered = false;
    g_last_seq = 0xFF;
    for (int i = 0; i < WL_RING; i++) g_remote_have[i] = 0;
    g_own_next = g_played = g_remote_played = 0;
}

/* Linked again, the pair as it was. Back in a lobby — perhaps with a
 * console that left it and came back, whose numbers start again — a
 * client answers whatever is sent first. */
static void wl_transport_relink(void) {
    if (!g_match && !g_tail && !g_host) g_last_seq = 0xFF;
}

/* link_play_begin, over the air: frame-indexed, the first WL_DELAY frames
 * of both consoles no buttons at all. */
void wireless_match_begin(void) {
    g_match = true;
    g_played_match = true;
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
        /* Only into the lobby BEFORE the match: after it, the other's
         * last tail packets, still coming while our records word goes
         * out, would land in the names swap as answers. */
        if (!g_match && !g_played_match && n >= 2) {
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
    g_login_fails = 0;
    wl_transport_reset();
}

/* A MATCH LEFT STRAIGHT FOR THE TITLE — EXIT GAME on the pause menu, which
 * both consoles take on the same frame of the match but not at the same
 * moment — goes nowhere near the records, where the tail is sent: the one
 * that got there first said Bye at once, and the other, still frames short
 * of the EXIT, waited ten seconds for them and ended under SIGNAL LOST. So
 * the tail goes out here, a frame at a time, until the other has left too
 * (bit 29 or a lobby word from it), or WL_FLUSH_FRAMES have gone by. */
#define WL_FLUSH_FRAMES 30

void wireless_close(void) {
    if (!g_wl_on) return;
    if (g_state == WL_LINKED && (g_match || g_tail)) {
        g_match = false;
        g_tail = true;
        for (int f = 0; f < WL_FLUSH_FRAMES && g_tail; f++) {
            vsync();
            if (!wl_exchange()) break;
        }
    }
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

/* ----------------------------------------------------------------------- *
 * The game, sent over the air
 *
 * A GBA switched on with no cartridge and an adapter in boots the adapter's
 * own loader, which lists the "multiboot" rooms around and downloads the
 * one the player picks. This is the other end: the room (game ID bit 15),
 * a handshake with the loader, the image in 84-byte packets, and the end.
 * The loader is Nintendo's and speaks Nintendo's own layer over the
 * adapter's data — a 3-byte header from the host, 2 from a client, with a
 * sequence number, an acknowledgement bit and a state — so everything
 * below is that layer, as gba-link-connection (afska) implements and runs
 * it on hardware: LinkWirelessOpenSDK.hpp for the layer,
 * LinkWirelessMultiboot.hpp for the steps, which are followed one by one.
 *
 * ONE DIFFERENCE, and it is INFERRED harmless: that code exchanges with
 * SendDataAndWait, where the adapter takes the clock back to say it has
 * transmitted; this uses SendData and ReceiveData, as its own LinkWireless
 * does for everything, a few times a frame, and lets the protocol's own
 * retries cover a packet that was not on the air yet. Checked only against
 * the stand-in loader in tools/run_wireless.py (INFERRED until a real
 * adapter has booted it).
 * ----------------------------------------------------------------------- */

#define MB_GAME_ID_FLAG   0x8000u
#define MB_PAYLOAD        84      /* bytes in one of the host's packets */
#define MB_INFLIGHT       4       /* packets sent and not yet acknowledged */
#define MB_PER_FRAME      4       /* exchanges with the adapter a frame */
#define MB_FINAL_OFFS     3
#define MB_PATIENCE       600     /* frames without progress, then give up */

enum { MB_OFF = 0, MB_STARTING = 1, MB_COMMUNICATING = 2, MB_ENDING = 3 };

typedef struct {
    uint8_t size, phase, n, ack, state;
} MbHeader;

/* The host's 22 bits: size 0-6, phase 9-10, n 11-12, ack 13, state 14-17,
 * the clients it is for 18-21. */
static uint32_t mb_server_header(MbHeader h, unsigned slots) {
    return (uint32_t)h.size | ((uint32_t)h.phase << 9) | ((uint32_t)h.n << 11) |
           ((uint32_t)h.ack << 13) | ((uint32_t)h.state << 14) |
           ((uint32_t)slots << 18);
}

/* A client's 14: size 0-4, phase 5-6, n 7-8, ack 9, state 10-13. */
static MbHeader mb_client_header(unsigned v) {
    MbHeader h;
    h.size = (uint8_t)(v & 0x1F);
    h.phase = (uint8_t)((v >> 5) & 3);
    h.n = (uint8_t)((v >> 7) & 3);
    h.ack = (uint8_t)((v >> 9) & 1);
    h.state = (uint8_t)((v >> 10) & 0xF);
    return h;
}

/* SequenceNumber::fromPacketId: a packet's number as (n, phase). */
static MbHeader mb_sequence(uint32_t id) {
    MbHeader h = { 0, (uint8_t)(id % 4), (uint8_t)(((id + 4) / 4) % 4), 0,
                   MB_COMMUNICATING };
    return h;
}

static bool mb_same(MbHeader a, MbHeader b) {
    return a.n == b.n && a.phase == b.phase && a.state == b.state;
}

/* What came back from the client last exchange: its packets, parsed. */
#define MB_MAX_PACKETS 8
static MbHeader g_mb_in[MB_MAX_PACKETS];
static uint8_t g_mb_in_payload[MB_MAX_PACKETS][14];
static int g_mb_in_count;

/* One exchange: the host's packet (words already laid out, `bytes` of
 * them counted), then what the client sent. False if the adapter did not
 * answer. */
static bool mb_exchange(const uint32_t *words, int n_words, unsigned bytes) {
    uint32_t p[24];
    p[0] = bytes;                       /* the host's field */
    for (int i = 0; i < n_words; i++) p[1 + i] = words[i];
    if (wl_command(CMD_SEND_DATA, p, 1 + n_words, 0, 0) < 0) return false;
    uint32_t r[24];
    int m = wl_command(CMD_RECEIVE_DATA, 0, 0, r, 24);
    if (m < 0) return false;
    g_mb_in_count = 0;
    if (m < 2) return true;
    unsigned client = (r[0] >> 8) & 0x1F;      /* client 0's bytes */
    if (client > 16) client = 16;
    const uint8_t *b = (const uint8_t *)&r[1];
    unsigned at = 0;
    if (client > (unsigned)(m - 1) * 4) return true;
    while (client - at >= 2 && g_mb_in_count < MB_MAX_PACKETS) {
        MbHeader h = mb_client_header((unsigned)(b[at] | (b[at + 1] << 8)));
        at += 2;
        for (unsigned j = 0; j < 14; j++) g_mb_in_payload[g_mb_in_count][j] = 0;
        if (h.size > 0 && h.size <= 14 && client - at >= h.size) {
            for (unsigned j = 0; j < h.size; j++)
                g_mb_in_payload[g_mb_in_count][j] = b[at + j];
            at += h.size;
        }
        g_mb_in[g_mb_in_count++] = h;
    }
    return true;
}

/* LinkWirelessOpenSDK::createServerBuffer: the header and the payload's
 * first byte in one word, the rest four to a word. */
static int mb_server_buffer(uint32_t *w, const uint8_t *data, uint32_t size,
                            MbHeader seq, unsigned slots, uint32_t offset,
                            const uint8_t *first_page, unsigned *bytes) {
    /* min(size, 84) whatever the offset, as createServerBuffer has it: the
     * last packet goes out whole, zeros past the end. */
    uint32_t payload = size > MB_PAYLOAD ? MB_PAYLOAD : size;
    seq.size = (uint8_t)payload;
    seq.ack = 0;
    int n = 0;
#define MB_BYTE(i) ((offset + (i)) >= size ? 0 : \
                    (offset + (i)) < MB_PAYLOAD && first_page ? \
                    first_page[offset + (i)] : data[offset + (i)])
    w[n] = mb_server_header(seq, slots);
    if (payload) w[n] |= (uint32_t)MB_BYTE(0) << 24;
    n++;
    for (uint32_t i = 1; i < payload; i += 4) {
        uint32_t word = 0;
        for (uint32_t j = 0; j < 4 && i + j < payload; j++)
            word |= (uint32_t)MB_BYTE(i + j) << (8 * j);
        w[n++] = word;
    }
#undef MB_BYTE
    *bytes = 3 + payload;
    return n;
}

/* The host's acknowledgement of a client's packet. */
static int mb_ack_buffer(uint32_t *w, MbHeader of, unsigned *bytes) {
    of.size = 0;
    of.ack = 1;
    w[0] = mb_server_header(of, 1);
    *bytes = 3;
    return 1;
}

/* The frame's work: every MB_PER_FRAME exchanges the caller's frame (which
 * waits for the vertical blank, draws, and says whether B stopped it);
 * between those, a pause, so the adapter's radio has turns of its own. */
typedef bool (*MbFrame)(int stage, uint32_t done, uint32_t total);
static MbFrame g_mb_frame;
static int g_mb_beat;
static int g_mb_stage;
static uint32_t g_mb_done, g_mb_total;

static bool mb_tick(void) {
    if (++g_mb_beat % MB_PER_FRAME)
        wl_wait_lines(228 / MB_PER_FRAME);
    else if (g_mb_frame(g_mb_stage, g_mb_done, g_mb_total))
        return false;
    return true;
}

typedef enum { MB_GO, MB_STOP, MB_FAIL } MbStep;

/* LinkWirelessMultiboot::exchangeAndValidate, around one of three sends:
 * nothing (a byte of nothing, as the loader expects), an acknowledgement of
 * the last good header, or a given packet. `want` decides which packet
 * from the client ends the step; `sticky` makes every packet looked at the
 * one acknowledged next (the name's step). */
typedef bool (*MbWant)(MbHeader h, const uint8_t *payload);
static MbHeader g_mb_last;

static MbStep mb_until(int send, const uint32_t *words, int n_words,
                       unsigned bytes, MbWant want, bool sticky) {
    int quiet = 0;
    for (;;) {
        if (!mb_tick()) return MB_STOP;
        uint32_t w[24] = { 0 };
        int n = 0;
        unsigned b = 1;
        if (send == 1) n = mb_ack_buffer(w, g_mb_last, &b);
        if (send == 2) {
            for (int i = 0; i < n_words; i++) w[i] = words[i];
            n = n_words;
            b = bytes;
        }
        if (!mb_exchange(w, n, b)) return MB_FAIL;
        for (int i = 0; i < g_mb_in_count; i++) {
            if (sticky) g_mb_last = g_mb_in[i];
            if (want(g_mb_in[i], g_mb_in_payload[i])) {
                g_mb_last = g_mb_in[i];
                return MB_GO;
            }
        }
        if (++quiet > MB_PATIENCE * MB_PER_FRAME) return MB_FAIL;
    }
}

static uint8_t g_mb_name[2][6];
static bool g_mb_named;

static bool mb_any(MbHeader h, const uint8_t *p) {
    (void)h; (void)p;
    return true;
}
static bool mb_starting(MbHeader h, const uint8_t *p) {
    (void)p;
    return h.n == 2 && h.state == MB_STARTING;
}
static bool mb_name_first(MbHeader h, const uint8_t *p) {
    if (!(h.n == 1 && h.phase == 0 && h.state == MB_COMMUNICATING)) return false;
    for (int i = 0; i < 6; i++) g_mb_name[0][i] = p[i];
    return true;
}
static bool mb_name_rest(MbHeader h, const uint8_t *p) {
    if (h.n == 1 && h.phase == 1 && h.state == MB_COMMUNICATING) {
        for (int i = 0; i < 6; i++) g_mb_name[1][i] = p[i];
        g_mb_named = true;
    }
    return h.state == MB_OFF;
}
static MbHeader g_mb_expect;
static bool mb_acked(MbHeader h, const uint8_t *p) {
    (void)p;
    return h.ack && mb_same(h, g_mb_expect);
}

/* What the loader says it is: "RFU-MB-DL", split over two packets. */
static bool mb_name_ok(void) {
    static const uint8_t kName[2][6] = {
        { 0x00, 0x00, 0x52, 0x46, 0x55, 0x2D },
        { 0x4D, 0x42, 0x2D, 0x44, 0x4C, 0x00 }
    };
    if (!g_mb_named) return false;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 6; j++)
            if (g_mb_name[i][j] != kName[i][j]) return false;
    return true;
}

/* MultiTransfer, for the one client: which packets are out and which have
 * come back acknowledged, `acked` the count of the first ones all done. */
typedef struct { uint32_t id; bool ack; bool on; } MbPending;

static MbStep mb_send_image(const uint8_t *image, uint32_t len) {
    /* The loader boots only an image whose bytes 4-15 say RFU-MBOOT (and
     * the Single-Pak copy reads them to know where it came from). */
    uint8_t first[MB_PAYLOAD];
    static const uint8_t kPatch[12] = { 0x52, 0x46, 0x55, 0x2D, 0x4D, 0x42,
                                        0x4F, 0x4F, 0x54, 0x00, 0x00, 0x00 };
    for (int i = 0; i < MB_PAYLOAD; i++)
        first[i] = (i >= 4 && i < 16) ? kPatch[i - 4] : image[i];

    MbPending pend[MB_INFLIGHT];
    for (int i = 0; i < MB_INFLIGHT; i++) pend[i].on = false;
    uint32_t acked = 0;            /* packets 0..acked-1 are across */
    uint32_t next = 0;             /* the one to send */
    uint32_t packets = (len + MB_PAYLOAD - 1) / MB_PAYLOAD;
    int quiet = 0;

    while (acked < packets) {
        g_mb_done = acked * MB_PAYLOAD / 4;
        if (!mb_tick()) return MB_STOP;
        /* Note it as out, unless it is already. */
        bool known = false;
        uint32_t top = 0;
        int out = 0;
        for (int i = 0; i < MB_INFLIGHT; i++)
            if (pend[i].on) {
                out++;
                if (pend[i].id == next) known = true;
                if (pend[i].id > top) top = pend[i].id;
            }
        if (!known && next >= acked && (out == 0 || next > top))
            for (int i = 0; i < MB_INFLIGHT; i++)
                if (!pend[i].on) {
                    pend[i].id = next;
                    pend[i].ack = false;
                    pend[i].on = true;
                    break;
                }
        uint32_t w[24];
        unsigned bytes;
        int n = mb_server_buffer(w, image, len, mb_sequence(next), 0xF,
                                 next * MB_PAYLOAD, next == 0 ? first : 0,
                                 &bytes);
        if (!mb_exchange(w, n, bytes)) return MB_FAIL;
        /* Acknowledgements: mark, and move `acked` past every packet done
         * with nothing older still out. */
        bool moved = false;
        for (int k = 0; k < g_mb_in_count; k++) {
            if (!g_mb_in[k].ack) continue;
            for (int i = 0; i < MB_INFLIGHT; i++)
                if (pend[i].on && mb_same(mb_sequence(pend[i].id), g_mb_in[k]))
                    pend[i].ack = true;
            int best = -1;
            for (int i = 0; i < MB_INFLIGHT; i++)
                if (pend[i].on && pend[i].ack &&
                    (best < 0 || pend[i].id > pend[best].id))
                    best = i;
            if (best < 0) continue;
            bool complete = true;
            for (int i = 0; i < MB_INFLIGHT; i++)
                if (pend[i].on && !pend[i].ack && pend[i].id < pend[best].id)
                    complete = false;
            if (complete) {
                acked = pend[best].id + 1;
                for (int i = 0; i < MB_INFLIGHT; i++)
                    if (pend[i].on && pend[i].ack) pend[i].on = false;
                moved = true;
            }
        }
        quiet = moved ? 0 : quiet + 1;
        if (quiet > MB_PATIENCE * MB_PER_FRAME) return MB_FAIL;
        /* Transfer::nextCursor: the next new one while there is room in
         * flight, otherwise the oldest still unacknowledged. */
        out = 0;
        top = 0;
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < MB_INFLIGHT; i++)
            if (pend[i].on) {
                out++;
                if (pend[i].id > top) top = pend[i].id;
                if (!pend[i].ack && pend[i].id < oldest) oldest = pend[i].id;
            }
        if (out > 0 && out < MB_INFLIGHT) next = top + 1;
        else next = oldest != 0xFFFFFFFFu ? oldest : acked;
        if (next >= packets) next = oldest != 0xFFFFFFFFu ? oldest : acked;
    }
    g_mb_done = g_mb_total;
    return MB_GO;
}

WlSendResult wireless_multiboot_send(const uint8_t *image, uint32_t len,
                                     MbFrame frame) {
    g_mb_frame = frame;
    g_mb_beat = 0;
    g_mb_stage = WL_SEND_WAITING;
    g_mb_done = 0;
    g_mb_total = len / 4;
    g_mb_named = false;
    MbHeader zero = { 0, 0, 0, 0, MB_OFF };
    g_mb_last = zero;
    MbStep step = MB_FAIL;
    uint32_t r[4];
    int n;

    if (!wl_start_tx(1) || !wl_open_room(WL_GAME_ID | MB_GAME_ID_FLAG))
        goto out;
    /* Nobody yet: the room stays open until a loader joins it, or B. */
    for (;;) {
        if (frame(g_mb_stage, 0, g_mb_total)) { step = MB_STOP; goto out; }
        n = wl_command(CMD_POLL_CONNS, 0, 0, r, 4);
        if (n < 0) goto out;
        if (n > 0) break;
    }
    g_mb_stage = WL_SEND_SENDING;
    /* The handshake, in LinkWirelessMultiboot::handshakeClient's order. */
    if ((step = mb_until(0, 0, 0, 1, mb_any, false)) != MB_GO) goto out;
    if ((step = mb_until(1, 0, 0, 0, mb_starting, false)) != MB_GO) goto out;
    if ((step = mb_until(1, 0, 0, 0, mb_name_first, false)) != MB_GO) goto out;
    if ((step = mb_until(1, 0, 0, 0, mb_name_rest, true)) != MB_GO) goto out;
    step = MB_FAIL;
    if (!mb_name_ok()) goto out;
    /* ...what the loader still had to say, until it has nothing. */
    for (int quiet = 0;; quiet++) {
        if (!mb_tick()) { step = MB_STOP; goto out; }
        if (!mb_exchange(0, 0, 1)) goto out;
        if (g_mb_in_count == 0) break;
        if (quiet > MB_PATIENCE * MB_PER_FRAME) goto out;
    }
    if (wl_command(CMD_END_HOST, 0, 0, r, 4) < 0) goto out;

    /* "ROM start", acknowledged; the image; the end, acknowledged; and
     * three last words of the session going off. */
    {
        static const uint8_t kStart[7] = { 0x00, 0x54, 0x00, 0x00, 0x00, 0x02, 0x00 };
        MbHeader seq = { 0, 0, 1, 0, MB_STARTING };
        uint32_t w[24];
        unsigned bytes;
        int nw = mb_server_buffer(w, kStart, 7, seq, 1, 0, 0, &bytes);
        g_mb_expect = seq;
        if ((step = mb_until(2, w, nw, bytes, mb_acked, false)) != MB_GO) goto out;
    }
    if ((step = mb_send_image(image, len)) != MB_GO) goto out;
    g_mb_stage = WL_SEND_FINISHING;
    {
        MbHeader seq = { 0, 0, 0, 0, MB_ENDING };
        uint32_t w[24];
        unsigned bytes;
        int nw = mb_server_buffer(w, 0, 0, seq, 1, 0, 0, &bytes);
        g_mb_expect = seq;
        if ((step = mb_until(2, w, nw, bytes, mb_acked, false)) != MB_GO) goto out;
        MbHeader off = { 0, 0, 1, 0, MB_OFF };
        nw = mb_server_buffer(w, 0, 0, off, 0xF, 0, 0, &bytes);
        for (int i = 0; i < MB_FINAL_OFFS; i++) {
            if (!mb_tick()) { step = MB_STOP; goto out; }
            if (!mb_exchange(w, nw, bytes)) { step = MB_FAIL; goto out; }
        }
    }
    step = MB_GO;
out:
    wl_command(CMD_BYE, 0, 0, 0, 0);
    return step == MB_GO ? WL_SEND_DONE
         : step == MB_STOP ? WL_SEND_STOPPED : WL_SEND_FAILED;
}
