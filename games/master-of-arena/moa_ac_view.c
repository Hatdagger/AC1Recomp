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

struct moa_acv {
    acr_skeleton skel;
    acr_anim_set anims;
    acv_part part[ACR_PART_KINDS];
    float radius, centre_y;          /* for framing, from the bind pose */
};

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
    const int index[ACR_PART_KINDS] = {
        [ACR_PART_CORE] = rec[0], [ACR_PART_HEAD] = rec[1], [ACR_PART_ARMS] = rec[2], [ACR_PART_LEGS] = rec[3],
        [ACR_PART_HAND] = weapon_arms ? -1 : (rec[4] == 255 ? 0 : rec[4]),   /* 0: the empty hand */
        [ACR_PART_BACK_LEFT] = rec[6] == 255 ? -1 : rec[6],
        [ACR_PART_BACK_RIGHT] = rec[7] == 255 ? -1 : rec[7],
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

moa_acv *moa_acv_load(const char *dir, const uint8_t rec[13], const uint8_t paint[48]) {
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

/* ---- posing ---- */

typedef struct acv_vtx { float x, y, z, nx, ny, nz, u, v; } acv_vtx;

/* Calls `fn` for every triangle of the AC posed at `time`, in model space
 * (x right, y up, z towards the viewer at yaw 0). */
typedef void (*tri_fn)(void *ctx, const acv_vtx t[3], const acv_tex *tex, const uint8_t rgb[3]);

static void for_each_tri(const moa_acv *v, float time, tri_fn fn, void *ctx) {
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
            if (mt->textured) {
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
            }
            fn(ctx, o, tex, rgb);
        }
    }
}

typedef struct bounds { float lo[3], hi[3]; int any; } bounds;
static void grow(void *ctx, const acv_vtx t[3], const acv_tex *tex, const uint8_t rgb[3]) {
    (void)tex; (void)rgb;
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

static void draw_tri(void *ctx, const acv_vtx t[3], const acv_tex *tex, const uint8_t rgb[3]) {
    raster *r = (raster *)ctx;
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
    put_tri(r, s, tex, rgb);
}

void moa_acv_render(const moa_acv *v, float yaw, float pitch, float time, int w, int h, uint8_t *rgba) {
    memset(rgba, 0, (size_t)w * h * 4);
    if (!v || w <= 0 || h <= 0) return;
    raster r;
    r.w = w; r.h = h; r.rgba = rgba;
    r.depth = (float *)malloc(sizeof(float) * (size_t)w * h);
    if (!r.depth) return;
    for (int i = 0; i < w * h; ++i) r.depth[i] = 1e30f;
    r.cy = cosf(yaw); r.sy = sinf(yaw); r.cp = cosf(pitch); r.sp = sinf(pitch);
    r.centre_y = v->centre_y;
    r.dist = v->radius * 3.2f;
    /* the whole AC fits the shorter side with a little room */
    const float side = (float)(w < h ? w : h);
    r.scale = 0.46f * side * (r.dist - v->radius) / v->radius;
    for_each_tri(v, time, draw_tri, &r);
    free(r.depth);
}

/* ---- part details ---- */

static const char *const k_cat_dir[MOA_CAT__COUNT] = {
    "head", "core", "legs", "arms", "hand", "back", "blade", "booster", "fcs", "generator"
};

/* Shown in this order when the file has them. */
static const struct { const char *key, *label; int better; } k_fields[] = {
    { "weight", "WEIGHT", -1 },          { "energy_drain", "ENERGY DRAIN", -1 },
    { "ap", "AP", 1 },                   { "defense_shell", "DEF. SHELL", 1 },
    { "defense_energy", "DEF. ENERGY", 1 },
    { "arm_load_limit", "ARM LOAD", 1 }, { "option_slots", "OPTION SLOTS", 1 },
    { "carry_limit", "MAX. WEIGHT", 1 }, { "drain_standing", "DRAIN (STANDING)", -1 },
    { "stability", "STABILITY", 1 },     { "turn_speed", "TURN SPEED", 1 },
    { "turn_accel", "TURN ACCEL.", 1 },
    { "output", "OUTPUT", 1 },           { "capacity", "CAPACITY", 1 },
    { "red_zone", "RED ZONE", -1 },
    { "power", "BOOST POWER", 1 },       { "boost_drain", "BOOST DRAIN", -1 },
    { "attack", "ATTACK", 1 },           { "ammo", "AMMO", 1 },
    { "range", "RANGE", 1 },             { "salvo", "LOCKS AT ONCE", 1 },
    { "energy_per_shot", "ENERGY / SHOT", -1 },
    { "shot_speed", "SHOT SPEED", 1 },   { "impact", "IMPACT", 1 },
    { "homing", "HOMING", 1 },           { "ammo_price", "AMMO PRICE", -1 },
    { "max_locks", "MAX. LOCKS", 1 },
    { "price", "PRICE", 0 },
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

int moa_acv_parse_details(const char *text, int cat, struct moa_acv_details *out) {
    memset(out, 0, sizeof *out);
    const char *e = text + strlen(text);
    copy_string(text, e, "name", out->name, sizeof out->name);
    copy_string(text, e, "type", out->type, sizeof out->type);
    if (!out->name[0]) return 0;
    for (size_t i = 0; i < sizeof k_fields / sizeof k_fields[0] && out->count < MOA_ACV_FIELDS; ++i) {
        const char *v = find_key(text, e, k_fields[i].key);
        if (!v) continue;
        /* a generator's drain line is not written; a booster's output is "power" */
        struct moa_acv_field *f = &out->f[out->count++];
        snprintf(f->key, sizeof f->key, "%s", k_fields[i].key);
        snprintf(f->label, sizeof f->label, "%s", k_fields[i].label);
        f->value = strtol(v, NULL, 10);
        f->better = k_fields[i].better;
    }
    if (cat == MOA_CAT_FCS) {
        /* lock_class_N = [range, box w, h, inner w, h, lock time] */
        for (int k = 0; k < 4 && out->count + 2 <= MOA_ACV_FIELDS; ++k) {
            char key[16];
            snprintf(key, sizeof key, "lock_class_%d", k);
            const char *v = find_key(text, e, key);
            if (!v || !(v = strchr(v, '['))) continue;
            long n[6] = { 0 };
            char *p = (char *)v + 1;
            for (int j = 0; j < 6; ++j) { n[j] = strtol(p, &p, 10); if (*p == ',') ++p; }
            struct moa_acv_field *f = &out->f[out->count++];
            snprintf(f->key, sizeof f->key, "lock_range_%d", k);
            snprintf(f->label, sizeof f->label, "LOCK RANGE %c", 'A' + k);
            f->value = n[0];
            f->better = 1;
            f = &out->f[out->count++];
            snprintf(f->key, sizeof f->key, "lock_time_%d", k);
            snprintf(f->label, sizeof f->label, "LOCK TIME %c", 'A' + k);
            f->value = n[5];
            f->better = -1;
        }
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
