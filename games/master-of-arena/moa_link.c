/* Master of Arena: Link Versus memory cards (launcher). See moa_link.h.
 *
 * PS1 memory card image (128 KB): frame 0 header, frames 1..15 the directory
 * (one per block), then 15 blocks of 8 KB. A directory frame: u32 state
 * (0x51 first block of a save, 0x52 middle, 0x53 last, 0xA0 free, 0xA1..0xA3
 * deleted), u32 size in bytes (first block only), u16 next block (0..14, or
 * 0xFFFF for none), the save's name from byte 10, and an XOR of bytes 0..126
 * in byte 127. A save's first block starts with its header: "SC", icon
 * flags, block count, then its title (Shift-JIS) from byte 4. */
#include "moa_link.h"
#include "moa_card.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#  include <direct.h>
#endif

#define MC_SIZE  131072
#define MC_BLOCK 8192

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) snprintf(err, n, "%s", msg);
}

static int read_image(const char *path, uint8_t *mc)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t got = fread(mc, 1, MC_SIZE, f);
    fclose(f);
    return got == MC_SIZE && mc[0] == 'M' && mc[1] == 'C';
}

static void make_dirs(const char *path)
{
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *q = dir + 1; *q; q++) {
        if (*q != '/' && *q != '\\') continue;
        const char c = *q;
        *q = '\0';
#if defined(_WIN32)
        _mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
        *q = c;
    }
}

static int write_file(const char *path, const uint8_t *mc)
{
    make_dirs(path);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(mc, 1, MC_SIZE, f) == MC_SIZE;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* The old image beside the card as <path>.bak, then the new one. */
static int write_with_backup(const char *path, const uint8_t *old_mc, const uint8_t *mc,
                             char *err, size_t n)
{
    if (old_mc) {
        char bak[1100];
        snprintf(bak, sizeof bak, "%s.bak", path);
        if (!write_file(bak, old_mc)) { set_err(err, n, "the backup copy could not be written"); return 0; }
    }
    if (!write_file(path, mc)) {
        set_err(err, n, old_mc ? "the memory card could not be written (backup kept)"
                                : "the memory card could not be written");
        return 0;
    }
    return 1;
}

static void frame_seal(uint8_t *fr)
{
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= fr[i];
    fr[127] = x;
}

static void frame_free(uint8_t *fr)
{
    memset(fr, 0, 128);
    fr[0] = 0xA0;
    fr[8] = fr[9] = 0xFF;
    frame_seal(fr);
}

static uint32_t frame_state(const uint8_t *fr)
{
    return (uint32_t)fr[0] | (uint32_t)fr[1] << 8 | (uint32_t)fr[2] << 16 | (uint32_t)fr[3] << 24;
}

static int frame_used(const uint8_t *fr) { return (frame_state(fr) & 0xF0) == 0x50; }
static int frame_first(const uint8_t *fr) { return frame_state(fr) == 0x51; }

/* A formatted, empty card. */
static void format_image(uint8_t *mc)
{
    memset(mc, 0, MC_SIZE);
    for (int i = 0; i < 64; i++) {
        uint8_t *fr = mc + i * 128;
        if (i == 0 || i == 63) { fr[0] = 'M'; fr[1] = 'C'; }
        else if (i < 16) { fr[0] = 0xA0; fr[8] = fr[9] = 0xFF; }
        else if (i < 36) { fr[0] = fr[1] = fr[2] = fr[3] = 0xFF; fr[8] = fr[9] = 0xFF; }
        else continue;
        frame_seal(fr);
    }
}

/* The directory slots (1..15) of the save starting at `slot`, in order. */
static int save_chain(const uint8_t *mc, int slot, int out[15])
{
    if (slot < 1 || slot > 15 || !frame_first(mc + slot * 128)) return 0;
    int n = 0, s = slot;
    while (s >= 1 && s <= 15 && n < 15) {
        for (int k = 0; k < n; k++) if (out[k] == s) return n;   /* a loop: stop */
        out[n++] = s;
        const uint8_t *fr = mc + s * 128;
        const unsigned next = (unsigned)fr[8] | (unsigned)fr[9] << 8;
        if (next == 0xFFFF) break;
        s = (int)next + 1;
    }
    return n;
}

/* Plain letters for a Shift-JIS title: full-width letters, digits and space
 * become ASCII, ASCII stays, anything else becomes '?'. */
static void title_text(const uint8_t *t, size_t len, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < len && t[i] && o + 1 < n;) {
        const uint8_t a = t[i];
        if (a < 0x80) { out[o++] = (char)a; i++; continue; }
        if (i + 1 >= len) break;
        const uint8_t b = t[i + 1];
        char c = '?';
        if (a == 0x82 && b >= 0x60 && b <= 0x79) c = (char)('A' + (b - 0x60));
        else if (a == 0x82 && b >= 0x81 && b <= 0x9A) c = (char)('a' + (b - 0x81));
        else if (a == 0x82 && b >= 0x4F && b <= 0x58) c = (char)('0' + (b - 0x4F));
        else if (a == 0x81 && b == 0x40) c = ' ';
        else if (a == 0x81 && b == 0x5B) c = '-';
        else if (a == 0x81 && b == 0x7B) c = '+';
        out[o++] = c;
        i += 2;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

int moa_memcard_list_saves(const char *path, struct moa_card_save *out, int max)
{
    uint8_t *mc = (uint8_t *)malloc(MC_SIZE);
    if (!mc) return -1;
    if (!read_image(path, mc)) { free(mc); return -1; }
    struct moa_story_save story[15];
    const int ns = moa_memcard_list_story(path, story, 15);
    int n = 0;
    for (int i = 1; i < 16 && n < max; i++) {
        const uint8_t *fr = mc + i * 128;
        if (!frame_first(fr)) continue;
        struct moa_card_save *s = &out[n++];
        memset(s, 0, sizeof *s);
        s->slot = i;
        int chain[15];
        s->blocks = save_chain(mc, i, chain);
        memcpy(s->file, fr + 10, 20);
        s->file[20] = '\0';
        const uint8_t *b = mc + i * MC_BLOCK;
        if (b[0] == 'S' && b[1] == 'C') title_text(b + 4, 64, s->title, sizeof s->title);
        for (int k = 0; k < ns; k++) {
            if (story[k].slot != i) continue;
            s->moa = 1;
            memcpy(s->pilot, story[k].pilot, sizeof s->pilot);
            memcpy(s->ac, story[k].ac, sizeof s->ac);
        }
        s->quick_start = strcmp(s->file, MOA_LINK_QS_FILE) == 0 ||
                         strcmp(s->file, MOA_LINK_QS_HP_FILE) == 0;
    }
    free(mc);
    return n;
}

static int free_slots(const uint8_t *mc, int out[15])
{
    int n = 0;
    for (int i = 1; i < 16; i++) if (!frame_used(mc + i * 128)) out[n++] = i;
    return n;
}

int moa_memcard_free_blocks(const char *path)
{
    uint8_t *mc = (uint8_t *)malloc(MC_SIZE);
    if (!mc) return -1;
    int n = -1, slots[15];
    if (read_image(path, mc)) n = free_slots(mc, slots);
    free(mc);
    return n;
}

static int name_on_card(const uint8_t *mc, const char *name)
{
    for (int i = 1; i < 16; i++) {
        const uint8_t *fr = mc + i * 128;
        if (frame_first(fr) && strncmp((const char *)fr + 10, name, 20) == 0) return i;
    }
    return 0;
}

/* Put `blocks` (count blocks of 8 KB) on the card as one save named `name`,
 * in free slots. 1 = ok. */
static int place_save(uint8_t *mc, const char *name, const uint8_t *blocks, int count)
{
    int slots[15];
    if (free_slots(mc, slots) < count || count < 1) return 0;
    for (int k = 0; k < count; k++) {
        uint8_t *fr = mc + slots[k] * 128;
        memset(fr, 0, 128);
        const uint32_t state = k == 0 ? 0x51 : (k == count - 1 ? 0x53 : 0x52);
        fr[0] = (uint8_t)state;
        if (k == 0) {
            const uint32_t size = (uint32_t)count * MC_BLOCK;
            fr[4] = (uint8_t)size; fr[5] = (uint8_t)(size >> 8); fr[6] = (uint8_t)(size >> 16);
        }
        if (k + 1 < count) { fr[8] = (uint8_t)(slots[k + 1] - 1); fr[9] = 0; }
        else { fr[8] = fr[9] = 0xFF; }
        if (k == 0) memcpy(fr + 10, name, strlen(name) < 20 ? strlen(name) : 20);
        frame_seal(fr);
        memcpy(mc + slots[k] * MC_BLOCK, blocks + (size_t)k * MC_BLOCK, MC_BLOCK);
    }
    return 1;
}

int moa_memcard_copy_save(const char *src, int slot, const char *dst, char *err, size_t n)
{
    set_err(err, n, "");
    uint8_t *a = (uint8_t *)malloc(MC_SIZE), *b = (uint8_t *)malloc(MC_SIZE),
            *old = (uint8_t *)malloc(MC_SIZE), *blocks = (uint8_t *)malloc(15 * MC_BLOCK);
    int ok = 0;
    if (!a || !b || !old || !blocks) goto done;
    if (!read_image(src, a)) { set_err(err, n, "the card to copy from could not be read"); goto done; }
    if (!read_image(dst, b)) { set_err(err, n, "the card to copy to could not be read"); goto done; }
    memcpy(old, b, MC_SIZE);
    int chain[15];
    const int count = save_chain(a, slot, chain);
    if (!count) { set_err(err, n, "that save could not be found"); goto done; }
    for (int k = 0; k < count; k++)
        memcpy(blocks + (size_t)k * MC_BLOCK, a + chain[k] * MC_BLOCK, MC_BLOCK);
    char name[21];
    memcpy(name, a + slot * 128 + 10, 20);
    name[20] = '\0';
    if (name_on_card(b, name)) {
        /* A Master of Arena save takes the next free letter, as the game's own
         * new saves do; anything else would overwrite the copy there. */
        if (strncmp(name, "BASLUS-01030", 12) != 0 || strlen(name) != 13) {
            set_err(err, n, "that save is already on the other card");
            goto done;
        }
        int letter = -1;
        for (int c = 0; c < 24 && letter < 0; c++) {   /* Y and Z stay for QUICK START */
            name[12] = (char)('A' + c);
            if (!name_on_card(b, name)) letter = c;
        }
        if (letter < 0) { set_err(err, n, "the other card has no free save name"); goto done; }
    }
    int slots[15];
    if (free_slots(b, slots) < count) {
        snprintf(err, n, "the other card is full (this save needs %d block%s)", count, count == 1 ? "" : "s");
        goto done;
    }
    place_save(b, name, blocks, count);
    ok = write_with_backup(dst, old, b, err, n);
done:
    free(a); free(b); free(old); free(blocks);
    return ok;
}

int moa_memcard_delete_save(const char *path, int slot, char *err, size_t n)
{
    set_err(err, n, "");
    uint8_t *mc = (uint8_t *)malloc(MC_SIZE), *old = (uint8_t *)malloc(MC_SIZE);
    int ok = 0;
    if (!mc || !old) goto done;
    if (!read_image(path, mc)) { set_err(err, n, "the memory card could not be read"); goto done; }
    memcpy(old, mc, MC_SIZE);
    int chain[15];
    const int count = save_chain(mc, slot, chain);
    if (!count) { set_err(err, n, "that save could not be found"); goto done; }
    for (int k = 0; k < count; k++) {
        frame_free(mc + chain[k] * 128);
        memset(mc + chain[k] * MC_BLOCK, 0, MC_BLOCK);
    }
    ok = write_with_backup(path, old, mc, err, n);
done:
    free(mc); free(old);
    return ok;
}

int moa_memcard_create(const char *path, char *err, size_t n)
{
    set_err(err, n, "");
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); set_err(err, n, "a card with that name is already there"); return 0; }
    uint8_t *mc = (uint8_t *)malloc(MC_SIZE);
    if (!mc) return 0;
    format_image(mc);
    const int ok = write_with_backup(path, NULL, mc, err, n);
    free(mc);
    return ok;
}

/* ---- QUICK START ------------------------------------------------------------ */

static void quickstart_block(int human_plus, uint8_t block[MOA_SAVE_BLOCK])
{
    struct moa_card c;
    moa_card_new(&c);
    snprintf(c.pilot, sizeof c.pilot, "PLAYER");
    snprintf(c.ac, sizeof c.ac, "%s", human_plus ? "QUICK HP" : "QUICK");
    c.plus = human_plus ? 9 : 0;
    moa_card_to_save_full(&c, block);
}

static int quickstart_put_missing(uint8_t *mc, int *restored)
{
    static const struct { const char *name; int hp; } k[2] = {
        { MOA_LINK_QS_FILE, 0 }, { MOA_LINK_QS_HP_FILE, 1 } };
    uint8_t *block = (uint8_t *)malloc(MOA_SAVE_BLOCK);
    if (!block) return 0;
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        if (name_on_card(mc, k[i].name)) continue;
        quickstart_block(k[i].hp, block);
        if (!place_save(mc, k[i].name, block, 1)) { ok = 0; break; }
        if (restored) (*restored)++;
    }
    free(block);
    return ok;
}

int moa_link_quickstart_ensure(const char *path, int *restored, char *err, size_t n)
{
    set_err(err, n, "");
    if (restored) *restored = 0;
    uint8_t *mc = (uint8_t *)malloc(MC_SIZE), *old = (uint8_t *)malloc(MC_SIZE);
    int ok = 0;
    if (!mc || !old) goto done;
    int had = 0;
    {
        FILE *f = fopen(path, "rb");
        if (f) {
            had = 1;
            const size_t got = fread(old, 1, MC_SIZE, f);
            fclose(f);
            if (got != MC_SIZE) memset(old + got, 0, MC_SIZE - got);
        }
    }
    const int readable = had && old[0] == 'M' && old[1] == 'C';
    if (readable) memcpy(mc, old, MC_SIZE);
    else format_image(mc);
    int put = 0;
    if (!quickstart_put_missing(mc, &put)) {
        set_err(err, n, "the QUICK START card is full: delete a save or use RESET");
        goto done;
    }
    if (restored) *restored = put;
    if (!put && readable) { ok = 1; goto done; }     /* nothing to do */
    ok = write_with_backup(path, had ? old : NULL, mc, err, n);
done:
    free(mc); free(old);
    return ok;
}

int moa_link_quickstart_reset(const char *path, char *err, size_t n)
{
    set_err(err, n, "");
    uint8_t *mc = (uint8_t *)malloc(MC_SIZE), *old = (uint8_t *)malloc(MC_SIZE);
    int ok = 0;
    if (!mc || !old) goto done;
    const int had = read_image(path, old);
    format_image(mc);
    if (!quickstart_put_missing(mc, NULL)) goto done;
    ok = write_with_backup(path, had ? old : NULL, mc, err, n);
done:
    free(mc); free(old);
    return ok;
}
