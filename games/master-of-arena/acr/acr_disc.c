#include "acr_disc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_err(char *err, size_t n, const char *msg) {
    if (err && n) { strncpy(err, msg, n - 1); err[n - 1] = '\0'; }
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    if (b > a) return 0;
    for (size_t i = 0; i < b; ++i)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

/* From a cue sheet: the first FILE and its first track's mode. */
static int parse_cue(const char *cue_path, char *bin_path, size_t bin_size,
                     uint32_t *sector_bytes, uint32_t *data_offset, char *err, size_t err_size) {
    FILE *f = fopen(cue_path, "rb");
    if (!f) { set_err(err, err_size, "cannot open the .cue file"); return 0; }
    char line[1024], name[512] = "";
    int have_mode = 0;
    while (fgets(line, sizeof line, f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!name[0] && strncmp(p, "FILE", 4) == 0) {
            const char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && (size_t)(q2 - q1 - 1) < sizeof name) {
                memcpy(name, q1 + 1, (size_t)(q2 - q1 - 1));
                name[q2 - q1 - 1] = '\0';
            }
        } else if (!have_mode && strncmp(p, "TRACK", 5) == 0) {
            if (strstr(p, "MODE2/2352")) { *sector_bytes = 2352; *data_offset = 24; have_mode = 1; }
            else if (strstr(p, "MODE1/2352")) { *sector_bytes = 2352; *data_offset = 16; have_mode = 1; }
            else if (strstr(p, "MODE1/2048")) { *sector_bytes = 2048; *data_offset = 0; have_mode = 1; }
        }
    }
    fclose(f);
    if (!name[0] || !have_mode) { set_err(err, err_size, "the .cue has no data track"); return 0; }
    /* The bin sits next to the cue. */
    const char *slash = strrchr(cue_path, '/');
    const char *bslash = strrchr(cue_path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    size_t dir_len = slash ? (size_t)(slash - cue_path + 1) : 0;
    if (dir_len + strlen(name) + 1 > bin_size) { set_err(err, err_size, "path too long"); return 0; }
    memcpy(bin_path, cue_path, dir_len);
    strcpy(bin_path + dir_len, name);
    return 1;
}

int acr_disc_open(acr_disc *d, const char *path, char *err, size_t err_size) {
    char bin[1024];
    memset(d, 0, sizeof *d);
    if (ends_with_ci(path, ".cue")) {
        if (!parse_cue(path, bin, sizeof bin, &d->sector_bytes, &d->data_offset, err, err_size))
            return 0;
    } else {
        if (strlen(path) + 1 > sizeof bin) { set_err(err, err_size, "path too long"); return 0; }
        strcpy(bin, path);
        d->sector_bytes = ends_with_ci(path, ".iso") ? 2048u : 2352u;
        d->data_offset = d->sector_bytes == 2352u ? 24u : 0u;
    }
    d->file = fopen(bin, "rb");
    if (!d->file) { set_err(err, err_size, "cannot open the disc image"); return 0; }
    if (fseek(d->file, 0, SEEK_END) != 0) { acr_disc_close(d); set_err(err, err_size, "cannot read the disc image"); return 0; }
    long size = ftell(d->file);
    if (size <= 0) { acr_disc_close(d); set_err(err, err_size, "the disc image is empty"); return 0; }
    d->sectors = (uint32_t)((unsigned long)size / d->sector_bytes);
    uint8_t pvd[ACR_SECTOR_USER];
    if (!acr_disc_read_user(d, 16, pvd, NULL) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        acr_disc_close(d);
        set_err(err, err_size, "no ISO 9660 file system found (is this a PS1 data disc?)");
        return 0;
    }
    return 1;
}

void acr_disc_close(acr_disc *d) {
    if (d->file) fclose(d->file);
    d->file = NULL;
}

int acr_disc_read_raw(acr_disc *d, uint32_t lba, uint8_t *out) {
    if (d->sector_bytes != ACR_SECTOR_RAW || lba >= d->sectors) return 0;
    if (fseek(d->file, (long)lba * (long)ACR_SECTOR_RAW, SEEK_SET) != 0) return 0;
    return fread(out, 1, ACR_SECTOR_RAW, d->file) == ACR_SECTOR_RAW;
}

int acr_disc_read_user(acr_disc *d, uint32_t lba, uint8_t *out, int *form2) {
    if (form2) *form2 = 0;
    if (lba >= d->sectors) return 0;
    if (d->sector_bytes == ACR_SECTOR_USER) {
        if (fseek(d->file, (long)lba * (long)ACR_SECTOR_USER, SEEK_SET) != 0) return 0;
        return fread(out, 1, ACR_SECTOR_USER, d->file) == ACR_SECTOR_USER;
    }
    uint8_t raw[ACR_SECTOR_RAW];
    if (!acr_disc_read_raw(d, lba, raw)) return 0;
    if (form2 && raw[15] == 2 && (raw[18] & 0x20u)) *form2 = 1;
    memcpy(out, raw + d->data_offset, ACR_SECTOR_USER);
    return 1;
}

#define ACR_ISO_MAX_DEPTH 8
#define ACR_ISO_MAX_PATH  512

static int walk_dir(acr_disc *d, uint32_t lba, uint32_t bytes, char *path, size_t path_len,
                    int depth, acr_iso_visit visit, void *ctx) {
    if (depth > ACR_ISO_MAX_DEPTH) return 1;
    uint8_t sector[ACR_SECTOR_USER];
    const uint32_t count = (bytes + ACR_SECTOR_USER - 1) / ACR_SECTOR_USER;
    for (uint32_t s = 0; s < count; ++s) {
        if (!acr_disc_read_user(d, lba + s, sector, NULL)) return 0;
        uint32_t p = 0;
        while (p < ACR_SECTOR_USER && sector[p]) {
            const uint32_t len = sector[p];
            const uint32_t name_len = len >= 33 ? sector[p + 32] : 0;
            if (len < 34 || p + len > ACR_SECTOR_USER || 33u + name_len > len) return 0;
            const uint8_t *rec = sector + p;
            p += len;
            if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) continue;   /* "." and ".." */
            char name[256];
            uint32_t n = 0;
            for (uint32_t i = 0; i < name_len && rec[33 + i] != ';'; ++i) {
                const char c = (char)rec[33 + i];
                if (c == '/' || c == '\\' || c == '\0') return 0;   /* refuse odd names */
                name[n++] = c;
            }
            if (n && name[n - 1] == '.') --n;   /* "README." for files with no extension */
            name[n] = '\0';
            if (!n || strcmp(name, "..") == 0 || strcmp(name, ".") == 0) continue;
            if (path_len + 1 + n + 1 > ACR_ISO_MAX_PATH) return 0;
            const size_t child_len = path_len + (path_len ? 1 : 0) + n;
            if (path_len) path[path_len] = '/';
            memcpy(path + path_len + (path_len ? 1 : 0), name, n + 1);
            const int is_dir = (rec[25] & 2u) != 0;
            const uint32_t child_lba = le32(rec + 2), child_bytes = le32(rec + 10);
            if (!visit(ctx, path, child_lba, child_bytes, is_dir)) return 0;
            if (is_dir && !walk_dir(d, child_lba, child_bytes, path, child_len, depth + 1, visit, ctx))
                return 0;
            path[path_len] = '\0';
        }
    }
    return 1;
}

int acr_iso_walk(acr_disc *d, acr_iso_visit visit, void *ctx) {
    uint8_t pvd[ACR_SECTOR_USER];
    if (!acr_disc_read_user(d, 16, pvd, NULL)) return 0;
    char path[ACR_ISO_MAX_PATH] = "";
    return walk_dir(d, le32(pvd + 156 + 2), le32(pvd + 156 + 10), path, 0, 0, visit, ctx);
}
