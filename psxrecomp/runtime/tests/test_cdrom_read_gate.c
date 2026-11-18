/*
 * Which SetMode values may be read faster than the real drive.
 *
 * Build/run: ctest -R cdrom_read_gate_test
 */
#include "cdrom_read_gate.h"

#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(expected, mode, xa, label) do {                                  \
    int actual = cdrom_read_mode_is_realtime((uint8_t)(mode), (xa));           \
    if (actual != (expected)) {                                                \
        fprintf(stderr, "FAIL: %s (mode=0x%02X xa=%d expected=%d got=%d)\n",   \
                (label), (unsigned)(mode), (xa), (expected), actual);          \
        failures++;                                                            \
    }                                                                          \
} while (0)

int main(void) {
    /* Ordinary data loads: may be accelerated, and then get flow control. */
    CHECK(0, 0x00, 0, "1x data read");
    CHECK(0, 0x80, 0, "2x data read");
    CHECK(0, 0xA0, 0, "2x whole-sector read (Tomba, Master of Arena) is not realtime");
    CHECK(0, 0x20, 0, "whole-sector bit alone is not realtime");

    /* Realtime media keeps the authentic cadence. */
    CHECK(1, 0xC0, 0, "XA-ADPCM");
    CHECK(1, 0x88, 0, "XA filter");
    CHECK(1, 0xE8, 0, "XA + filter + whole sector (STR)");
    CHECK(1, 0x80, 1, "an active XA stream");

    /* The rule for every mode: realtime exactly when 0x40 or 0x08 is set. */
    for (int m = 0; m < 256; m++)
        CHECK((m & 0x48) ? 1 : 0, m, 0, "bits 0x40 / 0x08 decide");

    if (failures) {
        fprintf(stderr, "FAILED (%d)\n", failures);
        return 1;
    }
    puts("ALL PASS");
    return 0;
}
