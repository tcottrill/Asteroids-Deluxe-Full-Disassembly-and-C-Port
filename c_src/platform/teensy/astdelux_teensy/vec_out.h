/* vec_out.h - the Teensy vector output: two DAC7811 on SPI0 for X/Y, five
 * GPIOs through a 74HC04 for the Z ladder.  Draws a frame's segment list
 * synchronously (plat_video_present calls vec_out_draw and waits). */
#ifndef VEC_OUT_H
#define VEC_OUT_H

#include <stdint.h>
#include "vec_beam.h"

void     vec_out_init(void);                          /* Z off first, SPI up, DACs to mid-scale */
void     vec_out_draw(const vec_seg *segs, int n);    /* draw one frame; Z off at the end */
void     vec_out_park(void);                          /* Z off, beam to the centre */
uint32_t vec_out_last_frame_us(void);                 /* draw time of the last frame */

#endif
