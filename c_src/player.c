/* The player's ship - DSTRD0.MAC: SHIELD, FIRE and MOVE.
 *
 * The three control handlers the main line calls every game frame, in
 * that order, plus the pieces they share with the rest of the ROM:
 * FIRE1/FIRE3 (EFIRE launches the saucer's torpedoes through them) and
 * MOVE2 (BNC clamps a bounced velocity with it).
 *
 * The switches are read exactly as the listing does.  `asl FIRESW /
 * ror LASTSW` is a read-modify-write on a read-only port: the write is
 * discarded and the point is to get bit 7 into carry, then into the
 * debounce byte.  Each read below is one ad_hw_switch() call at the
 * port's own address (disasm/astdelux_defines.asm), so the sites read
 * like the listing and the host sees every access the ROM makes.
 *
 * Velocities are 16-bit S9999.BBB, split as the memory model has them:
 * SHPXI/SHPYI (the XINC/YINC slot for the ship) are the high bytes and
 * XINCL/YINCL in page zero the low bytes.  MOVE1 assembles the pair in
 * A:X for MOVE2, and that packing is kept in the uint16_t: high byte
 * in bits 15-8, low in 7-0.
 */
#include "astdelux.h"

/* The control ports, as astdelux_defines.asm names them.  D7 = 1 for
 * on; the rest of the byte is whatever else the decoder puts on the
 * bus, which is why every test below looks only at bit 7. */
#define AD_HYPSW   0x2003   /* HYPERSPACE SWITCH - the shields button */
#define AD_FIRESW  0x2004   /* FIRE BUTTON */
#define AD_THRUST  0x2405   /* THRUST */
#define AD_ROTR    0x2406   /* ROTATE RIGHT */
#define AD_ROTL    0x2407   /* ROTATE LEFT */

/* The ship's own cells, by their slot in the parallel arrays: SHPPIX is
 * OBJ+$19, SHPXI is XINC+$19 and so on.  Indexing the raw page keeps
 * `sta SHPXI,y` (torpedo Y's velocity) and `adc SHPXL,x` (X = 0 ship,
 * 1 saucer) as the ROM's own arithmetic. */
#define SHP(base, i)  (AD_P->raw[(base) + AD_SHIP + (i)])

/* ------------------------------------------------------------------ */
/* SHIELD ($6675) - the shields button                                 */
/* ------------------------------------------------------------------ */
/* SHDON bit 7 is "shields up" for FIRE, CACCEL, COLIDE and SHLDPX.
 * The flag is a shift register, not a boolean: `lsr SHDON` assumes
 * off, then `asl HYPSW / ror SHDON` rotates the button in at bit 7 -
 * so the byte is shifted twice per frame, whatever the exits below do.
 * Shield power (SHLDS) drains one unit every fourth frame while held.
 */
void ad_shield(void)
{
    g.zp.f.SHDON >>= 1;                             /* ASSUME SHIELDS OFF */
    if (g.zp.f.NPLAYR == 0)
        return;                                     /* ATTRACT */
    if (AD_P->f.SHPPIX & 0x80)
        return;                                     /* EXPLODING */
    if (AD_P->f.SHPPIX == 0)
        return;                                     /* NOT VISIBLE */
    if (AD_P->f.SHLDS == 0)
        return;                                     /* NO SHIELD POWER LEFT */

    /* asl HYPSW / ror SHDON: carry = D7 of the port, into bit 7. */
    {
        uint8_t sw = ad_hw_switch(AD_HYPSW);
        g.zp.f.SHDON = (uint8_t)((g.zp.f.SHDON >> 1) | (sw & 0x80));
    }
    if (!(g.zp.f.SHDON & 0x80))
        return;                                     /* SHIELDS NOT ON */

    ad_sndpon(0x57);                                /* SHIELDS */
    if (g.zp.f.FRAME[0] & 0x03)
        return;
    AD_P->f.SHLDS--;                    /* TAKE SHIELD POWER EVERY 4 FRAMES */
}

/* ------------------------------------------------------------------ */
/* FIRE ($64BE) - the fire button, then into FIRE1                     */
/* ------------------------------------------------------------------ */
/* Debounce: LASTSW is a shift register of the button, newest reading
 * in bit 7.
 *
 *     asl FIRESW / ror LASTSW        carry = D7 now, rotate it in
 *     bit LASTSW                     N = this frame, V = last frame
 *     bpl FIRE2                      button not on
 *     bvs FIRE2                      button not different from last time
 *
 * A fresh press then sets up FIRE1's contract for the ship: R8 = 0
 * (position from SHPXL), TEMP3+1 = 3 (stop before the saucer's slots),
 * X = $19 (inherit the ship's velocity), ANGLE+1 = ANGLE, and Y = 7,
 * the last of the four ship torpedoes.
 */
void ad_fire(void)
{
    if (g.zp.f.NPLAYR == 0)
        return;                                     /* IN ATTRACT */
    if (g.zp.f.SHDON & 0x80)
        return;                                     /* SHIELDS?  YEP. CAN'T FIRE */

    {
        uint8_t sw = ad_hw_switch(AD_FIRESW);       /* FULLY DECODED FOR R/W */
        g.zp.f.LASTSW = (uint8_t)((g.zp.f.LASTSW >> 1) | (sw & 0x80));
    }
    if (!(g.zp.f.LASTSW & 0x80))
        return;                                     /* BUTTON NOT ON */
    if (g.zp.f.LASTSW & 0x40)
        return;                                     /* BUTTON NOT DIFFERENT FROM LAST TIME */
    if (AD_P->f.SDELAY != 0)
        return;                                     /* SHIP NOT VISIBLE YET */

    g.zp.f.R8 = 0;                                  /* sta R8: A is SDELAY = 0 */
    g.zp.f.TEMP3[1] = 0x03;                         /* STOPPING INDEX FOR SHIP */
    g.zp.f.ANGLE[1] = g.zp.f.ANGLE[0];              /* INDICATE DIRECTION */
    ad_fire1(0x07, AD_SHIP);                        /* NUMBER OF TORPEDOS ALLOWED */
}

/* ------------------------------------------------------------------ */
/* FIRE3 ($64EF) - launch a torpedo into slot Y                        */
/* ------------------------------------------------------------------ */
/* Y = the torpedo slot (relative to SHPPIX), X = the object whose
 * velocity the torpedo inherits, R8 = 0/1 for ship/saucer as the
 * launch position, ANGLE+1 = the firing angle.
 *
 * For the saucer, EFIRE passes X as the *target's* slot, so its
 * torpedoes carry the target's velocity, not the saucer's - a crude
 * lead.  That is the ROM (CALLING_NOTES.md section 16, item 12) and is
 * kept.
 *
 * Each axis is: half the direction component (`cmp #$80 / ror a` is an
 * arithmetic shift right) plus the inherited velocity, clamped to
 * $91..$6F.  Then the muzzle offset: the half-component times 3/2,
 * sign-extended from the *half* component (TEMP1/TEMP2 hold $00 or
 * $FF), added to the launcher's 16-bit position.  Note the `clc`
 * between the two adds - the carry out of the 3/2 sum is thrown away,
 * only the carry out of the position add reaches the high byte.
 */
static void fire3(uint8_t y, uint8_t x)
{
    uint8_t a;
    uint8_t ext;

    g.zp.f.TEMP3[0] = x;
    SHP(AD_OBJ, y) = 0x12;                          /* SET TIMER FOR LENGTH OF LIFE */

    /* COS(ANGLE)/2 + XINC,x -> SHPXI,y */
    a = ad_cos(g.zp.f.ANGLE[1]);
    x = g.zp.f.TEMP3[0];
    a = (uint8_t)((a >> 1) | (a & 0x80));           /* cmp #$80 / ror a: DIVIDE BY 2 */
    g.zp.f.TEMP1[1] = a;                            /* $0A */
    a = (uint8_t)(a + AD_P->raw[AD_XINC + x]);      /* ADD IN VELOCITY OF OBJECT */
    if (a & 0x80) {                                 /* IF NEGATIVE */
        if (a < 0x91)                               /* IF MIN EXCEEDED */
            a = 0x91;
    } else if (a >= 0x70) {                         /* IF MAX EXCEEDED */
        a = 0x6F;
    }
    SHP(AD_XINC, y) = a;                            /* SET X SPEED */

    /* SIN(ANGLE)/2 + YINC,x -> SHPYI,y */
    a = ad_sin(g.zp.f.ANGLE[1]);
    x = g.zp.f.TEMP3[0];
    a = (uint8_t)((a >> 1) | (a & 0x80));           /* DIVIDE BY 2 */
    g.zp.f.TEMP2[1] = a;                            /* $0D */
    a = (uint8_t)(a + AD_P->raw[AD_YINC + x]);      /* ADD IN VELOCITY OF OBJ */
    if (a & 0x80) {
        if (a < 0x91)
            a = 0x91;
    } else if (a >= 0x70) {
        a = 0x6F;
    }
    SHP(AD_YINC, y) = a;

    /* X position: $0A * 3/2 + SHPXL,R8 -> SHPXL,y (16-bit) */
    a = g.zp.f.TEMP1[1];                            /* SCALE TO PUT TORP AT NOSE OF SHIP */
    ext = (a & 0x80) ? 0xFF : 0x00;                 /* SIGN EXTENSION */
    g.zp.f.TEMP1[0] = ext;
    x = g.zp.f.R8;                                  /* SHIP/SAUCER INDEX */
    a = (uint8_t)((a >> 1) | (a & 0x80));           /* cmp #$80 / ror a */
    a = (uint8_t)(a + g.zp.f.TEMP1[1]);             /* MULTIPLY BY 3/2 (clc: carry dropped) */
    {
        unsigned sum = (unsigned)a + SHP(AD_OBJXL, x);      /* clc / adc SHPXL,x */
        SHP(AD_OBJXL, y) = (uint8_t)sum;
        SHP(AD_OBJXH, y) = (uint8_t)(g.zp.f.TEMP1[0] + SHP(AD_OBJXH, x) + (sum >> 8));
    }

    /* Y position: $0D * 3/2 + SHPYL,R8 -> SHPYL,y */
    a = g.zp.f.TEMP2[1];                            /* SCALE AGAIN TO PUT AT NOSE */
    ext = (a & 0x80) ? 0xFF : 0x00;
    g.zp.f.TEMP2[0] = ext;
    x = g.zp.f.R8;
    a = (uint8_t)((a >> 1) | (a & 0x80));
    a = (uint8_t)(a + g.zp.f.TEMP2[1]);             /* MULTIPLY BY 3/2 */
    {
        unsigned sum = (unsigned)a + SHP(AD_OBJYL, x);
        SHP(AD_OBJYL, y) = (uint8_t)sum;
        SHP(AD_OBJYH, y) = (uint8_t)(g.zp.f.TEMP2[0] + SHP(AD_OBJYH, x) + (sum >> 8));
    }

    /* ldy #$27 / cpx #$01 / bcc FIRE3_51 / ldy #$1F / jmp SNDOO
     * X is still R8, so the carry that selects the sound also becomes
     * SNDOO's priority flag: clear for the player, set for the saucer. */
    if (x < 0x01)
        ad_sndoo(0x27, false);                      /* PLAYER */
    else
        ad_sndoo(0x1F, true);                       /* SAUCER */
}

/* ------------------------------------------------------------------ */
/* FIRE1 ($64E4) - find a free torpedo slot, Y downward to TEMP3+1     */
/* ------------------------------------------------------------------ */
/* Y = the first slot to try, X = the velocity source; TEMP3+1 is the
 * stop index, R8 and ANGLE+1 as FIRE3 needs them - all set by the
 * caller (FIRE for the ship: Y=7, stop 3, so slots 7..4; EFIRE for the
 * saucer: Y=3, stop 1, so slots 3..2).  The first slot whose SHPPIX,y
 * is zero gets the torpedo; otherwise FIRE2, a bare rts.
 */
void ad_fire1(uint8_t y, uint8_t x)
{
    do {
        if (SHP(AD_OBJ, y) == 0) {                  /* WE FOUND INACTIVE ONE */
            fire3(y, x);
            return;
        }
        y--;
    } while (y != g.zp.f.TEMP3[1]);
    /* FIRE2: rts */
}

/* ------------------------------------------------------------------ */
/* MOVE2 ($6921) - clamp a 16-bit velocity                             */
/* ------------------------------------------------------------------ */
/* A = high byte (bits 15-8), X = low byte.  Positive values of $4000
 * and up become $3FFF.  Negative values below $C1xx become... $C101:
 *
 *     MOVE2_30: cmp #$C1 / bcs MOVE2_35      $C1..$FF pass
 *               ldx #$01                     ; -3FFF=C001
 *               lda #$C1                     ; SET MAX NEGATIVE
 *
 * The comment says $C001 but the code loads A = $C1, so the clamp is
 * $C101 - and $C100, which passes the test, is below it.  Kept as the
 * ROM has it.
 */
uint16_t ad_move2(uint16_t ax)
{
    uint8_t a = (uint8_t)(ax >> 8);

    if (!(a & 0x80)) {
        if (a < 0x40)
            return ax;                              /* IF IN RANGE */
        return 0x3FFF;                              /* MAX */
    }
    if (a >= 0xC1)                                  /* IF IN RANGE */
        return ax;
    return 0xC101;                                  /* SET MAX NEGATIVE */
}

/* ------------------------------------------------------------------ */
/* CACCEL ($68ED) - thrust acceleration for one axis                   */
/* ------------------------------------------------------------------ */
/* A = the axis's current velocity (high byte); R2 = $40 for cosine
 * (X) or 0 for sine (Y).  ACCEL[|v| >> 3] is the multiplier, halved
 * under shields, and the result is sin(ANGLE + R2) * multiplier * 4 as
 * a 16-bit delta: returns the low byte, R1 = the high byte.
 *
 *     jsr MULT / ldy #0 / asl a / bcc + / dey / asl a / sty R1 / rol R1
 *
 * The high byte is the sign-extension of the product (from bit 7) with
 * bit 6 of the product rotated into its bottom, so the delta is a
 * true 16-bit product times four.  The rts is preceded by clc so the
 * caller can `adc XINCL` straight away.  MULT clobbers Y, R1, R2, R3;
 * R1 is rewritten here and MOVE1 sets R2 afresh before each call.
 */
static uint8_t caccel(uint8_t a)
{
    uint8_t y;
    uint8_t prod;

    if (a & 0x80)
        a = ad_comp(a);                             /* GET ABS(VEL) (0-3F) */
    a >>= 3;                                        /* RANGE IT 0-7 */
    a = ad_rom(AD_ACCEL + a);                       /* GET ACCEL MULTIPLIER */
    if (g.zp.f.SHDON & 0x80)                        /* SHIELDS? */
        a >>= 1;                                    /* YES. USE 1/2 */
    g.zp.f.R1 = a;                                  /* MULTIPLIER */

    a = (uint8_t)(g.zp.f.ANGLE[0] + g.zp.f.R2);
    a = ad_sin(a);                                  /* SIN(ANGLE) */
    prod = ad_mult(a);                              /* CACCEL_4: *R1 */

    y = (prod & 0x80) ? 0xFF : 0x00;                /* asl a / bcc / dey: SIGN EXTENDED */
    a = (uint8_t)(prod << 2);                       /* *2, *2 */
    g.zp.f.R1 = (uint8_t)((y << 1) | ((prod >> 6) & 1));  /* sty R1 / rol R1 */
    return a;                                       /* clc / rts */
}

/* ------------------------------------------------------------------ */
/* MOVE1 ($6882) - thrust or drag, every other frame                   */
/* ------------------------------------------------------------------ */
/* Fallen into from MOVE_20.  With thrust: each axis's 16-bit velocity
 * (SHPXI:XINCL) plus CACCEL's delta, ranged by MOVE2.  Without
 * (MOVE1_80): a drag of (0 - v) * 4 on the 16-bit velocity, where v
 * is the high byte -
 *
 *     lda #0 / tax / sec / sbc SHPXI / asl a / asl a
 *     bcc + / dex / clc                sign from the bit shifted out
 *     adc XINCL / sta XINCL / txa / adc SHPXI / sta SHPXI
 */
static void move1(void)
{
    if (ad_hw_switch(AD_THRUST) & 0x80) {
        uint8_t  lo, hi;
        unsigned sum;
        uint16_t v;

        g.zp.f.R2 = 0x40;                           /* SIGNAL TO USE COS */
        lo = caccel(AD_P->f.SHPXI);                 /* COMPUTE ACCELERATION */
        sum = (unsigned)lo + g.zp.f.XINCL;          /* adc XINCL (C = 0): COMPUTE NEW VEL */
        hi = (uint8_t)(g.zp.f.R1 + AD_P->f.SHPXI + (sum >> 8));
        v = ad_move2((uint16_t)((hi << 8) | (sum & 0xFF)));   /* RANGE IT */
        AD_P->f.SHPXI = (uint8_t)(v >> 8);
        g.zp.f.XINCL  = (uint8_t)v;

        g.zp.f.R2 = 0x00;
        lo = caccel(AD_P->f.SHPYI);                 /* COMPUTE ACCEL */
        sum = (unsigned)lo + g.zp.f.YINCL;
        hi = (uint8_t)(g.zp.f.R1 + AD_P->f.SHPYI + (sum >> 8));
        v = ad_move2((uint16_t)((hi << 8) | (sum & 0xFF)));
        AD_P->f.SHPYI = (uint8_t)(v >> 8);
        g.zp.f.YINCL  = (uint8_t)v;
        return;
    }

    /* MOVE1_80: NO THRUST */
    {
        uint8_t  a, x;
        unsigned sum;

        a = (uint8_t)(0 - AD_P->f.SHPXI);           /* A 0-SHPXI */
        x = (a & 0x40) ? 0xFF : 0x00;               /* the carry out of the second asl */
        a = (uint8_t)(a << 2);                      /* *4 */
        sum = (unsigned)a + g.zp.f.XINCL;           /* adc XINCL, carry clear both ways */
        g.zp.f.XINCL = (uint8_t)sum;
        AD_P->f.SHPXI = (uint8_t)(x + AD_P->f.SHPXI + (sum >> 8));

        a = (uint8_t)(0 - AD_P->f.SHPYI);           /* A=0-SHPYI */
        x = (a & 0x40) ? 0xFF : 0x00;
        a = (uint8_t)(a << 2);
        sum = (unsigned)a + g.zp.f.YINCL;
        g.zp.f.YINCL = (uint8_t)sum;
        AD_P->f.SHPYI = (uint8_t)(x + AD_P->f.SHPYI + (sum >> 8));
    }
}

/* ------------------------------------------------------------------ */
/* NEARBY ($6935) - is the area round the ship free of rocks?          */
/* ------------------------------------------------------------------ */
/* Returns true when the ROM leaves Z set: no rock within 5 screen
 * units of the ship in both axes.  Otherwise SDELAY is incremented
 * (the ship waits another frame) and Z is clear.
 *
 * It also carries the anti-stalemate timer: every fourth frame SBTLT
 * counts, and when it wraps one rock is deleted - the first small one
 * found scanning 24 down to 0, else the first medium, and so on:
 *
 *     lda OBJ,x / asl a / bmi skip      bit 6 = special rock, leave it
 *     and R0 / beq skip                 R0 = 2, 4, 8, ... against OBJ*2
 *     dec NROCKS / lda #0 / sta OBJ,x
 *     ... asl R0 / bpl again            R0 runs to $80, so six passes
 *
 * The last three passes test picture bits rather than size bits.  That
 * is the ROM; a live rock always has a size bit, so they never find
 * anything new.
 *
 * The distance tests are on the high position bytes only:
 *
 *     lda OBJXH,x / sec / sbc SHPXH / cmp #5 / bcc close
 *     cmp #$FB / bcc far                 so -5..4 is close
 */
static bool nearby(void)
{
    uint8_t x;

    if ((g.zp.f.FRAME[0] & 0x03) == 0) {            /* COUNT EVERY 4 FRAMES */
        if (++g.zp.f.SBTLT == 0) {                  /* 16 SECONDS */
            g.zp.f.R0 = 0x02;                       /* FIND A SMALL OBJ */
            do {                                    /* NEARBY_14 */
                x = 0x18;
                do {                                /* NEARBY_13 */
                    uint8_t a = (uint8_t)(ad_obj()[x] << 1);   /* MAKE SURE ITS NOT A SPECIAL */
                    if (!(a & 0x80) && (a & g.zp.f.R0)) {
                        AD_P->f.NROCKS--;           /* TAKE 1 FROM TOTAL */
                        ad_obj()[x] = 0;            /* ZAP ROCK */
                        goto nearby_11;             /* (ALWAYS) */
                    }
                } while (x-- != 0);                 /* dex / bpl */
                g.zp.f.R0 <<= 1;                    /* LOOK FOR NEXT SIZE OBJ */
            } while (!(g.zp.f.R0 & 0x80));
        }
    }

nearby_11:
    x = 0x18;
    do {                                            /* NEARBY_10 */
        if (ad_obj()[x] != 0) {                     /* OBJECT ALIVE */
            uint8_t d = (uint8_t)(ad_objxh()[x] - AD_P->f.SHPXH);
            if (d < 0x05 || d >= 0xFB) {            /* IF CLOSE ENOUGH */
                d = (uint8_t)(ad_objyh()[x] - AD_P->f.SHPYH);
                if (d < 0x05 || d >= 0xFB) {        /* TOO CLOSE */
                    /* NEARBY_50: inc SDELAY / rts - "SETS NON-ZERO FLAG",
                     * Z being that of the incremented byte. */
                    return ++AD_P->f.SDELAY == 0;
                }
            }
        }
    } while (x-- != 0);                             /* LOOP THRU ALL OBJECTS */
    return true;                                    /* inx: ZERO FLAG ON EXIT */
}

/* ------------------------------------------------------------------ */
/* MOVE ($6847) - rotate and thrust the ship, or bring it back         */
/* ------------------------------------------------------------------ */
/* While the ship is dead (SHPPIX = 0) it counts SDELAY down and, at
 * zero, asks NEARBY; a clear area brings the ship back at quarter
 * size (SHPPIX = 1) with SBTL = 1 and the back-to-life sound.  Alive,
 * the rotate switches move ANGLE by +3 (left) or -3 (right), left
 * winning if both are held, and on even frames it falls into MOVE1.
 */
void ad_move(void)
{
    if (g.zp.f.NPLAYR == 0)
        return;                                     /* IN ATTRACT MODE */
    if (AD_P->f.SHPPIX & 0x80)
        return;                                     /* IF EXPLODING */

    if (AD_P->f.SHPPIX == 0) {
        /* MOVE_1 */
        if (--AD_P->f.SDELAY != 0)
            return;                                 /* NOT DONE YET */
        if (!nearby())
            return;                                 /* IF SOMETHING CLOSE BY */
        /* MOVE_92 */
        AD_P->f.SHPPIX = 0x01;                      /* USE 1/4 SIZE PICTURE */
        g.zp.f.SBTL = 0x01;                         /* SHIP BACK TO LIFE FLAG */
        ad_sndon(0x47);                             /* START BACK TO LIFE SOUND */
        return;
    }

    /* MOVE_5 */
    if (ad_hw_switch(AD_ROTL) & 0x80)
        g.zp.f.ANGLE[0] = (uint8_t)(g.zp.f.ANGLE[0] + 0x03);     /* MOVE_15 */
    else if (ad_hw_switch(AD_ROTR) & 0x80)          /* MOVE_10 */
        g.zp.f.ANGLE[0] = (uint8_t)(g.zp.f.ANGLE[0] + 0xFD);

    /* MOVE_20: lsr FRAME / bcs RTS.3 - EVERY OTHER FRAME CHECK THRUST */
    if (g.zp.f.FRAME[0] & 0x01)
        return;
    move1();
}
