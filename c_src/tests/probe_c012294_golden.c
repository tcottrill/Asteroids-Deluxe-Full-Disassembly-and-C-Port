/* probe_c012294_golden.c - A/B behaviour hash for c012294.c.
 *
 * Drives one chip through scripted scenarios that cover the audio paths
 * (cycle and legacy), every AUDCTL clock/join/filter/poly bit, STIMER,
 * SKCTL init and two-tone/async/serial modes, timer IRQs, the serial
 * port in both directions, the pot scanner, the keyboard and RANDOM, and
 * hashes every sample, every register read and every host callback with
 * its cycle stamp.  Build it once against a saved copy of the core and
 * once against the working copy (tests\build_c012294_ab.bat); identical
 * output means identical behaviour on everything exercised here. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "c012294.h"

static uint32_t h = 2166136261u;
static void hb(uint32_t v) { h = (h ^ (v & 0xFF)) * 16777619u; h = (h ^ ((v >> 8) & 0xFF)) * 16777619u;
                             h = (h ^ ((v >> 16) & 0xFF)) * 16777619u; h = (h ^ (v >> 24)) * 16777619u; }
static void h64(uint64_t v) { hb((uint32_t)v); hb((uint32_t)(v >> 32)); }

static uint32_t lcg = 12345;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static ad_pokey chip;
static unsigned events;

static void cb_irq(void *ctx, uint8_t mask) { (void)ctx; hb(0x1000000u | mask); h64(chip.cycles); events++; }
static int  cb_pot(void *ctx, int n) { (void)ctx; return 20 + n * 25; }
static int  cb_kbd(void *ctx, uint8_t *code, uint8_t *flags) { (void)ctx; (void)code; (void)flags; return 0; }
static int  serin_left;
static int  cb_serin(void *ctx) { (void)ctx; if (serin_left > 0) { serin_left--; return 0x5A + serin_left; } return -1; }
static void cb_serout(void *ctx, uint8_t d) { (void)ctx; hb(0x2000000u | d); h64(chip.cycles); events++; }
static void cb_irq_line(void *ctx, int a, uint64_t c) { (void)ctx; hb(0x3000000u | (unsigned)a); h64(c); events++; }
static void cb_ser_line(void *ctx, int m, uint64_t c) { (void)ctx; hb(0x4000000u | (unsigned)m); h64(c); events++; }
static void cb_clk_out(void *ctx, int l, uint64_t c) { (void)ctx; hb(0x5000000u | (unsigned)l); h64(c); events++; }
static void cb_clk_bi(void *ctx, int l, int d, uint64_t c) { (void)ctx; hb(0x6000000u | (unsigned)l | ((unsigned)d << 4)); h64(c); events++; }

static ad_pokey_host host = { NULL, cb_irq, cb_pot, cb_kbd, cb_serin, cb_serout };
static ad_pokey_io io = { NULL, cb_irq_line, cb_ser_line };
static ad_pokey_clocks clocks = { NULL, cb_clk_out, cb_clk_bi };

static int cycle_mode;
static unsigned samples;

/* Advance in a jagged batch pattern and drain/render audio as a host would. */
static void run(uint32_t cycles)
{
    static int16_t pcm[4096];
    while (cycles) {
        uint32_t step = 1 + (rnd() % 97);
        if (step > cycles) step = cycles;
        ad_pokey_advance(&chip, step);
        cycles -= step;
        if (cycle_mode) {
            int n = ad_pokey_audio_read(&chip, pcm, 4096);
            for (int i = 0; i < n; ++i) hb((uint16_t)pcm[i]);
            samples += (unsigned)n;
        } else if ((rnd() & 3) == 0) {
            int n = 1 + (int)(rnd() % 40);
            ad_pokey_render(&chip, pcm, n);
            for (int i = 0; i < n; ++i) hb((uint16_t)pcm[i]);
            samples += (unsigned)n;
        }
    }
}

static void rd(uint8_t reg) { hb(0x7000000u | ((unsigned)reg << 8) | ad_pokey_read(&chip, reg)); }
static void wr(uint8_t reg, uint8_t v) { ad_pokey_write(&chip, reg, v); }
static void snapshot(void)
{
    for (uint8_t r = 0; r < 16; ++r) rd(r);
    hb(chip.IRQST); hb(chip.IRQEN); hb(chip.rng.l9 | (chip.rng.l17 << 8));
    for (int i = 0; i < 4; ++i) { hb(chip.tcnt[i]); hb(chip.out[i]); }
}

static void fresh(int cyc)
{
    cycle_mode = cyc;
    ad_pokey_init(&chip, 1512000, 44100);
    ad_pokey_set_host(&chip, &host);
    ad_pokey_set_io(&chip, &io);
    ad_pokey_set_clocks(&chip, &clocks);
    ad_pokey_set_allpot(&chip, 0xA5);
    ad_pokey_set_cycle_audio(&chip, cyc != 0);
}

static void report(const char *name)
{
    printf("%-28s hash %08X  samples %u  events %u\n", name, h, samples, events);
}

/* 1. The Space Duel shield: fixed AUDF, C0/C2/C4/C6 per 6144-cycle tick,
 *    RANDOM reads charged in 64-cycle steps between ticks. */
static void scen_shield(int cyc)
{
    fresh(cyc);
    wr(W_SKCTL, 0); wr(W_SKCTL, 7);
    wr(W_AUDF1, 0xB0);
    static const uint8_t vols[4] = { 0xC0, 0xC2, 0xC4, 0xC6 };
    for (int tick = 0; tick < 600; ++tick) {
        uint32_t spent = 0;
        int reads = (int)(rnd() % 9);
        for (int i = 0; i < reads; ++i) { ad_pokey_advance(&chip, 64); spent += 64; rd(R_RANDOM); }
        run(6144 - spent);
        wr(W_AUDC1, vols[tick & 3]);
        if ((tick & 3) == 0) wr(W_AUDF1, 0xB0);
        if (tick == 300) { wr(W_AUDF3, 0); wr(W_AUDC3, 0); }
    }
    snapshot();
}

/* 2. Every AUDCTL mode with mixed distortions and both timer IRQs live. */
static void scen_audctl(int cyc)
{
    static const uint8_t ctls[] = { 0x00, 0x01, 0x40, 0x20, 0x60, 0x50, 0x28, 0x78,
                                    0x04, 0x02, 0x06, 0x80, 0x81, 0xC4, 0xAA, 0x7F };
    static const uint8_t dists[] = { 0xA0, 0xC4, 0x84, 0x24, 0x10, 0x64, 0x08, 0x44, 0xE7 };
    fresh(cyc);
    wr(W_SKCTL, 3);
    wr(W_IRQEN, 0x07);
    for (unsigned c = 0; c < sizeof ctls; ++c) {
        wr(W_AUDCTL, ctls[c]);
        for (int ch = 0; ch < 4; ++ch) {
            wr((uint8_t)(ch * 2), (uint8_t)(rnd() & 0xFF));
            wr((uint8_t)(ch * 2 + 1), dists[rnd() % (sizeof dists)]);
        }
        run(9000);
        rd(R_IRQST); wr(W_IRQEN, 0x07);          /* ack, keep enabled */
        wr((uint8_t)((rnd() & 3) * 2), (uint8_t)(rnd() & 0x1F));   /* small period mid-run */
        run(7000);
        rd(R_IRQST); rd(R_RANDOM);
        if (c & 1) wr(W_STIMER, 0);
        run(5000);
        rd(R_IRQST); wr(W_IRQEN, 0x00); rd(R_IRQST); wr(W_IRQEN, 0x07);
    }
    snapshot();
}

/* 3. SKCTL init in and out mid-run, held-chip reads, poly restarts. */
static void scen_init(int cyc)
{
    fresh(cyc);
    for (int i = 0; i < 4; ++i) { wr((uint8_t)(i * 2), (uint8_t)(3 + i * 37)); wr((uint8_t)(i * 2 + 1), 0xA8); }
    wr(W_IRQEN, 0x07);
    for (int rep = 0; rep < 12; ++rep) {
        wr(W_AUDCTL, (uint8_t)((rep & 1) ? 0x41 : 0x00));
        wr(W_SKCTL, (uint8_t)((rep & 2) ? 0x03 : 0x07));
        run(3000 + rep * 517);
        for (int k = 0; k < 6; ++k) { ad_pokey_advance(&chip, 1); rd(R_RANDOM); }
        wr(W_SKCTL, 0x00);
        run(1 + (rep * 5) % 23);           /* release inside the 17-clock drain */
        for (int k = 0; k < 20; ++k) { ad_pokey_advance(&chip, 1); rd(R_RANDOM); rd(R_IRQST); }
        wr(W_SKCTL, 0x07);
        run(4000);
        rd(R_IRQST); wr(W_IRQEN, 0x07);
    }
    snapshot();
}

/* 4. Two-tone, break, async receive, internal serial both ways, external
 *    clock edges, direct line driving, the keyboard, SKREST. */
static void scen_serial(int cyc)
{
    fresh(cyc);
    wr(W_SKCTL, 0x03);
    wr(W_AUDF1, 0x08); wr(W_AUDF2, 0x0C); wr(W_AUDF3, 0x20); wr(W_AUDF4, 0x05);
    wr(W_AUDCTL, 0x28);
    wr(W_AUDC1, 0xA6); wr(W_AUDC2, 0xA4); wr(W_AUDC4, 0xA2);
    wr(W_IRQEN, 0x3F);
    wr(W_SKCTL, 0x0B);                     /* two-tone */
    wr(W_SEROUT, 0x3C); run(20000); wr(W_SEROUT, 0xC3); run(30000);
    wr(W_SKCTL, 0x8B); run(3000);          /* force break in two-tone */
    rd(R_IRQST); rd(R_SKSTAT); wr(W_IRQEN, 0x3F);
    wr(W_SKCTL, 0x73);                     /* timer 2 out, timer 4 in, 15k... */
    serin_left = 3;
    wr(W_SEROUT, 0x55); run(40000);
    rd(R_SERIN); rd(R_SKSTAT); rd(R_IRQST); wr(W_SKREST, 0); rd(R_SKSTAT);
    wr(W_SKCTL, 0x13); run(500);           /* async receive, timers 3+4 held */
    ad_pokey_serial_receive(&chip, 0xA7); run(30000);
    rd(R_SERIN); rd(R_SKSTAT); rd(R_IRQST); wr(W_IRQEN, 0x3F);
    wr(W_SKCTL, 0x07);                     /* external clock */
    wr(W_SEROUT, 0x96);
    for (int i = 0; i < 60; ++i) { run(50); ad_pokey_serial_clock(&chip, i & 1); }
    rd(R_IRQST); rd(R_SKSTAT);
    ad_pokey_serial_line(&chip, 0); run(400); ad_pokey_serial_line(&chip, 1);
    for (int i = 0; i < 40; ++i) { run(37); ad_pokey_serial_clock(&chip, i & 1); rd(R_SKSTAT); }
    wr(W_SKCTL, 0x02);                     /* keyboard scan enabled */
    wr(W_IRQEN, 0xC0);
    ad_pokey_keyboard_key(&chip, 0x21, 0, true); run(100); rd(R_KBCODE); rd(R_SKSTAT);
    ad_pokey_keyboard_key(&chip, 0x22, ST_SHIFT, true); rd(R_SKSTAT); rd(R_IRQST);
    ad_pokey_poll(&chip);
    ad_pokey_keyboard_key(&chip, 0x22, 0, false); rd(R_SKSTAT);
    wr(W_SKREST, 0); rd(R_SKSTAT);
    run(20000);
    snapshot();
}

/* 5. Pots: legacy digital scan and the counter model, slow and fast. */
static void scen_pots(int cyc)
{
    for (int counter = 0; counter < 2; ++counter) {
        fresh(cyc);
        ad_pokey_set_pot_scan(&chip, counter != 0);
        wr(W_SKCTL, 0x03);
        rd(R_ALLPOT);
        wr(W_POTGO, 0); rd(R_ALLPOT);
        for (int i = 0; i < 300; ++i) { run(100); rd(R_ALLPOT); if (i % 40 == 0) rd((uint8_t)(i / 40)); }
        ad_pokey_set_allpot(&chip, 0x0F);
        run(3000); rd(R_ALLPOT);
        wr(W_SKCTL, 0x07); wr(W_POTGO, 0);
        for (int i = 0; i < 300; ++i) { ad_pokey_advance(&chip, 1); if (i % 7 == 0) rd((uint8_t)(i & 7)); }
        rd(R_ALLPOT);
        wr(W_SKCTL, 0x00); wr(W_POTGO, 0); run(500); rd(R_ALLPOT); rd(R_POT0);
        snapshot();
    }
}

/* 6. Reset semantics: reset mid-run keeps cycles/allpot/host, clears the rest. */
static void scen_reset(int cyc)
{
    fresh(cyc);
    wr(W_SKCTL, 0x07); wr(W_AUDF1, 0x11); wr(W_AUDC1, 0xA8); wr(W_IRQEN, 1);
    run(5000);
    ad_pokey_reset(&chip);
    h64(chip.cycles); rd(R_ALLPOT); rd(R_IRQST); rd(R_RANDOM);
    run(3000);
    wr(W_SKCTL, 0x07); wr(W_AUDF2, 0x02); wr(W_AUDC2, 0xC8); wr(W_AUDCTL, 0x10);
    run(6000);
    ad_pokey_set_cycle_audio(&chip, cyc == 0);   /* flip modes mid-run */
    cycle_mode = cyc == 0;
    run(6000);
    snapshot();
}

int main(void)
{
    for (int cyc = 0; cyc < 2; ++cyc) {
        char name[40];
        const char *tag = cyc ? "cycle" : "legacy";
        lcg = 12345;
#define SCEN(fn) do { h = 2166136261u; samples = 0; events = 0; fn(cyc); \
                      snprintf(name, sizeof name, #fn " (%s)", tag); report(name); } while (0)
        SCEN(scen_shield);
        SCEN(scen_audctl);
        SCEN(scen_init);
        SCEN(scen_serial);
        SCEN(scen_pots);
        SCEN(scen_reset);
#undef SCEN
    }
    return 0;
}
