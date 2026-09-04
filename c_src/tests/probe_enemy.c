/* probe_enemy.c - saucer launch and the special-rock cluster.
 * Build with objects.c and mathrom.c: `build_mod.bat enemy objects mathrom`. */
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
    g.zp.f.RVELP = 0x0A; g.zp.f.RVELM = 0xF6;
    AD_P->f.DIFCTY = 0x06; AD_P->f.SEDLAY = 0x98;
}

int ad_probe(void)
{
    int i;

    /* (a) ENEMY launches a saucer: game on, ship alive, one rock, delay
     * about to expire, on a frame with FRAME & 3 == 0. */
    fresh();
    g.zp.f.NPLAYR = 1; OBJ(AD_SHIP) = 1; OBJ(0) = 4; AD_P->f.NROCKS = 1;
    AD_P->f.EDELAY = 1; AD_P->f.RTIMER = 0; g.zp.f.FRAME[0] = 4;
    ad_enemy();
    printf("(a) saucer: SAUPIX=%02X pos=%02X%02X,%02X%02X vel=%02X,%02X SEDLAY=%02X EDELAY=%02X\n",
           AD_P->f.SAUPIX, AD_P->f.SAUXH, AD_P->f.SAUXL, AD_P->f.SAUYH, AD_P->f.SAUYL,
           AD_P->f.SAUXI, AD_P->f.SAUYI, AD_P->f.SEDLAY, AD_P->f.EDELAY);
    /* RANDOM = 0: height 0>>3 + 4 = 4 -> SAUYH = 4; left edge, +$10;
     * DIFCTY - NROCKS = 5 >= 4 -> small saucer (SAUPIX 1). */
    CHECK(AD_P->f.SAUPIX == 1 && AD_P->f.SAUYH == 4 && AD_P->f.SAUXH == 0 && AD_P->f.SAUXL == 0
          && AD_P->f.SAUXI == 0x10, "saucer launch");
    CHECK(AD_P->f.SEDLAY == 0x92 && AD_P->f.EDELAY == 1, "delays");
    /* Not on other frames. */
    fresh(); g.zp.f.NPLAYR = 1; OBJ(AD_SHIP) = 1; OBJ(0) = 4; AD_P->f.NROCKS = 1; AD_P->f.EDELAY = 1;
    g.zp.f.FRAME[0] = 5; ad_enemy();
    CHECK(AD_P->f.SAUPIX == 0 && AD_P->f.EDELAY == 1, "not every frame");
    /* Too many rocks while RTIMER runs. */
    fresh(); g.zp.f.NPLAYR = 1; OBJ(AD_SHIP) = 1; AD_P->f.NROCKS = 6; AD_P->f.EDELAY = 1; AD_P->f.RTIMER = 5;
    ad_enemy();
    CHECK(AD_P->f.SAUPIX == 0 && AD_P->f.RTIMER == 4 && AD_P->f.EDELAY == 1, "too many rocks");

    /* (b) A live saucer fires in attract mode at a close rock. */
    fresh();
    AD_P->f.SAUPIX = 1; AD_P->f.SAUXH = 0x10; AD_P->f.SAUYH = 0x0C; AD_P->f.EDELAY = 1;
    OBJ(3) = 4; OBJXH(3) = 0x12; OBJYH(3) = 0x0A;
    ad_enemy();
    printf("(b) efire: EDELAY=%02X R8=%02X TEMP3+1=%02X ANGLE+1=%02X\n",
           AD_P->f.EDELAY, g.zp.f.R8, g.zp.raw[0x0F], g.zp.raw[0x7A]);
    CHECK(AD_P->f.EDELAY == 0x0A && g.zp.f.R8 == 1 && g.zp.raw[0x0F] == 1, "fire setup");

    /* (c) RSAUCR parks it. */
    AD_P->f.SAUXI = 0x10; AD_P->f.SAUXH = 5;
    ad_rsaucr();
    CHECK(AD_P->f.SAUPIX == 0 && AD_P->f.SAUXI == 0 && AD_P->f.SAUXH == 0 && AD_P->f.SAUXL == 0
          && AD_P->f.EDELAY == 0x98, "rsaucr");

    /* (d) SETTIP: wave 3, few rocks, no specials yet, no launch this
     * wave -> three linked members. */
    fresh();
    AD_P->f.SROCKS = 2; AD_P->f.NROCKS = 2; OBJ(0) = 4; OBJ(1) = 4; g.zp.f.PLAYR = 0;
    ad_settip();
    printf("(d) settip: NROCKS=%u SPROCK=%u TRILE=%02X HSSND=%02X\n",
           AD_P->f.NROCKS, AD_P->f.SPROCK, g.zp.f.TRILE[0], g.zp.f.HSSND);
    for (i = 0x18; i >= 0x16; i--)
        printf("    slot %d: OBJ=%02X pos=%02X%02X,%02X%02X vel=%02X,%02X\n", i, OBJ(i),
               OBJXH(i), OBJXL(i), OBJYH(i), OBJYL(i), XINC(i), YINC(i));
    for (i = 0; i < 3; i++)
        printf("    control %d: SRTIME=%02X SRANG=%02X\n", i, AD_P->f.SRTIME[i], AD_P->f.SRANG[i]);
    CHECK(AD_P->f.NROCKS == 5 && AD_P->f.SPROCK == 3 && g.zp.f.TRILE[0] == 0x1A && g.zp.f.HSSND == 2,
          "cluster counts");
    CHECK(OBJ(0x18) == 0x4A && OBJ(0x17) == 0x46 && OBJ(0x16) == 0x42, "links 2,1,0: %02X %02X %02X",
          OBJ(0x18), OBJ(0x17), OBJ(0x16));
    CHECK(AD_P->f.SRTIME[2] == 0x81 && AD_P->f.SRTIME[1] == 0x85 && AD_P->f.SRTIME[0] == 0x83, "ITIME");
    CHECK(AD_P->f.SRANG[2] == 0x40 && AD_P->f.SRANG[1] == 0x98 && AD_P->f.SRANG[0] == 0xF0, "IANG");
    /* Member 2 (slot 24) at the RNDPOS origin (0,0) plus ITXL/ITYL[2] = $70, $00
     * with ITXH/ITYH[2] = 0, 0.  Member 1 copies member 2's position then
     * adds $58/-1, $50/0. */
    CHECK(OBJXH(0x18) == 0x00 && OBJXL(0x18) == 0x70 && OBJYH(0x18) == 0 && OBJYL(0x18) == 0, "member 2 pos");
    CHECK(OBJXH(0x17) == 0xFF && OBJXL(0x17) == 0xC8 && OBJYH(0x17) == 0 && OBJYL(0x17) == 0x50, "member 1 pos");
    CHECK(XINC(0x18) == 6 && YINC(0x18) == 6, "RNDXYI +6,+6");
    /* Gate: refused while SPROCK != 0. */
    ad_settip();
    CHECK(AD_P->f.SPROCK == 3, "no relaunch");

    /* (e) ATTACK on member 0 (slot 22, control 0) with the ship at the
     * centre: it turns toward the ship and gets a velocity. */
    g.zp.f.NPLAYR = 1; OBJ(AD_SHIP) = 1; OBJXH(AD_SHIP) = 0x10; OBJXL(AD_SHIP) = 0x60;
    OBJYH(AD_SHIP) = 0x0C; OBJYL(AD_SHIP) = 0x60;
    AD_P->f.SRTIME[0] = 0x7E; g.zp.f.R9 = 0; g.zp.f.FRAME[0] = 1;
    ad_attack(0x16);
    printf("(e) attack: SRANG0=%02X vel=%02X,%02X R9=%u\n", AD_P->f.SRANG[0], XINC(0x16), YINC(0x16), g.zp.f.R9);
    CHECK(g.zp.f.R9 == 1, "attacker counted");
    CHECK(XINC(0x16) != 0 || YINC(0x16) != 0, "moving");

    /* (f) SPLTTP on a waiting member: 500 points path, chain released. */
    fresh();
    g.zp.f.NPLAYR = 1; g.zp.f.TEMP3[0] = 0;
    OBJ(5) = 0x4A; AD_P->f.SRTIME[2] = 0x81; AD_P->f.SRTIME[0] = 0x83; AD_P->f.SRTIME[1] = 0x85;
    AD_P->f.NROCKS = 3;
    ad_splttp(5);
    printf("(f) splttp waiting: OBJ5=%02X slot24=%02X NROCKS=%u SRTIME=%02X %02X %02X\n",
           OBJ(5), OBJ(0x18), AD_P->f.NROCKS, AD_P->f.SRTIME[0], AD_P->f.SRTIME[1], AD_P->f.SRTIME[2]);
    CHECK(OBJ(5) == 0 && OBJ(0x18) == 0x4A && AD_P->f.NROCKS == 4, "clone and explode");
    CHECK(AD_P->f.SRTIME[2] == 0x7F && AD_P->f.SRTIME[0] == 0x7F && AD_P->f.SRTIME[1] == 0x7F, "chain released");
    /* A big flying diamond splits into two small specials. */
    fresh(); g.zp.f.NPLAYR = 1; g.zp.f.TEMP3[0] = 0; g.zp.f.ANGLE[0] = 0x40;
    OBJ(5) = 0x42; AD_P->f.SRTIME[0] = 0x7F; AD_P->f.NROCKS = 1; AD_P->f.SPROCK = 1;
    ad_splttp(5);
    /* FREESR scans 6 down to 0, so the second tip takes control entry 6. */
    printf("(f) splttp big: slot24=%02X slot23=%02X NROCKS=%u SPROCK=%u SRTIME0=%02X SRANG0=%02X SRTIME6=%02X SRANG6=%02X\n",
           OBJ(0x18), OBJ(0x17), AD_P->f.NROCKS, AD_P->f.SPROCK,
           AD_P->f.SRTIME[0], AD_P->f.SRANG[0], AD_P->f.SRTIME[6], AD_P->f.SRANG[6]);
    CHECK(OBJ(0x18) == 0x42 && OBJ(0x17) == 0x5A && AD_P->f.NROCKS == 3 && AD_P->f.SPROCK == 2, "two smalls");
    CHECK(AD_P->f.SRTIME[0] == 0x7E && AD_P->f.SRANG[0] == 0x60, "first tip ANGLE+$20");
    CHECK(AD_P->f.SRTIME[6] == 0x7E && AD_P->f.SRANG[6] == 0x1F, "second tip ANGLE-$21");

    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 2 : 0;
}
