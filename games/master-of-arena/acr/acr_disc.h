/* Reading a PS1 disc image: cue/bin (raw 2352-byte sectors) or a cooked
 * 2048-byte .iso, plus the ISO 9660 file system on it. */
#ifndef ACR_DISC_H
#define ACR_DISC_H

#include <stdint.h>
#include <stdio.h>

#define ACR_SECTOR_USER 2048u
#define ACR_SECTOR_RAW  2352u

typedef struct acr_disc {
    FILE    *file;
    uint32_t sector_bytes;   /* 2352 raw or 2048 cooked */
    uint32_t data_offset;    /* user data inside a sector: 24 Mode 2, 16 Mode 1, 0 cooked */
    uint32_t sectors;
} acr_disc;

/* `path` is a .cue, .bin or .iso. Returns 0 with a message in `err` on failure. */
int  acr_disc_open(acr_disc *d, const char *path, char *err, size_t err_size);
void acr_disc_close(acr_disc *d);

/* One sector's 2048 user bytes. `form2` (may be NULL) is set when the sector
 * is Mode 2 Form 2 (XA audio, video), whose payload is not plain file data. */
int acr_disc_read_user(acr_disc *d, uint32_t lba, uint8_t *out, int *form2);

/* The whole 2352-byte sector; raw images only. */
int acr_disc_read_raw(acr_disc *d, uint32_t lba, uint8_t *out);

/* Visit every file and directory. `path` uses '/' and has no ";1" version.
 * The callback returns 0 to stop the walk. */
typedef int (*acr_iso_visit)(void *ctx, const char *path, uint32_t lba,
                             uint32_t bytes, int is_dir);
int acr_iso_walk(acr_disc *d, acr_iso_visit visit, void *ctx);

#endif
