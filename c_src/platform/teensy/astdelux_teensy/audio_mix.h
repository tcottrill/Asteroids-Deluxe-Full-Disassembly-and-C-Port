/* audio_mix.h - the pure part of the Teensy audio path.
 *
 * A single-producer / single-consumer ring of mono int16 frames: the core
 * pushes its POKEY render (plat_audio_push, main context, ~176 frames per
 * 4 ms tick) and the Audio Library pulls 128 frames per block (interrupt
 * context).  The two clocks differ (44100 vs 44117.6 Hz) and the producer
 * stalls while a frame is drawn, so the consumer runs a one-frame-per-pull
 * rate adapter: duplicate a frame when the ring is low, skip one when it is
 * high.  Output is silent until the ring first fills to AUDIO_RING_START.
 *
 * The two cabinet-sample channels (explosion, thrust) are mixed into the
 * POKEY stream on the push side.  Samples are 8-bit unsigned mono at the
 * ring's rate; the table is handed in by audio_mix_set_samples so the probe
 * can supply its own.
 */
#ifndef AUDIO_MIX_H
#define AUDIO_MIX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_mix_init(void);
void audio_mix_set_samples(const uint8_t *const *data, const uint32_t *len, int count);

/* plat_sample_start/stop's contract: channel 0..AUDIO_CHANNELS-1, sample
 * index into the table, loop != 0 repeats until stopped. */
void audio_mix_sample_start(int channel, int sample, int loop);
void audio_mix_sample_stop(int channel);

/* Mix one frame: the POKEY value scaled by AUDIO_POKEY_GAIN/256 plus every
 * active sample channel's next byte (advancing it), clamped to int16.  Public
 * so the probe can check the arithmetic without the ring in the way. */
int16_t audio_mix_next(int16_t pokey);

void audio_mix_push(const int16_t *pokey, int frames);   /* producer: audio_mix_next into the ring */
void audio_mix_pull(int16_t *dst, int frames);           /* consumer */

uint32_t audio_mix_fill(void);       /* frames waiting */
uint32_t audio_mix_overruns(void);   /* frames dropped on push */
uint32_t audio_mix_underruns(void);  /* frames of silence on pull */
int      audio_mix_sample_active(int channel);

#ifdef __cplusplus
}
#endif

#endif
