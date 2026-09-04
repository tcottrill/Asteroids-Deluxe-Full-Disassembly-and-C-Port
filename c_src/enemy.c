/* The enemies - the saucer (ENEMY, LSF, EFIRE, LFCR, RSAUCR/SRSAUC in
 * DSTRD0.MAC) and the Deluxe "killer satellite" cluster (SETTIP,
 * ATTACK, SPLTTP and friends in TRIROT.MAC).
 *
 * The special rocks are ordinary object slots whose OBJ byte has bit 6
 * set; bits 5-2 link the slot to one of seven control entries, SRTIME
 * (bit 7 = still waiting in the cluster, bit 0 = size, the rest a
 * timer) and SRANG (the heading).  ATTACK steers each one toward the
 * ship - or, once it times out or the ship is gone, toward the saucer
 * slot, which walks it off the edge.
 *
 * Contracts: disasm/CALLING_NOTES.md sections 3, 7, 11 and 12.
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
#define SRTIME(i) (AD_P->f.SRTIME[i])
#define SRANG(i)  (AD_P->f.SRANG[i])
#define TEMP3   ZP.TEMP3[0]
#define TEMP3H  g.zp.raw[0x0F]
#define ANGLEH  g.zp.raw[0x7A]

/* ------------------------------------------------------------------ */
/* GPTS ($48B2) - points for the player, but not for the saucer         */
/* ------------------------------------------------------------------ */
/* TEMP3 = who hit it: 0 the ship, 4-7 the ship's torpedoes score;
 * 1-3 (the saucer and its torpedoes) do not.  `clc / jmp POINTS`. */
static void gpts(uint8_t a)
{
    uint8_t x = TEMP3;
    if (x != 0 && x < 0x04)                 /* SHIP'S TORPEDOES? NO. NO POINTS */
        return;
    ad_points(a, false);                    /* GIVE POINTS */
}

/* FREESR ($48A7) - a free control entry, 6 down to 0, or $FF. */
static uint8_t freesr(void)
{
    uint8_t x;
    for (x = 0x06; !(x & 0x80); x--)        /* MAXIMUM # OF SPECIAL ROCKS ALLOWED */
        if (SRTIME(x) == 0)                 /* FIND AN EMPTY SLOT */
            return x;
    return x;
}

/* ------------------------------------------------------------------ */
/* SPLTTP ($4800) - split tips on collision                             */
/* ------------------------------------------------------------------ */
/* Y = the special rock hit; TEMP3 = who hit it (set by SPLIT).  Three
 * cases on the control byte: still waiting in the cluster (release
 * the whole chain, 500 points), a small diamond (200, gone), a big
 * one (1000, and it splits into two small ones aimed ANGLE +/- 45). */
void ad_splttp(uint8_t y)
{
    uint8_t a, x;

    x = (uint8_t)((OBJ(y) & 0x3C) >> 2);    /* ISOLATE LINK FIELD, relative to 0 */
    a = SRTIME(x);                          /* GET CONTROL BYTE */
    if (a & 0x80) {                         /* still WAITING */
        ZP.XT = x;                          /* SAVE INDEX */
        gpts(0x05);                         /* 500 POINTS */
        x = ad_search();                    /* LOOK FOR FREE ENTRY */
        AD_P->f.NROCKS++;                   /* ADD ANOTHER TO TOTAL */
        a = OBJ(y);                         /* GET PIX CODE AGAIN, SAVE IT */
        ad_cpypos(x, y);                    /* COPY ALL PARAMETERS */
        OBJ(x) = a;                         /* (CPYPOS BLITZES 'OBJ'; PASS THE OLD CODE) */
        OBJ(y) = 0x00;                      /* CAUSE A BIG EXPLOSION */
        x = ZP.XT;                          /* RESTORE X */
        /* SPLTTP_3: FREE ALL THE OBJECTS IN THE CLUSTER, following the
         * link each control byte holds in its low bits. */
        for (;;) {
            a = SRTIME(x);
            if (!(a & 0x80))                /* DONE */
                break;
            SRTIME(x) = 0x7F;               /* RELEASE AS BIG OBJECT WITH MAX TIME */
            x = (uint8_t)((a >> 1) & 0x0F); /* THE LINK TO THE NEXT OBJECT */
        }
        return;                             /* SPLTTP_2: ldx TEMP3 */
    }
    /* SPLTTP_1 */
    if (!(a & 0x01)) {                      /* size bit: not A BIG ONE */
        SRTIME(x) = 0x00;                   /* DE-ACTIVATE CONTROL BYTE */
        gpts(0x20);                         /* GIVE 'EM 200 POINTS */
        ad_diasnd();                        /* GIVE SOUND FOR DIAMOND */
        return;
    }
    /* SPLTTP_4: a big diamond splits into two small ones */
    ZP.XT = x;                              /* SAVE X */
    gpts(0x10);                             /* 1000 */
    x = ad_search();                        /* FIND A FREE ENTRY */
    ad_cpypos(x, y);                        /* COPY POSITION PARAMETERS */
    AD_P->f.NROCKS++;                       /* ADD 1 TO TOTAL OBJECTS */
    AD_P->f.SPROCK++;                       /* ADD 1 TO SPECIAL COUNT */
    OBJ(x) = (uint8_t)((ZP.XT << 2) | 0x42);    /* A SPECIAL ROCK BUT A SMALL ONE */
    ZP.R0 = x;                              /* SAVE X */
    x = ZP.XT;                              /* POINT TO CONTROL BYTE */
    SRTIME(x) = 0x7E;
    /* adc #$20 with C = 0 (the asl above shifted a zero out) */
    SRANG(x) = (uint8_t)(ZP.ANGLE[0] + 0x20);   /* POINT IT AWAY APPROX 45 DEGREES */
    x = freesr();                           /* FIND A FREE SPECIAL ROCK ENTRY */
    ZP.XT = x;                              /* SAVE ENTRY POINTER */
    x = ad_searc1(ZP.R0);                   /* LOOK FOR NEXT FREE OBJ, below the last */
    ad_cpypos(x, y);                        /* MOVE POSITION PARAMETERS */
    AD_P->f.NROCKS++;                       /* ADD 1 TO TOTAL OBJECT COUNT */
    OBJ(x) = (uint8_t)((ZP.XT << 2) | 0x42);    /* SELECT SPECIAL ROCK */
    x = ZP.XT;                              /* POINT TO SPECIAL CONTROLS */
    SRTIME(x) = 0x7E;                       /* ACTIVATE OBJECT */
    /* `sbc #$20` with C = 0 from the asl, so this is ANGLE - $21: the
     * ROM's "approx 45 degrees" the other way is one degree short. */
    SRANG(x) = (uint8_t)(ZP.ANGLE[0] - 0x20 - 1);
    /* ldx TEMP3: X is the caller's */
}

/* ------------------------------------------------------------------ */
/* ATTACK ($48BE) - steer special rock X                                */
/* ------------------------------------------------------------------ */
void ad_attack(uint8_t x)
{
    uint8_t a, y, ang;
    bool c;

    ZP.R0 = x;                              /* SAVE X */
    ZP.R1 = AD_SHIP;                        /* ASSUME WE WILL TRACK SHIP */
    ZP.R9++;                                /* COUNT ATTACKING ENEMY */
    x = (uint8_t)((OBJ(x) >> 2) & 0x0F);    /* POINT TO CONTROL BYTE */
    y = SRTIME(x);                          /* GET CONTROL */
    if (y < 0x04) {                         /* TIMED OUT? YEP. TRACK TO EDGE */
        ZP.R1++;                            /* ATTACK_2: TRACK ON SNOW FLAKE */
    } else {
        if ((ZP.FRAME[0] & 0x0F) == 0)      /* TIME OUT ? */
            SRTIME(x) = (uint8_t)(y - 0x02);    /* TIME DOWN */
        /* ATTACK_3 */
        a = OBJ(AD_SHIP);                   /* SHIP VISIBLE? */
        if (a == 0 || (a & 0x80)) {         /* NOPE, or BLOWING UP: FORCE A TIME OUT */
            /* ATTACK_7 */
            SRTIME(x) = (uint8_t)((y & 0x01) | 0x02);   /* KEEP SIZE, AND A MINIMUM TIME */
            ZP.R1++;                        /* ATTACK_2 */
        }
    }
    /* ATTACK_4: the heading to the target, X then Y, through CPUTD,
     * RTST (the difficulty-aware range) and SCALER (arctan). */
    y = ZP.R1;
    ZP.XT = x;                              /* SAVE X */
    x = ZP.R0;                              /* POINT TO POSITION PARAMETERS */
    a = ad_cputd(x, y);
    a = ad_rtst(a, x);                      /* DO RANGE */
    ZP.R3 = a;                              /* SAVE X DIFFERENCE */
    ZP.R2 = ZP.TEMP2[0];
    y = (uint8_t)(ZP.R1 + 0x21);            /* SKIP TO Y PARAMETERS */
    x = (uint8_t)(x + 0x21);
    a = ad_cputd(x, y);                     /* COMPUTE Y DISTANCE */
    a = ad_rtst(a, x);                      /* RANGE IT */
    ang = ad_scaler(a);                     /* SCALE IT, GET ARCTAN */
    x = ZP.XT;                              /* RESTORE X */
    ZP.R2 = ang;                            /* SAVE ANGLE */
    if (ZP.R1 == AD_SHIP) {                 /* TRACKING ON SHIP? */
        /* Don't turn if the ship is pointing at the enemy:
         *     eor #$80 / sbc ANGLE      C = 1 from cpy, so a plain subtract */
        a = (uint8_t)((ang ^ 0x80) - ZP.ANGLE[0]);
        if (a & 0x80)
            a ^= 0xFF;
        /* ATTACK_20 */
        if (a < 0x10) {                     /* the ship is pointing at us */
            /* Is the object pointing at the ship?  `sbc SRANG,x` with
             * C = 0 from the failed bcs: one less. */
            a = (uint8_t)(ZP.R2 - SRANG(x) - 1);
            if (a & 0x80)
                a ^= 0xFF;
            /* ATTACK_22 */
            if (a < 0x40)                   /* YES. DON'T TURN */
                goto attack_18;
        }
    }
    /* ATTACK_21 */
    a = (uint8_t)(ZP.R2 - SRANG(x));        /* sec / sbc: TARGET - CURRENT */
    ZP.R2 = a;                              /* SAVE SIGN */
    if (a & 0x80)
        a ^= 0xFF;                          /* NEGATE IT */
    /* ATTACK_11 */
    if (a < 0x08)                           /* DIFFERENCE > 8? NOPE. DON'T TURN */
        goto attack_18;
    a = 0x01;                               /* PREPARE FOR SLOW TURN */
    if (ZP.R1 == AD_SHIP) {                 /* TRACKING ON SHIP? */
        uint8_t i = ZP.R9;                  /* GET OBJ # */
        if (i >= 0x07)                      /* LIMIT IT */
            i = 0x00;
        a = ad_rom((uint16_t)(AD_AVEL + i));    /* GET ANGULAR VELOCITY */
    }
    /* ATTACK_12: apply the turn in the direction of the sign */
    c = (ZP.R2 & 0x80) != 0;                /* asl R2: GET SIGN */
    ZP.R2 = (uint8_t)(ZP.R2 << 1);
    if (c)
        a ^= 0xFF;                          /* NEGATE IT (CARRY SET FROM ABOVE) */
    /* ATTACK_16: adc SRANG,x with that carry */
    SRANG(x) = (uint8_t)(a + SRANG(x) + (c ? 1 : 0));
attack_18:
    /* Speed: $1C plus min(BINSCR*2, $F), the carry from the compare
     * riding into the add when it was clamped. */
    a = (uint8_t)(ZP.BINSCR << 1);
    c = (a >= 0x10);                        /* MAX IT OUT */
    if (c)
        a = 0x0F;
    /* ATTACK_19 */
    a = (uint8_t)(a + 0x1C + (c ? 1 : 0));  /* MINIMUM VELOCITY */
    ZP.R1 = a;
    {
        uint8_t speed = a;                  /* pha */
        a = ad_mult(ad_cos(SRANG(x)));      /* COS(ANGLE) * speed */
        XINC(ZP.R0) = a;                    /* SUPPLY NEW X INCREMENT */
        ZP.R1 = speed;                      /* pla / sta R1 */
        x = ZP.XT;
        a = ad_mult(ad_sin(SRANG(x)));
        YINC(ZP.R0) = a;
    }
}

/* ------------------------------------------------------------------ */
/* RNDXYI ($4B3D) - +/-6 on each axis, at random                        */
/* ------------------------------------------------------------------ */
/* `jsr RNDXYI_1` then fall into it: RNDXYI_1 stores YINC each time and
 * the first result is copied into XINC on return. */
static uint8_t rndxyi_1(uint8_t x)
{
    uint8_t y = 0x06;                       /* ASSUME + */
    if (ad_hw_random() & 0x80)
        y = 0xFA;                           /* ELSE SET - */
    YINC(x) = y;
    return y;
}

void ad_rndxyi(uint8_t x)
{
    XINC(x) = rndxyi_1(x);                  /* SET UP X */
    rndxyi_1(x);
}

/* ------------------------------------------------------------------ */
/* SETTIP ($4A96) - initialise a special cluster                        */
/* ------------------------------------------------------------------ */
void ad_settip(void)
{
    uint8_t a, x, y;
    unsigned s;
    bool c;

    if ((AD_P->f.DIFCTY >> 1) < AD_P->f.NROCKS)     /* TOO MANY ROCKS? */
        return;
    /* SETTIP_1 */
    if (AD_P->f.SROCKS < 0x02)              /* ONLY COME OUT AFTER 2ND WAVE */
        return;
    x = AD_P->f.SPROCK;                     /* ALREADY HAVE SPECIALS? */
    if (x != 0)
        return;                             /* YEP. DON'T LAUNCH ANYMORE */
    y = AD_P->f.NROCKS;                     /* GET NUMBER OF ACTIVE OBJECTS */
    do {                                    /* SETTIP_6: from X = SPROCK = 0 */
        a = OBJ(x);                         /* SEARCH FOR ACTIVE BUT NOT EXPLODING */
        if (a == 0)
            ZP.R0 = x;                      /* SAVE HIGHEST INDEX TO FREE OBJECT */
        if (a & 0x80)                       /* EXPLODING. DON'T COUNT IT */
            y--;
        x++;                                /* NEXT OBJ */
    } while (x < AD_SHIP);                  /* LIMIT? */
    x = ZP.PLAYR;                           /* POINT TO PLAYER */
    if (ZP.TRILE[x] != 0) {                 /* LAUNCHED ANY THIS WAVE? */
        if (y < 0x03)                       /* FEWER THAN 3 ITEMS? YES. DON'T LAUNCH */
            return;
    }
    /* SETTIP_9 */
    y = AD_SAUCER;                          /* POINT TO SAUCER VELOCITIES */
    ZP.TRILE[x] = y;                        /* SET TRILE .NE. FOR NEXT TIME */
    x = ZP.R0;                              /* POINT TO FREE OBJ */
    ad_rndpos(x, y);                        /* GET POSITION AND VELOCITY */
    ad_rndxyi(x);                           /* GET RANDOM VELOCITY */
    y = 0x02;
    ZP.HSSND = y;                           /* SIGNAL TO MAKE NOISE WHEN CHANNEL AVAIL */
    for (;;) {                              /* SETTIP_5: three linked members */
        OBJ(x) = (uint8_t)((y << 2) | 0x42);    /* SPECIAL AND MEDIUM SIZE */
        AD_P->f.NROCKS++;                   /* COUNT OBJ */
        AD_P->f.SPROCK++;
        SRTIME(y) = ad_rom((uint16_t)(AD_ITIME + y));   /* INITIAL CONTROL DATA */
        SRANG(y) = ad_rom((uint16_t)(AD_IANG + y));     /* INITIAL ANGLE */
        s = (unsigned)ad_rom((uint16_t)(AD_ITXL + y)) + OBJXL(x);   /* INITIAL POSITION OFFSET */
        c = (s & 0x100) != 0;
        OBJXL(x) = (uint8_t)s;
        OBJXH(x) = (uint8_t)(ad_rom((uint16_t)(AD_ITXH + y)) + OBJXH(x) + (c ? 1 : 0));
        s = (unsigned)ad_rom((uint16_t)(AD_ITYL + y)) + OBJYL(x);
        c = (s & 0x100) != 0;
        OBJYL(x) = (uint8_t)s;
        OBJYH(x) = (uint8_t)(ad_rom((uint16_t)(AD_ITYH + y)) + OBJYH(x) + (c ? 1 : 0));
        if (y == 0)                         /* COUNT DOWN; DONE */
            return;
        y--;
        ZP.R0 = x;                          /* SAVE CURRENT INDEX */
        x = ad_searc1(x);                   /* GET NEXT FREE SPACE */
        ZP.R1 = y;                          /* SAVE Y */
        ad_cpypos(x, ZP.R0);                /* COPY POSITION PARAMETERS from the old */
        y = ZP.R1;                          /* RESTORE Y */
    }
}

/* ------------------------------------------------------------------ */
/* The saucer                                                          */
/* ------------------------------------------------------------------ */

/* SRSAUC ($6835) - park it off screen. */
void ad_srsauc(void)
{
    AD_P->f.SAUPIX = 0;                     /* CLEAR SAUCER */
    AD_P->f.SAUXI = 0;
    AD_P->f.SAUYI = 0;
    AD_P->f.SAUXH = 0;                      /* BE SURE X POSITION IS 0 */
    AD_P->f.SAUXL = 0;
}

/* RSAUCR ($6826) - reset it: the delay reloaded, its sound off. */
void ad_rsaucr(void)
{
    AD_P->f.EDELAY = AD_P->f.SEDLAY;        /* DELAY BEFORE RESTARTING */
    ad_sndoff(0x17);                        /* TURN OFF SOUND */
    ad_srsauc();
}

/* LFCR ($6482) - look for a close rock to shoot at.  Y enters as $19
 * and leaves as the rock's slot, or $19 again if there is none.  A
 * flying special rock is skipped; one still in its cluster is shot
 * "anyway".  Otherwise the rock must be within 8 high-byte units on
 * both axes. */
static uint8_t lfcr(uint8_t y)
{
    uint8_t a, x;

    for (y--; !(y & 0x80); y--) {           /* LFCR_2 */
        a = OBJ(y);
        if (a == 0 || (a & 0x80))           /* empty, or BLOWING UP */
            continue;
        if (a >= 0x40) {                    /* SPECIAL ROCK? */
            x = (uint8_t)(a >> 2);          /* $10 + link */
            if (!(PG[0xE8 + x] & 0x80))     /* lda EDELAY,x = SRTIME[link]: IS IT FLYING? */
                continue;                   /* YEP. SKIP IT */
            return y;                       /* SHOOT IT ANYWAY */
        }
        /* LFCR_4 */
        a = (uint8_t)(AD_P->f.SAUXH - OBJXH(y));    /* ABS (DIFFX) */
        if (a & 0x80)
            a ^= 0xFF;
        if (a >= 0x08)                      /* ARE THEY CLOSE IN X? NO. FORGET IT */
            continue;
        a = (uint8_t)(AD_P->f.SAUYH - OBJYH(y));    /* ABS (DIFFY) */
        if (a & 0x80)
            a ^= 0xFF;
        if (a >= 0x08)                      /* CLOSE IN Y? */
            continue;
        return y;
    }
    return AD_SHIP;                         /* REALLY NO ROCKS SO SHOOT @ SHIP */
}

/* EFIRE ($63E6) - enemy fire control. */
/* Rev 3's aim-error tables, $644F and $6453 in astdelux_main.asm (the
 * rev 3 listing), indexed by min(BINSCR, 3).  Rev 2's two-entry pair is
 * read from the ROM image at AD_EFIRE97/98 as rule 11 asks; these four
 * bytes each are typed in because the port carries only the rev 2
 * image - the one deliberate exception, per rule 13.  Same layout as
 * rev 2's: the mask, then the sign-extension byte OR'd in when the
 * masked byte is negative. */
static const uint8_t rev3_efire_97[4] = { 0x9F, 0x8F, 0x8F, 0x87 };
static const uint8_t rev3_efire_98[4] = { 0x60, 0x70, 0x70, 0x78 };

static void efire(void)
{
    uint8_t a, x, y;
    bool c;

    if ((uint8_t)(ZP.FRAME[0] << 1) == 0) { /* every 128 frames: CHANGE DIRECTION */
        x = (uint8_t)(ad_hw_random() & 0x03);
        AD_P->f.SAUYI = ad_rom((uint16_t)(AD_EFIRE99 + x));
    }
    /* EFIRE_10 */
    if (ZP.NPLAYR != 0 && AD_P->f.SDELAY != 0)
        return;                             /* DON'T FIRE IF YOU ARE DEAD */
    /* EFIRE_30 */
    if (--AD_P->f.EDELAY != 0)
        return;                             /* not TIME TO SHOOT */
    /* EFIRE_50 */
    AD_P->f.EDELAY = 0x0A;                  /* DELAY BEFORE NEXT SHOT */
    y = AD_SHIP;                            /* POINT TO SHIP */
    if (ZP.NPLAYR == 0) {                   /* ATTRACT MODE? YES. SHOOT AT ROCK */
        y = lfcr(y);                        /* EFIRE_82 */
        goto efire_81;
    }
    if (ad_hw_rom_rev() < 3) {
        /* Rev 2 only ($6411-$6416): rev 3 dropped the tamper trap here
         * and its RANDOM read with it ($63EC goes straight from the
         * attract test to ldx #$AA). */
        a = ad_hw_random();                 /* RANDOM NUMBER (kept in A for the trap) */
        if (ZP.PROT[2] != 0) {              /* CHECKSUM ERRORS? YEP. SHOOT WILD */
            ANGLEH = a;                     /* EFIRE_96: the random byte is the angle */
            goto efire_91;
        }
    }
    x = 0xAA;                               /* SHOOT AT PLAYER 2 OUT OF 3 */
    if (!(AD_P->f.SAUPIX & 0x01))           /* not LITTLE */
        x = 0x40;                           /* SHOOT AT PLAYER 1 OUT OF 4 */
    /* EFIRE_80 */
    if (x < ad_hw_random())                 /* cpx RANDOM / bcs: not at the player */
        y = lfcr(y);                        /* SHOOT @ ROCK */
efire_81:
    ZP.R8 = y;                              /* SAVE Y */
    x = AD_SAUCER;                          /* SAUCER IS DOING THE SHOOTING */
    a = ad_cputd(x, y);                     /* COMPUTE X DISTANCE */
    a = ad_range(a);                        /* CHECK DIRECTION */
    ZP.R3 = a;                              /* SAVE DIFFERENCE */
    ZP.R2 = ZP.TEMP2[0];
    y = (uint8_t)(ZP.R8 + 0x21);
    x = 0x3B;                               /* POINT TO Y PARAMETERS */
    a = ad_cputd(x, y);                     /* COMPUTE Y DISTANCE */
    a = ad_range(a);                        /* BE REAL MEAN */
    ANGLEH = ad_scaler(a);                  /* SCALE DIFFERENCES, GET ARCTAN */
    if (ZP.R8 == AD_SHIP) {                 /* SHOOTING AT SHIP? */
        if (ad_hw_rom_rev() >= 3) {
            /* Rev 3 ($642B-$6440) replaced the score compare below
             * with a four-step ramp on the score band:
             *
             *     ldx BINSCR / cpx #3 / bcc + / ldx #3
             *     lda RANDOM / and $644F,x / bpl + / ora $6453,x
             *     adc $7A / sta $7A
             *
             * so the aim error is +-32 below 10,000, +-16 to 29,999,
             * +-8 from 30,000, and the carry from `cpx #3` (BINSCR >= 3)
             * rides into the adc as rev 2's did.  BINSCR is $D3 in rev
             * 3's page zero; the port's is the same cell by name. */
            x = ZP.BINSCR;
            c = (x >= 0x03);
            if (c)
                x = 0x03;
            a = (uint8_t)(ad_hw_random() & rev3_efire_97[x]);
            if (a & 0x80)
                a |= rev3_efire_98[x];      /* SIGN EXTENSION */
            a = (uint8_t)(a + ANGLEH + (c ? 1 : 0));
            ANGLEH = a;
        } else {
            /* The aim error.  "SCORE > 60,000?" is `cmp $06`, zero page
             * $06 (XCOMP+1), not an immediate 6 - Atari's source reads
             * `CMP 6` where `CMP I,6` was meant - so the ROM compares
             * the score's top BCD byte against whatever XCOMP+1 last
             * held: the last object MOTION moved, its X high byte after
             * PICTUR's three shifts, 0..3.  So the saucer "gets mad"
             * from anywhere below 30,000, not at 60,000.  Kept as
             * written: every rev 1 and rev 2 ROM set has the C5 06.
             * The carry from that compare then rides into `adc $7A`. */
            a = g.zp.raw[0x64 + ZP.PLAYR3]; /* MSD OF SCORE */
            x = 0x00;
            c = (a >= g.zp.raw[0x06]);
            if (c)
                x++;                        /* GET MAD */
            /* EFIRE_90 */
            a = (uint8_t)(ad_hw_random() & ad_rom((uint16_t)(AD_EFIRE97 + x)));
            if (a & 0x80)
                a |= ad_rom((uint16_t)(AD_EFIRE98 + x));    /* SIGN EXTENSION */
            /* EFIRE_95 */
            a = (uint8_t)(a + ANGLEH + (c ? 1 : 0));        /* DON'T BE TOO GOOD - JUST CLOSE */
            ANGLEH = a;                     /* EFIRE_96: ANGLE TO AIM */
        }
    }
efire_91:
    y = 0x03;                               /* START LOOKING HERE */
    TEMP3H = 0x01;                          /* 2 SAUCER TORPEDOS */
    x = ZP.R8;                              /* POINT TO OBJ: the target's velocity */
    ZP.R8 = 0x01;                           /* SAUCER INDEX */
    ad_fire1(y, x);                         /* FIRE A TORPEDO */
}

/* LSF ($6399) - launch a saucer at a random height from one side. */
static void lsf(void)
{
    uint8_t a, x, y, r;
    bool c;

    /* Three random bits rotated through SAUYL (a scratch here); the
     * last lsr's carry is added in with the 4. */
    r = ad_hw_random();
    a = r;
    for (int i = 0; i < 3; i++) {
        c = (a & 0x01) != 0;
        a >>= 1;
        AD_P->f.SAUYL = (uint8_t)((AD_P->f.SAUYL >> 1) | (c ? 0x80 : 0));   /* ror SAUYL */
    }
    a = (uint8_t)(a + 0x04 + (c ? 1 : 0));  /* adc #$04 */
    if (a >= 0x12)                          /* MUST BE 0 TO 767 */
        a = (uint8_t)(a - 0x10);            /* sbc #$10, C = 1 */
    /* LSF_10 */
    AD_P->f.SAUYH = a;                      /* STARTING VERTICAL POSITION */
    c = (ad_hw_random() & 0x80) != 0;       /* DIRECTION INTO CARRY */
    a = 0x00;                               /* ASSUME START ON LEFT */
    x = 0x00;
    y = 0x10;                               /* VELOCITY + */
    if (c) {
        x = 0xFF;                           /* dex: LDX #-1 */
        a = 0x1F;
        y = 0xF0;
    }
    /* LSF_20 */
    AD_P->f.SAUXI = y;                      /* VELOCITY IN X */
    AD_P->f.SAUXH = a;                      /* X POS MSB */
    AD_P->f.SAUXL = x;                      /* X POS LSB */
    y = 0x02;
    if ((uint8_t)(AD_P->f.DIFCTY - AD_P->f.NROCKS) >= 0x04)   /* DIFFERENCE LARGE? */
        y--;                                /* SMALL SAUCER */
    /* LSF_40 */
    AD_P->f.SAUPIX = y;
    ad_sndon(ad_rom((uint16_t)(AD_SPSND - 1 + y)));   /* TURN ON SAUCER SOUND */
}

/* ENEMY ($6347) - launch the saucer, or let it shoot. */
void ad_enemy(void)
{
    uint8_t a, x;

    if (ZP.FRAME[0] & 0x03)                 /* ONLY EVERY 4TH FRAME */
        return;
    /* ENEMY_1 */
    a = AD_P->f.SAUPIX;
    if (a != 0) {
        if (a & 0x80) {                     /* BLOWING UP */
            ad_sndoff(0x17);
            return;
        }
        efire();                            /* ALIVE. GO SHOOT */
        return;
    }
    /* ENEMY_15 */
    ad_srsauc();                            /* POSITION OFF SCREEN */
    if (ZP.NPLAYR != 0) {                   /* not attract: needs a live ship */
        a = OBJ(AD_SHIP);
        if (a == 0 || (a & 0x80))           /* NOT VISIBLE, or EXPLODING */
            return;
    }
    /* ENEMY_6 */
    if (AD_P->f.RTIMER != 0)
        AD_P->f.RTIMER--;                   /* DECREMENT ZERO TIMER */
    /* ENEMY_2 */
    if (--AD_P->f.EDELAY != 0)
        return;                             /* NO TIME YET */
    AD_P->f.EDELAY = 0x01;                  /* DELAY BEFORE SHOOTING OR ENTERING */
    x = AD_P->f.NROCKS;
    if (x == 0)
        return;                             /* NO ROCKS */
    if (AD_P->f.RTIMER != 0) {              /* rocks have been hit lately */
        if (x >= AD_P->f.DIFCTY)
            return;                         /* TOO MANY ROCKS */
    }
    /* ENEMY_5: shorten the starting delay by 6, down to $20 */
    a = (uint8_t)(AD_P->f.SEDLAY - 0x06);
    if (a >= 0x20)
        AD_P->f.SEDLAY = a;
    lsf();
}
