/* The main line - DSTRD0.MAC, START at $6000.
 *
 * Structure follows disasm/MAINLINE_NOTES.md exactly; each call below
 * is one JSR in the listing, in order.  Routines not yet translated are
 * declared here and stubbed in stubs.c, so the shape of the frame is
 * visible and testable before the bodies exist.
 */
#include <string.h>

#include "astdelux.h"

ad_state g;

/* `sta BNKSEL` ($3C04).  Bit 7 selects which physical page is "page 2";
 * the port keeps an index instead of swapping memory, which is
 * equivalent because every access goes through AD_P. */
void ad_bnksel(uint8_t v)
{
    g.bnksel = v;
    g.bank = v >> 7;
}

/* SBANK ($6CB1) - select the current player's page:
 *
 *     lda PLAYR / lsr a / ror a / sta BNKSEL      0 or $80
 */
void ad_sbank(void)
{
    ad_bnksel((uint8_t)(g.zp.f.PLAYR << 7));
}

/* ------------------------------------------------------------------ */
/* One frame.  START2 ($6014) to the jump at the bottom.                */
/* Returns false when the wave is over and START1 should run again.     */
/* ------------------------------------------------------------------ */
bool ad_frame(void)
{
    /* START2: wait for the DVG to finish the previous list. */
    while (ad_hw_vg_busy())
        ;

    ad_rotast();

    /* START2_10: wait for frame sync.  SYNC is bumped by the NMI every
     * fourth interrupt, so this is the ~16 ms tick.  `lsr SYNC` both
     * tests and consumes it. */
    while (!(g.zp.f.SYNC & 1))
        ;
    g.zp.f.SYNC >>= 1;

    /* Hand the finished list to the DVG and start building the other.
     * Two lists alternate on FRAME bit 0; the first two bytes of vector
     * RAM hold a JMPL into whichever was completed last.  RADDR ($77DE)
     * is the table of those two JMPL words, and the same word, shifted
     * back from a DVG word address to a 6502 byte address, becomes the
     * build pointer for the other buffer:
     *
     *     lda RADDR,y / asl a / sta VGLIST        low byte, times two
     *     lda RADDR+1,y / rol a / and #$1F / ora #$40 / sta VGLIST+1
     */
    {
        uint8_t x = (uint8_t)((g.zp.f.FRAME[0] & 1) << 1);
        uint8_t y = x ^ 2;                      /* fill the other one */
        uint8_t lo = ad_rom(AD_RADDR + y);
        uint8_t hi = ad_rom(AD_RADDR + 1 + y);

        g.vram[0] = ad_rom(AD_RADDR + x);       /* give it to VG */
        g.vram[1] = ad_rom(AD_RADDR + 1 + x);
        g.zp.f.VGLIST[0] = (uint8_t)(lo << 1);
        g.zp.f.VGLIST[1] = (uint8_t)((((hi << 1) | (lo >> 7)) & 0x1F) | 0x40);
    }
    ad_hw_vg_go();
    ad_hw_watchdog();

    /* START2_12: next frame, and once every 256 of them a slice of the
     * anti-tamper checksum. */
    if (++g.zp.f.FRAME[0] == 0) {
        g.zp.f.FRAME[1]++;
        ad_protck();
    }

    ad_eaupd();
    if (ad_chkst())
        return false;              /* START2_50: begin a new game */

    ad_update();
    if (!ad_getint()) {            /* START2_20 if initials are in progress */
        if (!ad_scores()) {        /* START2_20 if the table is on screen */
            if (g.zp.f.GDELAY == 0) {
                /* The player block is skipped while a new player is
                 * starting, and while the high-score table is up -
                 * "not enough time for asteroids and score tables". */
                ad_shield();
                ad_fire();
                ad_move();
                ad_settip();
                ad_enemy();
            }
            /* START2_15: these run in attract mode too, which is what
             * makes the attract screen a real game with no ship. */
            ad_motion();
            ad_colide();
        }
    }

    /* START2_20 */
    ad_params();
    ad_sounds();
    ad_vgsabs(0x7F, 0x7F);         /* park the beam centre screen, for
                                    * minimum current draw */
    ad_vghalt();
    ad_pkytst();

    /* START2_21/30: the wave ends only when both the delay and the rock
     * count reach zero.  Both live on the player page, so they follow
     * the bank. */
    if (AD_P->f.RDELAY)
        AD_P->f.RDELAY--;
    return (AD_P->f.RDELAY | AD_P->f.NROCKS) != 0;
}

/* START ($6000). */
void ad_start(void)
{
    ad_init();

    /* Kick an EAROM read, unless a previous erase is still running. */
    if (g.zp.f.EAFLG == 0) {
        g.zp.f.EABC = 0;
        g.zp.f.EAX = 0;
        g.zp.f.EAHSX = 0;
        g.zp.f.EAFLG = 0x20;       /* read command */
    }

    for (;;) {
        ad_newast();               /* START1 */
        while (ad_frame())
            ;
    }
}

/* PWRON ($7CD7) - reset.  The RAM test itself is not translated; a C
 * port has nothing to test, and its only lasting effect is to clear
 * page 0, page 1, both player pages and vector RAM. */
void ad_pwron(void)
{
    memset(&g, 0, sizeof g);
    ad_hw_pokey_write(0x0F, 0);    /* $2C0F, as PWRON does before the test */
}
