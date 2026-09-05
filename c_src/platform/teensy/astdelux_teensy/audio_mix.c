/* audio_mix.c - ring buffer, rate adapter and sample mixer.  See audio_mix.h.
 *
 * Single producer, single consumer: `head` is written only by push, `tail`
 * only by pull, both volatile 32-bit counters that wrap naturally; fill is
 * head - tail.  AUDIO_RING_SIZE is a power of two so index = counter & MASK.
 * No locks: on the Teensy the consumer runs in the Audio Library's software
 * interrupt and the producer in loop().
 */
#include <string.h>
#include "teensy_config.h"
#include "audio_mix.h"

/* On the Teensy 4 put the ring in RAM2 (the core's DMAMEM section): RAM1 is
 * nearly full.  Not zero-initialised there, which is fine - only frames
 * between tail and head are ever read, and both start at 0. */
#if defined(__IMXRT1062__)
#define RING_RAM2 __attribute__((section(".dmabuffers"), used))
#else
#define RING_RAM2
#endif

#define RING_MASK (AUDIO_RING_SIZE - 1)

typedef struct {
    int      sample;
    uint32_t pos;
    uint8_t  loop;
    uint8_t  active;
} mix_chan;

static int16_t           ring[AUDIO_RING_SIZE] RING_RAM2;
static volatile uint32_t head, tail;
static volatile int      started;
static uint32_t          overruns, underruns;

static const uint8_t *const *smp_data;
static const uint32_t       *smp_len;
static int                   smp_count;
static mix_chan              chan[AUDIO_CHANNELS];

void audio_mix_init(void)
{
    head = tail = 0;
    started = 0;
    overruns = underruns = 0;
    memset(chan, 0, sizeof chan);
    /* the sample table survives init: the sketch sets it once at boot */
}

void audio_mix_set_samples(const uint8_t *const *data, const uint32_t *len, int count)
{
    smp_data = data;
    smp_len = len;
    smp_count = count;
}

void audio_mix_sample_start(int channel, int sample, int loop)
{
    if (channel < 0 || channel >= AUDIO_CHANNELS) return;
    if (sample < 0 || sample >= smp_count || smp_len[sample] == 0) return;
    chan[channel].sample = sample;
    chan[channel].pos = 0;
    chan[channel].loop = loop ? 1 : 0;
    chan[channel].active = 1;
}

void audio_mix_sample_stop(int channel)
{
    if (channel < 0 || channel >= AUDIO_CHANNELS) return;
    chan[channel].active = 0;
}

int audio_mix_sample_active(int channel)
{
    if (channel < 0 || channel >= AUDIO_CHANNELS) return 0;
    return chan[channel].active;
}

static int16_t clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

int16_t audio_mix_next(int16_t pokey)
{
    int32_t acc = ((int32_t)pokey * AUDIO_POKEY_GAIN) / 256;
    int c;
    for (c = 0; c < AUDIO_CHANNELS; c++) {
        mix_chan *ch = &chan[c];
        if (!ch->active) continue;
        acc += ((int32_t)smp_data[ch->sample][ch->pos] - 128) * AUDIO_SAMPLE_GAIN;
        if (++ch->pos >= smp_len[ch->sample]) {
            ch->pos = 0;
            if (!ch->loop) ch->active = 0;
        }
    }
    return clamp16(acc);
}

void audio_mix_push(const int16_t *pokey, int frames)
{
    int i;
    for (i = 0; i < frames; i++) {
        int16_t v = audio_mix_next(pokey[i]);    /* advance the samples even when dropping */
        if (head - tail >= AUDIO_RING_SIZE) {
            overruns++;
            continue;
        }
        ring[head & RING_MASK] = v;
        head++;
    }
}

void audio_mix_pull(int16_t *dst, int frames)
{
    uint32_t fill = head - tail;
    int dup = 0, skip = 0, i;

    if (!started) {
        if (fill < AUDIO_RING_START) {
            memset(dst, 0, (size_t)frames * sizeof *dst);
            return;                                  /* gated: not an underrun */
        }
        started = 1;
    }
    if (fill < AUDIO_RING_LOW) dup = 1;              /* consumer runs slow this block */
    else if (fill > AUDIO_RING_HIGH) skip = 1;       /* consumer runs fast this block */

    for (i = 0; i < frames; i++) {
        if (head - tail == 0) {
            dst[i] = 0;
            underruns++;
            continue;
        }
        dst[i] = ring[tail & RING_MASK];
        if (dup) {
            dup = 0;                                 /* emit this frame again next time */
        } else {
            tail++;
            if (skip && head - tail > 0) {
                tail++;                              /* drop one frame */
                skip = 0;
            }
        }
    }
}

uint32_t audio_mix_fill(void)      { return head - tail; }
uint32_t audio_mix_overruns(void)  { return overruns; }
uint32_t audio_mix_underruns(void) { return underruns; }
