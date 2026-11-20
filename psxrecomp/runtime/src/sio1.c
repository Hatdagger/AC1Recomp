/* sio1.c — the serial port (SIO1) and its link cable. See sio1.h. */
#include "sio1.h"

#include <stdlib.h>
#include <string.h>

extern void psx_irq_raise(uint32_t bit, uint32_t detail);

#define IRQ_BIT_SIO1 8u

#define STAT_TX_READY   0x0001u
#define STAT_RX_READY   0x0002u
#define STAT_TX_IDLE    0x0004u
#define STAT_ERRORS     0x0038u
#define STAT_DSR        0x0080u
#define STAT_CTS        0x0100u
#define STAT_IRQ        0x0200u

#define CTRL_TXEN       0x0001u
#define CTRL_DTR        0x0002u
#define CTRL_RXEN       0x0004u
#define CTRL_ACK        0x0010u
#define CTRL_RTS        0x0020u
#define CTRL_RESET      0x0040u
#define CTRL_TX_IRQ     0x0400u
#define CTRL_RX_IRQ     0x0800u
#define CTRL_DSR_IRQ    0x1000u

/* How often (guest cycles) a port that is waiting on its partner looks at the
 * cable. Well under one 8-byte packet at the slowest rate a game would pick,
 * and small enough that a wait loop is never skipped past a byte. */
#define SIO1_POLL_CYCLES 1024u

/* ── device ──────────────────────────────────────────────────────────────── */

static int link_present(const Sio1Device *d)
{
    return d->link && d->link->present(d->link->ctx);
}

/* Cycles one character takes on the wire: start bit, 5..8 data bits, optional
 * parity, 1 or 2 stop bits; one bit lasts reload * factor cycles. */
static uint32_t char_cycles(const Sio1Device *d)
{
    static const uint32_t factor[4] = { 1u, 1u, 16u, 64u };
    uint32_t bits = 1u + 5u + ((d->mode >> 2) & 3u)
                  + ((d->mode >> 4) & 1u)
                  + ((((d->mode >> 6) & 3u) == 3u) ? 2u : 1u);
    uint32_t reload = d->baud ? d->baud : 1u;
    return bits * reload * factor[d->mode & 3u];
}

static uint32_t rx_threshold(const Sio1Device *d)
{
    static const uint32_t n[4] = { 1u, 2u, 4u, 8u };
    return n[(d->ctrl >> 8) & 3u];
}

uint32_t sio1_dev_stat(Sio1Device *d)
{
    uint32_t s = d->flags;
    if (!d->tx_buf_full) s |= STAT_TX_READY;
    if (!d->tx_buf_full && !d->tx_shift_active) s |= STAT_TX_IDLE;
    if (d->rx_count) s |= STAT_RX_READY;
    if (d->in_lines & SIO1_LINE_DTR) s |= STAT_DSR;
    if (d->in_lines & SIO1_LINE_RTS) s |= STAT_CTS;
    return s;
}

/* Level-sensitive request sources; the request latches in STAT bit 9 until
 * the program acknowledges it (CTRL bit 4), and is only raised again by a
 * source that still (or newly) holds after that. */
static void update_irq(Sio1Device *d)
{
    if (d->flags & STAT_IRQ) return;
    uint32_t s = sio1_dev_stat(d);
    int req = 0;
    if ((d->ctrl & CTRL_RX_IRQ) && d->rx_count >= rx_threshold(d)) req = 1;
    if ((d->ctrl & CTRL_TX_IRQ) && (s & (STAT_TX_READY | STAT_TX_IDLE))) req = 1;
    if ((d->ctrl & CTRL_DSR_IRQ) && (s & STAT_DSR)) req = 1;
    if (!req) return;
    d->flags |= STAT_IRQ;
    if (d->raise_irq) d->raise_irq(d->irq_ctx);
}

static void publish_lines(Sio1Device *d)
{
    uint32_t out = 0;
    if (d->ctrl & CTRL_DTR) out |= SIO1_LINE_DTR;
    if (d->ctrl & CTRL_RTS) out |= SIO1_LINE_RTS;
    if (out != d->out_lines) {
        d->out_lines = out;
        if (d->link) d->link->set_lines(d->link->ctx, out);
    }
}

/* Start shifting the buffered byte out if the transmitter may send. */
static void tx_try_start(Sio1Device *d)
{
    if (d->tx_shift_active || !d->tx_buf_full) return;
    if (!(d->ctrl & CTRL_TXEN)) return;
    if (!(d->in_lines & SIO1_LINE_RTS)) return;   /* wait for CTS */
    d->tx_shift = d->tx_buf;
    d->tx_buf_full = 0;
    d->tx_shift_active = 1;
    d->tx_shift_done = 0;
    d->tx_shift_remaining = char_cycles(d);
}

/* Sample the cable: partner lines, then arriving bytes (only while the
 * receiver is enabled and the FIFO has room; the rest stays on the wire). */
static void cable_poll(Sio1Device *d)
{
    if (!d->link) return;
    if (d->link->poll) d->link->poll(d->link->ctx);
    d->in_lines = link_present(d)
                ? d->link->partner_lines(d->link->ctx)
                : (SIO1_LINE_DTR | SIO1_LINE_RTS);   /* unconnected: pulled up */
    if (d->ctrl & CTRL_RXEN) {
        uint8_t b;
        while (d->rx_count < SIO1_RX_FIFO && d->link->recv(d->link->ctx, &b)) {
            d->rx[(d->rx_head + d->rx_count) % SIO1_RX_FIFO] = b;
            d->rx_count++;
        }
    }
    if (d->tx_shift_active && d->tx_shift_done) {
        if (d->link->send(d->link->ctx, d->tx_shift)) {
            d->tx_shift_active = 0;
            d->tx_shift_done = 0;
        }
    }
    tx_try_start(d);
    update_irq(d);
}

static void dev_reset_regs(Sio1Device *d)
{
    d->ctrl = 0;
    d->mode = 0;
    d->baud = 0;
    d->flags = 0;
    d->rx_head = d->rx_count = 0;
    d->tx_buf_full = 0;
    d->tx_shift_active = 0;
    d->tx_shift_done = 0;
    d->tx_shift_remaining = 0;
}

void sio1_dev_init(Sio1Device *d, const Sio1Link *link,
                   void (*raise_irq)(void *ctx), void *irq_ctx)
{
    memset(d, 0, sizeof(*d));
    d->link = link;
    d->raise_irq = raise_irq;
    d->irq_ctx = irq_ctx;
    d->in_lines = SIO1_LINE_DTR | SIO1_LINE_RTS;
    d->out_lines = 0;
    if (link) link->set_lines(link->ctx, 0);
    d->poll_remaining = SIO1_POLL_CYCLES;
    cable_poll(d);
}

static uint8_t rx_pop(Sio1Device *d)
{
    if (!d->rx_count) return 0;
    uint8_t b = d->rx[d->rx_head];
    d->rx_head = (uint8_t)((d->rx_head + 1u) % SIO1_RX_FIFO);
    d->rx_count--;
    return b;
}

uint32_t sio1_dev_read(Sio1Device *d, uint32_t addr, int width)
{
    cable_poll(d);
    uint32_t reg = addr & ~1u;
    uint32_t v = 0;
    switch (addr & 0xFu) {
    case 0x0: case 0x1: case 0x2: case 0x3: {      /* DATA: pop 1/2/4 bytes */
        int n = width >= 4 ? 4 : (width == 2 ? 2 : 1);
        for (int i = 0; i < n; i++) v |= (uint32_t)rx_pop(d) << (8 * i);
        update_irq(d);
        cable_poll(d);                             /* room for more */
        return v;
    }
    case 0x4: case 0x5: case 0x6: case 0x7:
        v = sio1_dev_stat(d);
        return v >> (8u * (addr & 3u));
    default:
        break;
    }
    switch (reg & 0xFu) {
    case 0x8: v = d->mode | ((uint32_t)d->ctrl << 16); break;
    case 0xA: v = d->ctrl; break;
    case 0xC: v = 0;       break;                  /* MISC */
    case 0xE: v = d->baud; break;
    }
    if (width >= 4 || (addr & 1u) == 0) return v;
    return v >> 8;
}

static void write_ctrl(Sio1Device *d, uint32_t value)
{
    if (value & CTRL_RESET) {
        dev_reset_regs(d);
    } else {
        if (value & CTRL_ACK) d->flags &= (uint16_t)~(STAT_ERRORS | STAT_IRQ);
        d->ctrl = (uint16_t)(value & ~(CTRL_ACK | CTRL_RESET));
    }
    publish_lines(d);
    cable_poll(d);
}

void sio1_dev_write(Sio1Device *d, uint32_t addr, uint32_t value, int width)
{
    switch (addr & 0xFu) {
    case 0x0: case 0x1: case 0x2: case 0x3:        /* DATA */
        if (!d->tx_buf_full) {
            d->tx_buf = (uint8_t)value;
            d->tx_buf_full = 1;
        } else {
            d->tx_buf = (uint8_t)value;            /* overwrites, as on hw */
        }
        cable_poll(d);
        return;
    case 0x4: case 0x5: case 0x6: case 0x7:        /* STAT: read-only */
        return;
    case 0x8:
        if (width >= 4) {
            d->mode = (uint16_t)value;
            write_ctrl(d, value >> 16);
        } else if (width == 2) {
            d->mode = (uint16_t)value;
        } else {
            d->mode = (uint16_t)((d->mode & 0xFF00u) | (value & 0xFFu));
        }
        return;
    case 0x9:
        d->mode = (uint16_t)((d->mode & 0x00FFu) | ((value & 0xFFu) << 8));
        return;
    case 0xA:
        if (width == 1) value = (d->ctrl & 0xFF00u) | (value & 0xFFu);
        write_ctrl(d, value & 0xFFFFu);
        return;
    case 0xB:
        write_ctrl(d, (d->ctrl & 0x00FFu) | ((value & 0xFFu) << 8));
        return;
    case 0xC: case 0xD:                            /* MISC */
        return;
    case 0xE:
        if (width == 1) value = (d->baud & 0xFF00u) | (value & 0xFFu);
        d->baud = (uint16_t)value;
        return;
    case 0xF:
        d->baud = (uint16_t)((d->baud & 0x00FFu) | ((value & 0xFFu) << 8));
        return;
    }
}

void sio1_dev_advance(Sio1Device *d, uint32_t cycles)
{
    if (!d->link) return;
    int need_poll = 0;
    if (d->tx_shift_active && !d->tx_shift_done) {
        if (cycles >= d->tx_shift_remaining) {
            d->tx_shift_remaining = 0;
            d->tx_shift_done = 1;
            need_poll = 1;
        } else {
            d->tx_shift_remaining -= cycles;
        }
    }
    if (cycles >= d->poll_remaining) {
        d->poll_remaining = SIO1_POLL_CYCLES;
        need_poll = 1;
    } else {
        d->poll_remaining -= cycles;
    }
    if (need_poll) cable_poll(d);
}

uint32_t sio1_dev_cycles_to_event(const Sio1Device *d)
{
    if (!d->link) return UINT32_MAX;
    uint32_t best = UINT32_MAX;
    if (d->tx_shift_active && !d->tx_shift_done && d->tx_shift_remaining < best)
        best = d->tx_shift_remaining;
    /* Waiting on the partner: a byte to receive (receiver armed), CTS to send,
     * a byte still to put on the wire, or a DSR level to watch. */
    int waiting = (d->ctrl & (CTRL_RXEN | CTRL_RTS)) ||
                  d->tx_buf_full || d->tx_shift_done ||
                  (d->ctrl & CTRL_DSR_IRQ);
    if (waiting && d->poll_remaining < best) best = d->poll_remaining;
    if (best == 0) best = 1;
    return best;
}

/* ── the runtime's port ──────────────────────────────────────────────────── */

static Sio1Device    s_port;
static Sio1ShmCable *s_cable;
static int           s_state;   /* 0 = not looked yet, 1 = no cable, 2 = cable */

static void port_raise_irq(void *ctx)
{
    (void)ctx;
    psx_irq_raise(IRQ_BIT_SIO1, 0);
}

static void port_attach(void)
{
    if (s_state) return;
    s_state = 1;
    const char *name = getenv("PSX_LINK_CABLE");
    const char *side = getenv("PSX_LINK_SIDE");
    if (!name || !name[0] || !side) return;
    int n = atoi(side);
    if (n != 1 && n != 2) return;
    s_cable = sio1_shm_cable_open(name, n);
    if (!s_cable) return;
    sio1_dev_init(&s_port, sio1_shm_cable_link(s_cable), port_raise_irq, NULL);
    s_state = 2;
    atexit(sio1_shutdown);
}

int sio1_cable_attached(void)
{
    port_attach();
    return s_state == 2;
}

void sio1_shutdown(void)
{
    if (s_state != 2) return;
    s_state = 1;
    s_port.link = NULL;
    sio1_shm_cable_close(s_cable);
    s_cable = NULL;
}

uint32_t sio1_read(uint32_t addr, int width)
{
    port_attach();
    if (s_state == 2) return sio1_dev_read(&s_port, addr, width);
#if defined(PSX_SIO1_IDLE_STAT)
    /* No cable: the unconnected port's status word (inputs pulled up,
     * transmitter idle), constant so it is deterministic for netplay and
     * savestates. */
    if ((addr & ~3u) == 0x1F801054u)
        return (uint32_t)(PSX_SIO1_IDLE_STAT) >> (8u * (addr & 3u));
#endif
    return 0;
}

void sio1_write(uint32_t addr, uint32_t value, int width)
{
    port_attach();
    if (s_state == 2) sio1_dev_write(&s_port, addr, value, width);
}

void sio1_advance(uint32_t cycles)
{
    if (s_state == 2) sio1_dev_advance(&s_port, cycles);
}

uint32_t sio1_cycles_to_irq(uint32_t i_mask)
{
    (void)i_mask;   /* lines and bytes must be sampled even while masked */
    if (s_state != 2) return UINT32_MAX;
    return sio1_dev_cycles_to_event(&s_port);
}
