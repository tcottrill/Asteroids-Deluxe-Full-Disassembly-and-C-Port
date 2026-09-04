/* Messages - DSTMSG.MAC, $714E-$752B, and the copyright check at
 * $77F5-$784E.
 *
 * Text lives in the ROM packed three characters to two bytes, five bits
 * each, and ASTMSG unpacks it straight into the display list as one
 * JSRL per glyph.  The same unpacker doubles as the copy-protection
 * check: with CPMTST negative it XORs the glyph words it *would* have
 * written against what is already in vector RAM and accumulates the
 * difference in CRMERR (VGRCPT, called from UPDATE).  Since the port
 * models vector RAM and SETROL builds the list being checked, CRMERR
 * stays 0 here just as it does on an unmodified board.
 *
 * The packing, from the rol/lsr sequence in ASTMSG ($71C1-$71E0), for
 * a byte pair b0 b1:
 *
 *     char 1 = b0 bits 7-3
 *     char 2 = b0 bits 2-0, then b1 bits 7-6
 *     char 3 = b1 bits 5-1
 *     b1 bit 0 set  = last pair
 *     code 0        = end, wherever it falls
 *
 * and VGMSG2 maps a code to VGMSGA: 1 = blank, 2/3/4 = 0/1/2, 5..30 =
 * A..Z, 31 = the 38th word (the copyright glyph).
 */
#include "astdelux.h"

/* `lda (TEMP1,x)` with X = 0: the next byte of packed text. */
static uint8_t text_byte(void)
{
    return ad_rom((uint16_t)(g.zp.f.TEMP1[0] | (g.zp.f.TEMP1[1] << 8)));
}

/* CPR.UP ($7835) - fold one expected byte into the error flag:
 *
 *     eor (VGLIST),y / ora CRMERR / sta CRMERR / iny
 */
static void cpr_up(uint8_t a, uint8_t *y)
{
    uint8_t have = ad_vram_rd((uint16_t)(ad_vglist() + *y));

    g.zp.f.CRMERR = (uint8_t)(g.zp.f.CRMERR | (a ^ have));
    (*y)++;
}

/* VGMSG2 ($71EC) - emit one character.  A is the code times two; Y is
 * the running offset from VGLIST.
 *
 *     and #$3E / bne VGMSG2_5         code 0 ends the message: the ROM
 *                                     does pla/pla to discard ASTMSG's
 *                                     jsr and lands in VGMSG0.  Returned
 *                                     here as true, and ASTMSG breaks.
 *     cmp #$0A / bcc / adc #$0D       codes 5 and up skip the digits
 *                                     3-9: +$0E, the carry being set
 *     lda $56F6,x                     VGMSGA - 2, so code 1 is word 0
 *     bit CPMTST / bmi VGMSG2_12      test mode compares instead
 *
 * X is left 0 on the normal exit (VGMSG2_13) because ASTMSG uses it as
 * the (TEMP1,x) index; it is a constant here. */
static bool vgmsg2(uint8_t a, uint8_t *y)
{
    uint8_t x;
    uint8_t lo;

    a &= 0x3E;
    if (a == 0)
        return true;                            /* PURGE RTS */
    if (a >= 0x0A)
        a = (uint8_t)(a + 0x0E);                /* SET CORRECT INDEX */
    x = a;
    lo = ad_rom((uint16_t)(AD_VGMSGA - 2 + x)); /* 10. FOR A, 12. FOR B, ... */
    if (g.zp.f.CPMTST & 0x80) {
        cpr_up(lo, y);
        cpr_up(ad_rom((uint16_t)(AD_VGMSGA - 1 + x)), y);
    } else {
        ad_vram_wr((uint16_t)(ad_vglist() + *y), lo);
        (*y)++;
        ad_vram_wr((uint16_t)(ad_vglist() + *y),
                   ad_rom((uint16_t)(AD_VGMSGA - 1 + x)));
        (*y)++;
    }
    return false;
}

/* VGMSG1 ($71E6) - step the text pointer, then VGMSG2. */
static bool vgmsg1(uint8_t a, uint8_t *y)
{
    if (++g.zp.f.TEMP1[0] == 0)
        g.zp.f.TEMP1[1]++;
    return vgmsg2(a, y);
}

/* ASTMSG ($71BD) - unpack the text at TEMP1/TEMP1+1 into the list.
 *
 * Three characters per pair of bytes.  The loop's two exits both reach
 * VGMSG0 ($71E2): `lsr TEMP2 / bcc` at the end of a pair, or the
 * pla/pla in VGMSG2 on a code 0.  VGMSG0 is `dey / jmp VGADD`, which
 * moves VGLIST past the Y bytes written.
 *
 * The middle character is assembled by a rotate chain that leaves
 * TEMP2 changed on the way:
 *
 *     lda (TEMP1,x)       b1
 *     rol a               C = b1.7
 *     rol TEMP2           TEMP2 = b0<<1 | b1.7, C = b0.7
 *     rol a               (C = b1.6; A is then discarded)
 *     lda TEMP2
 *     rol a               A = b0<<2 | b1.7<<1 | b1.6
 *     asl a               A = b0<<3 | b1.7<<2 | b1.6<<1
 *
 * so after the `and #$3E` in VGMSG2 the code is b0 bits 2-0 above b1
 * bits 7-6.  The carry rotated in at the first step falls off the top
 * before the mask, whatever VGMSG2's cmp left it as. */
static void astmsg(void)
{
    uint8_t y = 0;                              /* Y IS INDEX FOR VGLIST */

    for (;;) {
        uint8_t b;
        uint8_t a;

        b = text_byte();
        g.zp.f.TEMP2[0] = b;
        if (vgmsg1((uint8_t)(b >> 2), &y))      /* 2*INDEX */
            break;

        b = text_byte();
        g.zp.f.TEMP2[0] = (uint8_t)((g.zp.f.TEMP2[0] << 1) | (b >> 7));   /* rol TEMP2 */
        a = (uint8_t)((g.zp.f.TEMP2[0] << 1) | ((b >> 6) & 0x01));         /* lda TEMP2 / rol a */
        a = (uint8_t)(a << 1);                                              /* asl a */
        if (vgmsg2(a, &y))
            break;

        b = text_byte();
        g.zp.f.TEMP2[0] = b;
        if (vgmsg1(b, &y))
            break;

        g.zp.f.TEMP2[0] >>= 1;                  /* lsr TEMP2 */
        if (b & 0x01)
            break;                              /* END OF LIST */
    }

    /* VGMSG0 */
    y--;
    ad_vgadd(y);
}

/* CM ($71B5) - position the beam, wait for it, then unpack.  A and X
 * are the position over 4, as VGSABS takes them. */
static void cm(uint8_t a, uint8_t x)
{
    ad_vgsabs(a, x);                            /* POSITION BEAM */
    ad_vgwait(0x70);                            /* WAIT FOR BEAM */
    astmsg();
}

/* VGME ($7198) - display message Y in language A.
 *
 *     asl a / tax                     2*LANGUAGE
 *     lda VGMSGT+1,x / sta TEMP1+1
 *     lda VGMSGT,x / sta TEMP1        the language block
 *     adc (TEMP1),y                   plus that block's offset for Y;
 *                                     "CARRY IS CLEAR FROM ASL ABOVE",
 *                                     so the add takes bit 7 of A
 *     sta TEMP1 / bcc / inc TEMP1+1
 *
 * then the message's screen position from VGMSGS, and CM. */
void ad_vgme(uint8_t a, uint8_t y)
{
    uint8_t  x = (uint8_t)(a << 1);
    uint8_t  carry = (uint8_t)(a >> 7);
    uint16_t sum;

    g.zp.f.TEMP1[1] = ad_rom((uint16_t)(AD_VGMSGT + 1 + x));
    g.zp.f.TEMP1[0] = ad_rom((uint16_t)(AD_VGMSGT + x));
    sum = (uint16_t)(g.zp.f.TEMP1[0]
                     + ad_rom((uint16_t)((g.zp.f.TEMP1[0] | (g.zp.f.TEMP1[1] << 8)) + y))
                     + carry);
    g.zp.f.TEMP1[0] = (uint8_t)sum;
    if (sum > 0xFF)
        g.zp.f.TEMP1[1]++;

    /* VGME_10 */
    y = (uint8_t)(y << 1);
    cm(ad_rom((uint16_t)(AD_VGMSGS + y)), ad_rom((uint16_t)(AD_VGMSGS + 1 + y)));
}

/* VGMSG ($718F) - display message Y in the language the option
 * switches select: OPTN4 ($2803) bits 0-1.  Medium characters. */
void ad_vgmsg(uint8_t y)
{
    uint8_t a = (uint8_t)(ad_hw_switch(0x2803) & 0x03);

    g.zp.f.VGSIZE = 0x10;                       /* MEDIUM SIZED CHARACTERS */
    ad_vgme(a, y);
}

/* SETROL ($714E) - rotate the four rock subroutines, then build the
 * copyright display list at $4702, which VGRCPT later checks against.
 * FRAME is the loop counter, which is why INIT warns that it is
 * cleared.  VGSIZE is left 0. */
void ad_setrol(void)
{
    g.zp.f.FRAME[0] = 4;
    do {
        ad_rotast();
    } while (--g.zp.f.FRAME[0] != 0);

    ad_set_vglist(0x4702);                      /* POINT TO VG RAM */
    ad_vgadd2(0xC1, 0xC9);                      /* JSRL <LABS,CIRCLR,CIRCLP> */
    g.zp.f.TEMP1[0] = (uint8_t)(AD_ASTM & 0xFF);
    g.zp.f.TEMP1[1] = (uint8_t)(AD_ASTM >> 8);
    g.zp.f.VGSIZE = 0;
    cm(0x70, 0x20);                             /* UNPACK ATARI, INC */
    ad_vgrtsl();                                /* FOLLOW WITH A RTSL */
}

/* CPYRS ($717E) - the copyright line on the high-score screen: a JSRL
 * to the circled-C shape, then ASTM unpacked at the current beam
 * position (`bne ASTMSG`, always taken). */
void ad_cpyrs(void)
{
    ad_vgadd2(0xC5, 0xC9);                      /* JSRL TO CIRCLC,CIRCLP */
    g.zp.f.TEMP1[0] = (uint8_t)(AD_ASTM & 0xFF);
    g.zp.f.TEMP1[1] = (uint8_t)(AD_ASTM >> 8);
    astmsg();
}

/* VGRCPT ($77F5) - the copyright check, run by UPDATE with CPMTST set
 * negative around the call.
 *
 * With VGLIST saved and pointed at $4702, CPR.UP compares the JSRL
 * SETROL wrote, then CPR.DT's eight bytes *backwards* (`ldx #7 / dex /
 * bpl`) against the LABS and WAIT that CM produced, then `dey / jsr
 * VGADD` steps VGLIST past those ten bytes and ASTMSG, in test mode,
 * compares the glyph words for ASTMT - a second copy of the packed
 * text, at $7845 - against those SETROL unpacked from ASTM.  Any
 * difference sticks in CRMERR. */
void ad_vgrcpt(void)
{
    uint8_t save_lo;
    uint8_t save_hi;
    uint8_t y = 0;
    uint8_t x;

    if (!(g.zp.f.CPMTST & 0x80))
        return;                                 /* SHOULD WE TEST?  NO */

    /* VGRCPT_1 */
    save_lo = g.zp.f.VGLIST[0];                 /* SAVE VGLIST */
    save_hi = g.zp.f.VGLIST[1];
    ad_set_vglist(0x4702);
    cpr_up(0xC1, &y);                           /* FIX IT */
    cpr_up(0xC9, &y);
    x = 7;
    do {                                        /* VGRCPT_2 */
        cpr_up(ad_rom((uint16_t)(AD_CPRDT + x)), &y);
        x--;
    } while (!(x & 0x80));
    y--;
    ad_vgadd(y);                                /* ADD Y+1 TO VGLIST */
    g.zp.f.TEMP1[0] = (uint8_t)(AD_ASTMT & 0xFF); /* SET UP TEMP1,TEMP1+1 */
    g.zp.f.TEMP1[1] = (uint8_t)(AD_ASTMT >> 8);
    astmsg();                                   /* CHECK MESSAGE */
    g.zp.f.VGLIST[1] = save_hi;                 /* RESTORE VGLIST */
    g.zp.f.VGLIST[0] = save_lo;
}
