/* Asteroids Deluxe - C conversion, public interface.
 *
 * Translated from disasm/astdelux2_main.asm (rev 2, the build Atari's
 * source archive documents).  See CONVENTIONS.md before adding code.
 *
 * Every routine called across a module boundary is declared here, with
 * its ROM address and the register contract it was translated from
 * (disasm/CALLING_NOTES.md).  Registers are uint8_t, because that is
 * what the 6502 passes; where a value is signed the ROM's own
 * arithmetic says so and the comment repeats it.  A result the ROM
 * leaves in a flag is a bool named for what *set* means.
 */
#ifndef ASTDELUX_H
#define ASTDELUX_H

#include <stdint.h>
#include <stdbool.h>

#include "astdelux_state.h"
#include "astdelux_rom.h"

/* ------------------------------------------------------------------ */
/* ROM tables                                                          */
/* ------------------------------------------------------------------ */
/* Read through ad_rom() at the address the listing shows, so
 * `lda SINCOS,x` is ad_rom(AD_SINCOS + x).  CALLING_NOTES.md section 15
 * says who reads each one. */
#define AD_AVEL     0x499C  /* 7   ATTACK: angular velocity vs attacker count */
#define AD_ITXL     0x4B2B  /* 3   SETTIP: cluster offsets and initial state */
#define AD_ITXH     0x4B2E
#define AD_ITYL     0x4B31
#define AD_ITYH     0x4B34
#define AD_ITIME    0x4B37
#define AD_IANG     0x4B3A
#define AD_SINCOS   0x4B51  /* 65  sin 0..90 degrees, times 127 */
#define AD_HITSCR   0x4D7D  /* 3   SPLIT: 1000/500/200 for small/medium/large */
#define AD_EXPPIC   0x4D80  /* 8   PICTUR: rock explosion JSRL words */
#define AD_EXP16    0x4D88  /* 224 SHPPIC: materialisation sparkle vectors */
#define AD_RSOURC   0x5002  /* 16  ROTAST: 8 rock source-vector pointers (see
                             *     CPXROT in CALLING_NOTES.md - index 16 reads
                             *     into SHLDVC, and the ROM means it) */
#define AD_SHLDVC   0x5012  /* 24  SHLDPX: shield octagon vectors */
#define AD_TFPIX    0x502A  /* 32 diamond JSRL words; +$40 = ship fragments */
#define AD_SHIPS    0x53BC  /* 9   CPTSPX: offsets of the ship orientations */
#define AD_SHIPSV   0x53C6  /* 386 the ship hull vectors */
#define AD_VGMSGA   0x56F8  /* 76  glyph JSRL words, index = character * 2 */
#define AD_SPSND    0x63E4  /* 2   LSF: saucer sound numbers, read at -1 */
#define AD_EFIRE97  0x647A  /* 2   EFIRE: aim error masks */
#define AD_EFIRE98  0x647C  /* 2   EFIRE: aim error sign extension */
#define AD_EFIRE99  0x647E  /* 4   EFIRE: saucer Y velocities */
#define AD_SIZOPT   0x6822  /* 4   MOV: DVG scale per rock size */
#define AD_ACCEL    0x6918  /* 8   CACCEL: thrust vs speed */
#define AD_RWAVE    0x69FF  /* 4   NEWAST: rocks per wave */
#define AD_TABLE    0x6F0A  /* 8   CPXROT: orientation control bytes */
#define AD_TSS      0x6F7C  /* 2   SOUNDS: the two thump sound numbers */
#define AD_ATANA    0x710F  /* 17  ATAN: arctangent lookup */
#define AD_VGMSGS   0x7219  /* 28  VGME: message screen positions */
#define AD_VGMSGT   0x7235  /* 8   VGME: the four language table pointers */
#define AD_ASTM     0x7603  /* 10  the packed copyright text */
#define AD_PNTRS    0x760D  /* 96  SNDOO: 12 sounds x 8 channel offsets */
#define AD_DASOUN   0x760D  /* 325 the whole sound block, PNTRS first */
#define AD_RADDR    0x77DE  /* 4   the two JMPL words that start each buffer */
#define AD_ROCKSA   0x77E2  /* 8   the four rock-subroutine JSRL words */
#define AD_MODULO   0x77EA  /* 8   $BONUS: coins per bonus unit */
#define AD_CPRDT    0x783D  /* 8   VGRCPT: expected copyright list bytes */
#define AD_ASTMT    0x7845  /* 10  VGRCPT: the packed text it checks against */
#define AD_EABDS    0x7BD2  /* 7   EAUPD: byte offsets from HSCORE, $FF ends */
#define AD_BONUS    0x7C6E  /* 4   SINIT: bonus-life plateau per option */
#define AD_ROMX     0x7FEE  /* 6   STEST6: checksum display, ROM letter X */
#define AD_ROMY     0x7FF4  /* 6   STEST6: checksum display, ROM letter Y */

/* ------------------------------------------------------------------ */
/* Machine state                                                       */
/* ------------------------------------------------------------------ */

#define AD_VRAM_SIZE 0x800          /* vector RAM, $4000-$47FF */

typedef struct {
    ad_zp_t     zp;                 /* page 0 */
    ad_pg1_t    pg1;                /* page 1, above the stack */
    ad_player_t page[2];            /* pages 2 and 3 */

    /* Which of the two pages is currently "page 2".  BNKSEL ($3C04)
     * bit 7 swaps the physical pages; keeping an index is equivalent
     * and cheaper, because every access goes through AD_P anyway. */
    int         bank;

    /* Vector RAM.  The port builds a real display list rather than
     * calling a draw API: the shapes are copied *and modified* on the
     * way in (CPYVEC applies sign and swap masks to rotate the ship),
     * so the list is where the drawing actually happens.  It also makes
     * the strongest differential test available - the generated bytes
     * can be compared against the real ROM's vector RAM. */
    uint8_t     vram[AD_VRAM_SIZE];

    /* Latches the ROM writes and later reads back. */
    uint8_t     bnksel;             /* $3C04 */
    uint8_t     lamps[2];           /* $3C00, $3C01 */
    uint8_t     expsnd;             /* $3600 explosion pitch/volume */
    bool        thrust_on;          /* $3C03 */
} ad_state;

extern ad_state g;

/* The active player's page.  Always "page 2" as far as the game is
 * concerned; which physical page that is depends on the bank.  AD_P3
 * is the other one, for the few places the ROM reads $03xx on purpose
 * (GTSP looks at the other player's ship; SRESET clears both). */
#define AD_P   (&g.page[g.bank])
#define AD_P3  (&g.page[g.bank ^ 1])

/* Vector RAM as the 6502 sees it, by address.  Reads outside $4000-$47FF
 * return 0 and writes are dropped; the ROM only ever points VGLIST into
 * vector RAM. */
static inline uint8_t ad_vram_rd(uint16_t a)
{
    return (a >= 0x4000u && a < 0x4000u + AD_VRAM_SIZE) ? g.vram[a - 0x4000u] : 0;
}
static inline void ad_vram_wr(uint16_t a, uint8_t v)
{
    if (a >= 0x4000u && a < 0x4000u + AD_VRAM_SIZE)
        g.vram[a - 0x4000u] = v;
}

/* VGLIST ($03/$04), the display-list build pointer, as one value. */
static inline uint16_t ad_vglist(void)
{
    return (uint16_t)(g.zp.f.VGLIST[0] | (g.zp.f.VGLIST[1] << 8));
}
static inline void ad_set_vglist(uint16_t v)
{
    g.zp.f.VGLIST[0] = (uint8_t)v;
    g.zp.f.VGLIST[1] = (uint8_t)(v >> 8);
}

/* ------------------------------------------------------------------ */
/* The parallel-array object model                                     */
/* ------------------------------------------------------------------ */
/* Slots 0..24 rocks, 25 ship, 26 saucer, 27-28 saucer torpedoes,
 * 29-32 ship torpedoes.  The ROM indexes across the boundaries on
 * purpose, so these return the base of a run rather than a typed
 * array.  The arrays are $21 apart, which is why `lda XINC+$21,x`
 * reads YINC: index the raw page (AD_P->raw[AD_XINC + i]) where the
 * listing does that.  See disasm/CALLING_NOTES.md section 0. */

#define AD_STRIDE  0x21     /* XINC -> YINC, OBJXH -> OBJYH, ... */
/* AD_SHIP (25) and AD_SAUCER (26) come from astdelux_state.h. */

static inline uint8_t *ad_obj(void)   { return AD_P->raw + AD_OBJ;   }
static inline uint8_t *ad_xinc(void)  { return AD_P->raw + AD_XINC;  }
static inline uint8_t *ad_yinc(void)  { return AD_P->raw + AD_YINC;  }
static inline uint8_t *ad_objxh(void) { return AD_P->raw + AD_OBJXH; }
static inline uint8_t *ad_objyh(void) { return AD_P->raw + AD_OBJYH; }
static inline uint8_t *ad_objxl(void) { return AD_P->raw + AD_OBJXL; }
static inline uint8_t *ad_objyl(void) { return AD_P->raw + AD_OBJYL; }

/* OBJ bit layout, from PG0123.MAC:
 *   bits 0-2  size: 1 small, 2 medium, 4 large
 *   bits 3-6  picture number
 *   bit  7    exploding
 * A special (Deluxe) rock has bit 6 set; bits 5-2 then link it to its
 * SRTIME/SRANG control entry. */
#define AD_OBJ_SIZE     0x07
#define AD_OBJ_PIC      0x78
#define AD_OBJ_EXPLODE  0x80
#define AD_OBJ_SPECIAL  0x40
#define AD_OBJ_LINK     0x3C

/* ------------------------------------------------------------------ */
/* Hardware, supplied by the host                                      */
/* ------------------------------------------------------------------ */
/* Names and addresses are the originals; see disasm/astdelux_defines.asm.
 * Each returns the byte the 6502 would have read, or takes the byte it
 * would have written. */

/* Switch inputs.  `addr` is the full 6502 address ($2001..$2007,
 * $2400..$2407, $2800..$2803) so call sites read like the listing.
 * Bit 7 of the result is the switch, as on the board. */
uint8_t ad_hw_switch(uint16_t addr);

/* Which program ROM revision is socketed: 2 (the port's base, MAME's
 * astdelux2) or 3 (MAME's astdelux).  Rev 3's behavioural differences
 * are gated on this at the point in the code where they occur, rev 2
 * being what runs otherwise - see CONVENTIONS.md rule 13. */
uint8_t ad_hw_rom_rev(void);

/* Machine time the ROM spent computing between two hardware accesses
 * whose spacing it depends on: `cycles` 6502 cycles ran here with no
 * hardware touched.  Hosts feed it to the POKEY clock (one CPU cycle is
 * one POKEY cycle on this board).  There is no 6502 to count for us, so
 * this is stated only where the ROM itself states the assumption - the
 * self-test's POTGO at $7ED0 and PKYTST's "S/B 0" read at $7FDD, with
 * four digit draws between them.  The same pattern Tempest's protection
 * needs (pokey.h's time-model note). */
void    ad_hw_cycles(uint16_t cycles);

uint8_t ad_hw_pokey_read(uint8_t reg);      /* $2C00..$2C0F */
void    ad_hw_pokey_write(uint8_t reg, uint8_t v);
uint8_t ad_hw_random(void);                 /* $2C0A, the RNG the game uses */

uint8_t ad_hw_earom_read(void);             /* EAIN   $2C40 */
void    ad_hw_earom_write(uint8_t addr, uint8_t data);  /* EADAL,x  $3200+X:
                                              * X = address, A = data */
void    ad_hw_earom_ctl(uint8_t v);         /* EACTL  $3A00 */

void    ad_hw_vg_go(void);                  /* GOADD  $3000 */
bool    ad_hw_vg_busy(void);                /* HALT   $2002 bit 7 */
void    ad_hw_vg_reset(void);               /* VGRESET $3800 */
void    ad_hw_watchdog(void);               /* WTDOG  $3400 */

void    ad_hw_explosion(uint8_t v);         /* EXPSND $3600 */
void    ad_hw_thrust(bool on);              /* SPTEN  $3C03 */
void    ad_hw_noise_reset(void);            /* NRESET $3E00 */
void    ad_hw_lamp(int which, bool on);     /* SLMP1/2 $3C00/$3C01 */
void    ad_hw_coin_counter(int which);      /* CCLFT/CCMID/CCRIT */

/* ------------------------------------------------------------------ */
/* mainline.c                                                          */
/* ------------------------------------------------------------------ */
void ad_pwron(void);        /* PWRON  $7CD7 - reset */
void ad_start(void);        /* START  $6000 - runs the game, never returns */
bool ad_frame(void);        /* one pass of the frame loop; false to restart
                             * the wave, mirroring START2 vs START1 */
void ad_bnksel(uint8_t v);  /* sta BNKSEL - bit 7 picks the page */
void ad_sbank(void);        /* SBANK  $6CB1 - BNKSEL from PLAYR */

/* ------------------------------------------------------------------ */
/* vgutil.c  (DSTCUT.MAC, plus VGBLNK and DIGITS)                      */
/* ------------------------------------------------------------------ */
/* VGLIST is the build pointer; these append to it exactly as the ROM's
 * routines do, so the resulting list can be diffed against hardware. */
void ad_vgadd2(uint8_t a, uint8_t x);       /* VGADD2 $7CCC  store A,X, +2 */
void ad_vgadd(uint8_t y);                   /* VGADD  $7A67  VGLIST += Y+1 */
void ad_vghalt(void);                       /* VGHALT $79EC */
void ad_vgrtsl(void);                       /* VGRTSL $79E8 */
void ad_vgjsrl(uint16_t shape);             /* VGJSRL $7A2A  byte address */
void ad_vgsabs(uint8_t x4, uint8_t y4);     /* VGSABS $7A31  X/4, Y/4 */
void ad_vglabs(uint8_t zp);                 /* VGLABS $7A4A  X = zp address of
                                             *   XL,XH,YL,YH; VGSIZE OR'd in */
void ad_vgchar(uint8_t y);                  /* VGCHAR $7A0A  Y = char * 2 */
void ad_vghex(uint8_t a);                   /* VGHEX  $79FD  low nibble of A */
bool ad_vghexz(uint8_t a, bool suppress);   /* VGHEXZ $79F7  C in, C out: still
                                             *   suppressing leading zeros */
void ad_vgwait(uint8_t a);                  /* VGWAIT $7AFC  A = timer */
void ad_vgawt(uint8_t x4, uint8_t y4);      /* VGAWT  $7AF7  LABS then wait 7 */
void ad_vgdot(uint8_t a, uint8_t x);        /* VGDOT  $7AFE  A timer, X intensity */
void ad_vgblnk(uint8_t a);                  /* VGBLNK $7CC5  digit then a space */
void ad_digits(bool suppress, uint8_t zp, uint8_t y);
                                            /* DIGITS $7C98  C, A = zp address of
                                             *   the LSB, Y = BCD bytes */

/* ------------------------------------------------------------------ */
/* message.c  (DSTMSG.MAC)                                             */
/* ------------------------------------------------------------------ */
void ad_vgmsg(uint8_t y);                   /* VGMSG  $718F  Y = message */
void ad_vgme(uint8_t a, uint8_t y);         /* VGME   $7198  A = language */
void ad_setrol(void);                       /* SETROL $714E  rotate rocks, build
                                             *   the copyright list at $4702 */
void ad_cpyrs(void);                        /* CPYRS  $717E  unpack ASTM */
void ad_vgrcpt(void);                       /* VGRCPT $77F5  copyright check */

/* ------------------------------------------------------------------ */
/* mathrom.c                                                           */
/* ------------------------------------------------------------------ */
uint8_t ad_sin(uint8_t a);                  /* SIN    $7137  A angle -> signed */
uint8_t ad_cos(uint8_t a);                  /* COS    $7134 */
uint8_t ad_atan(uint8_t x, uint8_t y);      /* ATAN   $70D4  signed X,Y -> angle */
uint8_t ad_comp(uint8_t a);                 /* COMP   $70EC  -A */
uint8_t ad_mult(uint8_t a);                 /* MULT   $49B3  A * R1 ($80 = 1.0);
                                             *   Y, R1, R2, R3 destroyed */
uint8_t ad_range(uint8_t a);                /* RANGE  $49DF  TEMP2:A wrapped the
                                             *   short way round; returns A */
uint8_t ad_rtst(uint8_t a, uint8_t x);      /* RTST   $49F5  RANGE with the
                                             *   difficulty hook; X = object */
uint8_t ad_cputd(uint8_t x, uint8_t y);     /* CPUTD  $4A1A  16-bit difference:
                                             *   returns high, TEMP2 = low */
uint8_t ad_scaler(uint8_t a);               /* SCALER $4A2A  R2:R3 dx, TEMP2:A
                                             *   dy -> angle (tail-calls ATAN) */

/* ------------------------------------------------------------------ */
/* sound.c  (DASOUN.MAC, SOUNDS)                                       */
/* ------------------------------------------------------------------ */
void ad_sndoff(uint8_t y);                  /* SNDOFF $7752  Y = sound number */
void ad_sndpon(uint8_t y);                  /* SNDPON $7758  start if idle */
void ad_sndon(uint8_t y);                   /* SNDON  $775B  start, always */
void ad_sndoo(uint8_t y, bool priority);    /* SNDOO  $775C  C = priority */
void ad_csound(void);                       /* CSOUND $7787  per NMI */
void ad_sounds(void);                       /* SOUNDS $6F12  per frame */
void ad_inisou(void);                       /* INISOU $784F  silence all */
void ad_diasnd(void);                       /* DIASND $49A4  diamond sound */

/* ------------------------------------------------------------------ */
/* earom.c                                                             */
/* ------------------------------------------------------------------ */
void ad_eaupd(void);                        /* EAUPD  $7B11  one step */
void ad_stearom(void);                      /* STEAROM $7BD9 start a write */

/* ------------------------------------------------------------------ */
/* nmi.c  (DSTNMI.MAC, DCIN65.MAC)                                     */
/* ------------------------------------------------------------------ */
void ad_nmi(void);                          /* NMI    $785C  every ~4 ms */

/* ------------------------------------------------------------------ */
/* objects.c                                                           */
/* ------------------------------------------------------------------ */
void    ad_motion(void);                    /* MOTION $66F8  falls into MOV */
void    ad_colide(void);                    /* COLIDE $610E */
uint8_t ad_search(void);                    /* SEARCH $6D6C  free rock slot,
                                             *   or $FF (the ROM tests N) */
uint8_t ad_searc1(uint8_t x);               /* SEARC1 $6D6E  from slot X down */
uint8_t ad_cpypos(uint8_t x, uint8_t y);    /* CPYPOS $6210  Y -> X; returns
                                             *   the new OBJ byte */
uint8_t ad_bloup(uint8_t y);                /* BLOUP  $62FC  blow up slot Y */
void    ad_ssbtlt(uint8_t a);               /* SSBTLT $6325  A -> SDELAY */
void    ad_split(uint8_t y, uint8_t x);     /* SPLIT  $6F7E  rock Y hit by X */
void    ad_newast(void);                    /* NEWAST $698E  start a wave */
void    ad_newvel(uint8_t x, uint8_t y);    /* NEWVEL $6A57  X gets Y's, varied */
uint8_t ad_newve1(uint8_t a);               /* NEWVE1 $6A7E  clamp a velocity */
void    ad_newshp(void);                    /* NEWSHP $6A37  ship to centre */
void    ad_rndpos(uint8_t x, uint8_t y);    /* RNDPOS $6A03  X at an edge */
void    ad_sreset(void);                    /* SRESET $66CF  clear both pages */

/* ------------------------------------------------------------------ */
/* draw.c                                                              */
/* ------------------------------------------------------------------ */
void    ad_pictur(uint8_t y, uint8_t x);    /* PICTUR $6BC4  Y scale, X slot,
                                             *   XCOMP = position */
void    ad_posbem(void);                    /* POSBEM $6C43  LABS + waits */
void    ad_shppic(void);                    /* SHPPIC $6E3C  ship + flame */
void    ad_shpexp(void);                    /* SHPEXP $6D77  ship fragments */
uint8_t ad_cpyvec(uint8_t x);               /* CPYVEC $6246  X+1 vectors from
                                             *   R0/R1; TEMP1/TEMP1+1 masks,
                                             *   R2 swap; returns Y */
uint8_t ad_aytor0(uint8_t y, uint8_t x);    /* AYTOR0 $6EB2  R0/R1 += Y+1, then
                                             *   CPYVEC */
uint8_t ad_cpxrot(uint8_t a);               /* CPXROT $6EE2  angle -> Y index;
                                             *   sets TEMP1, TEMP1+1, R2 */
void    ad_cptspx(void);                    /* CPTSPX $6EC1  R0/R1 -> ship hull */
void    ad_rotast(void);                    /* ROTAST $4B92  rewrite one rock
                                             *   subroutine in vector RAM */
void    ad_tripix(uint8_t a);               /* TRIPIX $4A6D  A = OBJ byte */
void    ad_shldpx(void);                    /* SHLDPX $669D  the shield */

/* ------------------------------------------------------------------ */
/* player.c                                                            */
/* ------------------------------------------------------------------ */
void     ad_shield(void);                   /* SHIELD $6675 */
void     ad_fire(void);                     /* FIRE   $64BE */
void     ad_move(void);                     /* MOVE   $6847  falls into MOVE1 */
void     ad_fire1(uint8_t y, uint8_t x);    /* FIRE1  $64E4  Y first slot to
                                             *   try, X velocity source;
                                             *   TEMP3+1, R8, ANGLE+1 set by
                                             *   the caller */
uint16_t ad_move2(uint16_t ax);             /* MOVE2  $6921  A:X clamped to
                                             *   $C001..$3FFF */

/* ------------------------------------------------------------------ */
/* enemy.c  (saucer, and TRIROT's special rock)                        */
/* ------------------------------------------------------------------ */
void ad_enemy(void);                        /* ENEMY  $6347 */
void ad_settip(void);                       /* SETTIP $4A96 */
void ad_attack(uint8_t x);                  /* ATTACK $48BE  X = special rock */
void ad_splttp(uint8_t y);                  /* SPLTTP $4800  Y = special rock;
                                             *   TEMP3 = who hit it */
void ad_rsaucr(void);                       /* RSAUCR $6826  falls into SRSAUC */
void ad_srsauc(void);                       /* SRSAUC $6835  park the saucer */
void ad_rndxyi(uint8_t x);                  /* RNDXYI $4B3D  +-6 velocities */

/* ------------------------------------------------------------------ */
/* score.c                                                             */
/* ------------------------------------------------------------------ */
void    ad_params(void);                    /* PARAMS $6A9A  the HUD */
bool    ad_scores(void);                    /* SCORES $6CB9  true = table shown */
void    ad_update(void);                    /* UPDATE $6FF0 */
bool    ad_getint(void);                    /* GETINT $6587  true = entering
                                             *   initials (A = 0 in the ROM) */
void    ad_points(uint8_t a, bool carry);   /* POINTS $6C60  A*1000 + C*10000 */
bool    ad_chkst(void);                     /* CHKST  $4BD1  true = new game */
void    ad_chkst2(void);                    /* CHKST2 $6102  "PLAYER n" */
void    ad_inital(uint8_t y);               /* INITAL $66E1  Y = INITL index */
void    ad_lives(uint8_t a, uint8_t y);     /* LIVES  $7C72  A = X/4, Y = lives */
void    ad_livess(uint8_t a, uint8_t x, uint8_t y);
                                            /* LIVESS $7C74  A = X/4, X = Y/4 */
void    ad_in3tim(void);                    /* IN3TIM $6D5E  R3 = INITL index */
uint8_t ad_gtsp(uint8_t a, uint8_t y);      /* GTSP   $6B90  returns Y */
void    ad_two(void);                       /* TWO    $6BA5 */

/* ------------------------------------------------------------------ */
/* frame.c                                                             */
/* ------------------------------------------------------------------ */
void ad_init(void);                         /* INIT   $7BF5  falls into SINIT */
void ad_sinit(void);                        /* SINIT  $7C37 */
void ad_pkytst(void);                       /* PKYTST $7FC1  also sets RVELP/M */
void ad_protck(void);                       /* START2_12 $604B checksum slice */

/* ------------------------------------------------------------------ */
/* stest.c  (DSTTST.MAC, from PWRON through SWCH)                      */
/* ------------------------------------------------------------------ */
void ad_stest3(void);                       /* STEST3 $7DF3  ROM checksum,
                                             *   bank-select test */
bool ad_stest_frame(void);                  /* STEST5..STEST7 $7E39-$7FBE,
                                             *   one display-loop pass;
                                             *   false = leave self-test */
void ad_bondsp(void);                       /* BONDSP $7C93  bonus-life
                                             *   score requirement */

#endif /* ASTDELUX_H */
