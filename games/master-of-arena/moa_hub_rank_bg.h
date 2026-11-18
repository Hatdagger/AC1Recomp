/* Hub background by arena rank (karate belt ladder).
 * Rank 50-46 white, 45-41 yellow, 40-36 orange, 35-31 green, 30-26 blue,
 * 25-21 purple, 20-16 red, 15-11 brown, 10-6 black, 5-2 black+red, 1 black+gold. */
#pragma once
#include <stdint.h>

/* Which of the 11 tiles a rank uses (rank 1..50; out-of-range values are clamped). */
int moa_hub_rank_tile_index(int rank);

/* Pixels (64x64, PS1 4-bit, 2048 bytes) and 16-colour palette for a rank.
 * Returns the tile index. */
int moa_hub_rank_tile(int rank, const uint8_t **pix, const uint16_t **clut);

/* Belt name for logs ("white", "black-gold", ...). */
const char *moa_hub_rank_belt(int rank);
