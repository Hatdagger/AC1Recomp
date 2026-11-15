/* Master of Arena: install the Project Phantasma arena from the player's own
 * discs. Nothing from either disc ships with the game: the arena package is
 * built here, on the player's computer, from their Master of Arena (USA)
 * disc 1 and their Armored Core: Project Phantasma (USA) disc. */
#ifndef MOA_PHANTASMA_H
#define MOA_PHANTASMA_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when `path` (.cue, .bin or .iso) is an Armored Core: Project Phantasma
 * (USA) disc image. */
int moa_phantasma_is_disc(const char *path);

/* Build the arena package (a .psxmod archive) from the two disc images and
 * write it to `out_path`. 1 = ok; 0 = failed, with the reason in `error`. */
int moa_phantasma_build(const char *moa_disc, const char *phantasma_disc,
                        const char *out_path, char *error, unsigned error_size);

/* Read `bytes` bytes at `offset` (a multiple of 2048) of a file on a disc
 * image (.cue / .bin / .iso), e.g. "GG/COM/FDAT.T". With bytes = 0 only the
 * file's size is reported. Free the result with free(); NULL = failed. */
unsigned char *moa_disc_read_range(const char *disc_path, const char *file_path,
                                   unsigned int offset, unsigned int bytes,
                                   unsigned int *file_size);

#ifdef __cplusplus
}
#endif

#endif
