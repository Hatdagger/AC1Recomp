/* AC models from acr-import's readable files: skeleton (set-NN.toml), joint
 * animations (set-NN-anims.txt), pieces (OBJ + MTL) and the colour ramp for
 * patterned textures. Parsers work on text the platform has loaded, so this
 * has no OS calls and builds for the Xbox too. Nothing here is game data.
 *
 * Spaces: the set files are in game space (x right, y down, z forward,
 * 4096 = a full turn, node matrix = parent x T(position) x Rx x Ry x Rz).
 * OBJ files and everything this returns are in model space: x, y up, z,
 * with the AC facing +z (game space with y and z negated). */
#ifndef ACR_MODEL_H
#define ACR_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define ACR_SKEL_MAX_NODES 64
#define ACR_ANIM_MAX       64

/* Part kinds a skeleton node can draw (the export's category names). */
enum { ACR_PART_CORE, ACR_PART_HEAD, ACR_PART_ARMS, ACR_PART_LEGS, ACR_PART_HAND,
       ACR_PART_BACK_LEFT, ACR_PART_BACK_RIGHT, ACR_PART_BOOSTER, ACR_PART_KINDS, ACR_PART_NONE = 255 };

/* "core", "back-left"... -> ACR_PART_*, or ACR_PART_NONE. */
int acr_part_kind(const char *name, size_t len);
const char *acr_part_kind_name(int kind);

typedef struct acr_skel_node {
    uint8_t id;
    int8_t  parent;              /* index into node[], -1 = root */
    uint8_t draw_kind;           /* ACR_PART_* or ACR_PART_NONE */
    uint8_t draw_piece;
    int16_t pos[3], rot[3];      /* bind pose, game space */
} acr_skel_node;

typedef struct acr_skeleton {
    int count;
    acr_skel_node node[ACR_SKEL_MAX_NODES];   /* parents come before children */
    int8_t index_of[256];                     /* node id -> index, -1 = none */
} acr_skeleton;

/* 1 on success. */
int acr_skeleton_parse(const char *text, acr_skeleton *out);

/* ---- animations ---- */

enum { ACR_KEY_ROT = 1, ACR_KEY_ROT_ZERO = 2, ACR_KEY_POS = 4, ACR_KEY_POS_ZERO = 8 };

typedef struct acr_anim_key { uint8_t node; uint8_t flags; int16_t rot[3], pos[3]; } acr_anim_key;
typedef struct acr_anim_frame { uint32_t first_key; uint16_t keys; uint8_t hold; } acr_anim_frame;
typedef struct acr_anim { uint32_t first_frame; uint16_t frames; uint16_t ticks; } acr_anim;

typedef struct acr_anim_set {
    acr_anim anim[ACR_ANIM_MAX];          /* frames = 0: not in this set */
    acr_anim_frame *frame; uint32_t frame_count;
    acr_anim_key *key; uint32_t key_count;
} acr_anim_set;

/* Node ids in the text are mapped through `skel`. 1 on success. */
int  acr_anim_set_parse(const char *text, const acr_skeleton *skel, acr_anim_set *out);
void acr_anim_set_free(acr_anim_set *set);

/* ---- pose ---- */

typedef struct acr_pose { int16_t rot[ACR_SKEL_MAX_NODES][3], pos[ACR_SKEL_MAX_NODES][3]; } acr_pose;

void acr_pose_bind(const acr_skeleton *skel, acr_pose *pose);
/* Frame shown `tick` ticks into an animation: each frame stays for its hold
 * time, the last one for good. */
int  acr_anim_frame_at(const acr_anim_set *set, int anim, unsigned tick);
/* Poses for animation `anim` at `tick`: the bind pose, then frames 0..k. A
 * missing animation leaves the bind pose. */
void acr_pose_play(const acr_skeleton *skel, const acr_anim_set *set, int anim, unsigned tick, acr_pose *pose);

/* Per node, the 3x4 matrix (row-major, last column = translation) that takes
 * a piece's OBJ vertices into model space. Rotation part also turns normals. */
/* Smooth playback (presentation only): frame k eases into frame k+1 over
 * k's hold, `tick` may be fractional. With `loop`, the last frame eases
 * back into the first; without, the last frame holds. */
void acr_pose_play_smooth(const acr_skeleton *skel, const acr_anim_set *set, int anim, float tick, int loop,
                          acr_pose *pose);
/* a..b by t (0..1): rotations the short way round, positions straight. */
void acr_pose_mix(const acr_skeleton *skel, const acr_pose *a, const acr_pose *b, float t, acr_pose *out);
void acr_pose_matrices(const acr_skeleton *skel, const acr_pose *pose, float out[ACR_SKEL_MAX_NODES][12]);

/* ---- meshes ---- */

#define ACR_MTL_MAX 16
typedef struct acr_material {
    char name[32];
    uint8_t r, g, b;
    uint8_t textured, semi, two_side;
    uint8_t kind;               /* part kind whose texture it uses (from the name), or ACR_PART_NONE */
    char texture[128];          /* map_Kd path, relative to the part folder */
} acr_material;

typedef struct acr_mtl { int count; acr_material m[ACR_MTL_MAX]; } acr_mtl;
int acr_mtl_parse(const char *text, acr_mtl *out);

typedef struct acr_mesh_vertex { float p[3], n[3], uv[2]; } acr_mesh_vertex;
typedef struct acr_mesh {
    int tri_count;
    acr_mesh_vertex *v;         /* 3 per triangle */
    uint8_t *material;          /* one per triangle, index into the acr_mtl */
} acr_mesh;

/* Triangles only (the export writes triangles; larger faces are fanned). */
int  acr_obj_parse(const char *text, const acr_mtl *mtl, acr_mesh *out);
void acr_mesh_free(acr_mesh *mesh);

/* ---- colours ---- */

/* Patterned textures (core, head, arms, legs) store colour slot x 64 +
 * shade. Palette from 4 slot colours (RGB, 0..63 steps): each slot ramps
 * to black (colour x (64 - even shade) / 64, rounded, capped at 31 in
 * 5-bit steps), a close fit to the game's own ramp. */
void acr_colour_ramp(const uint8_t colours[4][3], uint8_t palette[256][4]);

#endif
