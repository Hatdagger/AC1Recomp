/* AC renders off the game's thread: see moa_render_queue.h. */
#include "moa_render_queue.h"
#include "moa_ac_view.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#  include <windows.h>
typedef SRWLOCK rq_lock_t;
typedef CONDITION_VARIABLE rq_cond_t;
static rq_lock_t s_lock = SRWLOCK_INIT;
static rq_cond_t s_cond = CONDITION_VARIABLE_INIT;
static void rq_lock(void) { AcquireSRWLockExclusive(&s_lock); }
static void rq_unlock(void) { ReleaseSRWLockExclusive(&s_lock); }
static void rq_wait(void) { SleepConditionVariableSRW(&s_cond, &s_lock, INFINITE, 0); }
static void rq_wake(void) { WakeConditionVariable(&s_cond); }
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static void rq_lock(void) { pthread_mutex_lock(&s_lock); }
static void rq_unlock(void) { pthread_mutex_unlock(&s_lock); }
static void rq_wait(void) { pthread_cond_wait(&s_cond, &s_lock); }
static void rq_wake(void) { pthread_cond_signal(&s_cond); }
#endif

enum { RQ_FREE = 0, RQ_QUEUED, RQ_RUNNING, RQ_DONE };
#define RQ_SLOTS 24

struct rq_slot {
    int state;
    int dropped;                        /* RUNNING but no longer wanted */
    uint32_t gen;                       /* the handle's high bits */
    uint32_t order;                     /* first in, first out */
    char dir[1024];
    uint8_t rec[13], paint[48];
    int has_paint, size;
    float yaw, pitch, zoom;
    uint8_t *px;
};

static struct rq_slot s_slot[RQ_SLOTS];
static uint32_t s_gen = 1, s_order;
static int s_thread;                    /* 1 started, -1 could not start */

static moa_rq_job rq_handle(int i) { return (s_slot[i].gen << 5) | (uint32_t)(i + 1); }

static struct rq_slot *rq_find(moa_rq_job job)
{
    const int i = (int)(job & 31u) - 1;
    if (i < 0 || i >= RQ_SLOTS) return NULL;
    struct rq_slot *s = &s_slot[i];
    return s->state != RQ_FREE && rq_handle(i) == job ? s : NULL;
}

static void rq_run(struct rq_slot *s)
{
    uint8_t *px = NULL;
    moa_acv *v = moa_acv_load(s->dir, s->rec, s->has_paint ? s->paint : NULL);
    if (v) {
        px = (uint8_t *)calloc((size_t)s->size * (size_t)s->size, 4u);
        if (px) moa_acv_render_ex(v, s->yaw, s->pitch, 0.0f, s->zoom, 0u, NULL, s->size, s->size, px);
        moa_acv_free(v);
    }
    rq_lock();
    if (s->dropped) {
        free(px);
        s->state = RQ_FREE;
        s->dropped = 0;
    } else {
        s->px = px;
        s->state = RQ_DONE;
    }
    rq_unlock();
}

#if defined(_WIN32)
static DWORD WINAPI rq_main(LPVOID arg)
#else
static void *rq_main(void *arg)
#endif
{
    (void)arg;
    for (;;) {
        rq_lock();
        struct rq_slot *next = NULL;
        while (!next) {
            for (int i = 0; i < RQ_SLOTS; i++)
                if (s_slot[i].state == RQ_QUEUED && (!next || (int32_t)(s_slot[i].order - next->order) < 0))
                    next = &s_slot[i];
            if (!next) rq_wait();
        }
        next->state = RQ_RUNNING;
        next->dropped = 0;
        rq_unlock();
        rq_run(next);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* Under the lock. */
static int rq_start(void)
{
    if (s_thread) return s_thread > 0;
#if defined(_WIN32)
    HANDLE th = CreateThread(NULL, 0, rq_main, NULL, 0, NULL);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
    s_thread = th ? 1 : -1;
#else
    pthread_t th;
    s_thread = pthread_create(&th, NULL, rq_main, NULL) == 0 ? 1 : -1;
    if (s_thread > 0) pthread_detach(th);
#endif
    return s_thread > 0;
}

moa_rq_job moa_rq_submit(const char *models_dir, const uint8_t rec[13], const uint8_t paint[48], int size,
                         float yaw, float pitch, float zoom)
{
    if (!models_dir || !models_dir[0] || !rec || size <= 0 || size > 4096 ||
        strlen(models_dir) >= sizeof s_slot[0].dir)
        return 0;
    moa_rq_job job = 0;
    rq_lock();
    if (rq_start()) {
        for (int i = 0; i < RQ_SLOTS; i++) {
            struct rq_slot *s = &s_slot[i];
            if (s->state != RQ_FREE) continue;
            memset(s, 0, sizeof *s);
            s->state = RQ_QUEUED;
            s->gen = s_gen++ & 0x07FFFFFFu;
            if (!s->gen) s->gen = s_gen++ & 0x07FFFFFFu;
            s->order = s_order++;
            snprintf(s->dir, sizeof s->dir, "%s", models_dir);
            memcpy(s->rec, rec, 13);
            if (paint) { memcpy(s->paint, paint, 48); s->has_paint = 1; }
            s->size = size; s->yaw = yaw; s->pitch = pitch; s->zoom = zoom;
            job = rq_handle(i);
            break;
        }
    }
    if (job) rq_wake();
    rq_unlock();
    return job;
}

int moa_rq_take(moa_rq_job job, uint8_t **px)
{
    int r;
    *px = NULL;
    rq_lock();
    struct rq_slot *s = job ? rq_find(job) : NULL;
    if (!s || s->dropped) r = -1;
    else if (s->state != RQ_DONE) r = 0;
    else {
        *px = s->px;
        s->px = NULL;
        s->state = RQ_FREE;
        r = 1;
    }
    rq_unlock();
    return r;
}

void moa_rq_drop(moa_rq_job job)
{
    if (!job) return;
    rq_lock();
    struct rq_slot *s = rq_find(job);
    if (s) {
        if (s->state == RQ_RUNNING) s->dropped = 1;     /* the worker throws it away */
        else {
            free(s->px);
            s->px = NULL;
            s->state = RQ_FREE;
        }
    }
    rq_unlock();
}

int moa_rq_pending(void)
{
    int n = 0;
    rq_lock();
    for (int i = 0; i < RQ_SLOTS; i++)
        n += (s_slot[i].state == RQ_QUEUED || s_slot[i].state == RQ_RUNNING) && !s_slot[i].dropped;
    rq_unlock();
    return n;
}
