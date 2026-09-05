/* plat_teensy.h - what the sketch needs from the backend beyond the
 * ad_platform.h contract. */
#ifndef PLAT_TEENSY_H
#define PLAT_TEENSY_H

double plat_teensy_last_frame_ms(void);   /* plat_now_ms() of the last present */
void   plat_teensy_test_pattern(void);    /* draw the bring-up pattern once (VEC_TEST_PATTERN) */

#endif
