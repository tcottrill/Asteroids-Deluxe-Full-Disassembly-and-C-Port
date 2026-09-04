/* Sound - DASOUN.MAC ($7752-$77DD, with INISOU at $784F), SOUNDS ($6F12)
 * and DIASND ($49A4).
 *
 * Two mechanisms, described in disasm/SOUND_NOTES.md: a table-driven
 * sequencer with eight channels, one per POKEY register, that CSOUND
 * steps from the NMI; and the discrete explosion and thrust circuits,
 * which SOUNDS drives directly once a frame.
 *
 * All sequencer state is the six parallel page-zero arrays POINT,
 * CURRENT, FRAMES, COUNT, MCOUNT and MPNTR, indexed by channel 0-7.
 * POINT == 0 means the channel is free; MCOUNT bit 7 means "start".
 *
 * The data block DASOUN ($760D) begins with PNTRS, twelve rows of eight
 * bytes - one row per sound, one byte per channel - giving each channel
 * its offset into the sequence records that follow.  A sound number is
 * its row's last index (row*8+7), which is why SNDOO walks Y downward.
 * The records are read by CSOUND as
 *
 *     lda $766D,y     FCNT   interrupts between changes (0 ends a group)
 *     lda $766E,y     START  initial value
 *     lda $766F,y     CHNG   added each time
 *     lda $7670,y     TOT    number of changes
 *
 * so the offsets in PNTRS are relative to $766D = DASOUN + $60, and each
 * channel's sequence begins with a one-byte repeat count for the group
 * of records that follows it.
 */
#include "astdelux.h"

/* $766D - where CSOUND's four absolute,Y reads are based. */
#define AD_SNDREC   (AD_DASOUN + 0x60)

/* ------------------------------------------------------------------ */
/* The starter                                                         */
/* ------------------------------------------------------------------ */

/* SNDOO_4 ($7761) - the loop the three entry points share.
 *
 * X counts 7 down to 0 and Y follows it down the sound's PNTRS row.
 * `priority` is the carry (SNDPON sets it: leave a busy channel alone)
 * and `off` is the overflow flag SNDOFF raises with `bit SNDOO_1`,
 * which turns every offset into a zero.  X is pushed and pulled around
 * the loop in the ROM; here it is a local, so nothing to preserve.
 */
static void sndoo_4(uint8_t y, bool priority, bool off)
{
    uint8_t x = 7;                                  /* LOOP COUNTER */
    do {
        /* SNDOO_3: bcc SNDOO_5 / lda POINT,x / bne SNDOO_6 */
        if (!(priority && g.zp.f.POINT[x] != 0)) {
            /* SNDOO_5 */
            uint8_t a = ad_rom((uint16_t)(AD_DASOUN + y));   /* GET OFFSET */
            if (a != 0) {                           /* NONE. LEAVE CHANNEL */
                if (off)
                    a = 0;                          /* TURN SOUND OFF */
                /* SNDOO_7.  The listing's own comment: "NMI'S ARE
                 * RUNNING SO WE HAVE TO DO THE TURN ON IN A SPECIFIC
                 * ORDER.  FIRST WE TURN OFF ANY SOUNDS BY ZAPPING
                 * POINT.  THEN WE SET 'MCOUNT' TO '80' TO SIGNAL CSOUND
                 * TO START A SOUND.  'MCOUNT' IS NOT DISTURBED UNLESS
                 * POINT IS NOT ZERO.  LASTLY WE SET POINT TO THE
                 * REQUIRED OFFSET."  Kept in that order, because a host
                 * may run CSOUND on another thread just as the NMI did. */
                g.zp.f.POINT[x] = 0;
                g.zp.f.MCOUNT[x] = 0x80;
                g.zp.f.POINT[x] = a;
            }
        }
        /* SNDOO_6 */
        y--;                                        /* COUNT */
        x--;                                        /* COUNT */
    } while (!(x & 0x80));                          /* bpl SNDOO_3 */
}

/* SNDOFF ($7752) - silence a sound.  Y = sound number.
 *
 *     bit SNDOO_1         SEV: the RTS opcode $60 has bit 6 set
 *     clc / bcc SNDOO_4
 *
 * Note it skips the NPLAYR test, and that it still writes MCOUNT = $80
 * on the way to zeroing POINT; CSOUND never looks at MCOUNT while
 * POINT is zero, so that is harmless.
 */
void ad_sndoff(uint8_t y)
{
    sndoo_4(y, false, true);
}

/* SNDPON ($7758) - start a sound, but only on channels that are idle. */
void ad_sndpon(uint8_t y)
{
    ad_sndoo(y, true);                              /* sec / bcs SNDOO */
}

/* SNDON ($775B) - start a sound, taking the channels over. */
void ad_sndon(uint8_t y)
{
    ad_sndoo(y, false);                             /* clc */
}

/* SNDOO ($775C) - Y = sound number, C = priority.
 *
 *     lda NPLAYR / beq SNDOFF     ATTRACT MODE?  YEP. TURN SOUNDS OFF INSTEAD
 *     clv                         BE SURE THE V FLAG IS OFF
 *
 * so in attract mode every request to start a sound stops it instead.
 * Nothing but SNDOFF ever arrives with V set.
 */
void ad_sndoo(uint8_t y, bool priority)
{
    if (g.zp.f.NPLAYR == 0) {
        ad_sndoff(y);
        return;
    }
    sndoo_4(y, priority, false);
}

/* ------------------------------------------------------------------ */
/* The sequencer                                                       */
/* ------------------------------------------------------------------ */

/* CSOUND ($7787) - step all eight channels; once per NMI.
 *
 * Translated with the listing's labels as gotos, because the routine
 * is a state machine with four shared tails (CSOUND_7, _5, _6 and _1)
 * entered from different depths, and the structured rewrite would hide
 * which path a change to one of them affects.  Y is the channel's
 * POINT, an offset from $766D.
 *
 * Every channel, busy or not, ends at CSOUND_1 with `sta POKEY,x`, so
 * an idle channel writes zero to its register every interrupt.
 */
void ad_csound(void)
{
    uint8_t x = 7;                                  /* 8 ITEMS */
    do {
        uint8_t y = g.zp.f.POINT[x];                /* CSOUND_4: ANY SOUND? */
        uint8_t a = 0;

        if (y == 0)
            goto CSOUND_6;                          /* NO */
        if (g.zp.f.MCOUNT[x] & 0x80)                /* ARE WE TO START ONE? */
            goto CSOUND_11;                         /* YES. GO START IT */
        if (--g.zp.f.FRAMES[x] != 0)                /* COUNT DOWN TIMER */
            goto CSOUND_1;                          /* CONTINUE */
        if (--g.zp.f.COUNT[x] == 0)                 /* COUNT DOWN TOTAL COUNTER */
            goto CSOUND_2;                          /* DONE */
        /* CHANGE SOUND BY ADDING CHANGE CODE: lda CURRENT,x / clc / adc $766F,y */
        a = (uint8_t)(g.zp.f.CURRENT[x] + ad_rom((uint16_t)(AD_SNDREC + 2 + y)));
        goto CSOUND_5;

    CSOUND_2:
        y = (uint8_t)(y + 4);                       /* Y=Y+4: the next record */

    CSOUND_7:
        g.zp.f.POINT[x] = y;
        g.zp.f.COUNT[x] = ad_rom((uint16_t)(AD_SNDREC + 3 + y));    /* RESET TOTAL COUNTER */
        a = ad_rom((uint16_t)(AD_SNDREC + 1 + y));

    CSOUND_5:
        g.zp.f.CURRENT[x] = a;                                      /* RESET VALUE */
        g.zp.f.FRAMES[x] = ad_rom((uint16_t)(AD_SNDREC + y));       /* RESET TIMER */
        if (g.zp.f.FRAMES[x] != 0)
            goto CSOUND_1;                          /* CONTINUE */
        /* A zero FCNT ends the group of records. */
        y = g.zp.f.MPNTR[x];                        /* SET UP Y IN CASE OF RESTART */
        if (--g.zp.f.MCOUNT[x] != 0)                /* MACRO COUNT */
            goto CSOUND_7;                          /* RESTART */
        y = (uint8_t)(g.zp.f.POINT[x] + 1);         /* SKIP OVER TERM */
        if (y != 0)                                 /* bne CSOUND_11 (ALWAYS) */
            goto CSOUND_11;
        goto CSOUND_10;                             /* A is 0 here: same exit */

    CSOUND_11:
        a = ad_rom((uint16_t)(AD_SNDREC + y));      /* GET COUNT: the group's repeat count */
        if (a == 0)
            goto CSOUND_10;                         /* END */
        /* CSOUND_9 */
        g.zp.f.MCOUNT[x] = a;
        y++;                                        /* SKIP UP TO SOUND */
        g.zp.f.MPNTR[x] = y;                        /* SAVE POINTER */
        goto CSOUND_7;                              /* (ALWAYS) */

    CSOUND_10:
        y = a;                                      /* tay: THIS MAKES AN EASY EXIT */
        /* CSOUND_8 */
        g.zp.f.POINT[x] = y;                        /* ZAP THE POINTER */

    CSOUND_6:
        g.zp.f.CURRENT[x] = y;                      /* ZAP SOUND TOO */

    CSOUND_1:
        ad_hw_pokey_write(x, g.zp.f.CURRENT[x]);    /* WRITE TO POKEY */
        x--;                                        /* COUNT */
    } while (!(x & 0x80));                          /* bpl CSOUND_4 */
}

/* INISOU ($784F) - silence everything.
 *
 *     lda #0 / ldx #7 / 1$: sta POINT,x / dex / bpl 1$ / sta AUDCTL
 *
 * The listing shows the last store as `sta OPTN5`: $2C08 is the option
 * switches on a read and AUDCTL on a write.
 */
void ad_inisou(void)
{
    for (int x = 7; x >= 0; x--)
        g.zp.f.POINT[x] = 0;
    ad_hw_pokey_write(0x08, 0);                     /* AUDCTL */
}

/* ------------------------------------------------------------------ */
/* The main-line half                                                  */
/* ------------------------------------------------------------------ */

/* SOUNDS ($6F12) - decide what wants to play this frame.
 *
 * Extra-life (SND3) and special-rock (HSSND) requests wait for channel
 * 7 to be idle; the thump alternates between the two TSS sounds with
 * THUMP1 frames on and THUMP2 (reloaded from THUMP3) frames off; then
 * the explosion level decays by one a frame and the thrust switch is
 * passed through to the thrust circuit.  Like CSOUND, laid out with
 * the listing's labels: SOUNDS_27, _30 and _50 are reached from
 * several places each.
 *
 * R0 is the thrust flag.  `lsr R0` at the top assumes none, and
 * `asl THRUST / ror R0` - a read-modify-write on an input port, whose
 * only effect is to put bit 7 of the read into carry - shifts the
 * switch in as the new bit 7.  Only bit 7 is ever looked at.
 */
void ad_sounds(void)
{
    uint8_t x, a;

    g.zp.f.R0 >>= 1;                                /* ASSUME NO THRUST */
    x = g.zp.f.NPLAYR;
    if (x == 0)
        goto SOUNDS_60;                             /* ATTRACT. RESET EXPLOSION */
    x = g.zp.f.POINT[7];                            /* ldx $A9: CHANNELS 6 AND 7 MUST NOT BE BUSY */
    if (x != 0)
        goto SOUNDS_25;                             /* NO SUCH LUCK. WAIT UNTIL NEXT TIME */
    if (g.zp.f.SND3 != 0) {                         /* ANY EXTRA LIFE NOISE REQ? */
        ad_sndon(0x2F);                             /* YEP. START IT */
        g.zp.f.SND3 = x;                            /* stx: X is still 0 - CLEAR THE FLAG */
        goto SOUNDS_25;                             /* (ALWAYS) */
    }
    /* SOUNDS_24 */
    if (g.zp.f.HSSND != 0) {                        /* ANY HI SCORE NOISE REQ? */
        ad_sndon(0x5F);                             /* (the special rock's, in fact) */
        g.zp.f.HSSND = x;                           /* CLEAR FLAG FOR NEXT TIME */
    }

SOUNDS_25:
    a = AD_P->f.SHPPIX;                             /* SHIP? */
    if (a & 0x80)
        goto SOUNDS_27;                             /* SHIP IS DESTROYED */
    if (a == 0)
        goto SOUNDS_27;                             /* SHIP NOT ON SCREEN YET */
    /* asl THRUST / ror R0: SIGNAL THRUST */
    g.zp.f.R0 = (uint8_t)((g.zp.f.R0 >> 1) | (ad_hw_switch(0x2405) & 0x80));
    if (AD_P->f.NROCKS == 0)
        goto SOUNDS_27;                             /* IF NOT ROCKS */
    if (g.zp.f.THUMP1 == 0)
        goto SOUNDS_30;                             /* IF THUMP SOUND IS OFF */
    if (--g.zp.f.THUMP1 != 0)
        goto SOUNDS_50;                             /* LEAVE SOUND ON */

SOUNDS_27:
    a = AD_P->f.THUMP3;
    g.zp.f.THUMP2 = a;                              /* AT LEAST 8 FRAMES BEFORE ITS ON AGAIN */
    if (!(a & 0x80))                                /* bpl SOUNDS_50: "ALWAYS" */
        goto SOUNDS_50;

SOUNDS_30:
    if (--g.zp.f.THUMP2 != 0)
        goto SOUNDS_50;                             /* LEAVE SOUND OFF */
    g.zp.f.THUMP1 = 0x04;                           /* NUMBER FRAMES FOR SOUND TO BE ON */
    g.zp.f.LTHUMP++;                                /* CHEAP WAY TO TOGGLE BIT 0 */
    ad_sndpon(ad_rom((uint16_t)(AD_TSS + (g.zp.f.LTHUMP & 0x01))));

SOUNDS_50:
    a = g.zp.f.LEXPSND;
    x = a;
    if ((a & 0x3F) != 0)                            /* IF NO EXPLOSION VOLUME */
        x--;                                        /* the low six bits (volume in
                                                     * 2-5) count down one a frame;
                                                     * the pitch bits 6-7 stay */

SOUNDS_60:
    g.zp.f.LEXPSND = x;
    ad_hw_explosion(x);                             /* SET SOUND FREQUENCY/LEVEL */
    ad_hw_thrust((g.zp.f.R0 & 0x80) != 0);          /* sta SPTEN: the latch takes D7 */
}

/* DIASND ($49A4) - the diamond sound, when the last special rock of a
 * cluster goes.  Y is saved and restored around the SNDON in the ROM;
 * nothing to do here. */
void ad_diasnd(void)
{
    if (--AD_P->f.SPROCK != 0)
        return;                                     /* NOT LAST ONE */
    ad_sndon(0x3F);                                 /* DIAMOND NOISE */
}
