/* Copies every file off a Master of Arena disc and unpacks its .T archives.
 *
 *   <out>/disc/<path on disc>        every file, as stored
 *   <out>/disc/<path>.raw2352        files with XA (Form 2) sectors, kept whole
 *   <out>/unpacked/<path without .T>/NNN.bin   one file per archive entry
 *   <out>/unpacked/<path without .T>/archive.toml  the entry table
 *   <out>/parts/<category>/...toml   every part's stats (acr_parts_moa.c)
 *   <out>/motion/...toml             animation sets' root motion (acr_motion_moa.c)
 *   <out>/models/...                 AC models, textures, skeletons, animations (acr_models_moa.c)
 *
 * A MoA .T archive starts with a u16 entry count followed by count + 1 u16
 * sector offsets (relative to the archive start); entry i covers sectors
 * off[i] up to off[i + 1]. This matches the game's own loader. */
#ifndef ACR_IMPORT_H
#define ACR_IMPORT_H

#include <stddef.h>
#include <stdint.h>

typedef struct acr_import_stats {
    uint32_t files;            /* files copied */
    uint32_t raw_files;        /* files kept as raw sectors (XA audio, video) */
    uint32_t archives;         /* .T archives unpacked */
    uint32_t archive_entries;  /* entries written from them */
    uint32_t archives_skipped; /* .T files whose table did not check out */
    uint32_t parts;            /* part files written to <out>/parts */
    uint32_t motion_sets;      /* animation sets written to <out>/motion */
    uint32_t model_parts;      /* part models written to <out>/models */
    uint32_t model_sets;       /* skeletons + animations written to <out>/models/sets */
} acr_import_stats;

int acr_import_disc(const char *disc_path, const char *out_dir, acr_import_stats *stats,
                    char *err, size_t err_size);

/* The same, but only unpacks the .T archives (no copy of the disc's other
 * files): enough for parts/, motion/ and models/. The launcher's install
 * uses it and deletes <out>/unpacked afterwards. */
int acr_import_disc_models(const char *disc_path, const char *out_dir, acr_import_stats *stats,
                           char *err, size_t err_size);

/* Parse an archive table from a .T file's bytes. Fills `offsets` (count + 1
 * values) and returns the entry count, or 0 when the table is not valid. */
uint32_t acr_moa_archive_table(const uint8_t *data, size_t bytes, uint16_t *offsets,
                               uint32_t max_entries);

#endif
