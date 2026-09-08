/* probe_pokey.c - pokey.c's own checks, independent of the game.
 *
 *     build_mod.bat pokey
 *     test_pokey.exe --probe
 *
 * Every expectation below is derived independently of the C
 * translation: computed in Python straight from the generator
 * definitions in MAME 0.286's src/devices/sound/pokey.cpp
 * (poly_init_4_5()/poly_init_9_17(), copied arithmetic-for-arithmetic),
 * then pasted in as constants - the Python is shown in the comment next
 * to each one.  A fresh local ad_pokey is used throughout, never the
 * host's chip.
 */
#include <stdio.h>
#include <string.h>

#include "pokey.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ */
/* (1) Table periods                                                    */
/* ------------------------------------------------------------------ */
/* def poly_init_4_5(size):
 *     mask = (1 << size) - 1; lfsr = 0; xorbit = size - 1; out = []
 *     for i in range(mask):
 *         newbit = (~((lfsr >> 2) ^ (lfsr >> xorbit))) & 1
 *         lfsr = (lfsr << 1) | newbit
 *         out.append((lfsr & mask) & 1)
 *     return out
 *
 * def poly_init_9_17(size):
 *     mask = (1 << size) - 1; lfsr = mask; out = []
 *     for i in range(mask):
 *         if size == 17:
 *             in8 = ((lfsr >> 8) & 1) ^ ((lfsr >> 13) & 1)
 *             inn = lfsr & 1
 *             lfsr >>= 1
 *             lfsr = (lfsr & 0xff7f) | (in8 << 7)
 *             lfsr = (inn << 16) | lfsr
 *         else:
 *             inn = (lfsr & 1) ^ ((lfsr >> 5) & 1)
 *             lfsr >>= 1
 *             lfsr = (inn << 8) | lfsr
 *         out.append(lfsr & 1)
 *     return out
 *
 * poly4  = poly_init_4_5(4)     # MAME pokey_device::poly_init_4_5()
 * poly5  = poly_init_4_5(5)
 * poly9  = poly_init_9_17(9)    # MAME pokey_device::poly_init_9_17()
 * poly17 = poly_init_9_17(17)
 *
 * def smallest_period(seq):
 *     n = len(seq)
 *     for p in range(1, n):
 *         if n % p == 0 and all(seq[i] == seq[i % p] for i in range(n)):
 *             return p
 *     return n
 *
 * smallest_period(poly4)  -> 15       (== table size: no shorter divisor works)
 * smallest_period(poly5)  -> 31       (== table size: 31 is prime)
 * smallest_period(poly9)  -> 511      (== table size: 511 = 7*73, neither divides)
 * smallest_period(poly17) -> 131071   (== table size: 131071 is prime)
 *
 * Unlike the old shift-add recurrence this file started from - whose
 * 9-bit generator returned to its start state after 73 steps and the
 * 17-bit one after 33383, both well short of the tables that hold them
 * - MAME 0.286's true LFSRs are maximal-length: every one of these
 * reaches its full 2^n-1 period before repeating, so all four get the
 * strict "not periodic at any proper divisor of the table length"
 * check (511's proper divisors are 1, 7, 73; 131071 is prime, so its
 * only proper divisor is 1).
 */
static bool periodic_at(const uint8_t *seq, int n, int p)
{
    for (int i = 0; i < n; i++)
        if (seq[i] != seq[i % p])
            return false;
    return true;
}

static void check_periods(void)
{
    const uint8_t *poly4 = ad_pokey_dbg_poly4();
    const uint8_t *poly5 = ad_pokey_dbg_poly5();
    const uint8_t *poly9 = ad_pokey_dbg_poly9();
    const uint8_t *poly17 = ad_pokey_dbg_poly17();

    CHECK(!periodic_at(poly4, 15, 1) && !periodic_at(poly4, 15, 3) && !periodic_at(poly4, 15, 5),
          "poly4 should not be periodic at any proper divisor of 15 (1, 3, 5)");
    CHECK(!periodic_at(poly5, 31, 1),
          "poly5 should not be periodic at the only proper divisor of 31 (prime)");
    CHECK(!periodic_at(poly9, 511, 1) && !periodic_at(poly9, 511, 7) && !periodic_at(poly9, 511, 73),
          "poly9 should not be periodic at any proper divisor of 511 (1, 7, 73)");
    CHECK(!periodic_at(poly17, 131071, 1),
          "poly17 should not be periodic at the only proper divisor of 131071 (prime)");

    printf("periods: poly4=15 poly5=31 poly9=511 poly17=131071 (all maximal-length)\n");
}

/* ------------------------------------------------------------------ */
/* (2)/(3) First 16 RANDOM bytes                                        */
/* ------------------------------------------------------------------ */
/* def rand_init(size):
 *     mask = (1 << size) - 1; lfsr = mask; out = []
 *     for i in range(mask):
 *         if size == 17:
 *             in8 = ((lfsr >> 8) & 1) ^ ((lfsr >> 13) & 1)
 *             inn = lfsr & 1
 *             lfsr >>= 1
 *             lfsr = (lfsr & 0xff7f) | (in8 << 7)
 *             lfsr = (inn << 16) | lfsr
 *             out.append((lfsr >> 8) & 0xFF)     # RANDOM_C: poly17 >> 8
 *         else:
 *             inn = (lfsr & 1) ^ ((lfsr >> 5) & 1)
 *             lfsr >>= 1
 *             lfsr = (inn << 8) | lfsr
 *             out.append(lfsr & 0xFF)            # RANDOM_C: poly9 & 0xff
 *     return out
 *
 * rand17 = rand_init(17)
 * for k in range(16):
 *     print(hex(rand17[(8 + (k + 1) * 64) % 0x1FFFF]))   # no XOR - MAME doesn't invert
 * -> 0x0F 0x00 0xC9 0x8C 0xC1 0xD9 0x80 0xC7 0x36 0xBD 0xCF 0x35 0x41 0x36 0x1B 0x62
 *
 * The 8 is the 17-bit chain's reset position, not 0 - see RAND17_RESET_POS
 * in pokey.h; a chip released from SKCTL reset starts counting from table
 * position 8, so a read 64 cycles after release lands at rand17[8 + 64],
 * not rand17[64].
 *
 * rand9 = rand_init(9)
 * for k in range(16):
 *     print(hex(rand9[(k + 1) * 64 % 0x1FF]))
 * -> 0x75 0xAA 0xB3 0x98 0x45 0x20 0xF3 0x7F 0xBA 0x55 0x59 0xCC 0x22 0x10 0x79 0x3F
 *
 * Both generators are full-period (511/131071 - see check (1)), so
 * these positions aren't a special case of a shorter repeat; they are
 * simply where reads spaced 64 POKEY cycles apart land.  64 is the
 * hosts' stand-in cost for an un-annotated game read (AD_RANDOM_READ_COST
 * in app_win.c/host_stub.c); the chip itself charges nothing per read,
 * so the spacing is fed explicitly here with ad_pokey_advance().
 */
static const uint8_t expect17[16] = {
    0x0F, 0x00, 0xC9, 0x8C, 0xC1, 0xD9, 0x80, 0xC7,
    0x36, 0xBD, 0xCF, 0x35, 0x41, 0x36, 0x1B, 0x62
};
static const uint8_t expect9[16] = {
    0x75, 0xAA, 0xB3, 0x98, 0x45, 0x20, 0xF3, 0x7F,
    0xBA, 0x55, 0x59, 0xCC, 0x22, 0x10, 0x79, 0x3F
};

static void check_random_17(void)
{
    ad_pokey chip;
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_SKCTL, 7);        /* rng on, fast pot; AUDCTL still 0 */
    for (int k = 0; k < 16; k++) {
        ad_pokey_advance(&chip, 64);
        uint8_t v = ad_pokey_read(&chip, R_RANDOM);
        CHECK(v == expect17[k], "RANDOM[%d] (17-bit poly) = %02X, expected %02X", k, v, expect17[k]);
    }
}

static void check_random_9(void)
{
    ad_pokey chip;
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_AUDCTL, CTL_POLY9);   /* 9-bit poly */
    ad_pokey_write(&chip, W_SKCTL, 7);
    for (int k = 0; k < 16; k++) {
        ad_pokey_advance(&chip, 64);
        uint8_t v = ad_pokey_read(&chip, R_RANDOM);
        CHECK(v == expect9[k], "RANDOM[%d] (9-bit poly) = %02X, expected %02X", k, v, expect9[k]);
    }
}

/* ------------------------------------------------------------------ */
/* (4) SKCTL gating RANDOM                                              */
/* ------------------------------------------------------------------ */
/* rand9[0] (index 0 of the rand_init sequence above, before any step
 * runs) is 0xFF: from lfsr = mask (all ones), one step only ever touches
 * bit 8, masked off by RANDOM_C's `& 0xff`, so the 9-bit generator's
 * RANDOM byte doesn't move on step 0.  The 17-bit chain is different: a
 * gate-level transcription of Atari's POKEY schematics shows it does not
 * settle at position 0 while held - it takes several of the chain's own
 * steps to reach a fixed state, and it reaches that state at table
 * position RAND17_RESET_POS (8), not 0.  Entries 0..8 of rand17 (the
 * settling steps and the position itself) are all 0xFF anyway - bit 7
 * (cleared) and bit 16 (set from the fed-back-in bit) stay outside the
 * bits-8-15 window `>> 8 & 0xff` reads for all of them - so a chip held
 * in SKCTL reset (rand_pos9 = 0, rand_pos17 = RAND17_RESET_POS, held
 * there - see read_random()'s comment) reads 0xFF from either table,
 * independent of AUDCTL's poly9/17 select, but only the 9-bit table is
 * actually sitting at its table index 0.
 *
 * The rest of this check is the clock discipline MAME's SKCTL_C handler
 * and step_one_clock() give the reset: while the init bits are clear no
 * cycle moves the polynomial (Tempest's init at $CD95 zeroes SKCTL and
 * then reads RANDOM six times over a few dozen cycles, flagging the chip
 * if any two differ); machine time that passes while held is NOT charged
 * at release - counting starts from the release write, at position 8 for
 * the 17-bit chain; rewriting SKCTL with the same value is a no-op
 * (MAME: `if (data == m_SKCTL) return;`); and a write that changes other
 * bits but keeps the init bits set does not restart the sequence. */
static void check_random_skctl(void)
{
    ad_pokey chip;
    const uint8_t *rand17 = ad_pokey_dbg_rand17();
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_SKCTL, 7);
    ad_pokey_advance(&chip, 64);
    uint8_t first = ad_pokey_read(&chip, R_RANDOM);
    CHECK(first == expect17[0], "sanity: first RANDOM byte should match check (2), got %02X", first);

    ad_pokey_write(&chip, W_SKCTL, 0);         /* init bits clear: $CD95's reset */
    for (int i = 0; i < 6; i++) {
        uint8_t v = ad_pokey_read(&chip, R_RANDOM);
        CHECK(v == 0xFF, "held-reset read %d should be 0xFF (poly17[%d], AUDCTL=0), got %02X",
              i, RAND17_RESET_POS, v);
        ad_pokey_advance(&chip, 4);            /* CMP abs after LDA abs: 4 cycles */
    }

    ad_pokey_advance(&chip, 1000);             /* time spent held: must not count */
    ad_pokey_write(&chip, W_SKCTL, 7);         /* release: position 8 from here */
    uint8_t v = ad_pokey_read(&chip, R_RANDOM);
    CHECK(v == rand17[8], "at release RANDOM should read entry 8, got %02X", v);
    ad_pokey_advance(&chip, 4);
    v = ad_pokey_read(&chip, R_RANDOM);
    CHECK(v == rand17[12], "4 cycles after release RANDOM should read entry 12 (not 1012): got %02X, expected %02X",
          v, rand17[12]);

    ad_pokey_write(&chip, W_SKCTL, 7);         /* same value: no-op */
    v = ad_pokey_read(&chip, R_RANDOM);
    CHECK(v == rand17[12], "rewriting SKCTL with the same value should not restart: got %02X", v);

    ad_pokey_write(&chip, W_SKCTL, 3);         /* init bits still set: no restart */
    v = ad_pokey_read(&chip, R_RANDOM);
    CHECK(v == rand17[12], "SKCTL 7->3 keeps the init bits set and should not restart: got %02X", v);

    ad_pokey_advance(&chip, 60);               /* position 8+64 again */
    v = ad_pokey_read(&chip, R_RANDOM);
    CHECK(v == expect17[0], "64 cycles after release RANDOM should match check (2)'s first byte: got %02X", v);
}

/* ------------------------------------------------------------------ */
/* (5) Tempest's protection: the $AE1F nibble shift                    */
/* ------------------------------------------------------------------ */
/* Tempest ($AE1F, IRQs off) does, per chip,
 *
 *     LDA $60CA       ; R1
 *     LDY $60CA       ; R2, bus strobe exactly 4 CPU cycles after R1's
 *
 * and folds (R1 >> 4) ^ (R2 & 0x0F) into $011F, which must be zero:
 * the upper nibble of one read must be the lower nibble of a read four
 * cycles later.  CPU and POKEY both run at 12.096 MHz / 8 on that board
 * (and on this one), so four CPU cycles are four shifts.  It holds at
 * every position because RANDOM is bits 8..15 of a right-shifting
 * 17-bit register whose feedback only touches bits 7 and 16 (bits 0..7
 * of the 9-bit one, feedback into bit 8): after four shifts, old bits
 * 12..15 sit at 8..11 untouched.  MAME 4.5's changelog records the same
 * requirement ("a shift of 1 per cycle").
 *
 * Checked two ways: as a property of the tables, at every index, and
 * through the chip API from a spread of start positions, in both poly
 * selects, spaced exactly as the ROM's instructions are. */
static void check_tempest_ae1f(void)
{
    const uint8_t *rand17 = ad_pokey_dbg_rand17();
    const uint8_t *rand9 = ad_pokey_dbg_rand9();
    int bad17 = 0, bad9 = 0;
    for (uint32_t k = 0; k < 0x1FFFF; k++)
        if ((rand17[(k + 4) % 0x1FFFF] & 0x0F) != (rand17[k] >> 4))
            bad17++;
    for (uint32_t k = 0; k < 0x1FF; k++)
        if ((rand9[(k + 4) % 0x1FF] & 0x0F) != (rand9[k] >> 4))
            bad9++;
    CHECK(bad17 == 0, "rand17: %d positions where entry k+4's low nibble != entry k's high nibble", bad17);
    CHECK(bad9 == 0, "rand9: %d positions where entry k+4's low nibble != entry k's high nibble", bad9);

    for (int sel = 0; sel < 2; sel++) {
        ad_pokey chip;
        int bad = 0;
        ad_pokey_init(&chip, 1512000, 44100);
        if (sel)
            ad_pokey_write(&chip, W_AUDCTL, CTL_POLY9);
        ad_pokey_write(&chip, W_SKCTL, 7);
        for (int n = 0; n < 200; n++) {
            ad_pokey_advance(&chip, 37u + (uint32_t)n * 131u);   /* arbitrary game time */
            uint8_t r1 = ad_pokey_read(&chip, R_RANDOM);         /* LDA $60CA */
            ad_pokey_advance(&chip, 4);                          /* LDY $60CA strobe */
            uint8_t r2 = ad_pokey_read(&chip, R_RANDOM);
            if ((uint8_t)((r1 >> 4) ^ (r2 & 0x0F)) != 0)         /* $011F's nibble */
                bad++;
        }
        CHECK(bad == 0, "$AE1F check (%s poly): %d of 200 read pairs would fail", sel ? "9-bit" : "17-bit", bad);
    }
    printf("tempest $AE1F: nibble shift holds at all 131071 + 511 table positions\n");
}

/* ------------------------------------------------------------------ */
/* (6) Tempest's liveness check, and the bus read costing nothing      */
/* ------------------------------------------------------------------ */
/* $DA46: LDA $60CA, then up to six CMP $60CA four cycles apart; if every
 * one matched the first byte the chip is flagged dead ($7A).  Any real
 * clock feed passes this.  The second half pins the other side of the
 * contract: a read charges no cycles of its own, so two reads with no
 * machine time between them return the same byte - the host, not the
 * chip, decides what an un-annotated read costs. */
static void check_tempest_da46(void)
{
    ad_pokey chip;
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_SKCTL, 7);
    ad_pokey_advance(&chip, 1234);

    uint8_t a = ad_pokey_read(&chip, R_RANDOM);
    int same = 0;
    for (int i = 0; i < 6; i++) {
        ad_pokey_advance(&chip, 4);
        if (ad_pokey_read(&chip, R_RANDOM) == a)
            same++;
    }
    CHECK(same < 6, "$DA46: six 4-cycle-spaced reads all equal the first; the ROM would flag the chip dead");

    uint8_t b = ad_pokey_read(&chip, R_RANDOM);
    uint8_t c = ad_pokey_read(&chip, R_RANDOM);
    CHECK(b == c, "a RANDOM read must charge no cycles of its own: back-to-back reads gave %02X then %02X", b, c);
}

/* ------------------------------------------------------------------ */
/* (7) SKCTL reset on the audio side                                    */
/* ------------------------------------------------------------------ */
/* MAME's SKCTL_C handler zeroes m_p4/m_p5/m_p9/m_p17 on the transition
 * into reset, and step_one_clock() clocks neither polys nor channel
 * dividers while SK_RESET is low.  So: a chip that has been rendering
 * noise has non-zero poly phases; SKCTL=0 zeroes them; rendering while
 * held produces a flat line, moves no countdown and no phase; release
 * resumes from the frozen countdown with the phases restarted from the
 * seed.  AUDF1=3 keeps the channel period (4 * 28 = 112 ticks) above the
 * renderer's one-sample freeze threshold (34 ticks at 44.1 kHz). */
static void check_skctl_audio_reset(void)
{
    ad_pokey chip;
    int16_t buf[512];
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_AUDCTL, 0x00);
    ad_pokey_write(&chip, W_AUDC1, 0x88);         /* NOTPOLY5, 17-bit noise, volume 8 */
    ad_pokey_write(&chip, W_AUDF1, 0x03);
    ad_pokey_write(&chip, W_SKCTL, 7);

    ad_pokey_render(&chip, buf, 512);
    CHECK(chip.p17 != 0 || chip.poly_adjust != 0, "sanity: rendering noise should move the poly phases");

    ad_pokey_write(&chip, W_SKCTL, 0);            /* into reset */
    CHECK(chip.p4 == 0 && chip.p5 == 0 && chip.p9 == 0 && chip.p17 == 0 && chip.poly_adjust == 0,
          "SKCTL reset should zero the render poly phases (p4=%u p5=%u p9=%u p17=%u adj=%u)",
          chip.p4, chip.p5, chip.p9, chip.p17, chip.poly_adjust);

    const uint32_t cnt_before = chip.cnt[0];
    ad_pokey_render(&chip, buf, 256);
    bool flat = true;
    for (int i = 1; i < 256; i++)
        if (buf[i] != buf[0]) flat = false;
    CHECK(flat, "rendering while held in reset should be a flat line");
    CHECK(chip.cnt[0] == cnt_before, "channel countdown should not move while held (was %u, now %u)",
          cnt_before, chip.cnt[0]);
    CHECK(chip.p17 == 0 && chip.poly_adjust == 0, "poly phases should not move while held");

    ad_pokey_write(&chip, W_SKCTL, 7);            /* release */
    ad_pokey_render(&chip, buf, 512);
    bool moved = false;
    for (int i = 1; i < 512; i++)
        if (buf[i] != buf[0]) moved = true;
    CHECK(moved, "noise should resume after release");
    CHECK(chip.p17 != 0 || chip.poly_adjust != 0, "poly phases should advance again after release");
}

/* ------------------------------------------------------------------ */
/* (8) ALLPOT                                                           */
/* ------------------------------------------------------------------ */
static void check_allpot(void)
{
    ad_pokey chip;
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_set_allpot(&chip, 0x01);

    /* never scanned - fresh from power-up with SKCTL still 0, as this
     * board's INIT reads it, or Battlezone, which straps switches to the
     * pot pins and never issues POTGO: the input comparators' mask, the
     * DIP byte.  Altirra HRM: init mode does not touch the pot logic;
     * the power-up scan sits mid-way until the first POTGO; ALLPOT is
     * updated from the inputs every cycle.  (MAME's 0 here is wrong.) */
    uint8_t v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x01, "ALLPOT before any POTGO should read the DIP byte, got %02X", v);

    ad_pokey_write(&chip, W_SKCTL, 7);          /* fast pot */
    ad_pokey_write(&chip, W_POTGO, 7);
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x01, "ALLPOT immediately after POTGO should still read the DIP byte, got %02X", v);

    /* live from the comparators mid-scan: a line that trips (goes high)
     * reads 0, one that drops back below threshold reads 1 again */
    ad_pokey_advance(&chip, 100);
    ad_pokey_set_allpot(&chip, 0x00);            /* every line tripped */
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x00, "ALLPOT mid-scan with every line tripped should read 0, got %02X", v);
    ad_pokey_set_allpot(&chip, 0x81);            /* two lines back below threshold */
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x81, "ALLPOT mid-scan should follow lines that drop back below threshold, got %02X", v);

    /* fast pot: the counter stops at 229, not 228 (Altirra HRM 5.9) */
    ad_pokey_advance(&chip, 128);                /* 228 cycles into the scan */
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x81, "ALLPOT at 228 fast-pot cycles should still be scanning, got %02X", v);
    ad_pokey_advance(&chip, 1);                  /* 229: terminal count */
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x00, "ALLPOT after the fast-pot scan (229 cycles) should read 0, got %02X", v);
    ad_pokey_set_allpot(&chip, 0x9D);            /* pins change after the scan: still forced 0 */
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x00, "ALLPOT after the scan is forced to 0 regardless of the inputs, got %02X", v);
    ad_pokey_write(&chip, W_POTGO, 7);           /* the next scan sees the new pins */
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x9D, "ALLPOT right after the next POTGO should read the new pin mask, got %02X", v);

    /* slow pot: 228 counts of the 15 kHz clock, 114 cycles each */
    ad_pokey_write(&chip, W_SKCTL, 3);
    ad_pokey_write(&chip, W_POTGO, 7);
    ad_pokey_advance(&chip, 228u * DIV_15 - 1);
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x9D, "ALLPOT one cycle short of the slow scan's end should still be scanning, got %02X", v);
    ad_pokey_advance(&chip, 1);
    v = ad_pokey_read(&chip, R_ALLPOT);
    CHECK(v == 0x00, "ALLPOT after the slow-pot scan (228 x 114 cycles) should read 0, got %02X", v);
}

/* ------------------------------------------------------------------ */
/* (9) Tone                                                             */
/* ------------------------------------------------------------------ */
static void check_tone(void)
{
    ad_pokey chip;
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_AUDCTL, 0x00);        /* base_mult = DIV_64 */
    ad_pokey_write(&chip, W_AUDC1, 0xA8);         /* NOTPOLY5 | PURE | volume 8 */
    ad_pokey_write(&chip, W_AUDF1, 0x1F);

    /* A chip that has never had its SKCTL init bits set is still in
     * reset (MAME: SK_RESET low, nothing clocks), so it renders a flat
     * line however the channels are programmed.  Every ROM releases it
     * first - this board writes SKCTL=7 in the NMI, Tempest at $CDBA. */
    {
        int16_t held[100];
        bool flat = true;
        ad_pokey_render(&chip, held, 100);
        for (int i = 1; i < 100; i++)
            if (held[i] != held[0]) flat = false;
        CHECK(flat, "a chip never released from SKCTL reset should render a flat line");
    }
    ad_pokey_write(&chip, W_SKCTL, 3);            /* release: the chip runs */

    /* channel_period(ch0, AUDCTL=0) = (AUDF1 + 1) * DIV_64 = 32 * 28 = 896
     * POKEY cycles - the TRUE half-period; a full square-wave cycle is
     * 2 * 896 = 1792 cycles. */
    const int half_period = (0x1F + 1) * DIV_64;
    CHECK(half_period == 896, "half_period should be 896, got %d", half_period);

    enum { N = 4410 };
    int16_t samples[N];
    ad_pokey_render(&chip, samples, N);

    int sign_changes = 0, last_sign = 0;
    for (int i = 0; i < N; i++) {
        int s = (samples[i] > 0) - (samples[i] < 0);
        if (s != 0 && last_sign != 0 && s != last_sign)
            sign_changes++;
        if (s != 0)
            last_sign = s;
    }

    /* expected = N * (base_clock / sys_freq) / half_period
     *          = 4410 * (1512000 / 44100) / 896 = 168.75 */
    double expected = (double)N * (1512000.0 / 44100.0) / half_period;
    printf("tone: half_period=%d sign_changes=%d expected=%.2f\n",
           half_period, sign_changes, expected);
    CHECK(sign_changes >= expected - 2 && sign_changes <= expected + 2,
          "sign changes %d should be within +-2 of %.2f", sign_changes, expected);
}

/* ------------------------------------------------------------------ */
/* (10) Silence                                                         */
/* ------------------------------------------------------------------ */
static void check_silence(void)
{
    ad_pokey chip;
    enum { N = 100 };
    int16_t samples[N];
    bool all_zero;

    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_SKCTL, 3);         /* released: silence must come from the volume, not the reset */
    ad_pokey_write(&chip, W_AUDCTL, 0x00);
    ad_pokey_write(&chip, W_AUDC1, 0xA0);      /* NOTPOLY5 | PURE, volume 0 */
    ad_pokey_write(&chip, W_AUDF1, 0x1F);

    memset(samples, 0xFF, sizeof samples);     /* poison: a real 0 must be written */
    ad_pokey_render(&chip, samples, N);
    all_zero = true;
    for (int i = 0; i < N; i++)
        if (samples[i] != 0) all_zero = false;
    CHECK(all_zero, "AUDC1=0xA0 (volume 0) should render all zeros");

    ad_pokey_reset(&chip);
    memset(samples, 0xFF, sizeof samples);
    ad_pokey_render(&chip, samples, N);
    all_zero = true;
    for (int i = 0; i < N; i++)
        if (samples[i] != 0) all_zero = false;
    CHECK(all_zero, "after reset, all four channels should render zeros");
}

/* ------------------------------------------------------------------ */
/* (11) IRQ timers, keyboard, serial, pots                             */
/* ------------------------------------------------------------------ */
/* A host stub that records every callback the chip can make, so this
 * check drives ad_pokey_write()/read()/advance()/keyboard_key()/
 * serial_receive()/poll() through the same ad_pokey_host seam a real
 * host uses, rather than reaching into the chip directly.  th is a
 * fresh static struct per check_host_seam() call (there's only one, but
 * memset at the top keeps it that way if that changes). */
static struct {
    uint8_t irq_mask;             /* OR of every raise_irq() mask seen */
    int     irq_calls;
    uint8_t serout_byte;
    int     serout_calls;
    int     kbd_code_queued;      /* -1 once keyboard_scan has consumed it */
    int     serial_byte_queued;   /* -1 once serial_in has consumed it */
} th;

static void th_raise_irq(void *ctx, uint8_t mask)
{
    (void)ctx;
    th.irq_mask |= mask;
    th.irq_calls++;
}

static int th_pot_read(void *ctx, int n)
{
    (void)ctx;
    return n * 10;
}

static int th_keyboard_scan(void *ctx, uint8_t *code, uint8_t *flags)
{
    (void)ctx;
    if (th.kbd_code_queued < 0)
        return 0;
    *code = (uint8_t)th.kbd_code_queued;
    *flags = 0;
    th.kbd_code_queued = -1;
    return 1;
}

static int th_serial_in(void *ctx)
{
    (void)ctx;
    if (th.serial_byte_queued < 0)
        return -1;
    int b = th.serial_byte_queued;
    th.serial_byte_queued = -1;
    return b;
}

static void th_serial_out(void *ctx, uint8_t data)
{
    (void)ctx;
    th.serout_byte = data;
    th.serout_calls++;
}

static const ad_pokey_host test_host = {
    NULL, th_raise_irq, th_pot_read, th_keyboard_scan, th_serial_in, th_serial_out
};

static void check_host_seam(void)
{
    ad_pokey chip;
    ad_pokey_init(&chip, 1512000, 44100);
    memset(&th, 0, sizeof th);
    th.kbd_code_queued = -1;
    th.serial_byte_queued = -1;
    ad_pokey_set_host(&chip, &test_host);

    /* --- timer 0 (TIMR1, channel 0): SKCTL=7, AUDCTL=0, AUDF1=0 ->
     * period = (0 + 1) * DIV_64 = 28 cycles. --- */
    ad_pokey_write(&chip, W_SKCTL, 7);
    ad_pokey_write(&chip, W_AUDCTL, 0x00);
    ad_pokey_write(&chip, W_AUDF1, 0x00);
    ad_pokey_write(&chip, W_IRQEN, IRQ_TIMR1);

    ad_pokey_advance(&chip, 27);
    CHECK(th.irq_calls == 0, "timer1: no IRQ after 27 of 28 cycles, got %d call(s)", th.irq_calls);
    uint8_t irqst = ad_pokey_read(&chip, R_IRQST);
    CHECK(irqst == 0xFF, "timer1: R_IRQST should read 0xFF before the first borrow, got %02X", irqst);

    ad_pokey_advance(&chip, 1);            /* the 28th cycle: the borrow fires */
    CHECK(th.irq_calls == 1 && th.irq_mask == IRQ_TIMR1,
          "timer1: raise_irq(0x01) should fire at cycle 28, got %d call(s), mask %02X",
          th.irq_calls, th.irq_mask);
    irqst = ad_pokey_read(&chip, R_IRQST);
    CHECK(irqst == (uint8_t)~IRQ_TIMR1,
          "timer1: R_IRQST should read 0xFE (bit 0 set) after the borrow, got %02X", irqst);

    ad_pokey_advance(&chip, 28 * 5);       /* five more periods in one slice: IRQST is a latch */
    CHECK(th.irq_calls == 2, "timer1: a slice spanning several periods should still call raise_irq() once, got %d total",
          th.irq_calls);
    CHECK(th.irq_mask == IRQ_TIMR1, "timer1: IRQST should still just be 0x01, got %02X", th.irq_mask);

    ad_pokey_write(&chip, W_IRQEN, 0);     /* disabling clears the pending IRQST bit */
    irqst = ad_pokey_read(&chip, R_IRQST);
    CHECK(irqst == 0xFF, "timer1: IRQEN=0 should clear IRQST, R_IRQST should read 0xFF, got %02X", irqst);

    int calls_before_gate = th.irq_calls;
    ad_pokey_advance(&chip, 200);          /* the timer keeps counting; IRQEN=0 only gates the callback */
    CHECK(th.irq_calls == calls_before_gate,
          "timer1: no raise_irq() while IRQEN=0, got %d new call(s)", th.irq_calls - calls_before_gate);

    /* Re-enable: the countdown was never reset by the IRQEN writes, so
     * the next borrow lands wherever the 28-cycle phase says it should,
     * not 28 cycles after this write.  168 cycles (28 + 140) have run
     * since the last rearm (the AUDF1 write) with the IRQ gated off for
     * the last 200 of those; 200 % 28 == 4, so the countdown has 24
     * cycles left, not a fresh 28. */
    ad_pokey_write(&chip, W_IRQEN, IRQ_TIMR1);
    int calls_before_phase = th.irq_calls;
    ad_pokey_advance(&chip, 23);
    CHECK(th.irq_calls == calls_before_phase,
          "timer1: phase kept through the gated slice - 23 more cycles should not reach it yet, got %d new call(s)",
          th.irq_calls - calls_before_phase);
    ad_pokey_advance(&chip, 1);
    CHECK(th.irq_calls == calls_before_phase + 1,
          "timer1: the 24th cycle should reach the phase-preserved borrow, got %d new call(s)",
          th.irq_calls - calls_before_phase);

    /* --- timer 1 (TIMR2, channel 1): CH12_JOIN + CH1_HICLK, AUDF1=0x10,
     * AUDF2=0x00 -> period = AUDF2*256 + AUDF1 + 7 = 0x10 + 7 = 23
     * cycles; the AUDCTL write itself re-arms it. --- */
    ad_pokey_write(&chip, W_AUDF1, 0x10);
    ad_pokey_write(&chip, W_AUDF2, 0x00);
    ad_pokey_write(&chip, W_IRQEN, IRQ_TIMR2);
    ad_pokey_write(&chip, W_AUDCTL, CTL_CH12_JOIN | CTL_CH1_HICLK);
    CHECK(chip.divisor[1] == 23, "timer2 setup: channel 1's divisor should be 23, got %u",
          (unsigned)chip.divisor[1]);

    int calls_before_t2 = th.irq_calls;
    th.irq_mask = 0;   /* was left holding IRQ_TIMR1 from the section above */
    ad_pokey_advance(&chip, 22);
    CHECK(th.irq_calls == calls_before_t2, "timer2: no IRQ after 22 of 23 cycles, got %d new call(s)",
          th.irq_calls - calls_before_t2);
    ad_pokey_advance(&chip, 1);
    CHECK(th.irq_calls == calls_before_t2 + 1 && th.irq_mask == IRQ_TIMR2,
          "timer2: raise_irq(0x02) should fire at cycle 23, got %d new call(s), mask %02X",
          th.irq_calls - calls_before_t2, th.irq_mask);

    /* STIMER re-arms every timer to a full period from now, discarding
     * whatever phase it was mid-way through. */
    ad_pokey_advance(&chip, 10);           /* 10 cycles into the next 23-cycle period */
    ad_pokey_write(&chip, W_STIMER, 0);
    int calls_before_st = th.irq_calls;
    ad_pokey_advance(&chip, 22);
    CHECK(th.irq_calls == calls_before_st,
          "STIMER: no IRQ 22 cycles after STIMER (would have fired already without the rearm), got %d new call(s)",
          th.irq_calls - calls_before_st);
    ad_pokey_advance(&chip, 1);
    CHECK(th.irq_calls == calls_before_st + 1,
          "STIMER: a full 23-cycle period after STIMER should reach the borrow, got %d new call(s)",
          th.irq_calls - calls_before_st);

    /* While SKCTL holds the chip in reset, no timer counts - see
     * ad_pokey_advance(). */
    ad_pokey_write(&chip, W_SKCTL, 0);
    int calls_before_held = th.irq_calls;
    ad_pokey_advance(&chip, 1000);
    CHECK(th.irq_calls == calls_before_held,
          "held reset: no timer IRQ over 1000 cycles, got %d new call(s)", th.irq_calls - calls_before_held);
    ad_pokey_write(&chip, W_SKCTL, 7);     /* release for the keyboard/serial/pot checks below */

    /* --- keyboard --- */
    ad_pokey_write(&chip, W_IRQEN, IRQ_KEYBD);
    th.irq_calls = 0; th.irq_mask = 0;
    ad_pokey_keyboard_key(&chip, 0x21, ST_SHIFT, true);
    CHECK(th.irq_calls == 1 && th.irq_mask == IRQ_KEYBD,
          "keyboard: key down with IRQEN=IRQ_KEYBD should raise 0x40, got %d call(s), mask %02X",
          th.irq_calls, th.irq_mask);
    CHECK((chip.SKSTAT & (ST_KEYBD | ST_SHIFT)) == (ST_KEYBD | ST_SHIFT),
          "keyboard: SKSTAT should have ST_KEYBD|ST_SHIFT set, got %02X", chip.SKSTAT);
    CHECK(chip.KBCODE == 0x21, "keyboard: KBCODE should latch 0x21, got %02X", chip.KBCODE);

    /* A second key before KBCODE is read finds the prior code still
     * pending and sets the overrun bit, ST_KBERR. */
    ad_pokey_keyboard_key(&chip, 0x22, 0, true);
    CHECK((chip.SKSTAT & ST_KBERR) != 0, "keyboard: a second key before the read should set ST_KBERR, got %02X",
          chip.SKSTAT);

    ad_pokey_write(&chip, W_SKREST, 0);
    CHECK((chip.SKSTAT & ST_KBERR) == 0, "keyboard: SKREST should clear ST_KBERR, got %02X", chip.SKSTAT);

    uint8_t kb = ad_pokey_read(&chip, R_KBCODE);   /* R_KBCODE also clears kbd_pending */
    CHECK(kb == 0x22, "keyboard: R_KBCODE should read the still-latched second code, got %02X", kb);

    ad_pokey_keyboard_key(&chip, 0x22, 0, false);  /* key up */
    CHECK((chip.SKSTAT & ST_KEYBD) == 0, "keyboard: key up should clear ST_KEYBD, got %02X", chip.SKSTAT);

    ad_pokey_write(&chip, W_SKCTL, 0);             /* held: keyboard_key must be ignored */
    uint8_t skstat_before = chip.SKSTAT;
    ad_pokey_keyboard_key(&chip, 0x33, 0, true);
    CHECK(chip.SKSTAT == skstat_before, "keyboard: keyboard_key while SKCTL=0 should be ignored, got %02X (was %02X)",
          chip.SKSTAT, skstat_before);
    ad_pokey_write(&chip, W_SKCTL, 7);             /* release again */

    /* --- serial --- */
    ad_pokey_write(&chip, W_IRQEN, IRQ_SERIN);
    th.irq_calls = 0; th.irq_mask = 0;
    ad_pokey_serial_receive(&chip, 0x5A);
    CHECK(th.irq_calls == 1 && th.irq_mask == IRQ_SERIN,
          "serial: serial_receive with IRQEN=IRQ_SERIN should raise 0x20, got %d call(s), mask %02X",
          th.irq_calls, th.irq_mask);
    CHECK((chip.SKSTAT & ST_SERIN_BUSY) != 0, "serial: SKSTAT should have ST_SERIN_BUSY set, got %02X", chip.SKSTAT);

    uint8_t sv = ad_pokey_read(&chip, R_SERIN);
    CHECK(sv == 0x5A, "serial: R_SERIN should read 0x5A, got %02X", sv);
    CHECK((chip.SKSTAT & ST_SERIN_BUSY) == 0, "serial: R_SERIN read should clear ST_SERIN_BUSY, got %02X", chip.SKSTAT);

    ad_pokey_write(&chip, W_SEROUT, 0xA5);
    CHECK(th.serout_calls == 1 && th.serout_byte == 0xA5,
          "serial: W_SEROUT should call serial_out(0xA5), got %d call(s), byte %02X",
          th.serout_calls, th.serout_byte);

    /* --- poll: one keyboard code and one serial byte per call --- */
    th.kbd_code_queued = 0x55;
    th.serial_byte_queued = 0x66;
    ad_pokey_poll(&chip);
    CHECK(chip.KBCODE == 0x55 && (chip.SKSTAT & ST_KEYBD) != 0,
          "poll: should pull the queued keyboard code through keyboard_scan, got KBCODE=%02X SKSTAT=%02X",
          chip.KBCODE, chip.SKSTAT);
    CHECK(chip.SERIN == 0x66 && (chip.SKSTAT & ST_SERIN_BUSY) != 0,
          "poll: should pull the queued serial byte through serial_in, got SERIN=%02X SKSTAT=%02X",
          chip.SERIN, chip.SKSTAT);
    CHECK(th.kbd_code_queued == -1 && th.serial_byte_queued == -1,
          "poll: should consume both queued items in one call");

    /* --- pots --- */
    uint8_t pv = ad_pokey_read(&chip, R_POT0 + 3);
    CHECK(pv == 30, "pots: R_POT0+3 with the host should read pot_read(ctx,3) = 3*10 = 30, got %d", pv);

    ad_pokey_set_host(&chip, NULL);
    pv = ad_pokey_read(&chip, R_POT0 + 3);
    CHECK(pv == 0xFF, "pots: R_POT0+3 with no host should read 0xFF, got %02X", pv);
}

int ad_probe(void)
{
    check_periods();
    check_random_17();
    check_random_9();
    check_random_skctl();
    check_tempest_ae1f();
    check_tempest_da46();
    check_skctl_audio_reset();
    check_allpot();
    check_tone();
    check_silence();
    check_host_seam();

    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 2 : 0;
}
