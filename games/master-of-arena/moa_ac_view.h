/* Master of Arena: the launcher's 3D AC preview (Assembly screen).
 *
 * Models come from the optional "3D AC models" install (Extras), which runs
 * the Armored-CoreR importer (acr/) over the player's own disc 1 and keeps
 * its parts/, motion/ and models/ folders beside the program. Nothing from
 * the disc ships with the launcher.
 *
 * The AC is drawn by a small software rasteriser into an RGBA buffer (no GL
 * calls here), which the launcher uploads as a texture. That keeps it the
 * same on every back end and testable without a window.
 */
#ifndef MOA_AC_VIEW_H
#define MOA_AC_VIEW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- install ---- */

/* Unpack the archives of a Master of Arena disc 1 image into `work_dir` and
 * export parts/, motion/ and models/ there (work_dir/unpacked is left for
 * the caller to delete). 1 = ok, else a message in err. */
int moa_acv_import(const char *disc_path, const char *work_dir, char *err, size_t err_size);

/* 1 when `dir` holds a finished install (models/, motion/, parts/). */
int moa_acv_installed(const char *dir);

/* ---- the AC ---- */

typedef struct moa_acv moa_acv;

/* Build the AC for a 13-byte card record (core, head, arms, legs, hand,
 * blade, back L, back R, FCS, generator, options lo/hi, booster) with the
 * card's paint (head, core, arms, legs x base, optional, detail, joint x
 * RGB, 0..64; NULL = the disc's own colours). NULL when the install has no
 * model set for this core/arms/legs. */
moa_acv *moa_acv_load(const char *dir, const uint8_t rec[13], const uint8_t paint[48]);
void moa_acv_free(moa_acv *v);

/* The AC's emblem on its emblem plate (left shoulder; both sides of the
 * two-slot back units), as the game shows it: 16 colours (15-bit BGR, 0x0000
 * see-through) and a 64x64 colour index per pixel, as struct moa_card holds
 * it. Without an emblem (or with NULL) the plate is not drawn. */
void moa_acv_set_emblem(moa_acv *v, const uint16_t palette[16], const uint8_t index[64 * 64]);

/* Draw into `rgba` (w x h, 4 bytes a pixel, rows top first). `yaw` turns the
 * AC (radians, 0 = facing the viewer), `pitch` tilts the view, `time` (s)
 * plays the standing animation. The background is left transparent. */
void moa_acv_render(const moa_acv *v, float yaw, float pitch, float time, int w, int h, uint8_t *rgba);

/* Same, `zoom` x the usual size (1 = the whole AC), and the part kinds in
 * `wire_mask` (bit per ACR_PART_* kind) drawn as `wire_rgb` wireframe
 * instead of solid, as the garage marks the slot being changed. */
void moa_acv_render_ex(const moa_acv *v, float yaw, float pitch, float time, float zoom,
                       unsigned wire_mask, const uint8_t wire_rgb[3], int w, int h, uint8_t *rgba);

/* Wireframe kinds (ACR_PART_* in acr/acr_model.h). */
enum { MOA_ACV_CORE = 0, MOA_ACV_HEAD, MOA_ACV_ARMS, MOA_ACV_LEGS, MOA_ACV_HAND,
       MOA_ACV_BACK_LEFT, MOA_ACV_BACK_RIGHT, MOA_ACV_BOOSTER };

/* ---- one part on its own (the garage's part models) ---- */

/* 1 when the install has the garage part models (installs made before this
 * have only the AC models: install again to get them). */
int moa_acv_has_part_models(const char *dir);

/* The part as the Assembly screen shows it, framed to fit: `kind` is the
 * folder under models/menu ("head", "core", "arms", "legs", "booster",
 * "back", "blade", "hand", "fcs", "option", "generator"), `index` the part's
 * number. Head, core, arms and legs take `paint` (as moa_acv_load). Drawn
 * with moa_acv_render; time is ignored. NULL when there is no model. */
moa_acv *moa_acv_load_part(const char *dir, const char *kind, int index, const uint8_t paint[48]);

/* ---- part details (from the install's parts/ files) ---- */

#define MOA_ACV_FIELDS 24
struct moa_acv_field {
    char key[24];      /* "weight", "ap", "attack"... */
    char label[24];    /* as the garage shows it: "WEIGHT", "ARMOR POINT"... */
    long value;
    int better;        /* +1 higher is better, -1 lower is better, 0 neither */
    int is_text;       /* shown as `text` (TYPE words: ROUGH, PROVIDED...) */
    char text[24];
};
struct moa_acv_details {
    char name[24], type[24];
    int count;
    struct moa_acv_field f[MOA_ACV_FIELDS];
};

/* `cat` is enum moa_part_cat (moa_parts.h), `id` the part's number. 1 = found. */
int moa_acv_part_details(const char *dir, int cat, int id, struct moa_acv_details *out);

/* Parse one part file's text (exposed for the tests). 1 = ok. */
int moa_acv_parse_details(const char *text, int cat, struct moa_acv_details *out);

#ifdef __cplusplus
}
#endif

#endif
