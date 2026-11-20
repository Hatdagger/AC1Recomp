/* psx_link_session.h — which end of a link cable this process is, read from
 * the environment only (see sio1.h). Header-only so files linked into every
 * target (crash reports, freeze heartbeat) can use it without pulling in the
 * serial port.
 *
 *   PSX_LINK_CABLE=<name>   cable name (same for both sessions)
 *   PSX_LINK_SIDE=1|2       this session's end of the cable
 *
 * Session 2 is the second console on the PC. It keeps its own copies of the
 * files a session writes, named with PSX_LINK_FILE_TAG; session 1 uses the
 * player's usual files. */
#ifndef PSX_LINK_SESSION_H
#define PSX_LINK_SESSION_H

#include <stdlib.h>

#define PSX_LINK_FILE_TAG "link2"

/* 1 or 2 when PSX_LINK_CABLE and PSX_LINK_SIDE name a valid end, else 0. */
static inline int psx_link_session_from_env(void)
{
    const char *name = getenv("PSX_LINK_CABLE");
    const char *side = getenv("PSX_LINK_SIDE");
    if (!name || !name[0] || !side) return 0;
    if (side[0] == '1' && side[1] == '\0') return 1;
    if (side[0] == '2' && side[1] == '\0') return 2;
    return 0;
}

/* 1 when this process is link session 2 (keeps its own files). */
static inline int psx_link_is_second_session(void)
{
    return psx_link_session_from_env() == 2;
}

#endif /* PSX_LINK_SESSION_H */
