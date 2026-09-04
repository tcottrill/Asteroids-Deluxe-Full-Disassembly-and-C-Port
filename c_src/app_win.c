/* app_win.c - the platform-agnostic application loop and the hardware
 * seam for a real host.
 *
 *  - implements the ad_hw_* interface the game calls (astdelux.h) on
 *    top of the platform contract (platform/ad_platform.h): switches
 *    from plat_inputs, the DVG kick as a rendered frame, the clock
 *  - runs the NMI on machine time: one every 4 ms (the 3 kHz clock
 *    divided by 12), and a main-line frame whenever the NMI has raised
 *    SYNC, which is every fourth interrupt - so the game runs at the
 *    board's 62.5 Hz, not the monitor's
 *  - RANDOM/ALLPOT come from a real POKEY (pokey.c) under this port's
 *    modelled clock (pokey.h).  Sound output: the POKEY is rendered and
 *    streamed to plat_audio_push every tick (render_push_audio() below),
 *    and the two discrete circuits - explosion, thrust - play as
 *    recorded samples through plat_sample_start/stop, the way the
 *    user's AAE driver plays them (asteroid_explode_w/astdelux_sounds_w
 *    in asteroid.cpp).
 *
 * The harness host (host_stub.c) implements the same seam without a
 * platform; the two are never linked together.
 */
#include <stdio.h>
#include <string.h>

#include "astdelux.h"
#include "pokey.h"
#include "er2055.h"
#include "platform/ad_platform.h"

extern int dvg_render(void);

/* The NMI period.  The board's is 4 ms (3 kHz / 12), four to a frame,
 * so 62.5 frames a second.  ad_app_set_frame_rate() lets a host ask
 * for another rate - 60 Hz to match a 60 Hz monitor - by stretching
 * the period: the whole machine then runs proportionally slower, game,
 * POKEY pitch and all, exactly as if the board's clock were lower.
 * Only the audio block size follows the period, so the stream stays
 * real time. */
static double   nmi_ms = 4.0;           /* 1000 / (4 * frame rate) */
static unsigned frame_hz10 = 625;       /* frame rate x 10, for exact audio arithmetic */
#define AD_NMI_POKEY_CYCLES 6048        /* one NMI period at 1.512 MHz, whatever
                                         * its wall-clock length: underclocking */
#define AD_RANDOM_READ_COST 64          /* POKEY cycles this host charges before each
                                         * game RANDOM read: a stand-in for the 6502
                                         * cycles the read and its neighbours take,
                                         * since there is no CPU to count them (the
                                         * chip itself charges nothing - pokey.h) */
#define AD_MAINLINE_LEAD_CYCLES 512     /* POKEY cycles advanced before the main line
                                         * runs its frame: on the board the frame's
                                         * code runs for milliseconds after the NMI
                                         * that raised SYNC, so by the time PKYTST
                                         * reads ALLPOT the NMI's 228-cycle fast pot
                                         * scan has long finished ("S/B 0"); here the
                                         * frame runs at the NMI's instant, so stand
                                         * that time in explicitly */
#define AD_AUDIO_RATE 44100             /* render/output rate, Hz */

/* Fixed mixer channels for the two sampled discrete circuits (see
 * CONVENTIONS.md rule 8 - the mixer channel number is not a ROM/board
 * address, it is this host's own bookkeeping, same footing as picking
 * which GL texture unit a sprite goes on). */
#define AD_CHAN_THRUST    0
#define AD_CHAN_EXPLOSION 1

static plat_inputs cur_in;
static unsigned frames, nmis, dvg_errors;
static bool test_mode;                      /* running the cabinet self-test */

/* POKEY clock is the same 1.512 MHz as the 6502 (astdelux2_main.asm);
 * AD_AUDIO_RATE is the render/output rate, fed to both ad_pokey_init and
 * plat_audio_open so the render loop and the stream voice agree. */
static ad_pokey pokey;

/* The EAROM: high scores persist in `astdelux.nv` (plat_nvram_read/write,
 * platform/windows/plat_win.c), loaded once at start and saved whenever
 * the ROM's own write cycle finishes (see ad_app_step()) and at exit. */
static ad_er2055 earom;

/* ------------------------------------------------------------------ */
/* POKEY audio: render one tick's worth of samples and push them        */
/* ------------------------------------------------------------------ */
/* One NMI period of audio is AD_AUDIO_RATE / (4 * frame rate) frames:
 * 176.4 at 62.5 Hz, 183.75 at 60 Hz - not an integer - so a running
 * remainder in units of 1 / (4 * frame_hz10) carries the fractional
 * part across calls exactly: most ticks render the whole part, and
 * every few one more absorbs the accumulated remainder, so the long-run
 * output rate is exactly AD_AUDIO_RATE with no drift.
 *
 * Always called per NMI period, even from self-test: self-test's own
 * while loop advances machine time four periods at a pass because no
 * NMIs run there, but audio still renders one period at a time (four
 * calls per pass) rather than one four-period block - that would
 * overrun mixer.h's STREAM_BLOCK_FRAMES (512), and splitting it keeps
 * both paths on the same rate-locked sequence out of one accumulator. */
static uint32_t audio_frac;
static int16_t  audio_buf[256];         /* 220.5 frames at 50 Hz is the most */

static void render_push_audio_tick(void)
{
    const uint32_t den = 4u * frame_hz10;
    int frames_n;

    audio_frac += (uint32_t)AD_AUDIO_RATE * 10u;
    frames_n = (int)(audio_frac / den);
    audio_frac %= den;

    if (frames_n > (int)(sizeof audio_buf / sizeof audio_buf[0]))
        frames_n = (int)(sizeof audio_buf / sizeof audio_buf[0]);   /* defensive */

    ad_pokey_render(&pokey, audio_buf, frames_n);
    plat_audio_push(audio_buf, frames_n);
}

/* ------------------------------------------------------------------ */
/* The hardware seam                                                   */
/* ------------------------------------------------------------------ */

/* The option switches, as the board reads them.  A host sets them with
 * ad_app_set_dips() before ad_app_init(); the defaults are MAME's for
 * astdelux, which are the manual's factory settings.
 *
 * dsw1 is the 8-switch bank behind $2800-$2803.  The 6502 sees two
 * switches per address in bits 0-1 (asteroid.cpp's DSW1_r: address 0
 * gets bits 7-6, 1 gets 5-4, 2 gets 3-2, 3 gets 1-0), and the ROM names
 * them OPTN1..OPTN4:
 *
 *   bits 7-6  OPTN1 $2800  bonus life: 0 = 10,000, 1 = 12,000,
 *                          2 = 15,000, 3 = none (BONUS table $7C6E,
 *                          SINIT in frame.c)
 *   bit  5    -     $2801  bit 1: MAME calls it Difficulty (0 hard,
 *                          1 easy); rev 2 never reads it - it only
 *                          shows on the self-test screen
 *   bit  4    OPTN2 $2801  bit 0: 1 = two-coin minimum (score.c)
 *   bits 3-2  OPTN3 $2802  lives: value + 2 (plus one with no bonus
 *                          life, plus one at two coins per play)
 *   bits 1-0  OPTN4 $2803  language: 0 English, 1 German, 2 French,
 *                          3 Spanish (message.c)
 *
 * dsw2 is the coin bank on the POKEY pot pins, read through ALLPOT; a
 * switch reads high when OFF, and the NMI keeps the inverse in CMODE
 * (nmi.c).  In ALLPOT terms:
 *
 *   bits 1-0  coinage: 0 = two coins per play, 1 = one coin per play,
 *                      2 = two plays per coin, 3 = free play
 *   bits 3-2  right mech units: 3 = x1, 2 = x4, 1 = x5, 0 = x6
 *   bit  4    centre mech units: 1 = x1, 0 = x2
 *   bits 7-5  bonus coins (MODULO $77EA): 7 = none, 6 = 1 each 2,
 *                      5 = 1 each 4, 4 = 2 each 4, 3 = 1 each 5,
 *                      2 = 1 each 3, 1 and 0 = none */
static uint8_t dsw1 = 0x04;             /* English, 3 lives, 1 play min, hard, 10,000 */
static uint8_t dsw2 = 0x9D;             /* 1 coin 1 play, x1, x1, 2 bonus each 4 */

void ad_app_set_dips(uint8_t bank1, uint8_t bank2)
{
    dsw1 = bank1;
    dsw2 = bank2;
}

/* The program ROM revision the game behaves as: [game] revision in the
 * ini.  2 is the port's base; 3 switches on each rev 3 difference at
 * the point it occurs (astdelux.h, ad_hw_rom_rev). */
static uint8_t rom_rev = 2;

void ad_app_set_revision(int rev)
{
    rom_rev = (rev >= 3) ? 3 : 2;
}

uint8_t ad_hw_rom_rev(void)
{
    return rom_rev;
}

/* Main-line computation time the ROM declares (astdelux.h): straight
 * onto the POKEY clock. */
void ad_hw_cycles(uint16_t cycles)
{
    ad_pokey_advance(&pokey, cycles);
}

/* Switch inputs by full 6502 address; bit 7 = pressed, as the board
 * presents them.  The DIP addresses answer in bits 0-1, as the board
 * does (the game masks with #3). */
uint8_t ad_hw_switch(uint16_t addr)
{
    int on = 0;
    if ((addr & 0xFFFC) == 0x2800)
        return (uint8_t)((dsw1 >> (2 * (3 - (addr & 3)))) & 0x03);
    switch (addr) {
    case 0x2003: on = cur_in.shield; break;             /* HYPSW: shields */
    case 0x2004: on = cur_in.fire;   break;             /* FIRESW */
    case 0x2400: on = cur_in.coin1;  break;             /* left coin mech */
    case 0x2401: on = cur_in.coin2;  break;             /* centre coin mech */
    case 0x2403: on = cur_in.start1; break;             /* STRT1 */
    case 0x2404: on = cur_in.start2; break;             /* STRT2 */
    case 0x2405: on = cur_in.thrust; break;             /* THRUST */
    case 0x2406: on = cur_in.rotr;   break;             /* ROTR */
    case 0x2407: on = cur_in.rotl;   break;             /* ROTL */
    case 0x2007: on = cur_in.test;   break;             /* STSTSW */
    default: break;                                     /* slam, DIPs */
    }
    return on ? 0x80 : 0x00;
}

/* POKEY.  Register 8 (read) is ALLPOT: the coin DIP (OPTN5, dsw2 above)
 * is strapped to the pot pins, so pokey.c's pot-scan model answers it
 * (see ad_pokey_set_allpot() in ad_app_init() below). */
uint8_t ad_hw_pokey_read(uint8_t r)   { return ad_pokey_read(&pokey, r); }
void    ad_hw_pokey_write(uint8_t r, uint8_t v) { ad_pokey_write(&pokey, r, v); }

/* RANDOM, from the real POKEY polynomial (pokey.c).  The game's reads
 * are not cycle-annotated, so this host moves the chip a flat
 * AD_RANDOM_READ_COST before each one - see pokey.h's time model. */
uint8_t ad_hw_random(void)
{
    ad_pokey_advance(&pokey, AD_RANDOM_READ_COST);
    return ad_pokey_read(&pokey, 0x0A);
}

/* EAROM: a real ER2055 model (er2055.c) behind the same three latches
 * the board has - EAIN, EADAL,x and EACTL - persisted to astdelux.nv. */
uint8_t ad_hw_earom_read(void)        { return ad_er2055_data(&earom); }
void    ad_hw_earom_write(uint8_t addr, uint8_t data)
                                       { ad_er2055_set_addr_data(&earom, addr, data); }
void    ad_hw_earom_ctl(uint8_t v)    { ad_er2055_control(&earom, v); }

/* GOADD: the list at word 0 is complete - draw it.  This is the one
 * place a frame reaches the screen. */
void ad_hw_vg_go(void)
{
    frames++;
    plat_video_begin();
    if (dvg_render() != 0)
        dvg_errors++;
    plat_video_present();
}
bool    ad_hw_vg_busy(void)           { return false; }   /* drawn instantly */
void    ad_hw_vg_reset(void)          { }
void    ad_hw_watchdog(void)          { }

/* $3600 (ExpPitchVol), written every NMI tick while an explosion plays:
 * bits 6-7 are a size/volume class, constant for the whole explosion;
 * bits 0-5 are a pitch value the ROM only ever counts DOWN once it is
 * loaded - a hit reloads it to (size<<6)|0x3F (ObjHitSFX/L6B4A; the
 * ship's own hit uses 0x3E, L7074), then each later tick's write has a
 * smaller pitch than the last. So a NEW explosion is exactly the moment
 * the pitch bits jump back UP - the rising edge is a reliable one-shot
 * trigger for every size, and for a second hit landing before the first
 * explosion's countdown has finished, which a volume/class-only compare
 * would miss. Copied from the AAE driver's asteroid_explode_w(). */
void ad_hw_explosion(uint8_t v)
{
    static const int explode_sample[4] = {
        AD_SMP_EXPLODE1, AD_SMP_EXPLODE2, AD_SMP_EXPLODE3, AD_SMP_EXPLODE4
    };
    static uint8_t prev;
    const uint8_t pitch = v & 0x3F;
    const uint8_t prev_pitch = prev & 0x3F;

    g.expsnd = v;
    if (pitch > prev_pitch && pitch >= 0x3C) {       /* fresh timer loaded: explosion start */
        const int cls = v >> 6;                       /* 0..3 */
        plat_sample_start(AD_CHAN_EXPLOSION, explode_sample[cls], 0);
    }
    prev = v;
}

/* $3C03 (THRUST), the discrete thrust-noise gate: this port starts the
 * recorded thrust.wav looping on the off->on edge and stops it on the
 * on->off edge - the same edge-triggered gating as astdelux_sounds_w()
 * in the AAE driver, though that driver's sample_end_mixer() lets
 * the current loop pass finish rather than cutting immediately; a plain
 * plat_sample_stop() is the contract this platform header offers today. */
void ad_hw_thrust(bool on)
{
    static bool prev_on;

    g.thrust_on = on;
    if (on && !prev_on)
        plat_sample_start(AD_CHAN_THRUST, AD_SMP_THRUST, 1);
    else if (!on && prev_on)
        plat_sample_stop(AD_CHAN_THRUST);
    prev_on = on;
}

void    ad_hw_noise_reset(void)       { }
void    ad_hw_lamp(int w, bool on)
{
    if (w >= 0 && w < 2) {
        g.lamps[w] = on;
        plat_leds_out((uint8_t)((g.lamps[0] ? 1 : 0) | (g.lamps[1] ? 2 : 0)));
    }
}
void    ad_hw_coin_counter(int w)     { (void)w; }

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

/* START ($6000)'s non-looping half: INIT, kick the EAROM read, the
 * first wave.  Shared by ad_app_init and by the exit from self-test
 * (STEST6's `jmp START`, taken when the switch is released). */
static void start_game(void)
{
    ad_init();
    if (g.zp.f.EAFLG == 0) {
        g.zp.f.EABC = 0;
        g.zp.f.EAX = 0;
        g.zp.f.EAHSX = 0;
        g.zp.f.EAFLG = 0x20;
    }
    ad_newast();
}

/* A host may call this before ad_app_init: frame rate in Hz, 50..70.
 * 62.5 is the board; 60 matches a 60 Hz monitor at 4% slower play. */
void ad_app_set_frame_rate(double hz)
{
    if (hz < 50.0) hz = 50.0;
    if (hz > 70.0) hz = 70.0;
    frame_hz10 = (unsigned)(hz * 10.0 + 0.5);
    nmi_ms = 1000.0 / (4.0 * (frame_hz10 / 10.0));
}

void ad_app_init(void)
{
    /* Live before ad_pwron(), which writes SKCTL through
     * ad_hw_pokey_write() as PWRON does. */
    ad_pokey_init(&pokey, 1512000, AD_AUDIO_RATE);
    ad_pokey_set_allpot(&pokey, dsw2);

    ad_er2055_init(&earom);
    ad_er2055_control(&earom, 0);    /* board reset (asteroid_m.cpp); the
                                      * ROM's own INIT never writes EACTL
                                      * directly - ad_eaupd does, on its
                                      * first step - so mirror the
                                      * hardware reset explicitly here;
                                      * a no-op since init already leaves
                                      * control at 0. */
    if (plat_nvram_read(earom.rom, sizeof earom.rom) != 0) {
        /* astdelux.nv doesn't exist yet: stays zero-filled, same as a
         * fresh chip on this board (asteroid.cpp's ROMREGION_ERASE00). */
    }

    if (plat_audio_open(AD_AUDIO_RATE) != 0)
        fprintf(stderr, "plat_audio_open failed; continuing without sound\n");

    ad_pwron();
    plat_input_poll(&cur_in);
    if (ad_hw_switch(0x2007) & 0x80) {      /* STSTSW held at power-up */
        ad_stest3();
        test_mode = true;
    } else {
        start_game();                       /* START ($6000) */
    }
}

void ad_app_nvram_save(void)
{
    if (plat_nvram_write(earom.rom, sizeof earom.rom) == 0)
        earom.dirty = false;
}

/* Machine time.  Every elapsed 4 ms is one NMI; when the NMI has set
 * SYNC the main line runs its frame (START2), which consumes it.  The
 * frame loop's own spin on SYNC never turns here because it is only
 * entered once SYNC is up.  A stall longer than a frame is dropped
 * rather than caught up: a burst of NMIs would push SYNC past 3, which
 * on the board is the deliberate hang the watchdog answers. */
/* Saves astdelux.nv once the ROM's own EAROM write cycle has finished
 * (EAFLG back to 0) and something in the image actually changed - not
 * every frame, so an idle table costs nothing. */
static void maybe_save_nvram(void)
{
    if (earom.dirty && g.zp.f.EAFLG == 0)
        ad_app_nvram_save();
}

double ad_app_step(double now_ms)
{
    static double last_ms = -1.0, acc = 0.0;
    static double fps_t0 = -1.0;
    static unsigned fps_frames;
    static bool last_test_sw;
    double interval;

    plat_input_poll(&cur_in);

    if (last_ms < 0.0) last_ms = now_ms;
    acc += now_ms - last_ms;
    last_ms = now_ms;
    if (acc > 4.0 * nmi_ms)
        acc = 4.0 * nmi_ms;                     /* stall guard: one frame */

    /* STSTSW going from off to on while the game is running stands in
     * for the operator flipping the switch and power-cycling: on the
     * real board STSTSW is only ever read at PWRON/reset, so entering
     * self-test on demand (rather than only at power-up) has to fake
     * that reset here. */
    if (!test_mode && cur_in.test && !last_test_sw) {
        ad_pwron();
        ad_stest3();
        test_mode = true;
        acc = 0.0;
    }
    last_test_sw = cur_in.test != 0;

    if (test_mode) {
        /* STEST5..STEST7: no NMIs run in self-test (they are disabled
         * on the real board); one display-loop pass every ~16 ms. */
        while (acc >= 4.0 * nmi_ms) {
            int i;
            acc -= 4.0 * nmi_ms;
            ad_pokey_advance(&pokey, 4 * AD_NMI_POKEY_CYCLES);  /* NMIs don't run
                                                                  * in self-test,
                                                                  * but time still
                                                                  * passes */
            for (i = 0; i < 4; i++)
                render_push_audio_tick(); /* one pass = four NMI periods of audio */
            if (!ad_stest_frame()) {
                test_mode = false;
                start_game();                    /* jmp START */
                break;
            }
            maybe_save_nvram();          /* STEST6 drives ad_eaupd too */
        }
    } else {
        while (acc >= nmi_ms) {
            acc -= nmi_ms;
            nmis++;
            ad_pokey_advance(&pokey, AD_NMI_POKEY_CYCLES);
            ad_nmi();
            render_push_audio_tick();     /* after ad_nmi(): this tick's registers are set */
            if (g.zp.f.SYNC & 1) {
                ad_pokey_advance(&pokey, AD_MAINLINE_LEAD_CYCLES);
                if (!ad_frame())
                    ad_newast();                    /* START1: next wave */
                fps_frames++;
                maybe_save_nvram();
            }
        }
    }

    if (fps_t0 < 0.0) fps_t0 = now_ms;
    if (now_ms - fps_t0 >= 1000.0) {
        char buf[160];
        if (test_mode) {
            snprintf(buf, sizeof buf, "SELF-TEST  XT=%u CKERR=%02X",
                     g.zp.f.XT, g.zp.f.CKERR);
        } else {
            snprintf(buf, sizeof buf, "%u fps  NPLAYR=%u CRDT=%u NROCKS=%u score=%02X%02X%02X%s",
                     fps_frames, g.zp.f.NPLAYR, g.zp.f.CRDT, AD_P->f.NROCKS,
                     g.zp.f.SCORE[2], g.zp.f.SCORE[1], g.zp.f.SCORE[0],
                     dvg_errors ? "  DVG list errors" : "");
        }
        plat_status_text(buf);
        fps_frames = 0;
        fps_t0 = now_ms;
    }
    interval = (test_mode ? 4.0 * nmi_ms : nmi_ms) - acc;
    return interval;
}
