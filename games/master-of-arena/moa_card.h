/* Master of Arena: AI Battle cards (Raven Card Builder, formats 2 to 4).
 *
 * A card is a square picture (640x640 as the builder saves it; any size and
 * PNG or JPEG are read). A colour strip down its left edge carries the build,
 * the sixteen colours of the game emblem and a checksum; the centre carries
 * the emblem art. See the builder (index.html in ACMOA-AI-CARDBUILDER) for
 * the writer side; this reader follows its readCard() step for step.
 */
#ifndef MOA_CARD_H
#define MOA_CARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's own AI editor (RANKER MK AI, menu 0x8007FB78) has seven rows:
 * REBEL POINT (the Human PLUS level: the card's `plus`) and these six. */
enum moa_ai_slider {
    MOA_AI_SHOOT = 0, MOA_AI_BLADE, MOA_AI_LOCK, MOA_AI_AVOID, MOA_AI_FLY,
    MOA_AI_RANGE,                         /* 0 = SHORT, 50 = MID, 100 = LONG */
    MOA_AI_SLIDERS
};

enum moa_ai_preset {
    MOA_AI_PRESET_CUSTOM = 0, MOA_AI_PRESET_BALANCED, MOA_AI_PRESET_AGGRESSOR,
    MOA_AI_PRESET_SNIPER, MOA_AI_PRESET_DUELIST, MOA_AI_PRESET_EVASIVE,
    MOA_AI_PRESETS
};

#define MOA_AI_RECORD_SIZE 36

struct moa_card {
    uint8_t format;          /* 2, 3 or 4 */
    /* Arena record order: core, head, arms, legs, hand weapon, blade,
     * back left, back right, FCS, generator, options lo, options hi, booster. */
    uint8_t rec[13];
    uint8_t flag;            /* arena record byte 13 */
    uint8_t pattern;         /* always 0 from current builders */
    uint8_t variant;
    uint8_t plus;            /* Human PLUS level, 0..15 (builder: 0 or 9) */
    /* Four sections (head, core, arms, legs) x base, optional, detail, joint
     * x RGB, as the garage sliders show them (0..64). */
    uint8_t paint[48];
    char pilot[17];
    char ac[17];
    uint16_t palette[16];    /* game emblem colours, 15-bit BGR */
    uint8_t emblem[64 * 64]; /* game emblem, palette index per pixel */
    /* AI (format 4). Without it the unit flies the Balanced preset. */
    uint8_t has_ai;
    uint8_t ai_preset;                    /* MOA_AI_PRESET_*; CUSTOM = own sliders */
    uint8_t ai_slider[MOA_AI_SLIDERS];    /* 0..100, see enum moa_ai_slider */
    uint8_t ai_set[36];                   /* 1 = advanced override for that byte */
    uint8_t ai_raw[36];                   /* the override values */
};

/* Decode a card from an image file's bytes. 1 = ok; 0 = not a card / damaged. */
int moa_card_decode(const uint8_t *file, size_t bytes, struct moa_card *out);

/* The 64-byte arena AC record the game uses for an opponent. */
void moa_card_record(const struct moa_card *card, uint8_t record[64]);

/* ---- AI ---- */

/* Preset names ("BALANCED", ...; CUSTOM for 0) and slider values. */
const char *moa_ai_preset_name(int preset);
void moa_ai_preset_sliders(int preset, uint8_t out[MOA_AI_SLIDERS]);

/* Human PLUS level <-> REBEL POINT step: 0..3 as the game's editor (level
 * 0 / 2 / 5 / 7), plus step 4 = the builder's full level 9. */
int moa_ai_rebel_step(uint8_t plus);
uint8_t moa_ai_rebel_level(int step);

/* The 36-byte AI record the arena engine reads (0x8004AC80): the game's
 * default record, the sliders applied exactly as the game's editor does,
 * then the advanced overrides; byte 0 = the card's Human PLUS level. A card
 * without AI gets the Balanced preset. */
void moa_card_ai_record(const struct moa_card *card, uint8_t record[MOA_AI_RECORD_SIZE]);

/* The same from bare values (sliders may be NULL = Balanced). */
void moa_ai_build_record(uint8_t plus, const uint8_t *sliders, const uint8_t *set,
                         const uint8_t *raw, uint8_t record[MOA_AI_RECORD_SIZE]);

/* Give a card AI: a preset (sliders from the preset, overrides cleared). */
void moa_card_set_preset(struct moa_card *card, int preset);

/* ---- writing cards ---- */

/* A fresh card: the builder's default AC, the game's default paint, the
 * default emblem, Balanced AI. */
void moa_card_new(struct moa_card *card);

/* Paint from the emblem: its four most used colours (near-identical entries
 * counted together) become base, detail, joint and optional on every section. */
void moa_card_paint_from_emblem(struct moa_card *card);

/* Emblem from a picture file (PNG / JPEG / BMP ...): the centre square, cut to
 * 64x64 and 16 colours as the builder does. 1 = ok. */
int moa_card_emblem_from_image(struct moa_card *card, const uint8_t *file, size_t bytes);

/* A card from one Master of Arena scenario save (product BASLUS-01030, one
 * 8 KB block, as stored on the memory card). The AC, its paint, optional parts,
 * Human PLUS level, pilot and AC names and the player's emblem (a default one
 * when the save has none). 1 = ok. */
int moa_card_from_save(const uint8_t *block, size_t bytes, struct moa_card *out);

/* ---- cards as game saves ---- */

#define MOA_SAVE_BLOCK 8192

/* The card as a Master of Arena scenario save block (BASLUS-01030x): the AC,
 * paint, optional parts, names, emblem and Human PLUS, with the checksum the
 * game's DATA LOAD checks. Everything else in the save is left empty. */
void moa_card_to_save(const struct moa_card *card, uint8_t block[MOA_SAVE_BLOCK]);

/* A memory card image (.mcd, 128 KB) holding `count` cards as saves
 * BASLUS-01030A, B, ... in that order (the DATA LOAD list's 1, 2, ...). */
int moa_memcard_write_units(const char *path, const struct moa_card *const units[], int count);

/* Draw a card (format 4, or 3 when it carries no AI) and write it as PNG. */
int moa_card_write_png(const struct moa_card *card, const char *path);

/* Every scenario save on a memory card image (.mcd, 128 KB) becomes a card
 * file in `out_dir`, named after the AC (never overwriting a file). Returns how
 * many cards were written, or -1 when the memory card could not be read. */
int moa_card_export_memcard(const char *mcd_path, const char *out_dir);

#ifdef __cplusplus
}
#endif

#endif
