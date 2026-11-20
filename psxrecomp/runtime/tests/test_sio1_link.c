/* test_sio1_link.c — the serial port (SIO1) and its link cable.
 *
 * Two SIO1 devices joined by a cable, driven the way Sony's "sio:" driver in
 * Armored Core: Master of Arena drives them (SLUS_010.30 0x8002F26C async
 * read, 0x8002F570 sync write, IRQ handler 0x8002FD1C):
 *   receiver: CTRL = TXEN|RXEN|RTS|RX IRQ after 8 bytes; on the IRQ it reads
 *             two 32-bit words from DATA, toggles DTR, acknowledges.
 *   sender:   per byte waits STAT & 5 == 5, writes DATA; after 8 bytes waits
 *             for DSR to change (the receiver's DTR toggle).
 * Also: an unplugged port reads STAT 0x0185 (the title then offers split
 * screen), a plugged port reads the partner's DTR on DSR (title offers Link),
 * nothing is sent without CTS, RESET clears the port, and the shared-memory
 * cable carries the same exchange between two ends opened by name. */
#include "sio1.h"
#include "psx_link_session.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define DATA 0x1F801050u
#define STAT 0x1F801054u
#define MODE 0x1F801058u
#define CTRL 0x1F80105Au
#define BAUD 0x1F80105Eu

/* ── an in-memory cable ──────────────────────────────────────────────────── */

typedef struct {
    uint8_t  buf[2][64];
    unsigned head[2], tail[2];
    uint32_t lines[2];
    int      plugged[2];
} MemCable;

typedef struct { MemCable *c; int me; } MemEnd;

static int m_present(void *x) { MemEnd *e = x; return e->c->plugged[e->me ^ 1]; }
static uint32_t m_lines(void *x) { MemEnd *e = x; return e->c->lines[e->me ^ 1]; }
static void m_set(void *x, uint32_t l) { MemEnd *e = x; e->c->lines[e->me] = l; }
static int m_send(void *x, uint8_t b) {
    MemEnd *e = x; MemCable *c = e->c;
    if (c->head[e->me] - c->tail[e->me] >= 64) return 0;
    c->buf[e->me][c->head[e->me]++ % 64] = b; return 1;
}
static int m_recv(void *x, uint8_t *b) {
    MemEnd *e = x; MemCable *c = e->c; int o = e->me ^ 1;
    if (c->head[o] == c->tail[o]) return 0;
    *b = c->buf[o][c->tail[o]++ % 64]; return 1;
}

static int g_irq[2];
static void irq_a(void *x) { (void)x; g_irq[0]++; }
static void irq_b(void *x) { (void)x; g_irq[1]++; }

/* Run both consoles forward a little. */
static void run(Sio1Device *a, Sio1Device *b, uint32_t cycles)
{
    for (uint32_t t = 0; t < cycles; t += 64) {
        sio1_dev_advance(a, 64);
        sio1_dev_advance(b, 64);
    }
}

static void driver_setup(Sio1Device *d)
{
    sio1_dev_write(d, CTRL, 0x50, 2);          /* reset + ack */
    sio1_dev_write(d, MODE, 0x4E, 2);          /* x16, 8 bits, 1 stop */
    sio1_dev_write(d, BAUD, 1, 2);             /* the game's reload (2073600 bps) */
    sio1_dev_write(d, CTRL, 0x0305, 2);        /* TXEN | RXEN | RX IRQ after 8 */
}

/* Sender: one 8-byte packet with the chunk handshake. Returns 1 if every step
 * finished inside its wait budget (the driver's callback timeout). */
static int send_packet(Sio1Device *tx, Sio1Device *rx, const uint8_t *p)
{
    uint32_t dsr0 = sio1_dev_read(tx, STAT, 2) & 0x80u;
    for (int i = 0; i < 8; i++) {
        int tries = 0;
        while ((sio1_dev_read(tx, STAT, 2) & 5u) != 5u) {
            run(tx, rx, 128);
            if (++tries > 2000) return 0;
        }
        sio1_dev_write(tx, DATA, p[i], 1);
    }
    int tries = 0;
    while ((sio1_dev_read(tx, STAT, 2) & 0x80u) == dsr0) {
        run(tx, rx, 128);
        /* the receiver's IRQ handler runs when its request is up */
        if (rx->flags & 0x200u) {
            uint32_t w0 = sio1_dev_read(rx, DATA, 4), w1 = sio1_dev_read(rx, DATA, 4);
            uint8_t got[8];
            memcpy(got, &w0, 4); memcpy(got + 4, &w1, 4);
            CHECK(memcmp(got, p, 8) == 0);
            uint32_t c = sio1_dev_read(rx, CTRL, 2);
            sio1_dev_write(rx, CTRL, (c ^ 0x2u) | 0x10u, 2);   /* DTR toggle + ack */
        }
        if (++tries > 4000) return 0;
    }
    return 1;
}

static void test_mem_cable(void)
{
    MemCable cab; memset(&cab, 0, sizeof cab);
    MemEnd ea = { &cab, 0 }, eb = { &cab, 1 };
    Sio1Link la = { m_present, m_lines, m_set, m_send, m_recv, NULL, &ea };
    Sio1Link lb = { m_present, m_lines, m_set, m_send, m_recv, NULL, &eb };
    Sio1Device a, b;

    /* Only A plugged in: its inputs float high -> the "no cable" status. */
    cab.plugged[0] = 1;
    sio1_dev_init(&a, &la, irq_a, NULL);
    CHECK(sio1_dev_read(&a, STAT, 4) == 0x0185u);

    /* B arrives with DTR off: A's DSR drops, so the title offers Link. */
    cab.plugged[1] = 1;
    sio1_dev_init(&b, &lb, irq_b, NULL);
    run(&a, &b, 4096);
    CHECK((sio1_dev_read(&a, STAT, 4) & 0x180u) == 0);
    driver_setup(&a);
    driver_setup(&b);

    /* No CTS (B's RTS off): A's byte must stay put. */
    sio1_dev_write(&a, DATA, 0x55, 1);
    run(&a, &b, 20000);
    CHECK((sio1_dev_read(&a, STAT, 2) & 1u) == 0);       /* buffer still full */
    CHECK((sio1_dev_read(&b, STAT, 2) & 2u) == 0);       /* nothing arrived */
    sio1_dev_write(&a, CTRL, 0x50, 2);                   /* reset drops it */
    driver_setup(&a);
    CHECK((sio1_dev_read(&a, STAT, 2) & 5u) == 5u);

    /* B posts an async read (RTS + RX IRQ), A sends a packet. */
    sio1_dev_write(&b, CTRL, 0x0305 | 0x20 | 0x800, 2);
    run(&a, &b, 4096);
    CHECK(sio1_dev_read(&a, STAT, 2) & 0x100u);          /* A sees CTS */
    const uint8_t pkt[8] = { 'R', 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 'C' };
    g_irq[1] = 0;
    CHECK(send_packet(&a, &b, pkt));
    CHECK(g_irq[1] == 1);

    /* A second packet the other way. */
    sio1_dev_write(&a, CTRL, 0x0305 | 0x20 | 0x800, 2);
    const uint8_t pkt2[8] = { 'M', 1, 2, 3, 4, 5, 6, 7 };
    CHECK(send_packet(&b, &a, pkt2));

    /* The partner leaves: inputs float high again. */
    cab.plugged[1] = 0;
    run(&a, &b, 4096);
    CHECK((sio1_dev_read(&a, STAT, 2) & 0x180u) == 0x180u);
}

static void test_shm_cable(void)
{
    char name[32];
    snprintf(name, sizeof name, "t%ld", (long)(rand() % 100000));
    Sio1ShmCable *c1 = sio1_shm_cable_open(name, 1);
    CHECK(c1 != NULL);
    if (!c1) return;
    Sio1Device a, b;
    sio1_dev_init(&a, sio1_shm_cable_link(c1), irq_a, NULL);
    CHECK(sio1_dev_read(&a, STAT, 4) == 0x0185u);        /* nobody on the far end */

    Sio1ShmCable *c2 = sio1_shm_cable_open(name, 2);
    CHECK(c2 != NULL);
    if (!c2) { sio1_shm_cable_close(c1); return; }
    sio1_dev_init(&b, sio1_shm_cable_link(c2), irq_b, NULL);
    run(&a, &b, 4096);
    CHECK((sio1_dev_read(&a, STAT, 2) & 0x180u) == 0);
    driver_setup(&a);
    driver_setup(&b);
    sio1_dev_write(&b, CTRL, 0x0305 | 0x20 | 0x800, 2);
    run(&a, &b, 4096);
    const uint8_t pkt[8] = { 'R', 9, 8, 7, 6, 5, 4, 'C' };
    CHECK(send_packet(&a, &b, pkt));

    sio1_shm_cable_close(c2);
    b.link = NULL;                         /* that console is gone */
    sio1_dev_advance(&a, 4096);
    CHECK((sio1_dev_read(&a, STAT, 2) & 0x180u) == 0x180u);
    sio1_shm_cable_close(c1);

    CHECK(sio1_shm_cable_open("bad/name", 1) == NULL);
    CHECK(sio1_shm_cable_open(name, 3) == NULL);
}

static void test_no_cable_port(void)
{
    /* The runtime's port without PSX_LINK_CABLE: exactly the old behaviour. */
    CHECK(!sio1_cable_attached());
    sio1_write(CTRL, 0x0025, 2);
    CHECK(sio1_read(CTRL, 2) == 0);
    CHECK(sio1_cycles_to_irq(0xFFFFFFFFu) == UINT32_MAX);
}

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void test_link_session_env(void)
{
    /* Which end of the cable a session is, from the environment alone. */
    set_env("PSX_LINK_CABLE", NULL);
    set_env("PSX_LINK_SIDE", NULL);
    CHECK(psx_link_session_from_env() == 0);
    set_env("PSX_LINK_SIDE", "2");
    CHECK(psx_link_session_from_env() == 0);      /* no cable name */
    set_env("PSX_LINK_CABLE", "t");
    CHECK(psx_link_session_from_env() == 2);
    CHECK(psx_link_is_second_session());
    set_env("PSX_LINK_SIDE", "1");
    CHECK(psx_link_session_from_env() == 1);
    CHECK(!psx_link_is_second_session());
    set_env("PSX_LINK_SIDE", "3");
    CHECK(psx_link_session_from_env() == 0);
    set_env("PSX_LINK_SIDE", "12");
    CHECK(psx_link_session_from_env() == 0);
    set_env("PSX_LINK_CABLE", NULL);
    set_env("PSX_LINK_SIDE", NULL);
}

static void test_seats(void)
{
    /* Both ends see one set of seats; a write by either is seen by both. */
    char name[32];
    snprintf(name, sizeof name, "seat-test-%u", (unsigned)rand());
    Sio1ShmCable *a = sio1_shm_cable_open(name, 1);
    Sio1ShmCable *b = sio1_shm_cable_open(name, 2);
    CHECK(a && b);
    if (!a || !b) return;
    Sio1Seats s;
    sio1_shm_seat_read(b, &s);
    CHECK(s.focus == 0 && s.prompt == 0 && !s.claim[0][0] && !s.claim[1][0]);
    const uint32_t g0 = sio1_shm_seat_generation(a);
    memset(&s, 0, sizeof s);
    s.focus = 2; s.prompt = 1;
    snprintf(s.claim[0], sizeof s.claim[0], "keyboard");
    sio1_shm_seat_write(a, &s, SIO1_SEAT_FOCUS | SIO1_SEAT_PROMPT | SIO1_SEAT_CLAIM1);
    CHECK(sio1_shm_seat_generation(b) != g0);
    Sio1Seats r;
    sio1_shm_seat_read(b, &r);
    CHECK(r.focus == 2 && r.prompt == 1);
    CHECK(strcmp(r.claim[0], "keyboard") == 0 && r.claim[1][0] == 0);
    /* Only the named fields change; a long id is cut, never overrun. */
    memset(s.claim[1], 'x', sizeof s.claim[1]);
    s.claim[1][SIO1_SEAT_ID - 1] = 0;
    s.focus = 1;
    sio1_shm_seat_write(b, &s, SIO1_SEAT_CLAIM2);
    sio1_shm_seat_read(a, &r);
    CHECK(r.focus == 2 && strlen(r.claim[1]) == SIO1_SEAT_ID - 1);
    const uint32_t g1 = sio1_shm_seat_generation(a);
    sio1_shm_seat_read(a, &r);
    CHECK(sio1_shm_seat_generation(a) == g1);   /* reading is not a change */
    sio1_shm_cable_close(b);
    sio1_shm_cable_close(a);
}

int main(void)
{
    test_no_cable_port();
    test_link_session_env();
    test_mem_cable();
    test_shm_cable();
    test_seats();
    if (g_fail) return 1;
    printf("sio1_link_test: ok\n");
    return 0;
}
