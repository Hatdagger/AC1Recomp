#ifndef PSXRECOMP_CDROM_READ_GATE_H
#define PSXRECOMP_CDROM_READ_GATE_H

#include <stdint.h>

/* Which data reads may run faster than the real drive (speed divisor /
 * 'instant'). XA-ADPCM (SetMode 0x40) and the XA filter (0x08) are realtime
 * media and keep the authentic cadence, as does an active XA stream. The
 * whole-sector bit (0x20) alone is NOT realtime: Tomba's and Master of
 * Arena's ordinary file loads use SetMode 0xA0.
 *
 * One gate for both sides of acceleration. The sector cadence
 * (apply_read_speed) and the "never overwrite a sector the guest has not
 * taken" flow control (accelerated_read_active) must agree: when they
 * did not (cadence 0x48, flow control 0x68), SetMode 0xA0 reads ran at the
 * instant rate with NO flow control, so a busy guest lost sectors and
 * retried the same Setloc+ReadN in a loop (Master of Arena, Rogue Arena
 * test 7: LBA 161769-161772 re-read for ~60 s). */
static inline int cdrom_read_mode_is_realtime(uint8_t mode_reg, int xa_stream_active) {
    return xa_stream_active || (mode_reg & 0x48u) != 0;
}

#endif /* PSXRECOMP_CDROM_READ_GATE_H */
