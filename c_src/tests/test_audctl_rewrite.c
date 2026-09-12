/* An AUDCTL write must not restart the timers: a rewrite of the current
 * value is a no-op, and a clock change keeps each timer's remaining count.
 * Major Havoc rewrites AUDCTL $78 (both pairs linked, 1.79 MHz) twice a
 * frame and Battlezone $00 every frame; re-arming on those writes restarted
 * every tone.  Checked on the cycle-audio path (what the games hear) and
 * the legacy renderer, against an identical chip that was left alone. */
#include <stdio.h>
#include <string.h>
#include "c012294.h"

static int fails;

static void drain(ad_pokey *p, int16_t *dst, int n)
{
    int got = ad_pokey_audio_read(p, dst, n);
    memset(dst + got, 0, (size_t)(n - got) * sizeof *dst);
}

static void check(const char *what, const int16_t *a, const int16_t *b, int n)
{
    if (memcmp(a, b, (size_t)n * sizeof *a)) { printf("FAIL: %s\n", what); fails++; }
    else printf("PASS: %s\n", what);
}

static void setup(ad_pokey *p, int cycle)
{
    ad_pokey_init(p, 1250000, 48000);
    if (cycle) ad_pokey_set_cycle_audio(p, 1);
    ad_pokey_write(p, W_SKCTL, 0); ad_pokey_write(p, W_SKCTL, 3);
    ad_pokey_write(p, W_AUDCTL, 0x78);
    ad_pokey_write(p, W_AUDF1, 0x0C); ad_pokey_write(p, W_AUDF2, 0x0C);
    ad_pokey_write(p, W_AUDF3, 0x17); ad_pokey_write(p, W_AUDF4, 0x18);
    ad_pokey_write(p, W_AUDC2, 0xA8); ad_pokey_write(p, W_AUDC4, 0xA6);
    ad_pokey_write(p, W_AUDC1, 0); ad_pokey_write(p, W_AUDC3, 0);
    ad_pokey_advance(p, 30000);
}

int main(void)
{
    static int16_t a[4000], b[4000];
    ad_pokey ref, dut;

    /* cycle audio: rewrite the same AUDCTL mid-tone, twice, like the game */
    setup(&ref, 1); setup(&dut, 1);
    drain(&ref, a, 1000); drain(&dut, b, 1000);
    for (int rep = 0; rep < 4; ++rep) {
        ad_pokey_write(&dut, W_AUDCTL, 0x78);
        ad_pokey_advance(&ref, 12500); ad_pokey_advance(&dut, 12500);
    }
    drain(&ref, a, 1900); drain(&dut, b, 1900);
    check("cycle audio: AUDCTL rewrite leaves the 16-bit tones untouched", a, b, 1900);

    /* cycle audio: a same-value rewrite before a real clock change must not
     * alter what the change does */
    setup(&ref, 1); setup(&dut, 1);
    ad_pokey_write(&dut, W_AUDCTL, 0x78);
    ad_pokey_write(&ref, W_AUDCTL, 0x18); ad_pokey_write(&dut, W_AUDCTL, 0x18);
    ad_pokey_advance(&ref, 40000); ad_pokey_advance(&dut, 40000);
    drain(&ref, a, 1500); drain(&dut, b, 1500);
    check("cycle audio: same clock change, same output", a, b, 1500);

    /* legacy renderer: same rewrite check */
    setup(&ref, 0); setup(&dut, 0);
    ad_pokey_render(&ref, a, 500); ad_pokey_render(&dut, b, 500);
    ad_pokey_write(&dut, W_AUDCTL, 0x78);
    ad_pokey_render(&ref, a, 2000); ad_pokey_render(&dut, b, 2000);
    check("legacy render: AUDCTL rewrite leaves the tones untouched", a, b, 2000);

    /* a timer must not lose its phase to an unrelated AUDCTL bit: flip the
     * poly-9 select only and expect the 16-bit tone period to continue */
    setup(&ref, 1); setup(&dut, 1);
    drain(&ref, a, 1000); drain(&dut, b, 1000);
    ad_pokey_write(&dut, W_AUDCTL, 0xF8);
    ad_pokey_advance(&ref, 25000); ad_pokey_advance(&dut, 25000);
    drain(&ref, a, 900); drain(&dut, b, 900);
    check("cycle audio: poly-9 select flip keeps pure-tone timers in phase", a, b, 900);

    printf("AUDCTL rewrite checks: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
