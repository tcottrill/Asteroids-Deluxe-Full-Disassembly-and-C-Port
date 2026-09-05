/* ad_platform.h - the platform contract for the Asteroids Deluxe C port.
 *
 * Adapted from the Omega Race port's omega_platform.h: the core (the
 * game modules, dvg.c, app_win.c) is platform-agnostic and calls ONLY
 * these functions for rendering, input, time and storage.  A backend
 * under platform/<name>/ implements the whole contract in plain C and
 * owns main(); the build links exactly one.
 *
 * Backends:
 *   windows/   Win32 + OpenGL 3.3 (verbatim from the Omega Race port,
 *              with this game's inputs).  Builds astdelux_win.exe.
 *   teensy/    the Teensy 4.1 of the Masteroids-class replica board:
 *              DAC7811 X/Y, a 5-bit Z ladder, PT8211 audio, the cabinet
 *              switches.  An Arduino sketch; see teensy/README.md.
 *   (headless) host_stub.c is the harness host and does not use this
 *              contract; it implements the ad_hw_* seam directly.
 */
#ifndef AD_PLATFORM_H
#define AD_PLATFORM_H

#include <stdint.h>

/* ---- lifecycle --------------------------------------------------------- */

int  plat_init(void);      /* window/audio up; 0 = ok, nonzero = fail */
void plat_shutdown(void);

/* ---- video: the segment sink -------------------------------------------
 * The DVG walker (dvg.c) emits every lit segment through plat_video_line
 * in DVG beam space (x 0..1040, y 70..950 visible, y up), z = the intensity
 * nibble 0..15 as the display list carries it.  A zero-length segment
 * is a dot.  A raster backend draws between begin/present. */
void plat_video_begin(void);                    /* start of frame / clear */
void plat_video_line(float x0, float y0, float x1, float y1, int z);
void plat_video_present(void);                  /* flip */

/* ---- input: abstract controls ------------------------------------------
 * Polled by the core; the core maps them onto the ROM's switch ports
 * (ad_hw_switch).  Key bindings are backend policy.  Fields are 0/1. */
typedef struct {
    uint8_t rotl, rotr, thrust, fire, shield;
    uint8_t coin1, coin2;
    uint8_t start1, start2;
    uint8_t test;            /* cabinet self-test switch                 */
    uint8_t quit;
} plat_inputs;

void plat_input_poll(plat_inputs* in);

/* ---- audio: sample playback --------------------------------------------
 * For the two discrete circuits (explosion, thrust) - recorded samples,
 * same as the AAE driver plays them, rather than modelled
 * circuitry. `sample` is one of the AD_SMP_* numbers below. */
void plat_sample_start(int channel, int sample, int loop);
void plat_sample_stop(int channel);

/* Sample numbers for plat_sample_start, matching astdelux.zip's members
 * (asteroid.cpp's deluxesamples[]): explode1..4.wav then thrust.wav. */
enum {
    AD_SMP_EXPLODE1 = 0,
    AD_SMP_EXPLODE2,
    AD_SMP_EXPLODE3,
    AD_SMP_EXPLODE4,
    AD_SMP_THRUST
};

/* ---- audio: the POKEY's own output --------------------------------------
 * A continuously-fed mono PCM stream, pushed a block at a time (nominally
 * once per 4 ms NMI tick, rendered from the just-advanced ad_pokey) rather
 * than loaded as a sample. plat_audio_open brings the stream up at
 * sample_rate; plat_audio_push queues `frames` samples of 16-bit PCM;
 * plat_audio_close tears it down. A backend with no audio may no-op all
 * three (open returning nonzero, the core logs and continues either way -
 * see CONVENTIONS.md rule 8, hardware only through ad_hw_ and plat_ calls,
 * never bypassed even when the hardware in question is a sound card). */
int  plat_audio_open(int sample_rate);
void plat_audio_push(const int16_t *pcm, int frames);
void plat_audio_close(void);

/* ---- time --------------------------------------------------------------- */

double plat_now_ms(void);            /* monotonic, any epoch              */
void   plat_sleep_ms(int ms);        /* scheduling hint; may return early */

/* ---- storage: the EAROM image as one blob ------------------------------ */
int plat_nvram_read(void* buf, unsigned len);
int plat_nvram_write(const void* buf, unsigned len);

/* ---- misc --------------------------------------------------------------- */

void plat_leds_out(uint8_t lamps);       /* start-button lamps, bit 0/1    */
void plat_status_text(const char* s);    /* fps text in the window title   */

/* ---- core app loop (app_win.c) - what a backend's main() calls -------- */

void   ad_app_set_frame_rate(double hz); /* optional, before ad_app_init: 62.5
                                           is the board's; 60 runs the whole
                                           machine 4% slow to suit a 60 Hz
                                           monitor                          */
void   ad_app_set_revision(int rev);      /* optional, before ad_app_init: which
                                           program ROM revision to behave as,
                                           2 (default, the port's base) or 3 */
void   ad_app_set_dips(uint8_t dsw1, uint8_t dsw2);
                                        /* optional, before ad_app_init: the two
                                           option-switch banks as the board
                                           reads them - dsw1 the 8-switch bank
                                           at $2800 (bits 7-6 -> $2800, 5-4 ->
                                           $2801, 3-2 -> $2802, 1-0 -> $2803),
                                           dsw2 the coin bank on the POKEY pot
                                           pins (ALLPOT, high = switch off).
                                           Bit meanings: app_win.c's table   */
void   ad_app_init(void);               /* PWRON, INIT, first wave          */
double ad_app_step(double now_ms);      /* run the elapsed 4 ms NMIs, and a
                                           frame when SYNC says so; returns
                                           ms until the next NMI is due     */
void   ad_app_nvram_save(void);         /* flush on exit                    */

#endif
