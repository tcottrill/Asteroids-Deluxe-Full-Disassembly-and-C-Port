/* dvg_ref.c - the pre-state-machine float walker, kept as the reference
 * implementation for dvg_test.c's differential test (the DVG
 * state-machine backport's Task 2).  This is c_src/dvg.c verbatim as it
 * stood before that port (git show 7e7696d~1:c_src/dvg.c), with
 * dvg_render() renamed dvg_ref_render() and its static helpers given a
 * ref_ prefix so both can link into the same test binary alongside the
 * new PROM-driven dvg.c.
 *
 * It still walks the display list in vector RAM the way the DVG would,
 * at the opcode level rather than the state-machine level: the game
 * builds a real display list in g.vram ($4000-$47FF) and the hardware
 * runs it from word 0 after GO; subroutines (the shapes) live in the
 * vector ROM at $4800-$57FF, read here through ad_rom().  This is a
 * transcription of disasm/vramview.py's `run` and `delta`, which follow
 * DSTVEC.MAC's opcode set and the length rule the MAME DVG uses: a VCTR
 * or SVEC draws magnitude * 2^((op + LABS scale) - 9), a sum above 9
 * wrapping to a shift of 10.  The PROM state machine matters only for
 * timing, which this host does not model.
 *
 * LABS's x/y fields are masked to 10 bits here (see the LABS case
 * below), exactly as the pre-backport dvg.c had it - NOT sign-extended
 * to the hardware's full 12-bit two's-complement position the way the
 * new dvg.c's dv.dvx/dv.dvy are.  That mask is left untouched on
 * purpose: the whole point of dvg_test.c's differential test is to find
 * out whether a LABS with bit 10 or 11 set (which this walker silently
 * folds into 0..1023, but the hardware's 12-bit counters and bit-10
 * blanking treat very differently) ever actually appears in this game's
 * display lists, and if so, how the two models disagree about it.
 *
 * Every lit segment goes to plat_video_line in beam space, y up.
 */
#include "../astdelux.h"
#include "../platform/ad_platform.h"

#define DVG_REF_MAX_STEPS 20000
#define DVG_REF_MAX_DEPTH 8

/* Instrumentation for dvg_test.c's differential-test report (Task 2's
 * two open questions, see dvg_test.c's header): whether this ROM's
 * display lists ever hit either of the two spots where this walker and
 * the new dvg.c state machine are known to have no shared ground at
 * all, as opposed to the ordinary rounding/edge disagreements the test
 * tolerates elsewhere.
 *
 * dvg_ref_scale_ge10_count: a VCTR/SVEC whose combined scale s =
 * (scale + op) & 0xF reaches 10 or above.  dvg.c's dvg_handler_2 computes
 * fin = 0xfff - (((2 << s) & 0x7ff) ^ 0xfff), which is exactly 0 once
 * s >= 10 (the rate multiplier never ticks, so the hardware draws
 * nothing for that vector), while this walker's shift = s > 9 ? 10 :
 * 9 - s still draws it, just clamped to 1/1024 scale - not the same
 * disagreement as ordinary rounding drift, so it would show up as an
 * hw segment simply missing.  Not expected in this ROM's own shapes;
 * kept as a live check rather than trusting that.
 *
 * dvg_ref_labs_hibits_count: a LABS word (w1 = y word, w2 = x word)
 * with bit 10 or 11 set.  This walker masks LABS x/y to 10 bits (see
 * this file's header) exactly as the pre-backport dvg.c did, while the
 * new dvg.c loads the hardware's real 12-bit two's-complement position
 * (dvg_handler_3: dv.xpos = dv.dvx & 0xfff) - a LABS that actually uses
 * those bits would put the beam somewhere entirely different on each
 * side, not a bounded drift.
 *
 * Both record the first occurrence's address and words for follow-up;
 * dvg_test.c checks the counts after every frame and reports which
 * dump/frame it happened on if either ever moves. */
int dvg_ref_scale_ge10_count = 0;
uint16_t dvg_ref_scale_ge10_addr = 0, dvg_ref_scale_ge10_w1 = 0, dvg_ref_scale_ge10_w2 = 0;
int dvg_ref_labs_hibits_count = 0;
uint16_t dvg_ref_labs_hibits_addr = 0, dvg_ref_labs_hibits_w1 = 0, dvg_ref_labs_hibits_w2 = 0;

static uint16_t ref_fetch(uint16_t a)
{
    if (a >= 0x4000u && a < 0x4000u + AD_VRAM_SIZE)
        return (uint16_t)(g.vram[a - 0x4000u] | (g.vram[a + 1 - 0x4000u] << 8));
    return ad_rom16(a);
}

/* A VCTR (w1 = dy word, w2 = dx/intensity word) or SVEC (w1 only). */
static void ref_delta(uint16_t addr, uint16_t w1, uint16_t w2, int scale, float *dx, float *dy, int *z)
{
    int op = w1 >> 12, s, shift;
    int ix, iy;

    if (op == 0xF) {
        iy = ((w1 >> 8) & 3) << 8;
        ix = (w1 & 3) << 8;
        if (w1 & 0x400) iy = -iy;
        if (w1 & 0x004) ix = -ix;
        s = 2 + ((w1 >> 2) & 2) + ((w1 >> 11) & 1);
        *z = (w1 >> 4) & 0xF;
    } else {
        iy = w1 & 0x3FF;
        ix = w2 & 0x3FF;
        if (w1 & 0x400) iy = -iy;
        if (w2 & 0x400) ix = -ix;
        s = op;
        *z = w2 >> 12;
    }
    s = (s + scale) & 0xF;
    if (s >= 10) {
        if (dvg_ref_scale_ge10_count == 0) {
            dvg_ref_scale_ge10_addr = addr;
            dvg_ref_scale_ge10_w1 = w1;
            dvg_ref_scale_ge10_w2 = w2;
        }
        dvg_ref_scale_ge10_count++;
    }
    shift = s > 9 ? 10 : 9 - s;
    *dx = (float)ix / (float)(1 << shift);
    *dy = (float)iy / (float)(1 << shift);
}

/* Run the list from word 0.  Returns 0 on a clean HALT, else a small
 * code saying what went wrong (the frame drawn so far is kept). */
int dvg_ref_render(void)
{
    float x = 0.0f, y = 0.0f;
    int scale = 0, steps = 0, sp = 0;
    uint16_t a = 0x4000;
    uint16_t stack[DVG_REF_MAX_DEPTH];

    for (;;) {
        uint16_t w1, w2 = 0;
        int op;

        if (a < 0x4000u || a >= 0x5800u || (a & 1))
            return 1;                                   /* fetch outside RAM+ROM */
        if (++steps > DVG_REF_MAX_STEPS)
            return 2;                                   /* runaway, no HALT */
        w1 = ref_fetch(a);
        op = w1 >> 12;
        if (op <= 9) {                                  /* VCTR, 4 bytes */
            float dx, dy;
            int z;
            w2 = ref_fetch((uint16_t)(a + 2));
            ref_delta(a, w1, w2, scale, &dx, &dy, &z);
            if (z)
                plat_video_line(x, y, x + dx, y + dy, z);
            x += dx; y += dy;
            a = (uint16_t)(a + 4);
        } else if (op == 0xF) {                         /* SVEC, 2 bytes */
            float dx, dy;
            int z;
            ref_delta(a, w1, 0, scale, &dx, &dy, &z);
            if (z)
                plat_video_line(x, y, x + dx, y + dy, z);
            x += dx; y += dy;
            a = (uint16_t)(a + 2);
        } else if (op == 0xA) {                         /* LABS */
            w2 = ref_fetch((uint16_t)(a + 2));
            if ((w1 & 0xC00) || (w2 & 0xC00)) {
                if (dvg_ref_labs_hibits_count == 0) {
                    dvg_ref_labs_hibits_addr = a;
                    dvg_ref_labs_hibits_w1 = w1;
                    dvg_ref_labs_hibits_w2 = w2;
                }
                dvg_ref_labs_hibits_count++;
            }
            x = (float)(w2 & 0x3FF);
            y = (float)(w1 & 0x3FF);
            scale = w2 >> 12;
            a = (uint16_t)(a + 4);
        } else if (op == 0xB) {                         /* HALT */
            return 0;
        } else if (op == 0xC) {                         /* JSRL */
            if (sp >= DVG_REF_MAX_DEPTH)
                return 3;
            stack[sp++] = (uint16_t)(a + 2);
            a = (uint16_t)(0x4000u + ((w1 & 0x1FFF) << 1));
        } else if (op == 0xD) {                         /* RTSL */
            if (sp == 0)
                return 4;
            a = stack[--sp];
        } else {                                        /* JMPL */
            a = (uint16_t)(0x4000u + ((w1 & 0x1FFF) << 1));
        }
    }
}
