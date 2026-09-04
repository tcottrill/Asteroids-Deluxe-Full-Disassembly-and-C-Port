/* The NMI - DSTNMI.MAC, and the DCIN65.MAC coin module it includes.
 *
 * NMI ($785C) fires every ~4 ms.  It divides itself down to the frame
 * tick the main line spins on (INTCT, SYNC), re-reads the coin DIP
 * while no game is running, steps the sounds, and then falls through
 * the whole coin chain in one straight line:
 *
 *     NMI_3 -> MOOLAH/$DETCT -> $BONUS -> $CNVRT -> $EXT -> COINC -> rti
 *
 * Everything below ad_nmi() is that chain, in ROM order, one static
 * function per label.  The coin code is Atari's shared "COIN65" module
 * (DCIN65.MAC, Downend & Albaugh), assembled inline with the includer's
 * settings from DSTNMI.MAC: MECHS=3, OFFSET=1, EMCTRS=3, CNTINT=0,
 * BONADD=1, COIN=1 (coin inputs high-true), plus the defaults MODES=4,
 * MULTS=1, SLAM=1, PRST=30, POST=30.  The $-prefixed names keep their
 * spelling here minus the dollar.
 *
 * Hardware:  the coin and slam switches are read through ad_hw_switch()
 * with the addresses from astdelux_defines.asm; OPTN5 sits in POKEY
 * space ($2C08) and comes through ad_hw_pokey_read(8).
 *
 * What the host must know (CONVENTIONS.md rule 7):  when the main line
 * has fallen two ticks behind, the ROM's NMI hangs on purpose so the
 * watchdog resets the board.  ad_nmi() cannot spin, so it returns with
 * the state the ROM would have had at the hang - SYNC >= 4 - and does
 * nothing else that interrupt.  Nothing in the ROM ever brings SYNC
 * above 3 any other way, so a host must test `g.zp.f.SYNC >= 4` after
 * each ad_nmi() and treat it as the watchdog reset (ad_pwron() and
 * restart).  It must not just carry on: ad_frame()'s `lsr SYNC` would
 * halve the value and hide it.
 */
#include "astdelux.h"

/* Switch addresses, from astdelux_defines.asm (originals $COINA, $LAM). */
#define COINA   0x2400u     /* coin switches, left to right, one apart */
#define LAM     0x2006u     /* slam switch */

/* DCIN65.MAC's timing constants, as assembled. */
#define PRST_X8 0xF0        /* PRST*8 = 30 frames of pre-coin slam lockout */
#define POST_X4 0x78        /* POST*4 = 30 frames of post-coin slam delay */

static void moolah(void);
static bool detct(uint8_t x);
static void bonus(void);
static void cnvrt(void);
static void ext(void);
static void coinc(void);

/* ------------------------------------------------------------------ */
/* NMI ($785C) - the interrupt, every ~4 ms.                            */
/* ------------------------------------------------------------------ */
void ad_nmi(void)
{
    /* 785C  bit $01FF / bpl NMI_100 / rti
     *   The power-up gate: while bit 7 of $01FF is set the interrupt
     *   returns at once.  The self-test leaves it set and STEST6 clears
     *   it at $7E66 to enable interrupts.  DROPPED - the self-test is
     *   not translated, and ad_pwron() leaves page 1 zeroed, so the NMI
     *   is treated as always enabled.
     *
     * 7862  pha / tya / pha / txa / pha / cld
     *   NMI_100 saves the registers and clears decimal mode.  A C call
     *   has nothing to save; the CLD only matters to the tamper trap in
     *   COINC (see there).
     *
     * 7868  lda $01FF / ora $01D0 / NMI_2: bne NMI_2
     *   Stack overflow/underflow trap: if the byte above the stack or
     *   the floor under it is non-zero, spin until the watchdog resets
     *   the board.  DROPPED - the 6502 stack is not modelled, so there
     *   is nothing to run into page 1. */

    g.zp.f.INTCT++;                             /* 7870  another interrupt */
    if ((g.zp.f.INTCT & 3) == 0) {              /* 7872  and #3 / bne NMI_10: 16 ms */
        g.zp.f.SYNC++;                          /* 7878  main line sync up */
        if (g.zp.f.SYNC >= 3) {                 /* 787C  cmp #3 / bcc NMI_10: ok */
            if (g.zp.f.SYNC != 3) {
                /* 7880  NMI_HANG: bne NMI_HANG - "PROGRAM WHERE ARE YOU?"
                 *   SYNC has passed 3: the main line missed two ticks.
                 *   The ROM spins here forever and the watchdog resets
                 *   the board.  Not spun in C: return with INTCT and
                 *   SYNC as the ROM leaves them and nothing else done
                 *   (no CSOUND, no coins, no lamps) - the same as the
                 *   hung ROM.  SYNC >= 4 is the host's signal; see the
                 *   file comment.  Further calls keep counting INTCT
                 *   and SYNC and land here again, as re-entered NMIs
                 *   would on the board. */
                return;
            }
            ad_hw_vg_reset();                   /* 7882  NMI_127: sta VGRESET -
                                                 *   SYNC == 3 exactly: stop the
                                                 *   VG in case the main line is
                                                 *   stuck waiting on HALT, and
                                                 *   carry on */
        }
    }

    /* NMI_10 ($7885).  In attract mode, and only while neither player is
     * entering initials (UPDFLG and UPDFLG+1 both negative), reset POKEY
     * and re-read the coin DIP into CMODE.  During a game, or while
     * initials are being entered, sounds are live and POKEY is left
     * alone. */
    {
        uint8_t x = g.zp.f.NPLAYR;              /* 7885  ldx NPLAYR / bne NMI_3 */
        if (x == 0 &&
            ((g.zp.f.UPDFLG[0] & g.zp.f.UPDFLG[1]) & 0x80)) {   /* 7889  and $43 / bpl NMI_3 */
            ad_hw_pokey_write(0x0F, x);         /* 788F  stx $2C0F: SKCTL = 0, reset POKEY */
            /* 7892  ldx #4 / NMI_11: dex / bne NMI_11
             *   "WASTE TIME" - a few cycles for the reset to take.
             *   DROPPED, timing only. */
            ad_hw_pokey_write(0x0F, 7);         /* 7899  stx $2C0F: re-enable */
            ad_hw_pokey_write(0x0B, 7);         /* 789C  stx $2C0B: POTGO, start pots */
            g.zp.f.CMODE = (uint8_t)(ad_hw_pokey_read(0x08) ^ 0xFF);
                                                /* 789F  lda OPTN5 / eor #$FF / sta CMODE
                                                 *   the DIP is read through POKEY
                                                 *   ($2C08); switches are low-true
                                                 *   on the wire, high-true in CMODE */
        }
    }

    ad_csound();                                /* 78A6  NMI_3: jsr CSOUND */

    /* and fall into the coin module */
    moolah();                                   /* 78A9  MOOLAH .. $DETCT_9 */
    bonus();                                    /* 793A  $BONUS */
    cnvrt();                                    /* 7954  $CNVRT */
    ext();                                      /* 7977  $EXT */
    coinc();                                    /* 79A3  COINC .. rti */
}

/* ------------------------------------------------------------------ */
/* MOOLAH ($78A9) - walk the three mechs, right to left.               */
/* ------------------------------------------------------------------ */
/* For each mech, $DETCT debounces the switch; when a coin falls out the
 * mech-multiplier code at $DETCT_8..$DETCT_85 turns it into "units" and
 * queues a pulse for that mech's electro-mechanical counter. */
static void moolah(void)
{
    uint8_t x = 2;                              /* 78A9  ldx #2: right mech first */
    do {
        if (detct(x)) {                         /* 7908  $DETCT_8: bcc $DETCT_9 */
            /* Mech multipliers ($790A).  A = units minus one, because
             * $DETCT_85 adds it with the carry set. */
            uint8_t a = 0;                      /* 790A  lda #0: start with 0 (to add 1) */
            if (x == 2) {                       /* 790C  cpx #1 / bcc (left) / beq (centre) */
                /* right mech: GDM, CMODE bits 2-3 = 0,1,2,3 for 1,4,5,6 units */
                a = (uint8_t)((g.zp.f.CMODE & 0x0C) >> 2);  /* 7912  and #$0C / lsr / lsr */
                if (a != 0)                     /* 7918  beq $DETCT_85: 00 - add 1 */
                    a = (uint8_t)(a + 2);       /* 791A  adc #2, carry clear from the
                                                 *   lsr: map 1,2,3 to 3,4,5 */
            } else if (x == 1) {
                /* $DETCT_83 ($791E): centre mech, GHM, CMODE bit 4 = 2 units */
                if (g.zp.f.CMODE & 0x10)        /* 7920  and #$10 / beq $DETCT_85 */
                    a = 1;                      /* 7924  lda #1 */
            }
            /* left mech (x == 0): always one unit */

            /* $DETCT_85 ($7926): sec / pha / adc BCCNT / sta BCCNT / pla /
             * sec / adc CNCT / sta CNCT - both counts get A + 1. */
            g.zp.f.BCCNT = (uint8_t)(g.zp.f.BCCNT + a + 1);     /* bonus-adder count */
            g.zp.f.CNCT  = (uint8_t)(g.zp.f.CNCT + a + 1);      /* unit-coin count */
            g.zp.f.CCTIM[x]++;                  /* 7932  inc CCTIM,x: queue a pulse
                                                 *   for the E.M. counter */
        }
    } while (x-- != 0);                         /* 7934  $DETCT_9: dex / bmi $BONUS /
                                                 *   jmp $DETCT */
}

/* $DETCT ($78AB) - debounce one mech.  X = mech, 2..0 (right to left).
 * Returns the carry at $DETCT_8: true when a coin has just fallen out.
 *
 * CNSTT,x packs two counters (DCIN65.MAC "COIN DETECTION"):
 *   bits 0-4  count coin-present samples down from 31: fast (every
 *             interrupt) for the first five, then once every eighth
 *             interrupt, sticking at 0 (coin on too long, >800 ms);
 *   bits 5-7  count coin-absent samples up from 0, reset whenever the
 *             coin is seen.  When they wrap (8 samples, ~33 ms) the
 *             low counter is judged: 27-31 too short, 0 too long, 1-26
 *             a coin - which is then held in PSTSL,x for POST frames
 *             so a slam can still void it.
 * The idle state is CNSTT = $1F, not 0: the first wrap from a cleared
 * byte resets it to 31, and 31 stays 31.
 */
static bool detct(uint8_t x)
{
    /* 78AB  lda $COINA,x / asl a - bit 7 of the port into the carry.
     *   $2400 + x: left mech at $2400, middle $2401, right $2402. */
    bool coin_on = (ad_hw_switch((uint16_t)(COINA + x)) & 0x80) != 0;

    uint8_t a = (uint8_t)(g.zp.f.CNSTT[x] & 0x1F);  /* 78AF  lda CNSTT,x / and #$1F:
                                                     *   the coin-on down-counter */
    if (coin_on) {                              /* 78B3  bcc $DETCT_5: branch if absent */
        if (a != 0) {                           /* 78B5  beq $DETCT_1: stick at 0 */
            bool fast = a >= 0x1B;              /* 78B7  cmp #27 / bcs $DETCT_10:
                                                 *   first five samples run fast */
            if (fast || (g.zp.f.INTCT & 7) == 7) /* 78BB  tay / lda INTCT / and #7 /
                                                 *   cmp #7 / tya / bcc $DETCT_1 */
                a--;                            /* 78C5  $DETCT_10: sbc #1, carry set */
        }
        g.zp.f.CNSTT[x] = a;                    /* 78C7  $DETCT_1: sta CNSTT,x - the
                                                 *   and #$1F also reset the
                                                 *   coin-off up-counter */
    } else {
        /* $DETCT_5 ($78EC): coin absent.  Carry is clear on arrival. */
        if (a >= 0x1B) {                        /* 78EC  cmp #27 / bcs $DETCT_6: not
                                                 *   on for five samples - too short */
            g.zp.f.CNSTT[x] = 0x1F;             /* 78F9  $DETCT_6: lda #$1F / bcs
                                                 *   $DETCT_1 (carry set by the cmp) */
        } else {
            uint16_t sum = (uint16_t)(g.zp.f.CNSTT[x] + 0x20);  /* 78F0  lda CNSTT,x /
                                                 *   adc #$20: bump the coin-off
                                                 *   up-counter, carry clear */
            if (sum < 0x100) {                  /* 78F4  bcc $DETCT_1: no wrap */
                g.zp.f.CNSTT[x] = (uint8_t)sum;
            } else if ((uint8_t)sum == 0) {     /* 78F6  beq $DETCT_6: wrapped from
                                                 *   $E0 - coin was on too long */
                g.zp.f.CNSTT[x] = 0x1F;         /* 78F9  $DETCT_6 with the carry set */
            } else {
                /* 78F8  clc - a valid coin; $DETCT_6's bcs falls through */
                bool early;
                g.zp.f.CNSTT[x] = 0x1F;         /* 78FD  sta CNSTT,x: reset status */
                /* 78FF  lda PSTSL,x / beq $DETCT_7 / sec - "CHECK HOWIES
                 *   ASSUMPTION": if a previous coin is still waiting out
                 *   its post-slam delay, give that one its credit now. */
                early = g.zp.f.PSTSL[x] != 0;
                g.zp.f.PSTSL[x] = POST_X4;      /* 7904  $DETCT_7: lda #$78 / sta PSTSL,x:
                                                 *   delay acceptance POST frames */
                return early;                   /* straight into $DETCT_8, skipping
                                                 *   the slam check below */
            }
        }
    }

    /* $DETCT_1's tail ($78C9): the slam switch and its timers.  Reached
     * by every path except a freshly detected coin. */
    if (ad_hw_switch(LAM) & 0x80)               /* 78C9  lda $LAM / and #$80 / beq $DETCT_2 */
        g.zp.f.LMTIM = PRST_X8;                 /* 78D0  lda #$F0 / sta LMTIM: pre-coin
                                                 *   slam timer, "DECR. 8 TIMES/FRAME" */
    if (g.zp.f.LMTIM != 0) {                    /* 78D4  $DETCT_2: lda LMTIM / beq $DETCT_3 */
        g.zp.f.LMTIM--;                         /* 78D8  dec LMTIM: run the timer */
        g.zp.f.CNSTT[x] = 0;                    /* 78DC  clear coin status */
        g.zp.f.PSTSL[x] = 0;                    /* 78DE  and the post-coin slam timer */
    }

    /* $DETCT_3 ($78E0): clc, then run the post-coin slam timer; the coin
     * is accepted the interrupt it reaches zero. */
    if (g.zp.f.PSTSL[x] != 0) {                 /* 78E1  lda PSTSL,x / beq $DETCT_8 */
        g.zp.f.PSTSL[x]--;                      /* 78E5  dec PSTSL,x */
        if (g.zp.f.PSTSL[x] == 0)               /* 78E7  bne $DETCT_8 */
            return true;                        /* 78E9  sec: indicate a coin */
    }
    return false;                               /* carry clear: no coin */
}

/* ------------------------------------------------------------------ */
/* $BONUS ($793A) - the bonus adder.                                   */
/* ------------------------------------------------------------------ */
/* CMODE bits 5-7 pick a MODULO entry ($77EA: 7F 02 04 04 05 03 7F 7F),
 * the number of unit-coins that earn one bonus unit-coin in BC; mode 3
 * earns two.  $7F effectively means never - though literally it would
 * pay out at BCCNT = 127, and that is kept. */
static void bonus(void)
{
    uint8_t y = (uint8_t)(g.zp.f.CMODE >> 5);   /* 793A  GBAM: five lsr's */
    uint8_t a = (uint8_t)(g.zp.f.BCCNT - ad_rom(AD_MODULO + y));
                                                /* 7942  lda BCCNT / sec / sbc MODULO,y */
    if (a & 0x80)                               /* 7948  bmi $CNVRT: not enough yet -
                                                 *   the sign of the byte, not the
                                                 *   borrow, is what is tested */
        return;
    g.zp.f.BCCNT = a;                           /* 794A  keep the remainder */
    g.zp.f.BC++;                                /* 794C  one bonus unit-coin... */
    if (y == 3)                                 /* 794E  cpy #3 / bne $CNVRT */
        g.zp.f.BC++;                            /* 7952  ...or two, for mode 3 */
}

/* ------------------------------------------------------------------ */
/* $CNVRT ($7954) - coins to credits.                                  */
/* ------------------------------------------------------------------ */
/* CMODE bits 0-1: 0 free play (CNCT is just zeroed), 1 two plays per
 * coin, 2 one play per coin, 3 two coins per play.  Runs every
 * interrupt, so at most one price is converted per ~4 ms and bonus
 * coins in BC are spent on the very next tick after they cannot cover
 * a whole price from CNCT alone. */
static void cnvrt(void)
{
    uint8_t y = (uint8_t)(g.zp.f.CMODE & 3);    /* 7954  GCM: lda CMODE / and #3 / tay */
    uint8_t a = y;
    if (y != 0) {                               /* 7959  beq $CNVRT_2: free play */
        uint8_t  price = (uint8_t)((y >> 1) + (y & 1));    /* 795B  lsr / adc #0:
                                                 *   modes 1,2,3 -> price 1,1,2 */
        uint16_t diff  = (uint16_t)(g.zp.f.CNCT + (uint8_t)(price ^ 0xFF) + 1);
                                                /* 795E  eor #$FF / sec / adc CNCT:
                                                 *   A = CNCT - price, carry = no
                                                 *   borrow */
        a = (uint8_t)diff;
        if (diff < 0x100) {                     /* 7963  bcs $CNVRT_33 */
            a = (uint8_t)(a + g.zp.f.BC);       /* 7965  adc BC (carry clear): can the
                                                 *   bonus coins make up the price? */
            if (a & 0x80)                       /* 7967  bmi $EXT: no - and note the
                                                 *   sta CNCT is skipped, so CNCT
                                                 *   is left as it was */
                return;
            g.zp.f.BC = a;                      /* 7969  the bonus coins left over */
            a = 0;                              /* 796B  new CNCT */
        }
        /* $CNVRT_33 ($796D): generate credits, 1 or 2 */
        if (y < 2)                              /* 796D  cpy #2 / bcs $CNVRT_1 */
            g.zp.f.CRDT++;                      /* 7971  a second one for mode 1 */
        g.zp.f.CRDT++;                          /* 7973  $CNVRT_1: inc CRDT */
    }
    g.zp.f.CNCT = a;                            /* 7975  $CNVRT_2: sta CNCT */
}

/* ------------------------------------------------------------------ */
/* $EXT ($7977) - pulse the electro-mechanical coin counters.          */
/* ------------------------------------------------------------------ */
/* CCTIM,x is a timer in two nibbles: the low nibble is the number of
 * pulses still owed to that counter (queued by $DETCT_85), the high
 * nibble runs a pulse, counting down from $F on every second interrupt.
 * Bit 7 is the counter drive: set for $F..$8 (16 interrupts, "PULSE=4
 * FRAMES"), clear for $7..$0, after which the low nibble is looked at
 * again.  Only one counter runs at a time. */
static void ext(void)
{
    uint8_t x, y;

    if (g.zp.f.INTCT & 1)                       /* 7977  lda INTCT / lsr / bcs COINC:
                                                 *   only on even interrupts */
        return;

    y = 0;                                      /* 797C  ldy #0: "on" flag */
    x = 2;                                      /* 797E  ldx #2 */
    do {                                        /* $EXT_1 ($7980) */
        uint8_t a = g.zp.f.CCTIM[x];            /* 7980  lda CCTIM,x */
        if (a != 0 && a >= 0x10) {              /* 7982  beq $EXT_3: idle;
                                                 * 7984  cmp #$10 / bcc $EXT_3: pending
                                                 *   but not running */
            a = (uint8_t)(a + 0xEF + 1);        /* 7988  adc #$EF, carry set by the cmp:
                                                 *   high nibble minus one */
            y++;                                /* 798A  iny: something is running */
            g.zp.f.CCTIM[x] = a;                /* 798B  $EXT_2: sta CCTIM,x */
        }
    } while (x-- != 0);                         /* 798D  $EXT_3: dex / bpl $EXT_1 */

    if (y != 0)                                 /* 7990  tya / bne COINC */
        return;

    /* $EXT_4 ($7995): nothing is running - start the first counter that
     * has pulses owed. */
    x = 2;                                      /* 7993  ldx #2 */
    do {
        uint8_t a = g.zp.f.CCTIM[x];            /* 7995  lda CCTIM,x */
        if (a != 0) {                           /* 7997  beq $EXT_5: nothing owed */
            a = (uint8_t)(a + 0xEF);            /* 7999  clc / adc #$EF: high nibble
                                                 *   to $F, low nibble minus one */
            g.zp.f.CCTIM[x] = a;                /* 799C  sta CCTIM,x: start the timer */
            if (a & 0x80) {                     /* 799E  bmi COINC: don't start more */
                /* The pulse to counter x begins here: COINC will latch
                 * bit 7 into CCLFT/CCMID/CCRIT this same interrupt.  The
                 * host's accessor takes only the counter number - it
                 * counts a coin, rather than following the latch - so
                 * it is called once, at the rising edge, from here.
                 * See COINC for what the ROM writes. */
                ad_hw_coin_counter(x);
                return;
            }
        }
    } while (x-- != 0);                         /* 79A0  $EXT_5: dex / bpl $EXT_4 */
}

/* ------------------------------------------------------------------ */
/* COINC ($79A3) - drive the outputs, and return from the interrupt.   */
/* ------------------------------------------------------------------ */
static void coinc(void)
{
    /* 79A3  lda CCTIM / sta CCLFT ($3C05)
     * 79A8  lda CCTIM+1 / sta CCMID ($3C06)
     * 79AD  lda CCTIM+2 / sta CCRIT ($3C07)
     *   The counter latches take bit 7 of each timer, every interrupt.
     *   ad_hw_coin_counter() has no on/off argument, so the latch is
     *   not written here; the pulse is reported once, where $EXT_4
     *   raises it.  The timers themselves are in g.zp.f.CCTIM for a
     *   host that wants the level (bit 7 = drive on).
     *
     * 79B2  lda CKERR / beq COINC_17
     * 79B6  lda HOLE / beq COINC_17
     * 79BA  lda NPLAYR / beq COINC_17
     * 79BE  tsx / lda $0104,x / ora #$08 / sta $0104,x
     *   The second tamper trap: with a checksum error recorded, the
     *   copyright "hole" armed and a game in progress, OR $08 (the D
     *   flag) into the status byte the NMI pushed, so the interrupted
     *   code resumes in decimal mode and the game corrupts itself.  A
     *   pure hardware artefact of the 6502 status register - DROPPED,
     *   and it must never fire here: the C port has no saved status
     *   byte to poison, and a checksum can only fail on a modified ROM. */

    /* COINC_17 ($79C7): the start-button lamps.
     *
     *     lda LOUT1 / eor #3 / ror a / ror a / sta SLMP1 / ror a / sta SLMP2
     *
     * The lamps latch bit 7.  After two rors bit 7 holds bit 0 of
     * LOUT1^3, after three it holds bit 1; the carry that comes in from
     * $EXT lands in bit 6 and is never seen.  So lamp 1 is lit while
     * LOUT1 bit 0 is clear, lamp 2 while bit 1 is clear. */
    {
        uint8_t l = (uint8_t)(g.zp.f.LOUT1 ^ 3);
        ad_hw_lamp(0, (l & 0x01) != 0);         /* 79CD  sta SLMP1 ($3C00) */
        ad_hw_lamp(1, (l & 0x02) != 0);         /* 79D1  sta SLMP2 ($3C01) */
    }

    if (g.zp.f.LMTIM != 0) {                    /* 79D4  lda LMTIM / beq COINC_16:
                                                 *   tone on while a slam is remembered */
        ad_hw_pokey_write(0x00, 0x08);          /* 79DC  sta POKEY: AUDF1 = 8 */
        ad_hw_pokey_write(0x01, 0xAF);          /* 79DF  sty $2C01: AUDC1 = pure tone,
                                                 *   volume 15 */
    }

    /* 79E2  COINC_16: pla / tax / pla / tay / pla / rti ($79E7) */
}
