/* Scoring, the HUD, the high-score table and initials entry, and the
 * game-start / game-over state machine - DSTRD0.MAC's CHKST, CHKST1,
 * CHKST2, GETINT, INITAL, PARAMS, GTSP, TWO, POINTS, SCORES, IN3TIM,
 * UPDATE, and CASTST's LIVES/LIVESS.
 *
 * Scores are three BCD bytes per player at SCORE+PLAYR3 ($62-$64 and
 * $65-$67); the high-score table is ten of them at HSCORE ($23) with
 * three initials each at INITL ($44).  POINTS, CHKST's credit display
 * and SCORES' rank counter all run in decimal mode, so they go through
 * bcd_adc(), which is the NMOS 6502's ADC with D set.
 *
 * Contracts: disasm/CALLING_NOTES.md sections 3, 8 and 13.
 */
#include "astdelux.h"

#define ZP     (g.zp.f)
#define PG     (AD_P->raw)
#define OBJ(i)   PG[AD_OBJ + (i)]
#define TEMP2H  g.zp.raw[0x0D]
#define FRAMEH  ZP.FRAME[1]                 /* $77 */
#define UPDFLG1 ZP.UPDFLG[1]                /* $43 */

/* ADC in decimal mode, as the 6502 does it.  Returns the byte; *c is
 * the carry in and out. */
static uint8_t bcd_adc(uint8_t a, uint8_t b, bool *c)
{
    unsigned lo = (a & 0x0F) + (b & 0x0F) + (*c ? 1u : 0u);
    unsigned sum;
    if (lo >= 0x0A)
        lo = ((lo + 0x06) & 0x0F) + 0x10;
    sum = (a & 0xF0) + (b & 0xF0) + lo;
    if (sum >= 0xA0)
        sum += 0x60;
    *c = sum >= 0x100;
    return (uint8_t)sum;
}

/* ------------------------------------------------------------------ */
/* POINTS ($6C60) - add points to the score                             */
/* ------------------------------------------------------------------ */
/* A and the carry are the points/100, packed: four `ror a / ror R0`
 * pull the carry in at the top and shift the low nibble out into R0,
 * so R1:R0 = (C:A) >> 4 with the units digit forced to zero.  Then the
 * bonus-life plateau: when the score reaches it, the plateau moves up
 * by BONSCR and a life is added (never past ten). */
void ad_points(uint8_t a, bool carry)
{
    uint8_t x, r0 = ZP.R0;
    bool c = carry;

    /* ror a / ror R0, then lsr a / ror R0 three times: only the first
     * step takes the carry into A; the rest shift a zero in. */
    for (int i = 0; i < 4; i++) {
        bool ca = (a & 0x01) != 0;
        a = (uint8_t)((a >> 1) | ((i == 0 && c) ? 0x80 : 0));
        r0 = (uint8_t)((r0 >> 1) | (ca ? 0x80 : 0));   /* ror R0 */
    }
    ZP.R0 = r0;
    ZP.R1 = a;
    /* sed */
    x = ZP.PLAYR3;
    c = false;                              /* clc */
    ZP.SCORE[x]     = bcd_adc((uint8_t)(ZP.R0 & 0xF0), ZP.SCORE[x], &c);    /* LOW DIGIT ALWAYS 0 */
    ZP.SCORE[x + 1] = bcd_adc(ZP.R1, ZP.SCORE[x + 1], &c);
    ZP.SCORE[x + 2] = bcd_adc(0x00, ZP.SCORE[x + 2], &c);
    a = ZP.SCORE[x + 2];
    if (a < ZP.PLATEU[x + 2])               /* HIT THE NEXT PLATEAU? NOPE */
        goto points_19;
    if (a == ZP.PLATEU[x + 2]) {
        if (ZP.SCORE[x + 1] < ZP.PLATEU[x + 1])   /* CHECK NEXT BYTE DOWN */
            goto points_19;
    }
    /* POINTS_10: HIT LIMIT, SO UP IT A NOTCH */
    c = false;
    ZP.PLATEU[x + 1] = bcd_adc(ZP.BONSCR[1], ZP.PLATEU[x + 1], &c);
    ZP.PLATEU[x + 2] = bcd_adc(ZP.BONSCR[2], ZP.PLATEU[x + 2], &c);
    x = ZP.PLAYR;
    if (ZP.HITS[x] < 0x0A) {                /* NO MORE THAN 10 LIVES */
        ZP.HITS[x]++;                       /* GIVE 'EM ANOTHER LIFE */
        ZP.SND3++;                          /* SIGNAL THAT WE MUST MAKE NOISE */
    }
points_19:
    ;                                       /* cld / ldx R2 */
}

/* ------------------------------------------------------------------ */
/* INITAL ($66E1), IN3TIM ($6D5E), LIVES ($7C72), LIVESS ($7C74)        */
/* ------------------------------------------------------------------ */
void ad_inital(uint8_t y)
{
    uint8_t a = (uint8_t)(ZP.INITL[y] << 1); /* INDEX INTO VGMSGA TABLE */
    if (a != 0) {                           /* IF NOT A BLANK */
        ad_vgchar(a);
        return;
    }
    if (ZP.UPDFLG[0] & UPDFLG1 & 0x80) {    /* NOT UPDATING INITIALS */
        ad_vgchar(0);
        return;
    }
    ad_vgjsrl(0x56B0);                      /* INSERT JSRL TO UNDERLINE */
}

static void in3tim_1(void)
{
    ad_inital(ZP.R3);                       /* DISPLAY INITIAL */
    ZP.R3++;                                /* BUMP POINTER */
}

void ad_in3tim(void)
{
    in3tim_1();
    in3tim_1();
    in3tim_1();
}

/* LIVESS: Y-1 ship glyphs at (A/4, X/4), quarter size.
 *     dec TEMP1 / beq done / bpl again */
void ad_livess(uint8_t a, uint8_t x, uint8_t y)
{
    ZP.TEMP1[0] = y;
    ZP.VGSIZE = 0xE0;                       /* 1/4 SIZE PICTURE */
    ad_vgsabs(a, x);                        /* POSITION BEAM AND RESET SCALE FACTOR */
    ad_vgwait(0x70);
    for (;;) {                              /* LIVESS_11 */
        ZP.TEMP1[0]--;
        if (ZP.TEMP1[0] == 0 || (ZP.TEMP1[0] & 0x80))
            return;                         /* DONE */
        ad_vgadd2(0xA4, 0xCA);              /* LIVESS_10: JSRL TO PICTURE (SHIP17) */
    }
}

void ad_lives(uint8_t a, uint8_t y)
{
    ad_livess(a, 0xD5, y);                  /* Y POSITION FOR PICTURES */
}

/* ------------------------------------------------------------------ */
/* GTSP ($6B90) and TWO ($6BA5)                                         */
/* ------------------------------------------------------------------ */
/* Y = that player's HITS; comes back one less if their ship is on the
 * playfield.  For the player who is not up, the ship byte is read
 * from the other page: `ldx $0319`. */
uint8_t ad_gtsp(uint8_t a, uint8_t y)
{
    uint8_t x;

    if ((a ^ ZP.PLAYR) == 0)                /* WANT 1, IS 1 */
        x = OBJ(AD_SHIP);
    else
        x = AD_P3->raw[AD_OBJ + AD_SHIP];   /* WANT 2, IS 1 (or the reverse) */
    if (x == 0 || (x & 0x80))               /* NOT VISIBLE, or BLOWING UP */
        y++;
    return y;
}

/* TWO - twinkle the ship off screen while it materialises. */
void ad_two(void)
{
    uint8_t sbtl = ZP.SBTL, angle;

    if (sbtl >= 0xC0)                       /* AT MAX? YEP. JUST EXIT */
        return;
    /* eor #$FF / adc #$C1, C = 0 from the bcc: $C0 - SBTL */
    ZP.SBTL = (uint8_t)((sbtl ^ 0xFF) + 0xC1);
    angle = ZP.ANGLE[0];                    /* SAVE CURRENT ANGLE */
    ZP.ANGLE[0] = 0x40;                     /* 90 DEGREES */
    ad_shppic();                            /* DRAW A TWINKLE SHIP */
    ZP.ANGLE[0] = angle;                    /* RESTORE STUFF */
    ZP.SBTL = sbtl;
}

/* ------------------------------------------------------------------ */
/* PARAMS ($6A9A) - display parameters                                  */
/* ------------------------------------------------------------------ */
void ad_params(void)
{
    uint8_t a, x, y;

    if (ZP.NPLAYR != 0) {                   /* not ATTRACT: the copyright shape */
        ad_vgadd2(0x81, 0xC3);
        y = 0x00;                           /* INIT FOR SMALL */
        if (ZP.PLAYR != 0)                  /* PLAYER UP? NO, GIVE 'EM SMALL */
            goto params_11;
    }
    y = 0x10;                               /* PARAMS_12 */
params_11:
    ZP.VGSIZE = y;
    ad_vgsabs(0x19, 0xDB);                  /* POSITION BEAM */
    ad_vgwait(0x70);                        /* WAIT FOR BEAM */
    if (ZP.NPLAYR >= 0x02 && ZP.PLAYR == 0 && OBJ(AD_SHIP) == 0
        && !(AD_P->f.SDELAY & 0x80) && (ZP.FRAME[0] & 0x10) == 0)
        goto params_20;                     /* FLASH SCORE: player 1 waiting */
    /* PARAMS_10 */
    ad_digits(true, 0x62, 0x03);            /* DISPLAY PLAYER 1 SCORE */
params_20:
    y = ad_gtsp(0x00, ZP.HITS[0]);          /* GET SHIP PIX */
    ad_lives(0x28, y);                      /* DISPLAY NUMBER OF LIVES */
    if (ZP.PLAYR == 0) {                    /* PLAYER UP? */
        a = OBJ(AD_SHIP);
        if (a != 0 && !(a & 0x80))          /* ALIVE */
            ad_two();                       /* GO TWINKLE OFF SCREEN */
    }
    /* PARAMS_22 */
    ZP.VGSIZE = 0x00;                       /* SMALL */
    if ((ZP.HSCORE[1] | ZP.HSCORE[2]) != 0) {   /* HIGH SCORE? */
        ad_vgsabs(0x74, 0xDB);              /* POSITION BEAM */
        ad_vgwait(0x50);
        ad_digits(true, 0x23, 0x03);        /* DISPLAY HIGH SCORE */
        ZP.R3 = 0;
        ad_vgchar(0);                       /* BLANK */
        ad_in3tim();                        /* 3 INITIALS */
    }
    /* PARAMS_21 */
    if (ZP.NPLAYR != 0 && !(ZP.BONSCR[2] & 0x80)) {   /* not attract, and a bonus */
        ad_vgme(0x00, 0x0D);                /* BONUS MESSAGE, ENGLISH ONLY */
        ad_digits(true, (uint8_t)(0x68 + ZP.PLAYR3), 0x03);   /* COMPUTE @ */
    }
    /* PARAMS_23 */
    x = 0x10;                               /* MEDIUM SIZE */
    a = ZP.NPLAYR;
    if (a == 0x01)                          /* NO PLAYER 2 */
        return;
    if (a >= 0x02) {                        /* not ATTRACT */
        if (ZP.PLAYR == 0)                  /* PLAYER 2 not UP: SMALL */
            x = 0x00;
    }
    /* PARAMS_24 */
    ZP.VGSIZE = x;
    ad_vgsabs(0xC0, 0xDB);                  /* POSITION BEAM */
    ad_vgwait(0x50);
    if (ZP.NPLAYR != 0 && ZP.PLAYR != 0 && OBJ(AD_SHIP) == 0
        && !(AD_P->f.SDELAY & 0x80) && (ZP.FRAME[0] & 0x10) == 0)
        goto params_40;                     /* FLASH SCORE */
    /* PARAMS_30 */
    ad_digits(true, 0x65, 0x03);            /* DISPLAY SCORE FOR PLAYER 2 */
params_40:
    y = ad_gtsp(0x01, ZP.HITS[1]);          /* GET SHIP PIX */
    ad_lives(0xCF, y);                      /* DISPLAY NUMBER OF LIVES */
    if (ZP.PLAYR == 0)                      /* PLAYER 2 UP? NOPE */
        return;
    a = OBJ(AD_SHIP);
    if (a == 0 || (a & 0x80))               /* ALIVE? NOPE */
        return;
    ad_two();                               /* DO TWINKLE */
}

/* ------------------------------------------------------------------ */
/* SCORES ($6CB9) - display the high-score table                        */
/* ------------------------------------------------------------------ */
/* Attract mode only, alternating on FRAME+1 bit 2 with the copyright
 * and GAME OVER screen.  True while the table is up. */
bool ad_scores(void)
{
    uint8_t x, a;
    bool c;

    x = ZP.NPLAYR;
    if (x != 0)                             /* NOT ATTRACT MODE */
        return false;
    if ((FRAMEH & 0x04) == 0 && (ZP.HSCORE[0] | ZP.HSCORE[1] | ZP.HSCORE[2]) != 0) {
        /* SCORES_30 */
        ad_vgmsg(0x00);                     /* DISPLAY "HIGH SCORE" MESSAGE */
        ZP.R0 = 0;                          /* ITEM COUNTER (COUNTS BY 1 IN BCD) */
        ZP.R3 = 0;                          /* ITEM COUNTER (COUNTS BY 3 IN BINARY) */
        ZP.R2 = 0xA7;                       /* Y VALUE FOR FIRST LINE OF DISPLAY */
        for (x = 0x23; ; ) {                /* SCORES_20 */
            ZP.VGSIZE = 0x10;               /* CHARACTER SIZE */
            ZP.R1 = x;                      /* SCORE INDEX */
            if ((g.zp.raw[x] | g.zp.raw[x + 1] | g.zp.raw[x + 2]) == 0)
                break;                      /* A 0 ENDS THE SEARCH */
            ad_vgsabs(0x5F, ZP.R2);         /* POSITION BEAM */
            ad_vgwait(0x40);                /* WAIT FOR IT */
            c = false;
            ZP.R0 = bcd_adc(ZP.R0, 0x01, &c);   /* UPDATE BCD ITEM COUNTER */
            ad_digits(true, 0x10, 0x01);    /* the rank, ONLY 2 DIGITS */
            ad_vgdot(0x40, 0x40);           /* PRINT A PERIOD AFTER NUMBER */
            ad_vgchar(0);                   /* PRINT A SPACE AFTER THAT */
            ad_digits(true, ZP.R1, 0x03);   /* the score, 6 DIGITS */
            ad_vgchar(0);                   /* FOLLOW WITH A BLANK */
            ad_in3tim();                    /* DISPLAY 3 INITIALS */
            if (ZP.R0 < 0x04) {             /* ONE OF FIRST 3? */
                a = (uint8_t)(ZP.R2 + 0x02);    /* adc #$02, C = 0: Y POSITION */
                ad_livess(0xB2, a, 0x02);   /* PUT IN JSRL TO SHIP */
            }
            /* SCORES_21 */
            ZP.R2 = (uint8_t)(ZP.R2 - 0x08);    /* COMPUTE Y OF NEXT LINE */
            x = (uint8_t)(ZP.R1 + 3);       /* GET SCORE INDEX, +3 */
            if (x >= 0x41)                  /* END? */
                break;
        }
        return true;                        /* SCORES_80: SIGNAL SCORES ARE UP */
    }
    /* SCORES_90: the copyright and GAME OVER instead */
    ZP.VGSIZE = x;                          /* stx VGSIZE: 0 */
    ad_vgsabs(0x70, 0x68);                  /* POSITION BEAM */
    ad_vgwait(0x70);                        /* WAIT */
    ad_cpyrs();                             /* PUT UP COPYRIGHT */
    ad_vgmsg(0x07);                         /* GAME OVER */
    return false;                           /* SCORES_40 */
}

/* ------------------------------------------------------------------ */
/* UPDATE ($6FF0) - update the high-score table                         */
/* ------------------------------------------------------------------ */

/* UPDATE_30: player X/3's score goes in at entry Y.  Sets the flag to
 * collect their initials, works out how much of the EAROM must be
 * rewritten, shifts the table down, and drops the score in. */
static void update_30(uint8_t x, uint8_t y)
{
    uint8_t a, r0;

    ZP.TEMP2[0] = x;                        /* SAVE INDEX */
    ZP.UPDFLG[x >> 1] = y;                  /* FLAG TO GET PLAYERS INITIALS */
    ZP.R0 = y;                              /* SAVE IT HERE TOO */
    if (y < ZP.EAHSX) {                     /* KEEP LOWER OF 2 */
        ZP.EAHSX = y;
        ZP.R1 = 0;
        a = y;
        for (;;) {                          /* UPDATE_33: DIVIDE BY 3 */
            if (a < 0x03)
                break;
            a = (uint8_t)(a - 0x03);
            ZP.R1++;
        }
        /* UPDATE_32: A = [3 - (ENTRY/3)] * 7 - eor/adc with C = 0 from
         * the failed sbc, then R1*3 and *7 by shift-and-add */
        a = (uint8_t)((ZP.R1 ^ 0xFF) + 0x04);
        ZP.R1 = a;
        ZP.R1 = (uint8_t)(ZP.R1 << 1);
        a = (uint8_t)(a + ZP.R1);
        ZP.R1 = (uint8_t)(ZP.R1 << 1);
        a = (uint8_t)(a + ZP.R1);
        ZP.EABC = a;                        /* KEEP BYTE COUNT */
        ZP.EAX = 0x14;                      /* START @ END */
    }
    /* UPDATE_31: shift the entries below Y down by one */
    r0 = ZP.R0;
    for (x = 0x1B; ; ) {                    /* UPDATE_40 */
        if (x == r0)
            break;                          /* IF END OF COPY */
        ZP.INITL[x]     = ZP.INITL[x - 3];  /* COPY INITIALS DOWN */
        ZP.INITL[x + 1] = ZP.INITL[x - 2];
        ZP.INITL[x + 2] = ZP.INITL[x - 1];
        ZP.HSCORE[x]     = ZP.HSCORE[x - 3];    /* COPY SCORES DOWN */
        ZP.HSCORE[x + 1] = ZP.HSCORE[x - 2];
        ZP.HSCORE[x + 2] = ZP.HSCORE[x - 1];
        x = (uint8_t)(x - 3);
        if (x == 0)
            break;                          /* LOOP UNTIL X=0 OR DONE */
    }
    /* UPDATE_45 */
    ZP.INITL[x] = 0x0B;                     /* START LETTERS AT A */
    ZP.INITL[x + 1] = 0x00;                 /* CLEARS SECOND AND THIRD INITIALS */
    ZP.INITL[x + 2] = 0x00;
    FRAMEH = 0xF0;                          /* 1 MINUTE AT 60HZ: TIMEOUT GETTING INITIALS */
    x = ZP.TEMP2[0];
    ZP.HSCORE[y + 2] = ZP.SCORE[x + 2];
    ZP.HSCORE[y + 1] = ZP.SCORE[x + 1];
    ZP.HSCORE[y]     = ZP.SCORE[x];
}

void ad_update(void)
{
    uint8_t x, y, a;

    if (!(ZP.NPLAYR & 0x80))                /* only the FRAME AFTER END OF GAME */
        return;
    FRAMEH = 0xFF;                          /* PUT UP HIGH SCORE TABLE NEXT */
    ZP.UPDFLG[0] = 0xFF;                    /* CLEAR FLAGS */
    UPDFLG1 = 0xFF;
    ZP.EAHSX = 0xFF;
    ad_inisou();                            /* RESET SOUNDS */
    for (x = 0x03; !(x & 0x80); x = (uint8_t)(x - 3)) {   /* UPDATE_18: each player */
        for (y = 0; y < 0x1E; y = (uint8_t)(y + 3)) {     /* UPDATE_20: ten entries */
            /* three-byte compare, HSCORE - SCORE: borrow = new high */
            int t = (int)ZP.HSCORE[y] - ZP.SCORE[x];
            bool c = t >= 0;
            t = (int)ZP.HSCORE[y + 1] - ZP.SCORE[x + 1] - (c ? 0 : 1);
            c = t >= 0;
            t = (int)ZP.HSCORE[y + 2] - ZP.SCORE[x + 2] - (c ? 0 : 1);
            if (t < 0) {                    /* NEW HIGH SCORE */
                update_30(x, y);
                break;                      /* jmp UPDATE_28 */
            }
        }
    }
    /* UPDATE_28 tail: player 2's index moves down by one entry if
     * player 1 went in above it. */
    a = UPDFLG1;
    if (!(a & 0x80) && a >= ZP.UPDFLG[0]) {
        a = (uint8_t)(a + 0x02 + 1);        /* adc #$02 with C = 1: ADD 3 */
        if (a >= 0x1E)                      /* OUT OF TABLE NOW */
            a = 0xFF;
        UPDFLG1 = a;                        /* NEW INDEX */
    }
    /* UPDATE_29 */
    x = ZP.EAHSX;                           /* ON THE HIGH SCORE TABLE? */
    if (!(x & 0x80)) {
        ad_sndon(0x4F);                     /* NOISE */
        if (ZP.CRMERR != 0) {               /* COPYRIGHT ERROR? */
            /* The bootleg tell-tale: 1 OUT OF 3 TIMES, two FREE GAMES.
             * CRMERR stays 0 on an unmodified list, so this never runs. */
            if (0x55 >= ad_hw_random()) {
                ZP.CRDT++;
                ZP.CRDT++;
            }
        }
        /* UPDATE_97 */
        if (x == 0) {                       /* a new number one: run the check */
            ZP.CPMTST = (uint8_t)(0x80 | (ZP.CPMTST >> 1));   /* sec / ror CPMTST */
            ad_vgrcpt();                    /* TEST */
            ZP.CPMTST >>= 1;
        }
    }
    /* UPDATE_99 */
    ZP.NPLAYR = 0;                          /* FLAG DONE WITH UPDATE */
    ZP.UPDINT = 0;                          /* STARTING WITH FIRST INITIAL */
}

/* ------------------------------------------------------------------ */
/* GETINT ($6587) - get the player's initials for a high score          */
/* ------------------------------------------------------------------ */
/* True while entry is in progress (the ROM returns A = 0), false when
 * there is nothing to enter (A = $FF). */
bool ad_getint(void)
{
    uint8_t a, x, y;
    bool c;

    if (ZP.UPDFLG[0] & UPDFLG1 & 0x80) {    /* nothing to GET */
        if (ZP.NPLAYR == 0)                 /* ATTRACT? */
            ad_inisou();                    /* RESET SOUNDS */
        return false;                       /* MUST RETURN NEGATIVE */
    }
    /* GETINT_10 */
    if ((ZP.LPLAYR >> 1) != 0) {            /* LAST GAME was not a 1 PLAYER GAME */
        ad_vgmsg(0x01);                     /* DISPLAY MESSAGE 1: PLAYER */
        y = 0x02;
        if (UPDFLG1 & 0x80)                 /* not PLAYER 2 */
            y--;
        /* GETINT_20 */
        ZP.PLAYR = y;
        if ((ZP.FRAME[0] & 0x10) == 0)      /* FLASH PLAYER NUMBER */
            ad_vghex(y);                    /* DISPLAY PLAYER NUMBER */
    }
    /* GETINT_25 */
    ZP.PLAYR >>= 1;                         /* 0 OR 1 */
    ad_sbank();                             /* SET BANK FOR PLAYER CONTROLS */
    ad_vgmsg(0x02);                         /* DISPLAY MESSAGE 2 - INSTRUCTIONS */
    ad_vgmsg(0x03);
    ad_vgmsg(0x04);
    ad_vgmsg(0x05);
    ZP.VGSIZE = 0x20;                       /* USE LARGER CHARACTER FOR INITIALS */
    ad_vgsabs(0x64, 0x39);                  /* POSITION BEAM */
    ad_vgwait(0x70);                        /* WAIT FOR BEAM */
    x = ZP.PLAYR;
    y = ZP.UPDFLG[x];
    ZP.TEMP2[0] = y;
    TEMP2H = (uint8_t)(y + ZP.UPDINT);      /* INDEX FOR THE INITIAL WE ARE WORKING ON */
    ad_inital(y);                           /* DISPLAY INITIAL */
    ad_inital((uint8_t)(ZP.TEMP2[0] + 1));
    ad_inital((uint8_t)(ZP.TEMP2[0] + 2));  /* DISPLAY THIRD INITIAL */
    /* GETINT_50: asl HYPSW / rol LASTSW - SWITCH DEBOUNCE */
    c = (ad_hw_switch(0x2003) & 0x80) != 0;
    ZP.LASTSW = (uint8_t)((ZP.LASTSW << 1) | (c ? 1 : 0));
    if ((ZP.LASTSW & 0x1F) == 0x07) {       /* A VALID SWITCH: ADVANCE TO NEXT LETTER */
        ZP.UPDINT++;
        if (ZP.UPDINT >= 0x03) {            /* WE ARE DONE */
            ZP.UPDFLG[ZP.PLAYR] = 0xFF;     /* CLEAR UPDATING FLAG */
            goto getint_54;
        }
        /* GETINT_55 */
        TEMP2H++;
        x = TEMP2H;
        FRAMEH = 0xF4;                      /* RESET TIMEOUT: ABOUT 64 SECONDS */
        ZP.INITL[x] = 0x0B;                 /* SET INITIAL TO A */
    }
    /* GETINT_60 */
    if (FRAMEH == 0) {                      /* TIMEOUT */
        ZP.UPDFLG[0] = 0xFF;
        UPDFLG1 = 0xFF;                     /* STOP INITIALS */
        goto getint_54;
    }
    /* GETINT_65 */
    if ((ZP.FRAME[0] & 0x07) == 0) {        /* EVERY 8TH FRAME */
        x = TEMP2H;
        y = ZP.INITL[x];                    /* GET INITIAL */
        if (ad_hw_switch(0x2407) & 0x80)    /* ROTL: LEFT? */
            y++;
        /* GETINT_70 */
        if (ad_hw_switch(0x2406) & 0x80) {  /* ROTR: RIGHT? */
            y--;
            if (y & 0x80) {                 /* BEFORE A BLANK COMES Z */
                y = 0x24;                   /* GETINT_78 */
                goto getint_80;
            }
        }
        /* GETINT_75 */
        if (y >= 0x0B)                      /* IF GREATER THAN A */
            goto getint_80;
        if (y == 0x01)
            y = 0x0B;                       /* AFTER BLANK COMES A */
        else
            y = 0x00;                       /* BEFORE A COMES A BLANK */
        goto getint_85;
getint_80:
        if (y >= 0x25)                      /* past Z */
            y = 0x00;
getint_85:
        ZP.INITL[x] = y;
    }
    /* GETINT_90 */
    return true;                            /* MUST BE POSITIVE ON RETURN */

getint_54:
    ZP.PLAYR = 0;                           /* SET PLAYER NUMBER */
    ZP.UPDINT = 0;                          /* START NEXT PLAYER */
    FRAMEH = 0xF0;                          /* BRING UP HIGH SCORE TABLE NEXT */
    if (ZP.UPDFLG[0] & 0x80)                /* LAST? */
        ad_stearom();                       /* START EAROM */
    ad_sbank();                             /* SET CONTROLS FOR PLAYER 1 */
    (void)a;
    return true;                            /* A = 0 from SBANK: still "in progress" */
}

/* ------------------------------------------------------------------ */
/* CHKST ($4BD1) - check for the start or end of a game                  */
/* ------------------------------------------------------------------ */

/* CHKST2 ($6102) - "PLAYER n". */
void ad_chkst2(void)
{
    ad_vgmsg(0x01);                         /* DISPLAY "PLAYER" MESSAGE */
    ad_vghex((uint8_t)(ZP.PLAYR + 1));      /* DISPLAY PLAYER NUMBER: 1 OR 2 */
}

/* RTS.4_42 ($4CE6) - start the EAROM write if a high score is pending. */
static void rts4_42(void)
{
    if (ZP.UPDFLG[0] & UPDFLG1 & 0x80)      /* ANY HI-SCORES? NO */
        return;
    ad_stearom();                           /* YES. START EA ROM */
}

/* BM ($4CEF) - a message that blinks while a two-coin minimum is unmet. */
static void bm(uint8_t y)
{
    if ((ZP.TWOCM | ZP.R3) & 0x80) {        /* blink */
        if (ZP.FRAME[0] & 0x20)             /* OFF */
            return;
    }
    ad_vgmsg(y);                            /* BM_1 */
}

/* CHKST1 ($4CFE) - a game is in progress: game over, player change. */
static bool chkst1(void)
{
    uint8_t x;

    if ((ZP.FRAME[0] & 0x3F) == 0) {        /* ONLY EVERY 1 SECOND */
        if (AD_P->f.THUMP3 != 0x08)         /* not AT FASTEST RATE NOW */
            AD_P->f.THUMP3--;
    }
    /* CHKST1_70 */
    x = ZP.PLAYR;
    if (ZP.HITS[x] == 0) {                  /* not STILL IN GAME */
        if ((OBJ(AD_SHPTP) | OBJ(AD_SHPTP + 1) | OBJ(AD_SHPTP + 2) | OBJ(AD_SHPTP + 3)) == 0) {
            ad_vgmsg(0x07);                 /* GAME OVER MESSAGE */
            if (ZP.NPLAYR >= 0x02)          /* not a 1 PLAYER GAME */
                ad_chkst2();                /* DISPLAY PLAYER NUMBER */
        }
    }
    /* CHKST1_60 */
    if (OBJ(AD_SHIP) != 0)                  /* STILL ALIVE */
        return false;
    /* CHKST1_62 */
    if (AD_P->f.SDELAY != 0x80)             /* SHIP RETURNING TO LIFE */
        return false;
    ad_ssbtlt(0x10);
    x = ZP.NPLAYR;
    if ((ZP.HITS[0] | ZP.HITS[1]) == 0) {   /* GAME IS ALL OVER: CHKST1_90 */
        ZP.LPLAYR = x;                      /* SAVE NUMBER OF PLAYERS IN THIS GAME */
        ZP.NPLAYR = 0xFF;                   /* FLAG TO UPDATE HIGH SCORES */
        ZP.LOUT1 = 0x03;                    /* LET NMI WRITE TO LAMPS */
        ad_bnksel(0x03);                    /* PLAYER 1 */
        return false;
    }
    ad_rsaucr();                            /* RESET SAUCER VALUES */
    /* CHKST1_61 */
    x--;
    if (x == 0)                             /* ONE PLAYER NO MESSAGE NEEDED */
        return false;
    ZP.GDELAY = 0x80;                       /* DELAY BEFORE STARTING PLAYER */
    x = (uint8_t)(ZP.PLAYR ^ 0x01);         /* 1 TO 0 AND 0 TO 1 */
    if (ZP.HITS[x] == 0)                    /* NO HITS FOR THIS PLAYER */
        return false;
    ZP.PLAYR = x;                           /* SET PLAYER NUMBER */
    ad_bnksel((uint8_t)(x << 7));           /* txa / ror / ror: SWITCH PLAYERS */
    ZP.PLAYR2 = (uint8_t)(x << 1);
    ZP.PLAYR3 = (uint8_t)((x << 1) | ZP.PLAYR);
    return false;                           /* CHKST1_80 */
}

bool ad_chkst(void)
{
    uint8_t a, x, y;
    bool c;

    if (ZP.NPLAYR != 0) {                   /* GAME IN PROGRESS */
        if (ZP.GDELAY != 0) {               /* STAY in READY MODE */
            ZP.GDELAY--;
            ad_chkst2();                    /* DISPLAY PLAYER NUMBER MESSAGE */
            return false;
        }
        return chkst1();                    /* NOT IN PLAYER READY MODE */
    }
    /* CHKST_10 */
    ZP.R3 = 0;                              /* 2 COIN MINIMUM FLAG */
    a = (uint8_t)(ZP.CMODE & 0x03);         /* GET PRICE CODE */
    if (a == 0) {                           /* FREE PLAY */
        ZP.CRDT = 0x02;                     /* ALWAYS HAVE 2 CREDITS */
        goto chkst_15;
    }
    /* CHKST_35 */
    y = (uint8_t)(a + 0x07);                /* the price message */
    if (ZP.UPDFLG[0] & UPDFLG1 & 0x80) {    /* not UPDATING HI-SCORE */
        ad_vgmsg(y);                        /* GAME PRICE */
        a = ZP.CRDT;
        /* cmp #$40 / bcs . : a BAD VALUE hangs here until the watchdog
         * resets the board.  RAM would have to be corrupt; the port
         * carries on rather than spinning. */
        ZP.R0 = a;                          /* SAVE IT */
        ad_vgmsg(0x0B);                     /* CREDIT MESSAGE */
        /* Binary to BCD by shift-and-add in decimal mode: R1 collects
         * R2 (1, 2, 4, 8 ... in BCD) for each set bit of R0.  The loop
         * ends on Z from `lsr R0` when no bit was added, or never
         * directly after an add (Z is R1's then). */
        ZP.R1 = 0x00;                       /* CLEAR PRODUCT */
        ZP.R2 = 0x01;                       /* CONVERTER */
        for (;;) {                          /* CHKST_19 */
            bool z;
            c = (ZP.R0 & 0x01) != 0;        /* GET DATA BIT */
            ZP.R0 >>= 1;
            z = (ZP.R0 == 0);
            if (c) {                        /* ACCUMULATE IT */
                bool cc = false;
                ZP.R1 = bcd_adc(ZP.R1, ZP.R2, &cc);
                z = (ZP.R1 == 0);
            }
            if (z)                          /* CHKST_17: DONE */
                break;
            c = false;                      /* (CARRY S/B CLEAR HERE) */
            ZP.R2 = bcd_adc(ZP.R2, ZP.R2, &c);  /* DOUBLE CONVERTER VALUE */
        }
        /* CHKST_18: cld */
        ad_digits(true, 0x11, 0x01);        /* DISPLAY VALUE IN R1, ZERO SUPPRESS */
        if ((ZP.CMODE & 0x03) == 0x03) {    /* 50 CENT PLAY? */
            if (ZP.CNCT & 0x01)             /* ANY 1/2 CREDITS? */
                ad_vgadd2(0x20, 0xCB);      /* DRAW IN A 1/2: JSRL Z.5 */
        }
    }
chkst_15:
    y = 0x06;                               /* ASSUME 'PUSH START' */
    x = ZP.CRDT;                            /* CREDIT? */
    if (x == 0)
        ZP.TWOCM = 0;                       /* RESET 2 COIN MIN FLAG */
    /* CHKST_16 */
    if (ad_hw_switch(0x2801) & 0x01) {      /* OPTN2: 2 COIN MINIMUM OPTION */
        if (!(ZP.TWOCM & 0x80)) {           /* MINIMUM not MET */
            if (x < 0x01)                   /* 0 CREDITS */
                goto chkst_13;
            if (x == 0x01) {                /* 1 CREDIT: ror R3 with C = 1 */
                ZP.R3 = (uint8_t)(0x80 | (ZP.R3 >> 1));
chkst_13:
                y = 0x0C;                   /* '2 COIN MINIMUM' MESSAGE */
                bm(y);                      /* CHKST_21: DISPLAY MESSAGE */
                goto chkst_11;
            }
        }
    }
    /* CHKST_14 */
    ZP.TWOCM = 0xFF;                        /* SIGNAL MINIMUM MET */
    /* CHKST_12 */
    if (x == 0)                             /* ANY CREDITS? NO DON'T START */
        goto chkst_11;
    bm(y);                                  /* BLINK 'PRESS START' */
    y = ZP.CRDT;                            /* GET CREDITS */
    x = 0x01;
    a = ad_hw_switch(0x2403);               /* STRT1 */
    if (!(a & 0x80)) {                      /* not a ONE PLAYER START */
        if (y < 0x02)                       /* ONLY 1 CREDIT */
            goto chkst_40;
        a = ad_hw_switch(0x2404);           /* STRT2 */
        if (!(a & 0x80))                    /* NO START YET */
            goto chkst_40;
        ad_bnksel(a);                       /* sta BNKSEL: SWITCH TO PLAYER 2 */
        rts4_42();                          /* START EA ROM */
        ad_init();                          /* REINITIALIZE MEMORY */
        ad_newast();                        /* NEW ASTEROIDS */
        ad_newshp();                        /* PUT SHIP IN MIDDLE */
        ZP.HITS[1] = ZP.NHITS;              /* ENABLE 2ND PLAYER */
        x = 0x02;
        ZP.CRDT--;                          /* ONE CREDIT LESS */
    }
    /* CHKST_20 */
    ZP.NPLAYR = x;                          /* NUMBER OF PLAYERS */
    ZP.CRDT--;                              /* ONE CREDIT LESS */
    ad_bnksel(x);                           /* PLAYER 1 */
    ZP.LOUT1 = x;                           /* SET LAMP CODE */
    rts4_42();                              /* CHECK FOR EA START */
    ad_init();                              /* INITIALIZE MEM */
    ad_newshp();                            /* PUT SHIP IN MIDDLE */
    ZP.GDELAY = 0x80;
    ZP.PLAYR = 0;                           /* asl a: 0 */
    ZP.PLAYR2 = 0;
    ZP.PLAYR3 = 0;
    ZP.BC = 0;                              /* RESET BONUS ACCUMULATOR */
    ZP.BCCNT = 0;                           /* RESET BONUS COUNTER */
    ZP.HITS[0] = ZP.NHITS;                  /* NUMBER OF HITS ALLOWED */
    ZP.THUMP2 = 0x04;
    ad_hw_noise_reset();                    /* RESET NOISE GENERATOR */
    return true;                            /* EXIT WITH CARRY SET */

chkst_40:
    if ((ZP.FRAME[0] & 0x0F) == 0) {        /* change the start lamps */
        a = 0x01;                           /* ASSUME 2 PLAYER? */
        if (ZP.CRDT != 0x01)                /* MORE THAN 1 CREDIT */
            a = 0x03;
        ZP.LOUT1 ^= a;                      /* LET NMI WRITE TO OUT1 */
    }
    return false;                           /* CHKST_45 */

chkst_11:
    ZP.LOUT1 = 0;                           /* NO LAMPS, BUT DON'T START */
    return false;
}
