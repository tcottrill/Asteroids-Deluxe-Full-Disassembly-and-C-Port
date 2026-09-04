/* Probe for mathrom.c - checks the maths against values worked out by
 * hand from the listing (disasm/astdelux2_main.asm, TRIROT at
 * $49B3-$4A6C and DSTRD0 at $70D4-$714D).  Every asserted value is
 * printed so the numbers can be compared with a hand simulation.
 *
 * Built by build_mod.bat mathrom; run test_mathrom.exe --probe.
 * Returns the number of failures.
 */
#include <stdio.h>

#include "astdelux.h"

static int fails;

static void check(const char *what, unsigned got, unsigned want)
{
    printf("  %-40s got $%02X  want $%02X  %s\n", what, got, want,
           got == want ? "ok" : "FAIL");
    if (got != want)
        fails++;
}

/* (a) SIN over all 256 angles, folded the way SIN1 does it. */
static void probe_sin(void)
{
    printf("(a) SIN: table folding\n");
    int bad = 0;
    for (unsigned a = 0; a < 256; a++) {
        unsigned h = a & 0x7F;
        unsigned idx = (h >= 0x41) ? ((h ^ 0x7F) + 1) & 0xFF : h;   /* $80 - h */
        uint8_t want = ad_rom((uint16_t)(AD_SINCOS + idx));
        if (a & 0x80)
            want = (uint8_t)((want ^ 0xFF) + 1);                    /* COMP */
        if (ad_sin((uint8_t)a) != want) {
            printf("  sin($%02X) = $%02X, want $%02X  FAIL\n", a, ad_sin((uint8_t)a), want);
            bad++;
        }
    }
    printf("  all 256 angles against the folded table: %s\n", bad ? "FAIL" : "ok");
    fails += bad;

    check("sin($00)", ad_sin(0x00), 0x00);
    check("sin($40)  (SINCOS[$40], the peak)", ad_sin(0x40), ad_rom(AD_SINCOS + 0x40));
    check("sin($80)  (-sin(0) = COMP(0))", ad_sin(0x80), 0x00);
    check("sin($C0)  (-sin($40))", ad_sin(0xC0), (uint8_t)(-(int)ad_rom(AD_SINCOS + 0x40)));
    check("sin($20)  (SINCOS[$20])", ad_sin(0x20), ad_rom(AD_SINCOS + 0x20));
    check("sin($60)  (SINCOS[$80-$60] = [$20])", ad_sin(0x60), ad_rom(AD_SINCOS + 0x20));
    check("sin($41)  (SINCOS[$3F], the eor/adc fold)", ad_sin(0x41), ad_rom(AD_SINCOS + 0x3F));
    check("sin($7F)  (SINCOS[$01])", ad_sin(0x7F), ad_rom(AD_SINCOS + 0x01));

    /* Symmetry the folding produces: sin($80-a) == sin(a) on the
     * positive half, sin(a+$80) == -sin(a) everywhere. */
    bad = 0;
    for (unsigned a = 0; a < 0x80; a++)
        if (ad_sin((uint8_t)a) != ad_sin((uint8_t)(0x80 - a)))
            bad++;
    printf("  sin($80-a) == sin(a) for a in 0..$7F:   %s\n", bad ? "FAIL" : "ok");
    fails += bad;
    bad = 0;
    for (unsigned a = 0; a < 256; a++)
        if (ad_sin((uint8_t)(a + 0x80)) != (uint8_t)((ad_sin((uint8_t)a) ^ 0xFF) + 1))
            bad++;
    printf("  sin(a+$80) == -sin(a) for all a:        %s\n", bad ? "FAIL" : "ok");
    fails += bad;
}

/* (b) COS is SIN of the angle plus $40. */
static void probe_cos(void)
{
    printf("(b) COS: cos(a) == sin(a+$40)\n");
    int bad = 0;
    for (unsigned a = 0; a < 256; a++)
        if (ad_cos((uint8_t)a) != ad_sin((uint8_t)(a + 0x40)))
            bad++;
    printf("  all 256 angles: %s\n", bad ? "FAIL" : "ok");
    fails += bad;
    check("cos($00) (= sin($40))", ad_cos(0x00), ad_rom(AD_SINCOS + 0x40));
    check("cos($40) (= sin($80) = 0)", ad_cos(0x40), 0x00);
    check("cos($C0) (= sin($00) = 0)", ad_cos(0xC0), 0x00);
}

/* (c) ATAN on the axes and diagonals, plus two off-axis points whose
 * quotient exercises DIVIDE's fractional bits.
 *
 * Hand simulation of the listing:
 *   (+$40, 0)     ATAN1 -> ATAN2: |y|=0 < |x|=$40 -> ATAN3: DIVIDE(0,$40)=0,
 *                 ATANA[0] = $00.
 *   (0, +$40)     ATAN2: |y|=$40 >= |x|=0 -> arccot path: DIVIDE(0,$40)=0,
 *                 ATANA[0]=0, sbc #$40 = $C0, COMP = $40.
 *   (-$40, 0)     ATAN1 negates x, ATAN2 gives 0, eor #$80 = $80, COMP = $80.
 *   (0, -$40)     ATAN negates y, ATAN1 gives $40, COMP = $C0.
 *   (+$40, +$40)  equal: arccot path, DIVIDE($40,$40) = $10 (first bit
 *                 set, rest 0), ATANA[$10] = $20, sbc = $E0, COMP = $20.
 *   (-$40, +$40)  ATAN2 gives $20, eor #$80 = $A0, COMP = $60.
 *   (-$40, -$40)  ATAN1 gives $60, COMP = $A0.
 *   (+$40, -$40)  ATAN1 gives $20, COMP = $E0.
 *   (+$40, +$20)  DIVIDE($20,$40): 0, then $40>=$40 -> 1, then 0,0,0 =
 *                 $08; ATANA[8] = $13.
 *   (+$20, +$40)  arccot: DIVIDE($20,$40) = 8, ATANA[8]=$13, sbc #$40 =
 *                 $D3, COMP = $2D.
 *   (+$40, +$3F)  DIVIDE($3F,$40): 0, then $7E>=$40 -> $3E,1; $7C -> 1;
 *                 $78 -> 1; $70 -> 1 = $0F; ATANA[$0F] = $1F.
 */
static void probe_atan(void)
{
    printf("(c) ATAN: axes, diagonals, and two DIVIDE fractions\n");
    check("atan(x=+$40, y=  0)", ad_atan(0x40, 0x00), 0x00);
    check("atan(x=  0, y=+$40)", ad_atan(0x00, 0x40), 0x40);
    check("atan(x=-$40, y=  0)", ad_atan(0xC0, 0x00), 0x80);
    check("atan(x=  0, y=-$40)", ad_atan(0x00, 0xC0), 0xC0);
    check("atan(x=+$40, y=+$40)", ad_atan(0x40, 0x40), 0x20);
    check("atan(x=-$40, y=+$40)", ad_atan(0xC0, 0x40), 0x60);
    check("atan(x=-$40, y=-$40)", ad_atan(0xC0, 0xC0), 0xA0);
    check("atan(x=+$40, y=-$40)", ad_atan(0x40, 0xC0), 0xE0);
    check("atan(x=+$40, y=+$20)  quotient $08", ad_atan(0x40, 0x20), 0x13);
    check("atan(x=+$20, y=+$40)  $40 - $13", ad_atan(0x20, 0x40), 0x2D);
    check("atan(x=+$40, y=+$3F)  quotient $0F", ad_atan(0x40, 0x3F), 0x1F);
    check("atan(x=+$7F, y=+$01)  quotient 0", ad_atan(0x7F, 0x01), 0x00);
    /* Both zero takes the arccot path with divisor 0: DIVIDE's five
     * compares all succeed, the quotient is $1F, and ATANA[$1F] is the
     * byte past the 17-entry table - $712E, the $F4 operand of DIVIDE's
     * own `bpl DIVIDE_10`.  So the ROM answers $40 - $F4 = $4C, and the
     * port, reading the table through ad_rom(), answers the same. */
    check("atan(x=  0, y=  0)   (both zero, arccot path)", ad_atan(0x00, 0x00), 0x4C);
}

/* (d) MULT: R1 = $80 is unity, $40 halves, for both signs.
 *   +$50 * $80: asl R1 -> C=1, R3 = $50; lsr R2 = $28; asl R1 -> 0, no C
 *               -> done, $50.
 *   +$50 * $40: asl R1 -> $80, no C; lsr R2 = $28; asl R1 -> C=1, R3 =
 *               $28; lsr R2 = $14; asl R1 -> 0 -> done, $28.
 *   -$50 ($B0): magnitude $50 as above, re-negated: $B0 / $D8.
 *   +$51 * $40: the multiplicand is halved before the add: $28.
 */
static void probe_mult(void)
{
    printf("(d) MULT\n");
    g.zp.f.R1 = 0x80; check("MULT +$50 * $80 (identity)", ad_mult(0x50), 0x50);
    g.zp.f.R1 = 0x80; check("MULT -$50 * $80 (identity)", ad_mult(0xB0), 0xB0);
    g.zp.f.R1 = 0x40; check("MULT +$50 * $40 (half)", ad_mult(0x50), 0x28);
    g.zp.f.R1 = 0x40; check("MULT -$50 * $40 (half)", ad_mult(0xB0), 0xD8);
    g.zp.f.R1 = 0x40; check("MULT +$51 * $40 (truncates)", ad_mult(0x51), 0x28);
    g.zp.f.R1 = 0x80; check("MULT +$7F * $80", ad_mult(0x7F), 0x7F);
    g.zp.f.R1 = 0x00; check("MULT +$50 * $00", ad_mult(0x50), 0x00);
    g.zp.f.R1 = 0xC0; check("MULT +$40 * $C0 (1.5)", ad_mult(0x40), 0x60);
    g.zp.f.R1 = 0x80; (void)ad_mult(0x50);
    check("MULT leaves R1 = 0 afterwards", g.zp.f.R1, 0x00);
}

/* (e) COMP. */
static void probe_comp(void)
{
    printf("(e) COMP\n");
    check("comp($01)", ad_comp(0x01), 0xFF);
    check("comp($00)", ad_comp(0x00), 0x00);
    check("comp($80)", ad_comp(0x80), 0x80);
    check("comp($7F)", ad_comp(0x7F), 0x81);
}

/* (f) RANGE on +$0C00 and -$0C00: both inside the -$10..$0F window,
 * so both come back untouched.  Then $1234, which is wrapped:
 * TEMP2 = 0-$34 = $CC with a borrow, high = 0-$12-1 = $ED. */
static void probe_range(void)
{
    printf("(f) RANGE\n");
    g.zp.f.TEMP2[0] = 0x00;
    check("RANGE +$0C00: high", ad_range(0x0C), 0x0C);
    check("RANGE +$0C00: TEMP2 (low)", g.zp.f.TEMP2[0], 0x00);
    g.zp.f.TEMP2[0] = 0x00;
    check("RANGE -$0C00 ($F400): high", ad_range(0xF4), 0xF4);
    check("RANGE -$0C00: TEMP2 (low)", g.zp.f.TEMP2[0], 0x00);
    g.zp.f.TEMP2[0] = 0x34;
    check("RANGE +$1234 -> $EDCC: high", ad_range(0x12), 0xED);
    check("RANGE +$1234: TEMP2 (low)", g.zp.f.TEMP2[0], 0xCC);
    g.zp.f.TEMP2[0] = 0xCC;
    check("RANGE -$1234 ($EDCC) -> $1234: high", ad_range(0xED), 0x12);
    check("RANGE -$1234: TEMP2 (low)", g.zp.f.TEMP2[0], 0x34);
    g.zp.f.TEMP2[0] = 0x00;
    check("RANGE +$1000 -> $F000: high", ad_range(0x10), 0xF0);
    check("RANGE +$1000: TEMP2 (low)", g.zp.f.TEMP2[0], 0x00);
    g.zp.f.TEMP2[0] = 0x00;
    check("RANGE $F000 -> $1000: high", ad_range(0xF0), 0xF0);
}

/* RTST: below the difficulty threshold it is RANGE; above it the
 * sign is compared with a bit of the object index.
 *   x = 0:  X-axis, X < $21: lsr -> 0, lsr -> 0, ror -> 0.  Bit 7 clear.
 *   x = 2:  lsr -> 1 (C=0), lsr -> 0 (C=1), ror -> $80.  Bit 7 set.
 *   x = $21 (slot 0 + $21): lsr -> $10 (C=1), ror -> $88.  Bit 7 set.
 *   x = $22: lsr -> $11 (C=0), ror -> $08.  Bit 7 clear.
 */
static void probe_rtst(void)
{
    printf("RTST\n");
    g.zp.f.BINSCR = 0; g.zp.f.R9 = 0; g.zp.f.TEMP2[0] = 0;
    check("RTST easy, +$0C00 -> RANGE, untouched", ad_rtst(0x0C, 0), 0x0C);
    check("RTST easy, +$1200 -> RANGE, wrapped", ad_rtst(0x12, 0), 0xEE);
    check("RTST easy, +$0200 -> untouched (< $400)", ad_rtst(0x02, 0), 0x02);
    g.zp.f.BINSCR = 5; g.zp.f.R9 = 3;
    g.zp.f.TEMP2[0] = 0;
    check("RTST hard, x=0,   +$0C00 -> kept", ad_rtst(0x0C, 0x00), 0x0C);
    g.zp.f.TEMP2[0] = 0;
    check("RTST hard, x=0,   -$0C00 -> negated", ad_rtst(0xF4, 0x00), 0x0C);
    g.zp.f.TEMP2[0] = 0;
    check("RTST hard, x=2,   +$0C00 -> negated", ad_rtst(0x0C, 0x02), 0xF4);
    g.zp.f.TEMP2[0] = 0;
    check("RTST hard, x=$21, +$0C00 -> negated", ad_rtst(0x0C, 0x21), 0xF4);
    g.zp.f.TEMP2[0] = 0;
    check("RTST hard, x=$22, +$0C00 -> kept", ad_rtst(0x0C, 0x22), 0x0C);
    g.zp.f.TEMP2[0] = 0;
    check("RTST hard, x=0,   +$0200 -> untouched", ad_rtst(0x02, 0x00), 0x02);
    g.zp.f.BINSCR = 5; g.zp.f.R9 = 2; g.zp.f.TEMP2[0] = 0;
    check("RTST score ok but R9=2 -> RANGE", ad_rtst(0xF4, 0x02), 0xF4);
}

/* CPUTD: ship (slot 25) at $0510, rock 0 at $0320 -> $01F0. */
static void probe_cputd(void)
{
    printf("CPUTD\n");
    uint8_t *p = AD_P->raw;
    p[AD_OBJXL + AD_SHIP] = 0x10; p[AD_OBJXH + AD_SHIP] = 0x05;
    p[AD_OBJXL + 0]       = 0x20; p[AD_OBJXH + 0]       = 0x03;
    check("CPUTD ship - rock0, X axis: high", ad_cputd(0, AD_SHIP), 0x01);
    check("CPUTD ship - rock0, X axis: TEMP2", g.zp.f.TEMP2[0], 0xF0);
    check("CPUTD rock0 - ship: high ($FE10)", ad_cputd(AD_SHIP, 0), 0xFE);
    check("CPUTD rock0 - ship: TEMP2", g.zp.f.TEMP2[0], 0x10);
    /* Y axis through the +$21 alias: OBJXL+$21 is OBJYL. */
    p[AD_OBJYL + AD_SHIP] = 0x00; p[AD_OBJYH + AD_SHIP] = 0x02;
    p[AD_OBJYL + 0]       = 0x80; p[AD_OBJYH + 0]       = 0x00;
    check("CPUTD Y axis (+$21): high ($0180)", ad_cputd(0 + 0x21, AD_SHIP + 0x21), 0x01);
    check("CPUTD Y axis: TEMP2", g.zp.f.TEMP2[0], 0x80);
}

/* SCALER, end to end into ATAN.
 *   dx=+$0400, dy=0:   merged $04, bit 2 -> X=3, shift right 4:
 *                      dx=$0040 -> low $40; atan($40,0) = 0.
 *   dx=-$0400 ($FC00): |$FC| = $04 -> same shift; $FC00>>4 = $0FC0,
 *                      low byte $C0 (negative); atan($C0,0) = $80.
 *   dx=+$0800:         bit 3 -> X=4, shift left 3: $4000 -> high $40.
 *   dx=$0800,dy=$0400: dy<<3 = $2000 -> $20; atan($40,$20) = $13.
 *   dx=$0000,dy=$0001: merged 0 -> X=0, shift right 1: dy=0 -> atan(0,0)
 *                      = $40 (the arccot path with both zero).
 */
static void scaler_set(uint16_t dx, uint16_t dy)
{
    g.zp.f.R2 = (uint8_t)dx;
    g.zp.f.R3 = (uint8_t)(dx >> 8);
    g.zp.f.TEMP2[0] = (uint8_t)dy;
}
static void probe_scaler(void)
{
    printf("SCALER (tail-calls ATAN)\n");
    scaler_set(0x0400, 0x0000); check("SCALER dx=+$0400 dy=0", ad_scaler(0x00), 0x00);
    scaler_set(0x0000, 0x0400); check("SCALER dx=0 dy=+$0400", ad_scaler(0x04), 0x40);
    scaler_set(0xFC00, 0x0000); check("SCALER dx=-$0400 dy=0", ad_scaler(0x00), 0x80);
    scaler_set(0x0000, 0xFC00); check("SCALER dx=0 dy=-$0400", ad_scaler(0xFC), 0xC0);
    scaler_set(0x0400, 0x0400); check("SCALER dx=+$0400 dy=+$0400", ad_scaler(0x04), 0x20);
    scaler_set(0xFC00, 0xFC00); check("SCALER dx=-$0400 dy=-$0400", ad_scaler(0xFC), 0xA0);
    scaler_set(0x0800, 0x0000); check("SCALER dx=+$0800 dy=0 (left shift)", ad_scaler(0x00), 0x00);
    scaler_set(0x0800, 0x0400); check("SCALER dx=+$0800 dy=+$0400", ad_scaler(0x04), 0x13);
    scaler_set(0xF800, 0x0400); check("SCALER dx=-$0800 dy=+$0400", ad_scaler(0x04), 0x6D);
    scaler_set(0x0010, 0x0010); check("SCALER dx=+$0010 dy=+$0010 (X=0)", ad_scaler(0x00), 0x20);
    scaler_set(0x0000, 0x0001); check("SCALER dx=0 dy=+$0001 (shifted to 0)", ad_scaler(0x00), 0x4C);   /* atan(0,0), see above */
    scaler_set(0x0400, 0x0000); (void)ad_scaler(0x00);
    check("SCALER leaves R3:R2 shifted ($0040): R2", g.zp.f.R2, 0x40);
    check("SCALER leaves R3", g.zp.f.R3, 0x00);
}

int ad_probe(void)
{
    fails = 0;
    probe_sin();
    probe_cos();
    probe_atan();
    probe_mult();
    probe_comp();
    probe_range();
    probe_rtst();
    probe_cputd();
    probe_scaler();
    printf("mathrom probe: %d failure(s)\n", fails);
    return fails;
}
