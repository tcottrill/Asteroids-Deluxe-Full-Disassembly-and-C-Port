/* Vector list construction - DSTCUT.MAC, $79E8-$7B10.
 *
 * These build a real DVG display list in g.vram, byte for byte as the
 * ROM does.  That is deliberate: it is the strongest differential test
 * available, since the generated vector RAM can be compared against a
 * hardware or emulator dump directly.  Several of the quirks below only
 * matter for that comparison - see VGHALT.
 */
#include "astdelux.h"

/* VGLIST ($03/$04) is a 16-bit pointer into vector RAM. */
static inline uint16_t vglist(void)
{
    return (uint16_t)(g.zp.f.VGLIST[0] | (g.zp.f.VGLIST[1] << 8));
}

/* Write one byte at VGLIST + off.  Vector RAM is $4000-$47FF; the ROM
 * relies on that being the only place VGLIST ever points. */
static void vg_poke(uint8_t off, uint8_t v)
{
    uint16_t a = (uint16_t)(vglist() + off);
    if (a >= 0x4000 && a < 0x4000 + AD_VRAM_SIZE)
        g.vram[a - 0x4000] = v;
}

/* VGADD ($7A67) - advance VGLIST by Y+1.
 *
 *     tya / sec / adc VGLIST      the SEC is what makes it Y+1
 */
void ad_vgadd(uint8_t y)
{
    uint16_t v = (uint16_t)(vglist() + y + 1);
    g.zp.f.VGLIST[0] = (uint8_t)v;
    g.zp.f.VGLIST[1] = (uint8_t)(v >> 8);
}

/* VGADD2 ($7CCC) - store A,X as a word, then advance two. */
void ad_vgadd2(uint8_t a, uint8_t x)
{
    vg_poke(0, a);
    vg_poke(1, x);
    ad_vgadd(1);
}

/* VGHALT ($79EC) and VGRTSL ($79E8) share a tail at VGHAL1 ($79EE) that
 * stores the *same byte twice*:
 *
 *     lda #$B0 / ldy #0 / sta (VGLIST),y / iny / sta (VGLIST),y
 *
 * so a HALT goes into vector RAM as B0 B0, not 00 B0.  The DVG only
 * looks at the top nibble of the high byte, so the low byte is a
 * don't-care to the hardware - but writing the tidy 00 B0 would fail a
 * byte-for-byte comparison against the real machine.  Kept as-is.
 */
static void vghal1(uint8_t v)
{
    vg_poke(0, v);
    vg_poke(1, v);
    ad_vgadd(1);
}

void ad_vghalt(void) { vghal1(0xB0); }
void ad_vgrtsl(void) { vghal1(0xD0); }

/* VGJSRL ($7A2A) - append a call to a shape.
 *
 * The ROM is handed the shape's byte address split across A (high) and
 * X (low), and halves the pair in place:
 *
 *     lsr a / and #$0F / ora #$C0     high byte, opcode C
 *     txa / ror a                     low byte, taking the shifted bit
 *
 * which is the divide-by-two that turns a byte address into a DVG word
 * address.  Expressed here on the whole address; the $1FFF mask is the
 * one DSTVEC.MAC's JSRL macro applies.
 */
void ad_vgjsrl(uint16_t shape)
{
    uint16_t word = (uint16_t)(0xC000 | ((shape & 0x1FFF) >> 1));
    ad_vgadd2((uint8_t)word, (uint8_t)(word >> 8));
}

/* VGSABS ($7A31) into VGLABS ($7A4A) - set an absolute beam position.
 *
 * VGSABS scales both components by four into the XCOMP scratch area
 * ($05..$08), then falls into VGLABS with X=5, which assembles:
 *
 *     byte0 = $07                         Y low
 *     byte1 = ($08 & 0x0F) | 0xA0         Y high, opcode A
 *     byte2 = $05                         X low
 *     byte3 = ($06 & 0x0F) | VGSIZE       X high, plus the global scale
 *
 * The scale being OR'd in from VGSIZE rather than passed is why a
 * caller changes VGSIZE before positioning.
 */
void ad_vgsabs(uint8_t x, uint8_t y)
{
    uint16_t xc = (uint16_t)(x << 2);       /* asl / rol twice */
    uint16_t yc = (uint16_t)(y << 2);

    g.zp.f.XCOMP[0] = (uint8_t)xc;          /* $05 */
    g.zp.f.XCOMP[1] = (uint8_t)(xc >> 8);   /* $06 */
    g.zp.f.XCOMP[2] = (uint8_t)yc;          /* $07 */
    g.zp.f.XCOMP[3] = (uint8_t)(yc >> 8);   /* $08 */

    vg_poke(0, g.zp.f.XCOMP[2]);
    vg_poke(1, (uint8_t)((g.zp.f.XCOMP[3] & 0x0F) | 0xA0));
    vg_poke(2, g.zp.f.XCOMP[0]);
    vg_poke(3, (uint8_t)((g.zp.f.XCOMP[1] & 0x0F) | g.zp.f.VGSIZE));
    ad_vgadd(3);
}

/* VGCHAR ($7A0A) - append one character.
 *
 * Y is the character index times two.  It indexes VGMSGA ($56F8), a
 * table of already-formed JSRL words, and hands the pair straight to
 * VGADD2.  An index of $4A or more is replaced by 0, which is the
 * blank: 37 glyphs, space then 0-9 then A-Z.
 */
void ad_vgchar(uint8_t y)
{
    if (y >= 0x4A)
        y = 0;
    ad_vgadd2(ad_rom(AD_VGMSGA + y), ad_rom(AD_VGMSGA + 1 + y));
}

/* VGLABS ($7A4A) - append a LABS from a four-byte zero-page block.
 *
 * X is the zero-page *address* of the block, laid out XL, XH, YL, YH,
 * and every load is indexed by that absolute address:
 *
 *     lda VGBRIT,x        $02+X = YL          byte0
 *     lda VGLIST,x        $03+X = YH          byte1 = (YH & $0F) | $A0
 *     lda $00,x           $00+X = XL          byte2
 *     lda VGSIZE,x        $01+X = XH          byte3 = (XH & $0F) | VGSIZE
 *
 * The only direct caller, POSBEM, passes X = 5 (XCOMP); VGSABS falls in
 * with the same X, which is why ad_vgsabs above assembles the identical
 * four bytes.  Falls into VGADD with Y = 3.
 */
void ad_vglabs(uint8_t zp)
{
    const uint8_t *z = g.zp.raw;

    vg_poke(0, z[(uint8_t)(zp + 2)]);
    vg_poke(1, (uint8_t)((z[(uint8_t)(zp + 3)] & 0x0F) | 0xA0));
    vg_poke(2, z[zp]);
    vg_poke(3, (uint8_t)((z[(uint8_t)(zp + 1)] & 0x0F) | g.zp.f.VGSIZE));
    ad_vgadd(3);
}

/* VGHEX1 ($7A02) - the shared tail of VGHEX and VGHEXZ: A is a glyph
 * index, doubled for VGCHAR.  The ROM wraps the call in php/plp so the
 * carry (the zero-suppression state) survives VGCHAR; here the callers
 * hold that in a bool, so there is nothing to save. */
static void vghex1(uint8_t a)
{
    ad_vgchar((uint8_t)(a << 1));
}

/* VGHEX ($79FD) - append one hex digit.
 *
 *     and #$0F / clc / adc #$01       glyph = digit + 1, glyph 0 is the blank
 *
 * so 0-9 land on the digit glyphs and A-F on the letters A-F.  Leaves
 * C = 0, which is what VGHEXZ relies on. */
void ad_vghex(uint8_t a)
{
    vghex1((uint8_t)((a & 0x0F) + 1));
}

/* VGHEXZ ($79F7) - VGHEX with leading-zero suppression.
 *
 *     bcc VGHEX                       not suppressing: plain digit, C = 0
 *     and #$0F / beq VGHEX1           a zero while suppressing: glyph 0
 *                                     (the blank), and C stays set
 *
 * Returns the carry: still suppressing only if it was, and the digit
 * was 0. */
bool ad_vghexz(uint8_t a, bool suppress)
{
    if (!suppress) {
        ad_vghex(a);
        return false;
    }
    a &= 0x0F;
    if (a == 0) {
        vghex1(0);                      /* LEAVE C SET */
        return true;
    }
    ad_vghex(a);
    return false;
}

/* VGDOT ($7AFE) - a WAIT with an intensity, which the DVG draws as a
 * point at the current position.  A is the timer, X the intensity, and
 * the four bytes go in out of order:
 *
 *     ldy #1 / sta (VGLIST),y         byte1 = timer
 *     dey / tya / sta (VGLIST),y      byte0 = 0
 *     iny / iny / sta (VGLIST),y      byte2 = 0
 *     iny / txa / sta (VGLIST),y      byte3 = intensity
 */
void ad_vgdot(uint8_t a, uint8_t x)
{
    vg_poke(1, a);
    vg_poke(0, 0);
    vg_poke(2, 0);
    vg_poke(3, x);
    ad_vgadd(3);
}

/* VGWAIT ($7AFC) - `ldx #0` and fall into VGDOT: a dot with no
 * intensity is a plain wait. */
void ad_vgwait(uint8_t a)
{
    ad_vgdot(a, 0);
}

/* VGAWT ($7AF7) - position the beam, then `lda #$70` and fall into
 * VGWAIT.  Self-test only. */
void ad_vgawt(uint8_t x4, uint8_t y4)
{
    ad_vgsabs(x4, y4);
    ad_vgwait(0x70);
}

/* VGBLNK ($7CC5) - a digit followed by a space: `jsr VGHEX`, then the
 * VGSPAC JSRL word ($0A,$CB - the same word VGMSGA holds for the blank)
 * falls into VGADD2.  Self-test only. */
void ad_vgblnk(uint8_t a)
{
    ad_vghex(a);
    ad_vgadd2(0x0A, 0xCB);
}

/* DIGITS ($7C98) - display a multi-byte BCD number, MSB first.
 *
 * Entry: C = 1 to suppress leading zeros, A = zero-page address of the
 * least significant byte, Y = number of bytes.  The ROM converts that
 * into the address of the *most* significant byte and a count-down:
 *
 *     stx $1D                         parks the caller's X in TEMP4+2;
 *                                     nothing reads it back, so it is
 *                                     not modelled
 *     dey / sty $1C                   TEMP4+1 = bytes - 1
 *     clc / adc $1C / sta TEMP4       TEMP4 = address of the MSB
 *
 * then for each byte the high nibble goes to VGHEXZ, the suppression
 * carry is cleared when TEMP4+1 has reached 0 (the last byte, so a
 * value of zero still shows a "0"), and the low nibble follows.
 * `dec $1C / bpl` ends the loop when the count goes negative. */
void ad_digits(bool suppress, uint8_t zp, uint8_t y)
{
    g.zp.f.TEMP4[1] = (uint8_t)(y - 1);
    g.zp.f.TEMP4[0] = (uint8_t)(zp + g.zp.f.TEMP4[1]);
    do {
        uint8_t x = g.zp.f.TEMP4[0];            /* tax, then ldx TEMP4 */

        suppress = ad_vghexz((uint8_t)(g.zp.raw[x] >> 4), suppress);
        if (g.zp.f.TEMP4[1] == 0)
            suppress = false;                   /* DISPLAY LAST DIGIT (EVEN 0) */
        x = g.zp.f.TEMP4[0];
        suppress = ad_vghexz(g.zp.raw[x], suppress);
        g.zp.f.TEMP4[0]--;
        g.zp.f.TEMP4[1]--;
    } while (!(g.zp.f.TEMP4[1] & 0x80));       /* bpl DIGITS_10 */
}

/* VGVCTR ($7A72) and VGJMPL ($7A19) are never called by the shipped
 * ROM (only the GONOGO diagnostic links them) and are not translated. */
