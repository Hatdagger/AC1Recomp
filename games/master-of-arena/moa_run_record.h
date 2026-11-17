/* Master of Arena: Rogue Arena fight recorder.
 *
 * Logs every Rogue Arena fight to "<run file>.rec.csv": the player's buttons
 * and both ACs' position, facing, state, AP, energy, lock and refire timer,
 * looked at on every call of the arena engine's controller sampler and written
 * whenever any of them changed (nothing lost, nothing repeated), plus a per-fight summary
 * (time in the air, blade swings, shots, boosting, time locked on, range,
 * hits). The data is for mapping now and, later, for shaping a ghost's AI from
 * how the player fought. Each fight starts with a check of the AC layout
 * (docs/mapping/battle-system.md, measured in VS) and raw dumps of both AC
 * objects and player blocks, so the arena engine's layout can be confirmed
 * from a test log. Recording only reads guest memory.
 */
#ifndef MOA_RUN_RECORD_H
#define MOA_RUN_RECORD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A new session: forget any fight in progress. */
void moa_rec_reset(void);

/* Once per call of the fight's controller sampler while the fight runs.
 * run_file: the run file (the recording goes beside it). */
void moa_rec_sample(const char *run_file, unsigned attempt, unsigned wins, unsigned opponent);

/* The fight's result is in. Writes the summary once per fight and returns it
 * (for the run log), or NULL when no fight was being recorded. */
const char *moa_rec_end(int won);

#ifdef __cplusplus
}
#endif

#endif
