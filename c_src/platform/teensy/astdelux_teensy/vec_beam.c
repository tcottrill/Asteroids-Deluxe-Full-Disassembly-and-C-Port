/* vec_beam.c - the pure part of the vector output.  See vec_beam.h.
 *
 * Geometry is the stock DVG's: 10-bit counters and DACs, 1024x1024, centre
 * 512.  The DAC7811 is 12-bit, so a DVG code shifts left by VEC_SHIFT and
 * lands on 0..4092 with 512 at mid-scale (teensy_config.h).
 */
#include <math.h>
#include "teensy_config.h"
#include "vec_beam.h"

int16_t vec_map(float v, int flip)
{
    int c = (int)floorf(v + 0.5f);
    if (c < 0) c = 0;
    if (c > 1023) c = 1023;
    c <<= VEC_SHIFT;
    if (flip) c = VEC_DAC_MAX - c;
    return (int16_t)c;
}

uint16_t vec_dac_word(uint16_t code)
{
    return (uint16_t)(0x1000u | (code & 0x0FFFu));   /* C3..C0 = 0001: load and update */
}

uint8_t vec_z_code(int z)
{
    static const uint8_t table[16] = VEC_Z_TABLE;
    return table[z & 15];
}

uint8_t vec_z_pins(uint8_t code)
{
    uint8_t bits = (uint8_t)(code & 0x1F);
#if VEC_Z_INVERT
    bits ^= 0x1F;
#endif
    return bits;
}

int vec_plan(int x0, int y0, int x1, int y1, int step, vec_pt *out, int max)
{
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int len = ax > ay ? ax : ay;
    int n, i;

    if (len == 0 || max <= 0) return 0;
    if (step < 1) step = 1;
    n = (len + step - 1) / step;
    if (n > max) n = max;
    for (i = 1; i <= n; i++) {
        /* integer interpolation: i == n gives exactly (x1, y1) */
        out[i - 1].x = (int16_t)(x0 + dx * i / n);
        out[i - 1].y = (int16_t)(y0 + dy * i / n);
    }
    return n;
}
