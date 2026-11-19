/* Master of Arena: the hold-START menu of a network fight (versus rooms only).
 *
 * Holding START for a second during a fight opens a small see-through menu in
 * the bottom-left corner: RESUME, RESTART ROUND, CHANGE STAGE, RETURN TO
 * LOBBY. The fight keeps running; while the menu is open this pilot's AC gets
 * no input. A START tap or CIRCLE closes it. A quick START tap does nothing
 * else: the game's own pause (which froze both PCs) is gone from network
 * fights.
 *
 * RESTART ROUND and CHANGE STAGE are requests: the other pilot sees a strip
 * across the top ("<pilot> ASKS: ... HOLD START TO ANSWER") and answers with
 * ACCEPT or DECLINE at the top of their own menu, within 10 seconds (no
 * answer counts as declined). RETURN TO LOBBY needs no answer (press it
 * twice): both PCs go back to the room, as the Esc key does.
 *
 * How the two PCs stay in step: requests and answers ride in this pilot's
 * pad word as codes a real pad can't make (all four D-pad directions, L2 and
 * R2, and the code in the face buttons, L1 and R1; real input is cleaned of
 * opposite directions first), held for a few frames. Every PC sees both pad
 * words at the same frame of the match, in the link engine's packet wait
 * (moa_link_netplay.c). There the codes are taken out before the fight sees
 * them, and only an ACCEPT changes guest memory: it carries the action and
 * the stage, so no pending request lives in the simulation and rollback
 * stays safe. Each PC's picture (the strip, the menu, "waiting") is its own
 * host state.
 *
 * What an accepted restart does (read from the link engine, FDAT 208): the
 * game's own "no contest", which its pause has for SELECT: the end countdown
 * (match +0x10) goes straight to 30, CONTINUE?, with both player blocks
 * +0x08 |= 0x30 and nothing counted. CHANGE STAGE writes the new stage into
 * both player blocks first (moa_np_room_set_stage; player 2's block carries
 * it across the reload). Then CONTINUE? is answered for both: X between
 * rounds (the exe reloads the engine: a new round, the score kept), nothing
 * once the set is decided (the room takes both PCs back).
 */
#include "moa_netplay_menu.h"
#include "moa_netplay_room.h"
#include "moa_screen.h"

#include "mod_plugins.h"
#include "psx_netplay.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define NM_MATCH       0x80098E20u            /* link engine: match struct */
#define NM_END         (NM_MATCH + 0x10u)     /* end countdown: 0 fighting, 30 CONTINUE? */
#define NM_STEPS       (NM_MATCH + 0x12u)     /* steps since the round began (capped) */
#define NM_LINK_ERROR  0x80090F90u            /* link engine: the cable failed */
#define NM_REPLAY      0x8003EA92u            /* exe: replay flag */
#define NM_P1          0x80045198u            /* player blocks: +0 pad, +8 result flags */
#define NM_P2          0x80045538u
#define NM_RECV_PAD    (0x80043C70u + 932u + 2u)  /* the packet's remote pad */
#define NM_SEND_PAD    (0x80043C70u + 944u + 2u)  /* player 1's pad of the last frame */
#define NM_CONTINUE    30u
#define NM_RA_CONTINUE 0x800523F8u            /* the CONTINUE? check's own packet call returns here */
#define NM_MIN_STEPS   30u                    /* an accept counts once the round has run this long */

#define NM_HOLD_MS     1000u                  /* START held this long opens the menu */
#define NM_SEND_MS     150u                   /* a code is held this long */
#define NM_ANSWER_MS   10000u                 /* a request waits this long */
#define NM_NOTE_MS     3000u
#define NM_LEAVE_MS    3000u                  /* "press again to leave" lasts */

enum {
    B_START = 0x0008, B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
    B_L2 = 0x0100, B_R2 = 0x0200, B_L1 = 0x0400, B_R1 = 0x0800,
    B_CIRCLE = 0x2000, B_CROSS = 0x4000
};
#define B_DPAD (B_UP | B_RIGHT | B_DOWN | B_LEFT)
#define B_SIGN (B_DPAD | B_L2 | B_R2)         /* bits 0..9 of every code */

/* Code values 1..63: face buttons = bits 0-3, L1 / R1 = bits 4-5. */
enum { C_REQ_RESTART = 1, C_OK_RESTART = 2, C_DECLINE = 3, C_REQ_STAGE = 16, C_OK_STAGE = 32 };

int moa_link_netplay_engine_loaded(void);    /* moa_link_netplay.c */

static uint16_t code_word(int v)
{
    return (uint16_t)(B_SIGN | (v & 15) << 12 | ((v >> 4) & 3) << 10);
}
static int is_code(uint16_t w) { return (w & 0x03FFu) == B_SIGN; }
static int code_of(uint16_t w) { return ((w >> 12) & 15) | ((w >> 10) & 3) << 4; }
static int is_req(int c)
{
    return c == C_REQ_RESTART || (c >= C_REQ_STAGE && c < C_REQ_STAGE + moa_np_room_stage_count());
}
static int is_ok(int c)
{
    return c == C_OK_RESTART || (c >= C_OK_STAGE && c < C_OK_STAGE + moa_np_room_stage_count());
}

static uint64_t nm_now(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* ---- this PC's state (host only; the simulation never reads it) ------------ */
static struct {
    int open, page, cursor;            /* page 0: the menu, 1: the stage list */
    uint16_t prev;                     /* last frame's buttons (game layout) */
    uint16_t swallow;                  /* held when the menu closed: kept from the game until let go */
    int start_fired;
    uint64_t start_ms;
    int leave_armed;
    uint64_t leave_ms;
    int send;                          /* the code being sent */
    uint64_t send_until;
    int waiting;                       /* our request is out */
    uint64_t wait_ms;
    int incoming;                      /* the other pilot's request */
    uint64_t incoming_ms;
    int last_other;                    /* the other pilot's code last seen, for repeats */
    uint64_t last_other_ms;
    char note[64];
    uint64_t note_until;
    char drawn[512];                   /* what the overlay shows now ("" nothing) */
} M;

static int other_slot(void) { return psx_netplay_local_slot() == 1 ? 0 : 1; }

static void nm_note(const char *fmt, const char *who)
{
    snprintf(M.note, sizeof M.note, fmt, who);
    M.note_until = nm_now() + NM_NOTE_MS;
}

static int nm_fighting(void)
{
    return moa_link_netplay_engine_loaded() && psx_mod_read_half(NM_END) == 0 &&
           !psx_mod_read_byte(NM_LINK_ERROR) && !psx_mod_read_byte(NM_REPLAY);
}

/* ---- the simulation's side (both PCs, the same frame) ------------------------ */

/* A code from the other pilot (presentation only). Codes are held for a few
 * frames and rollback may show a frame twice: a repeat is the same request. */
static void nm_seen(int port, int c)
{
    if (port == psx_netplay_local_slot()) return;
    const uint64_t now = nm_now();
    if (c == M.last_other && now - M.last_other_ms < 1500u) { M.last_other_ms = now; return; }
    M.last_other = c;
    M.last_other_ms = now;
    if (is_req(c)) {
        if (M.open && M.page == 0 && !M.incoming) M.cursor += 2;  /* ACCEPT / DECLINE go on top */
        M.incoming = c;
        M.incoming_ms = now;
    } else if (c == C_DECLINE) {
        if (M.waiting) nm_note("%s DECLINED", moa_np_room_pilot(other_slot()));
        M.waiting = 0;
    } else if (is_ok(c)) {
        M.waiting = 0;
        M.incoming = 0;
    }
}

void moa_np_menu_pads(uint16_t *port1, uint16_t *port2)
{
    if (!moa_np_room_active() || !moa_link_netplay_engine_loaded()) return;
    const int fighting = psx_mod_read_half(NM_END) == 0 && !psx_mod_read_byte(NM_LINK_ERROR) &&
                         !psx_mod_read_byte(NM_REPLAY);
    const int settled = fighting && psx_mod_read_half(NM_STEPS) >= NM_MIN_STEPS;
    uint16_t *pad[2] = { port1, port2 };
    int act = 0;
    for (int k = 0; k < 2; k++) {
        const uint16_t w = *pad[k];
        if (is_code(w)) {
            const int c = code_of(w);
            nm_seen(k, c);
            if (settled && !act && is_ok(c)) act = c;
            *pad[k] = 0;                      /* the fight never sees a code */
        } else {
            *pad[k] = (uint16_t)(w & ~B_START);   /* no pause in network fights */
        }
    }
    if (!act) return;
    if (act >= C_OK_STAGE) {
        moa_np_room_set_stage(moa_np_room_stage_at(act - C_OK_STAGE, NULL));
        psx_mod_counter_add("moa.room.menu_change_stage", 1);
    } else {
        psx_mod_counter_add("moa.room.menu_restart", 1);
    }
    /* The game's own no contest: straight to CONTINUE?, nothing counted. */
    psx_mod_write_half(NM_END, (uint16_t)NM_CONTINUE);
    psx_mod_write_half(NM_P1 + 8u, (uint16_t)(psx_mod_read_half(NM_P1 + 8u) | 0x30u));
    psx_mod_write_half(NM_P2 + 8u, (uint16_t)(psx_mod_read_half(NM_P2 + 8u) | 0x30u));
}

void moa_np_menu_continue(uint32_t caller)
{
    if (!moa_np_room_active() || !moa_link_netplay_engine_loaded()) return;
    if (psx_mod_read_half(NM_END) != NM_CONTINUE || psx_mod_read_byte(NM_LINK_ERROR) ||
        psx_mod_read_byte(NM_REPLAY))
        return;
    /* The pads at CONTINUE?: player 2 presses nothing; player 1 nothing once
     * the set is decided, else X. While CONTINUE? waits the engine trades
     * pads twice a pass, in the frame update and then in the CONTINUE? check
     * (`caller`: where that packet call returns). A press counts only after
     * a release, so the first gives nothing and the check's own gives X. */
    psx_mod_write_half(NM_RECV_PAD, 0);
    const int press = caller == NM_RA_CONTINUE && !moa_np_room_set_over() && !psx_mod_read_half(NM_P1);
    psx_mod_write_half(NM_SEND_PAD, press ? (uint16_t)B_CROSS : 0);
}

/* ---- the menu (this PC's own pad, before it is sent) ------------------------- */

enum { R_ACCEPT, R_DECLINE, R_RESUME, R_RESTART, R_STAGE, R_LEAVE };

/* The main page's rows: ACCEPT / DECLINE on top while a request waits. */
static int nm_rows(int kind[6])
{
    int n = 0;
    if (M.incoming) { kind[n++] = R_ACCEPT; kind[n++] = R_DECLINE; }
    kind[n++] = R_RESUME; kind[n++] = R_RESTART; kind[n++] = R_STAGE; kind[n++] = R_LEAVE;
    return n;
}

static void nm_send(int code)
{
    M.send = code;
    M.send_until = nm_now() + NM_SEND_MS;
}

static void nm_close(uint16_t held)
{
    M.open = 0;
    M.page = 0;
    M.leave_armed = 0;
    M.swallow = held;
}

static int nm_stage_index(int id)
{
    for (int i = 0; i < moa_np_room_stage_count(); i++)
        if (moa_np_room_stage_at(i, NULL) == id) return i;
    return 0;
}

static void nm_pick(uint16_t held)
{
    const uint64_t now = nm_now();
    if (M.page == 1) {                                   /* a stage: the request */
        if (M.waiting) return;
        if (M.incoming == C_REQ_STAGE + M.cursor) {      /* the other pilot asked for the same */
            nm_send(C_OK_STAGE + M.cursor);
            M.incoming = 0;
        } else {
            nm_send(C_REQ_STAGE + M.cursor);
            M.waiting = 1;
            M.wait_ms = now;
        }
        nm_close(held);
        return;
    }
    int kind[6];
    const int n = nm_rows(kind);
    if (M.cursor < 0 || M.cursor >= n) return;
    switch (kind[M.cursor]) {
    case R_ACCEPT:
        nm_send(M.incoming == C_REQ_RESTART ? C_OK_RESTART : C_OK_STAGE + (M.incoming - C_REQ_STAGE));
        M.incoming = 0;
        nm_close(held);
        break;
    case R_DECLINE:
        nm_send(C_DECLINE);
        M.incoming = 0;
        nm_close(held);
        break;
    case R_RESUME:
        nm_close(held);
        break;
    case R_RESTART:
        if (M.waiting) break;
        if (M.incoming == C_REQ_RESTART) {               /* both want it: that's an accept */
            nm_send(C_OK_RESTART);
            M.incoming = 0;
        } else {
            nm_send(C_REQ_RESTART);
            M.waiting = 1;
            M.wait_ms = now;
        }
        nm_close(held);
        break;
    case R_STAGE:
        if (M.waiting) break;
        M.page = 1;
        M.cursor = nm_stage_index(moa_np_room_stage_now());
        break;
    case R_LEAVE:
        if (M.leave_armed && now - M.leave_ms < NM_LEAVE_MS) {
            psx_mod_counter_add("moa.room.menu_leave", 1);
            nm_close(held);
            moa_np_room_leave();
        } else {
            M.leave_armed = 1;
            M.leave_ms = now;
        }
        break;
    }
}

static void nm_draw(void)
{
    const uint64_t now = nm_now();
    char strip[96] = "";
    const char *other = moa_np_room_pilot(other_slot());
    if (M.incoming && !M.open) {
        const int left = (int)((NM_ANSWER_MS - (now - M.incoming_ms) + 999u) / 1000u);
        const char *what = "RESTART ROUND";
        if (M.incoming >= C_REQ_STAGE) moa_np_room_stage_at(M.incoming - C_REQ_STAGE, &what);
        snprintf(strip, sizeof strip, "%s ASKS: %s  -  HOLD START TO ANSWER  -  %d", other, what,
                 left < 0 ? 0 : left);
    } else if (M.waiting) {
        const int left = (int)((NM_ANSWER_MS - (now - M.wait_ms) + 999u) / 1000u);
        snprintf(strip, sizeof strip, "WAITING FOR %s  -  %d", other, left < 0 ? 0 : left);
    } else if (M.note[0] && now < M.note_until) {
        snprintf(strip, sizeof strip, "%s", M.note);
    }
    const char *rows[12];
    int dim[12] = {0}, count = 0, marked = -1;
    char title[80] = "", leave[32];
    if (M.open && M.page == 1) {
        snprintf(title, sizeof title, "CHANGE STAGE");
        count = moa_np_room_stage_count();
        if (count > 12) count = 12;
        for (int i = 0; i < count; i++) moa_np_room_stage_at(i, &rows[i]);
        marked = nm_stage_index(moa_np_room_stage_now());
    } else if (M.open) {
        if (M.incoming) {
            const char *what = "RESTART ROUND";
            if (M.incoming >= C_REQ_STAGE) moa_np_room_stage_at(M.incoming - C_REQ_STAGE, &what);
            snprintf(title, sizeof title, "%s ASKS: %s", other, what);
        } else {
            snprintf(title, sizeof title, "NETWORK MATCH");
        }
        int kind[6];
        count = nm_rows(kind);
        snprintf(leave, sizeof leave, "%s", M.leave_armed ? "PRESS AGAIN TO LEAVE" : "RETURN TO LOBBY");
        for (int i = 0; i < count; i++) {
            static const char *const names[] = { "ACCEPT", "DECLINE", "RESUME", "RESTART ROUND",
                                                 "CHANGE STAGE", "" };
            rows[i] = kind[i] == R_LEAVE ? leave : names[kind[i]];
            dim[i] = M.waiting && (kind[i] == R_RESTART || kind[i] == R_STAGE);
        }
    }
    const char *footer = M.page == 1 ? "CROSS ASKS FOR IT  -  CIRCLE GOES BACK" : "START OR CIRCLE CLOSES";
    int ww = 0, wh = 0;
    psx_mod_window_size(&ww, &wh);
    /* Only a new picture when something on it changed. */
    char key[sizeof M.drawn];
    int len = snprintf(key, sizeof key, "%dx%d|%s|%s|%d|%d|", ww, wh, strip, title, M.open ? M.cursor : -1,
                       marked);
    for (int i = 0; i < count && len > 0 && len < (int)sizeof key; i++)
        len += snprintf(key + len, sizeof key - (size_t)len, "%s%d,", rows[i], dim[i]);
    if (!strip[0] && !title[0]) key[0] = '\0';
    if (strcmp(key, M.drawn) == 0) return;
    snprintf(M.drawn, sizeof M.drawn, "%s", key);
    if (!key[0]) {
        psx_mod_set_overlay_image(NULL, 0, 0);
        return;
    }
    struct moa_screen_netmenu m;
    memset(&m, 0, sizeof m);
    m.strip = strip[0] ? strip : NULL;
    m.title = title[0] ? title : NULL;
    m.row = rows;
    m.dim = dim;
    m.count = count;
    m.cursor = M.cursor;
    m.marked = marked;
    m.footer = m.title ? footer : NULL;
    int w = 0, h = 0;
    const uint32_t *px = moa_screen_render_netmenu(ww, wh, &m, &w, &h);
    if (px) psx_mod_set_overlay_image(px, w, h);
}

static void nm_clear(void)
{
    const char keep_drawn = M.drawn[0];
    memset(&M, 0, sizeof M);
    if (keep_drawn) psx_mod_set_overlay_image(NULL, 0, 0);
}

static void nm_filter(PSXGameNetPad *p)
{
    if (!moa_np_room_active()) return;
    uint16_t b = (uint16_t)(~p->buttons & 0xFFFFu);       /* game layout: 1 = pressed */
    /* A real pad never holds opposite directions, so it can never form a code. */
    if ((b & B_UP) && (b & B_DOWN)) b &= (uint16_t)~(B_UP | B_DOWN);
    if ((b & B_LEFT) && (b & B_RIGHT)) b &= (uint16_t)~(B_LEFT | B_RIGHT);
    if (!moa_link_netplay_engine_loaded()) {            /* not in a fight: the pad as it is */
        if (M.open || M.drawn[0] || M.waiting || M.incoming) nm_clear();
        M.prev = b;
        p->buttons = (uint16_t)~b & 0xFFFFu;
        return;
    }
    const uint64_t now = nm_now();
    const int fighting = nm_fighting();
    if (!fighting) {                                    /* the round ended: so did all of this */
        if (M.open) nm_close(b);
        M.waiting = M.incoming = 0;
        M.send_until = 0;
        M.note_until = 0;
    }
    if (M.incoming && now - M.incoming_ms >= NM_ANSWER_MS) {
        M.incoming = 0;
        if (M.open && M.page == 0) M.cursor = 0;
    }
    if (M.waiting && now - M.wait_ms >= NM_ANSWER_MS + 600u) {
        M.waiting = 0;
        nm_note("NO ANSWER FROM %s", moa_np_room_pilot(other_slot()));
    }
    if (M.leave_armed && now - M.leave_ms >= NM_LEAVE_MS) M.leave_armed = 0;

    /* START never reaches the game. Held a second in a fight it opens the
     * menu (while still held); a tap closes it. */
    const int start = (b & B_START) != 0, was = (M.prev & B_START) != 0;
    const uint16_t prev = M.prev;
    M.prev = b;
    b &= (uint16_t)~B_START;
    if (start && !was) { M.start_ms = now; M.start_fired = 0; }
    if (start && !M.start_fired && fighting && now - M.start_ms >= NM_HOLD_MS) {
        M.start_fired = 1;
        if (!M.open) {
            M.open = 1;
            M.page = 0;
            M.cursor = 0;
            M.leave_armed = 0;
            psx_mod_counter_add("moa.room.menu_open", 1);
        }
    }
    if (!start && was && !M.start_fired && M.open) nm_close(b);

    if (M.open) {
        const uint16_t press = (uint16_t)(b & ~prev);
        if (M.page == 0) {
            int kind[6];
            const int n = nm_rows(kind);
            if (M.cursor >= n) M.cursor = n - 1;
            if (press & B_UP) { M.cursor = (M.cursor + n - 1) % n; M.leave_armed = 0; }
            if (press & B_DOWN) { M.cursor = (M.cursor + 1) % n; M.leave_armed = 0; }
            if (press & B_CIRCLE) nm_close(b);
            else if (press & B_CROSS) nm_pick(b);
        } else {
            const int n = moa_np_room_stage_count();
            if (press & B_UP) M.cursor = (M.cursor + n - 1) % n;
            if (press & B_DOWN) M.cursor = (M.cursor + 1) % n;
            if (press & B_CIRCLE) {
                int kind[6];
                const int rows = nm_rows(kind);
                M.page = 0;
                M.cursor = 0;
                for (int i = 0; i < rows; i++) if (kind[i] == R_STAGE) M.cursor = i;
            } else if (press & B_CROSS) {
                nm_pick(b);
            }
        }
    }
    /* What the fight gets: nothing while the menu is open, the buttons that
     * closed it only once they are let go, a code while one is sent. */
    M.swallow &= b;
    if (M.open) b = 0;
    else b &= (uint16_t)~M.swallow;
    const int sending = fighting && now < M.send_until;
    if (sending) b = code_word(M.send);
    if (M.open || sending) p->lx = p->ly = p->rx = p->ry = 0x80u;
    p->buttons = (uint16_t)~b & 0xFFFFu;
    nm_draw();
}

PSX_MOD_CONSTRUCTOR(moa_netplay_menu_register)
{
    psx_game_set_netplay_local_pad_filter(nm_filter);
}
