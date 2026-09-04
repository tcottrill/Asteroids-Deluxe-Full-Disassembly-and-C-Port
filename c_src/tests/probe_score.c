/* probe_score.c - POINTS, CHKST, UPDATE, PARAMS and SCORES.
 * Build with everything: the HUD needs vgutil/message/draw, and the
 * high-score screen is dumped for vramview.py. */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"

static int fails;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

static void fresh(void)
{
    memset(&g, 0, sizeof g);
    g.zp.f.RVELP = 0x0A; g.zp.f.RVELM = 0xF6;
    AD_P->f.DIFCTY = 6;
    g.zp.f.UPDFLG[0] = g.zp.f.UPDFLG[1] = 0xFF;
    g.zp.f.PLATEU[1] = 0x00; g.zp.f.PLATEU[2] = 0x01;     /* bonus at 10 000 */
    g.zp.f.BONSCR[1] = 0x00; g.zp.f.BONSCR[2] = 0x01;
}

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(g.vram, 1, sizeof g.vram, f); fclose(f); printf("wrote %s\n", path); }
}

static void show_score(const char *what)
{
    printf("    %s: SCORE=%02X%02X%02X PLATEU=%02X%02X HITS=%u SND3=%u\n", what,
           g.zp.f.SCORE[2], g.zp.f.SCORE[1], g.zp.f.SCORE[0],
           g.zp.f.PLATEU[2], g.zp.f.PLATEU[1], g.zp.f.HITS[0], g.zp.f.SND3);
}

int ad_probe(void)
{
    /* (a) POINTS: the packed A:C form.  $20 = 200, C alone = 1000,
     * $10 = 100 (HITSCR's small-rock value). */
    fresh(); g.zp.f.HITS[0] = 3;
    ad_points(0x20, false); show_score("+200");
    CHECK(g.zp.f.SCORE[1] == 0x02 && g.zp.f.SCORE[0] == 0 && g.zp.f.SCORE[2] == 0, "200");
    ad_points(0x00, true);  show_score("+1000");
    CHECK(g.zp.f.SCORE[1] == 0x12, "1200");
    ad_points(0x10, false); show_score("+100");
    CHECK(g.zp.f.SCORE[1] == 0x13, "1300");
    /* BCD carry into the next byte, and the plateau: 9900 + 100. */
    g.zp.f.SCORE[1] = 0x99; g.zp.f.SCORE[0] = 0;
    ad_points(0x10, false); show_score("9900+100");
    CHECK(g.zp.f.SCORE[1] == 0x00 && g.zp.f.SCORE[2] == 0x01, "10000");
    CHECK(g.zp.f.HITS[0] == 4 && g.zp.f.SND3 == 1, "extra life at the plateau");
    CHECK(g.zp.f.PLATEU[2] == 0x02 && g.zp.f.PLATEU[1] == 0x00, "plateau moved to 20000");

    /* (b) CHKST in attract mode, 1 coin 1 credit. */
    fresh(); g.zp.f.CMODE = 1; g.zp.f.CRDT = 0; g.zp.f.LOUT1 = 3;
    ad_set_vglist(0x4002);
    CHECK(!ad_chkst() && g.zp.f.LOUT1 == 0 && g.zp.f.TWOCM == 0xFF, "no credits: no lamps");
    printf("(b) chkst 0 credits emitted %u bytes\n", ad_vglist() - 0x4002);
    g.zp.f.CRDT = 2; g.zp.f.FRAME[0] = 0x10; g.zp.f.LOUT1 = 0;
    CHECK(!ad_chkst() && g.zp.f.LOUT1 == 3, "2 credits, no start: both lamps toggled on");
    g.zp.f.CRDT = 1; g.zp.f.LOUT1 = 0;
    CHECK(!ad_chkst() && g.zp.f.LOUT1 == 1, "1 credit: lamp 1");
    fresh(); g.zp.f.CMODE = 0;
    CHECK(!ad_chkst() && g.zp.f.CRDT == 2, "free play gives 2 credits");
    /* A game in progress in ready mode counts GDELAY down. */
    fresh(); g.zp.f.NPLAYR = 1; g.zp.f.GDELAY = 2;
    CHECK(!ad_chkst() && g.zp.f.GDELAY == 1, "ready mode");
    /* Game over: no lives, ship gone, SDELAY $80 -> NPLAYR = $FF. */
    fresh(); g.zp.f.NPLAYR = 1; g.zp.f.HITS[0] = 0; AD_P->f.SDELAY = 0x80;
    CHECK(!ad_chkst() && g.zp.f.NPLAYR == 0xFF && g.zp.f.LPLAYR == 1 && g.zp.f.LOUT1 == 3, "game over");

    /* (c) UPDATE: 500 into an empty table, then 300 goes second. */
    fresh(); g.zp.f.NPLAYR = 0xFF; g.zp.f.SCORE[1] = 0x05;
    ad_update();
    printf("(c) update: HSCORE0=%02X%02X%02X INITL=%02X %02X %02X UPDFLG=%02X %02X EAHSX=%02X EABC=%02X EAX=%02X NPLAYR=%02X\n",
           g.zp.f.HSCORE[2], g.zp.f.HSCORE[1], g.zp.f.HSCORE[0], g.zp.f.INITL[0], g.zp.f.INITL[1], g.zp.f.INITL[2],
           g.zp.f.UPDFLG[0], g.zp.f.UPDFLG[1], g.zp.f.EAHSX, g.zp.f.EABC, g.zp.f.EAX, g.zp.f.NPLAYR);
    CHECK(g.zp.f.HSCORE[1] == 0x05 && g.zp.f.INITL[0] == 0x0B && g.zp.f.UPDFLG[0] == 0 && g.zp.f.UPDFLG[1] == 0xFF,
          "500 at rank 1");
    CHECK(g.zp.f.EAHSX == 0 && g.zp.f.EABC == 21 && g.zp.f.EAX == 0x14 && g.zp.f.NPLAYR == 0, "EAROM setup");
    g.zp.f.NPLAYR = 0xFF; g.zp.f.SCORE[1] = 0x03; g.zp.f.INITL[0] = 0x0C; g.zp.f.INITL[1] = 0x0D; g.zp.f.INITL[2] = 0x0E;
    ad_update();
    CHECK(g.zp.f.HSCORE[1] == 0x05 && g.zp.f.HSCORE[4] == 0x03 && g.zp.f.INITL[3] == 0x0B && g.zp.f.UPDFLG[0] == 3,
          "300 at rank 2: %02X %02X %02X %02X", g.zp.f.HSCORE[1], g.zp.f.HSCORE[4], g.zp.f.INITL[3], g.zp.f.UPDFLG[0]);
    CHECK(g.zp.f.INITL[0] == 0x0C && g.zp.f.INITL[1] == 0x0D, "rank 1 initials kept");
    CHECK(g.zp.f.EABC == 14, "EABC for entry 1: %u", g.zp.f.EABC);
    /* And 400 goes between them: 500, 400, 300. */
    g.zp.f.NPLAYR = 0xFF; g.zp.f.SCORE[1] = 0x04; g.zp.f.UPDFLG[0] = 0xFF;
    ad_update();
    CHECK(g.zp.f.HSCORE[1] == 0x05 && g.zp.f.HSCORE[4] == 0x04 && g.zp.f.HSCORE[7] == 0x03 && g.zp.f.INITL[6] == 0x0B,
          "400 inserted at rank 2, 300 shifted to 3");

    /* (d) GETINT with a pending entry runs and reports in progress. */
    fresh(); g.zp.f.UPDFLG[0] = 0; g.zp.f.LPLAYR = 1; g.zp.f.INITL[0] = 0x0B;
    ad_set_vglist(0x4002);
    CHECK(ad_getint(), "getint in progress");
    printf("(d) getint emitted %u bytes\n", ad_vglist() - 0x4002);
    fresh();
    CHECK(!ad_getint(), "getint idle");

    /* (e) PARAMS during a game: score 1300, 3 lives, high score 500 "A  ". */
    fresh(); g.zp.f.NPLAYR = 1; g.zp.f.HITS[0] = 3; g.zp.f.SCORE[1] = 0x13;
    g.zp.f.HSCORE[1] = 0x05; g.zp.f.INITL[0] = 0x0B; g.zp.f.INITL[1] = 0x0C; g.zp.f.INITL[2] = 0x0D;
    AD_P->raw[AD_OBJ + AD_SHIP] = 1; g.zp.f.SBTL = 0xC0;
    ad_set_vglist(0x4002);
    ad_params();
    printf("(e) params emitted %u bytes\n", ad_vglist() - 0x4002);
    CHECK(ad_vglist() > 0x4040, "params drew something");

    /* (f) SCORES in attract mode with three entries: dump the screen. */
    fresh(); g.zp.f.NPLAYR = 0; g.zp.f.FRAME[1] = 0;
    g.zp.f.HSCORE[2] = 0x01; g.zp.f.HSCORE[1] = 0x23; g.zp.f.HSCORE[0] = 0x40;   /* 12340 */
    g.zp.f.HSCORE[4] = 0x05;                                                     /* 500 */
    g.zp.f.HSCORE[7] = 0x03;                                                     /* 300 */
    g.zp.f.INITL[0] = 0x0B; g.zp.f.INITL[1] = 0x0C; g.zp.f.INITL[2] = 0x0D;     /* ABC */
    g.zp.f.INITL[3] = 0x22; g.zp.f.INITL[4] = 0x23; g.zp.f.INITL[5] = 0x24;     /* XYZ */
    g.zp.f.INITL[6] = 0x0B; g.zp.f.INITL[7] = 0x00; g.zp.f.INITL[8] = 0x00;     /* A   */
    g.vram[0] = 0x01; g.vram[1] = 0xE0;
    ad_set_vglist(0x4002);
    CHECK(ad_scores(), "scores shown");
    ad_params();                            /* the attract HUD under it */
    ad_vgsabs(0x7F, 0x7F);
    ad_vghalt();
    printf("(f) high-score screen ends at $%04X\n", ad_vglist());
    dump("probe_score.bin");
    /* The other attract screen: copyright and GAME OVER. */
    g.zp.f.FRAME[1] = 4;
    ad_set_vglist(0x4002);
    for (int i = 0; i < 4; i++) { g.zp.f.FRAME[0] = (uint8_t)i; ad_rotast(); }
    ad_setrol();                            /* the copyright subroutine at $4702 */
    ad_set_vglist(0x4002);
    CHECK(!ad_scores(), "game over screen");
    ad_vghalt();
    dump("probe_score2.bin");

    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 2 : 0;
}
