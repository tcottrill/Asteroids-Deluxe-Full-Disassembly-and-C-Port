/* The object pictures - DSTRD0.MAC and TRIROT.MAC.
 *
 * PICTUR ($6BC4) positions the beam and dispatches on the object; the
 * rest of this file is what it dispatches to.  Nothing here calls a
 * drawing API: every routine appends DVG words to vector RAM through
 * VGLIST exactly as the ROM does, and CPYVEC ($6246) is where the
 * shapes are actually made - it copies a vector list out of ROM while
 * XORing sign masks into it (rotation by reflection), exchanging X
 * and Y (rotation by transposition), and ORing in an intensity the
 * ship frames are stored without.  ROTAST ($4B92) does the same thing
 * to the four rock subroutines that live *inside* vector RAM.
 *
 * The register contracts are in disasm/CALLING_NOTES.md sections 2, 9,
 * 11 and 12.  Where a routine's exit X matters to the next call (the
 * ROM chains AYTOR0 calls with whatever X CPYVEC left behind) the
 * static forms below carry X through a pointer; the public ad_*
 * entries are the fixed-signature wrappers.
 */
#include "astdelux.h"

/* ------------------------------------------------------------------ */
/* Zero-page aliases, as the listing names them                         */
/* ------------------------------------------------------------------ */
#define ZP      (g.zp.f)
#define TEMP1   ZP.TEMP1[0]         /* $09  X sign mask, and the intensity injector */
#define TEMP1H  g.zp.raw[0x0A]      /* $0A  Y sign mask (TEMP1+1; spelled by
                                     * address because TEMP1 is itself a
                                     * macro and would expand inside) */
#define TEMP3   ZP.TEMP3[0]         /* $0E */
#define XCOMP0  ZP.XCOMP[0]         /* $05  X low  */
#define XCOMP1  ZP.XCOMP[1]         /* $06  X high */
#define XCOMP2  ZP.XCOMP[2]         /* $07  Y low  */
#define XCOMP3  ZP.XCOMP[3]         /* $08  Y high */

/* (R0),y - the 6502 read the copy engine does.  R0/R1 point into the
 * vector ROM for every caller in the shipped game (RSOURC, SHLDVC,
 * SHIPSV, EXP16, the TFPIX fragments), but the address space is
 * modelled rather than assumed: a pointer into vector RAM reads
 * vector RAM. */
static inline uint16_t r0r1(void)
{
    return (uint16_t)(ZP.R0 | (ZP.R1 << 8));
}

static inline uint8_t ld_r0(uint8_t y)
{
    uint16_t a = (uint16_t)(r0r1() + y);
    if (a >= 0x4000u && a < 0x4000u + AD_VRAM_SIZE)
        return g.vram[a - 0x4000u];
    return ad_rom(a);
}

/* (VGLIST),y */
static inline void st_vg(uint8_t y, uint8_t v)
{
    ad_vram_wr((uint16_t)(ad_vglist() + y), v);
}

/* `lda TABLE,x` on the active player page, for an index that the ROM
 * lets run past the page: $0200+off.  TRIPIX's control-byte index can
 * reach 15, which from SRTIME ($02F8) is $0307 - the other page. */
static inline uint8_t pg2_rd(uint16_t off)
{
    return off < 0x100u ? AD_P->raw[off] : AD_P3->raw[(off - 0x100u) & 0xFFu];
}

static inline void pg2_wr(uint16_t off, uint8_t v)
{
    if (off < 0x100u)
        AD_P->raw[off] = v;
    else
        AD_P3->raw[(off - 0x100u) & 0xFFu] = v;
}

/* ------------------------------------------------------------------ */
/* CPYVEC / CPYVYX ($6246 / $6279) - copy and modify vectors            */
/* ------------------------------------------------------------------ */
/* Entry: TEMP1 = X sign mask (0 or 4, plus any intensity to OR in),
 * TEMP1+1 = Y sign mask, R0/R1 = source list, R2 negative = swap X
 * and Y, X = count of vectors minus one.  Exit: VGLIST advanced, Y =
 * index of the last byte written, R0/R1 and R2 untouched, R3/R4
 * clobbered.  X leaves as the ROM leaves it: the loop is `dex / bpl`,
 * so an X below $80 exits as $FF, and an X of $80 or more copies one
 * vector and exits as X-1.  SHLDPX and SHPPIC chain AYTOR0 calls on
 * that leftover X, so the static form carries it.
 *
 * A source vector is four bytes (low Y, opcode+high Y, low X, Z+high
 * X) or two (a short vector, opcode $F0 and up).  The Y mask is XORed
 * into the first opcode byte, the X mask into the last - which is how
 * TEMP1 doubles as an intensity injector: the ship and shield frames
 * are stored with Z = 0, and XOR against 0 is OR.
 */
/* The unswapped loop, CPYVEC_10..CPYVEC_20, entered with Y = $FF. */
static uint8_t cpyvec_10(uint8_t *px, uint8_t y)
{
    uint8_t x = *px;
    uint8_t a;

    for (;;) {                               /* CPYVEC_10 */
        y++;
        y++;
        a = (uint8_t)(ld_r0(y) ^ TEMP1H);    /* opcode byte, Y sign applied */
        st_vg(y, a);
        y--;                                 /* back up to the LSB */
        if (a >= 0xF0) {                     /* CPYVEC_20: short vector */
            a = (uint8_t)(ld_r0(y) ^ TEMP1); /* mask for X sign */
            st_vg(y, a);
            y++;
        } else {
            st_vg(y, ld_r0(y));              /* copy the rest */
            y++;
            y++;
            st_vg(y, ld_r0(y));
            y++;
            a = (uint8_t)(ld_r0(y) ^ TEMP1); /* apply any sign changes */
            st_vg(y, a);
        }
        /* CPYVEC_15 */
        x--;
        if (x & 0x80)
            break;
    }
    *px = x;
    ad_vgadd(y);                             /* jmp VGADD - the return path */
    return y;
}

/* The swapped copy.  Each long vector's two words are exchanged by
 * XOR-juggling the packed bytes so the opcode stays with the first
 * word and Z with the second; R3, R4 and the stack (`pha`, here a
 * local) are the scratch.  The `bcs CPYVYX_5` that ends the short
 * case is an always-branch: nothing between the `cmp #$F0` and it
 * touches carry. */
static uint8_t cpyvyx(uint8_t *px, uint8_t y)
{
    uint8_t x = *px;
    uint8_t a, pushed;

    for (;;) {                               /* CPYVYX */
        y++;
        ZP.R4 = ld_r0(y);                    /* low byte of the first word */
        y++;
        a = ld_r0(y);                        /* opcode byte */
        if (a >= 0xF0) {                     /* CPYVYX_1: short */
            a = (uint8_t)(a ^ ZP.R4);        /* (Z + XAS + X) ^ (OPCODE + YAS + Y) */
            a &= 0x07;                       /* X ^ Y */
            pushed = a;
            a = (uint8_t)(a ^ ld_r0(y));     /* X ^ Y ^ (OPCODE + YAS + Y) */
            a = (uint8_t)(a ^ TEMP1H);       /* sign correction */
            st_vg(y, a);                     /* (OPCODE + YAS + X) */
            y--;
            a = pushed;
            a = (uint8_t)(a ^ ld_r0(y));     /* (Z + XAS + X) ^ Y ^ X */
            a = (uint8_t)(a ^ TEMP1);
            st_vg(y, a);                     /* (Z + XAS + Y) */
            /* bcs CPYVYX_5 - always */
        } else {                             /* long: skip to the MSB of the X part */
            y++;
            y++;
            a = (uint8_t)(a ^ ld_r0(y));     /* (OPCODE + MSB Y) ^ (Z + MSB X) */
            a &= 0x0F;                       /* (MSB Y) ^ (MSB X) */
            ZP.R3 = a;
            a = (uint8_t)(a ^ ld_r0(y));     /* ... ^ (Z + MSB X) = Z + MSB Y */
            a = (uint8_t)(a ^ TEMP1);        /* apply sign change */
            st_vg(y, a);                     /* (Z + MSB Y) */
            y--;                             /* LS part of X */
            pushed = ld_r0(y);               /* pha */
            st_vg(y, ZP.R4);                 /* LS Y goes where LS X was */
            y--;                             /* the opcode byte */
            a = (uint8_t)(ZP.R3 ^ TEMP1H);
            a = (uint8_t)(a ^ ld_r0(y));     /* (OPCODE + MSB X) */
            st_vg(y, a);
            y--;                             /* first byte of the instruction */
            st_vg(y, pushed);                /* pla; LS X goes first */
            y++;
            y++;
        }
        /* CPYVYX_5 */
        y++;
        x--;
        if (x & 0x80)
            break;
    }
    *px = x;
    ad_vgadd(y);                             /* jmp VGADD */
    return y;
}

/* CPYVEC ($6246) proper: `ldy #$FF / bit R2 / bmi CPYVYX`. */
static uint8_t cpyvec(uint8_t *px)
{
    uint8_t y = 0xFF;

    if (ZP.R2 & 0x80)                        /* swap? */
        return cpyvyx(px, y);                /* yep */
    return cpyvec_10(px, y);
}

/* The cross-module form; X's exit value is not part of the published
 * contract. */
uint8_t ad_cpyvec(uint8_t x)
{
    return cpyvec(&x);
}

/* AYTOR0 ($6EB2) - R0/R1 += Y+1, then CPYVEC.  "Advance the source
 * cursor past what was just copied and copy the next chunk."
 *
 *     tya / sec / adc R0 / sta R0 / lda #0 / adc R1 / sta R1
 */
static uint8_t aytor0(uint8_t y, uint8_t *px)
{
    uint16_t t = (uint16_t)(y + 1 + ZP.R0);
    ZP.R0 = (uint8_t)t;
    ZP.R1 = (uint8_t)(ZP.R1 + (t >> 8));
    return cpyvec(px);                       /* jmp CPYVEC */
}

uint8_t ad_aytor0(uint8_t y, uint8_t x)
{
    return aytor0(y, &x);
}

/* ------------------------------------------------------------------ */
/* CPXROT ($6EE2) - object orientation index evaluator                  */
/* ------------------------------------------------------------------ */
/* A = angle 0..$FF.  Returns Y, an even word index 0..$10 into a table
 * of nine orientations (0-45 degrees in 4 steps of ~5.6 degrees, plus
 * the end point), and leaves R2 negative when X and Y must be
 * swapped, TEMP1 = the X sign mask, TEMP1+1 = the Y sign mask.
 * TABLE ($6F0A) holds one control byte per octant: bit 7 negate the
 * picture select, bit 5 swap, bit 1 X mask, bit 0 Y mask.
 *
 * The negate path is `eor #$0E / adc #$01` with carry still set from
 * the `asl` that tested bit 7, so it adds two: 14 - a + 2 = 16 - a,
 * which is how an index of $10 arises.  RSOURC has only eight words,
 * so ROTAST with Y = $10 reads the first word of SHLDVC; the ROM means
 * it and this port reproduces it (CALLING_NOTES.md section 9). */
uint8_t ad_cpxrot(uint8_t a)
{
    uint8_t y;
    bool c;

    a >>= 1;                                 /* throw away the low bit of the angle */
    ZP.R0 = a;                               /* save the residual */
    y = (uint8_t)(a >> 4);                   /* the half-quadrant number */
    a = ad_rom((uint16_t)(AD_TABLE + y));    /* operations code */
    c = (a & 0x80) != 0;                     /* asl a: negate bit into carry */
    a = (uint8_t)(a << 1);
    ZP.R2 = a;                               /* residual ops bits */
    a = (uint8_t)(ZP.R0 & 0x0E);             /* the portion within 45 degrees */
    if (c) {                                 /* negate 3 bits to 4 bits */
        a ^= 0x0E;
        a = (uint8_t)(a + 0x01 + 1);         /* adc #$01, carry set from above */
    }
    y = a;                                   /* CPXROT_1: pass it to the user */
    TEMP1 = (uint8_t)(ZP.R2 & 0x04);         /* X mask */
    ZP.R2 = (uint8_t)(ZP.R2 << 1);           /* swap bit into bit 7 */
    TEMP1H = (uint8_t)(ZP.R2 & 0x04);        /* Y mask */
    return y;
}

/* ------------------------------------------------------------------ */
/* CPTSPX ($6EC1) - point R0/R1 at the ship hull for ANGLE              */
/* ------------------------------------------------------------------ */
/* "Pictures are stored 90-135 so back up": the angle is shifted by
 * $40 before CPXROT.  SHIPS ($53BC) holds nine byte offsets, in words,
 * from SHIPSV ($53C6); the ROM doubles the offset and adds it to $53C6
 * across two bytes with the carries (`ldx #$53 ... inx ... adc #$C6 /
 * txa / adc #0`).  Exit X = 0: one vector to copy. */
void ad_cptspx(void)
{
    uint8_t a, y, x;
    uint16_t t;
    bool c;

    a = (uint8_t)(ZP.ANGLE[0] - 0x40);       /* sec / sbc #$40 */
    y = ad_cpxrot(a);                        /* Y comes back pointing to a word */
    y >>= 1;                                 /* fix it to point to a byte */
    x = 0x53;                                /* high byte of the ship vectors */
    a = ad_rom((uint16_t)(AD_SHIPS + y));    /* the offset */
    c = (a & 0x80) != 0;                     /* asl a */
    a = (uint8_t)(a << 1);
    if (c)                                   /* X = X + carry, then clc */
        x++;
    t = (uint16_t)(a + 0xC6);                /* CPTSPX_5: add the offset to the address */
    ZP.R0 = (uint8_t)t;
    ZP.R1 = (uint8_t)(x + (t >> 8));         /* txa / adc #0 */
    /* ldx #0: the caller's CPYVEC copies one vector.  TTST is the RTS. */
}

/* ------------------------------------------------------------------ */
/* SHLDPX ($669D) - the shield                                          */
/* ------------------------------------------------------------------ */
/* Three passes over SHLDVC ($5012): an invisible move to a corner of
 * the octagon, eight lit short vectors, and the move back.  The
 * intensity - SHLDS & $F0 with a floor of $60 - is smuggled into the
 * lit pass through TEMP1.  The last pass is a `jmp AYTOR0` with X as
 * the previous CPYVEC left it, $FF, which copies exactly one vector. */
void ad_shldpx(void)
{
    uint8_t a, x, y;

    if (!(ZP.SHDON & 0x80))                  /* shields up? */
        return;
    a = (uint8_t)(AD_P->f.SHLDS & 0xF0);
    if (a < 0x60)
        a = 0x60;                            /* a minimum */
    /* SHLDPX_1: A is pushed here and popped before the lit pass. */
    ZP.R0 = 0x12;                            /* (R0,R1) = SHLDVC */
    ZP.R1 = 0x50;
    x = 0;
    TEMP1 = 0;                               /* no inversions */
    TEMP1H = 0;
    ZP.R2 = 0;
    y = cpyvec(&x);                          /* invisible line to a corner */
    x = 0x07;                                /* 8 lines in the octagon */
    TEMP1 = a;                               /* fool CPYVEC into putting intensity */
    y = aytor0(y, &x);
    TEMP1 = 0;                               /* reset intensity */
    aytor0(y, &x);                           /* jmp AYTOR0, X = $FF from the loop above */
}

/* ------------------------------------------------------------------ */
/* SHPPIC ($6E3C) - the ship, and its flame                             */
/* ------------------------------------------------------------------ */
/* While SBTL is below $C0 the ship is materialising: SBTL steps by 6
 * and ten pairs of vectors from EXP16 ($4D88) are copied with random
 * inversion masks and each dot randomly lit or blanked - the twinkle.
 * Then CPTSPX finds the hull for ANGLE, one vector is copied with no
 * intensity change (the move out to the nose), and the rest of the
 * hull follows with the intensity OR'd into TEMP1.  THRTST adds the
 * two flame vectors when the thrust switch is down and FRAME bit 2 is
 * set, a 15 Hz flicker.
 *
 * The chained AYTOR0 calls run on whatever X the previous CPYVEC
 * exited with: 0 on the first, then $FF, $FE, ... - each copies one
 * vector, as the ROM intends.  Every RANDOM read is kept in order. */
void ad_shppic(void)
{
    uint8_t a, x, y;

    a = ZP.SBTL;
    if (a < 0xC0) {                          /* twinkle on the screen? */
        a = (uint8_t)(a + 0x06);             /* adc #6, carry clear */
        ZP.SBTL = a;
        TEMP1 = (uint8_t)(ad_hw_random() & 0x04);   /* random inversion bits */
        TEMP1H = (uint8_t)(ad_hw_random() & 0x04);
        ZP.R2 = (uint8_t)(ad_hw_random() & 0x80);
        ZP.R5 = 0x09;                        /* 10 pairs of vectors */
        ZP.R1 = 0x4D;                        /* R0,R1 = EXP16 - 1 */
        ZP.R0 = 0x87;
        y = 0x00;
        x = 0x00;
        for (;;) {                           /* SHPPIC_2 */
            y = aytor0(y, &x);               /* R0,R1 += Y+1, copy one vector */
            a = ad_hw_random();
            a = (a & 0x01) ? 0x70 : 0x00;    /* lsr a / lda #0 / bcc / lda #$70 */
            TEMP1 = (uint8_t)(a | TEMP1);    /* SHPPIC_4: merge with the inversion bit */
            y = aytor0(y, &x);
            TEMP1 = (uint8_t)(TEMP1 & 0x04); /* clear the intensity, keep the inversion bit */
            ZP.R5--;
            if (ZP.R5 & 0x80)
                break;
        }
        y = aytor0(y, &x);                   /* return the beam to centre at 0 intensity */
    }

    /* SHPPIC_1 */
    ad_cptspx();
    x = 0x00;
    y = cpyvec(&x);                          /* with no intensity change */
    a = (uint8_t)(ZP.SBTL & 0xF0);           /* the intensity */
    if (a < 0x60)
        a = 0x60;                            /* below which it can't be seen */
    TEMP1 = (uint8_t)(a | TEMP1);            /* SHPPIC_3: merge in the X inversion bit */
    x = 0x07;                                /* vector count - 1 */
    y = aytor0(y, &x);

    /* THRTST ($6EA5) */
    if (!(ad_hw_switch(0x2405) & 0x80))      /* bit THRUST: no thrust, exit */
        return;
    if (!(ZP.FRAME[0] & 0x04))               /* flicker at 15 Hz */
        return;
    x = 0x01;                                /* all thrusts are only 2 vectors */
    aytor0(y, &x);                           /* falls into AYTOR0 */
}

/* ------------------------------------------------------------------ */
/* POSBEM ($6C43) - position the beam, then wait                        */
/* ------------------------------------------------------------------ */
/* A LABS from XCOMP ($05..$08) at VGSIZE, followed by a wait of
 * $70 - VGSIZE.  The subtraction is eight-bit and VGSIZE can exceed
 * $70, so it wraps; the loop then emits waits of $90 and backs the
 * residue down by $10 until it drops below $A0.  Translated as the
 * ROM has it, wrap and all. */
void ad_posbem(void)
{
    uint8_t a;

    ad_vglabs(0x05);                         /* position the beam */
    a = (uint8_t)(0x70 - ZP.VGSIZE);         /* a wait of 7, backed down by the scale */
    for (;;) {                               /* POSBEM_28 */
        if (a < 0xA0)
            break;                           /* POSBEM_30 */
        ad_vgwait(0x90);                     /* insert a wait */
        a = (uint8_t)(a - 0x10);             /* back down again */
        if (a == 0)                          /* bne POSBEM_28 - "(ALWAYS)" */
            break;
    }
    ad_vgwait(a);                            /* insert a wait and exit */
}

/* ------------------------------------------------------------------ */
/* TRIPIX ($4A6D) - the special rock's picture                          */
/* ------------------------------------------------------------------ */
/* A is the OBJ byte already shifted left once by PICTUR_35 (that is
 * how the special bit got tested with `bpl`), so three shifts right
 * and a mask of $0F recover bits 5..2 of OBJ: the link to the
 * SRTIME/SRANG control entry.  SRTIME bit 0 selects the size bank
 * (the FRM wrappers for the big one), SRANG >> 2 one of 32
 * orientations, and the JSRL word comes from TFPIX ($502A). */
void ad_tripix(uint8_t a)
{
    uint8_t x, y;

    a >>= 3;
    a &= 0x0F;
    x = a;                                   /* point to the control byte */
    a = pg2_rd((uint16_t)(0xF8 + x));        /* SRTIME,x */
    /* lsr a / lda #0 / ror a / ror a: bit 0 lands in bit 6 */
    ZP.R1 = (uint8_t)((a & 0x01) << 6);
    a = pg2_rd((uint16_t)(0xF1 + x));        /* SRANG,x: the orientation */
    a >>= 2;                                 /* an index to 1 of 32 words */
    if (ZP.R1 & 0x40)                        /* big pix? */
        a &= 0x1E;                           /* yep, zap the MSB */
    a &= 0x3E;                               /* TRIPIX_1: zap bit 0 */
    a |= ZP.R1;                              /* index into the FRM JSRLs if big */
    y = a;
    x = ad_rom((uint16_t)(AD_TFPIX + 1 + y));   /* MSB of the JSRL */
    a = ad_rom((uint16_t)(AD_TFPIX + y));       /* LSB */
    ad_vgadd2(a, x);                         /* jmp VGADD2 */
}

/* ------------------------------------------------------------------ */
/* ROTAST ($4B92) - rewrite one rock subroutine in vector RAM           */
/* ------------------------------------------------------------------ */
/* FRAME & 3 picks which of the four rock subroutines to redraw and
 * which of ASTERS ($86..$89) is its rotation accumulator.  The
 * rotation rates are -2, -1, +1, +2: `clc / adc #$FE / adc ASTERS,x`
 * makes the second add take the carry out of the first, so rocks 2
 * and 3 get one more.  The accumulator is then folded into the angle
 * form CPXROT wants, the source list is taken from RSOURC ($5002) -
 * or, for an index of $10, from the first word of SHLDVC - and 13
 * vectors are copied to the address unpacked from ROCKSA's JSRL word.
 * Rocks 2 and 3 are mirrored by flipping TEMP1 with (x*2*2) & 4.
 * It may only run while the DVG is halted: the list is live. */
void ad_rotast(void)
{
    uint8_t a, x, y;
    uint16_t t;
    bool c;

    a = (uint8_t)(ZP.FRAME[0] & 0x03);
    x = a;
    t = (uint16_t)(a + 0xFE);                /* clc / adc #$FE: the rotation velocity */
    a = (uint8_t)t;
    t = (uint16_t)(a + ZP.ASTERS[x] + (t >> 8));   /* adc ASTERS,x - carry rides in */
    a = (uint8_t)t;
    ZP.ASTERS[x] = a;                        /* ACC = XXQQPPPX */
    a &= 0xF0;                               /* CPXROT needs QQQPPPXX */
    a = (uint8_t)(a + ZP.ASTERS[x]);         /* clc / adc ASTERS,x */
    a = (uint8_t)(a << 1);                   /* ACC = QQ0PPPXX */
    y = ad_cpxrot(a);                        /* index into the vector tables */
    ZP.R0 = ad_rom((uint16_t)(AD_RSOURC + y));       /* RSOURC,y - 9th word is SHLDVC */
    ZP.R1 = ad_rom((uint16_t)(AD_RSOURC + 1 + y));
    a = (uint8_t)(x << 1);                   /* rock number to a word index */
    x = a;
    a = (uint8_t)((a << 1) & 0x04);          /* the mirror bit, from the code */
    TEMP1 = (uint8_t)(a ^ TEMP1);
    a = ad_rom((uint16_t)(AD_ROCKSA + x));   /* LSB of the JSRL */
    c = (a & 0x80) != 0;
    a = (uint8_t)(a << 1);                   /* make it a byte address */
    ZP.VGLIST[0] = a;
    a = ad_rom((uint16_t)(AD_ROCKSA + 1 + x));
    a = (uint8_t)((a << 1) | (c ? 1 : 0));   /* rol a */
    a ^= 0xC0;                               /* drop bit 15 and set bit 14 */
    ZP.VGLIST[1] = a;
    x = 0x0C;                                /* count of vectors */
    cpyvec(&x);                              /* move them, modifying as required */
    ad_vgrtsl();                             /* an RTSL to end the subroutine */
}

/* ------------------------------------------------------------------ */
/* SHPEXP ($6D77) - the ship's fragments                                */
/* ------------------------------------------------------------------ */
/* SHPPIX counts $A0..$FF while the ship explodes.  On the first frame
 * (below $A2) eight fragment records are seeded from the ship's
 * position: SXPXL/SXPYL/SXPXH/SXPYH ($0100..) hold the positions,
 * EXPTMB ($0120) the spin rate, EXPDX/EXPDY ($E5/$ED) the velocities,
 * EXPANG ($F5) the orientation.  Every frame each fragment moves,
 * spins, gets the beam positioned by POSBEM and has one vector copied
 * from the fragment frames at TFPIX+$40 ($506A) with the intensity
 * fading as SHPPIX climbs. */

/* SHPEXP_150 ($6E31): a random step in -4..-1, 1..4.
 *     lda RANDOM / and #7 / clc / adc #$FC / adc #0 */
static uint8_t shpexp_150(void)
{
    uint16_t t = (uint16_t)((ad_hw_random() & 0x07) + 0xFC);
    return (uint8_t)((uint8_t)t + (t >> 8));   /* adc #0 folds the carry in */
}

/* SHPEXP_100 ($6E12): advance one axis of fragment X and hand the
 * halved position to VGLABS through $07/$08.  Called with X for the X
 * axis and X+8 for Y, so EXPDX+8 is EXPDY and SXPXL+8 is SXPYL:
 * index the raw pages, as the ROM does.  Returns A = $08. */
static uint8_t shpexp_100(uint8_t x)
{
    uint8_t a, y;
    uint16_t t;

    y = 0x00;
    a = g.zp.raw[0xE5 + x];                  /* EXPDX,x: the direction */
    if (a & 0x80)
        y--;                                 /* sign extend */
    /* SHPEXP_101 */
    t = (uint16_t)(a + g.pg1.raw[0x00 + x]); /* clc / adc SXPXL,x */
    a = (uint8_t)t;
    g.pg1.raw[0x00 + x] = a;
    XCOMP2 = a;                              /* pass it to VGLABS here */
    t = (uint16_t)(y + g.pg1.raw[0x10 + x] + (t >> 8));   /* tya / adc SXPXH,x */
    a = (uint8_t)(t & 0x07);                 /* roll over top and sides */
    g.pg1.raw[0x10 + x] = a;
    XCOMP3 = (uint8_t)(a >> 1);              /* lsr a / sta $08 */
    XCOMP2 = (uint8_t)((XCOMP2 >> 1) | ((a & 0x01) << 7));   /* ror $07 */
    return XCOMP3;
}

void ad_shpexp(void)
{
    uint8_t a, x, pushed;
    bool c;

    a = AD_P->f.SHPPIX;
    if (a < 0xA2) {                          /* the first explosion frame */
        x = 0x07;                            /* number of vectors in the ship picture */
        for (;;) {                           /* SHPEXP_5 */
            uint8_t r;

            r = (uint8_t)(ad_hw_random() & 0x07);
            a = (uint8_t)(r ^ XCOMP0);       /* apply it to the position */
            c = (a & 0x80) != 0;
            a = (uint8_t)(a << 1);           /* scale it */
            g.pg1.f.SXPXL[x] = a;            /* X LSB */
            a = (uint8_t)((XCOMP1 << 1) | (c ? 1 : 0));   /* rol a */
            g.pg1.f.SXPXH[x] = a;            /* X MSB */

            r = (uint8_t)(ad_hw_random() & 0x07);
            a = (uint8_t)(r ^ XCOMP2);       /* add it to the position */
            c = (a & 0x80) != 0;
            a = (uint8_t)(a << 1);           /* scale it */
            g.pg1.f.SXPYL[x] = a;            /* Y LSB */
            a = (uint8_t)((XCOMP3 << 1) | (c ? 1 : 0));   /* rol a */
            c = (XCOMP3 & 0x80) != 0;        /* the carry that rol leaves */
            g.pg1.f.SXPYH[x] = a;            /* Y MSB */

            r = (uint8_t)(ad_hw_random() & 0x0F);
            a = (uint8_t)(r + 0xF8 + (c ? 1 : 0));   /* adc #$F8, carry from the rol */
            g.pg1.f.EXPTMB[x] = a;
            ZP.EXPDX[x] = shpexp_150();      /* X velocity */
            ZP.EXPDY[x] = shpexp_150();      /* Y velocity */
            x--;
            if (x & 0x80)
                break;                       /* dex / bpl SHPEXP_5 */
        }
    }

    /* SHPEXP_10 */
    ZP.R2 = 0x07;                            /* no X/Y flip required */
    ZP.R5 = 0x07;                            /* loop count */
    for (;;) {                               /* SHPEXP_20 */
        x = ZP.R5;                           /* vector counter */
        a = AD_P->f.SHPPIX;                  /* $A0..$FF */
        c = (a == 0);                        /* COMP: eor #$FF / clc / adc #1 - carry only from 0 */
        a = ad_comp(a);                      /* 60 - 0 in steps of 1 */
        a &= 0xF0;                           /* 60 - 0 in steps of $10 */
        {
            uint16_t t = (uint16_t)(a + 0x60 + (c ? 1 : 0));   /* adc #$60: $C0 - $60 */
            a = (uint8_t)t;
            c = (t >> 8) != 0;
        }
        TEMP1 = a;                           /* the intensity */
        a = (uint8_t)(ZP.EXPANG[x] + g.pg1.f.EXPTMB[x] + (c ? 1 : 0));   /* adc: update it */
        ZP.EXPANG[x] = a;
        a >>= 2;                             /* the code */
        pushed = a;                          /* pha: for the frame lookup */
        a >>= 3;                             /* put it in bit 2 */
        a &= 0x04;                           /* isolate bit 2 */
        TEMP1H = a;                          /* the Y inversion bit */
        TEMP1 = (uint8_t)(a | TEMP1);        /* merged with the intensity */
        a = shpexp_100(x);                   /* compute X */
        XCOMP1 = a;                          /* move it back to the X slot */
        XCOMP0 = XCOMP2;
        x = (uint8_t)(x + 0x08);             /* skip up to the Y parameters */
        shpexp_100(x);                       /* compute them */
        ad_posbem();                         /* position the beam */
        a = (uint8_t)(pushed & 0x1E);        /* pla: isolate the pix code */
        x = a;
        a = ad_rom((uint16_t)(AD_TFPIX + 0x40 + x));    /* the frame routine's JSRL */
        c = (a & 0x80) != 0;
        a = (uint8_t)(a << 1);               /* unpack it to a byte address */
        ZP.R0 = a;
        a = ad_rom((uint16_t)(AD_TFPIX + 0x41 + x));
        a = (uint8_t)((a << 1) | (c ? 1 : 0));          /* rol a */
        a ^= 0xC0;                           /* drop bit 15 and set bit 14 */
        ZP.R1 = a;
        x = 0x00;
        cpyvec(&x);                          /* move in one vector */
        ZP.R5--;                             /* count down */
        if (ZP.R5 & 0x80)
            break;
    }
}

/* ------------------------------------------------------------------ */
/* PICTUR ($6BC4) - display an object's picture                         */
/* ------------------------------------------------------------------ */
/* Y = the DVG scale, X = the object slot, XCOMP..XCOMP+3 = its 16-bit
 * position.  The position is turned into beam coordinates - X right
 * three, Y right two, plus 128, then right one more - POSBEM places
 * the beam, and PICTUR_31 appends the picture: an explosion JSRL from
 * EXPPIC by size, the ship's fragments, the shield and ship, the
 * saucer's fixed JSRL, a dot for a torpedo (which also ages the
 * torpedo every fourth frame), the special rock through TRIPIX, or a
 * rock subroutine from ROCKSA.  X is restored from TEMP3 on the way
 * out: "the following code always returns here". */
static void pictur_31(uint8_t x)
{
    uint8_t a, y;

    a = pg2_rd(x);                           /* OBJ,x */
    if (a & 0x80) {                          /* exploding */
        if (x == 0x19) {                     /* PICTUR_33: the ship */
            ad_shpexp();
            return;
        }
        a &= 0x0C;
        a >>= 1;                             /* 0, 2, 4 or 6 */
        y = a;
        ad_vgadd2(ad_rom((uint16_t)(AD_EXPPIC + y)),          /* PICTUR_78 */
                  ad_rom((uint16_t)(AD_EXPPIC + 1 + y)));
        return;
    }
    /* PICTUR_35 */
    if (x == 0x19) {                         /* PICTUR_50: the ship */
        ad_shldpx();                         /* the shield, if enabled */
        ad_shppic();                         /* jmp SHPPIC */
        return;
    }
    if (x == 0x1A) {                         /* PICTUR_60: the saucer */
        ad_vgadd2(0x34, 0xC7);
        return;
    }
    if (x > 0x1A) {                          /* PICTUR_70: a torpedo, a dot */
        ad_vgdot(0x70, 0xF0);
        x = TEMP3;
        if ((ZP.FRAME[0] & 0x03) == 0)       /* every fourth frame */
            pg2_wr(x, (uint8_t)(pg2_rd(x) - 1));   /* dec OBJ,x: the active count */
        return;                              /* PICTUR_75 */
    }
    a = (uint8_t)(a << 1);
    if (a & 0x80) {                          /* a special rock */
        ad_tripix(a);                        /* jmp TRIPIX */
        return;
    }
    /* PICTUR_37 */
    a >>= 3;
    a &= 0x06;
    y = a;
    ad_vgadd2(ad_rom((uint16_t)(AD_ROCKSA + y)),              /* PICTUR_78 */
              ad_rom((uint16_t)(AD_ROCKSA + 1 + y)));
}

void ad_pictur(uint8_t y, uint8_t x)
{
    uint16_t t;

    ZP.VGSIZE = y;                           /* the scaling factor */
    TEMP3 = x;                               /* save X */
    /* lsr $06 / ror XCOMP, three times: X to integer form */
    t = (uint16_t)(XCOMP0 | (XCOMP1 << 8));
    t >>= 3;
    XCOMP0 = (uint8_t)t;
    XCOMP1 = (uint8_t)(t >> 8);
    /* lsr $08 / ror $07, twice */
    t = (uint16_t)(XCOMP2 | (XCOMP3 << 8));
    t >>= 2;
    XCOMP2 = (uint8_t)t;
    XCOMP3 = (uint8_t)(t >> 8);
    XCOMP3++;                                /* inc $08: add 128 (range 127 < Y < 897) */
    t = (uint16_t)(XCOMP2 | (XCOMP3 << 8));  /* lsr $08 / ror $07 */
    t >>= 1;
    XCOMP2 = (uint8_t)t;
    XCOMP3 = (uint8_t)(t >> 8);
    ad_posbem();                             /* position the beam */
    x = TEMP3;
    pictur_31(x);
    /* ldx TEMP3: X is a register, restored for the caller; in C the
     * caller kept its own copy. */
}
