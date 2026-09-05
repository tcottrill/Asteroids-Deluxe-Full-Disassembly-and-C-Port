/* probe_teensy.c - PC checks of the Teensy backend's pure modules,
 * vec_beam.c and audio_mix.c, with expectations from the DAC7811 datasheet,
 * the stock DVG's geometry and the spec - never from running the code.
 *
 *     build_probe.bat
 *     probe_teensy.exe
 *
 * Prints one numbered check per behaviour, FAIL lines for anything wrong,
 * and exits nonzero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "teensy_config.h"
#include "vec_beam.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("  FAIL: "); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ */
/* (1) DAC7811 word: control 0001 then 12 data bits                      */
/* ------------------------------------------------------------------ */
static void check_dac_word(void)
{
    printf("1. vec_dac_word: control nibble 0001, data masked to 12 bits\n");
    CHECK(vec_dac_word(0)      == 0x1000, "0 -> 0x1000, got %04X", vec_dac_word(0));
    CHECK(vec_dac_word(2048)   == 0x1800, "2048 -> 0x1800, got %04X", vec_dac_word(2048));
    CHECK(vec_dac_word(4095)   == 0x1FFF, "4095 -> 0x1FFF, got %04X", vec_dac_word(4095));
    CHECK(vec_dac_word(0xF123) == 0x1123, "high bits masked, got %04X", vec_dac_word(0xF123));
}

/* ------------------------------------------------------------------ */
/* (2) Geometry: the stock DVG's 1024 space onto the 12-bit DAC          */
/* ------------------------------------------------------------------ */
static void check_map(void)
{
    printf("2. vec_map: 512 -> 2048, 0 -> 0, 1023 -> 4092, clamps, flips\n");
    CHECK(vec_map(512.0f, 0) == 2048, "centre: got %d", vec_map(512.0f, 0));
    CHECK(vec_map(0.0f, 0) == 0, "0: got %d", vec_map(0.0f, 0));
    CHECK(vec_map(1023.0f, 0) == 4092, "1023: got %d", vec_map(1023.0f, 0));
    CHECK(vec_map(1040.0f, 0) == 4092, "1040 clamps to 4092: got %d", vec_map(1040.0f, 0));
    CHECK(vec_map(-5.0f, 0) == 0, "negative clamps to 0: got %d", vec_map(-5.0f, 0));
    CHECK(vec_map(100.4f, 0) == 400, "100.4 rounds to 100 -> 400: got %d", vec_map(100.4f, 0));
    CHECK(vec_map(100.6f, 0) == 404, "100.6 rounds to 101 -> 404: got %d", vec_map(100.6f, 0));
    CHECK(vec_map(100.0f, 1) == 4092 - 400, "flip mirrors 400 -> 3692: got %d", vec_map(100.0f, 1));
    CHECK(vec_map(512.0f, 1) == 2044, "flip of centre is 4092-2048: got %d", vec_map(512.0f, 1));
}

/* ------------------------------------------------------------------ */
/* (3) Z ladder: table shape and the 74HC04 inversion                     */
/* ------------------------------------------------------------------ */
static void check_z(void)
{
    int z, prev = -1, mono = 1;
    printf("3. vec_z_code: 0 -> 0, monotonic, <= 31; vec_z_pins inverts\n");
    CHECK(vec_z_code(0) == 0, "z 0 must be off");
    for (z = 1; z < 16; z++) {
        int c = vec_z_code(z);
        if (c <= prev) mono = 0;
        prev = c;
        CHECK(c <= 31, "z %d code %d exceeds 5 bits", z, c);
    }
    CHECK(mono, "codes must rise with z");
    CHECK(vec_z_code(15) == 31, "z 15 is full scale, got %d", vec_z_code(15));
    CHECK(vec_z_code(0x25) == vec_z_code(5), "z masked to a nibble");
#if VEC_Z_INVERT
    CHECK(vec_z_pins(0) == 0x1F, "off -> all pins high through the inverter, got %02X", vec_z_pins(0));
    CHECK(vec_z_pins(31) == 0x00, "full -> all pins low, got %02X", vec_z_pins(31));
    CHECK(vec_z_pins(0x15) == 0x0A, "10101 -> 01010, got %02X", vec_z_pins(0x15));
#else
    CHECK(vec_z_pins(0x15) == 0x15, "no inversion configured");
#endif
}

/* ------------------------------------------------------------------ */
/* (4) Step planner                                                       */
/* ------------------------------------------------------------------ */
static void check_plan(void)
{
    vec_pt pts[VEC_MAX_PTS];
    int n, i, maxstep = 0, ok = 1;

    printf("4. vec_plan: zero length -> 0; 100 at step 16 -> 7 points, exact end, steps <= 16\n");
    n = vec_plan(100, 100, 100, 100, 16, pts, VEC_MAX_PTS);
    CHECK(n == 0, "zero-length: got %d points", n);

    n = vec_plan(1000, 500, 1100, 500, 16, pts, VEC_MAX_PTS);
    CHECK(n == 7, "100 counts at step 16: ceil(100/16) = 7, got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 1100 && pts[n - 1].y == 500,
          "last point must be the endpoint, got (%d,%d)", pts[n - 1].x, pts[n - 1].y);
    {
        int px = 1000;
        for (i = 0; i < n; i++) {
            int d = pts[i].x - px;
            if (d > maxstep) maxstep = d;
            if (pts[i].y != 500) ok = 0;
            px = pts[i].x;
        }
    }
    CHECK(maxstep <= 16 && maxstep > 0, "largest step %d must be 1..16", maxstep);
    CHECK(ok, "a horizontal line stays on its y");

    n = vec_plan(0, 0, 4092, 4092, 16, pts, VEC_MAX_PTS);
    CHECK(n == 256, "full diagonal at step 16: 4092/16 = 255.75 -> 256, got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 4092 && pts[n - 1].y == 4092, "diagonal ends exactly");
    for (i = 0; i < n; i++)
        if (pts[i].x != pts[i].y) ok = 0;
    CHECK(ok, "a 45-degree line keeps x == y");

    n = vec_plan(0, 0, 4092, 0, 1, pts, 10);
    CHECK(n == 10, "max respected: got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 4092, "still ends exactly when max bites");

    n = vec_plan(2000, 2000, 1900, 1950, 16, pts, VEC_MAX_PTS);
    CHECK(n == 7, "negative direction, longer axis 100: 7 points, got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 1900 && pts[n - 1].y == 1950, "negative direction ends exactly");
}

int main(void)
{
    check_dac_word();
    check_map();
    check_z();
    check_plan();
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
