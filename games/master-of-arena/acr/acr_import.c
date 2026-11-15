#include "acr_import.h"
#include "acr_disc.h"
#include "acr_parts_moa.h"
#include "acr_motion_moa.h"
#include "acr_models_moa.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define acr_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define acr_mkdir(p) mkdir((p), 0755)
#endif

#define ACR_PATH_MAX 1024
#define ACR_ARCHIVE_MAX_ENTRIES 1022   /* what fits after the count in one sector */

static void set_err(char *err, size_t n, const char *msg, const char *detail) {
    if (err && n) snprintf(err, n, "%s%s%s", msg, detail ? ": " : "", detail ? detail : "");
}

/* mkdir -p for the directory part of `path` (or all of it when `whole`). */
static int make_dirs(const char *path, int whole) {
    char buf[ACR_PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof buf) return 0;
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i <= len; ++i) {
        if (buf[i] == '/' || buf[i] == '\\' || (whole && buf[i] == '\0')) {
            const char saved = buf[i];
            buf[i] = '\0';
            if (acr_mkdir(buf) != 0 && errno != EEXIST) return 0;
            buf[i] = saved;
        }
    }
    return 1;
}

static int write_file(const char *path, const void *data, size_t bytes) {
    if (!make_dirs(path, 0)) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, bytes, f) == bytes;
    return fclose(f) == 0 && ok;
}

static int ends_with_t(const char *s) {
    const size_t n = strlen(s);
    return n > 2 && s[n - 2] == '.' && toupper((unsigned char)s[n - 1]) == 'T';
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t acr_moa_archive_table(const uint8_t *data, size_t bytes, uint16_t *offsets,
                               uint32_t max_entries) {
    if (!data || bytes < ACR_SECTOR_USER) return 0;
    const uint32_t count = le16(data);
    if (count == 0 || count > max_entries || count > ACR_ARCHIVE_MAX_ENTRIES) return 0;
    const uint32_t sectors = (uint32_t)((bytes + ACR_SECTOR_USER - 1) / ACR_SECTOR_USER);
    for (uint32_t i = 0; i <= count; ++i) {
        offsets[i] = le16(data + 2 + 2 * i);
        if (i == 0 && offsets[0] < 1) return 0;               /* sector 0 is the table */
        if (i > 0 && offsets[i] < offsets[i - 1]) return 0;   /* must not go backwards */
    }
    if (offsets[count] > sectors) return 0;
    return count;
}

typedef struct import_ctx {
    acr_disc *disc;
    const char *out;
    acr_import_stats *stats;
    char *err;
    size_t err_size;
    int archives_only;   /* launcher install: unpack the .T archives, copy nothing else */
} import_ctx;

static int unpack_archive(import_ctx *c, const char *disc_path, const uint8_t *data, size_t bytes) {
    uint16_t off[ACR_ARCHIVE_MAX_ENTRIES + 1];
    const uint32_t count = acr_moa_archive_table(data, bytes, off, ACR_ARCHIVE_MAX_ENTRIES);
    if (!count) { c->stats->archives_skipped++; return 1; }
    char dir[ACR_PATH_MAX], path[ACR_PATH_MAX + 32];
    const int n = snprintf(dir, sizeof dir, "%s/unpacked/%s", c->out, disc_path);
    if (n <= 2 || (size_t)n >= sizeof dir) return 0;
    dir[n - 2] = '\0';   /* drop ".T" */
    const size_t index_cap = ACR_PATH_MAX + 96u * (size_t)count;
    char *index = malloc(index_cap);
    if (!index) return 0;
    size_t at = (size_t)snprintf(index, index_cap, "# %s: %u entries, offsets in 2048-byte sectors\n",
                                 disc_path, (unsigned)count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t start = (size_t)off[i] * ACR_SECTOR_USER;
        size_t size = (size_t)(off[i + 1] - off[i]) * ACR_SECTOR_USER;
        if (start + size > bytes) size = bytes > start ? bytes - start : 0;
        snprintf(path, sizeof path, "%s/%03u.bin", dir, (unsigned)i);
        if (!write_file(path, data + start, size)) { free(index); return 0; }
        at += (size_t)snprintf(index + at, index_cap - at,
                               "[[entry]]\nindex = %u\nsector = %u\nsectors = %u\n",
                               (unsigned)i, (unsigned)off[i], (unsigned)(off[i + 1] - off[i]));
        c->stats->archive_entries++;
    }
    snprintf(path, sizeof path, "%s/archive.toml", dir);
    const int ok = write_file(path, index, at);
    free(index);
    if (ok) c->stats->archives++;
    return ok;
}

static int visit(void *ctx, const char *disc_path, uint32_t lba, uint32_t bytes, int is_dir) {
    import_ctx *c = (import_ctx *)ctx;
    if (is_dir) return 1;
    if (c->archives_only && !ends_with_t(disc_path)) return 1;
    const uint32_t sectors = (bytes + ACR_SECTOR_USER - 1) / ACR_SECTOR_USER;
    uint8_t *data = malloc((size_t)sectors * ACR_SECTOR_USER + 1);
    if (!data) { set_err(c->err, c->err_size, "out of memory reading", disc_path); return 0; }
    int has_form2 = 0;
    for (uint32_t s = 0; s < sectors; ++s) {
        int form2 = 0;
        if (!acr_disc_read_user(c->disc, lba + s, data + (size_t)s * ACR_SECTOR_USER, &form2)) {
            free(data);
            set_err(c->err, c->err_size, "cannot read", disc_path);
            return 0;
        }
        has_form2 |= form2;
    }
    char path[ACR_PATH_MAX];
    int ok;
    if (has_form2) {
        /* XA audio and video: keep every raw sector so nothing is lost. */
        free(data);
        data = malloc((size_t)sectors * ACR_SECTOR_RAW + 1);
        if (!data) { set_err(c->err, c->err_size, "out of memory reading", disc_path); return 0; }
        for (uint32_t s = 0; s < sectors; ++s)
            if (!acr_disc_read_raw(c->disc, lba + s, data + (size_t)s * ACR_SECTOR_RAW)) {
                free(data);
                set_err(c->err, c->err_size, "cannot read", disc_path);
                return 0;
            }
        snprintf(path, sizeof path, "%s/disc/%s.raw2352", c->out, disc_path);
        ok = write_file(path, data, (size_t)sectors * ACR_SECTOR_RAW);
        if (ok) c->stats->raw_files++;
    } else if (c->archives_only) {
        snprintf(path, sizeof path, "%s/unpacked/%s", c->out, disc_path);
        ok = unpack_archive(c, disc_path, data, bytes);
    } else {
        snprintf(path, sizeof path, "%s/disc/%s", c->out, disc_path);
        ok = write_file(path, data, bytes);
        if (ok) c->stats->files++;
        if (ok && ends_with_t(disc_path)) ok = unpack_archive(c, disc_path, data, bytes);
    }
    free(data);
    if (!ok) set_err(c->err, c->err_size, "cannot write", path);
    return ok;
}

static int import_disc(const char *disc_path, const char *out_dir, acr_import_stats *stats,
                       int archives_only, char *err, size_t err_size) {
    acr_disc disc;
    memset(stats, 0, sizeof *stats);
    if (!acr_disc_open(&disc, disc_path, err, err_size)) return 0;
    if (!make_dirs(out_dir, 1)) {
        acr_disc_close(&disc);
        set_err(err, err_size, "cannot create", out_dir);
        return 0;
    }
    import_ctx c = { &disc, out_dir, stats, err, err_size, archives_only };
    const int ok = acr_iso_walk(&disc, visit, &c);
    if (!ok && err && !err[0]) set_err(err, err_size, "the disc's file system could not be read", NULL);
    acr_disc_close(&disc);
    if (!ok) return 0;
    /* Master of Arena: export the part tables when the menu module is there. */
    char menu[ACR_PATH_MAX], parts_dir[ACR_PATH_MAX];
    snprintf(menu, sizeof menu, "%s/unpacked/GG/COM/FDAT/206.bin", out_dir);
    FILE *probe = fopen(menu, "rb");
    if (probe) {
        fclose(probe);
        snprintf(parts_dir, sizeof parts_dir, "%s/parts", out_dir);
        if (!acr_export_moa_parts(menu, parts_dir, &stats->parts, err, err_size)) return 0;
        char motion_dir[ACR_PATH_MAX], unpacked[ACR_PATH_MAX];
        snprintf(motion_dir, sizeof motion_dir, "%s/motion", out_dir);
        snprintf(unpacked, sizeof unpacked, "%s/unpacked", out_dir);
        if (!make_dirs(motion_dir, 1) ||
            !acr_export_moa_motion(menu, unpacked, motion_dir, &stats->motion_sets, err, err_size))
            return 0;
        char models_dir[ACR_PATH_MAX];
        snprintf(models_dir, sizeof models_dir, "%s/models", out_dir);
        if (!make_dirs(models_dir, 1) ||
            !acr_export_moa_models(menu, unpacked, models_dir, &stats->model_parts, &stats->model_sets,
                                   err, err_size))
            return 0;
    }
    return 1;
}

int acr_import_disc(const char *disc_path, const char *out_dir, acr_import_stats *stats,
                    char *err, size_t err_size) {
    return import_disc(disc_path, out_dir, stats, 0, err, err_size);
}

int acr_import_disc_models(const char *disc_path, const char *out_dir, acr_import_stats *stats,
                           char *err, size_t err_size) {
    return import_disc(disc_path, out_dir, stats, 1, err, err_size);
}
