/* probe_stest.c - STEST3's checksum/bank-select test, and one pass of
 * the self-test display loop (STEST5/STEST6/STEST7 + SWCH), checked
 * against bytes and addresses derived from the listing
 * ($7DF3-$7FBE). */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

extern bool ad_host_random_off;
extern void ad_host_set_allpot(uint8_t v);
extern bool ad_host_test_sw;
extern bool ad_host_fire_sw, ad_host_thrust_sw, ad_host_rotl_sw, ad_host_rotr_sw;

/* True if the `n`-byte needle appears anywhere in the vector RAM this
 * pass actually wrote (offset 0 up to the current VGLIST). */
static bool vram_has(const uint8_t *needle, size_t n)
{
    uint16_t end = (uint16_t)(ad_vglist() - 0x4000);
    if (end > AD_VRAM_SIZE)
        end = AD_VRAM_SIZE;
    for (uint16_t i = 0; (size_t)(end - i) >= n; i++) {
        if (memcmp(&g.vram[i], needle, n) == 0)
            return true;
    }
    return false;
}

int ad_probe(void)
{
    ad_host_random_off = true;              /* RANDOM = 0 throughout */

    /* (1) STEST3 ($7DF3): the CKSUM bytes place six checksums that all
     * come out 0 on the real ROM image, and the port's bank-select
     * test (AD_P/AD_P3 physically swap) should find no error. */
    ad_pwron();
    ad_stest3();
    printf("R0-R5=%02X %02X %02X %02X %02X %02X CKERR=%02X XT=%02X\n",
           g.zp.raw[0x10], g.zp.raw[0x11], g.zp.raw[0x12],
           g.zp.raw[0x13], g.zp.raw[0x14], g.zp.raw[0x15],
           g.zp.f.CKERR, g.zp.f.XT);
    CHECK(g.zp.raw[0x10] == 0 && g.zp.raw[0x11] == 0 && g.zp.raw[0x12] == 0
          && g.zp.raw[0x13] == 0 && g.zp.raw[0x14] == 0 && g.zp.raw[0x15] == 0,
          "R0..R5 should all be 0 on the real ROM image");
    CHECK(g.zp.f.CKERR == 0, "CKERR=%02X, should be the OR of six zero sums", g.zp.f.CKERR);
    CHECK(g.zp.f.XT == 0, "XT=%02X, the bank-select test found an error", g.zp.f.XT);

    /* (2) One display-loop pass with the switch held and nothing else
     * pressed.  TEST1's JSRL word (A2 CB) must be in the list; BNKERR's
     * (E6 CB) must not, since XT=0; the pass ends with VGSABS($7F,$7F)
     * (FC A1 FC 11 - VGSIZE=$10 at that point) then VGHALT (B0 B0). */
    ad_host_test_sw = true;
    {
        static const uint8_t test1[2]  = { 0xA2, 0xCB };
        static const uint8_t bnkerr[2] = { 0xE6, 0xCB };
        static const uint8_t tail[6]   = { 0xFC, 0xA1, 0xFC, 0x11, 0xB0, 0xB0 };
        uint16_t end;
        bool r = ad_stest_frame();

        end = (uint16_t)(ad_vglist() - 0x4000);
        printf("frame1: r=%d VGLIST=$%04X\n", r, ad_vglist());
        CHECK(r, "the first ad_stest_frame() should return true");
        CHECK(ad_vglist() > 0x4000, "VGLIST should have advanced past $4000");
        CHECK(vram_has(test1, 2), "TEST1's JSRL word (A2 CB) not found in the list");
        CHECK(!vram_has(bnkerr, 2), "BNKERR's JSRL word (E6 CB) found, but XT=0");
        CHECK(end >= 6 && memcmp(&g.vram[end - 6], tail, 6) == 0,
              "list should end VGSABS($7F,$7F)+VGHALT (FC A1 FC 11 B0 B0)");
    }

    /* (3) The switch-difference logic (SWCH_4).  FIRESW ($2004) is
     * read in the 5-switch pass, which folds into ASTERS[1] (the first
     * run's second SWCH_4 call); the 8-switch pass (ASTERS[0]/[2]) sees
     * nothing held.  A held-but-unchanging FIRE settles to no
     * difference on the next identical frame. */
    ad_host_fire_sw = true;
    ad_stest_frame();
    printf("ASTERS(fire, 1st)=%02X %02X %02X %02X\n",
           g.zp.f.ASTERS[0], g.zp.f.ASTERS[1], g.zp.f.ASTERS[2], g.zp.f.ASTERS[3]);
    CHECK(g.zp.f.ASTERS[0] == 0, "ASTERS[0] (8-switch pass) should be 0, nothing else held");
    CHECK(g.zp.f.ASTERS[1] != 0, "ASTERS[1] should show the FIRE difference");
    ad_stest_frame();
    printf("ASTERS(fire, 2nd)=%02X %02X %02X %02X\n",
           g.zp.f.ASTERS[0], g.zp.f.ASTERS[1], g.zp.f.ASTERS[2], g.zp.f.ASTERS[3]);
    CHECK(g.zp.f.ASTERS[0] == 0 && g.zp.f.ASTERS[1] == 0 && g.zp.f.ASTERS[2] == 0,
          "ASTERS[0..2] should settle to 0 once FIRE stops changing: got %02X %02X %02X",
          g.zp.f.ASTERS[0], g.zp.f.ASTERS[1], g.zp.f.ASTERS[2]);
    ad_host_fire_sw = false;

    /* (4) The erase trigger ($7F2A-$7F45): all four buttons together
     * sets EAFLG=LPLAYR=$80 (bit 7 = the AND result) and primes
     * EABC=$15, EAX=$14 - but STEST6_11 falls straight into EAUPD in
     * that SAME pass (7F47-7F5D), and INTCT is 0 (untouched by checks
     * 1-3, since LPLAYR was 0 there), so EAUPD's gate is open and it
     * takes its first erase step immediately: EABC and EAX each end
     * the pass one lower than they were just set to (see earom.c's
     * erase branch).  A following pass must not re-arm them back to
     * $15/$14 - LPLAYR is nonzero now, so the trigger's `bmi
     * STEST6_11` (7F3A) skips the set entirely - even though EAUPD's
     * own countdown (gated to once every 4 passes here) continues. */
    ad_host_fire_sw = ad_host_thrust_sw = ad_host_rotl_sw = ad_host_rotr_sw = true;
    ad_stest_frame();
    printf("erase, 1st: EAFLG=%02X LPLAYR=%02X EABC=%02X EAX=%02X\n",
           g.zp.f.EAFLG, g.zp.f.LPLAYR, g.zp.f.EABC, g.zp.f.EAX);
    CHECK(g.zp.f.EAFLG == 0x80, "EAFLG=%02X, should be $80", g.zp.f.EAFLG);
    CHECK(g.zp.f.LPLAYR == 0x80, "LPLAYR=%02X, should be $80", g.zp.f.LPLAYR);
    CHECK(g.zp.f.EABC == 0x14, "EABC=%02X, should be $14 ($15 set, then EAUPD's "
          "first erase step decrements it once in the same pass)", g.zp.f.EABC);
    CHECK(g.zp.f.EAX == 0x13, "EAX=%02X, should be $13 ($14 set, then decremented "
          "once by that same EAUPD step)", g.zp.f.EAX);
    {
        uint8_t eabc = g.zp.f.EABC, eax = g.zp.f.EAX;
        ad_stest_frame();
        printf("erase, 2nd: EAFLG=%02X LPLAYR=%02X EABC=%02X (was %02X) EAX=%02X (was %02X)\n",
               g.zp.f.EAFLG, g.zp.f.LPLAYR, g.zp.f.EABC, eabc, g.zp.f.EAX, eax);
        CHECK(g.zp.f.LPLAYR == 0x80 && g.zp.f.EAFLG != 0,
              "still erasing on the following pass");
        CHECK(!(g.zp.f.EABC == 0x15 && g.zp.f.EAX == 0x14),
              "the trigger re-armed EABC/EAX on a following pass (LPLAYR gate failed)");
    }
    ad_host_fire_sw = ad_host_thrust_sw = ad_host_rotl_sw = ad_host_rotr_sw = false;

    /* (5) Switch released: leave self-test. */
    ad_host_test_sw = false;
    CHECK(!ad_stest_frame(), "ad_stest_frame() should return false once STSTSW is off");

    /* (6) The POKEY test with a real coin byte on the pot pins.  STEST6
     * writes SKCTL=7, POTGO, reads OPTN5 at once (mid-scan: the coin
     * byte, inverted into CMODE), draws four digits, then SINIT's
     * PKYTST reads OPTN5 again expecting 0 ("S/B 0") - the fast scan
     * finished long ago on the board.  With $01 on the pins (main()'s
     * default) PKYTST's `ror` hides a wrong answer; $9D, the factory
     * setting, does not: any non-zero bit above bit 0 makes PERR
     * non-zero and puts PKYERR on the screen. */
    ad_host_set_allpot(0x9D);
    ad_host_random_off = false;             /* PKYTST's other half compares four
                                             * RANDOM samples: a RANDOM stuck at 0
                                             * is exactly what it calls a dead
                                             * POKEY (PERR bit 7) */
    ad_host_test_sw = true;
    ad_stest_frame();
    ad_host_random_off = true;
    CHECK(g.zp.f.CMODE == 0x62, "CMODE=%02X, should be ~$9D = $62 (OPTN5 read right after POTGO)",
          g.zp.f.CMODE);
    CHECK(g.zp.f.PERR[0] == 0, "PERR=%02X, should be 0: PKYTST's OPTN5 read must find the scan finished",
          g.zp.f.PERR[0]);
    CHECK(g.zp.f.RVELP == 0x0A && g.zp.f.RVELM == 0xF6,
          "rock speeds RVELP=%02X RVELM=%02X should be the normal $0A/$F6, not the easy $05/$FB",
          g.zp.f.RVELP, g.zp.f.RVELM);
    ad_host_test_sw = false;

    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 2 : 0;
}
