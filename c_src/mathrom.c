/* The maths - TRIROT.MAC's MULT/RANGE/RTST/CPUTD/SCALER at $49B3-$4A6C
 * and DSTRD0.MAC's ATAN/COMP/DIVIDE/COS/SIN at $70D4-$714D.
 *
 * Everything here is 8-bit two's complement done the way the 6502 does
 * it: values are uint8_t so wraparound is free, "negative" is a bit-7
 * test, and a borrow is carried by hand where the ROM chains an SBC.
 * The routines share zero-page cells with their callers - CPUTD leaves
 * the low byte in TEMP2, MULT reads R1 and wrecks R1-R3, SCALER eats
 * R2:R3 - so those stay in g.zp rather than becoming locals.
 *
 * Contracts: disasm/CALLING_NOTES.md section 6.
 */
#include "astdelux.h"

/* ------------------------------------------------------------------ */
/* COMP ($70EC) - two's complement negate.                             */
/* ------------------------------------------------------------------ */
/*     eor #$FF / clc / adc #$01
 *
 * Only A is touched.  $80 stays $80, which is why SCALER can OR two
 * "absolute" values and still find a bit 7 set; it never matters,
 * because the ranged differences are far smaller than $8000. */
uint8_t ad_comp(uint8_t a)
{
    return (uint8_t)((a ^ 0xFF) + 1);
}

/* ------------------------------------------------------------------ */
/* SIN1 ($7141) - 0..$7F (0..180 degrees) via the quarter-wave table.  */
/* ------------------------------------------------------------------ */
/*     cmp #$41 / bcc SIN1_10          first quadrant: index directly
 *     eor #$7F / adc #$00             else $80-A: the CMP left C set,
 *                                     so the ADC adds the 1
 *     SIN1_10: tax / lda SINCOS,x
 *
 * SINCOS ($4B51) is 65 bytes, sin 0..90 degrees times 127; index $40
 * is the peak. */
static uint8_t sin1(uint8_t a)
{
    if (a >= 0x41)
        a = (uint8_t)((a ^ 0x7F) + 1);
    return ad_rom(AD_SINCOS + a);
}

/* ------------------------------------------------------------------ */
/* SIN ($7137) - A = angle (0..$FF is 0..360 degrees) -> sine, -127..127 */
/* ------------------------------------------------------------------ */
/*     bpl SIN1                        pi > angle >= 0: table half
 *     and #$7F / jsr SIN1 / jmp COMP  sin(pi + a) = -sin(a)
 *
 * The ROM requires N to reflect A on entry; here the sign is bit 7. */
uint8_t ad_sin(uint8_t a)
{
    if (!(a & 0x80))
        return sin1(a);
    return ad_comp(sin1((uint8_t)(a & 0x7F)));
}

/* COS ($7134) - `clc / adc #$40` then falls into SIN. */
uint8_t ad_cos(uint8_t a)
{
    return ad_sin((uint8_t)(a + 0x40));
}

/* ------------------------------------------------------------------ */
/* DIVIDE ($7121) - 5-bit unsigned quotient of A / TEMP2+1.            */
/* ------------------------------------------------------------------ */
/*     ldy #$04
 *     DIVIDE_10: cmp $0D / bcc DIVIDE_20      divisor bigger: bit is 0
 *                sbc $0D                      else subtract, C stays 1
 *     DIVIDE_20: rol TEMP2                    shift the bit in
 *                asl a / dey / bpl DIVIDE_10  five times
 *     lda TEMP2 / and #$1F
 *
 * TEMP2 is the quotient shift register: whatever it held is shifted up
 * and out, and the AND at the end keeps the five new bits.  The ASL
 * of the dividend is 8-bit, so a dividend with bit 7 set loses it -
 * SCALER's normalisation keeps every operand below $80 so that never
 * happens in play.  The first bit is the integer part, the other four
 * are sixteenths: $10 exactly when dividend == divisor. */
static uint8_t divide(uint8_t a)
{
    uint8_t d = g.zp.f.TEMP2[1];
    for (int y = 4; y >= 0; y--) {
        bool bit = (a >= d);                    /* cmp $0D: C = A >= divisor */
        if (bit)
            a = (uint8_t)(a - d);               /* sbc with C set: no borrow */
        g.zp.f.TEMP2[0] = (uint8_t)((g.zp.f.TEMP2[0] << 1) | (bit ? 1 : 0));
        a = (uint8_t)(a << 1);
    }
    return (uint8_t)(g.zp.f.TEMP2[0] & 0x1F);
}

/* ------------------------------------------------------------------ */
/* ATAN ($70D4) and its sub-entries: quadrant folding by recursion.     */
/* ------------------------------------------------------------------ */
/* The ROM folds the argument into the first octant with three nested
 * calls, each of which negates or mirrors what the level below
 * returns.  Kept as the same three functions calling each other, so
 * the result is bit-exact where the folding is asymmetric (COMP of 0
 * is 0; $40 - r for the arccot branch).
 *
 * Register roles going down:
 *   ATAN   X = x component, Y = y component, both signed
 *   ATAN1  A = |y|, X = x (signed)
 *   ATAN2  A = |x|, Y = |y|
 *   ATAN3  A = dividend, TEMP2+1 = divisor
 */

/* ATAN3 ($7107) - first octant: table lookup on the 5-bit quotient.
 *
 *     jsr DIVIDE / tax / lda ATANA,x
 *
 * ATANA ($710F) is 17 bytes, angle in 1/256 turn for tan = n/16. */
static uint8_t atan3(uint8_t a)
{
    return ad_rom(AD_ATANA + divide(a));
}

/* ATAN2 ($70F2) - first quadrant.  A = |x|, Y = |y|.
 *
 *     sta $0D                         divisor = |x|
 *     tya / cmp $0D / bcc ATAN3       |y| < |x|: arctan(y/x) directly
 *     ldy $0D / sta $0D / tya         else swap: divisor = |y|, A = |x|
 *     jsr ATAN3 / sec / sbc #$40 / jmp COMP     arctan(y/x) = $40 - arccot
 *
 * Equal magnitudes take the second path (the CMP leaves C set), where
 * DIVIDE yields exactly $10 and ATANA[$10] = $20: 45 degrees. */
static uint8_t atan2_(uint8_t a, uint8_t y)
{
    g.zp.f.TEMP2[1] = a;                        /* divisor (X) */
    a = y;
    if (a < g.zp.f.TEMP2[1])
        return atan3(a);
    y = g.zp.f.TEMP2[1];
    g.zp.f.TEMP2[1] = a;
    a = y;
    a = atan3(a);
    a = (uint8_t)(a - 0x40);                    /* sec / sbc #$40 */
    return ad_comp(a);
}

/* ATAN1 ($70E0) - right half-plane.  A = |y|, X = x (signed).
 *
 *     tay                             Y = |y|
 *     txa / bpl ATAN2                 x >= 0: straight in
 *     jsr COMP / jsr ATAN2            else arctan(y/-x)
 *     eor #$80                        ... and fall into COMP:
 *                                     arctan(y/x) = $80 - arctan(y/-x)
 */
static uint8_t atan1(uint8_t a, uint8_t x)
{
    uint8_t y = a;
    a = x;
    if (!(a & 0x80))
        return atan2_(a, y);
    a = ad_comp(a);
    a = atan2_(a, y);
    a ^= 0x80;
    return ad_comp(a);
}

/* ATAN ($70D4) - X = x component, Y = y component, signed.
 * Returns the angle, 0..$FF with $40 = 90 degrees.
 *
 *     tya / bpl ATAN1                 y >= 0
 *     jsr COMP / jsr ATAN1 / jmp COMP arctan(y/x) = -arctan(-y/x)
 *
 * Uses TEMP2 and TEMP2+1 as DIVIDE's scratch; both are left behind. */
uint8_t ad_atan(uint8_t x, uint8_t y)
{
    uint8_t a = y;
    if (!(a & 0x80))
        return atan1(a, x);
    a = ad_comp(a);
    a = atan1(a, x);
    return ad_comp(a);
}

/* ------------------------------------------------------------------ */
/* MULT ($49B3) - A = multiplicand (signed) times R1 ($80 = 1.000).    */
/* ------------------------------------------------------------------ */
/* Shift-and-add on the magnitude, with the sign of the multiplicand
 * remembered in Y and put back at the end.  Y, R1, R2, R3 are
 * destroyed; X is not touched.
 *
 *     tay / bpl MULT_1 / eor #$FF / clc / adc #$01     |A|
 *     MULT_1: sta R2 / lda #$00 / sta R3
 *     MULT_5: asl R1 / bcs MULT_2 / bne MULT_3         next multiplier bit
 *     MULT_6: lda R3 / cpy #$00 / bpl MULT_4           CPY leaves C set,
 *             eor #$FF / adc #$00                      so this is -R3
 *     MULT_4: rts
 *     MULT_2: lda R3 / clc / adc R2 / sta R3           add the multiplicand
 *     MULT_3: lsr R2 / bne MULT_5 / beq MULT_6         halve it
 *
 * The loop ends either when the multiplier runs out of bits (the ASL
 * left R1 zero with no carry) or when the halved multiplicand reaches
 * zero, whichever is first.  Note the halving is a truncation each
 * step, so odd multiplicands lose a little: $51 * $40 is $28, not $28.8. */
uint8_t ad_mult(uint8_t a)
{
    uint8_t y = a;                              /* tay: the sign */
    if (a & 0x80)
        a = (uint8_t)((a ^ 0xFF) + 1);
    g.zp.f.R2 = a;
    g.zp.f.R3 = 0;

    for (;;) {
        /* MULT_5 */
        bool c = (g.zp.f.R1 & 0x80) != 0;
        g.zp.f.R1 = (uint8_t)(g.zp.f.R1 << 1);
        if (c)
            g.zp.f.R3 = (uint8_t)(g.zp.f.R3 + g.zp.f.R2);   /* MULT_2 */
        else if (g.zp.f.R1 == 0)
            break;                                          /* MULT_6 */
        /* MULT_3 */
        g.zp.f.R2 >>= 1;
        if (g.zp.f.R2 == 0)
            break;                                          /* MULT_6 */
    }

    a = g.zp.f.R3;
    if (y & 0x80)
        a = (uint8_t)((a ^ 0xFF) + 1);          /* eor #$FF / adc #$00, C=1 */
    return a;
}

/* ------------------------------------------------------------------ */
/* RANGE ($49DF) - wrap a 16-bit difference the short way round.       */
/* ------------------------------------------------------------------ */
/* RANGE_4 ($49E9): (TEMP2,R5) = 0 - (TEMP2,R5), returning the high
 * byte.  Shared with RTST, which enters here to force the *long* way.
 *
 *     lda #$00 / sec / sbc TEMP2 / sta TEMP2
 *     lda #$00 / sbc R5                   the SBC's borrow chains up
 */
static uint8_t range_4(void)
{
    uint8_t lo = g.zp.f.TEMP2[0];
    bool borrow = (lo != 0);                    /* 0 - lo clears C unless lo == 0 */
    g.zp.f.TEMP2[0] = (uint8_t)(0 - lo);
    return (uint8_t)(0 - g.zp.f.R5 - (borrow ? 1 : 0));
}

/* RANGE: TEMP2:A = difference (low:high).  A high byte inside
 * -$10..$0F is left alone; anything larger means the other way round
 * the torus is shorter, so the whole 16-bit value is negated.  R5 is
 * destroyed.
 *
 *     cmp #$10 / bcc RANGE_1
 *     cmp #$F0 / bcs RANGE_1
 *     sta R5 / (RANGE_4)
 */
uint8_t ad_range(uint8_t a)
{
    if (a < 0x10)
        return a;
    if (a >= 0xF0)
        return a;
    g.zp.f.R5 = a;
    return range_4();
}

/* ------------------------------------------------------------------ */
/* RTST ($49F5) - RANGE with the difficulty hook.  X = object index.   */
/* ------------------------------------------------------------------ */
/* Below 40,000 points (BINSCR < 5) or with fewer than three attackers
 * (R9 < 3) this is RANGE.  Otherwise the sign of the difference is
 * compared with a bit pulled out of the object index, and if they
 * disagree the difference is negated regardless - sending the special
 * rock the long way round so it comes at the ship from the wrong side.
 * Differences within +-$400 (a quarter screen) are returned untouched.
 *
 *     cmp #$04 / bcc RANGE_1
 *     cmp #$FC / bcs RANGE_1
 *     ldy BINSCR / cpy #$05 / bcc RANGE
 *     ldy R9 / cpy #$03 / bcc RANGE
 *     sta R5
 *     txa / cpx #$21 / bcs RTST_2         X-axis index: one extra shift
 *     lsr a
 *     RTST_2: lsr a / ror a               bit 0 of A -> bit 7
 *     eor R5 / bmi RANGE_4
 *     lda R5 / rts
 *
 * The comment in the source says "PUT X/Y BIT INTO BIT 7", but what
 * the LSR/ROR pair actually does is rotate the bit the LSR shifted out
 * back in at the top: bit 1 of the slot for an X test, bit 0 of
 * slot+$21 for a Y test.  Translated as it runs, not as described. */
uint8_t ad_rtst(uint8_t a, uint8_t x)
{
    if (a < 0x04)
        return a;
    if (a >= 0xFC)
        return a;
    if (g.zp.f.BINSCR < 0x05)
        return ad_range(a);
    if (g.zp.f.R9 < 0x03)
        return ad_range(a);
    g.zp.f.R5 = a;

    uint8_t t = x;                              /* txa */
    if (x < 0x21)                               /* cpx #$21 / bcs RTST_2 */
        t >>= 1;                                /* lsr a */
    /* RTST_2: lsr a sets C = bit 0, ror a brings it in at bit 7. */
    bool c = (t & 1) != 0;
    t >>= 1;
    t = (uint8_t)((t >> 1) | (c ? 0x80 : 0));

    if ((t ^ g.zp.f.R5) & 0x80)                 /* eor R5 / bmi RANGE_4 */
        return range_4();
    return g.zp.f.R5;
}

/* ------------------------------------------------------------------ */
/* CPUTD ($4A1A) - 16-bit position difference, object Y minus object X. */
/* ------------------------------------------------------------------ */
/* Y and X are object indexes into OBJXL/OBJXH; add $21 to both to get
 * the Y axis, since OBJYL is $21 past OBJXL (CALLING_NOTES section 0).
 * Returns the high byte; the low byte is left in TEMP2.
 *
 *     lda OBJXL,y / sec / sbc OBJXL,x / sta TEMP2
 *     lda OBJXH,y / sbc OBJXH,x
 */
uint8_t ad_cputd(uint8_t x, uint8_t y)
{
    const uint8_t *p = AD_P->raw;
    uint8_t ly = p[AD_OBJXL + y];
    uint8_t lx = p[AD_OBJXL + x];
    bool borrow = (ly < lx);
    g.zp.f.TEMP2[0] = (uint8_t)(ly - lx);
    return (uint8_t)(p[AD_OBJXH + y] - p[AD_OBJXH + x] - (borrow ? 1 : 0));
}

/* ------------------------------------------------------------------ */
/* SCALER ($4A2A) - normalise two differences, then the angle between. */
/* ------------------------------------------------------------------ */
/* Entry: R2:R3 = X difference (low:high), TEMP2:A = Y difference.
 * Both are shifted by the same amount so the larger magnitude lands
 * with its top bit at bit 6 of a byte, and those bytes go to ATAN.
 * Ends in `jmp ATAN`, so the return value is ATAN's.  R2, R3, R4, R5,
 * TEMP2 and TEMP2+1 are all destroyed.
 *
 *     sta R4 / bpl SCALER_1 / jsr COMP
 *     SCALER_1: sta R5                        |y high|
 *     lda R3 / bpl SCALER_2 / jsr COMP
 *     SCALER_2: ora R5                        |x high| OR |y high|
 *     ldx #$07
 *     SCALER_4: asl a / bmi SCALER_3 / dex / bne SCALER_4
 *
 * The ASL comes before the BMI, so the first test is of bit 6, and X
 * ends as (bit position + 1): 7 for bit 6 down to 1 for bit 0, and 0
 * if the merged high byte was zero (or only bit 7 set, which the
 * first ASL throws away).
 *
 *     SCALER_3: cpx #$04 / bcs SCALER_7
 *     SCALER_5: lsr R3 / ror R2 / lsr R4 / ror TEMP2      16-bit right
 *               dex / bpl SCALER_5                        X+1 times
 *               ldx R2 / ldy TEMP2 / jmp ATAN             the low bytes
 *     SCALER_7: asl R2 / rol R3 / asl TEMP2 / rol R4      16-bit left
 *               inx / cpx #$07 / bcc SCALER_7             until X = 7
 *               ldx R3 / ldy R4 / bcs SCALER_6            the high bytes
 *
 * The right shifts are LSR, not arithmetic, but the sign survives
 * anyway: a negative difference shifted right by X+1 places is still
 * negative in the low byte because RANGE has already kept |high| below
 * $10, so the low byte's bit 7 is a copy of the sign. */
uint8_t ad_scaler(uint8_t a)
{
    g.zp.f.R4 = a;
    if (a & 0x80)
        a = ad_comp(a);
    g.zp.f.R5 = a;                              /* |y high| */
    a = g.zp.f.R3;
    if (a & 0x80)
        a = ad_comp(a);
    a |= g.zp.f.R5;                             /* merged magnitudes */

    uint8_t x = 7;
    for (;;) {                                  /* SCALER_4 */
        a = (uint8_t)(a << 1);
        if (a & 0x80)
            break;
        x--;
        if (x == 0)
            break;
    }

    uint16_t dx = (uint16_t)((g.zp.f.R3 << 8) | g.zp.f.R2);
    uint16_t dy = (uint16_t)((g.zp.f.R4 << 8) | g.zp.f.TEMP2[0]);

    if (x < 4) {
        /* SCALER_5: shift right X+1 places, pass the low bytes. */
        do {
            dx >>= 1;
            dy >>= 1;
            x--;
        } while (!(x & 0x80));                  /* dex / bpl */
        g.zp.f.R3 = (uint8_t)(dx >> 8);
        g.zp.f.R2 = (uint8_t)dx;
        g.zp.f.R4 = (uint8_t)(dy >> 8);
        g.zp.f.TEMP2[0] = (uint8_t)dy;
        return ad_atan(g.zp.f.R2, g.zp.f.TEMP2[0]);
    }

    /* SCALER_7: shift left until X reaches 7, pass the high bytes. */
    do {
        dx = (uint16_t)(dx << 1);
        dy = (uint16_t)(dy << 1);
        x++;
    } while (x < 7);                            /* inx / cpx #$07 / bcc */
    g.zp.f.R3 = (uint8_t)(dx >> 8);
    g.zp.f.R2 = (uint8_t)dx;
    g.zp.f.R4 = (uint8_t)(dy >> 8);
    g.zp.f.TEMP2[0] = (uint8_t)dy;
    return ad_atan(g.zp.f.R3, g.zp.f.R4);
}
