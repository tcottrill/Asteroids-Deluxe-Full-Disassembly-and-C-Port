/* pokey.c - the POKEY core; see pokey.h for what was kept and dropped,
 * and for the time model ad_pokey_advance()/ad_pokey_read() share.
 *
 * Translated from the AAE emulator's engine-free POKEY core (Pokey,
 * minus every PokeyHost/timer/IRQ/serial/keyboard/pot-count/adapter
 * piece this board never touches).  The poly generators, the RANDOM
 * register and the SKCTL reset semantics follow MAME's pokey.cpp
 * (0.286, src/devices/sound/pokey.cpp) - credit to the MAME team for
 * the LFSR arithmetic and the reset model.  The pot scanner follows the
 * Altirra Hardware Reference (Avery Lee), which measured the real chip.
 * Where a comment below says "as the original" it means the algorithm
 * is copied verbatim; only the C++ class's `this->member` becomes
 * `p->member` and the PokeyHost calls become either a struct field this
 * port keeps locally (`cycles` for now_cpu_cycles(), `allpot` for the
 * DIP byte) or are dropped outright.
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
 * bit13 back into bit7 of the low byte.  One pass over the states writes
 * both slices of each: g_polyN gets the audio toggle bit (`& 1`),
 * g_randN gets the RANDOM-register byte (`& 0xff` for 9-bit,
 * `>> 8 & 0xff` for 17-bit).
 *
 * These are `static`, not `const`, and built at runtime rather than
 * typed in - the one deliberate exception to "no globals besides g and
 * const tables" (CONVENTIONS.md rule 10).  On a microcontroller too
 * small for a 128 KB runtime table, the alternative is either a
 * flash-resident `const` table (computed once, offline, by this same
 * arithmetic) or stepping the LFSR live in read_random()/render()
 * instead of precomputing it - either avoids the RAM cost; this port
 * takes the table because the Windows/headless hosts have RAM to
 * spare. */
static uint8_t g_poly4[15];
static uint8_t g_poly5[31];
static uint8_t g_poly9[511];
static uint8_t g_poly17[131071];
static uint8_t g_rand9[511];
static uint8_t g_rand17[131071];
static bool    g_tables_built = false;

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
 * comment above): g_polyN gets the toggle bit, g_randN gets the RANDOM
 * byte.  Seeded from lfsr = mask (all ones), not 0, which is what makes
 * a chip held in SKCTL reset read 0xFF (see read_random() below). */
static void poly_init_9_17(uint8_t *poly, uint8_t *rnd, int size)
{
    uint32_t mask = (size == 17) ? 0x1FFFFu : 0x1FFu;
    uint32_t lfsr = mask;
    for (uint32_t i = 0; i < mask; ++i) {
        if (size == 17) {
            uint32_t in8 = ((lfsr >> 8) & 1u) ^ ((lfsr >> 13) & 1u);
            uint32_t in  = lfsr & 1u;
            lfsr >>= 1;
            lfsr = (lfsr & 0xFF7Fu) | (in8 << 7);
            lfsr = (in << 16) | lfsr;
            rnd[i] = (uint8_t)((lfsr >> 8) & 0xFFu);
        } else {
            uint32_t in = (lfsr & 1u) ^ ((lfsr >> 5) & 1u);
            lfsr >>= 1;
            lfsr = (in << 8) | lfsr;
            rnd[i] = (uint8_t)(lfsr & 0xFFu);
        }
        poly[i] = (uint8_t)(lfsr & 1u);
    }
}

static void build_tables(void)
{
    if (g_tables_built)
        return;
    poly_init_4_5(g_poly4, 4);
    poly_init_4_5(g_poly5, 5);
    poly_init_9_17(g_poly9, g_rand9, 9);
    poly_init_9_17(g_poly17, g_rand17, 17);
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
    p->rand_pos9 = p->rand_pos17 = 0;
    /* p->cycles and p->allpot are this port's stand-ins for host-owned
     * state (the machine clock, the DIP bank) - a chip reset doesn't
     * touch either, same as Pokey::reset() never touches its host. */
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

/* Machine time: n POKEY clocks at once.  The polynomial counters only
 * move while SKCTL's init bits are set.  See pokey.h's time-model note
 * for who calls this and with what. */
void ad_pokey_advance(ad_pokey *p, uint32_t cycles)
{
    p->cycles += cycles;
    if (p->rng_enabled) {
        p->rand_pos9 = (uint32_t)(((uint64_t)p->rand_pos9 + cycles) % 0x1FFu);
        p->rand_pos17 = (uint32_t)(((uint64_t)p->rand_pos17 + cycles) % 0x1FFFFu);
    }
}

/* ------------------------------------------------------------------ */
/* Writes                                                              */
/* ------------------------------------------------------------------ */

void ad_pokey_write(ad_pokey *p, uint8_t reg, uint8_t v)
{
    const uint8_t a = reg & 0x0F;
    switch (a) {
    case W_AUDF1:
        p->AUDF[0] = v;
        recompute_channel(p, 0); update_render_channel(p, 0);
        if (p->AUDCTL & CTL_CH12_JOIN) { recompute_channel(p, 1); update_render_channel(p, 1); }
        break;
    case W_AUDF2:
        p->AUDF[1] = v; recompute_channel(p, 1); update_render_channel(p, 1); break;
    case W_AUDF3:
        p->AUDF[2] = v;
        recompute_channel(p, 2); update_render_channel(p, 2);
        if (p->AUDCTL & CTL_CH34_JOIN) { recompute_channel(p, 3); update_render_channel(p, 3); }
        break;
    case W_AUDF4:
        p->AUDF[3] = v; recompute_channel(p, 3); update_render_channel(p, 3); break;

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
        break;

    case W_STIMER:
        for (int i = 0; i < 4; ++i) { p->cnt[i] = 0; p->out[i] = 0; }
        break;

    case W_SKCTL:
        /* A rewrite of the current value is a no-op.  Entering reset
         * (init bits clear) zeroes the polynomial positions, which
         * ad_pokey_advance() then holds there until a write sets the
         * init bits again; release changes nothing but the gate, so
         * counting starts from position 0 at that write.  The chip has
         * one set of shift registers, so the render's poly phases
         * (p4..p17, plus the ticks pending in poly_adjust) restart from
         * the seed too, and ad_pokey_render() holds still until
         * release. */
        if (v == p->SKCTL)
            break;
        p->SKCTL = v;
        p->rng_enabled = (v & SK_INIT) != 0;
        if (!p->rng_enabled) {
            p->rand_pos9 = p->rand_pos17 = 0;
            p->p4 = p->p5 = p->p9 = p->p17 = p->poly_adjust = 0;
        }
        break;

    case W_POTGO:
        p->pot_scanning = true;
        p->pot_scan_ever = true;
        p->pot_scan_start = p->cycles;
        break;

    /* W_SKREST, W_SEROUT, W_IRQEN: dropped (SKREST clears IRQ status
     * bits this port never sets; no serial; no IRQ). */
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Reads                                                               */
/* ------------------------------------------------------------------ */

/* RANDOM: the byte at the current polynomial position, uninverted,
 * which only ad_pokey_advance() moves (see pokey.h's time-model note).
 * The read charges nothing, so a caller that knows the 6502 cycle
 * distance to its previous read advances by exactly that first, and
 * back-to-back reads with no machine time between them return the same
 * byte.
 *
 * With SKCTL held in reset the positions sit at 0 and RANDOM reads
 * table entry 0 - not a special case, just where the LFSR sits.  With
 * the all-ones seed, entry 0 of BOTH tables is 0xFF (the first step
 * from all-ones only clears the top bit either generator feeds back
 * in), so a held-reset chip reads 0xFF regardless of AUDCTL's poly9/17
 * select. */
static uint8_t read_random(ad_pokey *p)
{
    build_tables();
    return (p->AUDCTL & CTL_POLY9) ? g_rand9[p->rand_pos9] : g_rand17[p->rand_pos17];
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
    switch (reg & 0x0F) {
    case R_RANDOM: return read_random(p);
    case R_ALLPOT: return allpot_read(p);
    default:       return 0xFF;
    }
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
