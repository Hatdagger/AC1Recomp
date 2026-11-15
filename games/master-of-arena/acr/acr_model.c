#include "acr_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- small text helpers ---- */

static const char *line_end(const char *p) {
    while (*p && *p != '\n') ++p;
    return p;
}

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
    return p;
}

/* Reads up to n integers from "[a, b, c]" or "a b c"; returns how many. */
static int read_ints(const char *p, const char *end, long *v, int n) {
    int k = 0;
    while (p < end && k < n) {
        if (*p == '#') break;
        if (*p == '-' || (*p >= '0' && *p <= '9')) {
            char *next;
            v[k++] = strtol(p, &next, 10);
            p = next;
        } else {
            ++p;
        }
    }
    return k;
}

static int starts(const char *p, const char *end, const char *word) {
    const size_t n = strlen(word);
    return (size_t)(end - p) >= n && memcmp(p, word, n) == 0 &&
           ((size_t)(end - p) == n || p[n] == ' ' || p[n] == '=' || p[n] == '\t' || p[n] == '\r');
}

/* "key = value" -> value start, or NULL. */
static const char *value_of(const char *p, const char *end, const char *key) {
    if (!starts(p, end, key)) return NULL;
    p += strlen(key);
    p = skip_ws(p, end);
    if (p < end && *p == '=') ++p;
    return skip_ws(p, end);
}

static const char *const k_kind[ACR_PART_KINDS] = {
    "core", "head", "arms", "legs", "hand", "back-left", "back-right", "booster"
};

int acr_part_kind(const char *name, size_t len) {
    for (int i = 0; i < ACR_PART_KINDS; ++i)
        if (strlen(k_kind[i]) == len && memcmp(name, k_kind[i], len) == 0) return i;
    return ACR_PART_NONE;
}

const char *acr_part_kind_name(int kind) {
    return kind >= 0 && kind < ACR_PART_KINDS ? k_kind[kind] : "";
}

/* ---- skeleton ---- */

int acr_skeleton_parse(const char *text, acr_skeleton *out) {
    memset(out, 0, sizeof *out);
    memset(out->index_of, -1, sizeof out->index_of);
    acr_skel_node raw[ACR_SKEL_MAX_NODES];
    int parent_id[ACR_SKEL_MAX_NODES];
    int n = -1;
    for (const char *p = text; *p; ) {
        const char *e = line_end(p), *s = skip_ws(p, e), *v;
        long x[3];
        if (starts(s, e, "[[node]]")) {
            if (++n >= ACR_SKEL_MAX_NODES) return 0;
            memset(&raw[n], 0, sizeof raw[n]);
            raw[n].draw_kind = ACR_PART_NONE;
            parent_id[n] = -1;
        } else if (n >= 0 && (v = value_of(s, e, "id")) && read_ints(v, e, x, 1) == 1) {
            if (x[0] < 0 || x[0] > 255) return 0;
            raw[n].id = (uint8_t)x[0];
        } else if (n >= 0 && (v = value_of(s, e, "parent")) && read_ints(v, e, x, 1) == 1) {
            parent_id[n] = (int)x[0];
        } else if (n >= 0 && (v = value_of(s, e, "position")) && read_ints(v, e, x, 3) == 3) {
            for (int k = 0; k < 3; ++k) raw[n].pos[k] = (int16_t)x[k];
        } else if (n >= 0 && (v = value_of(s, e, "rotation")) && read_ints(v, e, x, 3) == 3) {
            for (int k = 0; k < 3; ++k) raw[n].rot[k] = (int16_t)x[k];
        } else if (n >= 0 && (v = value_of(s, e, "draws")) && v < e && *v == '"') {
            const char *a = v + 1, *b = a;
            while (b < e && *b != ' ' && *b != '"') ++b;
            raw[n].draw_kind = (uint8_t)acr_part_kind(a, (size_t)(b - a));
            if (read_ints(b, e, x, 1) == 1 && x[0] >= 0 && x[0] < 256) raw[n].draw_piece = (uint8_t)x[0];
        }
        p = *e ? e + 1 : e;
    }
    const int count = n + 1;
    if (count <= 0) return 0;
    /* Order so every parent comes before its children. */
    uint8_t placed[ACR_SKEL_MAX_NODES] = { 0 };
    for (int pass = 0; pass < count && out->count < count; ++pass) {
        for (int i = 0; i < count; ++i) {
            if (placed[i]) continue;
            int pi = -1;
            if (parent_id[i] >= 0) {
                if (parent_id[i] > 255 || out->index_of[parent_id[i]] < 0) continue;
                pi = out->index_of[parent_id[i]];
            }
            if (out->index_of[raw[i].id] >= 0) return 0;   /* repeated id */
            acr_skel_node *d = &out->node[out->count];
            *d = raw[i];
            d->parent = (int8_t)pi;
            out->index_of[raw[i].id] = (int8_t)out->count++;
            placed[i] = 1;
        }
    }
    return out->count == count;   /* otherwise a parent is missing or a loop */
}

/* ---- animations ---- */

static int grow(void **p, uint32_t *cap, uint32_t need, size_t item) {
    if (need <= *cap) return 1;
    uint32_t c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * item);
    if (!q) return 0;
    *p = q;
    *cap = c;
    return 1;
}

void acr_anim_set_free(acr_anim_set *set) {
    free(set->frame);
    free(set->key);
    memset(set, 0, sizeof *set);
}

int acr_anim_set_parse(const char *text, const acr_skeleton *skel, acr_anim_set *out) {
    memset(out, 0, sizeof *out);
    uint32_t frame_cap = 0, key_cap = 0;
    int cur = -1;
    uint8_t holds[256];
    int hold_count = 0;
    for (const char *p = text; *p; ) {
        const char *e = line_end(p), *s = skip_ws(p, e), *v;
        long x[8];
        if (starts(s, e, "anim")) {
            if (read_ints(s + 4, e, x, 1) != 1 || x[0] < 0 || x[0] >= ACR_ANIM_MAX) goto fail;
            cur = (int)x[0];
            out->anim[cur].first_frame = out->frame_count;
            out->anim[cur].frames = 0;
            out->anim[cur].ticks = 0;
            hold_count = 0;
        } else if (cur >= 0 && starts(s, e, "hold")) {
            hold_count = 0;
            for (const char *q = s + 4; q < e && hold_count < 256; ) {
                if (*q >= '0' && *q <= '9') {
                    char *next;
                    holds[hold_count++] = (uint8_t)strtol(q, &next, 10);
                    q = next;
                } else {
                    ++q;
                }
            }
        } else if (cur >= 0 && starts(s, e, "frame")) {
            acr_anim *a = &out->anim[cur];
            if (!grow((void **)&out->frame, &frame_cap, out->frame_count + 1, sizeof *out->frame)) goto fail;
            acr_anim_frame *f = &out->frame[out->frame_count++];
            f->first_key = out->key_count;
            f->keys = 0;
            f->hold = a->frames < hold_count ? holds[a->frames] : 1;
            if (a->frames + 1 < 65535) a->frames++;
            if (a->frames > 1) a->ticks = (uint16_t)(a->ticks + out->frame[out->frame_count - 2].hold);
        } else if (cur >= 0 && out->anim[cur].frames && starts(s, e, "node")) {
            if (read_ints(s + 4, s + 4 + 6 < e ? s + 4 + 6 : e, x, 1) != 1) goto fail;
            if (x[0] < 0 || x[0] > 255 || skel->index_of[x[0]] < 0) { p = *e ? e + 1 : e; continue; }
            if (!grow((void **)&out->key, &key_cap, out->key_count + 1, sizeof *out->key)) goto fail;
            acr_anim_key *k = &out->key[out->key_count];
            memset(k, 0, sizeof *k);
            k->node = (uint8_t)skel->index_of[x[0]];
            const char *r = NULL, *q = NULL;
            for (const char *t = s; t + 3 <= e; ++t) {
                if (!r && t[0] == 'r' && t[1] == 'o' && t[2] == 't') r = t + 3;
                if (!q && t[0] == 'p' && t[1] == 'o' && t[2] == 's') q = t + 3;
            }
            if (r) {
                const char *r_end = (q && q > r) ? q - 3 : e;
                r = skip_ws(r, r_end);
                if (r_end - r >= 4 && !memcmp(r, "zero", 4)) k->flags |= ACR_KEY_ROT_ZERO;
                else if (read_ints(r, r_end, x, 3) == 3) {
                    k->flags |= ACR_KEY_ROT;
                    for (int i = 0; i < 3; ++i) k->rot[i] = (int16_t)x[i];
                }
            }
            if (q) {
                const char *q_end = (r && r > q) ? r - 3 : e;
                q = skip_ws(q, q_end);
                if (q_end - q >= 4 && !memcmp(q, "zero", 4)) k->flags |= ACR_KEY_POS_ZERO;
                else if (read_ints(q, q_end, x, 3) == 3) {
                    k->flags |= ACR_KEY_POS;
                    for (int i = 0; i < 3; ++i) k->pos[i] = (int16_t)x[i];
                }
            }
            out->key_count++;
            out->frame[out->frame_count - 1].keys++;
        }
        (void)v;
        p = *e ? e + 1 : e;
    }
    return 1;
fail:
    acr_anim_set_free(out);
    return 0;
}

/* ---- pose ---- */

void acr_pose_bind(const acr_skeleton *skel, acr_pose *pose) {
    memset(pose, 0, sizeof *pose);
    for (int i = 0; i < skel->count; ++i) {
        memcpy(pose->rot[i], skel->node[i].rot, sizeof pose->rot[i]);
        memcpy(pose->pos[i], skel->node[i].pos, sizeof pose->pos[i]);
    }
}

int acr_anim_frame_at(const acr_anim_set *set, int anim, unsigned tick) {
    if (anim < 0 || anim >= ACR_ANIM_MAX || !set->anim[anim].frames) return -1;
    const acr_anim *a = &set->anim[anim];
    unsigned t = 0;
    for (int f = 0; f + 1 < a->frames; ++f) {
        t += set->frame[a->first_frame + f].hold;
        if (tick < t) return f;
    }
    return a->frames - 1;
}

void acr_pose_play(const acr_skeleton *skel, const acr_anim_set *set, int anim, unsigned tick, acr_pose *pose) {
    acr_pose_bind(skel, pose);
    const int last = acr_anim_frame_at(set, anim, tick);
    for (int f = 0; f <= last; ++f) {
        const acr_anim_frame *fr = &set->frame[set->anim[anim].first_frame + (uint32_t)f];
        for (uint32_t k = 0; k < fr->keys; ++k) {
            const acr_anim_key *key = &set->key[fr->first_key + k];
            if (key->node >= skel->count) continue;
            if (key->flags & ACR_KEY_ROT) memcpy(pose->rot[key->node], key->rot, sizeof key->rot);
            else if (key->flags & ACR_KEY_ROT_ZERO) memset(pose->rot[key->node], 0, sizeof key->rot);
            if (key->flags & ACR_KEY_POS) memcpy(pose->pos[key->node], key->pos, sizeof key->pos);
            else if (key->flags & ACR_KEY_POS_ZERO) memset(pose->pos[key->node], 0, sizeof key->pos);
        }
    }
}

void acr_pose_mix(const acr_skeleton *skel, const acr_pose *a, const acr_pose *b, float t, acr_pose *out) {
    if (t <= 0.0f) { if (out != a) *out = *a; return; }
    if (t >= 1.0f) { if (out != b) *out = *b; return; }
    for (int i = 0; i < skel->count; ++i)
        for (int j = 0; j < 3; ++j) {
            int d = ((b->rot[i][j] - a->rot[i][j]) % 4096 + 4096 + 2048) % 4096 - 2048;
            out->rot[i][j] = (int16_t)(a->rot[i][j] + (int)lroundf((float)d * t));
            out->pos[i][j] = (int16_t)lroundf((float)a->pos[i][j] + (float)(b->pos[i][j] - a->pos[i][j]) * t);
        }
}

void acr_pose_play_smooth(const acr_skeleton *skel, const acr_anim_set *set, int anim, float tick, int loop,
                          acr_pose *pose) {
    if (tick < 0.0f) tick = 0.0f;
    const unsigned whole = (unsigned)tick;
    const int f = acr_anim_frame_at(set, anim, whole);
    acr_pose_play(skel, set, anim, whole, pose);
    if (f < 0) return;
    const acr_anim *a = &set->anim[anim];
    if (f == a->frames - 1 && (!loop || a->frames < 2)) return;
    unsigned start = 0;
    for (int k = 0; k < f; ++k) start += set->frame[a->first_frame + k].hold;
    unsigned hold = set->frame[a->first_frame + f].hold;
    if (!hold) hold = 1;
    acr_pose next;
    acr_pose_play(skel, set, anim, f + 1 < a->frames ? start + hold : 0, &next);
    acr_pose_mix(skel, pose, &next, (tick - (float)start) / (float)hold, pose);
}

/* 3x4 row-major: a * b */
static void mul34(const float a[12], const float b[12], float o[12]) {
    float r[12];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        r[i * 4 + 3] += a[i * 4 + 3];
    }
    memcpy(o, r, sizeof r);
}

void acr_pose_matrices(const acr_skeleton *skel, const acr_pose *pose, float out[ACR_SKEL_MAX_NODES][12]) {
    float world[ACR_SKEL_MAX_NODES][12];   /* game space */
    const float k = 6.28318530718f / 4096.0f;
    for (int i = 0; i < skel->count; ++i) {
        const float a = pose->rot[i][0] * k, b = pose->rot[i][1] * k, c = pose->rot[i][2] * k;
        const float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cc = cosf(c), sc = sinf(c);
        /* Rx * Ry * Rz */
        const float local[12] = {
            cb * cc,                 -cb * sc,                 sb,       pose->pos[i][0],
            sa * sb * cc + ca * sc,  -sa * sb * sc + ca * cc,  -sa * cb, pose->pos[i][1],
            -ca * sb * cc + sa * sc, ca * sb * sc + sa * cc,   ca * cb,  pose->pos[i][2],
        };
        const int p = skel->node[i].parent;
        if (p >= 0) mul34(world[p], local, world[i]);
        else memcpy(world[i], local, sizeof local);
        /* piece matrix = F * world * T(-bind position) * F, F = diag(1, -1, -1).
           The bind position, not the animated one: vertices sit in bind-pose
           model space, so an animated offset (the root's walk bob, node 2,
           which draws the legs' hip connector) must move the piece. */
        float m[12];
        const int16_t *bp = skel->node[i].pos;
        const float back[12] = { 1, 0, 0, (float)-bp[0], 0, 1, 0, (float)-bp[1],
                                 0, 0, 1, (float)-bp[2] };
        mul34(world[i], back, m);
        static const float sgn[3] = { 1, -1, -1 };
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 4; ++col)
                out[i][r * 4 + col] = m[r * 4 + col] * sgn[r] * (col < 3 ? sgn[col] : 1.0f);
    }
}

/* ---- MTL / OBJ ---- */

int acr_mtl_parse(const char *text, acr_mtl *out) {
    memset(out, 0, sizeof *out);
    acr_material *m = NULL;
    for (const char *p = text; *p; ) {
        const char *e = line_end(p), *s = skip_ws(p, e);
        if (starts(s, e, "newmtl")) {
            if (out->count >= ACR_MTL_MAX) return 0;
            m = &out->m[out->count++];
            memset(m, 0, sizeof *m);
            m->r = m->g = m->b = 255;
            m->kind = ACR_PART_NONE;
            const char *a = skip_ws(s + 6, e), *b = a;
            while (b < e && *b != ' ' && *b != '\r' && *b != '\t') ++b;
            size_t n = (size_t)(b - a);
            if (n >= sizeof m->name) n = sizeof m->name - 1;
            memcpy(m->name, a, n);
            /* "arms", "arms-semi", "back-left-2side"... -> the part kind before the flags */
            size_t base = n;
            for (;;) {
                if (base > 5 && !memcmp(m->name + base - 5, "-semi", 5)) { m->semi = 1; base -= 5; continue; }
                if (base > 6 && !memcmp(m->name + base - 6, "-2side", 6)) { m->two_side = 1; base -= 6; continue; }
                break;
            }
            m->kind = (uint8_t)acr_part_kind(m->name, base);
        } else if (m && starts(s, e, "Kd")) {
            const char *q = s + 2;
            float c[3] = { 1, 1, 1 };
            for (int i = 0; i < 3; ++i) {
                char *next;
                c[i] = strtof(q, &next);
                if (next == q) break;
                q = next;
            }
            m->r = (uint8_t)(c[0] * 255.0f + 0.5f);
            m->g = (uint8_t)(c[1] * 255.0f + 0.5f);
            m->b = (uint8_t)(c[2] * 255.0f + 0.5f);
        } else if (m && starts(s, e, "map_Kd")) {
            const char *a = skip_ws(s + 6, e), *b = e;
            while (b > a && (b[-1] == '\r' || b[-1] == ' ')) --b;
            size_t n = (size_t)(b - a);
            if (n >= sizeof m->texture) n = sizeof m->texture - 1;
            memcpy(m->texture, a, n);
            m->texture[n] = 0;
            m->textured = 1;
        } else if (m && starts(s, e, "d")) {
            if (strtof(s + 1, NULL) < 0.99f) m->semi = 1;
        }
        p = *e ? e + 1 : e;
    }
    return 1;
}

void acr_mesh_free(acr_mesh *mesh) {
    free(mesh->v);
    free(mesh->material);
    memset(mesh, 0, sizeof *mesh);
}

int acr_obj_parse(const char *text, const acr_mtl *mtl, acr_mesh *out) {
    memset(out, 0, sizeof *out);
    float *pos = NULL, *nrm = NULL, *uv = NULL;
    uint32_t np = 0, nn = 0, nt = 0, cp = 0, cn = 0, ct = 0, cv = 0, cm = 0;
    int mat = 0, ok = 1;
    for (const char *p = text; *p && ok; ) {
        const char *e = line_end(p), *s = skip_ws(p, e);
        if (e - s >= 2 && s[0] == 'v' && (s[1] == ' ' || s[1] == 'n' || s[1] == 't')) {
            const int kind = s[1] == ' ' ? 0 : s[1] == 'n' ? 1 : 2, dims = kind == 2 ? 2 : 3;
            float **arr = kind == 0 ? &pos : kind == 1 ? &nrm : &uv;
            uint32_t *cnt = kind == 0 ? &np : kind == 1 ? &nn : &nt, *cap = kind == 0 ? &cp : kind == 1 ? &cn : &ct;
            if (!grow((void **)arr, cap, (*cnt + 1) * (uint32_t)dims, sizeof(float))) { ok = 0; break; }
            const char *q = s + 2;
            for (int i = 0; i < dims; ++i) {
                char *next;
                (*arr)[*cnt * (uint32_t)dims + (uint32_t)i] = strtof(q, &next);
                q = next;
            }
            (*cnt)++;
        } else if (starts(s, e, "usemtl")) {
            const char *a = skip_ws(s + 6, e), *b = a;
            while (b < e && *b != ' ' && *b != '\r') ++b;
            mat = 0;
            for (int i = 0; i < mtl->count; ++i)
                if (strlen(mtl->m[i].name) == (size_t)(b - a) && !memcmp(mtl->m[i].name, a, (size_t)(b - a))) mat = i;
        } else if (e - s >= 2 && s[0] == 'f' && s[1] == ' ') {
            long idx[8][3];
            int corners = 0;
            for (const char *q = s + 1; q < e && corners < 8; ) {
                q = skip_ws(q, e);
                if (q >= e) break;
                idx[corners][0] = idx[corners][1] = idx[corners][2] = 0;
                for (int part = 0; part < 3 && q < e && *q != ' '; ++part) {
                    if (*q != '/') {
                        char *next;
                        idx[corners][part] = strtol(q, &next, 10);
                        q = next;
                    }
                    if (q < e && *q == '/') ++q;
                }
                while (q < e && *q != ' ') ++q;
                ++corners;
            }
            for (int t = 1; t + 1 < corners; ++t) {
                const int c3[3] = { 0, t, t + 1 };
                if (!grow((void **)&out->v, &cv, (uint32_t)(out->tri_count + 1) * 3, sizeof *out->v) ||
                    !grow((void **)&out->material, &cm, (uint32_t)out->tri_count + 1, 1)) { ok = 0; break; }
                for (int j = 0; j < 3; ++j) {
                    acr_mesh_vertex *v = &out->v[out->tri_count * 3 + j];
                    memset(v, 0, sizeof *v);
                    const long vi = idx[c3[j]][0], ti = idx[c3[j]][1], ni = idx[c3[j]][2];
                    if (vi < 1 || (uint32_t)vi > np) { ok = 0; break; }
                    memcpy(v->p, pos + (vi - 1) * 3, sizeof v->p);
                    if (ti >= 1 && (uint32_t)ti <= nt) memcpy(v->uv, uv + (ti - 1) * 2, sizeof v->uv);
                    if (ni >= 1 && (uint32_t)ni <= nn) memcpy(v->n, nrm + (ni - 1) * 3, sizeof v->n);
                }
                if (!ok) break;
                out->material[out->tri_count++] = (uint8_t)mat;
            }
        }
        p = *e ? e + 1 : e;
    }
    free(pos); free(nrm); free(uv);
    if (!ok) acr_mesh_free(out);
    return ok;
}

/* ---- colours ---- */

void acr_colour_ramp(const uint8_t colours[4][3], uint8_t palette[256][4]) {
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < 64; ++k) {
            for (int ch = 0; ch < 3; ++ch) {
                int v = (colours[s][ch] * (64 - (k & ~1)) + 32) / 64;
                if (v > 31) v = 31;
                palette[s * 64 + k][ch] = (uint8_t)(v << 3 | v >> 2);
            }
            palette[s * 64 + k][3] = 255;
        }
}
