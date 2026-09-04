/* probe_objects.c - the object engine against known inputs.
 *
 * The headless host's RANDOM is 0 and its switches read 0, so every
 * "random" choice here is the all-zero one: picture 0, X axis, edge 0.
 * Build with draw.c too (`build_mod.bat objects draw`) and the frame
 * dump at the end shows the wave through PICTUR.
 */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"

static int fails;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PG     (AD_P->raw)
#define OBJ(i)   PG[AD_OBJ + (i)]
#define XINC(i)  PG[AD_XINC + (i)]
#define YINC(i)  PG[AD_YINC + (i)]
#define OBJXH(i) PG[AD_OBJXH + (i)]
#define OBJYH(i) PG[AD_OBJYH + (i)]
#define OBJXL(i) PG[AD_OBJXL + (i)]
#define OBJYL(i) PG[AD_OBJYL + (i)]

extern bool ad_host_random_off;

static void fresh(void)
{
    memset(&g, 0, sizeof g);
    ad_host_random_off = true;              /* every random choice is the zero one */
    g.zp.f.RVELP = 0x0A;                    /* what PKYTST sets, normal speeds */
    g.zp.f.RVELM = 0xF6;
    AD_P->f.DIFCTY = 0x06;                  /* as INIT leaves it */
    g.zp.f.PLATEU[1] = 0x00; g.zp.f.PLATEU[2] = 0x01;   /* bonus life at 10 000,
                                             * so POINTS does not hand one out */
    g.zp.f.BONSCR[1] = 0x00; g.zp.f.BONSCR[2] = 0x01;
}

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(g.vram, 1, sizeof g.vram, f); fclose(f); printf("wrote %s\n", path); }
}

int ad_probe(void)
{
    int i;

    /* (a) NEWAST in attract mode, first wave. */
    fresh();
    ad_newast();
    printf("(a) NEWAST: NROCKS=%u SROCKS=%u DIFCTY=%u EDELAY=%02X THUMP3=%02X\n",
           AD_P->f.NROCKS, AD_P->f.SROCKS, AD_P->f.DIFCTY, AD_P->f.EDELAY, AD_P->f.THUMP3);
    for (i = 0x18; i >= 0x12; i--)
        printf("    slot %2d: OBJ=%02X pos=%02X%02X,%02X%02X vel=%02X,%02X\n", i,
               OBJ(i), OBJXH(i), OBJXL(i), OBJYH(i), OBJYL(i), XINC(i), YINC(i));
    CHECK(AD_P->f.NROCKS == 6 && AD_P->f.SROCKS == 1 && AD_P->f.DIFCTY == 7, "wave 1 counts");
    CHECK(AD_P->f.EDELAY == 0x7F && AD_P->f.THUMP3 == 0x30, "wave 1 delays");
    for (i = 0x13; i <= 0x18; i++)
        CHECK(OBJ(i) == 0x04 && XINC(i) == 0x0A && YINC(i) == 0x0A, "rock %d spawned", i);
    for (i = 0; i <= 0x12; i++)
        CHECK(OBJ(i) == 0, "slot %d clear", i);

    /* (b) MOTION: velocities added, both wraps.  Ship slot stays 0. */
    OBJXH(0x18) = 0x1F; OBJXL(0x18) = 0xFA;        /* about to wrap X at $2000 */
    OBJYH(0x17) = 0x17; OBJYL(0x17) = 0xFA;        /* about to wrap Y at $1800 */
    ad_motion();
    printf("(b) MOTION: rock 24 -> %02X%02X,%02X%02X  rock 23 -> %02X%02X,%02X%02X\n",
           OBJXH(0x18), OBJXL(0x18), OBJYH(0x18), OBJYL(0x18),
           OBJXH(0x17), OBJXL(0x17), OBJYH(0x17), OBJYL(0x17));
    CHECK(OBJXH(0x18) == 0x00 && OBJXL(0x18) == 0x04, "X wrap $1FFA+$A -> $0004");
    CHECK(OBJYH(0x18) == 0x00 && OBJYL(0x18) == 0x0A, "Y from 0 + $A");
    CHECK(OBJYH(0x17) == 0x00 && OBJYL(0x17) == 0x04, "Y wrap $17FA+$A -> $0004");
    CHECK(OBJXH(0x13) == 0x00 && OBJXL(0x13) == 0x0A, "plain step");
    CHECK(g.zp.f.BINSCR == 0, "BINSCR for score 0");
    /* A negative velocity: -10 from 0 wraps X to $1FF6. */
    XINC(0x13) = 0xF6; OBJXL(0x13) = 0; OBJXH(0x13) = 0;
    ad_motion();
    CHECK(OBJXH(0x13) == 0x1F && OBJXL(0x13) == 0xF6, "negative X wrap -> %02X%02X",
          OBJXH(0x13), OBJXL(0x13));

    /* (c) An explosion runs down and frees the slot; the last rock
     * starts RDELAY. */
    fresh();
    OBJ(3) = 0xA0; AD_P->f.NROCKS = 1;
    for (i = 0; i < 200 && OBJ(3) != 0; i++)
        ad_motion();
    printf("(c) explosion took %d frames; NROCKS=%u RDELAY=%02X\n", i, AD_P->f.NROCKS, AD_P->f.RDELAY);
    CHECK(OBJ(3) == 0 && AD_P->f.NROCKS == 0 && AD_P->f.RDELAY == 0x7F, "explosion end");

    /* (d) Ship hits a large rock, no shields, game on: ship explodes,
     * loses a life, rock splits into two mediums and explodes. */
    fresh();
    g.zp.f.NPLAYR = 1; g.zp.f.HITS[0] = 3;
    OBJ(AD_SHIP) = 0x01; OBJXH(AD_SHIP) = 0x10; OBJXL(AD_SHIP) = 0x60; OBJYH(AD_SHIP) = 0x0C; OBJYL(AD_SHIP) = 0x60;
    OBJ(0) = 0x04; OBJXH(0) = 0x10; OBJXL(0) = 0x60; OBJYH(0) = 0x0C; OBJYL(0) = 0x60;
    XINC(0) = 0x0A; YINC(0) = 0xF6;
    AD_P->f.NROCKS = 1;
    ad_colide();
    printf("(d) ship vs rock: SHPPIX=%02X OBJ0=%02X NROCKS=%u HITS=%u SDELAY=%02X CHIST=%02X %02X\n",
           OBJ(AD_SHIP), OBJ(0), AD_P->f.NROCKS, g.zp.f.HITS[0], AD_P->f.SDELAY,
           g.zp.f.CHIST[0], g.zp.f.CHIST[1]);
    CHECK(OBJ(AD_SHIP) == 0xA0 && XINC(AD_SHIP) == 0 && YINC(AD_SHIP) == 0, "ship explodes");
    CHECK(OBJ(0) == 0xA0, "rock explodes");
    CHECK(AD_P->f.NROCKS == 3, "two fragments added");
    CHECK(g.zp.f.HITS[0] == 2 && AD_P->f.SDELAY == 0x81 && g.zp.f.SBTLT == 5, "life taken");
    CHECK(g.zp.f.CHIST[1] == 0xFF, "no shield history");
    CHECK((OBJ(0x18) & 0x07) == 0x02 && (OBJ(0x17) & 0x07) == 0x02, "fragments medium: %02X %02X",
          OBJ(0x18), OBJ(0x17));
    CHECK(OBJXL(0x18) == (0x60 ^ ((XINC(0x18) & 0x1F) << 1)), "fragment X de-correlated");

    /* (e) A ship torpedo hits a medium rock: torpedo gone, two smalls. */
    fresh();
    g.zp.f.NPLAYR = 1;
    OBJ(AD_SHIP + 7) = 0x12; OBJXH(AD_SHIP + 7) = 0x05; OBJXL(AD_SHIP + 7) = 0x00; OBJYH(AD_SHIP + 7) = 0x05; OBJYL(AD_SHIP + 7) = 0x00;
    OBJ(4) = 0x0A; OBJXH(4) = 0x05; OBJXL(4) = 0x10; OBJYH(4) = 0x05; OBJYL(4) = 0x10;
    AD_P->f.NROCKS = 1;
    ad_colide();
    printf("(e) torpedo vs rock: torpedo=%02X OBJ4=%02X NROCKS=%u frags=%02X %02X RTIMER=%02X\n",
           OBJ(AD_SHIP + 7), OBJ(4), AD_P->f.NROCKS, OBJ(0x18), OBJ(0x17), AD_P->f.RTIMER);
    CHECK(OBJ(AD_SHIP + 7) == 0 && OBJ(4) == 0xA0 && AD_P->f.NROCKS == 3, "torpedo split");
    CHECK((OBJ(0x18) & 0x07) == 0x01 && (OBJ(0x17) & 0x07) == 0x01, "fragments small");
    CHECK(AD_P->f.RTIMER == 0x50, "RTIMER reset");
    /* Same again but far apart: no hit.  (Clear the fragments, which
     * sit at the rock's old position.) */
    OBJ(0x18) = 0; OBJ(0x17) = 0;
    OBJXH(4) = 0x10;
    OBJ(AD_SHIP + 7) = 0x12; OBJ(4) = 0x0A; AD_P->f.NROCKS = 1;
    ad_colide();
    CHECK(OBJ(AD_SHIP + 7) == 0x12 && OBJ(4) == 0x0A, "no hit when apart");

    /* (f) Shields: ship with shields hits a large rock -> bounce, no
     * damage to either, shields down by $B0 (+carry), CHIST records it. */
    fresh();
    g.zp.f.NPLAYR = 1; g.zp.f.HITS[0] = 3; g.zp.f.SHDON = 0x80; AD_P->f.SHLDS = 0xFF;
    g.zp.f.CHIST[1] = 0xFF;
    OBJ(AD_SHIP) = 0x01; OBJXH(AD_SHIP) = 0x10; OBJXL(AD_SHIP) = 0x60; OBJYH(AD_SHIP) = 0x0C; OBJYL(AD_SHIP) = 0x60;
    OBJ(2) = 0x04; OBJXH(2) = 0x10; OBJXL(2) = 0x70; OBJYH(2) = 0x0C; OBJYL(2) = 0x70;
    XINC(2) = 0x0A; YINC(2) = 0x0A;
    AD_P->f.NROCKS = 1;
    ad_colide();
    printf("(f) shielded: SHPPIX=%02X OBJ2=%02X SHLDS=%02X CHIST=%02X %02X ship vel=%02X,%02X HITS=%u\n",
           OBJ(AD_SHIP), OBJ(2), AD_P->f.SHLDS, g.zp.f.CHIST[0], g.zp.f.CHIST[1],
           XINC(AD_SHIP), YINC(AD_SHIP), g.zp.f.HITS[0]);
    CHECK(OBJ(AD_SHIP) == 0x01 && OBJ(2) == 0x04 && g.zp.f.HITS[0] == 3, "bounce: nothing destroyed");
    CHECK((g.zp.f.CHIST[0] & 0x80) && g.zp.f.CHIST[1] == 2, "collision history");
    CHECK(AD_P->f.SHLDS == 0xAF || AD_P->f.SHLDS == 0xB0, "shields -$50: %02X", AD_P->f.SHLDS);

    /* (g) SEARCH on a full field. */
    fresh();
    for (i = 0; i <= 0x18; i++) OBJ(i) = 4;
    CHECK(ad_search() == 0xFF, "search full");
    OBJ(7) = 0;
    CHECK(ad_search() == 7, "search finds 7");
    CHECK(ad_searc1(6) == 0xFF, "searc1 below 7");

    /* (h) A frame through PICTUR: the wave, drawn. */
    fresh();
    for (i = 0; i < 4; i++) {               /* the rock subroutines, as SETROL builds them */
        g.zp.f.FRAME[0] = (uint8_t)i;
        ad_rotast();
    }
    ad_newast();
    for (i = 0x13; i <= 0x18; i++) {        /* spread them out */
        OBJXH(i) = (uint8_t)(0x02 + 3 * (i - 0x13));
        OBJYH(i) = (uint8_t)(0x04 + 2 * (i - 0x13));
    }
    g.vram[0] = 0x01; g.vram[1] = 0xE0;     /* JMPL $4002 */
    ad_set_vglist(0x4002);
    ad_motion();
    ad_vghalt();
    printf("(h) frame list ends at $%04X\n", ad_vglist());
    dump("probe_objects.bin");

    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 2 : 0;
}
