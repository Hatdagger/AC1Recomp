#include "moa_hub_rank_bg.h"
#include "moa_hub_rank_tiles_data.h"

int moa_hub_rank_tile_index(int rank)
{
    if (rank > 50) rank = 50;
    if (rank <= 1) return 10;          /* rank 1: black + gold */
    if (rank <= 5) return 9;           /* 5-2: black + red */
    return (50 - rank) / 5;            /* 50-46 -> 0 ... 10-6 -> 8 */
}

int moa_hub_rank_tile(int rank, const uint8_t **pix, const uint16_t **clut)
{
    int i = moa_hub_rank_tile_index(rank);
    if (pix)  *pix  = moa_hub_rank_pix[i];
    if (clut) *clut = moa_hub_rank_clut[i];
    return i;
}

const char *moa_hub_rank_belt(int rank)
{
    return moa_hub_rank_belt_name[moa_hub_rank_tile_index(rank)];
}
