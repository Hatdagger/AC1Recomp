/* Master of Arena: AI Battle cards (Raven Card Builder, formats 2 and 3).
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

struct moa_card {
    uint8_t format;          /* 2 or 3 */
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
};

/* Decode a card from an image file's bytes. 1 = ok; 0 = not a card / damaged. */
int moa_card_decode(const uint8_t *file, size_t bytes, struct moa_card *out);

/* The 64-byte arena AC record the game uses for an opponent. */
void moa_card_record(const struct moa_card *card, uint8_t record[64]);

/* CRC-32 (zlib polynomial) of the 13 build bytes: seeds the AI draw so the
 * same card always borrows the same AI. */
uint32_t moa_card_seed(const struct moa_card *card);

/* AI draw. `pool` lists the in-game opponents as (head, arms, legs) triples in
 * arena-then-rank order; returns the index of the opponent whose AI the card
 * borrows, or -1 for an empty pool. Same legs +3, same arms +3, same head +3,
 * on top of one ticket; candidates are the opponents on the card's legs and
 * on the nearest similar legs when those are few. */
int moa_card_pick_ai(const struct moa_card *card, const uint8_t (*pool)[3],
                     int pool_count);

#ifdef __cplusplus
}
#endif

#endif
