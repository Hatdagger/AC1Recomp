/* AC renders off the game's thread (Rogue Arena VS screen portraits).
 *
 * One worker thread renders queued ACs from the 3D model install
 * (moa_acv_load + moa_acv_render_ex) into new RGBA buffers, oldest job
 * first. The caller gathers everything that touches the game (records,
 * paint) beforehand; a job only needs the model install. A job is a small
 * handle: poll it with moa_rq_take, give it up with moa_rq_drop (a running
 * render then finishes and is thrown away). */
#ifndef MOA_RENDER_QUEUE_H
#define MOA_RENDER_QUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t moa_rq_job;            /* 0 = no job */

/* Queue a render: `size` x `size` RGBA, rows top first, see-through
 * background. 0 if it could not be queued (no install, queue full, no
 * thread): the caller draws without it. */
moa_rq_job moa_rq_submit(const char *models_dir, const uint8_t rec[13], const uint8_t paint[48], int size,
                         float yaw, float pitch, float zoom);

/* 1 = finished: *px is the render (caller frees; NULL if the AC could not
 * be loaded) and the job is gone. 0 = still waiting or running. -1 = no
 * such job. */
int moa_rq_take(moa_rq_job job, uint8_t **px);

/* Not wanted any more (any state). */
void moa_rq_drop(moa_rq_job job);

/* Jobs waiting or running (for the log). */
int moa_rq_pending(void);

#ifdef __cplusplus
}
#endif

#endif
