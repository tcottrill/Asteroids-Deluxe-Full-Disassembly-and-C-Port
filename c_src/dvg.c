/* dvg.c - walk the display list in vector RAM the way the DVG would.
 *
 * The game builds a real display list in g.vram ($4000-$47FF) and the
 * hardware runs it from word 0 after GO; subroutines (the shapes) live
 * in the vector ROM at $4800-$57FF, read here through ad_rom().  This
 * is a transcription of disasm/vramview.py's `run` and `delta`, which
 * follow DSTVEC.MAC's opcode set and the length rule the MAME DVG uses:
 * a VCTR or SVEC draws magnitude * 2^((op + LABS scale) - 9), a sum
 * above 9 wrapping to a shift of 10.  The PROM state machine matters
 * only for timing, which this host does not model yet.
 *
 * Every lit segment goes to plat_video_line in beam space, y up.
 */
#include "astdelux.h"
#include "platform/ad_platform.h"

#define DVG_MAX_STEPS 20000
#define DVG_MAX_DEPTH 8

static uint16_t fetch(uint16_t a)
{
    if (a >= 0x4000u && a < 0x4000u + AD_VRAM_SIZE)
        return (uint16_t)(g.vram[a - 0x4000u] | (g.vram[a + 1 - 0x4000u] << 8));
    return ad_rom16(a);
}

/* A VCTR (w1 = dy word, w2 = dx/intensity word) or SVEC (w1 only). */
static void delta(uint16_t w1, uint16_t w2, int scale, float *dx, float *dy, int *z)
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
    shift = s > 9 ? 10 : 9 - s;
    *dx = (float)ix / (float)(1 << shift);
    *dy = (float)iy / (float)(1 << shift);
}

/* Run the list from word 0.  Returns 0 on a clean HALT, else a small
 * code saying what went wrong (the frame drawn so far is kept). */
int dvg_render(void)
{
    float x = 0.0f, y = 0.0f;
    int scale = 0, steps = 0, sp = 0;
    uint16_t a = 0x4000;
    uint16_t stack[DVG_MAX_DEPTH];

    for (;;) {
        uint16_t w1, w2 = 0;
        int op;

        if (a < 0x4000u || a >= 0x5800u || (a & 1))
            return 1;                                   /* fetch outside RAM+ROM */
        if (++steps > DVG_MAX_STEPS)
            return 2;                                   /* runaway, no HALT */
        w1 = fetch(a);
        op = w1 >> 12;
        if (op <= 9) {                                  /* VCTR, 4 bytes */
            float dx, dy;
            int z;
            w2 = fetch((uint16_t)(a + 2));
            delta(w1, w2, scale, &dx, &dy, &z);
            if (z)
                plat_video_line(x, y, x + dx, y + dy, z);
            x += dx; y += dy;
            a = (uint16_t)(a + 4);
        } else if (op == 0xF) {                         /* SVEC, 2 bytes */
            float dx, dy;
            int z;
            delta(w1, 0, scale, &dx, &dy, &z);
            if (z)
                plat_video_line(x, y, x + dx, y + dy, z);
            x += dx; y += dy;
            a = (uint16_t)(a + 2);
        } else if (op == 0xA) {                         /* LABS */
            w2 = fetch((uint16_t)(a + 2));
            x = (float)(w2 & 0x3FF);
            y = (float)(w1 & 0x3FF);
            scale = w2 >> 12;
            a = (uint16_t)(a + 4);
        } else if (op == 0xB) {                         /* HALT */
            return 0;
        } else if (op == 0xC) {                         /* JSRL */
            if (sp >= DVG_MAX_DEPTH)
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
