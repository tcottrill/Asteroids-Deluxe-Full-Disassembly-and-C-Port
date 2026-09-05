/* audio_out.cpp - see audio_out.h.
 *
 * AudioPokeyStream has no inputs; its update() (Audio Library software
 * interrupt, every 128 frames at 44117.6 Hz) pulls one block from the ring
 * audio_mix fills on the main side.  Both PT8211 channels get the same
 * block: the board mixes AUD_L and AUD_R into one amplifier.  The objects
 * are static so the graph exists from boot; until plat_audio_open has run
 * and the ring has filled to AUDIO_RING_START, update() emits silence.
 */
#include <Arduino.h>
#include <Audio.h>
#include "teensy_config.h"
#include "audio_mix.h"
#include "audio_out.h"
#include "src/samples_data.h"

extern "C" {
#include "src/core/platform/ad_platform.h"
}

class AudioPokeyStream : public AudioStream {
public:
    AudioPokeyStream() : AudioStream(0, NULL) {}
    virtual void update(void)
    {
        audio_block_t *b = allocate();
        if (!b) return;
        audio_mix_pull(b->data, AUDIO_BLOCK_SAMPLES);
        transmit(b, 0);
        release(b);
    }
};

static AudioPokeyStream    pokey_stream;
static AudioOutputPT8211   pt8211;
static AudioConnection     patch_l(pokey_stream, 0, pt8211, 0);
static AudioConnection     patch_r(pokey_stream, 0, pt8211, 1);

void audio_out_setup(void)
{
    audio_mix_init();
    audio_mix_set_samples(ad_smp_data, ad_smp_len, AD_SMP_COUNT);
}

extern "C" {

int plat_audio_open(int sample_rate)
{
    (void)sample_rate;          /* 44100 from the core; the ring's adapter absorbs
                                 * the 44117.6 Hz the I2S clock actually runs at */
    AudioMemory(8);
    return 0;
}

void plat_audio_push(const int16_t *pcm, int frames)
{
    audio_mix_push(pcm, frames);
}

void plat_audio_close(void)
{
}

void plat_sample_start(int channel, int sample, int loop)
{
    audio_mix_sample_start(channel, sample, loop);
}

void plat_sample_stop(int channel)
{
    audio_mix_sample_stop(channel);
}

}
