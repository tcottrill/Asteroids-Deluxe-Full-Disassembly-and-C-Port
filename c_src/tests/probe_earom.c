/* Probe for earom.c - drives EAUPD/STEAROM in isolation and checks the
 * hardware sequence against the listing.
 *
 *     build_mod.bat earom
 *     test_earom.exe --probe
 *
 * The host stub returns 0 from every EAROM read, so a read cycle sees
 * 21 zero bytes.  Six zero data bytes sum to a zero checksum and the
 * checksum byte read is also zero, so the ROM's compare *matches*.  To
 * exercise the mismatch path the probe corrupts EACS just before entry
 * 1's checksum step - the same condition a bad byte produces.
 */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"

/* ---- trace, fed by earom.c under AD_PROBE ------------------------- */

typedef struct { char kind; uint8_t x, a; } ea_ev;

static ea_ev  evs[64];          /* events of the current step */
static int    nev;
static uint8_t ctl_seq[4096];   /* every EACTL value, in order */
static int    nctl;
static int    failures;

extern void (*ad_earom_trace)(char kind, uint8_t x, uint8_t a);

static void trace(char kind, uint8_t x, uint8_t a)
{
    if (nev < (int)(sizeof evs / sizeof evs[0]))
        evs[nev++] = (ea_ev){ kind, x, a };
    if (kind == 'C' && nctl < (int)sizeof ctl_seq)
        ctl_seq[nctl++] = a;
}

static void fail(const char *what)
{
    printf("  FAIL: %s\n", what);
    failures++;
}

static void check(int ok, const char *what)
{
    if (!ok)
        fail(what);
}

/* One frame's worth of NMIs, then EAUPD as the main line calls it.
 * Prints a line only when the throttle let a step through. */
static int step_no;
static void frame(void)
{
    nev = 0;
    g.zp.f.INTCT += 4;
    ad_eaupd();
    if (nev == 0)
        return;
    printf("  %3d: EAX=%02X EABC=%02X EAFLG=%02X EAHSX=%02X EACS=%02X EABAD=%02X |",
           step_no++, g.zp.f.EAX, g.zp.f.EABC, g.zp.f.EAFLG,
           g.zp.f.EAHSX, g.zp.f.EACS, g.zp.f.EABAD);
    for (int i = 0; i < nev; i++) {
        switch (evs[i].kind) {
        case 'C': printf(" CTL=%02X", evs[i].a); break;
        case 'D': printf(" DAL[%02X]=%02X", evs[i].x, evs[i].a); break;
        case 'R': printf(" IN->%02X", evs[i].a); break;
        default:  printf(" ?"); break;
        }
    }
    printf("\n");
}

/* Run frames until EAFLG drops to zero, then one more so the idle
 * deselect is seen.  `limit` guards against a stuck machine. */
static void run_until_idle(int limit)
{
    while (g.zp.f.EAFLG != 0 && limit-- > 0)
        frame();
    if (g.zp.f.EAFLG != 0)
        fail("state machine never went idle");
    /* Advance to the next throttle window and take the idle step. */
    do
        frame();
    while ((g.zp.f.INTCT & 0x0C) != 0);
}

/* Compare the recorded EACTL sequence against an expected one. */
static void expect_ctl(const uint8_t *want, int n, const char *what)
{
    int ok = (nctl == n) && memcmp(ctl_seq, want, (size_t)n) == 0;
    printf("  EACTL sequence for %s: %d values, %s\n", what, nctl, ok ? "matches the listing" : "MISMATCH");
    if (!ok) {
        failures++;
        printf("    got :");
        for (int i = 0; i < nctl; i++) printf(" %02X", ctl_seq[i]);
        printf("\n    want:");
        for (int i = 0; i < n; i++) printf(" %02X", want[i]);
        printf("\n");
    }
    nctl = 0;
}

/* Expected EACTL values per step, straight from the listing. */
static const uint8_t READ_STEP[5]  = { 0x08, 0x09, 0x08, 0x00, 0x00 }; /* EAUPD_10 x3, deselect, EAUPD_2 */
static const uint8_t ERASE_STEP[2] = { 0x06, 0x0E };                    /* EAC1+EAC2, +EACE */
static const uint8_t WRITE_STEP[2] = { 0x04, 0x0C };                    /* EAC1, EAC1+EACE */

static int build_expect(uint8_t *buf, const uint8_t *step, int n, int reps)
{
    int k = 0;
    for (int r = 0; r < reps; r++)
        for (int i = 0; i < n; i++)
            buf[k++] = step[i];
    return k;
}

int ad_probe(void)
{
    uint8_t want[4096];
    int n;

    ad_earom_trace = trace;

    /* ---------------------------------------------------------- */
    printf("1. Throttle: EAUPD must act only when INTCT & $0C == 0\n");
    {
        int acted = 0;
        g.zp.f.EAFLG = 0;
        for (int i = 0; i < 16; i++) {
            nev = 0;
            g.zp.f.INTCT = (uint8_t)i;
            ad_eaupd();
            if (nev) acted++;
        }
        printf("  acted on %d of 16 INTCT values\n", acted);
        check(acted == 4, "expected 4 of 16 (INTCT 0..3)");
        check(nctl == 4 && ctl_seq[0] == 0, "idle step writes EACTL=0");
        nctl = 0;
    }

    /* ---------------------------------------------------------- */
    printf("2. Full read cycle, as START ($6003) kicks it\n");
    ad_pwron();
    g.zp.f.EABC = 0;
    g.zp.f.EAX = 0;
    g.zp.f.EAHSX = 0;
    g.zp.f.EAFLG = 0x20;
    step_no = 0;
    {
        int entry1_checked = 0;
        for (int guard = 0; g.zp.f.EAFLG != 0 && guard < 400; guard++) {
            /* Just before entry 1's checksum byte is read (EAHSX=3,
             * EABC=6, the terminator index), make the running checksum
             * disagree with the 0 the host will return, and plant
             * something in the entry's RAM so the blanking is visible.
             * We only do this once, when the *next* step is that one. */
            if (!entry1_checked && g.zp.f.EAHSX == 3 && g.zp.f.EABC == 6
                && (g.zp.f.EAFLG == 0x20) && ((g.zp.f.INTCT + 4) & 0x0C) == 0) {
                g.zp.f.EACS = 0x5A;
                for (int i = 0; i < 3; i++) {
                    g.zp.f.HSCORE[3 + i] = 0xAA;
                    g.zp.f.INITL[3 + i] = 0xBB;
                }
                printf("  (corrupting EACS to $5A and planting $AA/$BB in entry 1 before its checksum step)\n");
                frame();
                printf("  entry 1 after checksum step: HSCORE[3..5]=%02X %02X %02X INITL[3..5]=%02X %02X %02X EABAD=%02X EAHSX=%02X\n",
                       g.zp.f.HSCORE[3], g.zp.f.HSCORE[4], g.zp.f.HSCORE[5],
                       g.zp.f.INITL[3], g.zp.f.INITL[4], g.zp.f.INITL[5],
                       g.zp.f.EABAD, g.zp.f.EAHSX);
                check(g.zp.f.HSCORE[3] == 0 && g.zp.f.HSCORE[4] == 0 && g.zp.f.HSCORE[5] == 0,
                      "mismatch blanks HSCORE[EAHSX..+2]");
                check(g.zp.f.INITL[3] == 0 && g.zp.f.INITL[4] == 0 && g.zp.f.INITL[5] == 0,
                      "mismatch blanks INITL[EAHSX..+2]");
                check(g.zp.f.EABAD == 1, "mismatch sets EABAD");
                check(g.zp.f.EAHSX == 3, "mismatch leaves EAHSX where it was (ROM quirk)");
                check(g.zp.f.EABC == 0, "checksum step resets EABC");
                entry1_checked = 1;
                continue;
            }
            frame();
        }
        check(entry1_checked, "reached entry 1's checksum step");
    }
    printf("  read done: EAFLG=%02X EAX=%02X EAHSX=%02X EABAD=%02X\n",
           g.zp.f.EAFLG, g.zp.f.EAX, g.zp.f.EAHSX, g.zp.f.EABAD);
    check(g.zp.f.EAFLG == 0, "read cycle ends with EAFLG=0");
    check(g.zp.f.EAX == 0x14, "read cycle ends with EAX=$14 (last address, not advanced)");
    /* EAHSX advances only on a match: 0->3 (entry 0), stays 3 (entry 1
     * failed), then entry 2's bytes land in slot 3 and match, 3->6. */
    check(g.zp.f.EAHSX == 6, "EAHSX ends at 6: advanced for the two matching entries only");
    check(step_no == 21, "21 read steps");
    /* the idle step that follows */
    do frame(); while ((g.zp.f.INTCT & 0x0C) != 0);
    n = build_expect(want, READ_STEP, 5, 21);
    want[n++] = 0x00;                       /* the idle deselect */
    expect_ctl(want, n, "read (21 bytes + idle)");

    /* ---------------------------------------------------------- */
    printf("3. STEAROM with EABAD set: whole-ROM erase then write\n");
    /* Give the table something recognisable so the written data can
     * be checked, including the checksums. */
    for (int i = 0; i < 9; i++) {
        g.zp.f.HSCORE[i] = (uint8_t)(0x10 + i);
        g.zp.f.INITL[i] = (uint8_t)(0x01 + i);
    }
    /* UPDATE-style priming for entry 0; STEAROM overrides it because
     * EABAD is set. */
    g.zp.f.EAHSX = 0;
    g.zp.f.EABC = 21;
    g.zp.f.EAX = 0x14;
    ad_stearom();
    printf("  after STEAROM: EAFLG=%02X EABC=%02X EAX=%02X EAHSX=%02X EABAD=%02X\n",
           g.zp.f.EAFLG, g.zp.f.EABC, g.zp.f.EAX, g.zp.f.EAHSX, g.zp.f.EABAD);
    check(g.zp.f.EAFLG == 0x80, "STEAROM sets EAFLG=$80");
    check(g.zp.f.EABC == 0x15 && g.zp.f.EAX == 0x14, "EABAD widens to EABC=$15, EAX=$14");
    check(g.zp.f.EAHSX == 0 && g.zp.f.EABAD == 0, "EABAD path clears EAHSX and EABAD");
    /* STEAROM must refuse while busy. */
    g.zp.f.EABAD = 1;
    ad_stearom();
    check(g.zp.f.EABAD == 1 && g.zp.f.EABC == 0x15, "STEAROM is a no-op while EAFLG != 0");
    g.zp.f.EABAD = 0;

    step_no = 0;
    nev = 0;
    {
        /* erase phase */
        int erase_steps = 0;
        while (g.zp.f.EAFLG == 0x80) { frame(); if (nev) erase_steps++; }
        printf("  erase took %d steps; now EAFLG=%02X EAX=%02X EABC=%02X\n",
               erase_steps, g.zp.f.EAFLG, g.zp.f.EAX, g.zp.f.EABC);
        check(erase_steps == 21, "21 erase steps");
        check(g.zp.f.EAFLG == 0x40, "erase hands over to write");
        check(g.zp.f.EAX == 0 && g.zp.f.EABC == 0, "write starts at EAX=0, EABC=0");
        n = build_expect(want, ERASE_STEP, 2, 21);
        expect_ctl(want, n, "erase (21 addresses, EAX $14 down to 0)");
    }
    {
        /* write phase */
        uint8_t data[21];
        int nd = 0;
        int write_steps = 0;
        nctl = 0;
        while (g.zp.f.EAFLG == 0x40) {
            frame();
            if (nev) {
                write_steps++;
                for (int i = 0; i < nev; i++)
                    if (evs[i].kind == 'D' && nd < 21) {
                        check(evs[i].x == (uint8_t)nd, "write address is sequential from 0");
                        data[nd++] = evs[i].a;
                    }
            }
        }
        printf("  write took %d steps; EAFLG=%02X EAX=%02X EAHSX=%02X\n",
               write_steps, g.zp.f.EAFLG, g.zp.f.EAX, g.zp.f.EAHSX);
        check(write_steps == 21, "21 write steps");
        check(g.zp.f.EAFLG == 0, "write ends idle");
        check(g.zp.f.EAHSX == 9, "EAHSX advanced 3 per entry, to 9");
        printf("  data written:");
        for (int i = 0; i < nd; i++) printf(" %02X", data[i]);
        printf("\n");
        /* Expected: for entry k, HSCORE[3k..3k+2], INITL[3k..3k+2], sum. */
        {
            uint8_t exp[21];
            int k = 0;
            for (int e = 0; e < 3; e++) {
                uint8_t sum = 0;
                for (int i = 0; i < 3; i++) { exp[k++] = g.zp.f.HSCORE[3 * e + i]; sum = (uint8_t)(sum + g.zp.f.HSCORE[3 * e + i]); }
                for (int i = 0; i < 3; i++) { exp[k++] = g.zp.f.INITL[3 * e + i];  sum = (uint8_t)(sum + g.zp.f.INITL[3 * e + i]); }
                exp[k++] = sum;
            }
            check(nd == 21 && memcmp(data, exp, 21) == 0, "written bytes are score,initials,checksum per entry");
        }
        do frame(); while ((g.zp.f.INTCT & 0x0C) != 0);
        n = build_expect(want, WRITE_STEP, 2, 21);
        want[n++] = 0x00;
        expect_ctl(want, n, "write (21 bytes + idle)");
    }

    /* ---------------------------------------------------------- */
    printf("4. STEAROM after UPDATE primed a partial rewrite (entry 1 changed)\n");
    /* UPDATE ($7089): EAHSX = 3, EABC = (3 - 1) * 7 = 14, EAX = $14. */
    g.zp.f.EAHSX = 3;
    g.zp.f.EABC = 14;
    g.zp.f.EAX = 0x14;
    g.zp.f.EABAD = 0;
    ad_stearom();
    check(g.zp.f.EAFLG == 0x80 && g.zp.f.EABC == 14 && g.zp.f.EAX == 0x14 && g.zp.f.EAHSX == 3,
          "STEAROM without EABAD only sets the mode");
    step_no = 0;
    nctl = 0;
    {
        int erase_steps = 0, write_steps = 0;
        uint8_t first_write_addr = 0xFF;
        while (g.zp.f.EAFLG == 0x80) { frame(); if (nev) erase_steps++; }
        printf("  erase took %d steps; write starts at EAX=%02X\n", erase_steps, g.zp.f.EAX);
        check(erase_steps == 14, "14 erase steps (addresses $14 down to 7)");
        check(g.zp.f.EAX == 7, "write resumes at address 7");
        while (g.zp.f.EAFLG == 0x40) {
            frame();
            if (nev) {
                write_steps++;
                for (int i = 0; i < nev; i++)
                    if (evs[i].kind == 'D' && first_write_addr == 0xFF)
                        first_write_addr = evs[i].x;
            }
        }
        printf("  write took %d steps from address %02X; EAHSX=%02X\n",
               write_steps, first_write_addr, g.zp.f.EAHSX);
        check(write_steps == 14 && first_write_addr == 7, "14 write steps, 7..20");
        check(g.zp.f.EAHSX == 9, "EAHSX 3 -> 9");
        do frame(); while ((g.zp.f.INTCT & 0x0C) != 0);
        n = build_expect(want, ERASE_STEP, 2, 14);
        n += build_expect(want + n, WRITE_STEP, 2, 14);
        want[n++] = 0x00;
        expect_ctl(want, n, "partial erase+write (14 each) + idle");
    }

    /* ---------------------------------------------------------- */
    printf("5. STEAROM refuses an entry index that does not fit (EAHSX > 8)\n");
    g.zp.f.EAFLG = 0;
    g.zp.f.EAHSX = 9;
    ad_stearom();
    check(g.zp.f.EAFLG == 0, "EAHSX=9 rejected");
    g.zp.f.EAHSX = 8;
    ad_stearom();
    check(g.zp.f.EAFLG == 0x80, "EAHSX=8 accepted (8 >= EAHSX)");
    g.zp.f.EAFLG = 0;

    printf("%s: %d failure(s)\n", failures ? "PROBE FAILED" : "PROBE OK", failures);
    return failures ? 1 : 0;
}
