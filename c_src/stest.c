/* Cabinet self-test - DSTTST.MAC, $7CD7-$7FBE.
 *
 * Not translated, by decision: PWRON's RAM test (a C port has nothing
 * to test), STEST1's bad-RAM beep on the 3 kHz clock, STOP's spin
 * until the self-test switch is pushed, the 3 kHz timing loops that
 * pace the beep and the 16 ms display cadence, SWCH_5's delay loop, and
 * the watchdog strobes in the display loop - the host paces frames and
 * switch reads itself (rule 7/9).  PKYTST already lives in frame.c (it
 * runs every frame, self-test or not); VGBLNK and VGADD2 are in
 * vgutil.c, since every module that builds a display list can reach
 * them.
 *
 * What's here: STEST3 (the ROM checksum and bank-select test, run once
 * from PWRON) and the display loop STEST5/STEST6/STEST7/SWCH, run once
 * per ~16 ms tick for as long as the self-test switch stays on.
 */
#include "astdelux.h"

#define ZP  (g.zp.f)

/* 6502 cycles from POTGO ($7ED0) to PKYTST's OPTN5 read, see the note
 * at the ad_hw_cycles() call in ad_stest_frame(). */
#define AD_STEST6_DIGITS_CYCLES 1100

/* ------------------------------------------------------------------ */
/* STEST3 ($7DF3) - ROM checksum and bank-select test                   */
/* ------------------------------------------------------------------ */
/* Six 2K blocks, each summed with an 8-bit carry chained through all
 * 2048 additions (STEST3_10's `adc ($09),y`, run 8 times for the 8
 * pages of a block).  The six starting addresses below are what
 * STEST3_5/STEST3_20's compare chain walks: $4800-$57FF in two blocks,
 * then (`cmp #$58 / bne / lda #$60`) $6000-$7FFF in four, skipping the
 * unpopulated $5800-$5FFF.  Each sum starts at $55; the result goes to
 * R0..R5 and is OR'd into CKERR.  $09/$0A (TEMP1) and TEMP2, the
 * pointer and page counter, are pure scratch here - nothing outside
 * this routine reads them - so they stay local (rule 12).
 *
 * The bank-select test follows: $0300 (whichever physical page BNKSEL
 * is NOT currently showing at $0200) is set to $80; BNKSEL then flips
 * pages, and OBJ ($0200, now the OTHER physical page) must show that
 * $80, while the page now at $0300 must be the untouched, still-zero
 * page from PWRON's clear.  Either mismatch bumps XT. */
void ad_stest3(void)
{
    static const uint16_t bases[6] =
        { 0x4800, 0x5000, 0x6000, 0x6800, 0x7000, 0x7800 };

    for (int i = 0; i < 6; i++) {
        uint8_t sum = 0x55;                  /* STARTING PATTERN */
        bool carry = false;                  /* clc */

        for (int page = 0; page < 8; page++) {       /* 8 PAGES TO TEST */
            for (int y = 0; y < 256; y++) {           /* DO ONE PAGE */
                unsigned t = sum + ad_rom((uint16_t)(bases[i] + (page << 8) + y))
                             + (carry ? 1u : 0u);
                carry = t > 0xFF;
                sum = (uint8_t)t;
            }
        }
        g.zp.raw[0x10 + i] = sum;            /* STORE 6 CKSUMS STARTING AT R0 */
        ZP.CKERR |= sum;                      /* KEEP ERROR FLAGS */
        ad_hw_watchdog();                     /* WHAT HAVE YOU BEEN TYPING ON THESE, LINDA? */
    }

    /* 200=0 AND 300=80 */
    AD_P3->f.OBJ[0] = 0x80;                  /* sta $0300 */
    ad_bnksel(0x80);                          /* SWITCH TO PLAYER 2 */
    if (AD_P->f.OBJ[0] != 0x80)               /* BANK SELECT APPEARS OK? */
        ZP.XT++;                              /* ERROR */
    if (AD_P3->f.OBJ[0] != 0)
        ZP.XT++;                              /* ERROR */
}

/* ------------------------------------------------------------------ */
/* SWCH ($7F94-$7FBE) - read all the switches                          */
/* ------------------------------------------------------------------ */

/* SWCH_4 ($7FB4) - fold one reading against the last one seen, at the
 * next of four (R6..R9, ASTERS[0..3]) slots. */
static uint8_t swch4(uint8_t a, uint8_t *y)
{
    uint8_t x = a;                            /* tax: SAVE SWITCHES FOR A SEC */
    a = (uint8_t)(a ^ g.zp.raw[0x16 + *y]);    /* eor R6,y: COMPUTE DIFFERENCES */
    g.zp.raw[0x86 + *y] = a;                   /* sta ASTERS,y: SAVE DIFFERENCES */
    g.zp.raw[0x16 + *y] = x;                   /* stx R6,y: SAVE NEW READINGS */
    (*y)++;                                    /* iny: UP TO NEXT VARIABLES */
    return a;
}

/* SWCH ($7F94) - a recursion trick: `jsr SWCH_1` is immediately followed
 * by the SWCH_1 label, so when the call eventually RTS's (out of
 * SWCH_4) it lands right back at its own start and runs the whole body
 * a second time; only the SECOND time's RTS returns to the true caller.
 * So the body below runs twice, and Y (indexing R6../ASTERS[]) counts
 * 0,1,2,3 across the two runs, two SWCH_4 folds per run. */
static uint8_t swch(void)
{
    uint8_t y = 0;
    uint8_t a = 0;

    for (int run = 0; run < 2; run++) {
        /* SWCH_2: 8 switches, X = 7..0, rol'd from the port then ror'd
         * into A.  The read-modify-write on the input port is just a
         * read (rule 8); after 8 full rors A is entirely the new
         * bits - the FIRST read ($2407, ROTL) ends in bit 0, the LAST
         * ($2400) in bit 7 - so what A held on entry cannot matter,
         * and resetting it here is harmless. */
        a = 0;
        for (int x = 7; x >= 0; x--) {
            bool bit7 = (ad_hw_switch((uint16_t)(0x2400 + x)) & 0x80) != 0;
            a = (uint8_t)((a >> 1) | (bit7 ? 0x80 : 0));
        }
        a = swch4(a, &y);

        /* SWCH_3: 5 switches, X = 4..0, on $2003,x - NOTE A is not
         * cleared first: only 5 rors happen, so bits 7..3 end up the
         * five switches and bits 2..0 the top three bits of the
         * SWCH_4 result just folded above.  That is what the ROM
         * does; kept as-is. */
        for (int x = 4; x >= 0; x--) {
            bool bit7 = (ad_hw_switch((uint16_t)(0x2003 + x)) & 0x80) != 0;
            a = (uint8_t)((a >> 1) | (bit7 ? 0x80 : 0));
        }
        /* stx BNKSEL: X is $FF here (the loop above ends with X
         * wrapped past 0 by the final dex), switching to player 2
         * controls for the second run.  The port's ad_hw_switch is
         * not bank-aware, so this second pass reads the same
         * switches as the first. */
        ad_bnksel(0xFF);
        /* SWCH_5's delay loop is dropped: pure timing, no state. */
        a = swch4(a, &y);
    }
    return a;                                 /* the second run's second fold */
}

/* ------------------------------------------------------------------ */
/* BONDSP ($7C93) - bonus-life score requirement                        */
/* ------------------------------------------------------------------ */
/* Not yet in the port; STEST6 is its only caller.  Three BCD bytes at
 * BONSCR ($FD), leading zeros suppressed. */
void ad_bondsp(void)
{
    ad_digits(true, 0xFD, 0x03);
}

/* ------------------------------------------------------------------ */
/* STEST5/STEST6/STEST7 ($7E39-$7F91) - the display loop, one pass       */
/* ------------------------------------------------------------------ */
/* Called once per ~16 ms tick while the self-test switch is held.
 * Returns false when STEST6 finds the switch released (leave self-test
 * and start the game), true after drawing one test frame (call again). */
bool ad_stest_frame(void)
{
    /* STEST5 ($7E39) */
    ZP.VGSIZE = 0x10;                        /* SET CHARACTER SIZE */
    ad_bnksel(0x10);                         /* SWITCH TO PLAYER 1 CONTROLS */
    /* STEST5_5..STEST5_8: the 3kHz wait (~16ms) and the HALT wait are
     * the host's pacing job (rule 7); the watchdog kick is dropped
     * (rule 9). */
    while (ad_hw_vg_busy())
        ;
    ad_set_vglist(0x4000);                   /* self-test builds its list
                                              * directly at $4000, no
                                              * double buffering */

    /* STEST6 ($7E5F) */
    if (!(ad_hw_switch(0x2007) & 0x80)) {    /* S.T. SWITCH */
        /* sta $01FF: ENABLE NMI'S - the port's NMI is always enabled */
        return false;                        /* jmp START: BEGIN GAME */
    }

    if (ZP.XT != 0)
        ad_vgjsrl(0x57CC);                   /* JSRL BNKERR FOR BANK SELECT ERROR */

    /* STEST6_30/32/35: a checksum letter for each nonzero R0..R5. */
    ZP.TEMP2[1] = 0x96;                       /* STARTING Y VALUE FOR CHECKSUMS */
    for (int x = 5; x >= 0; x--) {
        if (g.zp.raw[0x10 + x] != 0) {        /* R0,x */
            ZP.TEMP2[1] = (uint8_t)(ZP.TEMP2[1] - 8);   /* 32 BELOW CURRENT LINE */
            ad_vgawt(0x20, ZP.TEMP2[1]);
            ad_vgchar(ad_rom((uint16_t)(AD_ROMX + x)));   /* GET X ADDRESS OF ROM */
            ad_vgchar(ad_rom((uint16_t)(AD_ROMY + x)));   /* GET Y ADDRESS OF ROM */
        }
    }

    ad_vgjsrl(0x5744);                       /* JSRL TO TEST PATTERN */
    ad_vgawt(0x93, 0xA0);                    /* POSITION BEAM */

    for (int x = 3; x >= 0; x--) {            /* GO FROM SW1 TO SW8 */
        uint8_t a = (uint8_t)(ad_hw_switch((uint16_t)(0x2800 + x)) & 0x03);
        ad_vgblnk(a);
    }

    ad_vgawt(0x93, 0xB0);                    /* POSITION */
    ad_hw_pokey_write(0x0F, 7);              /* RELEASE POKEY */
    ad_hw_pokey_write(0x0B, 7);

    {
        uint8_t a = (uint8_t)(ad_hw_pokey_read(0x08) ^ 0xFF);  /* GET COIN OPTIONS */
        bool c;

        ZP.TEMP2[0] = a;
        ZP.CMODE = a;

        c = (a & 0x80) != 0;                 /* asl a */
        a = (uint8_t)(a << 1);
        for (int i = 0; i < 3; i++) {         /* rol a (x3) */
            bool nc = (a & 0x80) != 0;
            a = (uint8_t)((a << 1) | (c ? 1 : 0));
            c = nc;
        }
        ZP.TEMP2[1] = (uint8_t)((ZP.TEMP2[1] << 1) | (c ? 1 : 0));  /* rol $0D */

        ad_vgblnk((uint8_t)(a & 0x07));                /* ISOLATE BONUS OPTIONS */
        ad_vgblnk((uint8_t)(ZP.TEMP2[1] & 0x01));       /* ISOLATE MIDDLE MECH MULTIPLIER */
        ad_vgblnk((uint8_t)((ZP.TEMP2[0] >> 2) & 0x03)); /* ISOLATE RIGHT MECH MULTIPLIER */
        ad_vgblnk((uint8_t)(ZP.TEMP2[0] & 0x03));        /* ISOLATE COIN MODE */
    }

    /* Machine time between POTGO ($7ED0) and PKYTST's OPTN5 read
     * ($7FDD, "S/B 0") inside SINIT: the ROM depends on the fast pot
     * scan (228 cycles) having finished by then, and on the board it
     * has - the four VGBLNK calls above run VGHEX/VGCHAR/VGADD2 each,
     * roughly 230 cycles apiece, plus about 200 of straight-line code
     * and PKYTST's own preamble, some 1,100 cycles in all.  The host
     * has no 6502 to count them, so they are declared here (astdelux.h,
     * ad_hw_cycles).  Only "more than 228" matters to the ROM; the
     * figure is the listing's, not a tuning knob. */
    ad_hw_cycles(AD_STEST6_DIGITS_CYCLES);

    ad_sinit();
    ad_livess(0x96, 0x94, (uint8_t)(ZP.NHITS + 1));   /* DISPLAY THEM */
    ZP.VGSIZE = 0x10;
    if (!(ZP.BONSCR[2] & 0x80)) {             /* BONUS? */
        ad_vgawt(0x8E, 0x83);
        ad_bondsp();                          /* DISPLAY BONUS SCORE */
    }

    if (ZP.PERR[0] != 0)                      /* ANY POKEY ERRORS? */
        ad_vgadd2(0xF4, 0xCB);                /* JSRL PKYERR */

    ZP.FRAME[0]++;                            /* inc FRAME: no carry into FRAME+1 */

    {
        uint8_t a = (uint8_t)(ad_hw_switch(0x2004)      /* FIRESW */
                               & ad_hw_switch(0x2405)    /* THRUST */
                               & ad_hw_switch(0x2407)    /* ROTL */
                               & ad_hw_switch(0x2406));  /* ROTR */
        if ((a & 0x80) && !(ZP.LPLAYR & 0x80)) {   /* not ALREADY ERASED */
            ZP.EAFLG = a;                      /* SIGNAL TO ERASE */
            ZP.LPLAYR = a;                     /* ONLY DO THIS ONCE */
            ZP.EABC = 0x15;                    /* 21 BYTES */
            ZP.EAX = 0x14;                     /* START @ 20 */
        }
    }

    /* STEST6_11 */
    if (ZP.LPLAYR != 0 && ZP.EAFLG != 0) {    /* ANY EA STUFF? ERASING OR WRITING? */
        ad_vgawt(0x94, 0x72);
        ad_vgjsrl(0x57F2);                    /* SAY WE'RE ERASING */
        ad_eaupd();                           /* DO EA STUFF */
        /* NMI'S DON'T WORK IN SELF TEST, SO FOOL EAUPD ROUTINE BY
         * SAYING 4 INTERRUPS HAPPENED */
        ZP.INTCT = (uint8_t)(ZP.INTCT + 4);
    }

    /* STEST6_29 */
    ad_vgsabs(0x7F, 0x7F);                    /* CENTER BEAM */
    ad_vghalt();                              /* STOP BEAM */

    /* STEST7 ($7F70) */
    for (int r = 7; r >= 0; r--)
        ad_hw_pokey_write((uint8_t)r, 0);     /* CLEAR POKEY REGISTERS */

    {
        uint8_t a = swch();                   /* GET PLAYER SWITCHES */
        a = (uint8_t)(a | ZP.ASTERS[2]);       /* $88 */
        a = (uint8_t)(a | ZP.ASTERS[1]);       /* $87 */
        a = (uint8_t)(a | ZP.ASTERS[0]);       /* ASTERS.  The fourth byte,
                                                * $89, is already in A: it is
                                                * what SWCH's last fold left */
        if (a != 0)
            a = 0xA4;                          /* TURN ON SOUND */
        ad_hw_pokey_write(1, a);               /* VOLUME SELECT */
        a = (uint8_t)(a >> 1);                 /* CLEAR BIT 7 */
        ad_hw_pokey_write(0, a);               /* FREQUENCY SELECT */
    }
    ad_hw_vg_go();                             /* START DRAWING NOW */
    return true;                               /* jmp STEST5: caller loops */
}
