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

/* Where the tables' 17-bit sequence is when the chip's chain has
 * settled under SKCTL reset: entries 0..8 of rand17 are all 0xFF and
 * the chip rests at 8 (see pokey.c's "RANDOM shift chain" section and
 * check (12) below); the 9-bit sequence rests at 0. */
#define RESET_POS17 8

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
 * The 8 is where the chip's settled reset state sits in this sequence
 * (RESET_POS17 above), not 0: a chip released from a full SKCTL reset
 * continues from table position 8, so a read 64 cycles after release
 * lands at rand17[8 + 64], not rand17[64].
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
    ad_pokey_advance(&chip, 8);                   /* the select takes a clock to reach the
                                                   * switch (and blanks it for one); a held
                                                   * chain absorbs that, and a ROM's two
                                                   * stores are at least four cycles apart */
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
/* A held chip reads 0xFF - but not from the write on.  SKCTL's init bits
 * clear feed zeros into the head of the 9-bit register one per clock,
 * so RANDOM (its complement) fills with ones from the top: eight clocks
 * after the write it is 0xFF and stays there.  rand9[0] (index 0 of the
 * rand_init sequence above) is 0xFF because from lfsr = mask (all ones)
 * one step only ever touches bit 8, masked off by RANDOM_C's `& 0xff`;
 * in the 17-bit sequence entries 0..8 are all 0xFF - bit 7 (cleared)
 * and bit 16 (set from the fed-back-in bit) stay outside the bits-8-15
 * window `>> 8 & 0xff` reads for all of them - and the chip's settled
 * state is entry 8 (RESET_POS17), not 0.  Check (12) pins the clock-by-
 * clock behaviour against a register-level model; this check keeps the
 * ROM-facing contract, and the contract that matters is Tempest's POKEY
 * protection at $CD95 (the commented Tempest source), played here on
 * two chips with the listing's own cycle counts, the write strobe of
 * each STA and the read strobe of each LDA/LDY/CMP/CPY on the
 * instruction's last cycle:
 *
 *     CD95 LDA #0          2
 *     CD97 STA $60CF       4   POKEY1 SKCTL = 0    strobe at cycle 0
 *     CD9A STA $60DF       4   POKEY2 SKCTL = 0    strobe at 4
 *     CD9D STA $0720       4
 *     CDA0 LDX #4          2
 *     CDA2 LDA $60CA       4   POKEY1 RANDOM       read at 14
 *     CDA5 LDY $60DA       4   POKEY2 RANDOM       read at 18
 *     CDA8 CMP $60CA       4   POKEY1 RANDOM       read at 22, 38, 54, 70, 86
 *     CDAB BNE $CDB0       2   (a mismatch flags $0720 and leaves)
 *     CDAD CPY $60DA       4   POKEY2 RANDOM       read at 26, 42, 58, 74, 90
 *     CDB0 BEQ $CDB7       3   (a mismatch flags $0720 and leaves)
 *     CDB7 DEX             2
 *     CDB8 BPL $CDA8       3   five compares in all
 *     CDBA LDA #7          2
 *     CDBC STA $60CF       4   POKEY1 SKCTL = 7    strobe at 100
 *     CDBF STA $60DF       4   POKEY2 SKCTL = 7    strobe at 104
 *
 * So each chip's first read lands fourteen clocks after its own SKCTL
 * store, six clocks after its 9-bit register has filled with zeros,
 * and every compare must then see the same 0xFF the LDA/LDY saw.  Any
 * model that reads 0xFF from eight clocks after the write on passes;
 * the chip has six clocks of margin.
 *
 * The rest is the clock discipline: rewriting SKCTL with the same value
 * is a no-op (MAME: `if (data == m_SKCTL) return;`); a write that
 * changes other bits but keeps the init bits set does not restart the
 * sequence; and time spent held leaves nothing to catch up on at
 * release, because the settled chain is a fixed point - a release after
 * 1000 held cycles reads the same as one after 17. */
static void check_random_skctl(void)
{
    ad_pokey chip;
    const uint8_t *rand17 = ad_pokey_dbg_rand17();
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_SKCTL, 7);
    ad_pokey_advance(&chip, 64);
    uint8_t first = ad_pokey_read(&chip, R_RANDOM);
    CHECK(first == expect17[0], "sanity: first RANDOM byte should match check (2), got %02X", first);

    /* $CD95 on both POKEYs, from a running state a game's worth of
     * cycles in, with the listing's spacing.  Both chips are advanced
     * together, as on the board. */
    {
        ad_pokey p1, p2;
        ad_pokey_init(&p1, 1512000, 44100);
        ad_pokey_init(&p2, 1512000, 44100);
        ad_pokey_write(&p1, W_SKCTL, 7);
        ad_pokey_write(&p2, W_SKCTL, 7);
        ad_pokey_advance(&p1, 123457); ad_pokey_advance(&p2, 123457 + 4);
        ad_pokey_write(&p1, W_SKCTL, 0);                             /* CD97, cycle 0 */
        ad_pokey_advance(&p1, 4); ad_pokey_advance(&p2, 4);
        ad_pokey_write(&p2, W_SKCTL, 0);                             /* CD9A, cycle 4 */
        ad_pokey_advance(&p1, 10); ad_pokey_advance(&p2, 10);
        uint8_t a = ad_pokey_read(&p1, R_RANDOM);                    /* CDA2, cycle 14 */
        ad_pokey_advance(&p1, 4); ad_pokey_advance(&p2, 4);
        uint8_t y = ad_pokey_read(&p2, R_RANDOM);                    /* CDA5, cycle 18 */
        CHECK(a == 0xFF && y == 0xFF, "$CDA2/$CDA5: the first reads after the reset should be 0xFF, got %02X %02X", a, y);
        int flagged = 0;
        for (int x = 4; x >= 0; x--) {
            ad_pokey_advance(&p1, 4); ad_pokey_advance(&p2, 4);
            if (ad_pokey_read(&p1, R_RANDOM) != a) flagged++;        /* CDA8 CMP */
            ad_pokey_advance(&p1, 4); ad_pokey_advance(&p2, 4);
            if (ad_pokey_read(&p2, R_RANDOM) != y) flagged++;        /* CDAD CPY */
            ad_pokey_advance(&p1, 8); ad_pokey_advance(&p2, 8);      /* BEQ DEX BPL */
        }
        CHECK(flagged == 0, "$CD95: %d compare(s) differed from the first read; the ROM would flag $0720", flagged);
        ad_pokey_advance(&p1, 2); ad_pokey_advance(&p2, 2);          /* BPL not taken, LDA #7 */
        ad_pokey_write(&p1, W_SKCTL, 7);                             /* CDBC, cycle 100 */
        ad_pokey_advance(&p1, 4); ad_pokey_advance(&p2, 4);
        ad_pokey_write(&p2, W_SKCTL, 7);                             /* CDBF, cycle 104 */
        ad_pokey_advance(&p1, 64); ad_pokey_advance(&p2, 64);
        CHECK(ad_pokey_read(&p1, R_RANDOM) != 0xFF || ad_pokey_read(&p2, R_RANDOM) != 0xFF,
              "$CDBC/$CDBF: both chips should be running again after the release");
    }

    ad_pokey_write(&chip, W_SKCTL, 0);         /* init bits clear: $CD95's reset */
    ad_pokey_advance(&chip, 1000);             /* time spent held: the chain is at rest */
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
/* $DA46 (the commented Tempest source), on a running chip:
 *
 *     DA44 LDX #5          2
 *     DA46 LDA $60CA       4   read at cycle 0
 *     DA49 CMP $60CA       4   read at 4, then 15, 26, 37, 48, 59
 *     DA4C BNE $DA53       3   a mismatch leaves: the chip is alive
 *     DA4E DEX             2
 *     DA4F BPL $DA49       3
 *     DA51 STA $7A             six matches: the chip is flagged dead
 *
 * The same again on POKEY2 at $DA55.  Any real clock feed passes this:
 * with the chain shifting once a cycle, the read four cycles on has
 * the first read's high nibble as its low nibble (check (5)) and a
 * fresh high nibble, so two equal reads are a 1-in-16 coincidence and
 * six in a row do not happen.  The second half pins the other side of
 * the contract: a read charges no cycles of its own, so two reads with
 * no machine time between them return the same byte - the host, not
 * the chip, decides what an un-annotated read costs. */
static void check_tempest_da46(void)
{
    ad_pokey chip;
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_write(&chip, W_SKCTL, 7);
    ad_pokey_advance(&chip, 1234);

    int dead = 0;
    for (int trial = 0; trial < 50; trial++) {
        ad_pokey_advance(&chip, 977u + (uint32_t)trial * 313u);    /* somewhere in a game */
        uint8_t a = ad_pokey_read(&chip, R_RANDOM);                 /* DA46 */
        int same = 0;
        ad_pokey_advance(&chip, 4);
        for (int x = 5; x >= 0; x--) {
            if (ad_pokey_read(&chip, R_RANDOM) == a) same++;        /* DA49 */
            else break;
            ad_pokey_advance(&chip, 11);                            /* BNE DEX BPL CMP */
        }
        if (same == 6) dead++;
    }
    CHECK(dead == 0, "$DA46: %d of 50 runs saw six compares equal the first read; the ROM would flag the chip dead", dead);

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
/* (11) IRQ timers, keyboard and SKSTAT, the serial port, pots          */
/* ------------------------------------------------------------------ */
/* A host stub that records every callback the chip can make, so this
 * check drives ad_pokey_write()/read()/advance()/keyboard_key()/
 * serial_receive()/poll() through the same ad_pokey_host seam a real
 * host uses, rather than reaching into the chip directly.  th is a
 * fresh static struct per check_host_seam() call (there's only one, but
 * memset at the top keeps it that way if that changes).  The serial
 * expectations are the chip's: two borrows of the selected timer per
 * bit, ten bits per frame, the load of the shifter as the "data needed"
 * event, the idle level as "transmission finished", and an overrun
 * being a byte that completes while the previous one's IRQ is still
 * pending (SER_core.v, IRQ_core.v, SKSTAT_reg.v). */
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
    /* IRQST bit 3 is the transmitter's idle level, low on a quiet chip,
     * so an idle chip reads 0xF7 - the timer reads below look past it. */
    uint8_t irqst = ad_pokey_read(&chip, R_IRQST);
    CHECK(irqst == (uint8_t)~IRQ_SEROC, "timer1: R_IRQST should read 0xF7 before the first borrow, got %02X", irqst);

    ad_pokey_advance(&chip, 1);            /* the 28th cycle: the borrow fires */
    CHECK(th.irq_calls == 1 && th.irq_mask == IRQ_TIMR1,
          "timer1: raise_irq(0x01) should fire at cycle 28, got %d call(s), mask %02X",
          th.irq_calls, th.irq_mask);
    irqst = ad_pokey_read(&chip, R_IRQST);
    CHECK(irqst == (uint8_t)~(IRQ_TIMR1 | IRQ_SEROC),
          "timer1: R_IRQST should read 0xF6 (bit 0 low) after the borrow, got %02X", irqst);

    ad_pokey_advance(&chip, 28 * 5);       /* five more periods in one slice: IRQST is a latch */
    CHECK(th.irq_calls == 2, "timer1: a slice spanning several periods should still call raise_irq() once, got %d total",
          th.irq_calls);
    CHECK(th.irq_mask == IRQ_TIMR1, "timer1: IRQST should still just be 0x01, got %02X", th.irq_mask);

    ad_pokey_write(&chip, W_IRQEN, 0);     /* disabling clears the pending IRQST bit */
    irqst = ad_pokey_read(&chip, R_IRQST);
    CHECK(irqst == (uint8_t)~IRQ_SEROC, "timer1: IRQEN=0 should clear IRQST, R_IRQST should read 0xF7, got %02X", irqst);

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

    /* While SKCTL holds the chip in reset only the 15/64 kHz clocks
     * stop.  Channel 2 here is joined to a fast-clock channel 1, so its
     * timer keeps counting through the hold; switch AUDCTL to the slow
     * clock (which re-arms every timer) and the same timer stands still
     * for the rest of the hold, then resumes with its phase intact at
     * release - see timer_runs() in pokey.c. */
    ad_pokey_write(&chip, W_SKCTL, 0);
    int calls_before_held = th.irq_calls;
    ad_pokey_advance(&chip, 1000);
    CHECK(th.irq_calls > calls_before_held,
          "held reset: a timer on the fast clock keeps firing, got %d new call(s)",
          th.irq_calls - calls_before_held);
    ad_pokey_write(&chip, W_AUDCTL, 0x00);  /* channel 2 on the 64 kHz clock: AUDF2=0 -> 28 cycles */
    calls_before_held = th.irq_calls;
    ad_pokey_advance(&chip, 1000);
    CHECK(th.irq_calls == calls_before_held,
          "held reset: a timer on the slow clock stands still, got %d new call(s)",
          th.irq_calls - calls_before_held);
    ad_pokey_write(&chip, W_SKCTL, 7);     /* release: the full 28 cycles are still ahead */
    ad_pokey_advance(&chip, 27);
    CHECK(th.irq_calls == calls_before_held,
          "release: the slow-clock timer resumes where it stood, no IRQ after 27 cycles, got %d new call(s)",
          th.irq_calls - calls_before_held);
    ad_pokey_advance(&chip, 1);
    CHECK(th.irq_calls == calls_before_held + 1,
          "release: the slow-clock timer's borrow lands on the 28th cycle, got %d new call(s)",
          th.irq_calls - calls_before_held);

    /* --- keyboard and SKSTAT ---
     * SKSTAT reads every condition as a 0 and bit 0 as a 1, so an idle
     * chip reads 0xFF.  A keyboard overrun is the chip's rule: a new
     * code while the keyboard IRQ is still pending in IRQST - reading
     * KBCODE clears nothing, writing IRQEN does. */
    uint8_t st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK(st == 0xFF, "SKSTAT idle should read 0xFF (all conditions clear, bit 0 high), got %02X", st);
    ad_pokey_write(&chip, W_IRQEN, IRQ_KEYBD);
    th.irq_calls = 0; th.irq_mask = 0;
    ad_pokey_keyboard_key(&chip, 0x21, ST_SHIFT, true);
    CHECK(th.irq_calls == 1 && th.irq_mask == IRQ_KEYBD,
          "keyboard: key down with IRQEN=IRQ_KEYBD should raise 0x40, got %d call(s), mask %02X",
          th.irq_calls, th.irq_mask);
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK(st == (uint8_t)(0xFF & ~(ST_KEYBD | ST_SHIFT)),
          "keyboard: SKSTAT should read key-down and shift as zeros (0xF3), got %02X", st);
    CHECK(ad_pokey_read(&chip, R_KBCODE) == 0x21, "keyboard: KBCODE should latch 0x21, got %02X", chip.KBCODE);

    ad_pokey_keyboard_key(&chip, 0x22, 0, true);   /* IRQ still pending: overrun */
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK((st & ST_KBERR) == 0, "keyboard: a second code with the IRQ pending should read overrun (bit 5 low), got %02X", st);
    CHECK((st & ST_SHIFT) != 0, "keyboard: shift released should read bit 3 high, got %02X", st);
    CHECK(ad_pokey_read(&chip, R_KBCODE) == 0x22, "keyboard: KBCODE should hold the second code");

    ad_pokey_write(&chip, W_SKREST, 0);
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK((st & ST_KBERR) != 0, "keyboard: SKREST should clear the overrun latch, got %02X", st);

    ad_pokey_write(&chip, W_IRQEN, 0);             /* clears the pending keyboard IRQ */
    ad_pokey_write(&chip, W_IRQEN, IRQ_KEYBD);
    ad_pokey_keyboard_key(&chip, 0x23, 0, true);   /* no IRQ pending: no overrun */
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK((st & ST_KBERR) != 0, "keyboard: a code with no IRQ pending is not an overrun, got %02X", st);

    ad_pokey_keyboard_key(&chip, 0x23, 0, false);  /* key up */
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK((st & ST_KEYBD) != 0, "keyboard: key up should read bit 2 high, got %02X", st);

    ad_pokey_write(&chip, W_SKCTL, 0x05);          /* scanner off (bit 1 clear), chip not held */
    ad_pokey_keyboard_key(&chip, 0x33, 0, true);
    CHECK(ad_pokey_read(&chip, R_KBCODE) == 0x23 && (ad_pokey_read(&chip, R_SKSTAT) & ST_KEYBD) != 0,
          "keyboard: a key with the scanner disabled should be ignored");
    ad_pokey_write(&chip, W_SKCTL, 7);

    /* --- serial output ---
     * SKCTL bit 5 alone: both directions clock from timer 4.  Channels
     * 3+4 joined on the fast clock with AUDF3=$28, AUDF4=0 give a 47-
     * cycle borrow (the Atari's 19200 baud setting at 1.79 MHz): a bit
     * is 94 cycles, a frame 940.  The last AUDF write re-armed timer 4,
     * so its first borrow is 47 cycles after the SEROUT below. */
    ad_pokey_write(&chip, W_AUDCTL, CTL_CH34_JOIN | CTL_CH3_HICLK);
    ad_pokey_write(&chip, W_AUDF3, 0x28);
    ad_pokey_write(&chip, W_AUDF4, 0x00);
    ad_pokey_write(&chip, W_SKCTL, 0x27);
    CHECK(chip.divisor[3] == 47, "serial setup: channel 4's divisor should be 47, got %u", (unsigned)chip.divisor[3]);
    th.irq_calls = 0; th.irq_mask = 0; th.serout_calls = 0;
    ad_pokey_write(&chip, W_IRQEN, IRQ_SEROR | IRQ_SEROC | IRQ_SERIN);
    CHECK(th.irq_mask == IRQ_SEROC, "serial: enabling bit 3 with the transmitter idle asserts it at once, got mask %02X", th.irq_mask);
    CHECK((ad_pokey_read(&chip, R_IRQST) & IRQ_SEROC) == 0, "serial: IRQST bit 3 should read 0 while idle");

    ad_pokey_write(&chip, W_SEROUT, 0xA5);
    th.irq_mask = 0;
    CHECK((ad_pokey_read(&chip, R_IRQST) & IRQ_SEROC) != 0, "serial: IRQST bit 3 should read 1 once a byte is waiting");
    ad_pokey_advance(&chip, 46);
    CHECK(th.irq_mask == 0, "serial: nothing happens before the next borrow, got mask %02X", th.irq_mask);
    ad_pokey_advance(&chip, 1);                    /* the borrow: the shifter takes the byte */
    CHECK(th.irq_mask == IRQ_SEROR, "serial: the load should raise 'output data needed' (0x10), got mask %02X", th.irq_mask);
    CHECK((ad_pokey_read(&chip, R_IRQST) & IRQ_SEROR) == 0, "serial: IRQST bit 4 should read 0 after the load");
    CHECK(th.serout_calls == 0, "serial: the byte has not left the pin yet");
    th.irq_mask = 0;
    ad_pokey_advance(&chip, 20 * 47 - 1);
    CHECK(th.serout_calls == 0, "serial: one cycle short of twenty borrows the byte is still going out");
    ad_pokey_advance(&chip, 1);
    CHECK(th.serout_calls == 1 && th.serout_byte == 0xA5,
          "serial: serial_out(0xA5) at the twentieth borrow, got %d call(s), byte %02X", th.serout_calls, th.serout_byte);
    CHECK(th.irq_mask == IRQ_SEROC, "serial: going idle should raise 'transmission finished' (0x08), got mask %02X", th.irq_mask);
    CHECK((ad_pokey_read(&chip, R_IRQST) & IRQ_SEROC) == 0, "serial: IRQST bit 3 back to 0 when idle");

    /* Double buffering: a byte written during a frame loads right
     * after it, so a stream has no gap; a byte written over a waiting
     * byte replaces it. */
    ad_pokey_write(&chip, W_SEROUT, 0x11);
    ad_pokey_write(&chip, W_SEROUT, 0x3C);         /* replaces 0x11 before any borrow */
    ad_pokey_advance(&chip, 47);                   /* load 0x3C */
    ad_pokey_write(&chip, W_SEROUT, 0x5A);         /* waits in the data register */
    th.irq_mask = 0;
    ad_pokey_advance(&chip, 20 * 47);              /* 0x3C out */
    CHECK(th.serout_calls == 2 && th.serout_byte == 0x3C, "serial: 0x3C should be the second byte out, got %d call(s), %02X",
          th.serout_calls, th.serout_byte);
    CHECK((th.irq_mask & IRQ_SEROC) == 0 && (ad_pokey_read(&chip, R_IRQST) & IRQ_SEROC) != 0,
          "serial: with 0x5A waiting the transmitter is not finished");
    ad_pokey_advance(&chip, 47);                   /* load 0x5A */
    CHECK((th.irq_mask & IRQ_SEROR) != 0, "serial: loading the waiting byte raises 'output data needed' again");
    ad_pokey_advance(&chip, 20 * 47);
    CHECK(th.serout_calls == 3 && th.serout_byte == 0x5A, "serial: 0x5A should follow with no gap, got %d call(s), %02X",
          th.serout_calls, th.serout_byte);

    /* --- serial input ---
     * Asynchronous mode (bit 4): the start bit re-arms timer 4, so the
     * frame is exactly twenty borrows from the moment the receiver
     * takes it.  The host's byte is picked up by the next advance. */
    ad_pokey_write(&chip, W_SKCTL, 0x37);
    th.serial_byte_queued = 0x66;                  /* 0110 0110, LSB first: 0 1 1 0 0 1 1 0 */
    th.irq_mask = 0;
    ad_pokey_advance(&chip, 1);
    CHECK(th.serial_byte_queued == -1, "serial in: the idle receiver should take the host's byte");
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK((st & ST_SERIN_BUSY) == 0 && (st & ST_SERIN_DATA) == 0,
          "serial in: during the start bit SKSTAT reads busy (bit 1 low) and the line low (bit 4), got %02X", st);
    ad_pokey_advance(&chip, 94);                   /* data bit 0 of 0x66 = 0 */
    CHECK((ad_pokey_read(&chip, R_SKSTAT) & ST_SERIN_DATA) == 0, "serial in: data bit 0 of 0x66 is low on the line");
    ad_pokey_advance(&chip, 94);                   /* data bit 1 = 1 */
    CHECK((ad_pokey_read(&chip, R_SKSTAT) & ST_SERIN_DATA) != 0, "serial in: data bit 1 of 0x66 is high on the line");
    ad_pokey_advance(&chip, 16 * 47 - 1);
    CHECK(chip.SERIN != 0x66 && th.irq_mask == 0, "serial in: one cycle short of the stop bit nothing has landed");
    ad_pokey_advance(&chip, 1);
    CHECK(ad_pokey_read(&chip, R_SERIN) == 0x66, "serial in: SERIN should hold 0x66 at the stop bit, got %02X", chip.SERIN);
    CHECK(th.irq_mask == IRQ_SERIN, "serial in: the stop bit should raise the input IRQ (0x20), got mask %02X", th.irq_mask);
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK((st & (ST_SERIN_BUSY | ST_SERIN_DATA | ST_OVERRUN)) == (ST_SERIN_BUSY | ST_SERIN_DATA | ST_OVERRUN),
          "serial in: idle again, line at mark, no overrun, got %02X", st);

    /* Overrun: the next byte completes while the input IRQ is still
     * pending. */
    th.serial_byte_queued = 0x77;
    ad_pokey_advance(&chip, 1);
    ad_pokey_advance(&chip, 20 * 47);
    st = ad_pokey_read(&chip, R_SKSTAT);
    CHECK(ad_pokey_read(&chip, R_SERIN) == 0x77 && (st & ST_OVERRUN) == 0,
          "serial in: a byte landing on a pending IRQ should read overrun (bit 6 low), got SERIN %02X SKSTAT %02X",
          chip.SERIN, st);
    ad_pokey_write(&chip, W_SKREST, 0);
    CHECK((ad_pokey_read(&chip, R_SKSTAT) & ST_OVERRUN) != 0, "serial in: SKREST clears the overrun latch");

    /* ad_pokey_serial_receive(): a start bit from the host directly;
     * refused while a frame is in progress. */
    ad_pokey_serial_receive(&chip, 0x88);
    CHECK((ad_pokey_read(&chip, R_SKSTAT) & ST_SERIN_BUSY) == 0, "serial in: serial_receive on an idle receiver starts a frame");
    ad_pokey_serial_receive(&chip, 0x99);          /* dropped: busy */
    ad_pokey_advance(&chip, 20 * 47);
    CHECK(ad_pokey_read(&chip, R_SERIN) == 0x88, "serial in: the frame in progress wins, got %02X", chip.SERIN);

    /* External clock modes have no clock here: nothing moves. */
    ad_pokey_write(&chip, W_SKCTL, 0x07);
    th.serout_calls = 0;
    ad_pokey_serial_receive(&chip, 0xAA);
    ad_pokey_write(&chip, W_SEROUT, 0xBB);
    ad_pokey_advance(&chip, 3000);
    CHECK(th.serout_calls == 0 && ad_pokey_read(&chip, R_SERIN) == 0x88,
          "serial: on the external bit clock neither direction moves");
    ad_pokey_write(&chip, W_IRQEN, 0);

    /* --- poll: one keyboard code per call --- */
    th.kbd_code_queued = 0x55;
    ad_pokey_poll(&chip);
    CHECK(chip.KBCODE == 0x55 && (ad_pokey_read(&chip, R_SKSTAT) & ST_KEYBD) == 0,
          "poll: should pull the queued keyboard code through keyboard_scan, got KBCODE=%02X", chip.KBCODE);
    CHECK(th.kbd_code_queued == -1, "poll: should consume the queued code");

    /* --- pots --- */
    uint8_t pv = ad_pokey_read(&chip, R_POT0 + 3);
    CHECK(pv == 30, "pots: R_POT0+3 with the host should read pot_read(ctx,3) = 3*10 = 30, got %d", pv);

    ad_pokey_set_host(&chip, NULL);
    pv = ad_pokey_read(&chip, R_POT0 + 3);
    CHECK(pv == 0xFF, "pots: R_POT0+3 with no host should read 0xFF, got %02X", pv);
}

/* ------------------------------------------------------------------ */
/* (12) The RANDOM shift chain, clock by clock                          */
/* ------------------------------------------------------------------ */
/* A register-level reference for the chip's RANDOM chain, written from
 * poly_core.v of Nick Mikstas's atari_pokey (a gate-level transcription
 * of Atari's POKEY schematics) in that file's own terms - the
 * lfsr9bit/lfsr17bit shift registers, swDelay, the three NORs of the
 * 9/17 switch and their delayed copies, Init - stepped one clock at a
 * time with no shortcuts.  pokey.c's chain takes the same registers but
 * jumps n clocks at once through GF(2) matrices whenever it is running
 * steadily; this check is what says the jumps, the settling under Init
 * and the select flip's transition clocks all land on the same bits as
 * the plain clock-by-clock model, through the chip's API, for:
 *
 *   - a long run of advances of every size the hosts use (1..64, an NMI
 *     period, a whole frame, and larger), 17-bit and 9-bit;
 *   - holds of every length from 1 to 24 clocks from a running chain:
 *     reads during the hold and for 64 clocks after release must match,
 *     a release inside 17 clocks resumes from a mix of old and new bits
 *     (so all but a coincidence or two differ from the settled
 *     sequence), and one at or after 17 must equal it - the 29-cycle
 *     hold the NMI's "WASTE TIME" loop gives this board included;
 *   - AUDCTL's poly select flipped mid-run, both ways, read every clock
 *     across the transition.
 *
 * The reference's release-after-a-settled-hold sequence is also where
 * RESET_POS17 comes from: it is the rand17 index the reference's bytes
 * continue from, found by searching the table. */
typedef struct {
    int l9[8], l17[8], swDelay, norsDelayed[3];
} ref_chain;

static void ref_step(ref_chain *s, int Init, int sel9bitPoly)
{
    int feedback917 = !(s->l9[5] ^ s->l9[0]);
    int nors[3];
    nors[0] = !(s->l17[0] | sel9bitPoly);
    nors[1] = !(s->swDelay | !sel9bitPoly);
    nors[2] = !(!sel9bitPoly | feedback917);
    int swOut = !(Init | s->norsDelayed[0] | s->norsDelayed[1] | s->norsDelayed[2]);
    for (int i = 0; i < 7; i++) { s->l9[i] = s->l9[i + 1]; s->l17[i] = s->l17[i + 1]; }
    s->l9[7] = swOut;
    s->l17[7] = feedback917;
    s->swDelay = sel9bitPoly;
    for (int i = 0; i < 3; i++) s->norsDelayed[i] = nors[i];
}

static uint8_t ref_random(const ref_chain *s)      /* rndNum = ~lfsr9bit */
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint8_t)((!s->l9[i]) << i);
    return v;
}

/* The chip's power-on state is a settled 17-bit hold (pokey.c's
 * chain_reset()); start the reference the same way: from anything,
 * hold Init for 17 clocks. */
static void ref_reset(ref_chain *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 17; i++) ref_step(s, 1, 0);
}

static void ref_advance(ref_chain *s, uint32_t n, int Init, int sel9)
{
    while (n--) ref_step(s, Init, sel9);
}

static void check_chain(void)
{
    static const uint32_t sizes[] = { 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 64, 6048, 24192, 100000, 131071, 200000 };
    const int nsizes = (int)(sizeof sizes / sizeof sizes[0]);

    /* A settled hold sits at rand17[RESET_POS17] and continues from
     * there. */
    {
        const uint8_t *rand17 = ad_pokey_dbg_rand17();
        ref_chain r; ref_reset(&r);
        int pos = -1;
        for (uint32_t k = 0; k < 0x1FFFF && pos < 0; k++) {
            ref_chain t = r; uint32_t j;
            for (j = 0; j < 32; j++) {
                if (rand17[(k + j) % 0x1FFFF] != ref_random(&t)) break;
                ref_step(&t, 0, 0);
            }
            if (j == 32) pos = (int)k;
        }
        CHECK(pos == RESET_POS17, "the reference's settled hold should continue from rand17[%d], found %d",
              RESET_POS17, pos);
    }

    /* Running: every advance size, both poly selects. */
    for (int sel = 0; sel < 2; sel++) {
        ad_pokey chip; ref_chain r;
        ad_pokey_init(&chip, 1512000, 44100);
        ref_reset(&r);
        if (sel) ad_pokey_write(&chip, W_AUDCTL, CTL_POLY9);
        ref_advance(&r, 0, 1, sel);
        ad_pokey_write(&chip, W_SKCTL, 7);
        int bad = 0, total = 0;
        for (int pass = 0; pass < 6; pass++) {
            for (int i = 0; i < nsizes; i++) {
                uint32_t n = sizes[i] + (uint32_t)pass;
                ad_pokey_advance(&chip, n);
                ref_advance(&r, n, 0, sel);
                total++;
                if (ad_pokey_read(&chip, R_RANDOM) != ref_random(&r)) bad++;
            }
        }
        CHECK(bad == 0, "%s poly: %d of %d advances disagree with the clock-by-clock reference",
              sel ? "9-bit" : "17-bit", bad, total);
    }

    /* Holds of 1..24 clocks from a running chain, then 64 clocks free. */
    {
        uint8_t settled[64];
        {
            ad_pokey chip;
            ad_pokey_init(&chip, 1512000, 44100);
            ad_pokey_write(&chip, W_SKCTL, 7);
            for (int c = 0; c < 64; c++) { settled[c] = ad_pokey_read(&chip, R_RANDOM); ad_pokey_advance(&chip, 1); }
        }
        for (int sel = 0; sel < 2; sel++) {
            int bad_hold = 0, bad_short = 0, bad_long = 0;
            for (uint32_t h = 1; h <= 24; h++) {
                ad_pokey chip; ref_chain r;
                ad_pokey_init(&chip, 1512000, 44100);
                ref_reset(&r);
                if (sel) ad_pokey_write(&chip, W_AUDCTL, CTL_POLY9);
                ad_pokey_write(&chip, W_SKCTL, 7);
                ad_pokey_advance(&chip, 1000 + h * 97); ref_advance(&r, 1000 + h * 97, 0, sel);
                ad_pokey_write(&chip, W_SKCTL, 0);
                for (uint32_t c = 0; c < h; c++) {
                    ad_pokey_advance(&chip, 1); ref_step(&r, 1, sel);
                    if (ad_pokey_read(&chip, R_RANDOM) != ref_random(&r)) bad_hold++;
                }
                ad_pokey_write(&chip, W_SKCTL, 7);
                bool same_as_settled = true;
                for (int c = 0; c < 64; c++) {
                    uint8_t v = ad_pokey_read(&chip, R_RANDOM);
                    if (v != ref_random(&r)) bad_hold++;
                    if (sel == 0 && v != settled[c]) same_as_settled = false;
                    ad_pokey_advance(&chip, 1); ref_step(&r, 0, sel);
                }
                if (sel == 0) {
                    if (h < 17 && same_as_settled) bad_short++;
                    if (h >= 17 && !same_as_settled) bad_long++;
                }
            }
            CHECK(bad_hold == 0, "%s poly: %d reads across holds of 1..24 clocks disagree with the reference",
                  sel ? "9-bit" : "17-bit", bad_hold);
            if (sel == 0) {
                /* A release inside 17 clocks carries old bits, which can
                 * happen to equal the settled ones for the last clock or
                 * two (one bit left to fill); most of the sixteen must
                 * differ, and every release at 17 or later must not. */
                CHECK(bad_short <= 2, "%d of 16 releases inside 17 clocks continued the settled sequence; they should carry old bits",
                      bad_short);
                CHECK(bad_long == 0, "%d releases at 17 clocks or later differed from the settled sequence", bad_long);
            }
        }
        /* The board's own hold: ldx #4 / dex / bne between the two SKCTL
         * stores at $788F and $7899 - about 29 cycles.  A settled release. */
        {
            ad_pokey chip;
            ad_pokey_init(&chip, 1512000, 44100);
            ad_pokey_write(&chip, W_SKCTL, 7);
            ad_pokey_advance(&chip, 5000);
            ad_pokey_write(&chip, W_SKCTL, 0);
            ad_pokey_advance(&chip, 29);
            ad_pokey_write(&chip, W_SKCTL, 7);
            bool same = true;
            for (int c = 0; c < 64; c++) {
                if (ad_pokey_read(&chip, R_RANDOM) != settled[c]) same = false;
                ad_pokey_advance(&chip, 1);
            }
            CHECK(same, "the NMI's 29-cycle hold should release from the settled state");
        }
    }

    /* AUDCTL's poly select flipped mid-run, read every clock across it. */
    {
        ad_pokey chip; ref_chain r;
        ad_pokey_init(&chip, 1512000, 44100);
        ref_reset(&r);
        ad_pokey_write(&chip, W_SKCTL, 7);
        int bad = 0;
        int sel = 0;
        for (int flip = 0; flip < 8; flip++) {
            ad_pokey_advance(&chip, 777); ref_advance(&r, 777, 0, sel);
            sel ^= 1;
            ad_pokey_write(&chip, W_AUDCTL, sel ? CTL_POLY9 : 0);
            for (int c = 0; c < 40; c++) {
                ad_pokey_advance(&chip, 1); ref_step(&r, 0, sel);
                if (ad_pokey_read(&chip, R_RANDOM) != ref_random(&r)) bad++;
            }
            ad_pokey_advance(&chip, 3001); ref_advance(&r, 3001, 0, sel);
            if (ad_pokey_read(&chip, R_RANDOM) != ref_random(&r)) bad++;
        }
        CHECK(bad == 0, "poly select flips: %d reads disagree with the reference", bad);
    }
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
    check_chain();

    printf(fails ? "probe: %d failure(s)\n" : "probe: all checks passed\n", fails);
    return fails ? 2 : 0;
}
