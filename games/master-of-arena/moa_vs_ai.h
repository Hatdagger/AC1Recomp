/* Master of Arena: VS AI habit profile.
 *
 * What an AI card stores: how a pilot fights, as numbers. A preset is a
 * ready-made profile; the habit recorder (step 3) measures one from a
 * person's play. All fields are small integers so a card can hold them.
 */
#ifndef MOA_VS_AI_H
#define MOA_VS_AI_H

#include <stdint.h>

enum { VS_BAND_NEAR, VS_BAND_MID, VS_BAND_FAR, VS_BANDS };
enum { VS_KIND_GUN, VS_KIND_MISSILE, VS_KIND_CANNON, VS_KINDS };

struct moa_vs_profile {
    uint16_t dist;          /* preferred distance to the enemy (world units) */
    uint16_t spread;        /* how far that wanders (±) */
    uint8_t  gboost;        /* % of moving steps on the ground booster */
    uint8_t  air;           /* % of a jump spent rising on the booster */
    uint8_t  jumps;         /* jumps per minute */
    uint8_t  efloor;        /* % of capacity: stop boosting below this */
    uint8_t  strafe;        /* % of in-range time spent strafing */
    uint8_t  switches;      /* strafe side changes per minute */
    uint8_t  weapon[VS_BANDS][VS_KINDS]; /* liking 0-3 per weapon kind, per band */
    uint8_t  fire;          /* % of locked steps the trigger is held (guns) */
    uint8_t  locks;         /* missile locks held before firing, 0 = all */
    uint8_t  blade;         /* swings per minute while inside blade reach */
    uint8_t  evade;         /* % chance to dodge a missile launched at us */
    uint8_t  react;         /* reaction delay in steps (22 = 1 s) */
};

/* Presets: Balanced, Aggressor, Sniper, Duelist, Evasive. */
enum { VS_PRESET_BALANCED, VS_PRESET_AGGRESSOR, VS_PRESET_SNIPER,
       VS_PRESET_DUELIST, VS_PRESET_EVASIVE, VS_PRESETS };
extern const struct moa_vs_profile moa_vs_presets[VS_PRESETS];
extern const char *const moa_vs_preset_names[VS_PRESETS];

#endif
