#ifndef ACR_PARTS_MOA_H
#define ACR_PARTS_MOA_H

#include <stddef.h>
#include <stdint.h>

/* Writes <out_dir>/<category>[/<group>]/<NNN>-<NAME>.toml for every part in
 * Master of Arena's tables, read from the menu module (unpacked
 * GG/COM/FDAT/206.bin). Returns 0 with a message in `err` on failure. */
int acr_export_moa_parts(const char *menu_module, const char *out_dir, uint32_t *count,
                         char *err, size_t err_size);

#endif
