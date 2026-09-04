/* probe_sound.c - exercise sound.c in isolation.
 *
 *     build_mod.bat sound
 *     test_sound.exe --probe
 *
 * The host stub's ad_hw_pokey_write() discards its argument, but CSOUND
 * writes CURRENT[x] to POKEY+x for every channel every tick, so the
 * per-tick CURRENT column *is* the POKEY write log for that channel.
 */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"

#define SNDREC (AD_DASOUN + 0x60)               /* $766D */

static int fails;

static void check(bool ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        fails++;
}

static void clear_channels(void)
{
    memset(g.zp.f.POINT, 0, 8);
    memset(g.zp.f.CURRENT, 0, 8);
    memset(g.zp.f.FRAMES, 0, 8);
    memset(g.zp.f.COUNT, 0, 8);
    memset(g.zp.f.MCOUNT, 0, 8);
    memset(g.zp.f.MPNTR, 0, 8);
}

/* Print the PNTRS row for a sound and, for each channel it uses, where
 * its sequence lives in the DASOUN block. */
static void show_sound(uint8_t snd)
{
    uint16_t row = (uint16_t)(AD_PNTRS + (snd & 0xF8));
    printf("sound $%02X: PNTRS row at $%04X:", snd, row);
    for (int i = 0; i < 8; i++)
        printf(" %02X", ad_rom((uint16_t)(row + i)));
    printf("\n");
    for (int i = 0; i < 8; i++) {
        uint8_t off = ad_rom((uint16_t)(row + i));
        if (off == 0)
            continue;
        uint16_t cnt = (uint16_t)(SNDREC + off);
        printf("  ch%d offset $%02X -> repeat count at $%04X (=%02X), records from $%04X:",
               i, off, cnt, ad_rom(cnt), cnt + 1);
        for (uint16_t p = (uint16_t)(cnt + 1); ad_rom(p) != 0; p = (uint16_t)(p + 4))
            printf(" [%02X %02X %02X %02X]", ad_rom(p), ad_rom((uint16_t)(p + 1)),
                   ad_rom((uint16_t)(p + 2)), ad_rom((uint16_t)(p + 3)));
        printf(" term\n");
    }
}

static bool any_busy(void)
{
    for (int i = 0; i < 8; i++)
        if (g.zp.f.POINT[i])
            return true;
    return false;
}

int ad_probe(void)
{
    uint8_t lastpoint[8];

    printf("sound probe\n");

    /* --- ship fire in a game --------------------------------------- */
    g.zp.f.NPLAYR = 1;
    clear_channels();
    show_sound(0x27);
    ad_sndon(0x27);
    printf("after SNDON $27: POINT=%02X %02X MCOUNT=%02X %02X\n",
           g.zp.f.POINT[0], g.zp.f.POINT[1], g.zp.f.MCOUNT[0], g.zp.f.MCOUNT[1]);
    check(g.zp.f.POINT[0] == 0x3D && g.zp.f.POINT[1] == 0x44,
          "SNDON $27 loads channels 0 and 1 from the PNTRS row");
    check(g.zp.f.MCOUNT[0] == 0x80 && g.zp.f.MCOUNT[1] == 0x80,
          "SNDON sets MCOUNT bit 7 on both");
    check(g.zp.f.POINT[2] == 0 && g.zp.f.POINT[7] == 0,
          "other channels untouched");

    memcpy(lastpoint, g.zp.f.POINT, 8);
    printf("tick | ch0 POINT CUR FRM CNT MCNT MPTR | ch1 POINT CUR FRM CNT MCNT MPTR | POKEY writes\n");
    bool went_busy = false;
    int idle_at = -1;
    for (int t = 1; t <= 80; t++) {
        ad_csound();
        printf("%4d |     %02X    %02X  %02X  %02X  %02X   %02X |     %02X    %02X  %02X  %02X  %02X   %02X | AUDF1=%02X AUDC1=%02X",
               t,
               g.zp.f.POINT[0], g.zp.f.CURRENT[0], g.zp.f.FRAMES[0], g.zp.f.COUNT[0], g.zp.f.MCOUNT[0], g.zp.f.MPNTR[0],
               g.zp.f.POINT[1], g.zp.f.CURRENT[1], g.zp.f.FRAMES[1], g.zp.f.COUNT[1], g.zp.f.MCOUNT[1], g.zp.f.MPNTR[1],
               g.zp.f.CURRENT[0], g.zp.f.CURRENT[1]);
        for (int i = 0; i < 2; i++) {
            if (g.zp.f.POINT[i] != lastpoint[i]) {
                if (g.zp.f.POINT[i])
                    printf("  ch%d -> record $%04X", i, SNDREC + g.zp.f.POINT[i]);
                else
                    printf("  ch%d idle", i);
                lastpoint[i] = g.zp.f.POINT[i];
            }
        }
        printf("\n");
        if (g.zp.f.CURRENT[0] || g.zp.f.CURRENT[1])
            went_busy = true;
        if (idle_at < 0 && went_busy && !any_busy())
            idle_at = t;
    }
    check(went_busy, "channels produced output");
    check(idle_at > 0, "both channels returned to idle");
    printf("  idle at tick %d\n", idle_at);
    check(g.zp.f.CURRENT[0] == 0 && g.zp.f.CURRENT[1] == 0,
          "idle channels write 0 to POKEY");

    /* --- SNDOFF ----------------------------------------------------- */
    ad_sndon(0x27);
    for (int t = 0; t < 3; t++)
        ad_csound();
    check(g.zp.f.POINT[0] != 0 && g.zp.f.CURRENT[0] != 0, "restarted, running");
    ad_sndoff(0x27);
    check(g.zp.f.POINT[0] == 0 && g.zp.f.POINT[1] == 0, "SNDOFF $27 zeroes POINT on both channels");
    ad_csound();
    check(g.zp.f.CURRENT[0] == 0 && g.zp.f.CURRENT[1] == 0, "next tick writes 0 to POKEY");

    /* --- SNDPON on a busy channel ----------------------------------- */
    ad_sndon(0x27);
    for (int t = 0; t < 5; t++)
        ad_csound();
    {
        uint8_t p0 = g.zp.f.POINT[0], c0 = g.zp.f.CURRENT[0], f0 = g.zp.f.FRAMES[0];
        uint8_t m0 = g.zp.f.MCOUNT[0];
        ad_sndpon(0x27);
        check(g.zp.f.POINT[0] == p0 && g.zp.f.CURRENT[0] == c0 && g.zp.f.FRAMES[0] == f0 &&
              g.zp.f.MCOUNT[0] == m0,
              "SNDPON leaves a busy channel alone");
        ad_sndon(0x27);
        check(g.zp.f.POINT[0] == 0x3D && g.zp.f.MCOUNT[0] == 0x80,
              "SNDON restarts a busy channel");
    }
    /* SNDPON on a mix: silence channel 1 only, then SNDPON should take
     * channel 1 and leave channel 0. */
    for (int t = 0; t < 5; t++)
        ad_csound();
    g.zp.f.POINT[1] = 0;
    {
        uint8_t c0 = g.zp.f.CURRENT[0];
        ad_sndpon(0x27);
        check(g.zp.f.POINT[1] == 0x44 && g.zp.f.MCOUNT[1] == 0x80 && g.zp.f.CURRENT[0] == c0 &&
              g.zp.f.MCOUNT[0] != 0x80,
              "SNDPON starts only the idle channel of a pair");
    }

    /* --- attract mode: SNDON becomes SNDOFF ------------------------- */
    ad_sndon(0x27);
    for (int t = 0; t < 3; t++)
        ad_csound();
    g.zp.f.NPLAYR = 0;
    ad_sndon(0x27);
    check(g.zp.f.POINT[0] == 0 && g.zp.f.POINT[1] == 0,
          "NPLAYR=0: SNDON $27 acts as SNDOFF");
    g.zp.f.NPLAYR = 0;
    clear_channels();
    ad_sndpon(0x27);
    check(!any_busy(), "NPLAYR=0: SNDPON starts nothing");
    g.zp.f.NPLAYR = 1;

    /* --- INISOU ----------------------------------------------------- */
    for (int i = 0; i < 8; i++)
        g.zp.f.POINT[i] = (uint8_t)(0x11 * (i + 1));
    ad_inisou();
    check(!any_busy(), "INISOU zeroes all 8 POINT entries");

    /* --- DIASND ----------------------------------------------------- */
    clear_channels();
    show_sound(0x3F);
    AD_P->f.SPROCK = 2;
    ad_diasnd();
    check(AD_P->f.SPROCK == 1 && !any_busy(), "DIASND: not the last rock, no sound");
    ad_diasnd();
    check(AD_P->f.SPROCK == 0 && g.zp.f.POINT[2] == 0x5D && g.zp.f.POINT[3] == 0x64,
          "DIASND: last rock starts $3F on channels 2 and 3");

    /* --- SOUNDS: thump and the discrete circuits -------------------- */
    clear_channels();
    show_sound(0x07);
    show_sound(0x0F);
    g.zp.f.NPLAYR = 1;
    AD_P->f.NROCKS = 4;
    AD_P->f.THUMP3 = 8;
    g.zp.f.THUMP1 = 0;
    g.zp.f.THUMP2 = 0;
    g.zp.f.LTHUMP = 0;
    g.zp.f.LEXPSND = 0x08 | 0x40;               /* volume 2, pitch 1 */
    g.zp.f.R0 = 0xFF;
    /* One frame with no ship on screen, as at the start of a wave: that
     * is the SOUNDS_27 path, which primes THUMP2 from THUMP3.  (With
     * THUMP2 left at 0 the ROM's `dec THUMP2` wraps to $FF and the first
     * thump is 256 frames away - faithfully.) */
    AD_P->f.SHPPIX = 0x00;
    ad_sounds();
    check(g.zp.f.THUMP2 == 8, "no ship yet: THUMP2 primed from THUMP3");
    AD_P->f.SHPPIX = 0x01;                      /* ship on screen, not exploding */
    {
        int first_thump = -1, nthumps = 0;
        uint8_t started[8] = { 0 };
        for (int f = 1; f <= 40; f++) {
            ad_sounds();
            /* A thump that SNDPON just started still has MCOUNT = $80,
             * because CSOUND has not run since. */
            if (g.zp.f.POINT[6] && g.zp.f.MCOUNT[6] == 0x80) {
                if (first_thump < 0)
                    first_thump = f;
                if (nthumps < 8)
                    started[nthumps] = g.zp.f.POINT[6];
                nthumps++;
                printf("frame %2d: thump #%d started, LTHUMP=%d, ch6 offset $%02X ch7 offset $%02X\n",
                       f, nthumps, g.zp.f.LTHUMP, g.zp.f.POINT[6], g.zp.f.POINT[7]);
            }
            for (int t = 0; t < 4; t++)
                ad_csound();
            if (f <= 12 || (f % 8) == 0)
                printf("frame %2d: THUMP1=%d THUMP2=%d LTHUMP=%d POINT6/7=%02X/%02X LEXPSND=%02X expsnd=%02X thrust=%d R0=%02X\n",
                       f, g.zp.f.THUMP1, g.zp.f.THUMP2, g.zp.f.LTHUMP,
                       g.zp.f.POINT[6], g.zp.f.POINT[7], g.zp.f.LEXPSND, g.expsnd, g.thrust_on, g.zp.f.R0);
        }
        check(first_thump == 8, "first thump after THUMP3 (8) frames");
        check(nthumps == 3 && started[0] == 0x0F && started[1] == 0x01 && started[2] == 0x0F,
              "thumps every 12 frames, alternating $0F (LTHUMP odd) then $07 (even)");
    }
    check(g.zp.f.LEXPSND == 0x40 && g.expsnd == 0x40,
          "explosion level counts down one a frame to zero volume, pitch bits kept");
    check(!g.thrust_on, "thrust off while the switch stub reads 0");
    check(g.zp.f.R0 == 0, "R0 shifted clean by the thrust sampling");

    /* Ship destroyed: thump held off, THUMP2 reloaded from THUMP3. */
    AD_P->f.SHPPIX = 0x80;
    g.zp.f.THUMP2 = 1;
    ad_sounds();
    check(g.zp.f.THUMP2 == 8, "ship exploding: THUMP2 reloaded from THUMP3");

    /* Extra life request waits for channel 7. */
    clear_channels();
    g.zp.f.SND3 = 1;
    g.zp.f.POINT[7] = 0x08;                     /* busy */
    ad_sounds();
    check(g.zp.f.SND3 == 1, "SND3 request deferred while channel 7 busy");
    g.zp.f.POINT[7] = 0;
    ad_sounds();
    check(g.zp.f.SND3 == 0 && g.zp.f.POINT[7] != 0, "SND3 request honoured: $2F started, flag cleared");
    show_sound(0x2F);

    /* Attract mode: explosion reset, no thump. */
    clear_channels();
    g.zp.f.NPLAYR = 0;
    g.zp.f.LEXPSND = 0x7C;
    g.zp.f.R0 = 0x80;
    ad_sounds();
    check(g.zp.f.LEXPSND == 0 && g.expsnd == 0 && !g.thrust_on && !any_busy(),
          "NPLAYR=0: explosion reset, thrust off, nothing started");

    printf("%s (%d failures)\n", fails ? "PROBE FAILED" : "probe passed", fails);
    return fails ? 1 : 0;
}
