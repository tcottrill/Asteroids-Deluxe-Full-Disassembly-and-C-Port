/* The object engine - DSTRD0.MAC's MOTION, COLIDE, DSTRCT, SPLIT,
 * NEWAST and their helpers, plus BOUNCE.MAC.
 *
 * Every object is a slot 0..32 in the parallel arrays on the player
 * page (OBJ, XINC, YINC, OBJXH, OBJYH, OBJXL, OBJYL, $21 apart).  The
 * code below indexes AD_P->raw with the listing's own offsets so the
 * ROM's habit of running one array into the next - SHPXI,x with x=$21
 * is SHPYI - carries over unchanged.
 *
 * Several routines here depend on the carry flag a *previous* routine
 * left behind: COLIDE's size accumulator adds the carry from its cpx/
 * cpy compares, BOUNCE_4 adds whatever carry reached it, and BNC's
 * "2*ROCK (APPROX)" rotates a stale carry into bit 0.  Those are
 * tracked in `c` variables and passed along, not idealised.
 *
 * Contracts: disasm/CALLING_NOTES.md sections 0, 1, 3, 7, 10, 12.
 */
#include "astdelux.h"

#define ZP     (g.zp.f)
#define PG     (AD_P->raw)
#define OBJ(i)   PG[AD_OBJ + (i)]
#define XINC(i)  PG[AD_XINC + (i)]
#define YINC(i)  PG[AD_YINC + (i)]
#define OBJXH(i) PG[AD_OBJXH + (i)]
#define OBJYH(i) PG[AD_OBJYH + (i)]
#define OBJXL(i) PG[AD_OBJXL + (i)]
#define OBJYL(i) PG[AD_OBJYL + (i)]
/* SHPPIX,x etc. are OBJ+$19,x: the ship's slot plus x. */
#define SHPPIX(x) OBJ(AD_SHIP + (x))
#define SHPXI(x)  XINC(AD_SHIP + (x))
#define SHPYI(x)  YINC(AD_SHIP + (x))
#define SHPXH(x)  OBJXH(AD_SHIP + (x))
#define SHPYH(x)  OBJYH(AD_SHIP + (x))
#define SHPXL(x)  OBJXL(AD_SHIP + (x))
#define SHPYL(x)  OBJYL(AD_SHIP + (x))

#define TEMP1   ZP.TEMP1[0]
#define TEMP1H  g.zp.raw[0x0A]
#define TEMP2   ZP.TEMP2[0]
#define TEMP3   ZP.TEMP3[0]
#define TEMP3H  g.zp.raw[0x0F]
#define TEMP4   ZP.TEMP4[0]
#define TEMP4A  g.zp.raw[0x1C]
#define TEMP4B  g.zp.raw[0x1D]

/* ------------------------------------------------------------------ */
/* NEWVE1 ($6A7E) - range a velocity                                    */
/* ------------------------------------------------------------------ */
/* Positive results are held to RVELP..$1F, negative ones to $E1..RVELM
 * (RVELM is negative, so "cmp RVELM / bcc" keeps anything more
 * negative than it).  The carry the routine leaves is what BOUNCE's
 * halving path hands on to BNC, so it is returned too:
 *
 *     cmp #$20 / bcc RTS.2          C = 0 unless clamped to $1F
 *     cmp RVELM / bcc RTS.2         C = 0 unless clamped to RVELM
 */
static uint8_t newve1(uint8_t a, bool *c)
{
    if (!(a & 0x80)) {                      /* POSITIVE RESULT */
        if (a < ZP.RVELP)                   /* NOT TOO CLOSE TO ZERO? */
            a = ZP.RVELP;
        /* NEWVE1_25 */
        if (a < 0x20) { *c = false; return a; }   /* WITHIN RANGE */
        *c = true;
        return 0x1F;
    }
    if (a < 0xE1)                           /* WITHIN RANGE? */
        a = 0xE1;
    /* NEWVE1_15 */
    if (a < ZP.RVELM) { *c = false; return a; }   /* NOT TOO CLOSE TO ZERO */
    *c = true;
    return ZP.RVELM;                        /* AT LEAST 1/2 */
}

uint8_t ad_newve1(uint8_t a)
{
    bool c;
    return newve1(a, &c);
}

/* ------------------------------------------------------------------ */
/* NEWVEL ($6A57) - new random velocity using old                       */
/* ------------------------------------------------------------------ */
/* X gets Y's velocity plus a random -16..+15 on each axis, ranged.
 *
 *     lda RANDOM / and #$8F / bpl / ora #$F0     0..15, or -1..-16
 */
void ad_newvel(uint8_t x, uint8_t y)
{
    uint8_t a;

    a = (uint8_t)(ad_hw_random() & 0x8F);
    if (a & 0x80)
        a |= 0xF0;                          /* -1 TO -16 */
    a = (uint8_t)(a + XINC(y));             /* clc / adc XINC,y */
    XINC(x) = ad_newve1(a);                 /* CHECK RANGE OF VELOCITIES */
    a = (uint8_t)(ad_hw_random() & 0x8F);
    if (a & 0x80)
        a |= 0xF0;
    a = (uint8_t)(a + YINC(y));
    YINC(x) = ad_newve1(a);
}

/* ------------------------------------------------------------------ */
/* NEWSHP ($6A37) - the ship to the centre, still, with full shields    */
/* ------------------------------------------------------------------ */
void ad_newshp(void)
{
    SHPXL(0) = 0x60;                        /* POSITION IN MIDDLE - JUST OFF CENTER */
    SHPYL(0) = 0x60;
    SHPXI(0) = 0x00;                        /* WITH NO VELOCITY */
    SHPYI(0) = 0x00;
    SHPXH(0) = 0x10;
    SHPYH(0) = 0x0C;
    AD_P->f.SHLDS = 0xFF;                   /* GIVE 'EM FULL SHIELDS */
}

/* ------------------------------------------------------------------ */
/* RNDPOS ($6A03) - a random large rock at a screen edge                */
/* ------------------------------------------------------------------ */
/* X = slot to fill, Y = slot whose velocity seeds NEWVEL.  The second
 * RANDOM byte's bit 0 picks the axis; bits 1-5 the position along it. */
void ad_rndpos(uint8_t x, uint8_t y)
{
    uint8_t a;
    bool c;

    a = (uint8_t)((ad_hw_random() & 0x38) | 0x04);   /* PICTURE NUMBER, SIZE */
    OBJ(x) = a;                             /* SET PICTURE */
    ad_newvel(x, y);                        /* GET NEW VELOCITY */
    a = ad_hw_random();
    c = (a & 0x01) != 0;                    /* lsr a */
    a = (uint8_t)((a >> 1) & 0x1F);
    if (!c) {                               /* RNDPOS_50: START ON X AXIS */
        OBJXH(x) = a;
        OBJYH(x) = 0x00;
        OBJYL(x) = 0x00;
        return;
    }
    if (a >= 0x18)                          /* START ON Y AXIS: IF 0 TO 767 */
        a &= 0x17;                          /* 384 TO 512 */
    /* RNDPOS_35 */
    OBJYH(x) = a;
    OBJXH(x) = 0x00;
    OBJXL(x) = 0x00;
}

/* ------------------------------------------------------------------ */
/* SEARCH ($6D6C) / SEARC1 ($6D6E) - a free rock slot, from X down      */
/* ------------------------------------------------------------------ */
/* Returns the slot, or $FF; the ROM's callers test N. */
uint8_t ad_searc1(uint8_t x)
{
    for (;;) {
        if (OBJ(x) == 0)                    /* FOUND ONE */
            return x;
        x--;
        if (x & 0x80)                       /* LOOP TIL EXHAUSTED */
            return x;
    }
}

uint8_t ad_search(void)
{
    return ad_searc1(0x18);
}

/* ------------------------------------------------------------------ */
/* CPYPOS ($6210) - copy attributes of rock Y into slot X               */
/* ------------------------------------------------------------------ */
/* Size bits come across; the picture bits are re-rolled from RANDOM,
 * which is why SPLTTP saves OBJ around it ("CPYPOS BLITZES 'OBJ'"). */
uint8_t ad_cpypos(uint8_t x, uint8_t y)
{
    uint8_t a;

    TEMP1 = (uint8_t)(OBJ(y) & 0x07);       /* SAVE SIZE */
    a = (uint8_t)((ad_hw_random() & 0x18) | TEMP1);   /* PICTURE NUMBER */
    OBJ(x) = a;
    OBJXL(x) = OBJXL(y);                    /* COPY POSITION */
    OBJXH(x) = OBJXH(y);
    OBJYL(x) = OBJYL(y);
    OBJYH(x) = OBJYH(y);
    XINC(x) = XINC(y);                      /* COPY VELOCITY */
    YINC(x) = YINC(y);
    return a;
}

/* ------------------------------------------------------------------ */
/* SRESET ($66CF) - soft reset: both player pages cleared               */
/* ------------------------------------------------------------------ */
void ad_sreset(void)
{
    AD_P->f.DIFCTY = 0x06;
    for (int i = 0; i < 0x100; i++) {
        AD_P->raw[i] = 0;                   /* sta OBJ,x */
        AD_P3->raw[i] = 0;                  /* sta $0300,x */
    }
}

/* ------------------------------------------------------------------ */
/* NEWAST ($698E) - start up new asteroids                              */
/* ------------------------------------------------------------------ */
void ad_newast(void)
{
    uint8_t x = 0x18, y;

    if (AD_P->f.RDELAY != 0)                /* still waiting: NEWAST_65 */
        goto clear;
    /* NEWAST_66 */
    if (ZP.NPLAYR != 0) {                   /* not attract mode */
        if (SHPPIX(0) == 0 || (SHPPIX(0) & 0x80))
            return;                         /* NOT WHILE SHIP IS INVISIBLE OR BLOWING UP */
    }
    /* NEWAST_11 */
    if (OBJ(AD_SAUCER) != 0)                /* SAUCER? YES. NO NEW ROCK */
        return;
    AD_P->f.SPROCK = 0;                     /* MAKE SURE THERE ARE NO SPECIALS */
    ZP.TRILE[ZP.PLAYR] = 0;                 /* ENABLE DIAMOND LAUNCH */
    for (x = 6; !(x & 0x80); x--)           /* NEWAST_20 */
        AD_P->f.SRTIME[x] = 0;
    x = AD_P->f.SROCKS;
    if (x < 0x3F) {
        AD_P->f.SROCKS++;                   /* NEWAST_14 */
    } else if (ZP.NPLAYR == 0) {
        ad_sreset();
        AD_P->f.SROCKS++;
    }
    /* NEWAST_15 */
    if (x >= 0x03)
        x = 0x03;
    y = ad_rom((uint16_t)(AD_RWAVE + x));   /* NEWAST_21 */
    /* NEWAST_5 */
    AD_P->f.NROCKS = y;                     /* ACTIVATE ROCKS */
    TEMP1 = y;                              /* KEEP A COPY */
    x = 0x18;
    if (AD_P->f.DIFCTY < 0x0A)              /* MINIMUM # OF ROCKS BEFORE MR. BILL */
        AD_P->f.DIFCTY++;
    /* NEWAST_2 */
    y = AD_SAUCER;                          /* POINT TO SAUCER VELOCITY */
    do {                                    /* NEWAST_10 */
        ad_rndpos(x, y);                    /* GET A RANDOM POSITION */
        x--;
    } while (--TEMP1 != 0);                 /* LOOP FOR EACH NEW ROCK */
    AD_P->f.EDELAY = 0x7F;                  /* SAUCER SHOULD WAIT */
    AD_P->f.THUMP3 = 0x30;                  /* RESET THUMP SOUND */
clear:
    /* NEWAST_65 / NEWAST_70: clear the rest of the rock slots */
    for (; !(x & 0x80); x--)
        OBJ(x) = 0;                         /* ASSUMES SROCKS < NOBJ */
}

/* ------------------------------------------------------------------ */
/* SSBTLT ($6325), BLOUP ($62FC) and BLOUP_10 ($631D)                   */
/* ------------------------------------------------------------------ */
void ad_ssbtlt(uint8_t a)
{
    AD_P->f.SDELAY = a;
    ZP.SBTLT = 0x05;
}

/* BLOUP - blow up slot Y.  The explosion sound's pitch comes from the
 * size:
 *
 *     and #$03 / eor #$02           2 = small, 3 = medium, 0 = large
 *     lsr a / ror a / ror a         bit 0 to bit 6, bit 1 to bit 7
 *     ora #$3F                      LENGTH OF EXPLOSION
 */
uint8_t ad_bloup(uint8_t y)
{
    uint8_t a = (uint8_t)((OBJ(y) & 0x03) ^ 0x02);
    bool c;

    c = (a & 0x01) != 0;  a >>= 1;                              /* lsr a */
    { bool c2 = (a & 0x01) != 0; a = (uint8_t)((a >> 1) | (c ? 0x80 : 0)); c = c2; }  /* ror a */
    a = (uint8_t)((a >> 1) | (c ? 0x80 : 0));                   /* ror a */
    ZP.LEXPSND = (uint8_t)(a | 0x3F);
    OBJ(y) = 0xA0;                          /* TIMER FOR EXPLOSION; STOP OBJECTS MOTION */
    XINC(y) = 0;
    YINC(y) = 0;
    return 0xA0;
}

/* BLOUP_10 - the ship crashed: take a life, and SSBTLT($81). */
static void bloup_10(void)
{
    ZP.HITS[ZP.PLAYR]--;                    /* TAKE A LIFE */
    ad_ssbtlt(0x81);                        /* DELAY BEFORE RENTRY */
}

/* ------------------------------------------------------------------ */
/* BOUNCE.MAC - shields                                                 */
/* ------------------------------------------------------------------ */

/* BNC_30 ($75F6) - 16-bit compare of rock Y's position against the
 * ship's (SHPXL,x / SHPXH,x; x = $21 for the Y axis).  Returns the
 * carry: 0 if the ship is on the right of (above) the rock. */
static bool bnc_30(uint8_t x, uint8_t y)
{
    uint16_t o = (uint16_t)(OBJXL(y) | (OBJXH(y) << 8));
    uint16_t s = (uint16_t)(SHPXL(x) | (SHPXH(x) << 8));
    return o >= s;
}

/* BOUNCE_4 ($754C) - `txa / adc SHLDS`, with whatever carry reached it.
 * Below zero the shields go off for the next collision. */
static void bounce_4(uint8_t x, bool c)
{
    unsigned s = (unsigned)x + AD_P->f.SHLDS + (c ? 1u : 0u);
    if (!(s & 0x100)) {                     /* CAN'T GO MINUS */
        s = 0;
        ZP.SHDON = 0;                       /* TURN THEM OFF FOR NEXT COLLISION */
    }
    AD_P->f.SHLDS = (uint8_t)s;             /* BOUNCE_5 */
}

/* BNC ($75AC) - bounce the ship off rock Y on one axis.  X = 0 for X,
 * $21 for Y (with Y already offset by $21 too); R0 = the size/special
 * code BOUNCE_6 saved; `c` = the carry on entry, which BNC_10's
 * `rol a` uses.  Returns the carry on exit (BOUNCE_14 adds it). */
static bool bnc(uint8_t x, uint8_t y, bool c)
{
    uint8_t a;
    uint8_t xinc = XINC(y);
    bool to25;

    ZP.R1 = x;                              /* SAVE X */
    if (!(xinc & 0x80)) {
        if (SHPXI(x) & 0x80) {
            to25 = false;                   /* bmi BNC_22 */
        } else {
            c = bnc_30(x, y);               /* SHIP/ROCK BOTH MOVING RIGHT (UP) */
            to25 = c;                       /* bcc BNC_22: ROCK OVERTOOK SHIP */
        }
    } else {                                /* BNC_20 */
        if (!(SHPXI(x) & 0x80)) {
            to25 = false;                   /* bpl BNC_22 */
        } else {
            c = bnc_30(x, y);               /* BOTH MOVING LEFT (DOWN) */
            to25 = !c;                      /* bcc BNC_25: SHIP OVERTOOK ROCK */
        }
    }
    if (to25) {                             /* BNC_25: SHIP = -ROCK */
        a = (uint8_t)(0 - xinc);            /* lda #0 / sec / sbc XINC,y */
        c = (xinc == 0);
    } else {                                /* BNC_22: SHIP = 2*ROCK */
        a = xinc;
        if (ZP.R0 & 0x80) {                 /* SPECIAL: SHIP = 1.5*ROCK */
            unsigned s;
            c = (a >= 0x80);                /* cmp #$80: C = sign */
            a = (uint8_t)((a >> 1) | (c ? 0x80 : 0));   /* ror a: ASR */
            s = (unsigned)a + xinc + (c ? 1u : 0u);     /* adc XINC,y */
            c = (s & 0x100) != 0;
            a = (uint8_t)s;
            c = (a & 0x01) != 0;            /* lsr a: EASY TO FALL THRU */
            a >>= 1;
        }
        /* BNC_10: rol a - the carry is whatever the path above left */
        {
            bool c2 = (a & 0x80) != 0;
            a = (uint8_t)((a << 1) | (c ? 1 : 0));
            c = c2;
        }
    }
    /* BNC_26: range it through MOVE2 with X = R1 as the low byte */
    a = (uint8_t)(ad_move2((uint16_t)((a << 8) | ZP.R1)) >> 8);
    if (a & 0x80) {
        c = (a >= 0xFB);                    /* NOT TOO CLOSE TO 0 */
        if (c)
            a = 0xFA;
    } else {                                /* BNC_1 */
        c = (a >= 0x06);
        if (!c)
            a = 0x06;
    }
    /* BNC_2 */
    x = ZP.R1;                              /* RESTORE X */
    SHPXI(x) = a;
    return c;
}

enum bounce_exit {
    BOUNCE_NORMAL,                          /* C = 0: carry on destroying */
    BOUNCE_SAUCER,                          /* C = 1: award points, blow up saucer */
    BOUNCE_ABSORBED                         /* pla/pla: back to COLIDE */
};

/* BOUNCE ($752C) - see if the shields take this collision.
 *
 * X = attacker (0 ship, 1 saucer, 2-3 saucer torpedoes, 4-7 ship
 * torpedoes), Y = target slot.  When something hits the ship the roles
 * are swapped so Y names the attacker (`adc #$18` with C = 1 from
 * `cpy #$19`).  Three exits, and the ship-rams-saucer case also
 * rewrites X and Y for DSTRCT, so both come back through pointers. */
static enum bounce_exit bounce(uint8_t *px, uint8_t *py)
{
    uint8_t x = *px, y = *py, a;
    bool c;

    if (!(ZP.SHDON & 0x80))                 /* SHIELDS ON? NOPE */
        return BOUNCE_NORMAL;
    if (x != 0) {                           /* not the ship hitting something */
        if (y != AD_SHIP)                   /* NOT SHIP. DO NORMAL DESTRUCTION */
            return BOUNCE_NORMAL;
        y = (uint8_t)(x + 0x18 + 1);        /* adc #$18 (C=1): RELATIVE TO OBJ */
        *py = y;
    }
    /* BOUNCE_2 */
    a = OBJ(y);                             /* GET OBJECT SIZE */
    if (y == AD_SAUCER) {                   /* BOUNCE_3: SAUCER */
        if (0x80 < AD_P->f.SHLDS) {         /* GOT ENUF? YEP */
            bounce_4(0x80, false);          /* BOUNCE_7: C = 0 from the bcc */
            return BOUNCE_SAUCER;           /* AWARD POINTS / BLOW UP SAUCER */
        }
        *px = 0x00;                         /* POINT TO SHIP */
        *py = AD_SAUCER;                    /* POINT TO SAUCER: BLOW 'EM BOTH UP */
        return BOUNCE_NORMAL;
    }
    if (y > AD_SAUCER) {                    /* TORPEDO */
        OBJ(y) = 0x00;                      /* ABSORB TORPEDOES */
        bounce_4(0xE1, true);               /* AT THE EXPENSE OF SOME SHIELDS;
                                             * C = 1 from `cpy #$1A` */
        return BOUNCE_ABSORBED;             /* BOUNCE_14 */
    }
    /* BOUNCE_6: a rock */
    ZP.CHIST[0] = (uint8_t)(0x80 | (ZP.CHIST[0] >> 1));   /* sec / ror CHIST: SAY SHIP HIT ROCK */
    if (y == ZP.CHIST[1]) {                 /* HIT THE SAME THING AS LAST TIME? */
        bounce_4(0xF8, true);               /* YEP. DON'T BOUNCE BUT TAKE SHIELD POWER */
        return BOUNCE_ABSORBED;
    }
    ZP.CHIST[1] = y;                        /* KEEP COLLISION HISTORY */
    c = (a & 0x80) != 0;                    /* asl a: C = OBJ bit 7, clear for a live rock */
    a = (uint8_t)((a << 1) & 0x86);         /* LARGE OR SPECIAL? */
    ZP.R0 = a;                              /* SAVE CODE */
    if (!(a & 0x80) && a != 0) {            /* small or medium: CUT ROCK SPEED IN HALF */
        uint8_t v = XINC(y);
        c = (v >= 0x80);                    /* SET CARRY TO MATCH SIGN */
        v = (uint8_t)((v >> 1) | (c ? 0x80 : 0));        /* ror a: ASR */
        XINC(y) = newve1(v, &c);            /* RANGE IT */
        v = YINC(y);
        c = (v >= 0x80);
        v = (uint8_t)((v >> 1) | (c ? 0x80 : 0));
        YINC(y) = newve1(v, &c);
    }
    /* BOUNCE_9 */
    c = bnc(0x00, y, c);                    /* BOUNCE OFF X */
    c = bnc(0x21, (uint8_t)(y + 0x21), false);   /* clc / adc #$21: BOUNCE OFF Y */
    bounce_4(0xB0, c);                      /* GET SHIELDS LOSS */
    return BOUNCE_ABSORBED;                 /* BOUNCE_14 */
}

/* ------------------------------------------------------------------ */
/* DSTRCT ($62C6) - destruction during collision                        */
/* ------------------------------------------------------------------ */
/* X = attacker (0 ship, 1 saucer, 2-7 torpedoes), Y = target slot.
 * Written with the listing's labels as gotos, because the ROM's own
 * structure is a small state machine that falls into BLOUP at the end
 * of most paths. */
static void dstrct(uint8_t x, uint8_t y)
{
    uint8_t a;
    bool c;

    switch (bounce(&x, &y)) {               /* SEE IF WE SHOULD BOUNCE */
    case BOUNCE_ABSORBED: return;           /* straight back to COLIDE */
    case BOUNCE_SAUCER:   goto ssbtlt_80;   /* BLOW UP SAUCER */
    case BOUNCE_NORMAL:   break;
    }
    if (x == 0x01) {                        /* SAUCER HITTING SHIP OR ROCKS */
        /* DSTRCT_1 */
        if (y != AD_SHIP)
            goto dstrct_62;                 /* SAUCER HIT ROCK */
        x = 0x00;                           /* dex */
        y = AD_SAUCER;                      /* iny: Y=NOBJ+1 */
    }
    /* DSTRCT_60 */
    if (x != 0)
        goto dstrct_63;                     /* IF NOT SHIP HIT A ROCK */
    bloup_10();                             /* SHIP CRASHED. TAKE A LIFE */
dstrct_62:
    SHPPIX(x) = 0xA0;                       /* EXPLOSION TIMER */
    SHPXI(x) = 0x00;                        /* STOP SHIP/SAUCER/TORPEDO */
    SHPYI(x) = 0x00;
    /* DSTRCT_61 */
    if (y < AD_SHIP)
        goto dstrct_65;                     /* SHIP OR SAUCER HITTING ROCK */
    goto ssbtlt_80;                         /* SHIP & SAUCER COLLIDE */
dstrct_63:
    SHPPIX(x) = 0x00;                       /* CLEAR TORPEDO */
    if (y == AD_SHIP) {                     /* HIT SHIP WITH TORPEDO: BLOUP_75 */
        bloup_10();
        ad_bloup(y);
        return;
    }
    if (y > AD_SHIP)
        goto ssbtlt_80;                     /* HIT SAUCER WITH TORPEDO */
dstrct_65:
    ad_split(y, x);                         /* SPLIT UP ROCKS */
    ad_bloup(y);                            /* falls into BLOUP */
    return;

ssbtlt_80:
    AD_P->f.EDELAY = AD_P->f.SEDLAY;        /* DELAY BEFORE ENTERING SAUCER */
    if (ZP.NPLAYR == 0) {                   /* IF IN ATTRACT */
        ad_bloup(y);
        return;
    }
    c = (OBJ(AD_SAUCER) & 0x01) != 0;       /* lda SAUPIX / lsr a: small? */
    /* SSBTLT_86 */
    a = 0x00;                               /* 1000 FOR SMALL */
    if (!c)
        a = 0x20;                           /* 200 POINTS FOR LARGE SAUCER */
    /* SSBTLT_85 */
    ad_points(a, c);                        /* ADD POINTS AND CHECK FOR 10K */
    ad_bloup(y);
}

/* ------------------------------------------------------------------ */
/* SPLIT ($6F7E) - split rock Y, hit by X                               */
/* ------------------------------------------------------------------ */
void ad_split(uint8_t y, uint8_t x)
{
    uint8_t a;

    TEMP3 = x;                              /* SAVE X */
    AD_P->f.RTIMER = 0x50;                  /* RESET TIMER IF ROCK HAS BEEN HIT */
    /* SPLIT_1 */
    a = (uint8_t)(OBJ(y) & 0x78);           /* SAVE PICTURE */
    if (a & 0x40) {                         /* asl a / bpl: a special rock */
        ad_splttp(y);                       /* jmp SPLTTP */
        return;
    }
    TEMP3H = a;                             /* asl / lsr: the picture bits, $0F */
    a = (uint8_t)(OBJ(y) & 0x07);
    x = (uint8_t)(a >> 1);                  /* the next size down */
    a = x;
    if (a != 0)                             /* EMPTY NOW? */
        a |= TEMP3H;
    /* SPLIT_10 */
    OBJ(y) = a;                             /* NEW PICTURE & SIZE */
    if (ZP.NPLAYR != 0) {                   /* not in attract */
        if (TEMP3 == 0 || TEMP3 >= 0x04) {  /* SHIP DOES IT, or a ship torpedo */
            /* SPLIT_15 */
            ad_points(ad_rom((uint16_t)(AD_HITSCR + x)), false);   /* SCORE FOR HIT */
        }                                   /* NO POINTS IF SAUCER DOES IT */
    }
    /* SPLIT_20 */
    if (OBJ(y) == 0)                        /* DISAPPEARED */
        goto split_90;
    x = ad_search();                        /* SEARCH FOR NEW ENTRY */
    if (x & 0x80)
        goto split_90;                      /* NO MORE ENTRIES */
    AD_P->f.NROCKS++;
    ad_cpypos(x, y);                        /* COPY POSITION FOR NEW ENTRY */
    ad_newvel(x, y);                        /* NEW VELOCITY */
    OBJXL(x) ^= (uint8_t)((XINC(x) & 0x1F) << 1);   /* PREVENT OVERLAPPING ROCKS */
    x = ad_searc1(x);                       /* LOOK FOR NEW ENTRY */
    if (x & 0x80)
        goto split_90;                      /* NO MORE ROOM */
    AD_P->f.NROCKS++;
    ad_cpypos(x, y);                        /* COPY POSITION & PICTURE + VELOCITY */
    ad_newvel(x, y);                        /* NEW VELOCITY USING OLD */
    OBJYL(x) ^= (uint8_t)((YINC(x) & 0x1F) << 1);
split_90:
    (void)TEMP3;                            /* ldx TEMP3: X is the caller's */
}

/* ------------------------------------------------------------------ */
/* COLIDE ($610E) - collision detector                                  */
/* ------------------------------------------------------------------ */

/* SSZ ($61FA) - the ship's size, added to A with the carry in hand:
 *     bit SHDON / bpl / adc #$08      shields up: slightly larger
 *     adc #$1C                        SHIP SIZE
 */
static uint8_t ssz(uint8_t a, bool c)
{
    unsigned s;
    if (ZP.SHDON & 0x80) {
        s = (unsigned)a + 0x08 + (c ? 1u : 0u);
        c = (s & 0x100) != 0;
        a = (uint8_t)s;
    }
    s = (unsigned)a + 0x1C + (c ? 1u : 0u);
    return (uint8_t)s;
}

/* SCRSZ ($6203) - the saucer's size: $1C, plus $12 if it is the large
 * one (SAUPIX bit 0 clear).  The first adc takes the carry in hand. */
static uint8_t scrsz(uint8_t a, bool c)
{
    unsigned s = (unsigned)a + 0x1C + (c ? 1u : 0u);
    a = (uint8_t)s;
    if (!(OBJ(AD_SAUCER) & 0x01))           /* lda SAUPIX / lsr a / bcs: LARGE */
        a = (uint8_t)(a + 0x12);            /* adc #$12 - C = 0 from the lsr */
    return a;
}

/* COLIDE_21..COLIDE_38: does object Y hit ship-group member X?
 * Returns true on a hit (after DSTRCT has run). */
static bool colide_21(uint8_t x, uint8_t y, uint8_t obj)
{
    uint8_t a, hi;
    unsigned s;
    int t;
    bool c;

    TEMP2 = obj;
    /* Quick test: the high bytes must differ by -2..+2.  `sec / sbc
     * SHPXH,x / sbc #$03 / cmp #$FA` normalises that to $FA..$FF. */
    t = (int)OBJXH(y) - SHPXH(x);           /* sec / sbc */
    c = t >= 0;
    t = (t & 0xFF) - 0x03 - (c ? 0 : 1);    /* sbc #$03 */
    c = t >= 0;
    a = (uint8_t)t;
    if (a < 0xFA)                           /* NOPE. TOO FAR. SKIP IT */
        return false;
    /* cmp left C = 1 */
    t = (int)OBJYH(y) - SHPYH(x);           /* sbc, C = 1 */
    c = t >= 0;
    t = (t & 0xFF) - 0x03 - (c ? 0 : 1);
    a = (uint8_t)t;
    if (a < 0xFA)
        return false;

    /* TEST X DIRECTION: 16-bit difference, halved, high byte must be
     * 0/1 (within 64) or $FE/$FF. */
    t = (int)OBJXL(y) - SHPXL(x);           /* sec / sbc */
    c = t >= 0;
    TEMP1 = (uint8_t)t;
    t = (int)OBJXH(y) - SHPXH(x) - (c ? 0 : 1);
    hi = (uint8_t)t;
    c = (hi & 0x01) != 0;                   /* lsr a */
    hi >>= 1;
    TEMP1 = (uint8_t)((TEMP1 >> 1) | (c ? 0x80 : 0));   /* ror TEMP1 */
    c = (hi & 0x80) != 0;                   /* asl a: C = 0 here, hi <= $7F */
    hi = (uint8_t)(hi << 1);
    if (hi != 0) {                          /* not WITHIN 64 */
        if (!(hi & 0x80))
            return false;                   /* TOO FAR AWAY */
        if ((uint8_t)(hi ^ 0xFE) != 0)
            return false;                   /* TOO FAR AWAY */
        /* negative: `eor #$FF / adc #$00` - the carry is the asl's,
         * which is clear, so this is the one's complement: the
         * "(CARRY SET FROM ABOVE)" comment is stale.  DISTANCE-1. */
        TEMP1 = (uint8_t)((TEMP1 ^ 0xFF) + (c ? 1 : 0));
    }
    /* COLIDE_25: the same for Y into TEMP1+1 */
    t = (int)OBJYL(y) - SHPYL(x);
    c = t >= 0;
    TEMP1H = (uint8_t)t;
    t = (int)OBJYH(y) - SHPYH(x) - (c ? 0 : 1);
    hi = (uint8_t)t;
    c = (hi & 0x01) != 0;
    hi >>= 1;
    TEMP1H = (uint8_t)((TEMP1H >> 1) | (c ? 0x80 : 0));
    c = (hi & 0x80) != 0;
    hi = (uint8_t)(hi << 1);
    if (hi != 0) {
        if (!(hi & 0x80))
            return false;
        if ((uint8_t)(hi ^ 0xFE) != 0)
            return false;
        TEMP1H = (uint8_t)((TEMP1H ^ 0xFF) + (c ? 1 : 0));   /* DISTANCE-1 BETWEEN OBJECTS */
    }

    /* COLIDE_35: the size accumulator.  The carry from `cpx #$01`
     * feeds the first adc in SSZ/SCRSZ. */
    a = 0x04;
    if (x == 0x01) {
        a = scrsz(a, true);                 /* SAUCER: cpx equal, C = 1 */
    } else if (x < 0x01) {
        a = ssz(a, false);                  /* SHIP: C = 0 */
    }                                       /* TORPEDO: nothing */
    /* COLIDE_41: cpy #$19 - the carry it leaves feeds the next adc */
    if (y < AD_SHIP) {                      /* ROCK */
        a = (uint8_t)(a + 0x2A);            /* SMALLEST ROCK/ENEMY SIZE, C = 0 */
        c = (TEMP2 & 0x01) != 0;  TEMP2 >>= 1;
        if (!c) {                           /* not SMALL */
            a = (uint8_t)(a + 0x1E);        /* adc, C = 0 from the lsr */
            c = (TEMP2 & 0x01) != 0;  TEMP2 >>= 1;
            if (!c)                         /* not MEDIUM */
                a = (uint8_t)(a + 0x3C);    /* LARGE */
        }
    } else if (y == AD_SHIP) {              /* COLIDE_44: SHIP, C = 1 */
        a = ssz(a, true);
    } else {                                /* COLIDE_43: SAUCER/SNOWFLAKE, C = 1 */
        a = scrsz(a, true);
    }
    /* COLIDE_38 */
    if (a < TEMP1)                          /* NO HIT */
        return false;
    if (a < TEMP1H)                         /* NO HIT */
        return false;
    /* cmp left C = 1 (a >= TEMP1+1) */
    TEMP2 = a;                              /* COMPUTE 1.5 * ABS(MINIMUM DIFF) */
    s = (unsigned)(a >> 1) + TEMP2;         /* lsr a / clc / adc TEMP2 */
    c = (s & 0x100) != 0;
    a = (uint8_t)s;
    { bool c2 = (a & 0x01) != 0; a = (uint8_t)((a >> 1) | (c ? 0x80 : 0)); c = c2; }   /* ror a: SCALE IT */
    TEMP2 = a;                              /* 3/2 DISTANCE */
    s = (unsigned)TEMP1H + TEMP1 + (c ? 1u : 0u);   /* COMPUTE ABS(DIFFX) + ABS(DIFFY) */
    c = (s & 0x100) != 0;
    a = (uint8_t)s;
    a = (uint8_t)((a >> 1) | (c ? 0x80 : 0));       /* ror a: SCALE IT */
    if (a >= TEMP2)                         /* CHOP OFF CORNERS: MISSED */
        return false;
    dstrct(x, y);                           /* BLOW 'EM UP; X is saved round it */
    return true;
}

void ad_colide(void)
{
    uint8_t x, y, a;

    ZP.CHIST[0] >>= 1;                      /* CLEAR SHIP COLLISION FLAG */
    for (x = 0x07; !(x & 0x80); x--) {      /* COLIDE_10 / COLIDE_13 */
        a = SHPPIX(x);
        if (a == 0 || (a & 0x80))           /* INACTIVE, or EXPLODING */
            continue;
        /* COLIDE_15 */
        y = AD_SAUCER;
        if (x < 0x04) {                     /* SHIP, SAUCER OR SAUCER'S TORPEDOES */
            y--;                            /* BYPASS SAUCER TO SAUCER COLLISION */
            if (x == 0)
                y--;                        /* SHIP DOESN'T COLLIDE WITH SHIP */
        }
        for (;;) {                          /* COLIDE_20 */
            uint8_t obj = OBJ(y);
            if (obj != 0 && !(obj & 0x80)) {    /* active, not an explosion */
                if (colide_21(x, y, obj))
                    break;                  /* COLIDE_39: ldy #0 / dey: END OF LOOP */
            }
            /* COLIDE_19 */
            if (y == 0)
                break;
            y--;
        }
    }
    /* x is $FF here.  DID SHIP COLLIDE WITH ANYTHING? */
    if (!(ZP.CHIST[0] & 0x80))
        ZP.CHIST[1] = 0xFF;                 /* NOPE. INVALIDATE FLAG */
    /* COLIDE_1: with the stack hole armed, save the position of the
     * JSRL on the hole for the copyright check. */
    if (ZP.HOLE != 0) {
        g.pg1.raw[(uint8_t)(0x01 + ZP.HOLE)] = ZP.VGLIST[0];
        g.pg1.raw[(uint8_t)(0x02 + ZP.HOLE)] = ZP.VGLIST[1];
    }
}

/* ------------------------------------------------------------------ */
/* MOTION ($66F8) / MOV ($6713) - motion update                         */
/* ------------------------------------------------------------------ */

/* MOV_11: an explosion.  A = the new OBJ byte (still negative). */
static void mov_11(uint8_t x, uint8_t a)
{
    uint8_t y;

    OBJ(x) = a;
    a = (uint8_t)((a & 0xF0) + 0x10);       /* A0 TO 0F0 -> B0 TO 0: the scale */
    if (x == AD_SHIP)
        a = 0xF0;                           /* SCALING FOR SHIP EXPLOSION */
    y = a;
    ZP.XCOMP[0] = OBJXL(x);
    ZP.XCOMP[1] = OBJXH(x);
    ZP.XCOMP[2] = OBJYL(x);
    ZP.XCOMP[3] = OBJYH(x);
    ad_pictur(y, x);                        /* MOV_30: DISPLAY PICTURE */
}

/* MOV_12: the exploding branch of the loop. */
static void mov_12(uint8_t x, uint8_t obj)
{
    uint8_t a;
    unsigned s;
    bool c;

    a = ad_comp(obj);                       /* TIME REMAINING (0 TO 60) */
    a >>= 4;
    if (x == AD_SHIP) {
        c = (ZP.FRAME[0] & 0x01) != 0;      /* lda FRAME / and #1 / lsr: ADD 1 EVERY OTHER FRAME */
        a = 0;
    } else {
        c = true;                           /* MOV_16: 1 + VALUE/16 */
    }
    s = (unsigned)a + obj + (c ? 1u : 0u);  /* MOV_42: NEW EXPLOSION PICTURE */
    a = (uint8_t)s;
    if (a & 0x80) {                         /* STILL INACTIVE */
        mov_11(x, a);
        return;
    }
    if (x == AD_SHIP) {                     /* MOV_18 */
        ad_newshp();                        /* RESET SHIP */
    } else if (x > AD_SHIP) {               /* MOV_40: THE SAUCER */
        AD_P->f.EDELAY = AD_P->f.SEDLAY;    /* DELAY BEFORE REENTERING */
    } else {
        if (--AD_P->f.NROCKS == 0)          /* no MORE ROCKS REMAIN */
            AD_P->f.RDELAY = 0x7F;          /* DELAY BEFORE STARTING */
    }
    /* MOV_17 */
    OBJ(x) = 0x00;                          /* RESET PICTURE */
}

void ad_motion(void)
{
    uint8_t x, y, a, obj;
    unsigned s;
    bool c;

    /* BINSCR = the score in units of 10 000, capped at 15, from the
     * BCD ten-thousands byte:
     *     and #$0F -> BINSCR; eor -> high nibble in place
     *     lsr -> hi*8 (R0); lsr lsr -> hi*2; adc R0; adc BINSCR */
    x = ZP.PLAYR3;
    a = g.zp.raw[0x64 + x];
    ZP.BINSCR = (uint8_t)(a & 0x0F);        /* ISOLATE LOW 4 BITS */
    a = (uint8_t)(a & 0xF0);                /* ISOLATE HI 4 BITS */
    a >>= 1;                                /* HIGH NIBBLE * 8 */
    ZP.R0 = a;
    a >>= 2;                                /* HI NIBBLE * 2 */
    a = (uint8_t)(a + ZP.R0);               /* HI NIBB * 10 (no carry possible) */
    a = (uint8_t)(a + ZP.BINSCR);           /* + LOW NIBBLE */
    if (a >= 0x0F)
        a = 0x0F;                           /* MAX IT OUT */
    ZP.BINSCR = a;                          /* KEEP BINARY SCORE LIMITED */

    /* MOV */
    ZP.R9 = 0;                              /* OBJECT COUNTER */
    for (x = 0x20; !(x & 0x80); x--) {      /* MOV_10 / MOV_13 */
        obj = OBJ(x);
        if (obj == 0)
            continue;                       /* inactive */
        if (obj & 0x80) {                   /* MOV_12: exploding */
            mov_12(x, obj);
            continue;
        }
        /* MOV_14: an active object */
        TEMP4 = obj;                        /* SAVE OBJ CODE FOR ATTACK */
        TEMP4 = (uint8_t)(TEMP4 << 1);      /* asl TEMP4: SPECIAL? -> bit 7 */
        if ((TEMP4 & 0x80) && x < AD_SHIP) {    /* a special ROCK */
            y = (uint8_t)((obj & 0x3C) >> 2);
            TEMP4A = y;                     /* SAVE INDEX */
            TEMP4B = AD_P->f.SRTIME[y];     /* GET CONTROL BYTE OF SPECIAL ROCK */
            if (!(TEMP4B & 0x80))           /* attacking? */
                ad_attack(x);               /* ATTACK */
        }
        /* MOV_1: X += XINC with sign extension, wrapped to 0..$1FFF */
        y = 0;
        a = XINC(x);
        if (a & 0x80)
            y--;                            /* SIGN EXTENSION */
        s = (unsigned)a + OBJXL(x);         /* clc / adc OBJXL,x */
        c = (s & 0x100) != 0;
        OBJXL(x) = (uint8_t)s;
        ZP.XCOMP[0] = (uint8_t)s;
        s = (unsigned)y + OBJXH(x) + (c ? 1u : 0u);
        a = (uint8_t)s;
        if (a >= 0x20) {                    /* not 0 TO 1023 */
            if (x == AD_SAUCER) {           /* THE SAUCER */
                ad_rsaucr();                /* RESET SAUCER VALUES */
                continue;                   /* MOV_28 */
            }
            /* MOV_35: a special rock that timed out is zapped */
            if ((TEMP4 & 0x80) && !(TEMP4B & 0x80) && TEMP4B < 0x04) {
                AD_P->f.NROCKS--;           /* ZAP THE OBJECT */
                AD_P->f.SPROCK--;
                OBJ(x) = 0x00;
                AD_P->f.SRTIME[TEMP4A] = 0x00;
                continue;                   /* MOV_28 */
            }
            a &= 0x1F;                      /* MOV_33 */
        }
        /* MOV_19 */
        OBJXH(x) = a;
        ZP.XCOMP[1] = (uint8_t)(a & 0x7F);
        /* MOV_2: Y += YINC, wrapped to 0..767 */
        y = 0;
        a = YINC(x);
        if (a & 0x80)
            y--;                            /* SIGN EXTENSION */
        s = (unsigned)a + OBJYL(x);         /* clc / adc OBJYL,x */
        c = (s & 0x100) != 0;
        OBJYL(x) = (uint8_t)s;
        ZP.XCOMP[2] = (uint8_t)s;
        s = (unsigned)y + OBJYH(x) + (c ? 1u : 0u);   /* 0 TO 767 PLEASE */
        a = (uint8_t)s;
        if (a >= 0x18) {                    /* not ALREADY 0 TO 767 */
            if (a == 0x18)
                a = 0x00;                   /* MOV_24: IF 768 UP */
            else
                a = 0x17;
        }
        /* MOV_25 */
        OBJYH(x) = a;
        ZP.XCOMP[3] = a;
        y = ad_rom((uint16_t)(AD_SIZOPT + (OBJ(x) & 0x03)));   /* GET SIZE OPTION */
        ad_pictur(y, x);                    /* MOV_30: DISPLAY PICTURE */
    }
}
