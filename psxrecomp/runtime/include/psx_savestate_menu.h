#ifndef PSX_SAVESTATE_MENU_H
#define PSX_SAVESTATE_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void psx_savestate_menu_set_state(int open, int selected_slot);
void psx_savestate_menu_set_external_overlay(int open, const uint32_t *pixels,
                                             int w, int h);
void psx_savestate_menu_note_slots_changed(void);
int  psx_savestate_menu_needs_present(void);
int  psx_savestate_menu_overlay_image(const uint32_t **pixels, int *w, int *h);
/* A plugin's overlay, under the two above (NULL clears it). The pixels must
 * stay valid until replaced. overlay_blend: 1 when the image last returned by
 * psx_savestate_menu_overlay_image is the plugin's (draw it with its alpha). */
void psx_savestate_menu_set_mod_overlay(const uint32_t *pixels, int w, int h);
int  psx_savestate_menu_overlay_blend(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_SAVESTATE_MENU_H */
