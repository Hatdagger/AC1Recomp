/* sio1.h — the serial port (SIO1, 0x1F801050..0x1F80105F): the PS1 link-cable
 * port, and the "cable" that joins two runtime sessions on one PC.
 *
 * Register model (psx-spx "Serial Port (SIO)"):
 *   0x1F801050 SIO1_DATA  write: TX byte. read: pops 1/2/4 bytes of the RX FIFO
 *                         for an 8/16/32-bit read (Sony's sio driver reads an
 *                         8-byte chunk as two 32-bit words).
 *   0x1F801054 SIO1_STAT  0 TX ready (buffer free), 1 RX FIFO not empty,
 *                         2 TX idle (buffer and shifter empty), 3-5 RX errors,
 *                         7 DSR in, 8 CTS in, 9 IRQ request.
 *   0x1F801058 SIO1_MODE  0-1 baud factor (1/16/64), 2-3 char length 5..8,
 *                         4 parity enable, 6-7 stop bits.
 *   0x1F80105A SIO1_CTRL  0 TX enable, 1 DTR out, 2 RX enable, 4 ACK (strobe),
 *                         5 RTS out, 6 RESET (strobe), 8-9 RX IRQ after
 *                         1/2/4/8 bytes, 10 TX IRQ, 11 RX IRQ, 12 DSR IRQ.
 *   0x1F80105E SIO1_BAUD  reload value; one bit lasts reload * factor cycles.
 *
 * The wiring is a real link cable's: my TX -> partner RX, my DTR -> partner
 * DSR, my RTS -> partner CTS. A transmission starts only while CTS is on.
 * An unconnected input reads 1 (DSR and CTS on), which with the transmitter
 * idle is STAT 0x0185 — the value Master of Arena used to pin.
 *
 * Without a cable (PSX_LINK_CABLE unset) the port behaves exactly as before
 * this module existed: STAT reads PSX_SIO1_IDLE_STAT when a title defines it
 * (0 otherwise), every other register reads 0 and writes are ignored. Nothing
 * is kept, so savestates, rollback and netplay digests are unaffected.
 *
 * With a cable, two sessions of the same build share one named cable:
 *   PSX_LINK_CABLE=<name>   cable name (same for both sessions)
 *   PSX_LINK_SIDE=1|2       this session's end of the cable
 * The two sessions are separate consoles with separate clocks, exactly as two
 * PS1s on a cable: bytes cross when they are shifted out, line levels are
 * sampled when the port is polled, and the game's own handshakes and timeouts
 * keep the pair in step. */
#ifndef PSX_SIO1_H
#define PSX_SIO1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Line bits on the cable (each side's outputs). */
#define SIO1_LINE_DTR 0x1u
#define SIO1_LINE_RTS 0x2u

/* One end of a cable, as seen by a device. `present` is 0 while nobody is on
 * the other end (inputs then read as unconnected). */
typedef struct Sio1Link {
    int      (*present)(void *ctx);
    uint32_t (*partner_lines)(void *ctx);         /* SIO1_LINE_* */
    void     (*set_lines)(void *ctx, uint32_t lines);
    int      (*send)(void *ctx, uint8_t byte);    /* 1 = on the wire, 0 = full */
    int      (*recv)(void *ctx, uint8_t *byte);   /* 1 = a byte arrived */
    void     (*poll)(void *ctx);                  /* liveness upkeep; may be NULL */
    void     *ctx;
} Sio1Link;

#define SIO1_RX_FIFO 8

typedef struct Sio1Device {
    const Sio1Link *link;
    void (*raise_irq)(void *ctx);
    void *irq_ctx;

    uint16_t ctrl, mode, baud;
    uint16_t flags;              /* STAT bits 3-5 and 9 */

    uint8_t  rx[SIO1_RX_FIFO];
    uint8_t  rx_head, rx_count;

    uint8_t  tx_buf;
    uint8_t  tx_buf_full;
    uint8_t  tx_shift;
    uint8_t  tx_shift_active;
    uint8_t  tx_shift_done;      /* shifted out, waiting for room on the wire */
    uint32_t tx_shift_remaining; /* cycles until the shifted byte is on the wire */

    uint32_t in_lines;           /* partner lines, as last sampled */
    uint32_t out_lines;          /* published */
    uint32_t poll_remaining;
} Sio1Device;

/* Device API (one device per console; tests build two). */
void     sio1_dev_init(Sio1Device *d, const Sio1Link *link,
                       void (*raise_irq)(void *ctx), void *irq_ctx);
uint32_t sio1_dev_read(Sio1Device *d, uint32_t addr, int width);
void     sio1_dev_write(Sio1Device *d, uint32_t addr, uint32_t value, int width);
void     sio1_dev_advance(Sio1Device *d, uint32_t cycles);
/* Cycles until the device next needs servicing (byte done, or a cable poll
 * while it is waiting on the partner). UINT32_MAX when nothing is pending. */
uint32_t sio1_dev_cycles_to_event(const Sio1Device *d);
uint32_t sio1_dev_stat(Sio1Device *d);

/* Runtime's port (lazily attached from the environment on first use). */
uint32_t sio1_read(uint32_t addr, int width);
void     sio1_write(uint32_t addr, uint32_t value, int width);
void     sio1_advance(uint32_t cycles);
uint32_t sio1_cycles_to_irq(uint32_t i_mask);
int      sio1_cable_attached(void);   /* 1 when PSX_LINK_CABLE is in use */
/* This process's end of a link cable from the environment: 1 or 2 when
 * PSX_LINK_CABLE and PSX_LINK_SIDE name a valid end, else 0. Reads the
 * environment only (never opens the cable), so it is safe at startup. Session
 * 2 is the second console on the PC and keeps its own copies of the files a
 * session writes (settings, game options, savestates, run reports); session 1
 * uses the player's usual ones. */
int      psx_link_session(void);
void     sio1_shutdown(void);         /* leave the cable (also atexit) */

/* Shared-memory cable (sio1_cable.c). Returns NULL on failure. */
typedef struct Sio1ShmCable Sio1ShmCable;
Sio1ShmCable  *sio1_shm_cable_open(const char *name, int side /* 1 or 2 */);
const Sio1Link *sio1_shm_cable_link(Sio1ShmCable *c);
void           sio1_shm_cable_close(Sio1ShmCable *c);
void           sio1_shm_cable_beat(Sio1ShmCable *c);   /* keep this end alive */
int            sio1_shm_cable_partner_present(Sio1ShmCable *c);

/* ── Seats ───────────────────────────────────────────────────────────────
 * Who plays at which console, shared by the two sessions of a cable (Link
 * Versus on one PC). Not part of the console: the game never sees it.
 *   focus   the side whose window was clicked last (0 = none yet)
 *   prompt  the side showing "press any button to take this screen" (0 = none)
 *   claim   each side's controller, as an id string the frontend makes
 *           ("keyboard", or one for a game controller); "" = none chosen */
#define SIO1_SEAT_ID 64
typedef struct Sio1Seats {
    int      focus, prompt;
    char     claim[2][SIO1_SEAT_ID];
    uint32_t generation;               /* changes on every write */
} Sio1Seats;
enum { SIO1_SEAT_FOCUS = 1u, SIO1_SEAT_PROMPT = 2u,
       SIO1_SEAT_CLAIM1 = 4u, SIO1_SEAT_CLAIM2 = 8u };
uint32_t sio1_shm_seat_generation(Sio1ShmCable *c);
void     sio1_shm_seat_read(Sio1ShmCable *c, Sio1Seats *out);
void     sio1_shm_seat_write(Sio1ShmCable *c, const Sio1Seats *in, unsigned what);

/* The runtime's cable (attached from the environment as sio1_cable_attached
 * does). All are no-ops / zero without a cable. */
uint32_t sio1_seat_generation(void);
void     sio1_seat_read(Sio1Seats *out);
void     sio1_seat_write(const Sio1Seats *in, unsigned what);
int      sio1_partner_present(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SIO1_H */
