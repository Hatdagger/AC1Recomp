/* sio1_cable.c — a link cable between two runtime sessions on one machine,
 * in named shared memory. See sio1.h.
 *
 * Layout: each side owns one byte ring (it writes `head`, the other side
 * reads and writes `tail`) and one line word (DTR/RTS). A side marks itself
 * attached and refreshes a heartbeat from the machine's monotonic clock; a
 * partner whose heartbeat is older than SIO1_CABLE_STALE_MS (it crashed or was
 * killed) counts as unplugged. A frozen but living session (a window being
 * dragged) stays plugged in: its silence is the game's own business, and the
 * game's own link timeout handles it as on hardware. */
#include "sio1.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#endif

#define SIO1_CABLE_MAGIC    0x4B4E4C31u   /* "1LNK" */
#define SIO1_CABLE_BUSY     0x59535542u   /* being set up */
#define SIO1_CABLE_VERSION  2u           /* 2: seats */
#define SIO1_CABLE_RING     4096u         /* power of two */
#define SIO1_CABLE_STALE_MS 15000u        /* well past every game timeout */
#define SIO1_HEARTBEAT_MS   250u

typedef struct {
    uint32_t head;                        /* written by the owner */
    uint32_t tail;                        /* written by the reader */
    uint8_t  buf[SIO1_CABLE_RING];
} CableRing;

typedef struct {
    uint32_t  magic;
    uint32_t  version;
    uint32_t  attached[2];
    uint32_t  lines[2];
    uint64_t  heartbeat_ms[2];
    CableRing ring[2];                    /* ring[i]: bytes sent by side i */
    /* Seats (sio1.h "seats"): who plays at which console. Guarded by
     * seat_lock; seat_gen counts every change. */
    uint32_t  seat_lock;
    uint32_t  seat_gen;
    uint32_t  seat_focus;                 /* 0, or the side last clicked */
    uint32_t  seat_prompt;                /* 0, or the side asking for a controller */
    char      seat_claim[2][SIO1_SEAT_ID]; /* each side's controller id */
} CableShm;

struct Sio1ShmCable {
    CableShm *shm;
    int       me;                         /* 0 or 1 */
    uint64_t  last_beat;
    Sio1Link  link;
    char      name[160];
#if defined(_WIN32)
    HANDLE    mapping;
#endif
};

/* ── atomics on shared memory ────────────────────────────────────────────── */

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
static uint32_t ld32(const uint32_t *p) { uint32_t v = *(volatile const uint32_t *)p; _ReadWriteBarrier(); MemoryBarrier(); return v; }
static void     st32(uint32_t *p, uint32_t v) { MemoryBarrier(); *(volatile uint32_t *)p = v; MemoryBarrier(); }
static uint64_t ld64(const uint64_t *p) { MemoryBarrier(); return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)p, 0, 0); }
static void     st64(uint64_t *p, uint64_t v) { InterlockedExchange64((volatile LONG64 *)p, (LONG64)v); }
static int      cas32(uint32_t *p, uint32_t want, uint32_t v) {
    return (uint32_t)InterlockedCompareExchange((volatile LONG *)p, (LONG)v, (LONG)want) == want;
}
#else
static uint32_t ld32(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void     st32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint64_t ld64(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void     st64(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static int      cas32(uint32_t *p, uint32_t want, uint32_t v) {
    return __atomic_compare_exchange_n(p, &want, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#endif

static uint64_t now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void pause_briefly(void)
{
#if defined(_WIN32)
    Sleep(1);
#else
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
#endif
}

/* ── link ops ────────────────────────────────────────────────────────────── */

static int cable_present(void *ctx)
{
    Sio1ShmCable *c = (Sio1ShmCable *)ctx;
    int other = c->me ^ 1;
    if (!ld32(&c->shm->attached[other])) return 0;
    uint64_t beat = ld64(&c->shm->heartbeat_ms[other]);
    uint64_t now = now_ms();
    return !(now > beat && now - beat > SIO1_CABLE_STALE_MS);
}

static uint32_t cable_partner_lines(void *ctx)
{
    Sio1ShmCable *c = (Sio1ShmCable *)ctx;
    return ld32(&c->shm->lines[c->me ^ 1]) & (SIO1_LINE_DTR | SIO1_LINE_RTS);
}

static void cable_set_lines(void *ctx, uint32_t lines)
{
    Sio1ShmCable *c = (Sio1ShmCable *)ctx;
    st32(&c->shm->lines[c->me], lines);
}

static int cable_send(void *ctx, uint8_t byte)
{
    Sio1ShmCable *c = (Sio1ShmCable *)ctx;
    CableRing *r = &c->shm->ring[c->me];
    uint32_t head = ld32(&r->head), tail = ld32(&r->tail);
    if (head - tail >= SIO1_CABLE_RING) return 0;
    r->buf[head & (SIO1_CABLE_RING - 1u)] = byte;
    st32(&r->head, head + 1u);
    return 1;
}

static int cable_recv(void *ctx, uint8_t *byte)
{
    Sio1ShmCable *c = (Sio1ShmCable *)ctx;
    CableRing *r = &c->shm->ring[c->me ^ 1];
    uint32_t head = ld32(&r->head), tail = ld32(&r->tail);
    if (head == tail) return 0;
    *byte = r->buf[tail & (SIO1_CABLE_RING - 1u)];
    st32(&r->tail, tail + 1u);
    return 1;
}

static void cable_poll(void *ctx)
{
    Sio1ShmCable *c = (Sio1ShmCable *)ctx;
    uint64_t now = now_ms();
    if (now - c->last_beat >= SIO1_HEARTBEAT_MS) {
        c->last_beat = now;
        st64(&c->shm->heartbeat_ms[c->me], now);
    }
}

/* ── seats ───────────────────────────────────────────────────────────────── */

static void seat_lock(CableShm *s)
{
    for (int i = 0; !cas32(&s->seat_lock, 0u, 1u); i++)
        if (i > 64) pause_briefly();
}

static void seat_unlock(CableShm *s)
{
    st32(&s->seat_gen, ld32(&s->seat_gen) + 1u);
    st32(&s->seat_lock, 0u);
}

void sio1_shm_cable_beat(Sio1ShmCable *c)
{
    if (c) cable_poll(c);
}

int sio1_shm_cable_partner_present(Sio1ShmCable *c)
{
    return c ? cable_present(c) : 0;
}

uint32_t sio1_shm_seat_generation(Sio1ShmCable *c)
{
    return c ? ld32(&c->shm->seat_gen) : 0u;
}

void sio1_shm_seat_read(Sio1ShmCable *c, Sio1Seats *out)
{
    memset(out, 0, sizeof *out);
    if (!c) return;
    CableShm *s = c->shm;
    seat_lock(s);
    out->focus  = (int)s->seat_focus;
    out->prompt = (int)s->seat_prompt;
    for (int i = 0; i < 2; i++) {
        memcpy(out->claim[i], s->seat_claim[i], SIO1_SEAT_ID);
        out->claim[i][SIO1_SEAT_ID - 1] = '\0';
    }
    out->generation = s->seat_gen;
    /* a read releases the lock without counting a change */
    st32(&s->seat_lock, 0u);
}

void sio1_shm_seat_write(Sio1ShmCable *c, const Sio1Seats *in, unsigned what)
{
    if (!c) return;
    CableShm *s = c->shm;
    seat_lock(s);
    if (what & SIO1_SEAT_FOCUS)  s->seat_focus  = (uint32_t)in->focus;
    if (what & SIO1_SEAT_PROMPT) s->seat_prompt = (uint32_t)in->prompt;
    for (int i = 0; i < 2; i++) {
        if (!(what & (SIO1_SEAT_CLAIM1 << i))) continue;
        memset(s->seat_claim[i], 0, SIO1_SEAT_ID);
        size_t n = strlen(in->claim[i]);
        if (n > SIO1_SEAT_ID - 1) n = SIO1_SEAT_ID - 1;
        memcpy(s->seat_claim[i], in->claim[i], n);
    }
    seat_unlock(s);
}

/* ── open / close ────────────────────────────────────────────────────────── */

static int name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) > 24) return 0;
    for (const char *p = name; *p; p++) {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
            return 0;
    }
    return 1;
}

Sio1ShmCable *sio1_shm_cable_open(const char *name, int side)
{
    if (!name_ok(name) || (side != 1 && side != 2)) return NULL;
    Sio1ShmCable *c = (Sio1ShmCable *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->me = side - 1;
    const size_t size = sizeof(CableShm);
#if defined(_WIN32)
    snprintf(c->name, sizeof c->name, "Local\\psxrecomp-link-%s", name);
    c->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, (DWORD)size, c->name);
    if (!c->mapping) { free(c); return NULL; }
    c->shm = (CableShm *)MapViewOfFile(c->mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!c->shm) { CloseHandle(c->mapping); free(c); return NULL; }
#else
    snprintf(c->name, sizeof c->name, "/psxl-%s", name);
    int fd = shm_open(c->name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { free(c); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || ((size_t)st.st_size < size && ftruncate(fd, (off_t)size) != 0)) {
        close(fd); free(c); return NULL;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { free(c); return NULL; }
    c->shm = (CableShm *)p;
#endif
    /* Fresh memory is zero: the first side to arrive sets the cable up. */
    CableShm *s = c->shm;
    if (cas32(&s->magic, 0u, SIO1_CABLE_BUSY)) {
        st32(&s->version, SIO1_CABLE_VERSION);
        st32(&s->magic, SIO1_CABLE_MAGIC);
    } else {
        for (int i = 0; i < 2000 && ld32(&s->magic) == SIO1_CABLE_BUSY; i++)
            pause_briefly();
    }
    if (ld32(&s->magic) != SIO1_CABLE_MAGIC || ld32(&s->version) != SIO1_CABLE_VERSION) {
        sio1_shm_cable_close(c);
        return NULL;
    }
    /* Plug this end in: nothing of a previous session of this side is left on
     * the wire in either direction. */
    CableRing *mine = &s->ring[c->me], *theirs = &s->ring[c->me ^ 1];
    st32(&mine->head, ld32(&mine->tail));
    st32(&theirs->tail, ld32(&theirs->head));
    st32(&s->lines[c->me], 0);
    c->last_beat = now_ms();
    st64(&s->heartbeat_ms[c->me], c->last_beat);
    st32(&s->attached[c->me], 1u);

    c->link.present       = cable_present;
    c->link.partner_lines = cable_partner_lines;
    c->link.set_lines     = cable_set_lines;
    c->link.send          = cable_send;
    c->link.recv          = cable_recv;
    c->link.poll          = cable_poll;
    c->link.ctx           = c;
    return c;
}

const Sio1Link *sio1_shm_cable_link(Sio1ShmCable *c)
{
    return c ? &c->link : NULL;
}

void sio1_shm_cable_close(Sio1ShmCable *c)
{
    if (!c) return;
    if (c->shm) {
        int partner = 0;
        if (ld32(&c->shm->magic) == SIO1_CABLE_MAGIC) {
            st32(&c->shm->lines[c->me], 0);
            st32(&c->shm->attached[c->me], 0);
            partner = (int)ld32(&c->shm->attached[c->me ^ 1]);
        }
#if defined(_WIN32)
        (void)partner;
        UnmapViewOfFile(c->shm);
        CloseHandle(c->mapping);
#else
        munmap(c->shm, sizeof(CableShm));
        /* The last side out removes the name (Windows does this itself when
         * the last handle closes). */
        if (!partner) shm_unlink(c->name);
#endif
    }
    free(c);
}
