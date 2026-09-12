/* probe_c012294_bench.c - wall-clock cost of the two-chip cycle-audio
 * feed the game runs: ten seconds of machine time in 6144-cycle IRQ
 * ticks with the shield on POKEY1 and thrust + the force-field hum on
 * POKEY2, drained every tick the way app_loop.c does.  Prints seconds of
 * CPU per second of machine time, and a hash of the summed output so a
 * faster core can be checked against a slower one. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "c012294.h"

int main(void)
{
    ad_pokey p[2];
    static int16_t buf[2][512];
    uint64_t phase = 0;
    uint32_t hash = 2166136261u;
    const int ticks = 2461;               /* ~10 s of machine time */
    for (int i = 0; i < 2; ++i) {
        ad_pokey_init(&p[i], 1512000, 44100);
        ad_pokey_set_cycle_audio(&p[i], true);
        ad_pokey_write(&p[i], W_SKCTL, 0); ad_pokey_write(&p[i], W_SKCTL, 7);
    }
    ad_pokey_write(&p[0], W_AUDF1, 0xB0);
    ad_pokey_write(&p[1], W_AUDF1, 0x02); ad_pokey_write(&p[1], W_AUDC1, 0x84);
    ad_pokey_write(&p[1], W_AUDCTL, 0x01);
    ad_pokey_write(&p[1], W_AUDF2, 0x60); ad_pokey_write(&p[1], W_AUDC2, 0xA3);
    clock_t t0 = clock();
    for (int t = 0; t < ticks; ++t) {
        static const uint8_t vols[4] = { 0xC0, 0xC2, 0xC4, 0xC6 };
        ad_pokey_advance(&p[0], 6144);
        ad_pokey_advance(&p[1], 6144);
        ad_pokey_write(&p[0], W_AUDC1, vols[t & 3]);
        if ((t & 3) == 0) { ad_pokey_write(&p[0], W_AUDF1, 0xB0); ad_pokey_write(&p[1], W_AUDCTL, 0x01); }
        phase += 44100ull * 6144;
        int n = (int)(phase / 1512000); phase %= 1512000;
        for (int i = 0; i < 2; ++i) ad_pokey_audio_read(&p[i], buf[i], n);
        for (int j = 0; j < n; ++j) {
            int v = buf[0][j] + buf[1][j];
            hash = (hash ^ (uint16_t)v) * 16777619u;
        }
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("bench: %.3f s CPU for %.2f s machine time (%.1f%%)  hash %08X\n",
           s, ticks * 6144.0 / 1512000.0, 100.0 * s / (ticks * 6144.0 / 1512000.0), hash);
    return 0;
}
