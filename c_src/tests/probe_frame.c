/* probe_frame.c - INIT/SINIT, PKYTST, and the checksum slice. */
#include <stdio.h>

#include "astdelux.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

extern bool ad_host_random_off;

int ad_probe(void)
{
    ad_host_random_off = true;              /* RANDOM = 0 throughout */
    /* (a) INIT with the headless host: OPTN1 = 0 (bonus at 0? table
     * byte 0), OPTN3 = 0 (2 lives + 2), CMODE = 0, RANDOM = 0. */
    g.zp.f.SCORE[0] = 0x12; g.zp.f.SCORE[5] = 0x34;
    AD_P->raw[0x19] = 0x55;
    ad_init();
    printf("SEDLAY=%02X EDELAY=%02X RDELAY=%02X DIFCTY=%02X THUMP3=%02X\n",
           AD_P->f.SEDLAY, AD_P->f.EDELAY, AD_P->f.RDELAY, AD_P->f.DIFCTY, AD_P->f.THUMP3);
    printf("UPDFLG=%02X %02X NHITS=%02X SDELAY=%02X SBTLT=%02X\n",
           g.zp.f.UPDFLG[0], g.zp.f.UPDFLG[1], g.zp.f.NHITS, AD_P->f.SDELAY, g.zp.f.SBTLT);
    printf("plateau $69/$6A=%02X %02X  $6C/$6D=%02X %02X  BONSCR+1/+2=%02X %02X\n",
           g.zp.f.PLATEU[1], g.zp.f.PLATEU[2], g.zp.f.PLATEU[4], g.zp.f.PLATEU[5],
           g.zp.f.BONSCR[1], g.zp.f.BONSCR[2]);
    printf("RVELP=%02X RVELM=%02X PERR=%02X  vram $4002-05: %02X %02X %02X %02X\n",
           g.zp.f.RVELP, g.zp.f.RVELM, g.zp.f.PERR[0],
           g.vram[2], g.vram[3], g.vram[4], g.vram[5]);
    CHECK(AD_P->f.SEDLAY == 0x98 && AD_P->f.EDELAY == 0x98 && AD_P->f.RDELAY == 0x7F
          && AD_P->f.DIFCTY == 6 && AD_P->f.THUMP3 == 0x30, "INIT delays");
    CHECK(g.zp.f.SCORE[0] == 0 && g.zp.f.SCORE[5] == 0, "scores cleared");
    CHECK(AD_P->raw[0x19] == 0, "page cleared");
    CHECK(g.zp.f.UPDFLG[0] == 0xFF && g.zp.f.UPDFLG[1] == 0xFF, "UPDFLG");
    /* SSBTLT is objects.c's; in this staging build it is a stub, so
     * SDELAY/SBTLT are checked only once objects.c is linked in. */
#ifdef AD_HAVE_objects
    CHECK(AD_P->f.SDELAY == 1 && g.zp.f.SBTLT == 5, "SSBTLT(1)");
#endif
    CHECK(g.vram[3] == 0xB0, "HALT at $4003");
    /* OPTN1 = 0 -> BONUS[0] = 0: plateau 0, ten-thousands 1 */
    CHECK(g.zp.f.PLATEU[1] == 0 && g.zp.f.PLATEU[2] == 1 && g.zp.f.BONSCR[1] == 0
          && g.zp.f.BONSCR[2] == 1, "plateau for OPTN1=0");
    /* OPTN3 = 0 -> 0 + 2 lives; bonus enabled (y != 3); CMODE 0 */
    CHECK(g.zp.f.NHITS == 2, "NHITS=%02X", g.zp.f.NHITS);
    /* RANDOM is 0 every frame here: four identical readings -> easy speeds,
     * PERR = $80 (the carry rotated into bit 7, OPTN5 = 0). */
    CHECK(g.zp.f.RVELP == 0x05 && g.zp.f.RVELM == 0xFB && g.zp.f.PERR[0] == 0x80,
          "PKYTST dead-POKEY path");

    /* (b) PKYTST with a changing history: fake the history so only the
     * cell just written matches -> normal speeds, PERR = 0. */
    g.zp.f.PERR[1] = 1; g.zp.f.PERR[2] = 2; g.zp.f.PERR[3] = 3; g.zp.f.PERR[4] = 4;
    g.zp.f.FRAME[0] = 2;                     /* writes $DE */
    ad_pkytst();
    CHECK(g.zp.f.PERR[3] == 0, "history cell $DE written");
    CHECK(g.zp.f.RVELP == 0x0A && g.zp.f.RVELM == 0xF6 && g.zp.f.PERR[0] == 0x00,
          "PKYTST normal: RVELP=%02X RVELM=%02X PERR=%02X",
          g.zp.f.RVELP, g.zp.f.RVELM, g.zp.f.PERR[0]);

    /* (c) The checksum: run 8 pages x 256 slices and see $8C come out 0
     * on the real ROM bytes, with HOLE = $FC afterwards. */
    g.zp.f.PROT[0] = g.zp.f.PROT[1] = g.zp.f.PROT[2] = g.zp.f.PROT[3] = 0;
    g.zp.f.HOLE = 0;
    for (int i = 0; i < 8 * 256; i++)
        ad_protck();
    printf("after 2048 slices: PROT=%02X %02X %02X %02X HOLE=%02X\n",
           g.zp.f.PROT[0], g.zp.f.PROT[1], g.zp.f.PROT[2], g.zp.f.PROT[3], g.zp.f.HOLE);
    CHECK(g.zp.f.PROT[2] == 0, "checksum signal $8C = %02X, should be 0", g.zp.f.PROT[2]);
    CHECK(g.zp.f.PROT[0] == 0 && g.zp.f.PROT[1] == 0 && g.zp.f.PROT[3] == 0, "PROT reset");
    CHECK(g.zp.f.HOLE == 0xFC, "HOLE");

    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 2 : 0;
}
