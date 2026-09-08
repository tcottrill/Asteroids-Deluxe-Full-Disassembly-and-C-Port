/* pokey.h - a POKEY (Atari C012294) sound and RNG core, plain C11.
 *
 * Translated from the AAE emulator's POKEY core
 * (aae/aae/sndhrdwr/aae_pokey.cpp / .h).  The engine-free Pokey class
 * came back nearly whole: timers 1/2/4 and IRQEN/IRQST, serial (SEROUT/
 * SERIN/SKREST), keyboard (KBCODE/SKSTAT), and the eight POT reads all
 * work here, driven through a small host seam (ad_pokey_host) in place
 * of AAE's virtual PokeyHost - see ad_pokey_set_host() below.  What
 * stays dropped is the AAE adapter itself: PokeyHost, pokey_sh_*,
 * mixer/stream/timer calls, Read_pokey_regs, quad-pokey, MEM callbacks
 * - none of that is chip behaviour, it is AAE's engine wiring.  AUDF1-4/
 * AUDC1-4/AUDCTL/STIMER/SKCTL writes and RANDOM/ALLPOT reads need no
 * host at all, same as before; every register nothing answers still
 * reads back 0xFF.
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
 * ALLPOT pot-scan window measures against) and clocks the RANDOM shift
 * chain n times - in O(1) while the chip is running, one register step
 * at a time for the few clocks after an SKCTL or AUDCTL write where the
 * chip is still settling.  A RANDOM read just returns the byte the
 * chain holds; two reads with no advance between them return the same
 * byte.
 *
 * SKCTL's init bits clear do not stop the chain: the clock keeps
 * running, and what changes is what shifts in.  The 9-bit register
 * takes a zero at its head every clock, the 17-bit extension takes what
 * the (now constant) feedback gives it, and after 17 clocks the whole
 * chain has settled to a fixed state that RANDOM reads as 0xFF (from
 * the 8th clock on).  Released sooner, the chain resumes from a mix of
 * old and new bits.  Asteroids Deluxe's NMI holds it for about 29
 * cycles ("WASTE TIME" between the two SKCTL stores at $788F and
 * $7899), so on this board every release starts from the settled state;
 * a ROM that releases inside 17 cycles gets what the chip would give
 * it.  Time that passes while held is charged like any other time -
 * there is nothing to catch up on at release, because the chain never
 * stopped.  The audio side is held the old way: the transition into
 * reset zeroes the render's poly phases (one set of shift registers on
 * the chip), and ad_pokey_render() fires no channel event and moves no
 * poly phase while held - outputs sit at their current levels, the
 * sample clock alone keeps running.  The RANDOM chain and the render
 * phases stay separate copies: both count POKEY cycles, but the render
 * lags machine time by up to a host tick, and reconciling them at a
 * read would need the CPU cycle position this port does not have.
 *
 * The three hardware timers (TIMR1/TIMR2/TIMR4, driven by channels
 * 0/1/3) count down the same n-cycle slice ad_pokey_advance() walks:
 * each timer's countdown is stepped by n in O(1), and if n reaches or
 * passes it the channel's divisor is added back on as many times as it
 * takes to land past n - IRQST is a latch, so a slice that crosses
 * several periods still only raises the IRQ once.  SKCTL's init bits
 * clear stop the 15 kHz and 64 kHz clocks but not the 1.79 MHz one, so
 * a held chip's timers only count if their channel runs off the fast
 * clock (AUDCTL's CH1/CH3 fast-clock bits, and the joined partner of
 * such a channel); the slow-clock ones stand still until release,
 * keeping their phase.  IRQEN only gates whether
 * a borrow reaches the IRQST latch and the host's raise_irq() - it
 * never touches a countdown's phase, because the real timers keep
 * counting whether or not their IRQ is enabled; a countdown re-arms
 * (restarts a full period from now) only where AUDF/AUDCTL/STIMER
 * writes say it does - see ad_pokey_write().
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
#define W_SKREST  0x0A
#define W_POTGO   0x0B
#define W_SEROUT  0x0D
#define W_IRQEN   0x0E
#define W_SKCTL   0x0F

/* ---- read-register offsets (addr & 0x0F) ---- */
#define R_POT0    0x00
#define R_ALLPOT  0x08
#define R_KBCODE  0x09
#define R_RANDOM  0x0A
#define R_SERIN   0x0D
#define R_IRQST   0x0E
#define R_SKSTAT  0x0F

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

/* ---- IRQEN / IRQST bits ---- */
#define IRQ_BREAK  0x80
#define IRQ_KEYBD  0x40
#define IRQ_SERIN  0x20
#define IRQ_SEROR  0x10
#define IRQ_SEROC  0x08
#define IRQ_TIMR4  0x04
#define IRQ_TIMR2  0x02
#define IRQ_TIMR1  0x01

/* ---- SKCTL bits ---- */
#define SK_BREAKEN  0x80   /* unused: no serial break generation modelled */
#define SK_BPS      0x70   /* unused: no serial baud-rate divider modelled */
#define SK_TWOTONE  0x08   /* unused: no serial two-tone mode modelled */
#define SK_FASTPOT  0x04
#define SK_INIT     0x03

/* ---- SKSTAT bits ---- */
#define ST_FRAME       0x80
#define ST_OVERRUN     0x40
#define ST_KBERR       0x20
#define ST_SERIN_BUSY  0x10
#define ST_SHIFT       0x08
#define ST_KEYBD       0x04
#define ST_SEROUT_ACT  0x02

/* ---- timing divisors ---- */
#define DIV_64  28
#define DIV_15  114

/* The RANDOM shift chain, register for register as the chip has it -
 * poly_core.v of Nick Mikstas's atari_pokey, a gate-level transcription
 * of Atari's POKEY schematics: the eight flip-flops of the 9-bit
 * register (RANDOM reads their complement), the eight of the 17-bit
 * extension, the flop that delays AUDCTL's 9/17 select by a clock, and
 * the three registered NOR outputs of the 9/17 switch, whose NOR is what
 * the head of the 9-bit register takes next clock (or a zero, while the
 * SKCTL init bits are clear).  Hardware polarity throughout.  The chain
 * clocks every POKEY cycle whatever SKCTL and AUDCTL say; those two only
 * change what feeds it.  See pokey.c's "RANDOM shift chain" section. */
typedef struct ad_rng_chain {
    uint8_t l9;        /* lfsr9bit[7:0]:  shifts right, head is bit 7 */
    uint8_t l17;       /* lfsr17bit[7:0]: shifts right, fed by the XNOR
                        * of l9 bits 5 and 0 */
    uint8_t swdelay;   /* swDelay: the poly-select bit, one clock late */
    uint8_t nd;        /* norsDelayed[2:0]: bit 0 the 17-bit path (l17
                        * bit 0), bit 2 the 9-bit path (the XNOR), bit 1
                        * the one-clock blank when the select flips */
} ad_rng_chain;

/* ---- render gain: AUDC volume nibble (0-15) times this is the level a
 * fully-on channel contributes to a sample ---- */
#define POKEY_GAIN (32767 / 11)

/* Host seam: the entire contract between the chip and the world, in
 * place of AAE's virtual PokeyHost.  A plain vtable-of-function-pointers
 * struct, not a C++ interface, so it builds on a microcontroller.  Every
 * callback is optional - ad_pokey NULL-checks each before calling it -
 * and the whole pointer may be NULL (ad_pokey_set_host(p, NULL)), which
 * is the default: a chip with no host wired up behaves exactly as this
 * port did before the host seam existed (writes/reads/advance/render
 * all still work; IRQs are just never raised, pots read 0xFF, keyboard
 * and serial are never fed). */
typedef struct ad_pokey_host {
    void *ctx;
    void (*raise_irq)(void *ctx, uint8_t mask);          /* IRQST bits that just fired */
    int  (*pot_read)(void *ctx, int n);                   /* POT0-7 count, 0..228 */
    int  (*keyboard_scan)(void *ctx, uint8_t *code, uint8_t *flags); /* 1 if a code is waiting */
    int  (*serial_in)(void *ctx);                         /* next SERIN byte, or -1 */
    void (*serial_out)(void *ctx, uint8_t data);
} ad_pokey_host;

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
    uint32_t divisor[4];     /* true half-period in base-clock ticks; also
                               * the period each hardware timer below re-
                               * arms to (channel_period(), recompute_channel()) */
    uint32_t rmax[4];        /* render Div_n_max (= divisor[i], or frozen) */
    uint32_t cnt[4];         /* render countdown (Div_n_cnt) */
    uint8_t  out[4];         /* render output level / toggle latch (Outvol) */
    int32_t  vol[4];         /* AUDC volume * GAIN (AUDV) */

    /* poly render phases + sample clock */
    uint32_t p4, p5, p9, p17, poly_adjust;
    uint32_t samp_cnt, samp_max;

    /* RNG: the RANDOM shift chain (see ad_rng_chain), clocked by
     * ad_pokey_advance() every POKEY cycle; rng_enabled is SKCTL's init
     * bits set, i.e. the chip is not held - while it is, the chain
     * takes zeros at its head instead of the switch's output */
    uint8_t  rng_enabled;
    ad_rng_chain rng;

    /* hardware timers: TIMR1/TIMR2/TIMR4, driven by channels 0/1/3
     * (tcnt index w -> channel timer_channel(w) in pokey.c).  Countdowns
     * in POKEY cycles, stepped by ad_pokey_advance() while rng_enabled
     * or while the channel runs off the fast clock (see timer_runs());
     * IRQEN, IRQST are the usual latch-and-mask pair (see fire_irq() and
     * ad_pokey_write()'s W_IRQEN case) */
    uint32_t tcnt[3];
    uint8_t  IRQEN, IRQST;

    /* keyboard: KBCODE/SKSTAT track AAE's keyboard_key()/scan_keyboard();
     * kbd_pending mirrors AAE's kbd_pending_ (an unread code -> the next
     * key sets ST_KBERR instead of silently overwriting it) */
    uint8_t  KBCODE, SKSTAT;
    bool     kbd_pending;

    /* serial: byte-at-a-time pass-through, no baud/framing modelled
     * (SK_BPS/SK_BREAKEN/SK_TWOTONE stay unused - see pokey.h's bit
     * defines) */
    uint8_t  SERIN, SEROUT;

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

    /* host wiring: NULL until ad_pokey_set_host() - see ad_pokey_host
     * above.  Not touched by ad_pokey_reset(), same as allpot: it is
     * host-owned wiring, not chip state a reset clears. */
    const ad_pokey_host *host;
} ad_pokey;

void    ad_pokey_init(ad_pokey *p, uint32_t clock_hz, uint32_t sample_rate);
void    ad_pokey_reset(ad_pokey *p);
void    ad_pokey_write(ad_pokey *p, uint8_t reg, uint8_t v);   /* reg 0..15 */
uint8_t ad_pokey_read(ad_pokey *p, uint8_t reg);               /* RANDOM, ALLPOT, IRQST, SKSTAT,
                                                                * KBCODE, SERIN, POT0-7; else 0xFF */
void    ad_pokey_set_allpot(ad_pokey *p, uint8_t v);           /* the DIP bank on the pot pins */
void    ad_pokey_set_host(ad_pokey *p, const ad_pokey_host *h); /* NULL = no host (default) */
void    ad_pokey_advance(ad_pokey *p, uint32_t cycles);        /* machine time, POKEY cycles; the
                                                                * only thing that clocks RANDOM and
                                                                * the hardware timers */
void    ad_pokey_render(ad_pokey *p, int16_t *dst, int n);     /* n mono samples at sample_rate */
void    ad_pokey_keyboard_key(ad_pokey *p, uint8_t code, uint8_t flags, bool down);
void    ad_pokey_serial_receive(ad_pokey *p, uint8_t data);
void    ad_pokey_poll(ad_pokey *p);                             /* host per-frame poll: pulls one
                                                                * keyboard code and one serial byte
                                                                * through the host, if either is
                                                                * waiting */

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
