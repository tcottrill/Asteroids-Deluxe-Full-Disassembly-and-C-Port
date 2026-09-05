/* probe_teensy.c - PC checks of the Teensy backend's pure modules,
 * vec_beam.c and audio_mix.c, with expectations from the DAC7811 datasheet,
 * the stock DVG's geometry and the spec - never from running the code.
 *
 *     build_probe.bat
 *     probe_teensy.exe
 *
 * Prints one numbered check per behaviour, FAIL lines for anything wrong,
 * and exits nonzero on any failure.
 */
#include <stdio.h>
#include <string.h>

#include "teensy_config.h"
#include "vec_beam.h"
#include "audio_mix.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("  FAIL: "); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------------------------ */
/* (1) DAC7811 word: control 0001 then 12 data bits                      */
/* ------------------------------------------------------------------ */
static void check_dac_word(void)
{
    printf("1. vec_dac_word: control nibble 0001, data masked to 12 bits\n");
    CHECK(vec_dac_word(0)      == 0x1000, "0 -> 0x1000, got %04X", vec_dac_word(0));
    CHECK(vec_dac_word(2048)   == 0x1800, "2048 -> 0x1800, got %04X", vec_dac_word(2048));
    CHECK(vec_dac_word(4095)   == 0x1FFF, "4095 -> 0x1FFF, got %04X", vec_dac_word(4095));
    CHECK(vec_dac_word(0xF123) == 0x1123, "high bits masked, got %04X", vec_dac_word(0xF123));
}

/* ------------------------------------------------------------------ */
/* (2) Geometry: the stock DVG's 1024 space onto the 12-bit DAC          */
/* ------------------------------------------------------------------ */
static void check_map(void)
{
    printf("2. vec_map: 512 -> 2048, 0 -> 0, 1023 -> 4092, clamps, flips\n");
    CHECK(vec_map(512.0f, 0) == 2048, "centre: got %d", vec_map(512.0f, 0));
    CHECK(vec_map(0.0f, 0) == 0, "0: got %d", vec_map(0.0f, 0));
    CHECK(vec_map(1023.0f, 0) == 4092, "1023: got %d", vec_map(1023.0f, 0));
    CHECK(vec_map(1040.0f, 0) == 4092, "1040 clamps to 4092: got %d", vec_map(1040.0f, 0));
    CHECK(vec_map(-5.0f, 0) == 0, "negative clamps to 0: got %d", vec_map(-5.0f, 0));
    CHECK(vec_map(100.4f, 0) == 400, "100.4 rounds to 100 -> 400: got %d", vec_map(100.4f, 0));
    CHECK(vec_map(100.6f, 0) == 404, "100.6 rounds to 101 -> 404: got %d", vec_map(100.6f, 0));
    CHECK(vec_map(100.0f, 1) == 4092 - 400, "flip mirrors 400 -> 3692: got %d", vec_map(100.0f, 1));
    CHECK(vec_map(512.0f, 1) == 2044, "flip of centre is 4092-2048: got %d", vec_map(512.0f, 1));
}

/* ------------------------------------------------------------------ */
/* (3) Z ladder: table shape and the 74HC04 inversion                     */
/* ------------------------------------------------------------------ */
static void check_z(void)
{
    int z, prev = -1, mono = 1;
    printf("3. vec_z_code: 0 -> 0, monotonic, <= 31; vec_z_pins inverts\n");
    CHECK(vec_z_code(0) == 0, "z 0 must be off");
    for (z = 1; z < 16; z++) {
        int c = vec_z_code(z);
        if (c <= prev) mono = 0;
        prev = c;
        CHECK(c <= 31, "z %d code %d exceeds 5 bits", z, c);
    }
    CHECK(mono, "codes must rise with z");
    CHECK(vec_z_code(15) == 31, "z 15 is full scale, got %d", vec_z_code(15));
    CHECK(vec_z_code(0x25) == vec_z_code(5), "z masked to a nibble");
#if VEC_Z_INVERT
    CHECK(vec_z_pins(0) == 0x1F, "off -> all pins high through the inverter, got %02X", vec_z_pins(0));
    CHECK(vec_z_pins(31) == 0x00, "full -> all pins low, got %02X", vec_z_pins(31));
    CHECK(vec_z_pins(0x15) == 0x0A, "10101 -> 01010, got %02X", vec_z_pins(0x15));
#else
    CHECK(vec_z_pins(0x15) == 0x15, "no inversion configured");
#endif
}

/* ------------------------------------------------------------------ */
/* (4) Step planner                                                       */
/* ------------------------------------------------------------------ */
static void check_plan(void)
{
    vec_pt pts[VEC_MAX_PTS];
    int n, i, maxstep = 0, ok = 1;

    printf("4. vec_plan: zero length -> 0; 100 at step 16 -> 7 points, exact end, steps <= 16\n");
    n = vec_plan(100, 100, 100, 100, 16, pts, VEC_MAX_PTS);
    CHECK(n == 0, "zero-length: got %d points", n);

    n = vec_plan(1000, 500, 1100, 500, 16, pts, VEC_MAX_PTS);
    CHECK(n == 7, "100 counts at step 16: ceil(100/16) = 7, got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 1100 && pts[n - 1].y == 500,
          "last point must be the endpoint, got (%d,%d)", pts[n - 1].x, pts[n - 1].y);
    {
        int px = 1000;
        for (i = 0; i < n; i++) {
            int d = pts[i].x - px;
            if (d > maxstep) maxstep = d;
            if (pts[i].y != 500) ok = 0;
            px = pts[i].x;
        }
    }
    CHECK(maxstep <= 16 && maxstep > 0, "largest step %d must be 1..16", maxstep);
    CHECK(ok, "a horizontal line stays on its y");

    n = vec_plan(0, 0, 4092, 4092, 16, pts, VEC_MAX_PTS);
    CHECK(n == 256, "full diagonal at step 16: 4092/16 = 255.75 -> 256, got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 4092 && pts[n - 1].y == 4092, "diagonal ends exactly");
    for (i = 0; i < n; i++)
        if (pts[i].x != pts[i].y) ok = 0;
    CHECK(ok, "a 45-degree line keeps x == y");

    n = vec_plan(0, 0, 4092, 0, 1, pts, 10);
    CHECK(n == 10, "max respected: got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 4092, "still ends exactly when max bites");

    n = vec_plan(2000, 2000, 1900, 1950, 16, pts, VEC_MAX_PTS);
    CHECK(n == 7, "negative direction, longer axis 100: 7 points, got %d", n);
    CHECK(n > 0 && pts[n - 1].x == 1900 && pts[n - 1].y == 1950, "negative direction ends exactly");
}

/* ------------------------------------------------------------------ */
/* (5) Ring: silence in -> zeros out; overrun and underrun counted        */
/* ------------------------------------------------------------------ */
static void check_ring(void)
{
    int16_t in[256], out[128];
    int i, allzero = 1;

    printf("5. audio_mix ring: gated start, zeros, overrun/underrun counters\n");
    audio_mix_init();
    memset(in, 0, sizeof in);

    /* below AUDIO_RING_START nothing plays yet */
    audio_mix_push(in, 100);
    CHECK(audio_mix_fill() == 100, "fill after 100 pushed: %u", audio_mix_fill());
    audio_mix_pull(out, 128);
    CHECK(audio_mix_fill() == 100, "gated: pull before START must not drain, fill %u", audio_mix_fill());
    CHECK(audio_mix_underruns() == 0, "gated silence is not an underrun");

    /* fill past START, then drain */
    for (i = 0; i < AUDIO_RING_START / 100 + 1; i++) audio_mix_push(in, 100);
    CHECK(audio_mix_fill() >= AUDIO_RING_START, "fill %u reached START", audio_mix_fill());
    audio_mix_pull(out, 128);
    for (i = 0; i < 128; i++) if (out[i]) allzero = 0;
    CHECK(allzero, "silence in, silence out");
    CHECK(audio_mix_fill() == (uint32_t)(100 + 100 * (AUDIO_RING_START / 100 + 1) - 128),
          "mid band: pull drained exactly 128, fill %u", audio_mix_fill());

    /* overrun: push more than the ring holds */
    for (i = 0; i < AUDIO_RING_SIZE / 256 + 2; i++) audio_mix_push(in, 256);
    CHECK(audio_mix_fill() == AUDIO_RING_SIZE, "ring full at SIZE, fill %u", audio_mix_fill());
    CHECK(audio_mix_overruns() > 0, "dropped frames counted");

    /* underrun: drain everything and then some */
    audio_mix_init();
    for (i = 0; i < AUDIO_RING_START / 100 + 1; i++) audio_mix_push(in, 100);
    for (i = 0; i < AUDIO_RING_SIZE / 128 + 2; i++) audio_mix_pull(out, 128);
    CHECK(audio_mix_fill() == 0, "drained, fill %u", audio_mix_fill());
    CHECK(audio_mix_underruns() > 0, "silence frames counted");
}

/* ------------------------------------------------------------------ */
/* (6) Rate adapter: one duplicate below LOW, one skip above HIGH         */
/* ------------------------------------------------------------------ */
static void check_adapter(void)
{
    int16_t in[AUDIO_RING_SIZE], out[128];
    int i;

    printf("6. audio_mix adapter: dup once per pull below LOW, skip once above HIGH\n");
    for (i = 0; i < AUDIO_RING_SIZE; i++) in[i] = (int16_t)i;   /* a ramp: frame k has value k */

    /* high band: push HIGH+200 frames, pull 128 -> exactly 129 consumed */
    audio_mix_init();
    audio_mix_push(in, AUDIO_RING_HIGH + 200);
    audio_mix_pull(out, 128);
    CHECK(audio_mix_fill() == (uint32_t)(AUDIO_RING_HIGH + 200 - 129),
          "skip: 129 consumed, fill %u", audio_mix_fill());
    CHECK(out[0] == 0 && out[127] == 128, "skip shows as one missing ramp value: out[127]=%d", out[127]);

    /* mid band: exactly 128 consumed */
    audio_mix_init();
    audio_mix_push(in, AUDIO_RING_START + 100);
    audio_mix_pull(out, 128);
    CHECK(audio_mix_fill() == (uint32_t)(AUDIO_RING_START + 100 - 128),
          "mid: 128 consumed, fill %u", audio_mix_fill());
    CHECK(out[127] == 127, "mid: ramp intact, out[127]=%d", out[127]);

    /* low band: start, then drain into the low band; a pull consumes 127 */
    audio_mix_init();
    audio_mix_push(in, AUDIO_RING_START + 10);
    while (audio_mix_fill() > (uint32_t)(AUDIO_RING_LOW + 100)) audio_mix_pull(out, 128);
    while (audio_mix_fill() >= (uint32_t)AUDIO_RING_LOW) audio_mix_pull(out, 128);
    {
        uint32_t before = audio_mix_fill();
        audio_mix_pull(out, 128);
        CHECK(before - audio_mix_fill() == 127, "dup: 127 consumed for 128 out, consumed %u",
              before - audio_mix_fill());
    }
    {
        int dups = 0;
        for (i = 1; i < 128; i++) if (out[i] == out[i - 1]) dups++;
        CHECK(dups == 1, "dup shows as exactly one repeated ramp value, got %d", dups);
    }
}

/* ------------------------------------------------------------------ */
/* (7) Sample mixer: loop wraps, one-shot ends, stop is immediate          */
/* ------------------------------------------------------------------ */
static void check_samples(void)
{
    static const uint8_t s_loop[4] = { 128 + 10, 128 + 20, 128 + 30, 128 + 40 };
    static const uint8_t s_once[3] = { 128 + 1, 128 + 2, 128 + 3 };
    const uint8_t *const data[2] = { s_loop, s_once };
    const uint32_t len[2] = { 4, 3 };
    int16_t in[16];

    printf("7. audio_mix samples: loop wraps, one-shot deactivates, stop immediate, gains\n");
    audio_mix_init();
    audio_mix_set_samples(data, len, 2);
    memset(in, 0, sizeof in);

    audio_mix_sample_start(0, 0, 1);                 /* looping 4-frame sample on channel 0 */
    audio_mix_push(in, 10);
    CHECK(audio_mix_sample_active(0), "loop still active after 10 frames");
    audio_mix_sample_start(1, 1, 0);                 /* one-shot on channel 1 */
    audio_mix_push(in, 5);
    CHECK(!audio_mix_sample_active(1), "one-shot of 3 frames inactive after 5");
    audio_mix_sample_stop(0);
    CHECK(!audio_mix_sample_active(0), "stop is immediate");

    /* the values, one frame at a time, no ring in the way */
    audio_mix_init();
    audio_mix_set_samples(data, len, 2);
    audio_mix_sample_start(0, 0, 1);
    CHECK(audio_mix_next(0) == 10 * AUDIO_SAMPLE_GAIN, "frame 0 = (138-128)*gain");
    CHECK(audio_mix_next(0) == 20 * AUDIO_SAMPLE_GAIN, "frame 1 = 20*gain");
    CHECK(audio_mix_next(0) == 30 * AUDIO_SAMPLE_GAIN, "frame 2 = 30*gain");
    CHECK(audio_mix_next(0) == 40 * AUDIO_SAMPLE_GAIN, "frame 3 = 40*gain");
    CHECK(audio_mix_next(0) == 10 * AUDIO_SAMPLE_GAIN, "frame 4 wrapped to the start");
    audio_mix_sample_stop(0);
    CHECK(audio_mix_next(0) == 0, "stopped channel contributes nothing");

    /* bad ids are ignored */
    audio_mix_sample_start(5, 0, 0);
    audio_mix_sample_start(0, 9, 0);
    CHECK(!audio_mix_sample_active(0), "unknown sample id does not start");
}

/* ------------------------------------------------------------------ */
/* (8) Gains and clamping                                                 */
/* ------------------------------------------------------------------ */
static void check_gain(void)
{
    static const uint8_t s_max[2] = { 255, 0 };
    const uint8_t *const data[1] = { s_max };
    const uint32_t len[1] = { 2 };
    int16_t v;

    printf("8. audio_mix gain: POKEY x AUDIO_POKEY_GAIN/256, sum clamps to int16\n");
    audio_mix_init();
    audio_mix_set_samples(data, len, 1);

    v = audio_mix_next(1024);
    CHECK(v == (int16_t)((1024 * AUDIO_POKEY_GAIN) / 256), "pokey gain: got %d", v);
    v = audio_mix_next(-1024);
    CHECK(v == (int16_t)((-1024 * AUDIO_POKEY_GAIN) / 256), "pokey gain negative: got %d", v);

    audio_mix_sample_start(0, 0, 1);               /* +127*gain then -128*gain */
    v = audio_mix_next(32767);
    CHECK(v == 32767, "positive sum clamps to 32767, got %d", v);
    v = audio_mix_next(-32768);
    CHECK(v == -32768, "negative sum clamps to -32768, got %d", v);
}

int main(void)
{
    check_dac_word();
    check_map();
    check_z();
    check_plan();
    check_ring();
    check_adapter();
    check_samples();
    check_gain();
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
