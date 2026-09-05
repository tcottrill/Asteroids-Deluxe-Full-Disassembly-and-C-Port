/* vec_beam.h - the pure part of the vector output: coordinate mapping,
 * DAC7811 word packing, the Z ladder code and the step planner.  No
 * hardware, no Arduino; compiled into the sketch and into the PC probe. */
#ifndef VEC_BEAM_H
#define VEC_BEAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int16_t x, y; } vec_pt;              /* a DAC-code position */
typedef struct { int16_t x0, y0, x1, y1; uint8_t z; } vec_seg;  /* one lit segment, z = DVG nibble */

/* DVG beam coordinate (0..1023, floats from dvg.c) -> DAC code.  Rounds,
 * clamps to 0..1023, shifts by VEC_SHIFT; flip mirrors about the centre
 * (code -> VEC_DAC_MAX - code). */
int16_t  vec_map(float v, int flip);

/* The DAC7811 "load and update" word: control 0001, 12 data bits. */
uint16_t vec_dac_word(uint16_t code);

/* DVG intensity nibble -> 5-bit ladder code (VEC_Z_TABLE). */
uint8_t  vec_z_code(int z);

/* Ladder code -> the five pin levels, bit i = level of PIN_Z_Bi, after the
 * 74HC04 inversion when VEC_Z_INVERT. */
uint8_t  vec_z_pins(uint8_t code);

/* Points from (x0,y0) exclusive to (x1,y1) inclusive, spaced `step` DAC
 * counts along the longer axis; the last point is exactly (x1,y1).  Returns
 * the count, 0 for a zero-length segment, never more than `max` (spacing
 * widens to fit). */
int      vec_plan(int x0, int y0, int x1, int y1, int step, vec_pt *out, int max);

#ifdef __cplusplus
}
#endif

#endif
