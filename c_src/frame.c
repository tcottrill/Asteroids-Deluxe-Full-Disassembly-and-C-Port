/* Game initialisation and the per-frame housekeeping - CASTST ($7BF5),
 * the POKEY test in DSTTST.MAC, and START2_12's checksum slice.
 *
 * INIT is what a new game (or a new player) starts from; SINIT is the
 * half that also runs from the self-test.  PKYTST runs every frame and,
 * as a side effect the game depends on, sets the minimum rock speeds.
 */
#include "astdelux.h"

#define ZP  (g.zp.f)

/* INIT ($7BF5) - initialisation.
 *
 *     bit HALT / bmi INIT           wait for the VG to stop before
 *                                   monkeying around with FRAME
 *     jsr SETROL                    set up the rock subroutines (clears FRAME)
 *     lda #$B0 / sta $4003          a HALT instruction first
 *
 * then clears both scores, the whole current player page and HSSND,
 * seeds the delays, and falls into SINIT. */
void ad_init(void)
{
    while (ad_hw_vg_busy())                  /* WAIT FOR VG TO STOP */
        ;
    ad_setrol();                             /* SET UP ASTEROID SUBROUTINES */
    ad_vram_wr(0x4003, 0xB0);                /* PUT A HALT INSTRUCTION FIRST */

    /* INIT_30: `sta $61,x / sta $64,x` for X = 3, 2, 1 clears $62-$67,
     * the two three-byte scores. */
    for (int i = 0; i < 6; i++)
        ZP.SCORE[i] = 0;
    /* INIT_31: X is 0 on exit from the loop above; clear the whole page. */
    for (int i = 0; i < 0x100; i++)
        AD_P->raw[i] = 0;
    ZP.HSSND = 0;                            /* TURN OFF DEATH STAR SOUND */
    ad_ssbtlt(0x01);                         /* DON'T WAIT */
    AD_P->f.SEDLAY = 0x98;                   /* ENOUGH FOR 3 LARGE SAUCERS */
    AD_P->f.EDELAY = 0x98;                   /* DELAY ENTRY OF SAUCER */
    AD_P->f.RDELAY = 0x7F;                   /* DELAY FOR STARTING ROCKS */
    AD_P->f.DIFCTY = 0x06;                   /* IF MORE ROCKS THAN THIS THEN
                                              * DON'T LAUNCH SAUCER */
    ZP.UPDFLG[0] = 0xFF;                     /* NOT UPDATING INITIALS */
    ZP.UPDFLG[1] = 0xFF;
    AD_P->f.THUMP3 = 0x30;                   /* RESET STARTING THUMP SOUND */
    ad_sinit();
}

/* SINIT ($7C37) - the part the self-test shares.
 *
 * The bonus-life plateau comes from BONUS ($7C6E) indexed by OPTN1
 * bits 0-1: 00 20 50 FF, i.e. 0, 20 000, 50 000 or none.  The value is
 * the BCD thousands byte; it seeds the two players' plateau cells and
 * BONSCR+1, and the ten-thousands byte is 1 unless there is no bonus
 * (the table byte is negative), in which case the $FF propagates.
 * NHITS is OPTN3 bits 0-1 plus 2, plus one more when there is no bonus,
 * plus one more at two coins per credit.  Ends `jmp INISOU`. */
void ad_sinit(void)
{
    uint8_t a, x, y;

    ad_pkytst();                             /* INIT ROCK VELOCITIES */
    y = (uint8_t)(ad_hw_switch(0x2800) & 0x03);   /* OPTN1: bonus level */
    a = ad_rom((uint16_t)(AD_BONUS + y));    /* GET BONUS PLATEAU */
    ZP.BONSCR[1] = a;                        /* $FE */
    ZP.PLATEU[1] = a;                        /* $69  LSB */
    ZP.PLATEU[4] = a;                        /* $6C */
    if (!(a & 0x80))                         /* bmi SINIT_32: NO BONUS */
        a = 0x01;
    ZP.PLATEU[2] = a;                        /* $6A */
    ZP.BONSCR[2] = a;                        /* $FF */
    ZP.PLATEU[5] = a;                        /* $6D */
    x = (uint8_t)(0x03 & ad_hw_switch(0x2802));   /* OPTN3: GET LIFE COUNT */
    x += 2;                                  /* +2 */
    if (y == 0x03)                           /* BONUS ENABLED? NO. */
        x++;                                 /* GIVE 'EM ANOTHER LIFE */
    /* SINIT_2 */
    if ((ZP.CMODE & 0x03) == 0x03)           /* 2 COINS/CREDIT? */
        x++;                                 /* GIVE 'EM 1 MORE */
    /* SINIT_1 */
    ZP.NHITS = x;
    ad_inisou();                             /* SET UP POKEY */
}

/* PKYTST ($7FC1) - POKEY test, and the rock-speed initialiser.
 *
 * One RANDOM sample per frame goes into a four-entry history at
 * $DC-$DF (PERR+1..PERR+4), indexed by FRAME & 3.  The sample is then
 * compared against all four cells - including the one it was just put
 * in, "WILL ALWAYS GET 1 ERROR" - and four matches means the POKEY is
 * dead:
 *
 *     cpy #$04                      C = (matches >= 4)
 *     ldy #$0A / ldx #$F6           minimum velocity, normal
 *     lda OPTN5 / ror a             C into bit 7, OPTN5 bit 0 out
 *     beq PKYTST_3
 *     ldx #$FB / ldy #$05           MAKE THE GAME EASY
 *
 * so the easy speeds are chosen when OPTN5 has any bit above bit 0
 * set, or when the POKEY reads the same value four frames running.
 * The rotated value is kept in PERR as the error code. */
void ad_pkytst(void)
{
    uint8_t a, x, y;
    bool c;

    x = (uint8_t)(ZP.FRAME[0] & 0x03);       /* ISOLATE LSB'S: INDEX */
    a = ad_hw_random();                      /* GET POKY'S RANDOM # */
    ZP.PERR[1 + x] = a;                      /* sta $DC,x: SAVE IT */
    y = 0;                                   /* CLEAR ERROR COUNTER */
    for (x = 4; x != 0; x--) {               /* PKYTST_2 */
        if (a == ZP.PERR[x])                 /* cmp PERR,x: SAME? */
            y++;                             /* COUNT IT */
    }
    c = (y >= 0x04);                         /* cpy #$04: SET CARRY */
    y = 0x0A;                                /* MINIMUM VELOCITY NORMAL */
    x = 0xF6;
    a = ad_hw_pokey_read(0x08);              /* OPTN5: S/B 0 */
    a = (uint8_t)((a >> 1) | (c ? 0x80 : 0));   /* ror a */
    if (a != 0) {
        x = 0xFB;                            /* MAKE THE GAME EASY */
        y = 0x05;
    }
    /* PKYTST_3 */
    ZP.RVELM = x;                            /* + */
    ZP.RVELP = y;                            /* - */
    ZP.PERR[0] = a;                          /* ERROR CODE */
}

/* START2_12 ($604B) - one slice of the rolling ROM checksum, run by the
 * frame loop once every 256 frames (after `inc FRAME` wraps; the
 * caller in mainline.c does the FRAME increments).
 *
 * PROT ($8A-$8D) is the walking address (low, high), the "signal"
 * byte $8C, and the running sum $8D.  Each slice adds one byte from
 * each of three ROM ranges, chained through the carry:
 *
 *     lda $8B / ora #$50 / sta R1           $50xx
 *     eor #$38 / tax                        $68xx, kept in X
 *     eor #$18 / sta R3                     $70xx
 *     lda PROT / sta R0 / sta R2            same low byte for all three
 *     lda $8D / clc
 *     adc (R0),y                            CS = CS + ($5000+N)
 *     stx R1 / adc (R0),y                   CS = CS + ($6800+N)
 *     adc (R2),y                            CS = CS + ($7000+N)
 *     adc #$00                              CS = CS + CARRY
 *
 * After the eighth page (X = $6F) the sum is folded: `eor #$03 / sta
 * $8C` - "SIGNAL ERROR OR NOT (S/B 0)" - so on an unmodified ROM the
 * three ranges sum to 3, which is what CKSUM.MAC's patch bytes arrange.
 * EFIRE and COINC read $8C as the tamper flag.  The port carries the
 * ROM bytes, so the sum comes out as on the real board and $8C stays 0
 * without special-casing.  The same pass also does `ldx #$FC / txs`,
 * moving the stack down, and `stx HOLE` - HOLE = $FC is the offset of
 * the "hole" that COLIDE and the copyright check use on page 1.  The
 * stack move itself has no meaning here; HOLE is kept. */
void ad_protck(void)
{
    uint8_t r1 = (uint8_t)(ZP.PROT[1] | 0x50);   /* MSB OF ADDRESS: 5000-5800 */
    uint8_t x  = (uint8_t)(r1 ^ 0x38);           /* 6800-7000 TOO */
    uint8_t r3 = (uint8_t)(x ^ 0x18);            /* 7000-7800 */
    uint8_t lo = ZP.PROT[0];                     /* GET LOW ADDRESS */
    unsigned s;

    ZP.R1 = r1;
    ZP.R3 = r3;
    ZP.R0 = lo;                                  /* PUT CHECKSUM ADDRESS IN R0,R1 */
    ZP.R2 = lo;                                  /* ALSO IN R2,R3 */

    s = ZP.PROT[3];                              /* GET ACCUM, clc */
    s += ad_rom((uint16_t)((r1 << 8) | lo));     /* CS=CS+(5000+N) */
    ZP.R1 = x;                                   /* stx R1 */
    s = (s & 0xFF) + ad_rom((uint16_t)((x << 8) | lo)) + (s >> 8);   /* CS=CS+(6800+N) */
    s = (s & 0xFF) + ad_rom((uint16_t)((r3 << 8) | lo)) + (s >> 8);  /* CS=CS+(7000+N) */
    s = (s & 0xFF) + (s >> 8);                   /* CS=CS+CARRY */

    if (++ZP.PROT[0] == 0) {                     /* NEXT ADDRESS; overflow? */
        ZP.PROT[1]++;                            /* HIGH ADDRESS */
        if (x == 0x6F) {                         /* END? */
            ZP.PROT[1] = 0;                      /* sty $8B: 0 -> HIGH ADDRESS */
            s = (s & 0xFF) ^ 0x03;               /* SEED */
            ZP.PROT[2] = (uint8_t)s;             /* SIGNAL ERROR OR NOT (S/B 0) */
            /* ldx #$FC / txs: the stack move, no equivalent here */
            ZP.HOLE = 0xFC;                      /* PUT A HOLE ON THE STACK */
            s = 0;                               /* tya: 0 -> ACC */
        }
    }
    /* START2_11 */
    ZP.PROT[3] = (uint8_t)s;                     /* KEEP ACCUM */
}
