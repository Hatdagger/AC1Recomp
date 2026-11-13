/* Master of Arena: the garage's part tables, read from the player's own disc,
 * and the build rules the Raven Card Builder applies (the game's own checks).
 *
 * The tables sit in the menu module (FDAT entry 206, loaded at 0x80050DD8).
 * Nothing from the disc ships with the launcher: it reads them from the disc
 * image the player already points it at.
 */
#ifndef MOA_PARTS_H
#define MOA_PARTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum moa_part_cat {
    MOA_CAT_HEAD = 0, MOA_CAT_CORE, MOA_CAT_LEGS, MOA_CAT_ARMS, MOA_CAT_HAND,
    MOA_CAT_BACK, MOA_CAT_BLADE, MOA_CAT_BOOST, MOA_CAT_FCS, MOA_CAT_GEN, MOA_CAT__COUNT
};

#define MOA_PARTS_MAX 64
#define MOA_OPTIONS 12

struct moa_part {
    uint8_t id;              /* the part's number in the card / arena record */
    char name[19];
    uint16_t weight;
    uint16_t drain;          /* energy drain (0 for a generator) */
    uint16_t ap, def_shell, def_energy;
    uint16_t extra;          /* core: arm load; legs: load; booster: power;
                                generator: output */
    uint8_t slots;           /* core: optional-part slots */
};

struct moa_parts {
    int count[MOA_CAT__COUNT];
    struct moa_part part[MOA_CAT__COUNT][MOA_PARTS_MAX];
    struct { char name[19]; char what[19]; uint8_t slots; } option[MOA_OPTIONS];
};

/* Read the tables from a Master of Arena (USA) disc 1 image. 1 = ok. */
int moa_parts_load(const char *disc_path, struct moa_parts *out);

const struct moa_part *moa_parts_find(const struct moa_parts *p, int cat, uint8_t id);

/* Sub-group shown in the builder's lists ("Weapon arms", "Missiles", ...;
 * "" when the slot has none). */
const char *moa_part_group(int cat, const char *name);

/* Which category each of the 13 card record bytes holds (-1: options). */
int moa_record_cat(int byte);

/* The builder's rules on a 13-byte card record: weapon arms empty the hands,
 * caterpillar legs drop the booster, options limited to the 12 parts. */
void moa_build_normalise(uint8_t rec[13]);

struct moa_build_check {
    int weight, ap, def_shell, def_energy;
    int leg_load, leg_max;       /* everything but the legs vs the legs' load */
    int arm_load, arm_max;       /* hand + blade (not weapon arms) vs the core */
    int drain, output;           /* energy */
    int slots_used, slots_max;
    int leg_over, arm_over, energy_over, slots_over, missing_booster;
    int blocked;                 /* the game will not field it */
};

/* Totals and the game's checks; Human PLUS (plus > 0) waives the load and
 * energy limits, never the booster or the option slots. */
void moa_build_check(const struct moa_parts *p, const uint8_t rec[13], int plus,
                     struct moa_build_check *out);

/* The versus maps' preview pictures (SELECT MAP), read from the disc's menu
 * archive. Map n (1..MOA_VS_MAPS) is preview[n - 1], RGBA, 104 x 100. */
#define MOA_VS_MAPS 10
#define MOA_MAP_W 104
#define MOA_MAP_H 100
struct moa_map_previews { uint32_t rgba[MOA_VS_MAPS][MOA_MAP_W * MOA_MAP_H]; };
int moa_map_previews_load(const char *disc_path, struct moa_map_previews *out);

/* What each optional part does (builder text). */
const char *moa_option_effect(int i);

#ifdef __cplusplus
}
#endif

#endif
