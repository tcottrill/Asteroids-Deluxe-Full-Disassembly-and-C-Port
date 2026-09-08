/* dvg.c - the Atari DVG hardware model: the 034602-01 state-machine PROM
 * driving a set of latch/strobe handlers over vector RAM/ROM, exactly the
 * way the real board runs a display list, rather than interpreting the
 * list's opcodes directly.
 *
 * This is a plain-C11 transcription of the dvg_device parts of MAME
 * 0.193's src/mame/video/avgdvg.cpp (update_databus, state_addr,
 * handler_0..handler_7, dvg_draw_to, vggo, vgrst, and the state loop in
 * run_state_machine) - the DVG-specific handlers only; this game has no
 * AVG or its game-specific subclasses.  MAME's own header credits that
 * file to:
 *
 *     avgdvg.c: Atari DVG and AVG
 *     license: BSD-3-Clause
 *     copyright-holders: Mathis Rosenhauer
 *     thanks-to: Eric Smith, Brad Oliver, Bernd Wiebelt, Aaron Giles,
 *                Andrew Caldwell
 *
 * MAME's names appear in comments beside each transcribed piece
 * (dvg_dmapush, dvg_data, etc - the names the original PROM/schematic
 * documentation and MAME's own comments use).
 *
 * Timing is counted but not used.  MAME charges 8 master-clock cycles
 * (12.096 MHz) per state and 8 per rate-multiplier tick of a vector
 * (dvg_gostrobe), and makes the HALT flag visible to the CPU only once
 * that many cycles have elapsed after GO.  This port keeps that
 * arithmetic - dvg_gostrobe returns its tick cost, the state loop adds
 * 8 per state, and the count at which HALT became visible is kept in
 * dvg_halt_cycles, readable through dvg_last_cycles() - but the host
 * draws the whole frame at GO and is paced by NMIs, not by the DVG, so
 * nothing consumes it yet; MAME's VGSLICE timeslicing and its halt
 * timer have no equivalent here.  The drawing behaviour is the part
 * that matters: the 12-bit integer position counters stepped by the
 * 7497 bit-rate multipliers, and the hardware blanking on counter
 * bit 10.
 *
 * State (pc, sp, stack, dvx, dvy, op, scale, intensity, xpos, ypos,
 * halt, state_latch, data) persists across calls exactly as it does on
 * the real board between GOs.  The 256x4-bit state PROM
 * (034602-01.c8) is already compiled in as ad_dvgprom (astdelux_rom.c);
 * this file only reads it.
 *
 * dvg_render() is called once per frame from ad_hw_vg_go() (app_win.c)
 * and reproduces go_w(): vggo() (dvy=0, op=0) then halt=0, then runs
 * the state loop until halt becomes 1 again, returning 0.  A runaway
 * list (no HALT opcode ever reached) is caught by a step limit; the
 * frame drawn so far is kept, the state machine is put back through
 * vgrst()+halt=1 so the next frame starts clean, and 2 is returned.
 *
 * Every lit segment goes to plat_video_line in beam space - the
 * hardware blanks any position with bit 10 set, so x and y run
 * 0..1023 - z = the intensity nibble 0..15.
 */
#include "astdelux.h"
#include "platform/ad_platform.h"

#define DVG_MAX_STEPS 200000

#define DVG_OP0(op)   ((op) & 1)
#define DVG_OP3(op)   ((op) & 8)
#define DVG_ST3(sl)   ((sl) & 8)

/* ---- persistent state (survives across dvg_render() calls, exactly
 * like the hardware between GOs) -------------------------------------- */
static struct {
    uint16_t pc;            /* m_pc     - 12-bit word address into vector RAM/ROM */
    uint8_t  sp;             /* m_sp     - 4-bit return-address stack pointer */
    uint16_t stack[4];       /* m_stack  - the 4-deep JSRL/RTSL return stack */
    uint16_t dvx;             /* m_dvx    - X delta/position latch, 12 bit */
    uint16_t dvy;             /* m_dvy    - Y delta/position latch, 12 bit */
    uint8_t  op;              /* m_op     - the 4-bit opcode nibble from latch1 */
    uint8_t  scale;           /* m_scale  - persistent vector scale */
    uint8_t  intensity;       /* m_intensity - persistent beam intensity, 0-15 */
    uint16_t xpos;            /* m_xpos   - X beam position counter, 12 bit */
    uint16_t ypos;            /* m_ypos   - Y beam position counter, 12 bit */
    uint8_t  halt;            /* m_halt   - HALT flag from haltstrobe (OP0) */
    uint8_t  state_latch;     /* m_state_latch - the PROM's latched output */
    uint8_t  data;            /* m_data   - the byte last read off the data bus */
} dv;

static int dvg_inited;

/* the "last point" the beam is at, for dvg_draw_to's line emission -
 * reset at the start of every dvg_render() call (not persistent). */
static int last_x, last_y, have_last;

/* Master-clock cycles from GO to the point HALT became visible on the
 * last dvg_render() (MAME: the halt timer's delay, measured from the
 * start of the slice - here the whole frame is one slice).  0 after a
 * runaway list.  Counted, exposed by dvg_last_cycles(), not consumed. */
static unsigned long dvg_halt_cycles;

static void dvg_update_databus(void);
static uint8_t dvg_state_addr(void);
static void dvg_draw_to(int x, int y, int intensity);
static void dvg_handler_0(void);
static void dvg_handler_1(void);
static int  dvg_handler_2(void);
static void dvg_handler_3(void);
static void dvg_handler_4(void);
static void dvg_handler_5(void);
static void dvg_handler_6(void);
static void dvg_handler_7(void);
static void dvg_vggo(void);
static void dvg_vgrst(void);

/* dvg_data (update_databus): "DVG uses low bit of state for address."
 * The unified $4000-$5FFF vector RAM/ROM address is 0x4000 +
 * ((pc<<1) | (state_latch&1)); below $4800 it's vector RAM
 * (g.vram), at/above $4800 it's the vector ROM (ad_rom(), populated
 * to $57FF on this board; ad_rom() reads 0 above that). MAME reads
 * both out of one contiguous "vectorram" share; this port's g.vram and
 * ad_rom() together are that same contiguous range. */
static void dvg_update_databus(void)
{
    uint16_t addr = (uint16_t)(0x4000u + (((uint32_t)dv.pc << 1) | (dv.state_latch & 1u)));

    if (addr < 0x4800u)
        dv.data = g.vram[addr - 0x4000u];
    else
        dv.data = ad_rom(addr);
}

/* dvg_state_addr: the PROM address is the latched state (with its top
 * "next-half-select" bit inverted) OR'd with the opcode nibble when
 * OP3 (the state machine's own opcode-decode enable) is set. */
static uint8_t dvg_state_addr(void)
{
    uint8_t addr = (uint8_t)(((((dv.state_latch >> 4) ^ 1) & 1) << 7) | (dv.state_latch & 0xfu));

    if (DVG_OP3(dv.op))
        addr |= (uint8_t)((dv.op & 7) << 4);

    return addr;
}

/* dvg_dmapush (handler_0): push pc onto the 4-deep return stack unless
 * OP0 is set (OP0 set means this state cycle is really feeding dmald). */
static void dvg_handler_0(void)
{
    if (DVG_OP0(dv.op) == 0)
    {
        dv.sp = (uint8_t)((dv.sp + 1) & 0xf);
        dv.stack[dv.sp & 3] = dv.pc;
    }
}

/* dvg_dmald (handler_1): OP0 set pops the return stack (RTSL); clear
 * loads pc from dvy (JSRL/JMPL's target, assembled via latch0/latch1). */
static void dvg_handler_1(void)
{
    if (DVG_OP0(dv.op))
    {
        dv.pc = dv.stack[dv.sp & 3];
        dv.sp = (uint8_t)((dv.sp - 1) & 0xf);
    }
    else
    {
        dv.pc = (uint16_t)(dv.dvy & 0xfffu);
    }
}

/* dvg_draw_to: MAME adds a point to the vector list only when neither
 * coordinate's bit 10 is set (((x|y)&0x400)==0 - the hardware blanks
 * the beam past that bit), and the vector device draws from the
 * previous added point to the new one at the new point's intensity (0
 * = blank move, i.e. no segment drawn but the position still moves).
 * We reproduce that with a "last point" instead of MAME's point list:
 * a point outside the window is dropped entirely (as it is on real
 * hardware - blanked, never latched as a new "last" position either,
 * matching vector_device's own add_point skip); otherwise, if this
 * point is lit and there was a prior point, emit the segment between
 * them, then remember this point as the new "last" one. */
static void dvg_draw_to(int x, int y, int intensity)
{
    if (((x | y) & 0x400) != 0)
        return;

    if (intensity > 0 && have_last)
        plat_video_line((float)last_x, (float)last_y, (float)x, (float)y, intensity);

    last_x = x;
    last_y = y;
    have_last = 1;
}

/* dvg_gostrobe (handler_2): the beam-deflection state. Computes this
 * vector's scale, then the 7497 bit-rate-multiplier tick count (fin)
 * and per-axis direction (dx/dy), then steps a 12-bit tick counter c
 * once per output pulse, checking each axis' multiplier (mx/my)
 * against c to decide whether that axis counts this tick - exactly
 * like the two cascaded 7497s do - clipping/unclipping through
 * dvg_draw_to whenever a counter's bit 10 changes.
 *
 * MAME's inner loop checks, for bit 0..11, whether
 * (c & ((2<<bit)-1)) == ((1<<bit)-1); this is true for exactly one
 * bit per value of c, namely bit = the number of trailing one-bits of
 * c (c ends in `bit` ones followed by a zero at position `bit`).
 * Since c increments by 1 each tick, that "trailing ones of c" is the
 * same as "trailing zeros of c+1" (incrementing c turns a run of low
 * ones into zeros and sets the next bit) - the classic ruler
 * sequence - so we compute it in O(1) (bounded by 12 shifts) per tick
 * instead of scanning all 12 bit positions. If that trailing-ones
 * count exceeds 11 (only true for c==0xfff), no bit in 0..11 matches,
 * so neither axis counts that tick, matching the original loop. */
static int dvg_trailing_ones(unsigned c)
{
    unsigned n = (c + 1u) & 0x1fffu;   /* 0x001..0x1000: never zero */
    int t = 0;

    while ((n & 1u) == 0u)
    {
        n >>= 1;
        t++;
    }
    return t;
}

static int dvg_handler_2(void)
{
    int scale, fin, dx, dy, c, mx, my, t, cycles;

    if (dv.op == 0xf)
    {
        scale = (dv.scale +
                    (((dv.dvy & 0x800) >> 11)
                    | (((dv.dvx & 0x800) ^ 0x800) >> 10)
                    | ((dv.dvx & 0x800)  >> 9))) & 0xf;

        dv.dvy &= 0xf00;
        dv.dvx &= 0xf00;
    }
    else
    {
        scale = (dv.scale + dv.op) & 0xf;
    }

    fin = 0xfff - (((2 << scale) & 0x7ff) ^ 0xfff);

    /* Count up or down */
    dx = (dv.dvx & 0x400) ? -1 : +1;
    dy = (dv.dvy & 0x400) ? -1 : +1;

    /* Scale factor for rate multipliers */
    mx = (dv.dvx << 2) & 0xfff;
    my = (dv.dvy << 2) & 0xfff;

    /* MAME: cycles = 8 * fin - eight master clocks per multiplier tick,
     * i.e. the 1.512 MHz vector clock, whatever the vector's length. */
    cycles = 8 * fin;
    c = 0;

    while (fin--)
    {
        int countx, county;

        t = dvg_trailing_ones((unsigned)c);
        countx = (t <= 11) && ((mx >> (11 - t)) & 1);
        county = (t <= 11) && ((my >> (11 - t)) & 1);

        c = (c + 1) & 0xfff;

        /*
         *  Since x- and y-counters always hold the correct count
         *  wrt. to each other, we can do clipping exactly like the
         *  hardware does. That is, as soon as any counter's bit 10
         *  changes to high, we finish the vector. If bit 10 changes
         *  from high to low, we start a new vector.
         */

        if (countx)
        {
            /* Is y valid and x entering or leaving the valid range? */
            if (((dv.ypos & 0x400) == 0)
                && ((dv.xpos ^ (dv.xpos + dx)) & 0x400))
            {
                if ((dv.xpos + dx) & 0x400)
                    /* We are leaving the valid range */
                    dvg_draw_to(dv.xpos, dv.ypos, dv.intensity);
                else
                    /* We are entering the valid range */
                    dvg_draw_to((dv.xpos + dx) & 0xfff, dv.ypos, 0);
            }
            dv.xpos = (uint16_t)((dv.xpos + dx) & 0xfff);
        }

        if (county)
        {
            if (((dv.xpos & 0x400) == 0)
                && ((dv.ypos ^ (dv.ypos + dy)) & 0x400))
            {
                if ((dv.xpos & 0x400) == 0)
                {
                    if ((dv.ypos + dy) & 0x400)
                        dvg_draw_to(dv.xpos, dv.ypos, dv.intensity);
                    else
                        dvg_draw_to(dv.xpos, (dv.ypos + dy) & 0xfff, 0);
                }
            }
            dv.ypos = (uint16_t)((dv.ypos + dy) & 0xfff);
        }
    }

    dvg_draw_to(dv.xpos, dv.ypos, dv.intensity);

    return cycles;
}

/* dvg_haltstrobe (handler_3): OP0 is the HALT flag itself (HALT is
 * 0xb); clear means LABS (0xa): load xpos/ypos from dvx/dvy and do a
 * blank move there. */
static void dvg_handler_3(void)
{
    dv.halt = (uint8_t)DVG_OP0(dv.op);

    if (DVG_OP0(dv.op) == 0)
    {
        dv.xpos = (uint16_t)(dv.dvx & 0xfff);
        dv.ypos = (uint16_t)(dv.dvy & 0xfff);
        dvg_draw_to(dv.xpos, dv.ypos, 0);
    }
}

/* dvg_latch0 (handler_4): low byte of dvy, unless op==0xf (SVEC, the
 * two-byte short vector of VECMAC.XX), whose low byte carries the dvx
 * top nibble and the intensity instead, so this state feeds latch3
 * (handler_7). Always advances pc. */
static void dvg_handler_4(void)
{
    dv.dvy &= 0xf00;
    if (dv.op == 0xf)
        dvg_handler_7();
    else
        dv.dvy = (uint16_t)((dv.dvy & 0xf00) | dv.data);

    dv.pc = (uint16_t)((dv.pc + 1) & 0xfffu);
}

/* dvg_latch1 (handler_5): top nibble of dvy plus the opcode nibble;
 * op==0xf (SVEC) clears both dvx/dvy down to their top nibble - the
 * short vector's 2-bit magnitudes live in bits 8-9. */
static void dvg_handler_5(void)
{
    dv.dvy = (uint16_t)((dv.dvy & 0xff) | ((dv.data & 0xf) << 8));
    dv.op = (uint8_t)(dv.data >> 4);

    if (dv.op == 0xf)
    {
        dv.dvx &= 0xf00;
        dv.dvy &= 0xf00;
    }
}

/* dvg_latch2 (handler_6): low byte of dvx, unless op==0xf; op bits 1
 * and 3 both set (LABS is 0xa) also latches the persistent scale from
 * the nibble latch3 just stored as "intensity" (LABS's second word
 * carries the scale where a VCTR carries the intensity). Always
 * advances pc. */
static void dvg_handler_6(void)
{
    dv.dvx &= 0xf00;
    if (dv.op != 0xf)
        dv.dvx = (uint16_t)((dv.dvx & 0xf00) | dv.data);

    if ((dv.op & 0xa) == 0xa)
        dv.scale = dv.intensity;

    dv.pc = (uint16_t)((dv.pc + 1) & 0xfffu);
}

/* dvg_latch3 (handler_7): top nibble of dvx plus the intensity nibble
 * (the scale nibble for LABS, see latch2). Reached as its own state or
 * through latch0's SVEC redirect. */
static void dvg_handler_7(void)
{
    dv.dvx = (uint16_t)((dv.dvx & 0xff) | ((dv.data & 0xf) << 8));
    dv.intensity = (uint8_t)(dv.data >> 4);
}

/* dvg_vggo: what GO does to the latches - clears dvy/op only. */
static void dvg_vggo(void)
{
    dv.dvy = 0;
    dv.op = 0;
}

/* dvg_vgrst: what RESET does - clears the state latch too. */
static void dvg_vgrst(void)
{
    dv.state_latch = 0;
    dv.dvy = 0;
    dv.op = 0;
}

/* dvg_render(): called once per frame from ad_hw_vg_go() (app_win.c).
 * First call ever: reproduces power-on reset_w (vgrst() + halt=1).
 * Every call: reproduces go_w() (vggo(); halt=0) and then the PROM
 * state loop (run_state_machine) until halt becomes 1 again. The
 * cycle arithmetic is MAME's: 8 master clocks per state plus what
 * dvg_gostrobe charges per vector; the running total at the state
 * that raised HALT is when MAME's halt timer would make the flag
 * visible, kept in dvg_halt_cycles. A runaway list is caught by a
 * step limit; the state machine is put back through vgrst()+halt=1 so
 * the next frame starts clean, and the frame drawn so far is kept. */
int dvg_render(void)
{
    int steps = 0;
    unsigned long cycles = 0;

    if (!dvg_inited)
    {
        dvg_vgrst();
        dv.halt = 1;
        dvg_inited = 1;
    }

    last_x = 0;
    last_y = 0;
    have_last = 0;
    dvg_halt_cycles = 0;

    dvg_vggo();
    dv.halt = 0;

    for (;;)
    {
        if (++steps > DVG_MAX_STEPS)
        {
            dvg_vgrst();
            dv.halt = 1;
            return 2;
        }

        /* Get next state */
        dv.state_latch = (uint8_t)((dv.state_latch & 0x10) | (ad_dvgprom[dvg_state_addr()] & 0xf));

        if (DVG_ST3(dv.state_latch))
        {
            /* Read vector RAM/ROM */
            dvg_update_databus();

            /* Decode state and call the corresponding handler */
            switch (dv.state_latch & 7)
            {
                case 0: dvg_handler_0(); break;
                case 1: dvg_handler_1(); break;
                case 2: cycles += (unsigned long)dvg_handler_2(); break;
                case 3: dvg_handler_3(); break;
                case 4: dvg_handler_4(); break;
                case 5: dvg_handler_5(); break;
                case 6: dvg_handler_6(); break;
                case 7: dvg_handler_7(); break;
            }
        }

        /* MAME: "If halt flag was set, let CPU catch up before we make
         * halt visible" - vg_halt_timer fires `cycles` master clocks
         * into the slice. Recorded instead of scheduled. */
        if (dv.halt && !(dv.state_latch & 0x10))
            dvg_halt_cycles = cycles;

        dv.state_latch = (uint8_t)((dv.halt << 4) | (dv.state_latch & 0xf));
        cycles += 8;

        if (dv.halt)
            break;
    }

    return 0;
}

/* Master-clock (12.096 MHz) cycles the last dvg_render() list took from
 * GO to HALT becoming visible; divide by 12096000 for seconds.
 * Purely informational: the host paces on NMIs and draws at GO. */
unsigned long dvg_last_cycles(void)
{
    return dvg_halt_cycles;
}
