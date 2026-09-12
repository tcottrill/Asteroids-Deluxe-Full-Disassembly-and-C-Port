/* A volume-only change must not stop/reseed the frequency/noise circuit.
 * Compare against an identical continuously audible channel, then unmute. */
#include <stdio.h>
#include <string.h>
#include "c012294.h"

int main(void)
{
    ad_pokey audible, muted;
    int16_t scratch[441], a[441], b[441];
    ad_pokey_init(&audible, 1512000, 44100);
    ad_pokey_write(&audible, W_SKCTL, 7);
    ad_pokey_write(&audible, W_AUDF1, 0xB0);
    ad_pokey_write(&audible, W_AUDC1, 0xC6);
    ad_pokey_render(&audible, scratch, 173);
    muted = audible;
    ad_pokey_write(&muted, W_AUDC1, 0xC0);
    ad_pokey_render(&audible, scratch, 441);
    ad_pokey_render(&muted, scratch, 441);
    ad_pokey_write(&muted, W_AUDC1, 0xC6);
    ad_pokey_render(&audible, a, 441);
    ad_pokey_render(&muted, b, 441);
    if (memcmp(a, b, sizeof a)) {
        puts("FAIL: muting changed the shield oscillator phase");
        return 1;
    }
    puts("PASS: shield oscillator keeps phase through zero volume");
    return 0;
}
