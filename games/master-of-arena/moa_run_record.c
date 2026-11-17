/* Rogue Arena fight recorder (moa_run_record.h). */
#include "moa_run_record.h"
#include "mod_plugins.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Player blocks (shared by every engine) and the AC object fields, from
 * docs/mapping/battle-system.md. The object is found through the block's
 * pointer, as AI Battle does in this same engine (moa_ai_battle.c). */
#define REC_SLOT1        0x80045198u
#define REC_SLOT_BYTES   928u
#define REC_PAD          0x004u      /* button word, active high in the block */
#define REC_ENERGY       0x016u
#define REC_LOCK         0x07Cu      /* locked AC object, 0 = none */
#define REC_OBJECT       0x398u
#define REC_OBJ_BYTES    376u
#define O_X 0x08u
#define O_Y 0x0Au
#define O_Z 0x0Cu
#define O_FACE   0x12u
#define O_STATE  0x45u
#define O_MOVE   0x46u
#define O_COOL   0x48u
#define O_AP     0x168u               /* AP x 4 */
#define O_AP_MAX 0x16Au
#define REC_EVERY 3u                  /* a row every 3rd sampler call */

static FILE *rec_f;
static int rec_on, rec_layout_ok;
static unsigned rec_fight, rec_calls, rec_rows;
static uint32_t rec_obj[2];
static struct {
    unsigned frames, air, swings, shots, boost, locked, landed, taken, ranged;
    double range_sum;
    int range_min, range_max;
    uint8_t state, cool;
    uint16_t ap[2], energy;
} rec_st;
static char rec_line[256];

static int rec_ram(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }
static int16_t rd_s16(uint32_t a) { return (int16_t)psx_mod_read_half(a); }

void moa_rec_reset(void)
{
    if (rec_f) fclose(rec_f);
    rec_f = NULL;
    rec_on = 0;
}

static void rec_dump(const char *what, uint32_t at, uint32_t bytes)
{
    fprintf(rec_f, "# %s %08X:", what, (unsigned)at);
    for (uint32_t i = 0; i < bytes; i++) {
        if (i % 32u == 0) fprintf(rec_f, "\n#  +%03X", (unsigned)i);
        fprintf(rec_f, " %02X", psx_mod_read_byte(at + i));
    }
    fputc('\n', rec_f);
}

static void rec_begin(const char *run_file, unsigned attempt, unsigned wins, unsigned opponent)
{
    char path[1100];
    snprintf(path, sizeof path, "%s.rec.csv", run_file);
    rec_f = fopen(path, "a");
    if (!rec_f) return;
    rec_on = 1;
    rec_fight++;
    rec_calls = rec_rows = 0;
    memset(&rec_st, 0, sizeof rec_st);
    rec_st.range_min = 1 << 30;
    rec_layout_ok = 1;
    for (int s = 0; s < 2; s++) {
        rec_obj[s] = psx_mod_read_word(REC_SLOT1 + (uint32_t)s * REC_SLOT_BYTES + REC_OBJECT);
        const uint32_t o = rec_obj[s];
        const uint16_t ap = rec_ram(o) ? psx_mod_read_half(o + O_AP) : 0;
        const uint16_t mx = rec_ram(o) ? psx_mod_read_half(o + O_AP_MAX) : 0;
        if (!rec_ram(o) || !mx || ap > mx) rec_layout_ok = 0;
    }
    fprintf(rec_f, "# fight attempt %u, rank %u, opponent entry %u; objects %08X %08X; layout %s\n",
            attempt, 50u - wins, opponent, (unsigned)rec_obj[0], (unsigned)rec_obj[1],
            rec_layout_ok ? "as in VS" : "DIFFERENT (positions, AP, state not read)");
    for (int s = 0; s < 2; s++) {
        char what[24];
        snprintf(what, sizeof what, "player block %d", s + 1);
        rec_dump(what, REC_SLOT1 + (uint32_t)s * REC_SLOT_BYTES, REC_SLOT_BYTES);
        if (rec_ram(rec_obj[s])) {
            snprintf(what, sizeof what, "AC object %d", s + 1);
            rec_dump(what, rec_obj[s], REC_OBJ_BYTES);
        }
    }
    fprintf(rec_f, "t,pad,x1,y1,z1,face1,state1,move1,ap1,energy1,lock1,cool1,"
                   "x2,y2,z2,face2,state2,move2,ap2,energy2,lock2,cool2\n");
    for (int s = 0; s < 2; s++) rec_st.ap[s] = rec_layout_ok ? psx_mod_read_half(rec_obj[s] + O_AP) : 0;
    rec_st.energy = psx_mod_read_half(REC_SLOT1 + REC_ENERGY);
}

void moa_rec_sample(const char *run_file, unsigned attempt, unsigned wins, unsigned opponent)
{
    if (!run_file || !run_file[0]) return;
    if (!rec_on) rec_begin(run_file, attempt, wins, opponent);
    if (!rec_on) return;
    const unsigned t = rec_calls++;
    const uint32_t b1 = REC_SLOT1, b2 = REC_SLOT1 + REC_SLOT_BYTES;
    const uint16_t energy = psx_mod_read_half(b1 + REC_ENERGY);
    const uint32_t lock = psx_mod_read_word(b1 + REC_LOCK);
    rec_st.frames++;
    if (energy < rec_st.energy) rec_st.boost++;
    rec_st.energy = energy;
    if (lock) rec_st.locked++;
    if (rec_layout_ok) {
        const uint32_t o1 = rec_obj[0], o2 = rec_obj[1];
        const uint8_t st = psx_mod_read_byte(o1 + O_STATE), cool = psx_mod_read_byte(o1 + O_COOL);
        if ((psx_mod_read_byte(o1 + O_MOVE) & 0xC0u) == 0xC0u) rec_st.air++;
        if ((st == 0x20u || st == 0x21u) && st != rec_st.state && rec_st.state != 0x20u && rec_st.state != 0x21u)
            rec_st.swings++;
        if (cool && !rec_st.cool) rec_st.shots++;
        rec_st.state = st;
        rec_st.cool = cool;
        const uint16_t ap1 = psx_mod_read_half(o1 + O_AP), ap2 = psx_mod_read_half(o2 + O_AP);
        if (ap1 < rec_st.ap[0]) rec_st.taken++;
        if (ap2 < rec_st.ap[1]) rec_st.landed++;
        rec_st.ap[0] = ap1; rec_st.ap[1] = ap2;
        const double dx = rd_s16(o2 + O_X) - rd_s16(o1 + O_X), dz = rd_s16(o2 + O_Z) - rd_s16(o1 + O_Z);
        const int r = (int)sqrt(dx * dx + dz * dz);
        rec_st.range_sum += r;
        rec_st.ranged++;
        if (r < rec_st.range_min) rec_st.range_min = r;
        if (r > rec_st.range_max) rec_st.range_max = r;
    }
    if (t % REC_EVERY) return;
    fprintf(rec_f, "%u,%04X", t, psx_mod_read_half(b1 + REC_PAD));
    for (int s = 0; s < 2; s++) {
        const uint32_t b = s ? b2 : b1, o = rec_obj[s];
        if (rec_layout_ok)
            fprintf(rec_f, ",%d,%d,%d,%u,%u,%u,%u", rd_s16(o + O_X), rd_s16(o + O_Y), rd_s16(o + O_Z),
                    psx_mod_read_half(o + O_FACE), psx_mod_read_byte(o + O_STATE), psx_mod_read_byte(o + O_MOVE),
                    psx_mod_read_half(o + O_AP) / 4u);
        else
            fprintf(rec_f, ",,,,,,,");
        fprintf(rec_f, ",%u,%u,%u", psx_mod_read_half(b + REC_ENERGY), psx_mod_read_word(b + REC_LOCK) ? 1u : 0u,
                rec_layout_ok ? psx_mod_read_byte(o + O_COOL) : 0u);
    }
    fputc('\n', rec_f);
    if (++rec_rows % 60u == 0) fflush(rec_f);
}

const char *moa_rec_end(int won)
{
    if (!rec_on) return NULL;
    rec_on = 0;
    const unsigned f = rec_st.frames ? rec_st.frames : 1u;
    snprintf(rec_line, sizeof rec_line,
             "fight %u %s: %u samples, air %u%%, boosting %u%%, locked %u%%, %u blade swings, %u shots, "
             "range avg %d (%d-%d), hits landed %u, taken %u%s",
             rec_fight, won ? "won" : "lost", rec_st.frames, rec_st.air * 100u / f, rec_st.boost * 100u / f,
             rec_st.locked * 100u / f, rec_st.swings, rec_st.shots,
             rec_st.ranged ? (int)(rec_st.range_sum / rec_st.ranged) : 0,
             rec_st.ranged ? rec_st.range_min : 0, rec_st.range_max,
             rec_st.landed, rec_st.taken, rec_layout_ok ? "" : " (layout not as in VS: partial)");
    if (rec_f) {
        fprintf(rec_f, "# end %s\n", rec_line);
        fclose(rec_f);
        rec_f = NULL;
    }
    return rec_line;
}
