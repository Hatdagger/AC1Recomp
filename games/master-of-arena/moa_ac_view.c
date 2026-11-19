/* Master of Arena: the launcher's 3D AC preview. See moa_ac_view.h. */
#include "moa_ac_view.h"
#include "moa_parts.h"

#include "acr/acr_import.h"
#include "acr/acr_model.h"
#include "acr/acr_png.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#endif

/* ---- files ---- */

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    const long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *t = (char *)malloc((size_t)n + 1);
    if (t && fread(t, 1, (size_t)n, f) != (size_t)n) { free(t); t = NULL; }
    if (t) t[n] = '\0';
    fclose(f);
    return t;
}

static uint8_t *read_bytes(const char *path, size_t *size) {
    char *t = read_text(path);
    if (!t) return NULL;
    FILE *f = fopen(path, "rb");
    long n = 0;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); }
    *size = n > 0 ? (size_t)n : 0;
    return (uint8_t *)t;
}

static int is_dir(const char *path) {
#ifdef _WIN32
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int moa_acv_import(const char *disc_path, const char *work_dir, char *err, size_t err_size) {
    acr_import_stats stats;
    if (err && err_size) err[0] = '\0';
    if (!acr_import_disc_models(disc_path, work_dir, &stats, err, err_size)) return 0;
    if (!stats.model_parts || !stats.model_sets || !stats.parts) {
        if (err && err_size) snprintf(err, err_size, "no AC models found: is this Master of Arena disc 1?");
        return 0;
    }
    return 1;
}

int moa_acv_installed(const char *dir) {
    char p[1100];
    snprintf(p, sizeof p, "%s/models/sets", dir);
    if (!is_dir(p)) return 0;
    snprintf(p, sizeof p, "%s/motion/models.toml", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    snprintf(p, sizeof p, "%s/parts", dir);
    return is_dir(p);
}

/* ---- tiny TOML reading (the importer's own output) ---- */

static const char *find_key(const char *b, const char *e, const char *key) {
    const size_t n = strlen(key);
    for (const char *p = b; p && p < e; ) {
        if ((p == b || p[-1] == '\n') && strncmp(p, key, n) == 0) {
            const char *q = p + n;
            while (q < e && (*q == ' ' || *q == '\t')) ++q;
            if (q < e && *q == '=') return q + 1;
        }
        p = strchr(p, '\n');
        if (p) ++p;
    }
    return NULL;
}

static long get_int(const char *b, const char *e, const char *key, long fallback) {
    const char *v = find_key(b, e, key);
    return v ? strtol(v, NULL, 10) : fallback;
}

static int list_has(const char *b, const char *e, const char *key, long want) {
    const char *v = find_key(b, e, key);
    if (!v) return 0;
    const char *open = strchr(v, '['), *nl = strchr(v, '\n');
    if (!open || (nl && open > nl)) return strtol(v, NULL, 10) == want;
    for (const char *p = open + 1; *p && *p != ']'; ) {
        char *end;
        const long x = strtol(p, &end, 10);
        if (end == p) { ++p; continue; }
        if (x == want) return 1;
        p = end;
    }
    return 0;
}

/* Same rule as the game (and Armored-CoreR's pick_model): cores 3 and 4 use
 * core 0's sets. */
static int pick_set(const char *dir, int core, int arms, int legs) {
    char path[1100];
    snprintf(path, sizeof path, "%s/motion/models.toml", dir);
    char *t = read_text(path);
    if (!t) return -1;
    if (core == 3 || core == 4) core = 0;
    int found = -1;
    for (char *b = strstr(t, "[[model]]"); b && found < 0; ) {
        char *next = strstr(b + 9, "[[model]]");
        const char *e = next ? next : b + strlen(b);
        if (get_int(b, e, "core", -1) == core && list_has(b, e, "arms", arms) && list_has(b, e, "legs", legs))
            found = (int)get_int(b, e, "id", -1);
        b = next;
    }
    free(t);
    return found;
}

/* ---- the AC ---- */

typedef struct acv_tex { int w, h; uint8_t *rgba; } acv_tex;
typedef struct acv_part {
    int present, pieces;
    acr_mtl mtl;
    acr_mesh piece[12];
    int has_joint[12];
    int joint[12][3];          /* part.toml: the bind position the export took off */
    acv_tex tex;
} acv_part;

#define ACV_SOLO_MAX 8
struct moa_acv {
    acr_skeleton skel;
    acr_anim_set anims;
    acv_part part[ACR_PART_KINDS];
    float radius, centre_y;          /* for framing, from the bind pose */
    /* a part on its own (the garage model): no skeleton, one mesh an object */
    int solo, solo_count;
    acr_mtl solo_mtl;
    acr_mesh solo_mesh[ACV_SOLO_MAX];
    acv_tex solo_tex[ACR_MTL_MAX];
    /* The emblem plate's picture (moa_acv_set_emblem); none = plate not drawn. */
    acv_tex emblem;
};

/* The emblem plate: faces with the "fx" material (left shoulder on most arms,
 * both sides of the two-slot back units 52 and 54). Their UVs cover the
 * 64x64 square at (128, 128) of a 256 page, where the game puts the AC's
 * emblem; the export has no picture for it, so without this they came out
 * as plain white boxes. */
static int is_emblem_plate(const acr_material *m) {
    return m->name[0] == 'f' && m->name[1] == 'x' && (m->name[2] == 0 || m->name[2] == '-');
}

void moa_acv_set_emblem(moa_acv *v, const uint16_t palette[16], const uint8_t index[64 * 64]) {
    if (!v) return;
    free(v->emblem.rgba);
    v->emblem.rgba = NULL;
    v->emblem.w = v->emblem.h = 0;
    if (!palette || !index) return;
    uint8_t *px = (uint8_t *)malloc(64 * 64 * 4);
    if (!px) return;
    for (int i = 0; i < 64 * 64; ++i) {
        const uint16_t c = palette[index[i] & 15];
        const int r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
        px[4 * i + 0] = (uint8_t)(r | r >> 5);
        px[4 * i + 1] = (uint8_t)(g | g >> 5);
        px[4 * i + 2] = (uint8_t)(b | b >> 5);
        px[4 * i + 3] = c == 0 ? 0 : 255;          /* 0x0000 is see-through, as on the PlayStation */
    }
    v->emblem.w = v->emblem.h = 64;
    v->emblem.rgba = px;
}

static int patterned(int kind) {
    return kind == ACR_PART_CORE || kind == ACR_PART_HEAD || kind == ACR_PART_ARMS || kind == ACR_PART_LEGS;
}

/* Card paint section for a patterned part kind: head, core, arms, legs. */
static int paint_section(int kind) {
    switch (kind) {
    case ACR_PART_HEAD: return 0;
    case ACR_PART_CORE: return 1;
    case ACR_PART_ARMS: return 2;
    default: return 3;
    }
}

static void load_texture(const char *path, int kind, const uint8_t *paint, acv_tex *out) {
    size_t size = 0;
    uint8_t *data = read_bytes(path, &size);
    if (!data) return;
    acr_image img;
    const int ok = acr_png_decode(data, size, &img);
    free(data);
    if (!ok) return;
    out->rgba = (uint8_t *)malloc((size_t)img.w * img.h * 4);
    if (out->rgba) {
        out->w = img.w;
        out->h = img.h;
        if (img.indexed) {
            uint8_t pal[256][4];
            memcpy(pal, img.palette, sizeof pal);
            if (paint && patterned(kind)) {
                uint8_t c[4][3];
                const uint8_t *s = paint + 12 * paint_section(kind);
                for (int i = 0; i < 4; ++i)
                    for (int k = 0; k < 3; ++k) c[i][k] = (uint8_t)(s[3 * i + k] > 63 ? 63 : s[3 * i + k]);
                acr_colour_ramp(c, pal);
            }
            for (int i = 0; i < img.w * img.h; ++i) memcpy(out->rgba + 4 * i, pal[img.pixels[i]], 4);
        } else {
            memcpy(out->rgba, img.pixels, (size_t)img.w * img.h * 4);
        }
    }
    acr_image_free(&img);
}

static int load_part(const char *models, int kind, int index, const uint8_t *paint, acv_part *p) {
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%s/%s/%02d", models, acr_part_kind_name(kind), index);
    snprintf(path, sizeof path, "%s/part.toml", dir);
    char *toml = read_text(path);
    if (!toml) return 0;
    const int pieces = (int)get_int(toml, toml + strlen(toml), "pieces", 0);
    if (pieces <= 0) { free(toml); return 0; }
    /* Each piece's joint (the bind position its vertices were moved by, in
       the set it was exported from). */
    {
        int k = 0;
        for (const char *b = strstr(toml, "[[piece]]"); b && k < 12; ++k) {
            const char *next = strstr(b + 9, "[[piece]]");
            const char *e = next ? next : b + strlen(b);
            const char *v = find_key(b, e, "joint");
            if (v && (v = strchr(v, '[')) && v < e) {
                char *q = (char *)v + 1;
                for (int c = 0; c < 3; ++c) { p->joint[k][c] = (int)strtol(q, &q, 10); if (*q == ',') ++q; }
                p->has_joint[k] = 1;
            }
            b = next;
        }
    }
    free(toml);
    snprintf(path, sizeof path, "%s/part.mtl", dir);
    char *mtl = read_text(path);
    if (!mtl || !acr_mtl_parse(mtl, &p->mtl)) { free(mtl); return 0; }
    free(mtl);
    p->pieces = pieces > 12 ? 12 : pieces;
    for (int k = 0; k < p->pieces; ++k) {
        snprintf(path, sizeof path, "%s/piece%d.obj", dir, k);
        char *obj = read_text(path);
        if (obj) acr_obj_parse(obj, &p->mtl, &p->piece[k]);
        free(obj);
    }
    snprintf(path, sizeof path, "%s/texture.png", dir);
    load_texture(path, kind, paint, &p->tex);
    p->present = 1;
    return 1;
}

void moa_acv_free(moa_acv *v) {
    if (!v) return;
    for (int i = 0; i < v->solo_count; ++i) acr_mesh_free(&v->solo_mesh[i]);
    for (int i = 0; i < ACR_MTL_MAX; ++i) free(v->solo_tex[i].rgba);
    free(v->emblem.rgba);
    for (int k = 0; k < ACR_PART_KINDS; ++k) {
        for (int i = 0; i < v->part[k].pieces; ++i) acr_mesh_free(&v->part[k].piece[i]);
        free(v->part[k].tex.rgba);
    }
    acr_anim_set_free(&v->anims);
    free(v);
}

static void frame_model(moa_acv *v);

/* The AC itself, framing not set. */
static moa_acv *load_ac(const char *dir, const uint8_t rec[13], const uint8_t paint[48]) {
    if (rec[0] == 255 || rec[2] == 255 || rec[3] == 255) return NULL;
    const int set = pick_set(dir, rec[0], rec[2], rec[3]);
    if (set < 0) return NULL;
    moa_acv *v = (moa_acv *)calloc(1, sizeof *v);
    if (!v) return NULL;
    char models[1100], path[1200];
    snprintf(models, sizeof models, "%s/models", dir);
    snprintf(path, sizeof path, "%s/sets/set-%02d.toml", models, set);
    char *t = read_text(path);
    const int skel_ok = t && acr_skeleton_parse(t, &v->skel);
    free(t);
    if (!skel_ok) { free(v); return NULL; }
    snprintf(path, sizeof path, "%s/sets/set-%02d-anims.txt", models, set);
    t = read_text(path);
    if (t) acr_anim_set_parse(t, &v->skel, &v->anims);   /* without it: the bind pose */
    free(t);
    const int weapon_arms = rec[2] >= 12;
    /* Two-slot back units (52 and up) sit in one back byte but are drawn on both sides. */
    int back_l = rec[6] == 255 ? -1 : rec[6], back_r = rec[7] == 255 ? -1 : rec[7];
    if (back_l >= 52) back_r = back_l;
    else if (back_r >= 52) back_l = back_r;
    const int index[ACR_PART_KINDS] = {
        [ACR_PART_CORE] = rec[0], [ACR_PART_HEAD] = rec[1], [ACR_PART_ARMS] = rec[2], [ACR_PART_LEGS] = rec[3],
        [ACR_PART_HAND] = weapon_arms ? -1 : (rec[4] == 255 ? 0 : rec[4]),   /* 0: the empty hand */
        [ACR_PART_BACK_LEFT] = back_l,
        [ACR_PART_BACK_RIGHT] = back_r,
        [ACR_PART_BOOSTER] = rec[12] == 255 ? -1 : rec[12],
    };
    for (int kind = 0; kind < ACR_PART_KINDS; ++kind)
        if (index[kind] >= 0) load_part(models, kind, index[kind], paint, &v->part[kind]);
    if (!v->part[ACR_PART_CORE].present || !v->part[ACR_PART_LEGS].present) { moa_acv_free(v); return NULL; }
    return v;
}

/* Like the game's garage camera, the framing never changes with the parts:
 * it is measured once per install on a plain mid-size AC (the builder's
 * default, unarmed) with room around it, so a tall head (HD-12-RADAR) or a
 * big back weapon sticks out further instead of shrinking and moving the
 * whole AC. */
static char  s_frame_dir[1100];
static float s_frame_radius, s_frame_centre_y;

static int fixed_frame(const char *dir) {
    if (s_frame_dir[0] && strcmp(s_frame_dir, dir) == 0) return 1;
    static const uint8_t ref[13] = { 0, 6, 0, 8, 255, 255, 255, 255, 255, 255, 0, 0, 0 };
    moa_acv *r = load_ac(dir, ref, NULL);
    if (!r) return 0;
    frame_model(r);
    s_frame_radius = r->radius * 1.08f;        /* room for taller parts */
    s_frame_centre_y = r->centre_y;
    moa_acv_free(r);
    snprintf(s_frame_dir, sizeof s_frame_dir, "%s", dir);
    return 1;
}

static moa_acv *acv_load_unlocked(const char *dir, const uint8_t rec[13], const uint8_t paint[48]) {
    moa_acv *v = load_ac(dir, rec, paint);
    if (!v) return NULL;
    if (fixed_frame(dir)) {
        v->radius = s_frame_radius;
        v->centre_y = s_frame_centre_y;
    } else {
        frame_model(v);                        /* no reference AC: fit this one */
    }
    return v;
}

/* ---- a part on its own ---- */

int moa_acv_has_part_models(const char *dir) {
    char p[1100];
    snprintf(p, sizeof p, "%s/models/menu", dir);
    return is_dir(p);
}

static int kind_of_folder(const char *kind) {
    if (!strcmp(kind, "head")) return ACR_PART_HEAD;
    if (!strcmp(kind, "core")) return ACR_PART_CORE;
    if (!strcmp(kind, "arms")) return ACR_PART_ARMS;
    if (!strcmp(kind, "legs")) return ACR_PART_LEGS;
    return ACR_PART_NONE;
}

static moa_acv *acv_load_part_unlocked(const char *dir, const char *kind, int index, const uint8_t paint[48]) {
    char base[1100], path[1300];
    snprintf(base, sizeof base, "%s/models/menu/%s/%02d", dir, kind, index);
    snprintf(path, sizeof path, "%s/part.toml", base);
    char *toml = read_text(path);
    if (!toml) return NULL;
    const int objects = (int)get_int(toml, toml + strlen(toml), "objects", 1);
    free(toml);
    moa_acv *v = (moa_acv *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->solo = 1;
    snprintf(path, sizeof path, "%s/part.mtl", base);
    char *mtl = read_text(path);
    if (!mtl || !acr_mtl_parse(mtl, &v->solo_mtl)) { free(mtl); free(v); return NULL; }
    free(mtl);
    const int pk = kind_of_folder(kind);
    for (int i = 0; i < v->solo_mtl.count; ++i) {
        acr_material *m = &v->solo_mtl.m[i];
        if (!m->textured || !m->texture[0]) continue;
        snprintf(path, sizeof path, "%s/%s", base, m->texture);
        /* only the part's own texture.png carries the AC's colours */
        const int own = strcmp(m->texture, "texture.png") == 0;
        load_texture(path, own ? pk : ACR_PART_NONE, own ? paint : NULL, &v->solo_tex[i]);
    }
    /* Which objects the garage shows: boosters come as 3, 1 and 5 nozzle
       blocks (one is shown), arms as right and left (upper + lower), back
       units as left and right copies. */
    int show[ACV_SOLO_MAX], n_show = 0;
    if (!strcmp(kind, "booster") && objects == 3) show[n_show++] = 1;
    else if (!strcmp(kind, "arms") && objects == 4) show[n_show++] = 0;   /* 2 is the same arm again */
    else if ((!strcmp(kind, "arms") || !strcmp(kind, "back")) && objects == 2) show[n_show++] = 0;
    else for (int k = 0; k < objects && n_show < ACV_SOLO_MAX; ++k) show[n_show++] = k;
    for (int i = 0; i < n_show && v->solo_count < ACV_SOLO_MAX; ++i) {
        const int k = show[i];
        snprintf(path, sizeof path, "%s/object%d.obj", base, k);
        char *obj = read_text(path);
        if (obj && acr_obj_parse(obj, &v->solo_mtl, &v->solo_mesh[v->solo_count])) v->solo_count++;
        free(obj);
    }
    if (!v->solo_count) { moa_acv_free(v); return NULL; }
    /* turn about the part's own middle */
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (int k = 0; k < v->solo_count; ++k)
        for (int t = 0; t < v->solo_mesh[k].tri_count * 3; ++t)
            for (int c = 0; c < 3; ++c) {
                const float q = v->solo_mesh[k].v[t].p[c];
                if (q < lo[c]) lo[c] = q;
                if (q > hi[c]) hi[c] = q;
            }
    for (int k = 0; k < v->solo_count; ++k)
        for (int t = 0; t < v->solo_mesh[k].tri_count * 3; ++t) {
            v->solo_mesh[k].v[t].p[0] -= 0.5f * (lo[0] + hi[0]);
            v->solo_mesh[k].v[t].p[2] -= 0.5f * (lo[2] + hi[2]);
        }
    frame_model(v);
    return v;
}

/* Loading may run on two threads at once (the VS screen's renders run on
 * moa_render_queue's worker while the game's thread draws part pictures):
 * the shared framing above and the PNG decoder's tables are built on first
 * use, so loads take turns. Renders need no lock. */
#if defined(_WIN32)
static SRWLOCK s_load_lock = SRWLOCK_INIT;
static void load_lock(void) { AcquireSRWLockExclusive(&s_load_lock); }
static void load_unlock(void) { ReleaseSRWLockExclusive(&s_load_lock); }
#else
static pthread_mutex_t s_load_lock = PTHREAD_MUTEX_INITIALIZER;
static void load_lock(void) { pthread_mutex_lock(&s_load_lock); }
static void load_unlock(void) { pthread_mutex_unlock(&s_load_lock); }
#endif

moa_acv *moa_acv_load(const char *dir, const uint8_t rec[13], const uint8_t paint[48]) {
    load_lock();
    moa_acv *v = acv_load_unlocked(dir, rec, paint);
    load_unlock();
    return v;
}

moa_acv *moa_acv_load_part(const char *dir, const char *kind, int index, const uint8_t paint[48]) {
    load_lock();
    moa_acv *v = acv_load_part_unlocked(dir, kind, index, paint);
    load_unlock();
    return v;
}

/* ---- posing ---- */

typedef struct acv_vtx { float x, y, z, nx, ny, nz, u, v; } acv_vtx;

/* Calls `fn` for every triangle of the AC posed at `time`, in model space
 * (x right, y up, z towards the viewer at yaw 0). */
typedef void (*tri_fn)(void *ctx, const acv_vtx t[3], const acv_tex *tex, const uint8_t rgb[3], int kind);

static void solo_tris(const moa_acv *v, tri_fn fn, void *ctx) {
    for (int k = 0; k < v->solo_count; ++k) {
        const acr_mesh *mesh = &v->solo_mesh[k];
        for (int t = 0; t < mesh->tri_count; ++t) {
            const int mi = mesh->material[t];
            const acr_material *mt = &v->solo_mtl.m[mi];
            const acv_tex *tex = mt->textured && v->solo_tex[mi].rgba ? &v->solo_tex[mi] : NULL;
            const uint8_t rgb[3] = { tex ? 128 : mt->r, tex ? 128 : mt->g, tex ? 128 : mt->b };
            acv_vtx o[3];
            for (int j = 0; j < 3; ++j) {
                const acr_mesh_vertex *sv = &mesh->v[t * 3 + j];
                o[j].x = sv->p[0]; o[j].y = sv->p[1]; o[j].z = sv->p[2];
                o[j].nx = sv->n[0]; o[j].ny = sv->n[1]; o[j].nz = sv->n[2];
                o[j].u = sv->uv[0]; o[j].v = 1.0f - sv->uv[1];
            }
            fn(ctx, o, tex, rgb, ACR_PART_NONE);
        }
    }
}

static void for_each_tri(const moa_acv *v, float time, tri_fn fn, void *ctx) {
    if (v->solo) { solo_tris(v, fn, ctx); return; }
    acr_pose pose;
    if (v->anims.anim[0].frames) acr_pose_play_smooth(&v->skel, &v->anims, 0, time * 22.0f, 1, &pose);
    else acr_pose_bind(&v->skel, &pose);
    float mat[ACR_SKEL_MAX_NODES][12];
    acr_pose_matrices(&v->skel, &pose, mat);
    for (int i = 0; i < v->skel.count; ++i) {
        const acr_skel_node *node = &v->skel.node[i];
        if (node->draw_kind >= ACR_PART_KINDS) continue;
        const acv_part *p = &v->part[node->draw_kind];
        if (!p->present || node->draw_piece >= p->pieces) continue;
        const acr_mesh *mesh = &p->piece[node->draw_piece];
        const float *M = mat[i];
        /* The pose matrices take off this set's bind position for the node,
           but the vertices were moved by the bind position in the set the part
           was exported from. They differ where the hierarchy differs: weapon
           arm sets hang the hips (legs piece 0) under a root joint, so the
           hips floated above the head. Put back the difference (model space:
           y and z negated). */
        float d[3] = { 0, 0, 0 };
        if (p->has_joint[node->draw_piece]) {
            const int *jt = p->joint[node->draw_piece];
            d[0] = (float)(node->pos[0] + jt[0]);
            d[1] = -(float)(node->pos[1] + jt[1]);
            d[2] = -(float)(node->pos[2] + jt[2]);
        }
        for (int t = 0; t < mesh->tri_count; ++t) {
            const acr_material *mt = &p->mtl.m[mesh->material[t]];
            const acv_tex *tex = NULL;
            const int plate = is_emblem_plate(mt);
            if (plate) {
                if (!v->emblem.rgba) continue;         /* no emblem: nothing there */
                tex = &v->emblem;
            } else if (mt->textured) {
                const int kind = mt->kind < ACR_PART_KINDS ? mt->kind : node->draw_kind;
                tex = v->part[kind].tex.rgba ? &v->part[kind].tex
                    : (p->tex.rgba ? &p->tex : NULL);
            }
            const uint8_t rgb[3] = { tex ? 128 : mt->r, tex ? 128 : mt->g, tex ? 128 : mt->b };
            acv_vtx o[3];
            for (int j = 0; j < 3; ++j) {
                const acr_mesh_vertex *s0 = &mesh->v[t * 3 + j];
                acr_mesh_vertex sv = *s0;
                sv.p[0] += d[0]; sv.p[1] += d[1]; sv.p[2] += d[2];
                const acr_mesh_vertex *s = &sv;
                /* Model space is game space turned half a turn about x (a
                   rotation, not a mirror), and the camera below projects it
                   the way the game does: no flip, or the rifle ends up in the
                   left hand and the emblem on the wrong shoulder. */
                o[j].x = M[0] * s->p[0] + M[1] * s->p[1] + M[2] * s->p[2] + M[3];
                o[j].y = M[4] * s->p[0] + M[5] * s->p[1] + M[6] * s->p[2] + M[7];
                o[j].z = M[8] * s->p[0] + M[9] * s->p[1] + M[10] * s->p[2] + M[11];
                o[j].nx = M[0] * s->n[0] + M[1] * s->n[1] + M[2] * s->n[2];
                o[j].ny = M[4] * s->n[0] + M[5] * s->n[1] + M[6] * s->n[2];
                o[j].nz = M[8] * s->n[0] + M[9] * s->n[1] + M[10] * s->n[2];
                o[j].u = s->uv[0];
                o[j].v = 1.0f - s->uv[1];
                if (plate) {                           /* the page's emblem square -> the whole emblem */
                    o[j].u = (o[j].u - 0.5f) * 4.0f;
                    o[j].v = (o[j].v - 0.5f) * 4.0f;
                }
            }
            fn(ctx, o, tex, rgb, node->draw_kind);
        }
    }
}

typedef struct bounds { float lo[3], hi[3]; int any; } bounds;
static void grow(void *ctx, const acv_vtx t[3], const acv_tex *tex, const uint8_t rgb[3], int kind) {
    (void)tex; (void)rgb; (void)kind;
    bounds *b = (bounds *)ctx;
    for (int j = 0; j < 3; ++j) {
        const float p[3] = { t[j].x, t[j].y, t[j].z };
        for (int k = 0; k < 3; ++k) {
            if (!b->any || p[k] < b->lo[k]) b->lo[k] = p[k];
            if (!b->any || p[k] > b->hi[k]) b->hi[k] = p[k];
        }
        b->any = 1;
    }
}

static void frame_model(moa_acv *v) {
    bounds b;
    memset(&b, 0, sizeof b);
    for_each_tri(v, 0.0f, grow, &b);
    if (!b.any) { v->radius = 1.0f; return; }
    v->centre_y = 0.5f * (b.lo[1] + b.hi[1]);
    float r = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float c = k == 1 ? v->centre_y : 0.0f;
        const float a = fabsf(b.lo[k] - c), z = fabsf(b.hi[k] - c);
        if (a > r) r = a;
        if (z > r) r = z;
    }
    v->radius = r > 1.0f ? r : 1.0f;
}

/* ---- the rasteriser ---- */

typedef struct raster {
    int w, h;
    uint8_t *rgba;
    float *depth;
    float cy, sy, cp, sp, centre_y, scale, dist;
    unsigned wire_mask;              /* part kinds drawn as wireframe (bit per kind) */
    uint8_t wire_rgb[3];
    float *lines;                    /* screen-space edges, 6 floats each */
    size_t n_lines, cap_lines;
    int pass;                        /* 0: fill, 1: collect wire edges */
} raster;

typedef struct svtx { float x, y, z, u, v, light; } svtx;

static void put_tri(raster *r, const svtx s[3], const acv_tex *tex, const uint8_t rgb[3]) {
    const float area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
    if (fabsf(area) < 1e-6f) return;
    float x0 = fminf(s[0].x, fminf(s[1].x, s[2].x)), x1 = fmaxf(s[0].x, fmaxf(s[1].x, s[2].x));
    float y0 = fminf(s[0].y, fminf(s[1].y, s[2].y)), y1 = fmaxf(s[0].y, fmaxf(s[1].y, s[2].y));
    int ix0 = (int)floorf(x0), ix1 = (int)ceilf(x1), iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > r->w - 1) ix1 = r->w - 1;
    if (iy1 > r->h - 1) iy1 = r->h - 1;
    const float inv = 1.0f / area;
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = ((s[1].x - px) * (s[2].y - py) - (s[2].x - px) * (s[1].y - py)) * inv;
            const float w1 = ((s[2].x - px) * (s[0].y - py) - (s[0].x - px) * (s[2].y - py)) * inv;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float z = w0 * s[0].z + w1 * s[1].z + w2 * s[2].z;
            float *d = &r->depth[y * r->w + x];
            if (z >= *d) continue;
            float c[3] = { rgb[0], rgb[1], rgb[2] };
            if (tex) {
                /* affine, as the PlayStation draws it */
                float u = w0 * s[0].u + w1 * s[1].u + w2 * s[2].u;
                float vv = w0 * s[0].v + w1 * s[1].v + w2 * s[2].v;
                int tx = (int)floorf(u * tex->w), ty = (int)floorf(vv * tex->h);
                tx = tx < 0 ? 0 : tx >= tex->w ? tex->w - 1 : tx;
                ty = ty < 0 ? 0 : ty >= tex->h ? tex->h - 1 : ty;
                const uint8_t *t = tex->rgba + 4 * (ty * tex->w + tx);
                if (t[3] == 0) continue;                      /* see-through texel */
                /* texel modulated by the vertex colour, 128 = 1.0 */
                for (int k = 0; k < 3; ++k) c[k] = t[k] * rgb[k] / 128.0f;
            }
            const float l = w0 * s[0].light + w1 * s[1].light + w2 * s[2].light;
            uint8_t *o = &r->rgba[4 * (y * r->w + x)];
            for (int k = 0; k < 3; ++k) {
                const float q = c[k] * l;
                o[k] = (uint8_t)(q > 255.0f ? 255 : q);
            }
            o[3] = 255;
            *d = z;
        }
}

static void add_line(raster *r, const svtx *a, const svtx *b) {
    /* each edge once: quads come as two triangles sharing a diagonal, and
       neighbours share edges */
    for (size_t i = 0; i < r->n_lines; ++i) {
        const float *l = r->lines + 6 * i;
        if ((fabsf(l[0] - a->x) < 0.01f && fabsf(l[1] - a->y) < 0.01f && fabsf(l[3] - b->x) < 0.01f &&
             fabsf(l[4] - b->y) < 0.01f) ||
            (fabsf(l[0] - b->x) < 0.01f && fabsf(l[1] - b->y) < 0.01f && fabsf(l[3] - a->x) < 0.01f &&
             fabsf(l[4] - a->y) < 0.01f))
            return;
    }
    if (r->n_lines == r->cap_lines) {
        const size_t cap = r->cap_lines ? r->cap_lines * 2 : 512;
        float *nl = (float *)realloc(r->lines, cap * 6 * sizeof(float));
        if (!nl) return;
        r->lines = nl;
        r->cap_lines = cap;
    }
    float *l = r->lines + 6 * r->n_lines++;
    l[0] = a->x; l[1] = a->y; l[2] = a->z; l[3] = b->x; l[4] = b->y; l[5] = b->z;
}

/* Lines over the filled AC: hidden where another part is in front. */
static void draw_lines(raster *r) {
    for (size_t i = 0; i < r->n_lines; ++i) {
        const float *l = r->lines + 6 * i;
        const float dx = l[3] - l[0], dy = l[4] - l[1];
        const int steps = (int)(fmaxf(fabsf(dx), fabsf(dy))) + 1;
        for (int k = 0; k <= steps; ++k) {
            const float t = (float)k / (float)steps;
            const int x = (int)floorf(l[0] + dx * t), y = (int)floorf(l[1] + dy * t);
            if (x < 0 || y < 0 || x >= r->w || y >= r->h) continue;
            const float z = l[2] + (l[5] - l[2]) * t;
            if (z > r->depth[y * r->w + x] + 2.0f) continue;
            uint8_t *o = &r->rgba[4 * (y * r->w + x)];
            o[0] = r->wire_rgb[0]; o[1] = r->wire_rgb[1]; o[2] = r->wire_rgb[2]; o[3] = 255;
        }
    }
}

static void draw_tri(void *ctx, const acv_vtx t[3], const acv_tex *tex, const uint8_t rgb[3], int kind) {
    raster *r = (raster *)ctx;
    const int wire = kind < 32 && ((r->wire_mask >> kind) & 1u);
    if (wire != r->pass) return;
    svtx s[3];
    for (int j = 0; j < 3; ++j) {
        /* turn by yaw about y, then tilt by pitch about x */
        const float x = t[j].x, y = t[j].y - r->centre_y, z = t[j].z;
        const float x1 = x * r->cy + z * r->sy, z1 = -x * r->sy + z * r->cy;
        const float y2 = y * r->cp - z1 * r->sp, z2 = y * r->sp + z1 * r->cp;
        const float nx = t[j].nx * r->cy + t[j].nz * r->sy, nz1 = -t[j].nx * r->sy + t[j].nz * r->cy;
        const float ny = t[j].ny * r->cp - nz1 * r->sp, nz = t[j].ny * r->sp + nz1 * r->cp;
        /* camera on +z looking back at the AC */
        const float depth = r->dist - z2;
        if (depth < 1.0f) return;
        const float f = r->scale / depth;
        s[j].x = 0.5f * r->w + x1 * f;
        s[j].y = 0.5f * r->h - y2 * f;
        s[j].z = depth;
        s[j].u = t[j].u;
        s[j].v = t[j].v;
        /* key light from the upper left front, plus ambient */
        const float len = sqrtf(nx * nx + ny * ny + nz * nz);
        const float d = len > 0 ? (nx * -0.45f + ny * 0.6f + nz * 0.66f) / len : 0.0f;
        s[j].light = 0.6f + 0.6f * (d > 0 ? d : 0.0f);
    }
    if (wire) {
        for (int j = 0; j < 3; ++j) add_line(r, &s[j], &s[(j + 1) % 3]);
        return;
    }
    put_tri(r, s, tex, rgb);
}

void moa_acv_render(const moa_acv *v, float yaw, float pitch, float time, int w, int h, uint8_t *rgba) {
    moa_acv_render_ex(v, yaw, pitch, time, 1.0f, 0, NULL, w, h, rgba);
}

void moa_acv_render_ex(const moa_acv *v, float yaw, float pitch, float time, float zoom,
                       unsigned wire_mask, const uint8_t wire_rgb[3], int w, int h, uint8_t *rgba) {
    memset(rgba, 0, (size_t)w * h * 4);
    if (!v || w <= 0 || h <= 0) return;
    raster r;
    memset(&r, 0, sizeof r);
    r.w = w; r.h = h; r.rgba = rgba;
    r.wire_mask = wire_mask;
    if (wire_rgb) memcpy(r.wire_rgb, wire_rgb, 3);
    r.depth = (float *)malloc(sizeof(float) * (size_t)w * h);
    if (!r.depth) return;
    for (int i = 0; i < w * h; ++i) r.depth[i] = 1e30f;
    r.cy = cosf(yaw); r.sy = sinf(yaw); r.cp = cosf(pitch); r.sp = sinf(pitch);
    r.centre_y = v->centre_y;
    r.dist = v->radius * 3.2f;
    /* the whole AC fits the shorter side with a little room */
    const float side = (float)(w < h ? w : h);
    r.scale = 0.46f * side * (r.dist - v->radius) / v->radius * (zoom > 0.1f ? zoom : 0.1f);
    for_each_tri(v, time, draw_tri, &r);
    if (wire_mask) {
        r.pass = 1;
        for_each_tri(v, time, draw_tri, &r);
        draw_lines(&r);
    }
    free(r.lines);
    free(r.depth);
}

/* ---- part details ---- */

static const char *const k_cat_dir[MOA_CAT__COUNT] = {
    "head", "core", "legs", "arms", "hand", "back", "blade", "booster", "fcs", "generator"
};

static void copy_string(const char *b, const char *e, const char *key, char *out, size_t n) {
    out[0] = '\0';
    const char *v = find_key(b, e, key);
    if (!v) return;
    const char *q = strchr(v, '"');
    const char *nl = strchr(v, '\n');
    if (!q || (nl && q > nl)) return;
    size_t i = 0;
    for (++q; *q && *q != '"' && i + 1 < n; ++q) out[i++] = *q;
    out[i] = '\0';
}

/* The garage's own details pages: its rows, in its order, with its words
 * (menu module 0x800D279C.. label lists, 0x800D2988.. value texts). Numbers
 * come from the part record (raw words from byte 36), so installs made
 * before these fields were named still show them; weapon rows come from the
 * weapon detail tables the importer writes out by name. */
static const char *const k_none_prov[2] = { "NONE", "PROVIDED" };
static const char *const k_computer[3] = { "STANDARD", "DETAILED", "ROUGH" };
static const char *const k_map[4] = { "AREA&PLACE NAME", "PLACE NAME DISP", "AREA MEMORY", "NO MEMORY" };
static const char *const k_radar_type[4] = { "STANDARD", "CIRCLE", "OCTAGON", "GRID" };
static const char *const k_weapon_lock[5] = { "WIDE & SHALLOW", "STANDARD", "SPECIAL", "NARROW & DEEP", "NONE" };
static const char *const k_fcs_lock[5] = { "STANDARD", "SIDEWAY", "LENGTHWAY", "WIDE & SHALLOW", "NARROW & DEEP" };

typedef struct det_ctx {
    const char *b, *e;
    long raw[48];
    int n_raw;
    struct moa_acv_details *out;
} det_ctx;

/* Record byte `off` (36 and up) from the raw words. */
static long rec_u8(const det_ctx *c, int off) {
    const int w = (off - 36) / 2;
    if (off < 36 || w >= c->n_raw) return 0;
    return (off & 1) ? (c->raw[w] >> 8) & 255 : c->raw[w] & 255;
}
static long rec_u16(const det_ctx *c, int off) {
    const int w = (off - 36) / 2;
    return (off >= 36 && w < c->n_raw) ? c->raw[w] & 0xFFFF : 0;
}

static struct moa_acv_field *add_field(det_ctx *c, const char *key, const char *label, int better) {
    if (c->out->count >= MOA_ACV_FIELDS) return NULL;
    struct moa_acv_field *f = &c->out->f[c->out->count++];
    memset(f, 0, sizeof *f);
    snprintf(f->key, sizeof f->key, "%s", key);
    snprintf(f->label, sizeof f->label, "%s", label);
    f->better = better;
    return f;
}
static void num(det_ctx *c, const char *key, const char *label, long v, int better) {
    struct moa_acv_field *f = add_field(c, key, label, better);
    if (f) f->value = v;
}
static void word(det_ctx *c, const char *key, const char *label, long v, const char *const *names, int n) {
    struct moa_acv_field *f = add_field(c, key, label, 0);
    if (!f) return;
    f->value = v;
    f->is_text = 1;
    snprintf(f->text, sizeof f->text, "%s", (v >= 0 && v < n) ? names[v] : "");
}
static long named(const det_ctx *c, const char *key, long fallback) {
    return get_int(c->b, c->e, key, fallback);
}

static void weapon_rows(det_ctx *c) {
    if (!find_key(c->b, c->e, "attack")) return;
    word(c, "lock_class", "WEAPON LOCK", named(c, "lock_class", 4), k_weapon_lock, 5);
    num(c, "attack", "ATTACK POWER", named(c, "attack", 0), 1);
    num(c, "ammo", "NUMBER OF AMMO", named(c, "ammo", 0), 1);
    struct moa_acv_field *f = add_field(c, "ammo_type", "AMMO TYPE", 0);
    if (f) {
        const long fl = named(c, "ammo_flags", -1);
        const char *t = fl >= 0 ? ((fl & 0x10) ? "SOLID" : (fl & 0x20) ? "ENERGY" : "")
                                : (named(c, "energy_damage", 0) ? "ENERGY" : "SOLID");
        f->is_text = 1;
        snprintf(f->text, sizeof f->text, "%s", t);
    }
    num(c, "ammo_price", "AMMO PRICE", named(c, "ammo_price", 0), -1);
    num(c, "range", "RANGE", named(c, "range", 0), 1);
    num(c, "salvo", "MAXIMUM-LOCK", named(c, "salvo", 0), 1);
    num(c, "fire_interval", "RELOAD TIME", named(c, "fire_interval", 0), -1);
}

int moa_acv_parse_details(const char *text, int cat, struct moa_acv_details *out) {
    memset(out, 0, sizeof *out);
    det_ctx c;
    memset(&c, 0, sizeof c);
    c.b = text;
    c.e = text + strlen(text);
    c.out = out;
    copy_string(c.b, c.e, "name", out->name, sizeof out->name);
    copy_string(c.b, c.e, "type", out->type, sizeof out->type);
    if (!out->name[0]) return 0;
    const char *r = find_key(c.b, c.e, "raw");
    if (r && (r = strchr(r, '['))) {
        char *p = (char *)r + 1;
        while (c.n_raw < 48) {
            char *q;
            const long v = strtol(p, &q, 10);
            if (q == p) break;
            c.raw[c.n_raw++] = v;
            p = q;
            while (*p == ' ' || *p == ',') ++p;
            if (*p == ']') break;
        }
    }
    const int index = (int)named(&c, "game_index", 0);
    num(&c, "price", "PRICE", rec_u16(&c, 36) | rec_u16(&c, 38) << 16, 0);
    num(&c, "weight", "WEIGHT", rec_u16(&c, 40), -1);
    if (cat == MOA_CAT_GEN) {
        num(&c, "output", "ENERGY OUTPUT", rec_u16(&c, 42), 1);
        num(&c, "capacity", "MAXIMUM CHARGE", rec_u16(&c, 44), 1);
        num(&c, "red_zone", "CHARGE REDZONE", rec_u16(&c, 46), -1);
        return 1;
    }
    num(&c, "energy_drain", "ENERGY DRAIN", rec_u16(&c, 42), -1);
    switch (cat) {
    case MOA_CAT_HEAD: case MOA_CAT_CORE: case MOA_CAT_ARMS: case MOA_CAT_LEGS:
        num(&c, "ap", "ARMOR POINT", rec_u16(&c, 44), 1);
        num(&c, "defense_shell", "DEF_SHELL", rec_u16(&c, 46), 1);
        num(&c, "defense_energy", "DEF_ENERGY", rec_u16(&c, 48), 1);
        break;
    default: break;
    }
    switch (cat) {
    case MOA_CAT_HEAD:
        word(&c, "computer", "COMPUTER TYPE", rec_u8(&c, 0x32), k_computer, 3);
        word(&c, "map", "MAP TYPE", rec_u8(&c, 0x33), k_map, 4);
        word(&c, "noise", "NOISE CANCELER", rec_u8(&c, 0x34) ? 1 : 0, k_none_prov, 2);
        word(&c, "bio", "BIO SENSOR", rec_u8(&c, 0x35) ? 1 : 0, k_none_prov, 2);
        word(&c, "radar", "RADAR FUNCTION", rec_u8(&c, 0x36) ? 1 : 0, k_none_prov, 2);
        if (rec_u8(&c, 0x36)) {
            num(&c, "radar_range", "RADAR RANGE", rec_u16(&c, 0x38), 1);
            word(&c, "radar_type", "RADAR TYPE", rec_u8(&c, 0x3B), k_radar_type, 4);
        }
        break;
    case MOA_CAT_CORE:
        num(&c, "arm_load_limit", "MAX WEIGHT", rec_u16(&c, 0x32), 1);
        num(&c, "vs_mg_response", "VS-MG-RESPONSE", rec_u8(&c, 0x34), 1);
        num(&c, "vs_mg_angle", "VS-MG-ANGLE", rec_u8(&c, 0x35), 1);
        num(&c, "option_slots", "EXTENSION SLOTS", rec_u8(&c, 0x36), 1);
        break;
    case MOA_CAT_LEGS:
        num(&c, "carry_limit", "MAX WEIGHT", rec_u16(&c, 0x32), 1);
        num(&c, "speed", "SPEED", rec_u16(&c, 0x36), 1);
        num(&c, "stability", "STABILITY", rec_u16(&c, 0x40), 1);
        word(&c, "jump", "JUMP FUNCTION", rec_u16(&c, 0x42) ? 1 : 0, k_none_prov, 2);
        break;
    case MOA_CAT_ARMS:
        if (index >= 12) weapon_rows(&c);
        break;
    case MOA_CAT_HAND:
        weapon_rows(&c);
        break;
    case MOA_CAT_BACK:
        word(&c, "radar", "RADAR FUNCTION", rec_u16(&c, 0x2C) ? 1 : 0, k_none_prov, 2);
        if (rec_u16(&c, 0x2C)) {
            num(&c, "radar_range", "RADAR RANGE", rec_u16(&c, 0x2E), 1);
            word(&c, "radar_type", "RADAR TYPE", rec_u16(&c, 0x30), k_radar_type, 4);
        }
        weapon_rows(&c);
        break;
    case MOA_CAT_BLADE:
        num(&c, "charge_drain", "CHARGE DRAIN", rec_u16(&c, 0x2C), -1);
        num(&c, "attack", "ATTACK POWER", rec_u16(&c, 0x30), 1);
        break;
    case MOA_CAT_BOOST:
        num(&c, "power", "BOOST POWER", rec_u16(&c, 0x2C), 1);
        num(&c, "boost_drain", "CHARGE DRAIN", rec_u16(&c, 0x2E), -1);
        break;
    case MOA_CAT_FCS:
        num(&c, "max_locks", "MAXIMUM LOCK", rec_u8(&c, 0x2C), 1);
        word(&c, "lock_type", "LOCK TYPE", rec_u8(&c, 0x2D), k_fcs_lock, 5);
        break;
    default: break;
    }
    return 1;
}

/* parts/<category>[/<sub>]/NNN-name.toml */
static int find_part_file(const char *dir, int depth, int id, char *out, size_t n) {
    char want[8];
    snprintf(want, sizeof want, "%03d-", id);
#ifdef _WIN32
    char pattern[1200];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        const char *name = fd.cFileName;
        if (name[0] == '.') continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth == 0 && find_part_file(path, 1, id, out, n)) found = 1;
        } else if (strncmp(name, want, 4) == 0) {
            snprintf(out, n, "%s", path);
            found = 1;
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    for (struct dirent *de; !found && (de = readdir(d)); ) {
        if (de->d_name[0] == '.') continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (is_dir(path)) {
            if (depth == 0 && find_part_file(path, 1, id, out, n)) found = 1;
        } else if (strncmp(de->d_name, want, 4) == 0) {
            snprintf(out, n, "%s", path);
            found = 1;
        }
    }
    closedir(d);
    return found;
#endif
}

int moa_acv_part_details(const char *dir, int cat, int id, struct moa_acv_details *out) {
    memset(out, 0, sizeof *out);
    if (cat < 0 || cat >= MOA_CAT__COUNT || id < 0 || id > 254) return 0;
    char base[1100], path[1200];
    snprintf(base, sizeof base, "%s/parts/%s", dir, k_cat_dir[cat]);
    if (!find_part_file(base, 0, id, path, sizeof path)) return 0;
    char *t = read_text(path);
    if (!t) return 0;
    const int ok = moa_acv_parse_details(t, cat, out);
    free(t);
    return ok;
}
