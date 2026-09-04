/* probe_er2055.c - er2055.c's own checks, independent of the game, plus
 * one round trip through earom.c's real state machine (ad_eaupd).
 *
 *     build_mod.bat er2055
 *     test_er2055.exe --probe
 *
 * Checks (1)-(5) drive a fresh, local ad_er2055 directly, the same way
 * probe_pokey.c drives a local ad_pokey - never the host's chip - with
 * expectations derived from MAME 0.286's er2055_device model
 * (src/devices/machine/er2055.cpp) and how asteroid.cpp's
 * earom_control_w wires it, not from running this translation. Check
 * (6) instead drives the *host's* chip (host_stub.c's static `earom`,
 * reached through ad_host_earom() below) via earom.c's ad_eaupd/
 * ad_stearom, because that is the only way to see the ER2055 model
 * through the ROM's own EAUPD state machine end to end.
 */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"
#include "er2055.h"

/* Exposed by host_stub.c under AD_PROBE for check (6) below. */
extern ad_er2055 *ad_host_earom(void);

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("  FAIL: "); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ */
/* (1) Fresh chip                                                       */
/* ------------------------------------------------------------------ */
static void check_fresh(void)
{
    ad_er2055 e;
    int i, allzero = 1;

    printf("1. Fresh chip: zero-filled (ROMREGION_ERASE00), data() is 0\n");
    ad_er2055_init(&e);
    for (i = 0; i < 64; i++)
        if (e.rom[i] != 0) allzero = 0;
    CHECK(allzero, "fresh chip: every byte should be 0");
    CHECK(ad_er2055_data(&e) == 0, "fresh chip: data() should be 0");
    CHECK(!e.dirty, "fresh chip: not dirty");
}

/* ------------------------------------------------------------------ */
/* (2) Erase sequence for address 5                                      */
/* ------------------------------------------------------------------ */
static void check_erase(void)
{
    ad_er2055 e;
    int i, others_ok;

    printf("2. Erase sequence for address 5 (EAC1+EAC2, then +EACE)\n");
    ad_er2055_init(&e);
    /* Give every byte a known non-$FF value, so "erase changed only
     * address 5" and "not before the chip is selected" are both real
     * assertions rather than accidentally-true zero comparisons. */
    for (i = 0; i < 64; i++)
        e.rom[i] = (uint8_t)(0x10 + i);
    e.dirty = false;

    ad_er2055_control(&e, 0x06);              /* EAC1|EAC2: deselected - mode set up only */
    CHECK(e.rom[5] == 0x15, "erase: address 5 must be unchanged before this call selects the chip");
    ad_er2055_set_addr_data(&e, 5, 0xAB);      /* STORE ADDRESS (data is don't-care for erase) */
    ad_er2055_control(&e, 0x0E);              /* EAC1|EAC2|EACE: selects the chip - erase fires HERE */
    CHECK(e.rom[5] == 0xFF, "erase: address 5 becomes $FF on the selecting call");

    others_ok = 1;
    for (i = 0; i < 64; i++)
        if (i != 5 && e.rom[i] != (uint8_t)(0x10 + i))
            others_ok = 0;
    CHECK(others_ok, "erase: no other address changed");
    CHECK(e.dirty, "erase sets dirty");
}

/* ------------------------------------------------------------------ */
/* (3) Write sequence for address 5, data $5A                            */
/* ------------------------------------------------------------------ */
static void check_write(void)
{
    ad_er2055 e;

    printf("3. Write sequence for address 5 = $5A (EAC1, then +EACE)\n");
    ad_er2055_init(&e);
    /* Erase address 5 first, through the real sequence, so the write
     * test starts from a genuinely erased byte. */
    ad_er2055_control(&e, 0x06);
    ad_er2055_set_addr_data(&e, 5, 0);
    ad_er2055_control(&e, 0x0E);
    CHECK(e.rom[5] == 0xFF, "setup: address 5 erased before the write test");
    e.dirty = false;

    ad_er2055_set_addr_data(&e, 5, 0x5A);
    ad_er2055_control(&e, 0x04);               /* EAC1: not yet selected */
    CHECK(e.rom[5] == 0xFF, "write: unchanged before this call selects the chip");
    ad_er2055_control(&e, 0x0C);               /* EAC1|EACE: selects - write (AND) fires HERE */
    CHECK(e.rom[5] == 0x5A, "write to an erased byte: $FF & $5A == $5A");
    CHECK(e.dirty, "write sets dirty");

    /* The same write on a byte holding $0F: a write without an erase
     * can only clear bits. */
    e.rom[5] = 0x0F;
    e.dirty = false;
    ad_er2055_set_addr_data(&e, 5, 0x5A);
    ad_er2055_control(&e, 0x04);
    ad_er2055_control(&e, 0x0C);
    CHECK(e.rom[5] == 0x0A, "write without erase ANDs: $0F & $5A == $0A");
    CHECK(e.dirty, "write sets dirty (second case)");
}

/* ------------------------------------------------------------------ */
/* (4) Read sequence for address 5                                       */
/* ------------------------------------------------------------------ */
static void check_read(void)
{
    ad_er2055 e;

    printf("4. Read sequence for address 5 (EACE, +EACK, EACE)\n");
    ad_er2055_init(&e);
    e.rom[5] = 0x42;

    ad_er2055_set_addr_data(&e, 5, 0xEE);      /* address latched; data is a
                                                 * poison placeholder - the
                                                 * ROM's own read sequence
                                                 * puts EACE (0x08) on the
                                                 * bus here, equally
                                                 * irrelevant, since a real
                                                 * read only ever gets its
                                                 * byte from the clock's
                                                 * falling edge below */
    ad_er2055_control(&e, 0x08);               /* EACE: select, read armed */
    CHECK(ad_er2055_data(&e) == 0xEE, "read: nothing latched before the clock's falling edge");
    ad_er2055_control(&e, 0x09);               /* EACE|EACK: clock rises */
    CHECK(ad_er2055_data(&e) == 0xEE, "read: a rising edge does not latch");
    ad_er2055_control(&e, 0x08);               /* EACE: clock falls while selected - latch */
    CHECK(ad_er2055_data(&e) == 0x42, "read: the falling edge latches rom[address]");

    /* Not selected at all: nothing latched, even across a real falling
     * clock edge. */
    ad_er2055_set_addr_data(&e, 5, 0x99);
    ad_er2055_control(&e, 0x01);               /* EACK only: chip not selected */
    ad_er2055_control(&e, 0x00);               /* clock falls, still not selected */
    CHECK(ad_er2055_data(&e) == 0x99, "read: no latch while the chip was never selected");
}

/* ------------------------------------------------------------------ */
/* (5) Control 0 on a fresh chip                                         */
/* ------------------------------------------------------------------ */
static void check_control_zero_noop(void)
{
    ad_er2055 e, before;

    printf("5. Control 0 on a fresh chip changes nothing observable\n");
    ad_er2055_init(&e);
    before = e;
    ad_er2055_control(&e, 0);
    /* Not a whole-struct compare: this driver's earom_control_w() passes
     * cs2 = 1 on every call (CS2 is tied high on the board), so the
     * internal composite control latch does pick up that bit even here
     * (0 -> C1|CS2, per MAME's own set_control - it always writes
     * m_control_state before checking whether the chip is selected).
     * With CS1 (EACE) still 0 the chip is not selected, so nothing
     * update_state() could touch - rom[], data() and dirty - moves at
     * all; that is the invariant "changes nothing" means. */
    CHECK(memcmp(e.rom, before.rom, sizeof e.rom) == 0, "control 0 on a fresh chip: rom[] unchanged");
    CHECK(e.data == before.data, "control 0 on a fresh chip: data() unchanged");
    CHECK(e.dirty == before.dirty, "control 0 on a fresh chip: dirty unchanged");
}

/* ------------------------------------------------------------------ */
/* (6) Round trip through the ROM's own state machine (ad_eaupd)         */
/* ------------------------------------------------------------------ */
/* One frame's worth of throttle, then a state-machine step - same
 * pacing as probe_earom.c's frame(), but silent (this probe checks the
 * end state, not the trace). */
static void step_intct(void)
{
    g.zp.f.INTCT = (uint8_t)(g.zp.f.INTCT + 4);
    ad_eaupd();
}

static void run_until_idle(int limit)
{
    while (g.zp.f.EAFLG != 0 && limit-- > 0)
        step_intct();
}

static void check_round_trip(void)
{
    ad_er2055 *host = ad_host_earom();
    uint8_t expect[21];
    int k, e, i;

    printf("6. Round trip through ad_eaupd/ad_stearom (STEST6-style erase+write, then START's read)\n");

    /* Prime a recognisable RAM table at the addresses earom.c reads
     * (HSCORE_AT/INITL_AT): three entries of three score bytes and
     * three initials. */
    for (i = 0; i < 9; i++) {
        g.zp.f.HSCORE[i] = (uint8_t)(0x10 + i);
        g.zp.f.INITL[i]  = (uint8_t)(0x40 + i);
    }

    /* STEST6 primes the whole-ROM erase-then-write directly. */
    g.zp.f.EABC = 0x15;
    g.zp.f.EAX = 0x14;
    g.zp.f.EAHSX = 0;
    g.zp.f.EAFLG = 0x80;
    run_until_idle(200);
    CHECK(g.zp.f.EAFLG == 0, "erase+write finishes idle");

    /* Expected: for entry e, EABDS order is score[0..2], initials[0..2],
     * then the checksum - the sum of those six bytes mod 256 - computed
     * here straight from the RAM table just primed, independently of
     * ad_eaupd's own EACS accumulator. */
    k = 0;
    for (e = 0; e < 3; e++) {
        uint8_t sum = 0;
        for (i = 0; i < 3; i++) {
            expect[k] = g.zp.f.HSCORE[3 * e + i];
            sum = (uint8_t)(sum + expect[k]);
            k++;
        }
        for (i = 0; i < 3; i++) {
            expect[k] = g.zp.f.INITL[3 * e + i];
            sum = (uint8_t)(sum + expect[k]);
            k++;
        }
        expect[k++] = sum;
    }
    CHECK(memcmp(host->rom, expect, 21) == 0,
          "ROM holds the 21 bytes in EABDS order with correct per-entry checksums");

    /* START's own read: zero the RAM table, prime EABC=EAX=EAHSX=0,
     * EAFLG=$20, step to idle - the table must come back byte for byte
     * and EABAD stay 0. */
    memset(g.zp.f.HSCORE, 0, sizeof g.zp.f.HSCORE);
    memset(g.zp.f.INITL, 0, sizeof g.zp.f.INITL);
    g.zp.f.EABC = 0;
    g.zp.f.EAX = 0;
    g.zp.f.EAHSX = 0;
    g.zp.f.EABAD = 0;
    g.zp.f.EAFLG = 0x20;
    run_until_idle(200);
    CHECK(g.zp.f.EAFLG == 0, "first read finishes idle");
    {
        int ok = 1;
        for (i = 0; i < 9; i++)
            if (g.zp.f.HSCORE[i] != (uint8_t)(0x10 + i) || g.zp.f.INITL[i] != (uint8_t)(0x40 + i))
                ok = 0;
        CHECK(ok, "a clean read restores the table byte for byte");
    }
    CHECK(g.zp.f.EABAD == 0, "a clean read leaves EABAD at 0");

    /* Corrupt one image byte - entry 1's first score byte, ROM address
     * 7 (entry 0 occupies 0-6, entry 1 occupies 7-13, entry 2 occupies
     * 14-20) - and read again.  Entry 1's checksum now disagrees, so
     * EAUPD_11's compare blanks HSCORE[3..5]/INITL[3..5] and sets
     * EABAD, and - the same quirk probe_earom.c documents, "the next
     * entry's bytes land in the same RAM slots" because EAHSX is not
     * advanced on a mismatch - entry 2's own (uncorrupted, matching)
     * bytes then overwrite those same slots right after, so the final
     * RAM state holds entry 2's data at HSCORE[3..5]/INITL[3..5], not
     * zero, and HSCORE[6..8]/INITL[6..8] are never reached this pass. */
    host->rom[7] = (uint8_t)(host->rom[7] ^ 0xFF);

    /* Zero the RAM table again first, so which slots this second read
     * actually touches - and which it doesn't - is unambiguous. */
    memset(g.zp.f.HSCORE, 0, sizeof g.zp.f.HSCORE);
    memset(g.zp.f.INITL, 0, sizeof g.zp.f.INITL);
    g.zp.f.EABC = 0;
    g.zp.f.EAX = 0;
    g.zp.f.EAHSX = 0;
    g.zp.f.EABAD = 0;
    g.zp.f.EAFLG = 0x20;
    run_until_idle(200);
    CHECK(g.zp.f.EAFLG == 0, "second read finishes idle");
    CHECK(g.zp.f.EABAD == 1, "the corrupted entry sets EABAD");
    CHECK(g.zp.f.EAHSX == 6, "EAHSX still advances to 6 (two matches out of three, ROM quirk)");
    {
        int ok = 1;
        for (i = 0; i < 3; i++)
            if (g.zp.f.HSCORE[i] != (uint8_t)(0x10 + i) || g.zp.f.INITL[i] != (uint8_t)(0x40 + i))
                ok = 0;
        CHECK(ok, "entry 0 (unaffected) reads back correctly");
        ok = 1;
        for (i = 3; i < 6; i++)
            if (g.zp.f.HSCORE[i] != (uint8_t)(0x10 + (i + 3)) || g.zp.f.INITL[i] != (uint8_t)(0x40 + (i + 3)))
                ok = 0;
        CHECK(ok, "entry 2's data lands in slots 3..5 (entry 1's mismatch quirk)");
        ok = 1;
        for (i = 6; i < 9; i++)
            if (g.zp.f.HSCORE[i] != 0 || g.zp.f.INITL[i] != 0)
                ok = 0;
        CHECK(ok, "slots 6..8 are never reached this pass, so stay at the zero they were primed to");
    }
}

int ad_probe(void)
{
    check_fresh();
    check_erase();
    check_write();
    check_read();
    check_control_zero_noop();
    check_round_trip();

    printf("%s: %d failure(s)\n", fails ? "PROBE FAILED" : "PROBE OK", fails);
    return fails ? 1 : 0;
}
