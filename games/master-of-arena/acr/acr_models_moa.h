#ifndef ACR_MODELS_MOA_H
#define ACR_MODELS_MOA_H

#include <stddef.h>
#include <stdint.h>

/* Master of Arena AC models as readable files (docs/research/gen1-models.md):
 *
 *   <out>/<category>/<NN>/piece<K>.obj   one OBJ per piece, in its joint's space
 *   <out>/<category>/<NN>/part.mtl       materials (texture, flat colours)
 *   <out>/<category>/<NN>/texture.png    8-bit indexed texture (pattern 0)
 *   <out>/<category>/<NN>/part.toml      which set it came from, piece -> slot
 *   <out>/sets/set-NN.toml               skeleton: nodes, parents, slots, bind pose
 *   <out>/sets/set-NN-anims.txt          the set's joint animations as text
 *
 * Reads the menu module (set table) and the unpacked GG/P*\/PA*.T and GG/MS
 * archives. Returns 0 with a message in `err` on failure. */
int acr_export_moa_models(const char *menu_module, const char *unpacked_dir, const char *out_dir,
                          uint32_t *parts, uint32_t *sets, char *err, size_t err_size);

/* What each slot of a set holds, from the highest slot down. */
enum { ACR_MOA_HEAD, ACR_MOA_CORE, ACR_MOA_LEGS, ACR_MOA_ARMS, ACR_MOA_HAND, ACR_MOA_BACK_LEFT,
       ACR_MOA_BACK_RIGHT, ACR_MOA_BOOSTER };
typedef struct acr_moa_role { uint8_t part, piece; } acr_moa_role;

/* Fills `roles` (highest slot first) for a set whose arms have `arms_pieces`
 * (7, or 8 for weapon arms, which have no hand) and whose legs have
 * `legs_pieces` (7, 1 for caterpillars, 9 for four legs). Returns the count. */
unsigned acr_moa_slot_roles(unsigned arms_pieces, unsigned legs_pieces, acr_moa_role *roles,
                            unsigned max);

/* Writes an 8-bit indexed PNG (stored, uncompressed deflate). `palette` is
 * 256 RGB triples, `alpha` 256 values (NULL = all opaque). Returns 0 on failure. */
int acr_write_png8(const char *path, unsigned w, unsigned h, const uint8_t *pixels,
                   const uint8_t *palette, const uint8_t *alpha);
uint32_t acr_crc32(const uint8_t *data, size_t n, uint32_t crc);

#endif
