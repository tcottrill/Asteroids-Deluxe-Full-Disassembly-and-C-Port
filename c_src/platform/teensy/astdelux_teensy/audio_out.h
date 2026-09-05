/* audio_out.h - the Teensy audio output: audio_mix's ring pulled by an
 * AudioStream into AudioOutputPT8211 (pins 7/20/21).  Implements the
 * plat_audio_* and plat_sample_* half of ad_platform.h. */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

void audio_out_setup(void);   /* hand the sample table to audio_mix; call from plat_init */

#endif
