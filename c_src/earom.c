/* EAROM interface - EAROM.MAC, $7B11-$7BF4.
 *
 * The high-score table (three entries of three score bytes and three
 * initials) lives in an electrically-alterable ROM that is slow to
 * erase and write, so nothing blocks on it.  EAUPD is a state machine
 * the main line steps once per frame; EAFLG says what it is doing:
 *
 *     0     idle          every step just deselects the chip
 *     $80   erasing       one address per step, EAX counting down
 *     $40   writing       one byte per step, EAX counting up
 *     $20   reading       one byte per step, EAX counting up
 *
 * EAROM.MAC gives the control-latch bits and what the C1/C2 pair means:
 *
 *     EACK = 1   clock
 *     EAC2 = 2   C2
 *     EAC1 = 4   C1 (inverted to the chip)
 *     EACE = 8   chip select
 *                         C1 C2:  0 0 read   1 0 write   1 1 erase
 *
 * The 21 bytes are laid out as EABDS ($7BD2) describes: for each entry,
 * the three score bytes (offsets 0,1,2 from HSCORE), the three initials
 * (INITL-HSCORE = $21, +1, +2), then the terminator $FF, which stands
 * for the entry's checksum byte.  EABC indexes EABDS, EAHSX is the
 * entry's base index into HSCORE (0, 3, 6), EACS the running checksum.
 *
 * Who primes the machine:
 *   - START ($6003) stores EABC=EAX=EAHSX=0, EAFLG=$20 to read it all,
 *     unless an erase is still running (mainline.c ad_start).
 *   - UPDATE ($7089) leaves EAHSX = the lowest entry that changed,
 *     EABC = (3 - entry/3) * 7 bytes to erase and EAX = $14, so an
 *     erase walks *down* from the last address over just the entries
 *     that changed, and the write that follows resumes upward from
 *     where the erase stopped.  STEAROM then sets EAFLG bit 7 - or
 *     widens the job to the whole ROM if a read ever failed (EABAD).
 *   - STEST6 ($7F3C) does the whole-ROM erase directly from self-test.
 */
#include "astdelux.h"

#define EACK 0x01
#define EAC2 0x02
#define EAC1 0x04
#define EACE 0x08

/* ------------------------------------------------------------------ */
/* Hardware access                                                     */
/* ------------------------------------------------------------------ */
/* Every EAROM access goes through these three, so a probe build can
 * see the sequence the ROM produces.  The trace hook only exists when
 * probe_earom.c is compiled in (build_mod.bat defines AD_PROBE then);
 * a normal build has no trace at all. */
void (*ad_earom_trace)(char kind, uint8_t x, uint8_t a) = 0;
#define EA_TRACE(kind, x, a) \
    do { if (ad_earom_trace) ad_earom_trace((kind), (x), (a)); } while (0)

/* sta EACTL. */
static void eactl(uint8_t a)
{
    EA_TRACE('C', 0, a);
    ad_hw_earom_ctl(a);
}

/* sta EADAL,x - the ROM's one store that carries *two* things to the
 * hardware: X is the EAROM address (it selects $3200+X, and the board
 * takes the low address bits as the chip's address) and A is the data
 * byte on the bus.  Erase and read only care about the address; the
 * write needs both, and both now reach the host: ad_hw_earom_write(x, a)
 * takes the address and the data byte together, so this is a straight
 * pass-through. */
static void eadal(uint8_t x, uint8_t a)
{
    EA_TRACE('D', x, a);
    ad_hw_earom_write(x, a);
}

/* lda EAIN. */
static uint8_t eain(void)
{
    uint8_t a = ad_hw_earom_read();
    EA_TRACE('R', 0, a);
    return a;
}

/* HSCORE,x and INITL,x with x running past the arrays.  EABDS carries
 * INITL-HSCORE = $21 as an offset, so `lda HSCORE,x` with x = $21..$2B
 * is how the ROM reaches the initials: index page 0 by address, as the
 * 6502 does, rather than the C arrays. */
#define HSCORE_AT(x) g.zp.raw[0x23 + (x)]
#define INITL_AT(x)  g.zp.raw[0x44 + (x)]

/* ------------------------------------------------------------------ */
/* EAUPD ($7B11) - one step of the EAROM state machine.               */
/* ------------------------------------------------------------------ */
/* Called from the main line every frame ($608B) and from the self-test
 * ($7F5D, which fakes INTCT += 4 because NMIs are off there).  A, X, Y
 * are clobbered; nothing is returned.
 *
 * The structure below is the listing's: the three modes each end at
 * EAUPD_2 with the value to leave in the control latch in A, and the
 * read and write modes share the address-advance at EAUPD_4.  The
 * variable `a` is that A. */
void ad_eaupd(void)
{
    uint8_t a, x, y;

    /* CAN ONLY DO THIS ONCE EVERY 16 INTERRUPTS
     *
     *     lda INTCT / and #$0C / bne EAUPD_1
     *
     * INTCT counts NMIs; the frame loop runs every four, so this
     * admits one step in every four frames. */
    if (g.zp.f.INTCT & 0x0C)
        return;                                     /* EAUPD_1 */

    a = g.zp.f.EAFLG;                               /* ANY ACTIVITY? */
    if (a == 0) {
        eactl(0);                                   /* EAUPD_2: NO. DESELECT EA ROM AND EXIT */
        return;
    }

    if (a & 0x80) {
        /* ERASE.  bpl EAUPD_3 not taken.
         *
         *     ldx EAX / lda #EAC1+EAC2 / sta EACTL / sta EADAL,x
         *
         * The mode goes to the control latch with the chip deselected,
         * then the address is latched, then the chip is selected. */
        x = g.zp.f.EAX;
        a = EAC1 | EAC2;
        eactl(a);                                   /* DE-SELECT */
        eadal(x, a);                                /* STORE ADDRESS */
        a = EAC1 | EAC2 | EACE;
        g.zp.f.EAX--;                               /* NEXT @ */
        if (--g.zp.f.EABC == 0) {                   /* COUNT */
            g.zp.f.EAFLG = 0x40;                    /* SELECT WRITE MODE */
            g.zp.f.EAX++;                           /* EAUPD_9: back to the last erased */
        }
        eactl(a);                                   /* EAUPD_2: DO EA CONTROL */
        return;
    }

    /* EAUPD_3: read or write.
     *
     *     ldy EABC / bne EAUPD_5 / sty EACS
     *
     * A zero byte count is the first byte of an entry, and resets the
     * checksum (Y is that zero). */
    y = g.zp.f.EABC;                                /* GET BYTE POINTER */
    if (y == 0)
        g.zp.f.EACS = 0;                            /* RESET CHECKSUM */

    /* EAUPD_5:  asl a / bpl EAUPD_10
     *
     * Shifting EAFLG left puts bit 6 (write) in N.  The shift also
     * clears carry, and every ADC below leans on that: none of them is
     * preceded by a CLC. */
    if (a & 0x40) {
        /* WRITE. */
        uint8_t off = ad_rom(AD_EABDS + y);         /* GET @ OF @ */
        if (off & 0x80) {
            /* The $FF terminator: this byte is the checksum.
             *
             *     sta EABC              EABC = $FF, so the INC below
             *                           wraps it to 0 for the next entry
             *     inc EAHSX x3          UP TO NEXT INITIAL
             *     lda EACS / bcc EAUPD_7 (ALWAYS) */
            g.zp.f.EABC = off;                      /* RESET BYTE COUNT */
            g.zp.f.EAHSX += 3;
            a = g.zp.f.EACS;                        /* GET CHECKSUM */
        } else {
            /* EAUPD_6:  adc EAHSX / tax
             *           lda HSCORE,x / adc EACS / sta EACS / lda HSCORE,x
             *
             * Carry is clear on both ADCs: off + EAHSX is at most
             * $23 + 8, so the first never carries out. */
            x = (uint8_t)(off + g.zp.f.EAHSX);      /* COMPUTE SOURCE ADDR */
            g.zp.f.EACS = (uint8_t)(HSCORE_AT(x) + g.zp.f.EACS);  /* ACCUMULATE CHKSUM */
            a = HSCORE_AT(x);
        }
        /* EAUPD_7 */
        g.zp.f.EABC++;
        x = g.zp.f.EAX;                             /* @INTO EA ROM TO PUT DATA */
        eadal(x, a);                                /* SET UP ADDRESS/DATA */
        eactl(EAC1);                                /* SELECT WRITE MODE */
        a = EAC1 | EACE;                            /* what EAUPD_2 will store */
    } else {
        /* EAUPD_10: READ.
         *
         *     ldx EAX / lda #EACE / sta EADAL,x     SELECT @
         *     sta EACTL                             SELECT READ MODE
         *     lda #EACE+EACK / sta EACTL            clock it
         *     lda #EACE / sta EACTL
         *     ldx #0                                NEED A 0 LATER */
        uint8_t off;
        x = g.zp.f.EAX;
        a = EACE;
        eadal(x, a);
        eactl(EACE);
        eactl(EACE | EACK);
        eactl(EACE);
        x = 0;
        off = ad_rom(AD_EABDS + y);                 /* GET @ OF @ */
        if (off & 0x80) {
            /* The terminator: read the entry's checksum and compare.
             *
             *     stx EABC / lda EAIN / stx EACTL / eor EACS / beq EAUPD_12 */
            g.zp.f.EABC = x;                        /* RESET BYTE COUNT */
            a = eain();                             /* READ ROM DATA */
            eactl(x);                               /* DE-SELECT CHIP */
            if ((a ^ g.zp.f.EACS) != 0) {           /* MATCH CHKSUM? */
                /* CHECKSUM ERROR. IGNORE ROM DATA, CLEAR RAM DATA
                 *
                 *     ldy #2 / tya / adc EAHSX / tax / lda #0
                 *     EAUPD_13: sta HSCORE,x / sta INITL,x / dex / dey / bpl EAUPD_13
                 *
                 * Carry is still clear from the ASL (EOR leaves it
                 * alone), so X = EAHSX + 2 and the loop blanks the three
                 * score bytes and three initials of this entry, top
                 * down.  EAHSX is *not* advanced on this path: the next
                 * entry's bytes land in the same RAM slots. */
                x = (uint8_t)(2 + g.zp.f.EAHSX);    /* COMPUTE DST @ */
                for (y = 2; ; y--, x--) {
                    HSCORE_AT(x) = 0;               /* BLANK OUT SCORE AND INITIALS */
                    INITL_AT(x) = 0;
                    if (y == 0)
                        break;
                }
                g.zp.f.EABAD++;                     /* SIGNAL WE GOT A BAD ENTRY */
            } else {
                /* EAUPD_12 */
                g.zp.f.EAHSX += 3;                  /* TO NEXT GROUP OF SCORE/INITIALS */
            }
        } else {
            /* EAUPD_11:  adc EAHSX / tay
             *            lda EAIN / stx EACTL / sta HSCORE,y
             *            adc EACS / sta EACS / inc EABC / lda #0
             *
             * Carry is clear on both ADCs, as in the write. */
            y = (uint8_t)(off + g.zp.f.EAHSX);      /* COMPUTE DST, SAVE IT */
            a = eain();                             /* GET ROM DATA */
            eactl(x);                               /* DE-SELECT CHIP */
            HSCORE_AT(y) = a;                       /* PASS IT */
            g.zp.f.EACS = (uint8_t)(a + g.zp.f.EACS);  /* ACCUMULATE CHECKSUM */
            g.zp.f.EABC++;                          /* NEXT BYTE */
        }
        /* EAUPD_14:  ldx EAX / bpl EAUPD_4 (ALWAYS)
         *
         * A is 0 on all three read paths when they get here (the
         * `beq` result, the `lda #0`s), so the read leaves the chip
         * deselected at EAUPD_2. */
        a = 0;                                      /* DE-SELECT */
        x = g.zp.f.EAX;
    }

    /* EAUPD_4:  inx / cpx #21. / bcc EAUPD_9
     *           ldx #0 / stx EAFLG / beq EAUPD_2 (ALWAYS)
     *
     * Past the last address the activity flag drops, but A - $0C for a
     * write, 0 for a read - still goes to the control latch. */
    x++;
    if (x < 0x15) {
        g.zp.f.EAX++;                               /* EAUPD_9: NEXT ADDRESS */
    } else {
        g.zp.f.EAFLG = 0;                           /* YES. RESET ACTIVITY FLAG */
    }
    eactl(a);                                       /* EAUPD_2: DO EA CONTROL */
}

/* ------------------------------------------------------------------ */
/* STEAROM ($7BD9) - start an erase-then-write.                        */
/* ------------------------------------------------------------------ */
/* Called from CHKST's RTS.4_42 ($4CEC, a jmp) and GETINT ($6622) once
 * the high-score table has been updated in RAM.  EAHSX, EABC and EAX
 * are expected to be primed already (UPDATE, see the file comment);
 * this only sets the mode - and, when a read left a bad entry behind,
 * rewrites the whole ROM instead.  Y is clobbered.
 *
 *     ldy EAFLG / bne STEAROM_1        ALREADY WORKING? YEP. NEVER MIND
 *     lda #3*3-1 / cmp EAHSX / bcc     FIT IN ROM?  8 >= EAHSX, else NO
 *     ror EAFLG                        SET BIT 7. (STARTS SEQUENCE)
 *
 * The ROR shifts in carry, which the CMP just set (A >= EAHSX), on top
 * of a zero EAFLG: EAFLG becomes $80.
 */
void ad_stearom(void)
{
    uint8_t y = g.zp.f.EAFLG;
    if (y != 0)
        return;                                     /* ALREADY WORKING? YEP */
    if (8 < g.zp.f.EAHSX)
        return;                                     /* FIT IN ROM? NO */
    g.zp.f.EAFLG = 0x80;                            /* ror EAFLG, C=1 */
    if (g.zp.f.EABAD == 0)
        return;                                     /* ANY BAD ENTRIES? NO */
    g.zp.f.EABC = 0x15;                             /* DO THE WHOLE ROM */
    g.zp.f.EAX = 0x14;
    g.zp.f.EAHSX = y;                               /* RESET START ADDRESS (Y = 0) */
    g.zp.f.EABAD = y;                               /* RESET BAD FLAG */
}
