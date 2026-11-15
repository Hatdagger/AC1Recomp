#ifndef ACR_MOTION_MOA_H
#define ACR_MOTION_MOA_H

#include <stddef.h>
#include <stdint.h>

/* Writes <out_dir>/models.toml (which animation set each core/arms/legs
 * combination uses) and <out_dir>/model-NN.toml (each set's root motion),
 * from the menu module and the unpacked GG/P*\/PA*.T archives.
 * Returns 0 with a message in `err` on failure. */
int acr_export_moa_motion(const char *menu_module, const char *unpacked_dir, const char *out_dir,
                          uint32_t *count, char *err, size_t err_size);

#endif
