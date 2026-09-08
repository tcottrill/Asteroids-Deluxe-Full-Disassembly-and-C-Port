/* pokey.c - the POKEY core; see pokey.h for what was kept and dropped,
 * and for the time model ad_pokey_advance()/ad_pokey_read() share.
 *
 * Translated from the AAE emulator's engine-free POKEY core (Pokey and
 * PokeyHost), minus the AAE adapter (pokey_sh_*, mixer/stream/timer
 * calls, Read_pokey_regs, quad-pokey, MEM callbacks) - that layer is
 * AAE's engine wiring, not chip behaviour.  The timers/IRQ/serial/
 * keyboard/pot pieces are here, driven through ad_pokey_host in place of
 * AAE's virtual PokeyHost.  The audio poly tables and the SKCTL hold on
 * the audio side follow MAME's pokey.cpp (0.286,
 * src/devices/sound/pokey.cpp) - credit to the MAME team for the LFSR
 * arithmetic.  The pot scanner follows the Altirra Hardware Reference
 * (Avery Lee), which measured the real chip.  The RANDOM shift chain -
 * its registers, what SKCTL's init bits do to it clock by clock, and
 * which timers keep counting while it is held - follows a gate-level
 * transcription of Atari's schematics (Nick Mikstas's atari_pokey,
 * poly_core.v, clock_gen_core.v, freq_control.v) - see pokey.h's
 * ad_rng_chain.  Where a comment below says "as AAE" or "as the
 * original" it means the algorithm is copied verbatim; only the C++
 * class's `this->member` becomes `p->member`, and PokeyHost's virtual
 * calls become ad_pokey_host's optional function-pointer calls (or a
 * struct field this port keeps locally, like `cycles` for
 * now_cpu_cycles() and `allpot` for the DIP byte).
 *
 * The parts taken from MAME's pokey.cpp are used under that file's
 * BSD-3-Clause terms, copyright the MAME team and the copyright holders
 * it names (Brad Oliver, Eric Smith, Juergen Buchmueller, and others):
 * redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that redistributions of source
 * code retain this notice, that redistributions in binary form
 * reproduce it in the documentation, and that the names of the
 * copyright holders are not used to endorse derived products without
 * permission; the software is provided "as is" without warranty.  The
 * rest of this file is under the project's licence (see LICENSE).
 */
#include <string.h>

#include "pokey.h"

/* ------------------------------------------------------------------ */
/* Poly/RNG tables - built once, on first use                          */
/* ------------------------------------------------------------------ */
/* The real chip's LFSRs, all maximal-length (2^n-1 states; probe_pokey.c
 * check (1) proves it).  poly4/5 are a Fibonacci LFSR with XNOR
 * feedback on bits 2 and size-1, seeded from 0.  poly9/17 feed bit0 XOR
 * bit5 back into bit8 (9-bit), seeded from all ones; the 17-bit case is
 * that 9-bit LFSR extended by an 8-bit shift register, folding bit8 XOR
 * bit13 back into bit7 of the low byte.  The render indexes g_polyN for
 * the audio toggle bit (`& 1`).  RANDOM does not use a table: it reads
 * the shift chain itself (the "RANDOM shift chain" section below) - the
 * g_randN slices (`& 0xff` for 9-bit, `>> 8 & 0xff` for 17-bit) are
 * built only for the probe, which checks the chain against them.
 *
 * These are `static`, not `const`, and built at runtime rather than
 * typed in - the one deliberate exception to "no globals besides g and
 * const tables" (CONVENTIONS.md rule 10).  On a microcontroller too
 * small for a 128 KB runtime table, the alternative is either a
 * flash-resident `const` table (computed once, offline, by this same
 * arithmetic) or stepping the LFSR live in render() instead of
 * precomputing it - either avoids the RAM cost; this port takes the
 * table because the Windows/headless hosts have RAM to spare. */
static uint8_t g_poly4[15];
static uint8_t g_poly5[31];
static uint8_t g_poly9[511];
static uint8_t g_poly17[131071];
#ifdef AD_PROBE
static uint8_t g_rand9[511];
static uint8_t g_rand17[131071];
#endif
static bool    g_tables_built = false;

/* Jump tables for the RANDOM shift chain: for each poly select (index
 * 0 = 17-bit, 1 = 9-bit) the chain's one-clock transition as a 17x17
 * matrix over GF(2), raised to every power of two up to 2^31, so that
 * ad_pokey_advance() can clock the chain n times in O(popcount(n)).
 * Row r is the mask of chain bits that feed bit r; a step is seventeen
 * AND-and-parity operations.  Built by build_tables(). */
#define CHAIN_BITS 17
static uint32_t g_jump[2][32][CHAIN_BITS];

/* Fibonacci LFSR: each step folds bits 2 and (size-1) of the running
 * state through XNOR into a new bit shifted in at position 0; only the
 * table entry is masked down to `size` bits; the running `lfsr` is left
 * to grow (it never affects the tap bits, which stay at fixed low
 * offsets from the shifted-in end). */
static void poly_init_4_5(uint8_t *poly, int size)
{
    uint32_t mask = (1u << size) - 1;
    uint32_t lfsr = 0;
    int xorbit = size - 1;
    for (uint32_t i = 0; i < mask; ++i) {
        uint32_t newbit = (~((lfsr >> 2) ^ (lfsr >> xorbit))) & 1u;
        lfsr = (lfsr << 1) | newbit;
        poly[i] = (uint8_t)((lfsr & mask) & 1u);
    }
}

/* Writes both slices of each state in the same pass (see the table
 * comment above): g_polyN gets the toggle bit, g_randN (probe builds
 * only; NULL otherwise) gets the RANDOM byte.  Seeded from lfsr = mask
 * (all ones): in this register's polarity that is the complement of the
 * chip's all-zero 9-bit register, so the tables line up with the chain
 * (see chain_to_vec()). */
static void poly_init_9_17(uint8_t *poly, uint8_t *rnd, int size)
{
    uint32_t mask = (size == 17) ? 0x1FFFFu : 0x1FFu;
    uint32_t lfsr = mask;
    for (uint32_t i = 0; i < mask; ++i) {
        uint8_t byte;
        if (size == 17) {
            uint32_t in8 = ((lfsr >> 8) & 1u) ^ ((lfsr >> 13) & 1u);
            uint32_t in  = lfsr & 1u;
            lfsr >>= 1;
            lfsr = (lfsr & 0xFF7Fu) | (in8 << 7);
            lfsr = (in << 16) | lfsr;
            byte = (uint8_t)((lfsr >> 8) & 0xFFu);
        } else {
            uint32_t in = (lfsr & 1u) ^ ((lfsr >> 5) & 1u);
            lfsr >>= 1;
            lfsr = (in << 8) | lfsr;
            byte = (uint8_t)(lfsr & 0xFFu);
        }
        if (rnd)
            rnd[i] = byte;
        poly[i] = (uint8_t)(lfsr & 1u);
    }
}

/* ------------------------------------------------------------------ */
/* RANDOM shift chain                                                  */
/* ------------------------------------------------------------------ */
/* The chip's 9/17-bit polynomial, register for register from
 * poly_core.v (see ad_rng_chain in pokey.h for the register names).
 * Hardware polarity: the 9-bit register's bits are the complement of
 * the RANDOM byte, the XNOR of its bits 5 and 0 feeds the 17-bit
 * extension, and the head of the 9-bit register takes the NOR of the
 * three registered switch outputs - or a zero while the SKCTL init
 * bits are clear (Init).  With the 17-bit poly selected the switch
 * routes bit 0 of the extension round to the head (17 stages counting
 * the switch's own flop); with the 9-bit poly it routes the XNOR
 * straight round (9 stages), and the extension keeps shifting unseen.
 * The clock never stops: Init and the select only change the feeds.
 *
 * Holding Init shifts a zero into the head every clock.  After eight
 * the 9-bit register is clear (RANDOM 0xFF), from the ninth the XNOR of
 * two zeros feeds ones into the extension, and after seventeen the
 * whole chain is at rest: it stays there for as long as the hold lasts,
 * and a release inside those seventeen clocks resumes from whatever mix
 * of old and new bits the chain holds at that moment.  The one-clock
 * blank when the select flips (nors[1]) and the clock's delay on the
 * select (swDelay) are in here too, so flipping AUDCTL's poly bit
 * mid-run does what the chip does. */

/* One clock of the chain: the negedge always block of poly_core.v. */
static void chain_step(ad_rng_chain *c, bool init, bool sel9)
{
    uint32_t fb917 = (((c->l9 >> 5) ^ c->l9) & 1u) ^ 1u;        /* ~(l9[5] ^ l9[0]) */
    uint32_t nors0 = ((c->l17 & 1u) | (uint32_t)sel9) ^ 1u;     /* ~(l17[0] | sel9) */
    uint32_t nors1 = ((uint32_t)c->swdelay | (uint32_t)!sel9) ^ 1u; /* ~(swDelay | ~sel9) */
    uint32_t nors2 = ((uint32_t)!sel9 | fb917) ^ 1u;            /* ~(~sel9 | fb917) */
    uint32_t swout = ((uint32_t)init | (uint32_t)(c->nd != 0)) ^ 1u; /* ~(Init | nD[0..2]) */
    c->l9      = (uint8_t)((c->l9 >> 1) | (swout << 7));
    c->l17     = (uint8_t)((c->l17 >> 1) | (fb917 << 7));
    c->swdelay = (uint8_t)sel9;
    c->nd      = (uint8_t)(nors0 | (nors1 << 1) | (nors2 << 2));
}

/* The state a held chain settles in (and never leaves while held):
 * 9-bit register clear, extension all ones, every switch output low,
 * the select delay caught up. */
static bool chain_settled(const ad_rng_chain *c, bool sel9)
{
    return c->l9 == 0 && c->l17 == 0xFF && c->nd == 0 && c->swdelay == (uint8_t)sel9;
}

/* Running steadily: not held, the select delay caught up, and only the
 * selected path's switch output live.  Then the next clock is a linear
 * function of the seventeen bits chain_to_vec() packs, and the jump
 * tables apply; otherwise (the two clocks after a select flip) it is
 * stepped one clock at a time. */
static bool chain_steady(const ad_rng_chain *c, bool sel9)
{
    return c->swdelay == (uint8_t)sel9 && (c->nd & 2u) == 0 &&
           (c->nd & (sel9 ? 1u : 4u)) == 0;
}

/* The steady chain as a 17-bit vector in the tables' polarity (every
 * bit complemented): bits 15..8 the 9-bit register (RANDOM as read),
 * bits 7..0 the extension, bit 16 the complement of what the switch
 * will feed the head next clock.  This is exactly the register layout
 * poly_init_9_17() steps for the 17-bit poly, and its 9-bit poly is
 * bits 16..8 of the same vector - which is why the probe's table
 * positions and the chain agree. */
static uint32_t chain_to_vec(const ad_rng_chain *c)
{
    uint32_t swout_next = (uint32_t)(c->nd == 0);
    return ((uint32_t)(uint8_t)~c->l9 << 8) | (uint32_t)(uint8_t)~c->l17 |
           ((swout_next ^ 1u) << 16);
}

static void vec_to_chain(ad_rng_chain *c, uint32_t v, bool sel9)
{
    uint32_t nswout = (v >> 16) & 1u;
    c->l9      = (uint8_t)~(v >> 8);
    c->l17     = (uint8_t)~v;
    c->swdelay = (uint8_t)sel9;
    c->nd      = (uint8_t)(sel9 ? nswout << 2 : nswout);
}

/* One clock on the vector (chain_step() in the tables' polarity, steady
 * state only): shift right; the head (bit 15) takes bit 16; the
 * extension's head (bit 7) takes bit 8 XOR bit 13; bit 16 takes bit 0
 * for the 17-bit poly, bit 8 XOR bit 13 for the 9-bit one. */
static uint32_t chain_step_vec(uint32_t v, bool sel9)
{
    uint32_t t = ((v >> 8) ^ (v >> 13)) & 1u;
    uint32_t nv = (v >> 1) & 0x7F7Fu;
    nv |= ((v >> 16) & 1u) << 15;
    nv |= t << 7;
    nv |= (sel9 ? t : (v & 1u)) << 16;
    return nv;
}

static uint32_t parity17(uint32_t x)
{
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return x & 1u;
}

static uint32_t vec_apply(const uint32_t *m, uint32_t v)
{
    uint32_t out = 0;
    for (int r = 0; r < CHAIN_BITS; ++r)
        out |= parity17(m[r] & v) << r;
    return out;
}

/* g_jump[sel9][0] from chain_step_vec() a unit vector at a time, then
 * each power of two as the square of the one before (row r of A*A is
 * the XOR of A's rows named by row r of A). */
static void build_jump(bool sel9)
{
    uint32_t (*m)[CHAIN_BITS] = g_jump[sel9];
    for (int r = 0; r < CHAIN_BITS; ++r)
        m[0][r] = 0;
    for (int b = 0; b < CHAIN_BITS; ++b) {
        uint32_t out = chain_step_vec(1u << b, sel9);
        for (int r = 0; r < CHAIN_BITS; ++r)
            if ((out >> r) & 1u)
                m[0][r] |= 1u << b;
    }
    for (int k = 1; k < 32; ++k) {
        for (int r = 0; r < CHAIN_BITS; ++r) {
            uint32_t row = 0;
            for (int b = 0; b < CHAIN_BITS; ++b)
                if ((m[k - 1][r] >> b) & 1u)
                    row ^= m[k - 1][b];
            m[k][r] = row;
        }
    }
}

/* Clock the chain n times.  Held: one clock at a time until it settles
 * (at most seventeen), then nothing - the settled state is a fixed
 * point.  Running: one clock at a time through a select flip's two
 * transition clocks, then the jump tables for the rest. */
static void chain_advance(ad_rng_chain *c, uint32_t n, bool init, bool sel9)
{
    while (n) {
        if (init) {
            if (chain_settled(c, sel9))
                return;
            chain_step(c, true, sel9);
            --n;
            continue;
        }
        if (!chain_steady(c, sel9)) {
            chain_step(c, false, sel9);
            --n;
            continue;
        }
        uint32_t v = chain_to_vec(c);
        for (int k = 0; k < 32; ++k)
            if ((n >> k) & 1u)
                v = vec_apply(g_jump[sel9][k], v);
        vec_to_chain(c, v, sel9);
        return;
    }
}

/* The chain as ad_pokey_reset() leaves it: at rest under a long hold
 * with the 17-bit poly selected (AUDCTL = 0), the state a chip that has
 * seen SKCTL = 0 for seventeen clocks is in. */
static void chain_reset(ad_rng_chain *c)
{
    c->l9 = 0; c->l17 = 0xFF; c->swdelay = 0; c->nd = 0;
}

static void build_tables(void)
{
    if (g_tables_built)
        return;
    poly_init_4_5(g_poly4, 4);
    poly_init_4_5(g_poly5, 5);
#ifdef AD_PROBE
    poly_init_9_17(g_poly9, g_rand9, 9);
    poly_init_9_17(g_poly17, g_rand17, 17);
#else
    poly_init_9_17(g_poly9, NULL, 9);
    poly_init_9_17(g_poly17, NULL, 17);
#endif
    build_jump(false);
    build_jump(true);
    g_tables_built = true;
}

#ifdef AD_PROBE
const uint8_t *ad_pokey_dbg_poly4(void)  { build_tables(); return g_poly4;  }
const uint8_t *ad_pokey_dbg_poly5(void)  { build_tables(); return g_poly5;  }
const uint8_t *ad_pokey_dbg_poly9(void)  { build_tables(); return g_poly9;  }
const uint8_t *ad_pokey_dbg_poly17(void) { build_tables(); return g_poly17; }
const uint8_t *ad_pokey_dbg_rand9(void)  { build_tables(); return g_rand9;  }
const uint8_t *ad_pokey_dbg_rand17(void) { build_tables(); return g_rand17; }
#endif

/* ------------------------------------------------------------------ */
/* Channel period, exactly as documented in aae_pokey.cpp               */
/* ------------------------------------------------------------------ */
/* Returns the TRUE half-period in base-clock ticks. */
static uint32_t channel_period(const uint8_t *AUDF, uint8_t AUDCTL, uint32_t base_mult, int ch)
{
    bool hi1 = (AUDCTL & CTL_CH1_HICLK) != 0;
    bool hi3 = (AUDCTL & CTL_CH3_HICLK) != 0;
    uint32_t d;
    switch (ch) {
    case 0: d = hi1 ? (uint32_t)(AUDF[0] + 4) : (uint32_t)((AUDF[0] + 1) * base_mult); break;
    case 1:
        if (AUDCTL & CTL_CH12_JOIN)
            d = hi1 ? (uint32_t)(AUDF[1] * 256 + AUDF[0] + 7)
                    : (uint32_t)((AUDF[1] * 256 + AUDF[0] + 1) * base_mult);
        else
            d = (uint32_t)((AUDF[1] + 1) * base_mult);
        break;
    case 2: d = hi3 ? (uint32_t)(AUDF[2] + 4) : (uint32_t)((AUDF[2] + 1) * base_mult); break;
    case 3:
        if (AUDCTL & CTL_CH34_JOIN)
            d = hi3 ? (uint32_t)(AUDF[3] * 256 + AUDF[2] + 7)
                    : (uint32_t)((AUDF[3] * 256 + AUDF[2] + 1) * base_mult);
        else
            d = (uint32_t)((AUDF[3] + 1) * base_mult);
        break;
    default: return 1;
    }
    return d ? d : 1;
}

static void recompute_channel(ad_pokey *p, int ch)
{
    if (ch < 0 || ch >= 4)
        return;
    p->divisor[ch] = channel_period(p->AUDF, p->AUDCTL, p->base_mult, ch);
}

static void recompute_all(ad_pokey *p)
{
    for (int i = 0; i < 4; ++i)
        recompute_channel(p, i);
}

/* Hardware timer index (0,1,2 = TIMR1/TIMR2/TIMR4) -> the AUDF channel
 * that drives its period, and its IRQEN/IRQST bit.  As AAE's
 * timer_channel()/timer_irq_bit(); the IRQ_TIMR1/2/4 bits are 1<<w by
 * definition. */
static int timer_channel(int which)
{
    return which == 2 ? 3 : which;
}

static uint8_t timer_irq_bit(int which)
{
    return which == 0 ? IRQ_TIMR1 : which == 1 ? IRQ_TIMR2 : IRQ_TIMR4;
}

/* Does timer w count this slice?  Always while the chip runs.  Held
 * (SKCTL init bits clear), the 15 kHz and 64 kHz clocks stop but the
 * 1.79 MHz one does not (clock_gen_core.v holds its two clock LFSRs on
 * Init; freq_control.v's carry for channels 1 and 3 is the fast-clock
 * enable OR the slow clock), so a channel on the fast clock - and the
 * joined partner it clocks - keeps counting. */
static bool timer_runs(const ad_pokey *p, int which)
{
    if (p->rng_enabled)
        return true;
    switch (which) {
    case 0:  return (p->AUDCTL & CTL_CH1_HICLK) != 0;
    case 1:  return (p->AUDCTL & (CTL_CH12_JOIN | CTL_CH1_HICLK)) == (CTL_CH12_JOIN | CTL_CH1_HICLK);
    default: return (p->AUDCTL & (CTL_CH34_JOIN | CTL_CH3_HICLK)) == (CTL_CH34_JOIN | CTL_CH3_HICLK);
    }
}

/* Latch a fired IRQ into IRQST and tell the host, if either is wired up
 * to hear about it.  IRQST only ever gets bits ORed in here; a write to
 * IRQEN or SKREST is what clears them (see ad_pokey_write()). */
static void fire_irq(ad_pokey *p, uint8_t mask)
{
    p->IRQST |= mask;
    if (p->host && p->host->raise_irq)
        p->host->raise_irq(p->host->ctx, mask);
}

/* The serial port and SKSTAT, defined in their own section below. */
static bool    sdo_idle(const ad_pokey *p);
static uint8_t skstat_read(const ad_pokey *p);
static void    serial_step(ad_pokey *p, const uint32_t *borrows);

/* Restart timer w's countdown a full period (the driving channel's
 * current divisor) from now.  Called only from the writes that AAE's
 * rearm_timers() covers - AUDF/AUDCTL/STIMER - never from an AUDC or
 * IRQEN write, and never from ad_pokey_advance() itself: a borrow
 * re-arms in place (see the comment there), it does not call this. */
static void rearm_timer(ad_pokey *p, int w)
{
    p->tcnt[w] = p->divisor[timer_channel(w)];
}

static void update_render_channel(ad_pokey *p, int ch)
{
    if (ch < 0 || ch >= 4)
        return;

    /* update_channel_freq(): set Div_n_max(render); clamp Div_n_cnt down */
    uint32_t new_val = channel_period(p->AUDF, p->AUDCTL, p->base_mult, ch);
    if (new_val != p->rmax[ch]) {
        p->rmax[ch] = new_val;
        if (p->cnt[ch] > new_val)
            p->cnt[ch] = new_val;
    }

    /* Outvol seeding + disable (freeze) */
    uint8_t audc = p->AUDC[ch];
    uint32_t samp_thresh = p->samp_max >> 8;
    if ((audc & AUDC_VOLONLY) || !(audc & AUDC_VOLMASK) || (p->rmax[ch] < samp_thresh)) {
        p->out[ch] = 1;   /* Outvol = 1 (participates in the initial DC sum) */

        bool disable =
            (ch == 2 && !(p->AUDCTL & CTL_CH1_FILTER)) ||
            (ch == 3 && !(p->AUDCTL & CTL_CH2_FILTER)) ||
            (ch == 0 || ch == 1) ||
            (p->rmax[ch] < samp_thresh);
        if (disable) {
            p->rmax[ch] = p->cnt[ch] = 0x7FFFFFFF;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ad_pokey_reset(ad_pokey *p)
{
    for (int i = 0; i < 4; ++i) {
        p->AUDF[i] = p->AUDC[i] = 0;
        p->divisor[i] = p->base_clock;   /* sane before any write */
        p->rmax[i] = 0x7FFFFFFF;
        p->cnt[i] = 0;
        p->out[i] = 0;
        p->vol[i] = 0;
    }
    p->AUDCTL = 0;
    p->base_mult = DIV_64;
    p->p4 = p->p5 = p->p9 = p->p17 = p->poly_adjust = 0;
    p->samp_cnt = 0;
    p->samp_max = p->sys_freq ? ((p->base_clock << 8) / p->sys_freq) : 0;
    p->SKCTL = 0;
    p->pot_scanning = false;
    p->pot_scan_ever = false;
    p->pot_scan_start = 0;
    p->rng_enabled = 0;
    chain_reset(&p->rng);
    /* Each timer starts a full period away; divisor[i] was just seeded
     * to base_clock above (the same "sane before any write" fallback
     * the divisors themselves use). */
    for (int i = 0; i < 3; ++i)
        p->tcnt[i] = p->divisor[timer_channel(i)];
    p->IRQEN = p->IRQST = 0;
    p->KBCODE = 0;
    p->kb_down = p->kb_shift = false;
    p->SERIN = p->SEROUT = 0;
    p->sdo_pending = p->sdo_busy = false;
    p->sdo_byte = 0; p->sdo_left = 0;
    p->sdi_busy = false;
    p->sdi_byte = 0; p->sdi_left = 0;
    p->st_latch = 0;
    /* p->cycles, p->allpot and p->host are this port's stand-ins for
     * host-owned state (the machine clock, the DIP bank, the callback
     * wiring) - a chip reset doesn't touch any of them, same as
     * Pokey::reset() never touches its host. */
}

void ad_pokey_init(ad_pokey *p, uint32_t clock_hz, uint32_t sample_rate)
{
    memset(p, 0, sizeof *p);
    p->base_clock = clock_hz ? clock_hz : 1;
    p->sys_freq = sample_rate ? sample_rate : 1;
    build_tables();
    ad_pokey_reset(p);
}

void ad_pokey_set_allpot(ad_pokey *p, uint8_t v)
{
    p->allpot = v;
}

void ad_pokey_set_host(ad_pokey *p, const ad_pokey_host *h)
{
    p->host = h;
}

/* Machine time: n POKEY clocks at once.  The RANDOM chain clocks every
 * cycle, held or not (what it takes in differs); the three hardware
 * timers count while the chip runs, and while held only on the fast
 * clock (timer_runs()).  See pokey.h's time-model note for who calls
 * this and with what. */
void ad_pokey_advance(ad_pokey *p, uint32_t cycles)
{
    p->cycles += cycles;
    build_tables();
    chain_advance(&p->rng, cycles, !p->rng_enabled, (p->AUDCTL & CTL_POLY9) != 0);

    /* Each timer counts down tcnt[w] POKEY cycles to a borrow.  If this
     * slice reaches or passes it, the borrow fired - one or more times,
     * if the slice spans several periods, but IRQST is a latch so only
     * one fire_irq() is needed per slice regardless of how many.  n is
     * how many additional periods it takes for tcnt[w] to land past
     * cycles; adding n periods first and then subtracting the slice
     * leaves tcnt[w] holding the correct remaining distance to the
     * *next* borrow, with phase intact (see pokey.h's timer paragraph).
     * channel_period() (behind divisor[]) never returns 0, so this never
     * divides by zero. */
    uint32_t borrows[3] = { 0, 0, 0 };
    for (int w = 0; w < 3; ++w) {
        if (!timer_runs(p, w))
            continue;
        uint32_t divisor = p->divisor[timer_channel(w)];
        if (cycles >= p->tcnt[w]) {
            uint32_t n = (cycles - p->tcnt[w]) / divisor + 1;
            p->tcnt[w] += n * divisor;
            borrows[w] = n;
            uint8_t bit = timer_irq_bit(w);
            if (p->IRQEN & bit)
                fire_irq(p, bit);
        }
        p->tcnt[w] -= cycles;
    }
    serial_step(p, borrows);
}

/* ------------------------------------------------------------------ */
/* Writes                                                              */
/* ------------------------------------------------------------------ */

void ad_pokey_write(ad_pokey *p, uint8_t reg, uint8_t v)
{
    const uint8_t a = reg & 0x0F;
    switch (a) {
    /* AUDF writes rearm only the timers whose period they can change:
     * AUDF1 -> TIMR1 (+TIMR2 when ch1+2 joined), AUDF2 -> TIMR2, AUDF3 ->
     * TIMR4 only when ch3+4 joined, AUDF4 -> TIMR4.  AUDC writes never
     * touch a divisor, so they must not reset a timer's phase. */
    case W_AUDF1:
        p->AUDF[0] = v;
        recompute_channel(p, 0); update_render_channel(p, 0);
        if (p->AUDCTL & CTL_CH12_JOIN) { recompute_channel(p, 1); update_render_channel(p, 1); }
        rearm_timer(p, 0);
        if (p->AUDCTL & CTL_CH12_JOIN) rearm_timer(p, 1);
        break;
    case W_AUDF2:
        p->AUDF[1] = v; recompute_channel(p, 1); update_render_channel(p, 1);
        rearm_timer(p, 1); break;
    case W_AUDF3:
        p->AUDF[2] = v;
        recompute_channel(p, 2); update_render_channel(p, 2);
        if (p->AUDCTL & CTL_CH34_JOIN) {
            recompute_channel(p, 3); update_render_channel(p, 3);
            rearm_timer(p, 2);
        }
        break;
    case W_AUDF4:
        p->AUDF[3] = v; recompute_channel(p, 3); update_render_channel(p, 3);
        rearm_timer(p, 2); break;

    case W_AUDC1:
        p->AUDC[0] = v; p->vol[0] = (v & AUDC_VOLMASK) * POKEY_GAIN;
        recompute_channel(p, 0); update_render_channel(p, 0); break;
    case W_AUDC2:
        p->AUDC[1] = v; p->vol[1] = (v & AUDC_VOLMASK) * POKEY_GAIN;
        recompute_channel(p, 1); update_render_channel(p, 1); break;
    case W_AUDC3:
        p->AUDC[2] = v; p->vol[2] = (v & AUDC_VOLMASK) * POKEY_GAIN;
        recompute_channel(p, 2); update_render_channel(p, 2); break;
    case W_AUDC4:
        p->AUDC[3] = v; p->vol[3] = (v & AUDC_VOLMASK) * POKEY_GAIN;
        recompute_channel(p, 3); update_render_channel(p, 3); break;

    case W_AUDCTL:
        p->AUDCTL = v;
        p->base_mult = (v & CTL_CLK15) ? DIV_15 : DIV_64;
        recompute_all(p);
        for (int i = 0; i < 4; ++i)
            update_render_channel(p, i);
        rearm_timer(p, 0); rearm_timer(p, 1); rearm_timer(p, 2);
        break;

    case W_STIMER:
        for (int i = 0; i < 4; ++i) { p->cnt[i] = 0; p->out[i] = 0; }
        rearm_timer(p, 0); rearm_timer(p, 1); rearm_timer(p, 2);
        break;

    case W_SKCTL:
        /* A rewrite of the current value is a no-op.  The init bits
         * only change what the RANDOM chain takes in from the next
         * clock on (see chain_advance()): nothing is cleared here, the
         * clocks that follow do the clearing, and a release changes
         * nothing but the feed.  The render's poly phases (p4..p17,
         * plus the ticks pending in poly_adjust) restart from the seed
         * on entering reset, and ad_pokey_render() holds still until
         * release.  The hardware timers keep their phase either way -
         * only AUDF/AUDCTL/STIMER writes rearm them - and the slow-clock
         * ones stand still while held (see timer_runs()). */
        if (v == p->SKCTL)
            break;
        p->SKCTL = v;
        p->rng_enabled = (v & SK_INIT) != 0;
        if (!p->rng_enabled) {
            p->p4 = p->p5 = p->p9 = p->p17 = p->poly_adjust = 0;
            /* Init also resets both serial state machines (SER_core.v's
             * istate/ostate): a frame in flight is abandoned and the
             * data register counts as taken. */
            p->sdi_busy = false;
            p->sdo_busy = false;
            p->sdo_pending = false;
        }
        break;

    case W_POTGO:
        p->pot_scanning = true;
        p->pot_scan_ever = true;
        p->pot_scan_start = p->cycles;
        break;

    case W_SEROUT:
        /* Into the output data register; the shifter takes it at its
         * next bit clock (serial_step()).  A write over a byte the
         * shifter has not taken yet replaces it, as on the chip. */
        p->SEROUT = v;
        p->sdo_pending = true;
        break;

    case W_IRQEN: {
        /* Clear any pending IRQST bits being disabled, then set the
         * mask.  No timer scheduling here - the countdowns in
         * ad_pokey_advance() run unconditionally; IRQEN only gates
         * whether a borrow reaches fire_irq().  Bit 3 is a level, not a
         * latch: enabling it while the transmitter is idle asserts the
         * IRQ at once. */
        const uint8_t was = p->IRQEN;
        if (p->IRQST & (uint8_t)~v)
            p->IRQST &= v;
        p->IRQEN = v;
        if ((v & ~was & IRQ_SEROC) && sdo_idle(p) && p->host && p->host->raise_irq)
            p->host->raise_irq(p->host->ctx, IRQ_SEROC);
        break;
    }

    case W_SKREST:
        p->st_latch = 0;
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Reads                                                               */
/* ------------------------------------------------------------------ */

/* RANDOM: the complement of the chain's 9-bit register (poly_core.v's
 * rndNum = ~lfsr9bit), which only ad_pokey_advance() moves (see
 * pokey.h's time-model note).  The read charges nothing, so a caller
 * that knows the 6502 cycle distance to its previous read advances by
 * exactly that first, and back-to-back reads with no machine time
 * between them return the same byte.  Whichever poly AUDCTL selects,
 * the byte comes from the same eight flops; the select only changes
 * what feeds them.  A chip held in reset reads 0xFF once eight clocks
 * of zeros have shifted in, and before that the tail of what it was
 * doing. */
static uint8_t read_random(const ad_pokey *p)
{
    return (uint8_t)~p->rng.l9;
}

/* ALLPOT.  POKEY pot scanner, time-based like the real chip: each
 * ALLPOT bit is 1 while that pot line's counter is still running and 0
 * once its POT register has latched.  This board straps the coin DIP
 * (OPTN5) to the pot pins as digital levels - see astdelux2_main.asm's
 * NMI (SKCTL=7/POTGO=7 every 4 ms) and frame.c's PKYTST, which reads
 * this register expecting 0 well after the scan has had time to
 * finish ("S/B 0").  Boards that never issue POTGO get the DIP value
 * back on every read (pot_scan_ever false): a plain digital input.
 *
 * Per the Altirra Hardware Reference (5.9, measured on the chip): the
 * pot scan logic is not affected by SKCTL init mode beyond the slow
 * clock stopping; power-up starts an abnormal scan that the first POTGO
 * clears; ALLPOT is updated every cycle from the input comparators
 * (1 while an input is below threshold, 0 once it has tripped) and is
 * forced to 0 once the scan has finished, whatever the inputs do.  So
 * a chip held in reset from power-up, slow clock frozen and that scan
 * stuck mid-way, answers the grounded-line mask - the coin DIP byte.
 * That is what this model returns before any POTGO (`allpot`), and why
 * INIT at power-up sees the DIP byte, not 0. */
static uint8_t allpot_read(ad_pokey *p)
{
    if (p->pot_scanning) {
        const uint64_t elapsed = p->cycles - p->pot_scan_start;
        /* The scan ends at the counter's terminal count: 228 in slow
         * mode, one count per 114-cycle scan line; 229 in fast mode, one
         * count per machine cycle (the counter stops one value higher in
         * fast mode - Altirra HRM 5.9).  cpu_hz == pokey_hz in this port,
         * so counts are cycles directly (pokey.h's time-model note). */
        const uint64_t need = (p->SKCTL & SK_FASTPOT) ? 229u : (228u * DIV_15);
        if (elapsed >= need)
            p->pot_scanning = false;
    }
    /* ALLPOT is not a latch during a scan: it follows the pins live, so
     * a line that trips reads 0 and one that drops back below threshold
     * reads 1 again (POT0-7 themselves are not modelled; nothing here
     * reads them).  The host keeps `allpot` at the pins' current
     * grounded-line mask with ad_pokey_set_allpot().  Only the finished
     * scan is a latch, forced to 0 until the next POTGO. */
    if (!p->pot_scan_ever)
        return p->allpot;      /* no POTGO issued yet: the comparators' mask */
    if (!p->pot_scanning)
        return 0x00;           /* scan complete: forced to 0 */
    return p->allpot;          /* mid-scan: the comparators' mask, live */
}

uint8_t ad_pokey_read(ad_pokey *p, uint8_t reg)
{
    const uint8_t a = reg & 0x0F;
    switch (a) {
    case R_RANDOM: return read_random(p);
    case R_ALLPOT: return allpot_read(p);
    case R_IRQST: {
        /* Pending IRQs read as 0.  Bit 3 is not a latch: it is low
         * whenever the transmitter is idle, whatever IRQEN says. */
        uint8_t v = (uint8_t)(p->IRQST ^ 0xFF);
        if (sdo_idle(p)) v &= (uint8_t)~IRQ_SEROC; else v |= IRQ_SEROC;
        return v;
    }
    case R_SKSTAT: return skstat_read(p);
    case R_KBCODE: return p->KBCODE;
    case R_SERIN:  return p->SERIN;
    default:
        /* POT0-7 share offsets 0x00-0x07 with AUDF1-4/AUDC1-4's write
         * side; on the read side they are the only registers there. */
        if (a <= R_POT0 + 7) {
            if (p->host && p->host->pot_read)
                return (uint8_t)p->host->pot_read(p->host->ctx, a);
            return 0xFF;   /* AAE's default with no pot handler wired up */
        }
        return 0xFF;
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */
/* A key event from the host, standing in for the chip's matrix scan
 * (KEY_core.v): the scanner only runs with SKCTL's scan-enable bit set.
 * A new code latches into KBCODE and raises the keyboard IRQ; if that
 * IRQ was still pending from the previous code, the keyboard overrun
 * latch sets (IRQ_core.v: keyOvrun = setKey & the pending latch) -
 * nothing on the chip knows whether KBCODE was read, only whether its
 * IRQ was cleared.  SKSTAT's key-down and shift-key bits follow the
 * matrix live, so they come from every call, up or down. */
void ad_pokey_keyboard_key(ad_pokey *p, uint8_t code, uint8_t flags, bool down)
{
    if ((p->SKCTL & SK_KEYSCAN) == 0)
        return;
    p->kb_shift = (flags & ST_SHIFT) != 0;
    if (!down) {
        p->kb_down = false;
        return;
    }
    if (p->IRQST & IRQ_KEYBD)
        p->st_latch |= ST_KBERR;
    p->KBCODE = code;
    p->kb_down = true;
    if (p->IRQEN & IRQ_KEYBD)
        fire_irq(p, IRQ_KEYBD);
}

/* ------------------------------------------------------------------ */
/* Serial port                                                         */
/* ------------------------------------------------------------------ */
/* SER_core.v, a byte at a time.  Each direction is a ten-stage shift
 * register (start bit, eight data bits LSB first, stop bit) clocked by
 * a flop that toggles on a timer's borrow, so a bit is two borrows and
 * a frame twenty.  SKCTL bits 4..6 pick the timers: the receiver clocks
 * from timer 4 unless bits 5 and 4 are both clear (the external bit
 * clock), the transmitter from timer 2 with bits 6 and 5 set, timer 4
 * with either alone, the external clock with both clear.  No host here
 * has an external clock, so a direction on it stands still.
 *
 * Transmit: the shifter takes the data register at its next bit clock
 * and that load is the "output data needed" event (IRQ bit 4); twenty
 * borrows later the stop bit has left the pin and the byte goes to the
 * host.  A second SEROUT during a frame waits in the data register and
 * loads straight after, so a stream is gapless.  "Transmission
 * finished" (IRQ bit 3) is the idle level, see sdo_idle().
 *
 * Receive: a start bit is offered whenever the receiver is idle - the
 * host's serial_in() from ad_pokey_advance(), or ad_pokey_serial_
 * receive() directly.  In asynchronous mode (SKCTL bit 4) it resyncs
 * timers 3 and 4, then twenty borrows later the stop bit is sampled:
 * SERIN takes the byte, the input IRQ (bit 5) fires, and if that IRQ
 * was still pending from the previous byte the overrun latch sets
 * (IRQ_core.v: sdiOvrun = setSdiCompl & the pending latch).  SKSTAT's
 * busy bit is low from the start bit to the stop bit, and its serial-
 * data bit shows the level on the line.  A frame error needs a zero
 * stop bit, which a byte interface cannot deliver, so that latch never
 * sets here.  Not modelled either: the break bit, two-tone output. */

enum { SER_FRAME_BORROWS = 20 };

static int sdi_timer(const ad_pokey *p)
{
    return (p->SKCTL & 0x30) ? 2 : -1;
}

static int sdo_timer(const ad_pokey *p)
{
    switch (p->SKCTL & 0x60) {
    case 0x00: return -1;
    case 0x60: return 1;
    default:   return 2;
    }
}

static bool sdo_idle(const ad_pokey *p)
{
    return !p->sdo_busy && !p->sdo_pending;
}

/* The level on the serial input line: mark when idle, else the bit of
 * the arriving frame that is on the wire now. */
static bool sdi_line(const ad_pokey *p)
{
    if (!p->sdi_busy)
        return true;
    uint32_t bit = (SER_FRAME_BORROWS - p->sdi_left) / 2;
    if (bit == 0) return false;                      /* start bit */
    if (bit >= 9) return true;                       /* stop bit */
    return ((p->sdi_byte >> (bit - 1)) & 1u) != 0;
}

static uint8_t skstat_read(const ad_pokey *p)
{
    uint8_t v = ST_ALWAYS_ONE;
    if (!(p->st_latch & ST_FRAME))   v |= ST_FRAME;
    if (!(p->st_latch & ST_OVERRUN)) v |= ST_OVERRUN;
    if (!(p->st_latch & ST_KBERR))   v |= ST_KBERR;
    if (sdi_line(p))                 v |= ST_SERIN_DATA;
    if (!p->kb_shift)                v |= ST_SHIFT;
    if (!p->kb_down)                 v |= ST_KEYBD;
    if (!p->sdi_busy)                v |= ST_SERIN_BUSY;
    return v;
}

static void sdi_start(ad_pokey *p, uint8_t data)
{
    p->sdi_busy = true;
    p->sdi_byte = data;
    p->sdi_left = SER_FRAME_BORROWS;
    if (p->SKCTL & SK_ASYNC)
        rearm_timer(p, 2);
}

static void sdi_complete(ad_pokey *p)
{
    p->sdi_busy = false;
    p->SERIN = p->sdi_byte;
    if (p->IRQST & IRQ_SERIN)
        p->st_latch |= ST_OVERRUN;
    if (p->IRQEN & IRQ_SERIN)
        fire_irq(p, IRQ_SERIN);
}

/* One slice of serial time: borrows[w] is how many times timer w
 * borrowed in the slice ad_pokey_advance() just walked. */
static void serial_step(ad_pokey *p, const uint32_t *borrows)
{
    int ti = sdi_timer(p);
    if (ti >= 0) {
        if (!p->sdi_busy && p->host && p->host->serial_in) {
            int b = p->host->serial_in(p->host->ctx);
            if (b >= 0)
                sdi_start(p, (uint8_t)b);
        }
        if (p->sdi_busy) {
            if (borrows[ti] >= p->sdi_left)
                sdi_complete(p);
            else
                p->sdi_left -= borrows[ti];
        }
    }

    int to = sdo_timer(p);
    if (to >= 0) {
        uint32_t b = borrows[to];
        while (b) {
            if (!p->sdo_busy) {
                if (!p->sdo_pending)
                    break;
                p->sdo_byte = p->SEROUT;                 /* the load, one borrow */
                p->sdo_pending = false;
                p->sdo_busy = true;
                p->sdo_left = SER_FRAME_BORROWS;
                --b;
                if (p->IRQEN & IRQ_SEROR)
                    fire_irq(p, IRQ_SEROR);
                continue;
            }
            uint32_t take = b < p->sdo_left ? b : p->sdo_left;
            p->sdo_left -= take;
            b -= take;
            if (p->sdo_left == 0) {
                p->sdo_busy = false;
                if (p->host && p->host->serial_out)
                    p->host->serial_out(p->host->ctx, p->sdo_byte);
                if (!p->sdo_pending && (p->IRQEN & IRQ_SEROC) && p->host && p->host->raise_irq)
                    p->host->raise_irq(p->host->ctx, IRQ_SEROC);
            }
        }
    }
}

/* A start bit from the host, outside the serial_in() poll.  Dropped if
 * the receiver is busy (a line carries one frame at a time) or has no
 * clock. */
void ad_pokey_serial_receive(ad_pokey *p, uint8_t data)
{
    if (p->sdi_busy || sdi_timer(p) < 0)
        return;
    sdi_start(p, data);
}

/* ------------------------------------------------------------------ */
/* Poll                                                                */
/* ------------------------------------------------------------------ */
/* The host's per-frame poll, standing in for the matrix scan: pulls one
 * keyboard code through host->keyboard_scan() while the scanner is
 * enabled.  (Serial input is not polled here - the receiver asks the
 * host for a start bit itself, from ad_pokey_advance(), whenever it is
 * idle and clocked.) */
void ad_pokey_poll(ad_pokey *p)
{
    if ((p->SKCTL & SK_KEYSCAN) == 0 || !p->host || !p->host->keyboard_scan)
        return;
    uint8_t code = 0, flags = 0;
    if (p->host->keyboard_scan(p->host->ctx, &code, &flags))
        ad_pokey_keyboard_key(p, code, flags, true);
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */
/* Event-driven sound renderer: a clean-room reimplementation of the
 * proven Ron-Fries Pokey_process event loop.  It walks to the nearest
 * of {a channel divider expiry, the next sample boundary}, advances the
 * poly phases, toggles/samples each channel honoring NOTPOLY5 gating,
 * PURE/POLY4/POLY9/POLY17 selection, VOL_ONLY, and the CH1/CH2
 * high-pass filters, then emits one clipped int16 per sample boundary.
 *
 * Fixed-point note: samp_cnt is the sample-phase accumulator in Q8
 * (base-clock ticks << 8); samp_max = (base_clock << 8) / sys_freq.
 * The whole-ticks remaining until the next sample is samp_cnt >> 8.
 * Channel dividers (rmax/cnt) are in whole base-clock ticks.  A frozen
 * channel has rmax = cnt = 0x7FFFFFFF and never becomes the event
 * minimum, so it never toggles - it only contributes its (seeded)
 * Outvol to the running sum. */

/* suppress(): the high-pass-filter lambda from aae_pokey.cpp, as a
 * static function - a ch3 transition clocks (latches) ch1; ch4 clocks
 * ch2. */
static void suppress(ad_pokey *p, int32_t *cur, int next, uint8_t filt, int trig, int tgt)
{
    if ((p->AUDCTL & filt) && next == trig && p->out[tgt]) {
        p->out[tgt] = 0;
        *cur -= p->vol[tgt];
    }
}

void ad_pokey_render(ad_pokey *p, int16_t *dst, int n)
{
    if (!dst || n <= 0)
        return;
    build_tables();
    const uint8_t *poly4 = g_poly4;
    const uint8_t *poly5 = g_poly5;
    const uint8_t *poly9 = g_poly9;
    const uint8_t *poly17 = g_poly17;

    if (p->samp_max == 0)
        p->samp_max = p->sys_freq ? ((p->base_clock << 8) / p->sys_freq) : 1;

    /* Initial output summation: each channel contributes -AUDV/2, plus
     * +AUDV if its output latch (Outvol) is currently high. */
    int32_t cur = 0;
    for (int c = 0; c < 4; ++c) {
        cur -= p->vol[c] / 2;
        if (p->out[c])
            cur += p->vol[c];
    }

    /* SKCTL reset (init bits clear) holds the chip: neither the
     * polynomial counters nor the channel dividers clock, so every
     * output sits at its current level and only the sample clock keeps
     * running - no channel event can fire, no countdown moves, no poly
     * phase advances. */
    const bool held = !p->rng_enabled;

    enum { SAMPLE_EVENT = 127 };
    int produced = 0;
    while (produced < n) {
        int next = SAMPLE_EVENT;
        uint32_t event_min = p->samp_cnt >> 8;   /* whole ticks until next sample */

        /* Nearest channel-divider expiry; ties (<=) resolve to the channel. */
        if (!held) {
            for (int c = 0; c < 4; ++c) {
                if (p->cnt[c] <= event_min) { event_min = p->cnt[c]; next = c; }
            }
            for (int c = 0; c < 4; ++c)
                p->cnt[c] -= event_min;
            p->poly_adjust += event_min;
        }
        p->samp_cnt -= (event_min << 8);

        if (next != SAMPLE_EVENT) {
            /* Advance the poly phases by the elapsed ticks. */
            p->p4 = (p->p4 + p->poly_adjust) % 0x0000F;
            p->p5 = (p->p5 + p->poly_adjust) % 0x0001F;
            p->p9 = (p->p9 + p->poly_adjust) % 0x001FF;
            p->p17 = (p->p17 + p->poly_adjust) % 0x1FFFF;
            p->poly_adjust = 0;

            p->cnt[next] += p->rmax[next];
            const uint8_t audc = p->AUDC[next];
            uint8_t *outp = &p->out[next];
            bool toggle = false;
            if (!(audc & AUDC_VOLONLY)) {
                if ((audc & AUDC_NOTPOLY5) || poly5[p->p5]) {
                    if (audc & AUDC_PURE)         toggle = true;
                    else if (audc & AUDC_POLY4)   toggle = (poly4[p->p4] == !(*outp));
                    else if (p->AUDCTL & CTL_POLY9) toggle = (poly9[p->p9] == !(*outp));
                    else                           toggle = (poly17[p->p17] == !(*outp));
                }
            }

            suppress(p, &cur, next, CTL_CH1_FILTER, 2, 0);
            suppress(p, &cur, next, CTL_CH2_FILTER, 3, 1);

            if (toggle) {
                if (*outp) { cur -= p->vol[next]; *outp = 0; }
                else       { cur += p->vol[next]; *outp = 1; }
            }
        } else {
            p->samp_cnt += p->samp_max;
            int32_t v = cur;
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            dst[produced++] = (int16_t)v;
        }
    }
}
