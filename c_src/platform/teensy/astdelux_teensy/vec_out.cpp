/* vec_out.cpp - DAC7811 pair and Z ladder.  See vec_out.h and the spec.
 *
 * One DAC write is SYNC low, one 16-bit transfer, SYNC high: the DAC7811
 * latches on the 16th falling SCLK edge (stand-alone mode; daisy-chain is
 * switched off at init with 0x9000).  X and Y are written back to back, so
 * the two axes update about half a microsecond apart at 30 MHz - accepted.
 *
 * Beam stepping: a move (beam off) walks in VEC_MOVE_STEP counts, a lit
 * segment in VEC_DRAW_STEP counts (vec_plan), each step optionally dwelling.
 * Z is set before the first lit step and left on across consecutive
 * connected segments; it goes off before any move and at the end of the
 * frame.  A zero-length segment is a dot: Z on for VEC_DOT_US.
 *
 * Safety: Z is driven off before anything else at boot, and vec_out_park
 * blanks and centres the beam when the sketch has drawn nothing for
 * VEC_PARK_MS.
 */
#include <Arduino.h>
#include <SPI.h>
#include "teensy_config.h"
#include "vec_beam.h"
#include "vec_out.h"

static const SPISettings dac_settings(VEC_SPI_HZ, MSBFIRST, VEC_SPI_MODE);

static int      cur_x = -1, cur_y = -1;   /* -1: DAC contents unknown */
static int      cur_z = -1;               /* ladder code last written */
static uint32_t last_frame_us;

static inline void dac_write(uint8_t cs, int code)
{
    digitalWriteFast(cs, LOW);
    SPI.transfer16(vec_dac_word((uint16_t)code));
    digitalWriteFast(cs, HIGH);
}

static inline void beam_to(int x, int y)
{
    if (x != cur_x) { dac_write(PIN_CS_X, x); cur_x = x; }
    if (y != cur_y) { dac_write(PIN_CS_Y, y); cur_y = y; }
}

static inline void z_set(int code)
{
    if (code == cur_z) return;
    uint8_t p = vec_z_pins((uint8_t)code);
    digitalWriteFast(PIN_Z_B0, (p >> 0) & 1);
    digitalWriteFast(PIN_Z_B1, (p >> 1) & 1);
    digitalWriteFast(PIN_Z_B2, (p >> 2) & 1);
    digitalWriteFast(PIN_Z_B3, (p >> 3) & 1);
    digitalWriteFast(PIN_Z_B4, (p >> 4) & 1);
    cur_z = code;
}

/* Walk the beam from where it is to (x1, y1). */
static void beam_run(int x1, int y1, int step, unsigned dwell_us)
{
    static vec_pt pts[VEC_MAX_PTS];
    int n = vec_plan(cur_x, cur_y, x1, y1, step, pts, VEC_MAX_PTS);
    for (int i = 0; i < n; i++) {
        beam_to(pts[i].x, pts[i].y);
        if (dwell_us) delayMicroseconds(dwell_us);
    }
}

void vec_out_init(void)
{
    /* Z off before anything can light the tube */
    pinMode(PIN_Z_B0, OUTPUT); pinMode(PIN_Z_B1, OUTPUT); pinMode(PIN_Z_B2, OUTPUT);
    pinMode(PIN_Z_B3, OUTPUT); pinMode(PIN_Z_B4, OUTPUT);
    cur_z = -1;
    z_set(0);

    pinMode(PIN_CS_X, OUTPUT); digitalWriteFast(PIN_CS_X, HIGH);
    pinMode(PIN_CS_Y, OUTPUT); digitalWriteFast(PIN_CS_Y, HIGH);
    SPI.setMISO(PIN_SPI_MISO_ALT);      /* pin 12 is SW_FIRE on this board */
    SPI.begin();

    static const uint8_t cs_pins[2] = { PIN_CS_X, PIN_CS_Y };
    SPI.beginTransaction(dac_settings);
    for (int i = 0; i < 2; i++) {
        digitalWriteFast(cs_pins[i], LOW);
        SPI.transfer16(0x9000);          /* C3..C0 = 1001: daisy-chain disable */
        digitalWriteFast(cs_pins[i], HIGH);
    }
    cur_x = cur_y = -1;
    beam_to(VEC_DAC_CENTER, VEC_DAC_CENTER);
    SPI.endTransaction();
}

void vec_out_draw(const vec_seg *segs, int n)
{
    uint32_t t0 = micros();

    SPI.beginTransaction(dac_settings);
    for (int i = 0; i < n; i++) {
        const vec_seg *s = &segs[i];
        if (s->x0 != cur_x || s->y0 != cur_y) {
            z_set(0);
            beam_run(s->x0, s->y0, VEC_MOVE_STEP, VEC_MOVE_DWELL_US);
            if (VEC_SETTLE_US) delayMicroseconds(VEC_SETTLE_US);
        }
        z_set(vec_z_code(s->z));
        if (s->x1 == s->x0 && s->y1 == s->y0)
            delayMicroseconds(VEC_DOT_US);
        else
            beam_run(s->x1, s->y1, VEC_DRAW_STEP, VEC_DRAW_DWELL_US);
    }
    z_set(0);
    SPI.endTransaction();

    last_frame_us = micros() - t0;
}

void vec_out_park(void)
{
    z_set(0);
    SPI.beginTransaction(dac_settings);
    beam_to(VEC_DAC_CENTER, VEC_DAC_CENTER);
    SPI.endTransaction();
}

uint32_t vec_out_last_frame_us(void)
{
    return last_frame_us;
}
