/* Master of Arena: Link Versus memory cards (launcher).
 *
 * Link Versus runs two consoles on one PC. Each player picks their own card
 * for slot 1; slot 2 is always that player's QUICK START card: a card kept
 * between launches that holds two ready saves with every part owned, one
 * without Human PLUS and one with it at full. The launcher puts back a quick
 * start save that is missing (deleted, or the card is new) and leaves one the
 * player changed alone; RESET puts the card back to the two saves.
 *
 * The simple save manager lists a card's saves and copies or deletes one.
 * Every change keeps the previous image as <card>.bak.
 */
#ifndef MOA_LINK_H
#define MOA_LINK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One save on a memory card image. */
struct moa_card_save {
    int  slot;              /* directory slot of its first block, 1..15 */
    int  blocks;            /* blocks it uses */
    char file[21];          /* its name on the card, "BASLUS-01030A" */
    char title[40];         /* the save's own title, in plain letters */
    int  moa;               /* 1 = a Master of Arena save */
    char pilot[17], ac[17]; /* Master of Arena saves only */
    int  quick_start;       /* 1 = one of the QUICK START saves */
};

/* The saves on a card, up to `max`, in directory order. -1 = the card could
 * not be read (missing or not a memory card image). */
int moa_memcard_list_saves(const char *path, struct moa_card_save *out, int max);

/* Free blocks on a card (0..15), or -1 when it could not be read. */
int moa_memcard_free_blocks(const char *path);

/* Copy the save whose first block is in `slot` of card `src` onto card `dst`.
 * A Master of Arena save whose name is taken there gets the next free letter;
 * any other save already there is refused. 1 = ok, else a message in err. */
int moa_memcard_copy_save(const char *src, int slot, const char *dst,
                          char *err, size_t err_size);

/* Delete the save whose first block is in `slot`. 1 = ok, else a message. */
int moa_memcard_delete_save(const char *path, int slot, char *err, size_t err_size);

/* A new, empty memory card image at `path` (refuses to overwrite). 1 = ok. */
int moa_memcard_create(const char *path, char *err, size_t err_size);

/* The QUICK START card: puts back either quick start save that is missing,
 * making the card first when there is none (an unreadable one is kept as
 * <path>.bak). `restored` (may be NULL) gets how many saves were put back.
 * 1 = ok, else a message. */
int moa_link_quickstart_ensure(const char *path, int *restored, char *err, size_t err_size);

/* The QUICK START card back to just its two saves (old image kept as .bak). */
int moa_link_quickstart_reset(const char *path, char *err, size_t err_size);

/* The quick start saves' names on the card. */
#define MOA_LINK_QS_FILE    "BASLUS-01030Y"   /* QUICK START */
#define MOA_LINK_QS_HP_FILE "BASLUS-01030Z"   /* QUICK START + HUMAN PLUS */

#ifdef __cplusplus
}
#endif

#endif /* MOA_LINK_H */
