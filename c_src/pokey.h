/* pokey.h - a POKEY (Atari C012294) sound and RNG core, plain C11.
 *
 * Translated from the AAE emulator's POKEY core
 * (aae/aae/sndhrdwr/aae_pokey.cpp / .h), with every engine binding
 * removed: no timers/IRQ (rearm_timers, on_timer_fire, fire_irq, IRQEN/
 * IRQST), no serial, no keyboard, no eight-pot reads, and none of the
 * AAE adapter (PokeyHost, pokey_sh_*, mixer/stream/timer calls,
 * Read_pokey_regs, quad-pokey, MEM callbacks).  What is kept is exactly
 * what this board uses: AUDF1-4/AUDC1-4/AUDCTL/STIMER/SKCTL writes, and
 * RANDOM/ALLPOT reads (everything else reads back 0xFF, as real POKEY
 * does for a register nothing answers).
 *
 * This is not a ROM routine - astdelux2_main.asm never names a "POKEY.C"
 * module, the chip is hardware the ROM merely talks to - so it carries
 * no ad_ prefix and no ROM address comments.  It has no platform
 * includes (no stdio, no Windows headers): this file must build on a
 * microcontroller as well as on Windows, same as the rest of c_src.
 *
 * ---------------------------------------------------------------------
 * Time model (read this before touching read()/advance()/render()).
 *
 * The chip is exact given an exact clock feed, and charges nothing on
 * its own.  ad_pokey_advance(p, n) is the only thing that moves machine
 * time: it adds n POKEY cycles to the running `cycles` count (which the
 * ALLPOT pot-scan window measures against) and, while SKCTL's init bits
 * are set, steps the 9- and 17-bit RANDOM polynomials by n, in O(1).
 * A RANDOM read just returns the byte at the current position; two
 * reads with no advance between them return the same byte.  While SKCTL
 * holds the chip in reset (init bits clear) the positions sit at 0 and
 * advance() does not move them; counting starts from the write that
 * releases the reset, so time that passed while held is never charged
 * afterwards.  The audio side is held the same way: the transition into
 * reset zeroes the render's poly phases (one set of shift registers on
 * the chip), and ad_pokey_render() fires no channel event and moves no
 * poly phase
 * while held - outputs sit at their current levels, the sample clock
 * alone keeps running.  The RANDOM positions and the render phases stay
 * separate copies: both count POKEY cycles, but the render lags machine
 * time by up to a host tick, and reconciling them at a read would need
 * the CPU cycle position this port does not have.
 *
 * Who feeds the clock, and how exactly, is the host's business:
 *
 *   - coarse machine time: one NMI period (6048 POKEY cycles at
 *     1.512 MHz / 4 ms) right before ad_nmi(), because that is when the
 *     board's own clock would have ticked;
 *   - a stand-in per game read: this port has no 6502 with a cycle
 *     counter, so the hosts' ad_hw_random() advances a flat
 *     AD_RANDOM_READ_COST (64) before each un-annotated read - the
 *     polynomial has to move *something* between two reads in the same
 *     NMI, or a tight loop would see the identical byte.  A model, in
 *     the host, not in the chip;
 *   - exact deltas where a ROM cares: translated code that reads RANDOM
 *     twice in a short window calls ad_pokey_advance() with the true
 *     6502 cycle distance between the two bus strobes.  Tempest's
 *     protection at $AE1F (LDA abs then LDY abs on the same chip, both
 *     strobing on their fourth cycle) needs exactly 4 between the reads,
 *     and expects the upper nibble of the first byte to be the lower
 *     nibble of the second - see probe_pokey.c's check (5).
 *
 * Cycles here are POKEY cycles.  On this board and on Tempest the 6502
 * and POKEY share 12.096 MHz / 8, so one CPU cycle is one POKEY cycle
 * and a caller passes 6502 cycle counts straight through; a board with
 * a different ratio converts before calling.  Several chips on one
 * board (Tempest has two) must be advanced by the same amounts from the
 * same places, or their sequences drift apart.
 */
#ifndef AD_POKEY_H
#define AD_POKEY_H

#include <stdint.h>
#include <stdbool.h>

/* ---- write-register offsets (addr & 0x0F) ---- */
#define W_AUDF1   0x00
#define W_AUDC1   0x01
#define W_AUDF2   0x02
#define W_AUDC2   0x03
#define W_AUDF3   0x04
#define W_AUDC3   0x05
#define W_AUDF4   0x06
#define W_AUDC4   0x07
#define W_AUDCTL  0x08
#define W_STIMER  0x09
#define W_SKREST  0x0A   /* dropped: cleared IRQ status bits this port never sets */
#define W_POTGO   0x0B
#define W_SEROUT  0x0D   /* dropped: no serial */
#define W_IRQEN   0x0E   /* dropped: no IRQ */
#define W_SKCTL   0x0F

/* ---- read-register offsets (addr & 0x0F) ---- */
#define R_POT0    0x00
#define R_ALLPOT  0x08
#define R_KBCODE  0x09   /* dropped: no keyboard; reads 0xFF */
#define R_RANDOM  0x0A
#define R_SERIN   0x0D   /* dropped: no serial; reads 0xFF */
#define R_IRQST   0x0E   /* dropped: no IRQ; reads 0xFF */
#define R_SKSTAT  0x0F   /* dropped: no serial/keyboard status; reads 0xFF */

/* ---- AUDC bits ---- */
#define AUDC_NOTPOLY5  0x80
#define AUDC_POLY4     0x40
#define AUDC_PURE      0x20
#define AUDC_VOLONLY   0x10
#define AUDC_VOLMASK   0x0F

/* ---- AUDCTL bits ---- */
#define CTL_POLY9      0x80
#define CTL_CH1_HICLK  0x40
#define CTL_CH3_HICLK  0x20
#define CTL_CH12_JOIN  0x10
#define CTL_CH34_JOIN  0x08
#define CTL_CH1_FILTER 0x04
#define CTL_CH2_FILTER 0x02
#define CTL_CLK15      0x01

/* ---- SKCTL bits ---- */
#define SK_BREAKEN  0x80   /* unused: no serial break to enable */
#define SK_BPS      0x70   /* unused: no serial */
#define SK_TWOTONE  0x08   /* unused: no serial */
#define SK_FASTPOT  0x04
#define SK_INIT     0x03

/* ---- timing divisors ---- */
#define DIV_64  28
#define DIV_15  114

/* ---- render gain: AUDC volume nibble (0-15) times this is the level a
 * fully-on channel contributes to a sample ---- */
#define POKEY_GAIN (32767 / 11)

/* A POKEY chip instance.  Everything here is the subset of Pokey's
 * private state this board's use of the chip touches; see the .cpp for
 * the full member list this was trimmed from. */
typedef struct ad_pokey {
    /* registers */
    uint8_t  AUDF[4];
    uint8_t  AUDC[4];
    uint8_t  AUDCTL;
    uint8_t  SKCTL;

    /* clocking */
    uint32_t base_clock;     /* chip master clock, Hz (e.g. 1512000) */
    uint32_t sys_freq;       /* render sample rate, Hz (e.g. 44100) */
    uint32_t base_mult;      /* DIV_64 or DIV_15, from AUDCTL bit 0 */

    /* audio channels */
    uint32_t divisor[4];     /* true half-period in base-clock ticks (timer-
                               * facing in the original; nothing reads this
                               * without IRQ/timers, kept for fidelity) */
    uint32_t rmax[4];        /* render Div_n_max (= divisor[i], or frozen) */
    uint32_t cnt[4];         /* render countdown (Div_n_cnt) */
    uint8_t  out[4];         /* render output level / toggle latch (Outvol) */
    int32_t  vol[4];         /* AUDC volume * GAIN (AUDV) */

    /* poly render phases + sample clock */
    uint32_t p4, p5, p9, p17, poly_adjust;
    uint32_t samp_cnt, samp_max;

    /* RNG: the 9- and 17-bit polynomial positions, stepped by
     * ad_pokey_advance() while the SKCTL init bits are set (rng_enabled),
     * held at 0 while they are clear; RANDOM reads index the tables at
     * these positions */
    uint8_t  rng_enabled;
    uint32_t rand_pos9, rand_pos17;

    /* running machine time in POKEY cycles, fed only by
     * ad_pokey_advance(); the ALLPOT scan window measures against it */
    uint64_t cycles;

    /* pots: ALLPOT is derived from the scan window (see ad_pokey_read's
     * R_ALLPOT case) - during a scan it reads the still-counting-line
     * mask (the host-supplied DIP byte), after it 0x00. */
    uint8_t  allpot;          /* the DIP bank on the pot pins, host-supplied */
    bool     pot_scanning;
    bool     pot_scan_ever;   /* sticky: true from this chip's first POTGO on */
    uint64_t pot_scan_start;
} ad_pokey;

void    ad_pokey_init(ad_pokey *p, uint32_t clock_hz, uint32_t sample_rate);
void    ad_pokey_reset(ad_pokey *p);
void    ad_pokey_write(ad_pokey *p, uint8_t reg, uint8_t v);   /* reg 0..15 */
uint8_t ad_pokey_read(ad_pokey *p, uint8_t reg);               /* RANDOM, ALLPOT; else 0xFF */
void    ad_pokey_set_allpot(ad_pokey *p, uint8_t v);           /* the DIP bank on the pot pins */
void    ad_pokey_advance(ad_pokey *p, uint32_t cycles);        /* machine time, POKEY cycles;
                                                                * the only thing that clocks RANDOM */
void    ad_pokey_render(ad_pokey *p, int16_t *dst, int n);     /* n mono samples at sample_rate */

#ifdef AD_PROBE
/* Test-only accessors for the shared static poly/RNG tables (built once,
 * on first use).  Sizes: poly4=15, poly5=31, poly9=511, poly17=131071,
 * rand9=511, rand17=131071 - the chip's maximal-length LFSRs; see
 * probe_pokey.c's check (1). */
const uint8_t *ad_pokey_dbg_poly4(void);
const uint8_t *ad_pokey_dbg_poly5(void);
const uint8_t *ad_pokey_dbg_poly9(void);
const uint8_t *ad_pokey_dbg_poly17(void);
const uint8_t *ad_pokey_dbg_rand9(void);
const uint8_t *ad_pokey_dbg_rand17(void);
#endif

#endif /* AD_POKEY_H */
