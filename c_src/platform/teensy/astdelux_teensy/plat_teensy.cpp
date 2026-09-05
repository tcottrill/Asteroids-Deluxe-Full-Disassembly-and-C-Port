/* plat_teensy.cpp - the Teensy 4.1 backend for the Masteroids-class replica
 * board: everything in ad_platform.h that vec_out.cpp (video) and
 * audio_out.cpp (audio) do not cover.
 *
 *  - video sink: dvg.c's segments are mapped to DAC codes as they arrive
 *    (vec_map, the stock DVG's geometry) into a per-frame buffer that
 *    plat_video_present hands to vec_out_draw, synchronously
 *  - input: the cabinet switches on GPIO with pull-ups, LOW = pressed; the
 *    self-test switch is a real switch, so its level is passed straight
 *    through (ad_app_step edge-detects it)
 *  - time: micros() widened to 64 bits across its 71.6-minute wrap
 *  - the EAROM image in the Teensy's EEPROM behind a magic word
 *  - lamps on two GPIOs; the once-a-second status line to USB serial
 */
#include <Arduino.h>
#include <EEPROM.h>
#include <string.h>
#include "teensy_config.h"
#include "vec_beam.h"
#include "vec_out.h"
#include "audio_mix.h"
#include "audio_out.h"
#include "plat_teensy.h"

extern "C" {
#include "src/core/platform/ad_platform.h"
}

/* ------------------------------------------------------------------ */
/* video                                                                */
/* ------------------------------------------------------------------ */

DMAMEM static vec_seg segs[VEC_MAX_SEGS];   /* RAM2: RAM1 is nearly full (the POKEY's 256 KB of poly tables) */
static int      seg_n;
static uint32_t seg_dropped;          /* beyond VEC_MAX_SEGS, counted for the status line */
static double   last_frame_ms;

extern "C" void plat_video_begin(void)
{
    seg_n = 0;
}

extern "C" void plat_video_line(float x0, float y0, float x1, float y1, int z)
{
    if (seg_n >= VEC_MAX_SEGS) { seg_dropped++; return; }
    vec_seg *s = &segs[seg_n++];
    s->x0 = vec_map(x0, VEC_FLIP_X);
    s->y0 = vec_map(y0, VEC_FLIP_Y);
    s->x1 = vec_map(x1, VEC_FLIP_X);
    s->y1 = vec_map(y1, VEC_FLIP_Y);
    s->z  = (uint8_t)(z & 15);
}

extern "C" void plat_video_present(void)
{
    vec_out_draw(segs, seg_n);
    last_frame_ms = plat_now_ms();
}

double plat_teensy_last_frame_ms(void)
{
    return last_frame_ms;
}

/* The bring-up pattern, in DVG coordinates so it goes through the same
 * mapping as the game: the DVG's full 1024 square, the game's visible
 * window (y 70..950), a crosshair through the centre, sixteen short lines
 * one per intensity (left to right, dim to bright) and a dot. */
void plat_teensy_test_pattern(void)
{
    plat_video_begin();
    plat_video_line(0, 0, 1023, 0, 15);      plat_video_line(1023, 0, 1023, 1023, 15);
    plat_video_line(1023, 1023, 0, 1023, 15); plat_video_line(0, 1023, 0, 0, 15);
    plat_video_line(0, 70, 1023, 70, 8);     plat_video_line(0, 950, 1023, 950, 8);
    plat_video_line(0, 512, 1023, 512, 8);   plat_video_line(512, 0, 512, 1023, 8);
    for (int z = 0; z < 16; z++) {
        float x = 96.0f + 56.0f * (float)z;
        plat_video_line(x, 300, x, 400, z);
    }
    plat_video_line(512, 700, 512, 700, 15); /* a dot */
    plat_video_present();
    delay(16);
}

/* ------------------------------------------------------------------ */
/* input                                                                */
/* ------------------------------------------------------------------ */

static const uint8_t switch_pins[] = {
    PIN_SW_ROT_L, PIN_SW_ROT_R, PIN_SW_THRUST, PIN_SW_FIRE, PIN_SW_HYPER,
    PIN_SW_START1, PIN_SW_START2, PIN_SW_COIN_L, PIN_SW_COIN_C, PIN_SW_COIN_R,
    PIN_SW_SLAM, PIN_SW_SELFTEST, PIN_SW_DIAG,
};

static uint8_t unmapped_bits;         /* bit 0 COIN_R, 1 SLAM, 2 DIAG - status line only */

static inline uint8_t pressed(uint8_t pin) { return digitalReadFast(pin) == LOW ? 1 : 0; }

extern "C" void plat_input_poll(plat_inputs *in)
{
    in->rotl   = pressed(PIN_SW_ROT_L);
    in->rotr   = pressed(PIN_SW_ROT_R);
    in->thrust = pressed(PIN_SW_THRUST);
    in->fire   = pressed(PIN_SW_FIRE);
    in->shield = pressed(PIN_SW_HYPER);
    in->coin1  = pressed(PIN_SW_COIN_L);
    in->coin2  = pressed(PIN_SW_COIN_C);
    in->start1 = pressed(PIN_SW_START1);
    in->start2 = pressed(PIN_SW_START2);
    in->test   = pressed(PIN_SW_SELFTEST);
    in->quit   = 0;
    unmapped_bits = (uint8_t)(pressed(PIN_SW_COIN_R) | (pressed(PIN_SW_SLAM) << 1)
                              | (pressed(PIN_SW_DIAG) << 2));
}

/* ------------------------------------------------------------------ */
/* time                                                                 */
/* ------------------------------------------------------------------ */

extern "C" double plat_now_ms(void)
{
    static uint32_t last_us;
    static uint64_t total_us;
    uint32_t now = micros();
    total_us += (uint32_t)(now - last_us);     /* unsigned difference survives the wrap */
    last_us = now;
    return (double)total_us / 1000.0;
}

extern "C" void plat_sleep_ms(int ms)
{
    if (ms > 0) delay((uint32_t)ms);
}

/* ------------------------------------------------------------------ */
/* storage: the 64-byte ER2055 image in EEPROM behind a magic word       */
/* ------------------------------------------------------------------ */

static const uint8_t NV_MAGIC[4] = { 'A', 'D', 'N', 'V' };
#define NV_DATA_OFFSET 4

extern "C" int plat_nvram_read(void *buf, unsigned len)
{
    for (unsigned i = 0; i < 4; i++)
        if (EEPROM.read(i) != NV_MAGIC[i]) return 1;    /* fresh chip: leave zeros */
    uint8_t *p = (uint8_t *)buf;
    for (unsigned i = 0; i < len; i++) p[i] = EEPROM.read(NV_DATA_OFFSET + i);
    return 0;
}

extern "C" int plat_nvram_write(const void *buf, unsigned len)
{
    const uint8_t *p = (const uint8_t *)buf;
    if (NV_DATA_OFFSET + len > (unsigned)EEPROM.length()) return 1;
    for (unsigned i = 0; i < len; i++) EEPROM.update(NV_DATA_OFFSET + i, p[i]);   /* only changed bytes wear */
    for (unsigned i = 0; i < 4; i++) EEPROM.update(i, NV_MAGIC[i]);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lamps, status                                                        */
/* ------------------------------------------------------------------ */

extern "C" void plat_leds_out(uint8_t lamps)
{
    digitalWriteFast(PIN_LED_START1, (lamps & 1) ? HIGH : LOW);
    digitalWriteFast(PIN_LED_START2, (lamps & 2) ? HIGH : LOW);
}

extern "C" void plat_status_text(const char *s)
{
    Serial.println(s);
    Serial.printf("  draw %lu us  segs %d  dropped %lu  audio fill %lu over %lu under %lu  unmapped sw %u\n",
                  (unsigned long)vec_out_last_frame_us(), seg_n, (unsigned long)seg_dropped,
                  (unsigned long)audio_mix_fill(), (unsigned long)audio_mix_overruns(),
                  (unsigned long)audio_mix_underruns(), unmapped_bits);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                            */
/* ------------------------------------------------------------------ */

extern "C" int plat_init(void)
{
    Serial.begin(115200);                  /* USB: the rate is nominal; never blocks */
    for (uint8_t pin : switch_pins) pinMode(pin, INPUT_PULLUP);
    pinMode(PIN_LED_START1, OUTPUT); digitalWriteFast(PIN_LED_START1, LOW);
    pinMode(PIN_LED_START2, OUTPUT); digitalWriteFast(PIN_LED_START2, LOW);
    vec_out_init();
    audio_out_setup();
    seg_n = 0;
    seg_dropped = 0;
    last_frame_ms = plat_now_ms();
    Serial.println("astdelux_teensy backend up");
    return 0;
}

extern "C" void plat_shutdown(void)
{
    vec_out_park();
}
