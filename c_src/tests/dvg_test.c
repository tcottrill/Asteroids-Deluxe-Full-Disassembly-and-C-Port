/* dvg_test.c - standalone regression/differential test for dvg.c, the
 * Atari DVG state-machine model, ported from the Lunar Lander C port's
 * tests/dvg_test.c (see that file's header for the fuller rationale;
 * this comment covers what is the same and what had to change for this
 * game's own content).
 *
 * Links against ..\dvg.c (the new PROM-driven state machine) and
 * ..\astdelux_rom.c, plus this directory's dvg_ref.c (dvg_ref_render():
 * the pre-state-machine float walker, kept only as a reference for the
 * differential test below) and this file's own stubs - no other core
 * module needed.  `ad_state g` (astdelux.h) is defined right here
 * rather than pulled in from mainline.c, which drags in the whole game
 * (every module) just to get that one line - exactly the reason Lunar
 * Lander's dvg_test.c defines `ll_state g` itself instead of linking
 * llander's own mainline equivalent.  Both dvg_render() (the "hardware"
 * list) and dvg_ref_render() (the "reference" list) draw through
 * plat_video_line(); that function here records into whichever of two
 * segment arrays the global g_sink selects, instead of drawing
 * anything.
 *
 * Dump format (host_stub.c's --dumpall): each dump is a sequence of
 * 2304-byte records, one per DVG kick (ad_hw_vg_go()) - 256 bytes of
 * zero page (g.zp.raw) then 2048 bytes of vector RAM ($4000-$47FF).
 * Frame N's vector RAM starts at byte offset N*2304+256.  load_frame()
 * returns 1 on a full read, 0 on EOF (short/absent read - the normal
 * way a dump runs out of frames), and -1 on a real error (can't
 * open/seek the file).
 *
 * Test 1 (tests/attract.dump, frame 400): pins the display field on
 * the new hardware-model walker (dvg_render()), using THIS game's own
 * content - the two things score.c always puts on screen in attract
 * mode (ZP.NPLAYR == 0): ad_params()'s top-of-screen score digits
 * (score.c line ~194, positioned at ad_vgsabs(0x19,0xDB) = beam
 * (100,876)) and ad_cpyrs()'s copyright line, drawn from SCORES_90
 * (score.c's ad_scores(), the "no high scores yet" branch that always
 * fires on a fresh EAROM) at ad_vgsabs(0x70,0x68) = beam (448,416).
 * Unlike Lunar Lander's static terrain, this game's attract-mode
 * "demo" keeps the full rock simulation running in the background
 * (mainline.c's START2_15 comment: "these run in attract mode too,
 * which is what makes the attract screen a real game with no ship"),
 * and the rocks roam the *entire* 0..1023 field - checked directly: no
 * frame in a 600-frame attract.dump ever has the whole list's global
 * min y sit anywhere near the copyright text's, because some rock is
 * reliably lower, and by frame 128 a rock has already gone above the
 * score digits' own top edge too. So, unlike Lunar Lander's test 1,
 * this one can't pin the *global* max/min of the field-clipped list -
 * it pins two known, ROM-fixed segments directly: the top edge of
 * score.c's own "0" digit box (still all zeros this early - a fresh
 * EAROM/game) and the leftmost stroke of the copyright text, both
 * exact endpoints read off frame 400's actual dvg_render() output.
 * That is still exactly the same kind of check Lunar Lander's test 2
 * makes (a specific, verified segment identity) applied to test 1's
 * purpose (pinning the field on real ROM content) instead of test 2's
 * (a segment that must NOT appear).
 *
 * Test 2 (tests/play.dump, frame 1000): pins the hardware bit-10
 * blanking on the new walker.  Every endpoint dvg_render() emits must
 * satisfy 0 <= x <= 1023 and 0 <= y <= 1023, and the list must be
 * non-empty.  (This game's dvg.c has no separate off-screen
 * sign-extension bug of its own to probe for the way Lunar Lander's
 * old dvg_test did - dvg_render() only ever emits in-window
 * coordinates by construction (dvg_draw_to's bit-10 check) - so this
 * is a plain regression pin, not a negative check for a specific
 * segment.)
 *
 * Test 3 (differential, the important one): for every frame of
 * attract.dump, play.dump, play_noshield.dump and stest.dump, run
 * dvg_ref_render() and dvg_render() on the same vector RAM.
 * Liang-Barsky-clip every reference segment to the window [0,1024) x
 * [0,1024) (a segment entirely outside is dropped); a reference
 * segment that was NOT already a zero-length "dot" but clips down to
 * one exactly on the window boundary is also dropped - that's a real
 * artifact of continuous-space clipping meeting the hardware's integer
 * bit-10 blank, which never happens on the hardware side either (see
 * build_clipped_ref()).
 *
 * What survives is walked against dvg_render()'s list in lockstep
 * (see the matching loop in diff_one_dump()): same z, each endpoint
 * within TOL_XY units, in order.  There is exactly one other place the
 * two models are expected to disagree, and it runs the other way: a
 * vector whose start position has wrapped through the 12-bit counter's
 * blanked band (e.g. a LABS just off the left edge, x a small negative
 * two's-complement value) and crosses into the visible field partway
 * through.  dvg_render()'s bit-10 tracking (dvg_gostrobe, "entering the
 * valid range") draws that as two pieces: a silent (intensity 0, so it
 * never reaches plat_video_line) move to the entry point, then a real,
 * separately-recorded segment from there to the vector's endpoint -
 * exactly what the hardware's beam does.  dvg_ref_render() has no
 * concept of beam-position blanking at all (see its own header
 * comment) and only ever emits the whole vector as one segment from
 * its own unclipped, unwrapped running position, so it never produces
 * that split.  The result is hw carries one extra segment right before
 * a real match; if it starts exactly on the window's edge (0 or 1023)
 * and the very next hw segment is the one that matches ref, that is
 * this documented case and is skipped (counted as an "entry stub"),
 * not treated as a failure.  Any other mismatch - wrong z, endpoint
 * deviation over TOL_XY, or a count/order mismatch that doesn't fit
 * either sanctioned pattern - is a FAIL that prints the dump name,
 * frame, and both lists around the divergence, and stops the test
 * right there rather than silently continuing.
 *
 * Stub preference.  An entry stub (above) is a boundary-start hw
 * segment whose successor fits the reference better than the stub
 * itself does - it is the blank move to the window edge, not the real
 * vector.  Rule A's TOL_XY is widened to 8.0 units to cover this ROM's
 * own rock-shape chains (its own paragraph below), and that is wide
 * enough that a stub can coincidentally satisfy seg_matches() against
 * the *current* ref segment on its own, purely by accident of position
 * - in which case checking the primary match first would accept the
 * stub as if it were the real match and leave hw permanently one
 * segment ahead of ref for the rest of the frame.  So the lockstep loop
 * tests for a stub *before* the primary match, not as a fallback after
 * it fails: at (ref[i], hw[j]), if hw[j] starts exactly on the window
 * boundary (x0 or y0 is 0 or 1023 - hw's beam position is always an
 * exact integer, so this is an exact comparison, not a tolerance) and
 * hw[j+1] exists, matches ref[i] outright, and fits it strictly better
 * (smaller seg_dev) than hw[j] does, then hw[j] is a stub: count it,
 * advance j, and run the normal primary check against hw[j+1] instead.
 * The "matches outright" part matters: without it a stub whose
 * neighbours are both far from ref[i] (a rock leaving the window for
 * several vectors and re-entering) gets "preferred" on garbage
 * comparisons and the lists desync.  The same principle governs Rule
 * B's edge drops below: a skip is only ever taken when it resyncs the
 * two lists within a short run.  The failure-path
 * entry-stub handling in the resync-run loop below is unchanged and
 * still needed for the case this preference check doesn't catch (the
 * primary match fails outright rather than merely being outscored).
 *
 * dvg_ref.c keeps the pre-backport dvg.c's LABS mask exactly as it
 * was: 10 bits (w2 & 0x3FF / w1 & 0x3FF), not the new dvg.c's full
 * 12-bit two's-complement dv.dvx/dv.dvy.  That is deliberate (see
 * dvg_ref.c's header) - it is the one place the two walkers can
 * legitimately part ways for a reason that is NOT the entry-stub/
 * wrap-dot pair above: a LABS whose 6502-side word has bit 10 or 11
 * set.  This game's own display lists are checked for that below
 * (dvg_ref_labs_hibits_count, dvg_ref.c), and reported by main() at
 * the end of the run - not expected to ever fire, so it's a live
 * check rather than a silent assumption.
 *
 * Two more disagreements are expected everywhere in every dump, not
 * just at isolated boundary crossings, and get their own rules rather
 * than per-case tolerances:
 *
 * Rule A (rate-multiplier rounding drift, endpoint + shape tolerance).
 * For a vector of magnitude ix (10 bits) at combined scale s = (scale +
 * op) & 0xF, dvg_ref_render() moves the axis ix * 2^(s-9) exactly, in
 * floats.  The real hardware (dvg_handler_2) runs fin = 2^(s+1) rate-
 * multiplier ticks and steps the axis at tick c when bit (11 - t) of
 * mx = ix << 2 is set, t = trailing_ones(c).  Over the 2^(s+1) ticks, a
 * given t = k occurs 2^(s-k) times for k = 0..s, and t = s+1 occurs
 * once (the very last tick, c = fin-1); summing ix's bits weighted by
 * how often their tick count occurs gives floor(ix * 2^(s-9)) plus one
 * more step exactly when the next lower bit of ix is set - i.e. the
 * hardware rounds the exact float value half-up to an integer, every
 * vector, every axis.  That is at most a 0.5-unit-per-axis difference
 * per vector, and because consecutive VCTR/SVECs share one running
 * position, it accumulates along a chain until the next LABS reloads
 * the position exactly.  This ROM's rock shapes are chains of order 14
 * vectors at high scales, bounding the drift at about 7 units per axis;
 * Lunar Lander's chains were shorter and never exceeded 3, which is
 * where that game's TOL_XY of 3.0 came from - a property of that
 * game's shapes, not of the hardware.  This file's TOL_XY is widened to
 * cover this ROM's own chains, but a looser position tolerance alone
 * would also wave through an endpoint that's merely close by accident
 * (a wrong vector, or a shape bug) - so build_clipped_ref() also hands
 * back a per-segment "not clipped" flag, and every matched pair where
 * the reference segment carries that flag and the hw segment touches no
 * window edge is additionally required to have the *same shape*: the
 * per-axis delta (x1-x0, y1-y0) may differ by no more than the single-
 * vector 0.5-unit bound (plus float slop).  A shape violation is a FAIL
 * even though the endpoints alone were within TOL_XY.
 *
 * Rule B (edge-band grazing).  Within a few units of the blanking
 * boundary, Rule A's accumulated drift decides which side of the
 * boundary each model puts a vertex on, so the two models can
 * legitimately emit different *numbers* of fragments there - not just
 * different positions for the same fragment, the way the entry-stub/
 * wrap-dot pair above works.  In diff_one_dump()'s lockstep loop, once
 * a mismatch survives the existing entry-stub/wrap-dot resync attempt
 * (that check runs first, unchanged), the pair is inspected once more
 * before it's called a FAIL: if the reference segment lies entirely
 * within TOL_XY of an edge (0 or 1023) on both its endpoints, it's
 * dropped and counted as an "edge drop (ref)"; else if the hw segment
 * does, it's dropped and counted as an "edge drop (hw)"; else it's a
 * FAIL exactly as before.  Both counts are printed per dump; either one
 * running much above a small fraction of a percent of the dump's
 * segments would mean this rule is quietly hiding something other than
 * genuine edge grazing, so that's reported rather than accepted
 * silently.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../astdelux.h"
#include "../astdelux_rom.h"
#include "../platform/ad_platform.h"

/* dvg_ref.c's entry point; not declared in any shared header since it
 * only exists for this test. */
extern int dvg_ref_render(void);

/* dvg_ref.c's instrumentation (see its header): whether this ROM's
 * lists ever hit the two spots where the old float walker and the new
 * hardware model share no common ground at all (not just an ordinary
 * rounding/edge disagreement) - a combined scale of 10 or more, or a
 * LABS word with bit 10 or 11 set.  Checked and reported below. */
extern int dvg_ref_scale_ge10_count;
extern uint16_t dvg_ref_scale_ge10_addr, dvg_ref_scale_ge10_w1, dvg_ref_scale_ge10_w2;
extern int dvg_ref_labs_hibits_count;
extern uint16_t dvg_ref_labs_hibits_addr, dvg_ref_labs_hibits_w1, dvg_ref_labs_hibits_w2;

/* Each host owns g (astdelux.h's extern ad_state g); mainline.c's
 * definition drags in the whole game, so this test supplies its own,
 * exactly the way Lunar Lander's dvg_test.c does. */
ad_state g;

/* Master clock the DVG and POKEY share (astdelux.h/dvg.c's own
 * comments); no platform header defines this as a macro for this game,
 * unlike Lunar Lander's LL_MASTER_CLOCK_HZ, so it is local to this
 * file. */
#define AD_MASTER_CLOCK_HZ 12096000.0

/* Four NMIs at 4 ms each (mainline.c's ad_frame(), "SYNC is bumped by
 * the NMI every fourth interrupt, so this is the ~16 ms tick") is the
 * DVG's whole time budget for one display list. */
#define AD_FRAME_BUDGET_CYCLES ((unsigned long)(0.016 * AD_MASTER_CLOCK_HZ))

#define TOL     0.01

/* Rate-multiplier rounding drift (Rule A, this file's header): the
 * hardware rounds each vector's exact float displacement to an integer
 * half-up, at most 0.5 units off per vector per axis, accumulating
 * along a chain of vectors sharing one running position until the next
 * LABS reloads it.  This ROM's rock shapes chain about 14 vectors deep
 * at high scales, bounding the drift near 7 units per axis; Lunar
 * Lander's TOL_XY of 3.0 reflected that game's own (shorter) chains,
 * not a property of the hardware.  SHAPE_TOL is the per-vector bound
 * itself (0.5, plus float slop) and backs the tight per-segment shape
 * check that keeps this looser endpoint tolerance from hiding an
 * actually-wrong vector. */
#define TOL_XY  8.0
#define SHAPE_TOL (0.5 + 1e-3)

typedef struct { float x0, y0, x1, y1; int z; } seg_t;

#define MAX_SEGS 8192
static seg_t hw_segs[MAX_SEGS];
static int   n_hw;
static seg_t ref_segs[MAX_SEGS];
static int   n_ref;

typedef enum { SINK_HW, SINK_REF } sink_t;
static sink_t g_sink = SINK_HW;

void plat_video_line(float x0, float y0, float x1, float y1, int z)
{
    seg_t *arr;
    int *n;

    if (g_sink == SINK_HW) { arr = hw_segs;  n = &n_hw;  }
    else                   { arr = ref_segs; n = &n_ref; }

    if (*n < MAX_SEGS) {
        arr[*n].x0 = x0; arr[*n].y0 = y0;
        arr[*n].x1 = x1; arr[*n].y1 = y1;
        arr[*n].z  = z;
        (*n)++;
    }
}

static int near(float a, float b) { return fabsf(a - b) <= (float)TOL; }

/* Load frame `frame` (0-based) of dump `path`'s vector RAM into g.vram.
 * Returns 1 on a full read, 0 on EOF (short or absent read - normal
 * end of a dump), -1 on a real I/O error. */
static int load_frame(const char *path, long frame)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        fprintf(stderr, "dvg_test: cannot open %s\n", path);
        return -1;
    }
    if (fseek(f, frame * 2304L + 256L, SEEK_SET) != 0) {
        fclose(f);
        return 0;                                       /* past EOF */
    }
    n = fread(g.vram, 1, sizeof g.vram, f);
    fclose(f);
    if (n != sizeof g.vram)
        return 0;                                        /* short/no read: EOF */
    return 1;
}

/* The window is [0,1024) x [0,1024): 0 is a legitimately visible
 * position (hardware only blanks at bit 10, i.e. >= 1024), but 1024
 * itself already is blanked, so the upper edge has to be open.  Liang-
 * Barsky as usually written clips to a closed box, which would let a
 * point sitting exactly on x==1024 or y==1024 survive (q==0 is not
 * "outside"); nudge the upper bound down by an epsilon well under any
 * real fractional coordinate (positions are integers or halves at
 * worst) so an exactly-on-the-edge point is correctly rejected. */
#define WMAX 1023.999f

/* Liang-Barsky clip of (x0,y0)-(x1,y1) to [0,1024) x [0,1024). Returns
 * 1 and the clipped endpoints if any part survives, 0 if the whole
 * segment is outside the window.  *was_clipped (if non-NULL) reports
 * whether either end actually got trimmed (t0 > 0 or t1 < 1) - used by
 * build_clipped_ref() to flag segments Rule A's shape check can safely
 * apply to (this file's header). */
static int clip_seg(float x0, float y0, float x1, float y1,
                     float *cx0, float *cy0, float *cx1, float *cy1,
                     int *was_clipped)
{
    float t0 = 0.0f, t1 = 1.0f;
    float dx = x1 - x0, dy = y1 - y0;
    float p[4], q[4];
    int i;

    p[0] = -dx; q[0] = x0 - 0.0f;
    p[1] =  dx; q[1] = WMAX - x0;
    p[2] = -dy; q[2] = y0 - 0.0f;
    p[3] =  dy; q[3] = WMAX - y0;

    for (i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f)
                return 0;                                 /* parallel & outside */
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (r > t1) return 0;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return 0;
                if (r < t1) t1 = r;
            }
        }
    }

    *cx0 = x0 + t0 * dx; *cy0 = y0 + t0 * dy;
    *cx1 = x0 + t1 * dx; *cy1 = y0 + t1 * dy;
    if (was_clipped)
        *was_clipped = (t0 > 1e-4f) || (t1 < 1.0f - 1e-4f);
    return 1;
}

static int is_dot(float x0, float y0, float x1, float y1)
{
    return fabsf(x1 - x0) < 1e-4f && fabsf(y1 - y0) < 1e-4f;
}

/* Build the clipped reference list for one frame's ref_segs[0..n_ref)
 * into clipped[], returning its count.  A reference segment that was a
 * real (non-dot) vector but clips down to a single point exactly on
 * the window boundary is a continuous-space clipping artifact the
 * integer hardware doesn't share (it just never gets that fractional
 * pixel of the vector into the window at all), so it's dropped rather
 * than compared; *n_dropped counts those for the report.
 *
 * full_flags[0..returned count) is a parallel array: full_flags[k] is
 * true when clipped[k] survived untouched (clip_seg's *was_clipped came
 * back false) - i.e. this was not a vector poking through the window
 * edge, so Rule A's shape check (this file's header) may compare its
 * shape against the matching hw segment directly. */
static int build_clipped_ref(seg_t *clipped, int *full_flags, int max_clipped, int *n_dropped)
{
    int i, n = 0;

    *n_dropped = 0;
    for (i = 0; i < n_ref; i++) {
        float cx0, cy0, cx1, cy1;
        int was_clipped;
        if (!clip_seg(ref_segs[i].x0, ref_segs[i].y0, ref_segs[i].x1, ref_segs[i].y1,
                      &cx0, &cy0, &cx1, &cy1, &was_clipped))
            continue;                                     /* entirely outside */

        if (is_dot(cx0, cy0, cx1, cy1) && !is_dot(ref_segs[i].x0, ref_segs[i].y0,
                                                    ref_segs[i].x1, ref_segs[i].y1)) {
            (*n_dropped)++;
            continue;                                     /* boundary clip artifact */
        }

        /* Same artifact, one size up: a clipped fragment confined to
         * the last sub-unit strip inside the window edge (both x in
         * [1023,1024), or both y).  The hardware's integer counter is
         * either at 1023 (inside, and any vector through that column
         * is at least a whole step long) or at 1024 (blanked); a
         * fraction-long sliver between them is continuous clipping
         * meeting the bit-10 blank, and hw never emits it (frame 1441
         * of play_noshield.dump: a rock re-entering at the right edge
         * pokes three such slivers into the window). */
        if (was_clipped
            && ((cx0 >= 1023.0f && cx1 >= 1023.0f) || (cy0 >= 1023.0f && cy1 >= 1023.0f))) {
            (*n_dropped)++;
            continue;                                     /* sub-unit edge sliver */
        }

        if (n < max_clipped) {
            clipped[n].x0 = cx0; clipped[n].y0 = cy0;
            clipped[n].x1 = cx1; clipped[n].y1 = cy1;
            clipped[n].z  = ref_segs[i].z;
            full_flags[n] = !was_clipped;
            n++;
        }
    }
    return n;
}

/* Restrict hw's own list to segments entirely inside [0,1024) x
 * [0,1024) - dvg_render() never emits outside that window by
 * construction (dvg_draw_to's bit-10 check), so this should normally
 * be a no-op; kept for parity with the reference side's windowing and
 * as a belt-and-braces check that stays true to Lunar Lander's
 * structure. *n_dropped counts anything that did land outside. */
static int build_windowed_hw(seg_t *windowed, int max_windowed, int *n_dropped)
{
    int i, n = 0;

    *n_dropped = 0;
    for (i = 0; i < n_hw; i++) {
        seg_t s = hw_segs[i];
        if (s.x0 < 0.0f || s.x0 >= 1024.0f || s.y0 < 0.0f || s.y0 >= 1024.0f
            || s.x1 < 0.0f || s.x1 >= 1024.0f || s.y1 < 0.0f || s.y1 >= 1024.0f) {
            (*n_dropped)++;
            continue;
        }
        if (n < max_windowed)
            windowed[n++] = s;
    }
    return n;
}

static void print_seg(const char *label, seg_t s)
{
    printf("    %s (%.3f,%.3f)-(%.3f,%.3f) z=%d\n", label, (double)s.x0, (double)s.y0,
           (double)s.x1, (double)s.y1, s.z);
}

/* Max endpoint deviation between two segments (does not check z). */
static float seg_dev(seg_t a, seg_t b)
{
    float d = fabsf(a.x0 - b.x0);
    if (fabsf(a.y0 - b.y0) > d) d = fabsf(a.y0 - b.y0);
    if (fabsf(a.x1 - b.x1) > d) d = fabsf(a.x1 - b.x1);
    if (fabsf(a.y1 - b.y1) > d) d = fabsf(a.y1 - b.y1);
    return d;
}

/* A coordinate sitting on the visible window's edge (0 or the last
 * visible integer, 1023) - where a vector that starts blanked (beam
 * position wrapped negative/over the top in the 12-bit counter) would
 * materialize as it crosses into view, or where one leaving the
 * window gets cut off.  hw's edge coordinate is always exactly 0 or
 * 1023 (integer counters); the clipped reference's is clip_seg's WMAX
 * (1023.999) rather than 1023.0 on that side, so the tolerance has to
 * cover both. */
static int on_edge(float v)
{
    return fabsf(v) < 1.5f || fabsf(v - 1023.0f) < 1.5f;
}

/* A vector that crosses out of (or into) the window is where the two
 * models are expected to disagree by more than ordinary float noise -
 * dvg_ref_render() clips a continuous straight line, but the real beam
 * position is stepped by two independent bit-rate multipliers
 * (dvg_gostrobe's trailing-ones tick pattern), one per axis, and that
 * pattern is front-loaded, not evenly spread.  At the specific tick
 * where one axis crosses the window edge, the other axis can
 * legitimately be several units away from where a straight-line
 * interpolation would put it (see Lunar Lander's dvg_test.c for a
 * worked example against its own dvg.c; the algorithm is identical
 * here).  So: a boundary endpoint (on the window's edge) is allowed a
 * wider tolerance than an ordinary one, but only when the segment's
 * *other* endpoint (the already-established prior position) still
 * matches tightly and both sides agree it's a boundary case at all -
 * anything else still has to meet TOL_XY.  The same reasoning applies
 * symmetrically to a vector *entering* the window (the "start" point
 * on the edge instead of the "end" one). */
#define TOL_BOUNDARY 24.0

static float pt_dev(float x0, float y0, float x1, float y1)
{
    float d = fabsf(x0 - x1);
    if (fabsf(y0 - y1) > d) d = fabsf(y0 - y1);
    return d;
}

/* Rule B (edge-band grazing, this file's header): a coordinate within
 * TOL_XY of the window edge (0 or 1023) - the band where Rule A's
 * accumulated drift can put either model on either side of the
 * blanking boundary, so the two models can legitimately disagree about
 * how many fragments a beam grazing there produces. */
static int coord_near_edge(float v)
{
    return fabsf(v) <= (float)TOL_XY || fabsf(v - 1023.0f) <= (float)TOL_XY;
}

static int endpoint_near_edge(float x, float y)
{
    return coord_near_edge(x) || coord_near_edge(y);
}

/* Both endpoints of the segment have some coordinate near the window
 * edge - the condition under which diff_one_dump()'s lockstep loop may
 * drop an unmatched segment as edge grazing rather than failing. */
static int seg_near_edge(seg_t s)
{
    return endpoint_near_edge(s.x0, s.y0) && endpoint_near_edge(s.x1, s.y1);
}

static int seg_matches(seg_t a, seg_t b)
{
    if (a.z != b.z)
        return 0;
    if (seg_dev(a, b) <= (float)TOL_XY)
        return 1;

    /* leaving case: end point is the boundary one */
    if ((on_edge(a.x1) || on_edge(a.y1)) && (on_edge(b.x1) || on_edge(b.y1))
        && pt_dev(a.x0, a.y0, b.x0, b.y0) <= (float)TOL_XY
        && pt_dev(a.x1, a.y1, b.x1, b.y1) <= (float)TOL_BOUNDARY)
        return 1;

    /* entering case: start point is the boundary one */
    if ((on_edge(a.x0) || on_edge(a.y0)) && (on_edge(b.x0) || on_edge(b.y0))
        && pt_dev(a.x1, a.y1, b.x1, b.y1) <= (float)TOL_XY
        && pt_dev(a.x0, a.y0, b.x0, b.y0) <= (float)TOL_BOUNDARY)
        return 1;

    return 0;
}

/* Run the differential test over one dump.  Returns 1 on success, 0 on
 * a FAIL (already printed) meaning the whole test should stop. */
static int diff_one_dump(const char *path)
{
    long frame;
    long n_frames = 0;
    long n_segs_total = 0;
    long n_dropped_total = 0;
    long n_entry_stubs_total = 0;
    long n_wrap_dots_total = 0;
    long n_hw_windowed_out_total = 0;
    long n_over_budget = 0;             /* frames over AD_FRAME_BUDGET_CYCLES */
    long n_edge_drop_ref_total = 0;     /* Rule B: unmatched ref segment dropped as edge grazing */
    long n_edge_drop_hw_total = 0;      /* Rule B: unmatched hw segment dropped as edge grazing */
    unsigned long max_cycles = 0;       /* dvg_last_cycles(), informational */
    float max_dev = 0.0f;               /* Rule A: max endpoint deviation over matched pairs */
    float max_shape_dev = 0.0f;         /* Rule A: max per-axis shape deviation over matched pairs */
    int prev_ge10, prev_hibits;
    static seg_t clipped[MAX_SEGS];
    static int   clipped_full[MAX_SEGS];
    static seg_t hw[MAX_SEGS];

    for (frame = 0; ; frame++) {
        int rc = load_frame(path, frame);
        int n_clipped, n_dropped, n_hw_out, n_win;
        int i, j;

        if (rc == 0)
            break;                                        /* EOF: done with this dump */
        if (rc < 0) {
            printf("dvg_test: FAIL %s frame %ld: I/O error loading frame\n", path, frame);
            return 0;
        }

        prev_ge10 = dvg_ref_scale_ge10_count;
        prev_hibits = dvg_ref_labs_hibits_count;
        n_ref = 0; g_sink = SINK_REF; dvg_ref_render();
        n_hw  = 0; g_sink = SINK_HW;  dvg_render();
        if (dvg_ref_scale_ge10_count != prev_ge10)
            printf("dvg_test: NOTE %s frame %ld: combined scale>=10 vector seen at $%04X "
                   "(w1=$%04X w2=$%04X, total so far %d)\n", path, frame,
                   dvg_ref_scale_ge10_addr, dvg_ref_scale_ge10_w1, dvg_ref_scale_ge10_w2,
                   dvg_ref_scale_ge10_count);
        if (dvg_ref_labs_hibits_count != prev_hibits)
            printf("dvg_test: NOTE %s frame %ld: LABS with bit 10/11 set at $%04X "
                   "(w1=$%04X w2=$%04X, total so far %d)\n", path, frame,
                   dvg_ref_labs_hibits_addr, dvg_ref_labs_hibits_w1, dvg_ref_labs_hibits_w2,
                   dvg_ref_labs_hibits_count);
        if (dvg_last_cycles() > max_cycles) max_cycles = dvg_last_cycles();
        if (dvg_last_cycles() > AD_FRAME_BUDGET_CYCLES) n_over_budget++;

        n_clipped = build_clipped_ref(clipped, clipped_full, MAX_SEGS, &n_dropped);
        n_dropped_total += n_dropped;
        n_win = build_windowed_hw(hw, MAX_SEGS, &n_hw_out);
        n_hw_windowed_out_total += n_hw_out;
        n_frames++;

        /* Walk both lists in lockstep.  The two models agree exactly
         * almost everywhere; there are two documented, verified ways
         * hw can carry a segment ref doesn't, both stemming from the
         * same root cause - the reference walker tracks its position
         * as an unbounded float accumulator with no idea of the real
         * beam's 12-bit modular counters or bit-10 blanking, while hw
         * (dvg.c) tracks exactly what the hardware does:
         *
         * 1. Entry stub: a vector starts off-screen (wrapped through
         *    the counter's blanked band) and crosses into the visible
         *    field partway through.  hw draws that as two pieces - a
         *    silent move to the entry point, then the real segment
         *    from there to the vector's endpoint - while ref only ever
         *    emits the whole vector as one segment from its own
         *    position.  The extra piece starts exactly on the window's
         *    edge (see seg_matches's own comment for the analogous,
         *    verified case on the *endpoint* side of the same thing).
         *
         * 2. Orphan wrap dot: a zero-magnitude SVEC (dx=dy=0, used to
         *    flash a single lit point) fired after a long off-screen
         *    excursion with no intervening LABS/CNTR to resync
         *    position.  hw's wrapped 12-bit position is exactly right
         *    (it never stops being exact - there's no floating point
         *    involved at all); ref's linearly-accumulated float
         *    position has drifted arbitrarily far from it (off by
         *    whatever multiple of 4096 the excursion's un-wrapped
         *    counters would have crossed), so ref's own version of the
         *    same dot lands outside the window and gets dropped by
         *    build_clipped_ref entirely - it isn't just shifted, it's
         *    simply absent from ref's list, at any position.  So: a
         *    zero-length hw segment with no ref counterpart is
         *    tolerated too, on the same "does skipping it resync the
         *    rest of the list" condition.
         *
         * In both cases the fix is the same and it's the only one
         * applied: if skipping exactly one extra hw segment brings the
         * very next hw segment back into agreement with ref, that
         * extra segment is one of the two documented cases above, not
         * a bug - skip it and keep going.  Anything else is a real
         * failure. */
        i = 0; j = 0;
        for (;;) {
            int k, run;
            const int MAX_RESYNC_RUN = 16;

            if (i >= n_clipped && j >= n_win)
                break;

            /* Stub preference (this file's header): tested before the
             * primary match below, not after it fails, because Rule
             * A's widened TOL_XY can let a stub satisfy seg_matches()
             * against ref[i] by coincidence even though hw[j+1] fits
             * ref[i] better - accepting the stub as the match there
             * would leave hw permanently one segment ahead of ref. */
            if (i < n_clipped && j + 1 < n_win
                && (hw[j].x0 == 0.0f || hw[j].x0 == 1023.0f
                 || hw[j].y0 == 0.0f || hw[j].y0 == 1023.0f)
                && seg_matches(clipped[i], hw[j + 1])
                && seg_dev(clipped[i], hw[j + 1]) < seg_dev(clipped[i], hw[j])) {
                n_entry_stubs_total++;
                j++;
                continue;
            }

            /* Rule B preference, the mirror of the stub preference: an
             * edge-band segment on either side whose successor fits
             * the other side's current segment outright and strictly
             * better than it does itself is the one to drop, even if
             * it would have passed the primary check by coincidence
             * (frame 1452 of play_noshield.dump: a rock re-entering at
             * the left edge, hw's beam three units behind ref's). */
            if (i + 1 < n_clipped && j < n_win && seg_near_edge(clipped[i])
                && seg_matches(clipped[i + 1], hw[j])
                && seg_dev(clipped[i + 1], hw[j]) < seg_dev(clipped[i], hw[j])) {
                n_edge_drop_ref_total++;
                i++;
                continue;
            }
            if (i < n_clipped && j + 1 < n_win && seg_near_edge(hw[j])
                && seg_matches(clipped[i], hw[j + 1])
                && seg_dev(clipped[i], hw[j + 1]) < seg_dev(clipped[i], hw[j])) {
                n_edge_drop_hw_total++;
                j++;
                continue;
            }

            if (i < n_clipped && j < n_win && seg_matches(clipped[i], hw[j])) {
                float d = seg_dev(clipped[i], hw[j]);
                if (d > max_dev) max_dev = d;

                /* Rule A shape check (this file's header): only where
                 * the ref segment wasn't itself a boundary-clipped
                 * fragment and the hw segment touches no window edge -
                 * both conditions the drift-vs-a-different-vector
                 * ambiguity needs ruled out for the check to mean
                 * anything. */
                if (clipped_full[i]
                    && !(on_edge(hw[j].x0) || on_edge(hw[j].y0)
                      || on_edge(hw[j].x1) || on_edge(hw[j].y1))) {
                    float dx_ref = clipped[i].x1 - clipped[i].x0;
                    float dy_ref = clipped[i].y1 - clipped[i].y0;
                    float dx_hw  = hw[j].x1 - hw[j].x0;
                    float dy_hw  = hw[j].y1 - hw[j].y0;
                    float sdx = fabsf(dx_hw - dx_ref);
                    float sdy = fabsf(dy_hw - dy_ref);
                    float sdev = sdx > sdy ? sdx : sdy;

                    if (sdev > max_shape_dev) max_shape_dev = sdev;
                    if (sdx > (float)SHAPE_TOL || sdy > (float)SHAPE_TOL) {
                        int m, lo, hi;
                        printf("dvg_test: FAIL %s frame %ld: shape mismatch at ref[%d]/hw[%d] "
                               "dx_ref=%.4f dy_ref=%.4f dx_hw=%.4f dy_hw=%.4f (tol %.4f)\n",
                               path, frame, i, j, (double)dx_ref, (double)dy_ref,
                               (double)dx_hw, (double)dy_hw, (double)SHAPE_TOL);
                        lo = i - 4; if (lo < 0) lo = 0;
                        hi = i + 6;
                        for (m = lo; m < hi && m < n_clipped; m++) { printf("  ref[%d]:", m); print_seg("", clipped[m]); }
                        lo = j - 4; if (lo < 0) lo = 0;
                        hi = j + 6;
                        for (m = lo; m < hi && m < n_win; m++) { printf("  hw [%d]:", m); print_seg("", hw[m]); }
                        return 0;
                    }
                }

                i++; j++;
                continue;
            }

            /* Look for a short run of consecutive hw-only segments,
             * each individually one of the two documented cases (an
             * edge-touching stub, or an orphan zero-length dot), that
             * ends with hw resyncing back onto ref.  If the whole run
             * qualifies, skip it. */
            run = 0;
            if (i < n_clipped) {
                for (k = j; k < n_win && k < j + MAX_RESYNC_RUN; k++) {
                    int is_stub = on_edge(hw[k].x0) || on_edge(hw[k].y0)
                                || on_edge(hw[k].x1) || on_edge(hw[k].y1);
                    int is_dot  = hw[k].x0 == hw[k].x1 && hw[k].y0 == hw[k].y1;

                    if (seg_matches(clipped[i], hw[k])) {
                        run = k - j;
                        break;
                    }
                    if (!is_stub && !is_dot)
                        break;                               /* not a tolerable extra: give up */
                    if (is_dot && !is_stub)
                        n_wrap_dots_total++;
                    else
                        n_entry_stubs_total++;
                }
            }
            if (run > 0) {
                j += run;                                     /* skip the tolerated extras */
                continue;
            }

            /* Rule B (edge-band grazing, this file's header): tried
             * only after the entry-stub/wrap-dot resync above has
             * already given up.  A segment entirely within TOL_XY of
             * the window edge on both ends is where Rule A's drift can
             * legitimately put a vertex on either side of the blanking
             * boundary, so the two models can disagree about fragment
             * *count* there - drop whichever side has it and keep
             * going, rather than treating a count mismatch as a FAIL. */
            /* Like every other skip in this loop, an edge drop is only
             * allowed when it resyncs: a run of up to MAX_RESYNC_RUN
             * edge-band segments on one side that ends with the next
             * one matching the other side's current segment (or that
             * side being exhausted).  A drop that did not resync would
             * just move the FAIL somewhere less informative. */
            run = 0;
            if (i < n_clipped && seg_near_edge(clipped[i])) {
                for (k = i; k < n_clipped && k < i + MAX_RESYNC_RUN; k++) {
                    if (j >= n_win ? 0 : seg_matches(clipped[k], hw[j])) { run = k - i; break; }
                    if (!seg_near_edge(clipped[k])) { run = 0; break; }
                    if (j >= n_win && k + 1 == n_clipped) { run = k + 1 - i; break; }
                }
                if (run > 0) {
                    n_edge_drop_ref_total += run;
                    i += run;
                    continue;
                }
            }
            run = 0;
            if (j < n_win && seg_near_edge(hw[j])) {
                for (k = j; k < n_win && k < j + MAX_RESYNC_RUN; k++) {
                    if (i >= n_clipped ? 0 : seg_matches(clipped[i], hw[k])) { run = k - j; break; }
                    if (!seg_near_edge(hw[k])) { run = 0; break; }
                    if (i >= n_clipped && k + 1 == n_win) { run = k + 1 - j; break; }
                }
                if (run > 0) {
                    n_edge_drop_hw_total += run;
                    j += run;
                    continue;
                }
            }

            {
                int m, lo, hi;
                printf("dvg_test: FAIL %s frame %ld: unmatched segment at ref[%d]/hw[%d] "
                       "(clipped ref=%d, windowed hw=%d, boundary-dropped=%d)\n",
                       path, frame, i, j, n_clipped, n_win, n_dropped);
                lo = i - 2; if (lo < 0) lo = 0;
                hi = i + 8;
                for (m = lo; m < hi && m < n_clipped; m++) { printf("  ref[%d]:", m); print_seg("", clipped[m]); }
                lo = j - 2; if (lo < 0) lo = 0;
                hi = j + 8;
                for (m = lo; m < hi && m < n_win; m++) { printf("  hw [%d]:", m); print_seg("", hw[m]); }
            }
            return 0;
        }

        n_segs_total += n_win;
    }

    printf("dvg_test: %-20s frames=%-6ld segments=%-7ld max_dev=%.4f max_shape_dev=%.4f "
           "boundary_drops=%ld entry_stubs=%ld wrap_dots=%ld edge_drop_ref=%ld edge_drop_hw=%ld "
           "hw_2nd_band=%ld max_cycles=%lu (%.3f ms) over_budget=%ld\n",
           path, n_frames, n_segs_total, (double)max_dev, (double)max_shape_dev, n_dropped_total,
           n_entry_stubs_total, n_wrap_dots_total, n_edge_drop_ref_total, n_edge_drop_hw_total,
           n_hw_windowed_out_total, max_cycles,
           (double)max_cycles * 1000.0 / AD_MASTER_CLOCK_HZ, n_over_budget);

    /* Rule B's edge-drop counts are expected to be a small fraction of
     * the dump's segments (genuine edge grazing is rare); a much larger
     * share would mean the rule is quietly absorbing something else. */
    if (n_segs_total > 0) {
        double pct = 100.0 * (double)(n_edge_drop_ref_total + n_edge_drop_hw_total)
                     / (double)n_segs_total;
        if (pct > 0.1)
            printf("dvg_test: SUSPICIOUS %s: edge drops (ref=%ld hw=%ld) are %.4f%% of "
                   "%ld segments - investigate before trusting Rule B here\n",
                   path, n_edge_drop_ref_total, n_edge_drop_hw_total, pct, n_segs_total);
    }
    return 1;
}

int main(void)
{
    int rc;
    int i;
    static const char *dumps[] = {
        "attract.dump", "play.dump", "play_noshield.dump", "stest.dump"
    };

    /* ---- test 1: field pin (attract.dump, frame 400, hardware list) --- */
    /* Frame 400: attract mode (ZP.NPLAYR==0, a fresh EAROM with no high
     * scores yet, so ad_scores() always takes the SCORES_90 branch -
     * score.c) drawing both the top score row (ad_params()) and the
     * copyright line (ad_cpyrs()) while the background rock simulation
     * keeps running (mainline.c's START2_15).  Because the rocks roam
     * the whole 0..1023 field, the *global* max/min of the field-
     * clipped list is not a stable pin for this ROM (checked directly
     * against a 600-frame attract.dump: no frame's global min y is even
     * close to the copyright text's, and by frame 128 a rock's own
     * vertex has already gone above the score digits' top edge) - see
     * this file's header. Instead, pin the two features by their own
     * exact endpoints, read off this frame's real dvg_render() output:
     * the top edge of the player-1 score's "0" digit box (still all
     * zeroes - ad_params() at beam (100,876), the digit drawn as a
     * 16-unit square, so its top edge is the segment (220,900)-
     * (236,900)) and the leftmost stroke of the copyright text
     * (ad_cpyrs() at beam (448,416), the segment (368,288)-(368,264)). */
    if (load_frame("attract.dump", 400) != 1)
        return 1;
    n_hw = 0; g_sink = SINK_HW;
    rc = dvg_render();
    if (rc != 0) {
        printf("dvg_test: FAIL test1 dvg_render returned %d (expected 0)\n", rc);
        return 1;
    }
    {
        int found_score_top = 0, found_copyright_left = 0;
        int n_in_field = 0;
        float max_y = -1e9f, min_y = 1e9f;

        for (i = 0; i < n_hw; i++) {
            seg_t s = hw_segs[i];
            float xs[2] = { s.x0, s.x1 };
            float ys[2] = { s.y0, s.y1 };
            int k;

            for (k = 0; k < 2; k++) {
                if (xs[k] < 0.0f || xs[k] >= 1024.0f || ys[k] < 0.0f || ys[k] >= 1024.0f)
                    continue;                          /* hardware-blanked */
                n_in_field++;
                if (ys[k] > max_y) max_y = ys[k];
                if (ys[k] < min_y) min_y = ys[k];
            }

            if (s.z == 7
                && ((near(s.x0, 220.0f) && near(s.y0, 900.0f) && near(s.x1, 236.0f) && near(s.y1, 900.0f))
                 || (near(s.x1, 220.0f) && near(s.y1, 900.0f) && near(s.x0, 236.0f) && near(s.y0, 900.0f))))
                found_score_top = 1;
            if (s.z == 7
                && ((near(s.x0, 368.0f) && near(s.y0, 288.0f) && near(s.x1, 368.0f) && near(s.y1, 264.0f))
                 || (near(s.x1, 368.0f) && near(s.y1, 288.0f) && near(s.x0, 368.0f) && near(s.y0, 264.0f))))
                found_copyright_left = 1;
        }
        if (n_in_field == 0 || !found_score_top || !found_copyright_left) {
            printf("dvg_test: FAIL test1 n_in_field=%d found_score_top=%d found_copyright_left=%d "
                   "(field max_y=%.3f min_y=%.3f, for reference)\n",
                   n_in_field, found_score_top, found_copyright_left, (double)max_y, (double)min_y);
            return 1;
        }
    }
    printf("dvg_test: PASS test1 (field pin: score-row top y=900, copyright-line left stroke y=264..288)\n");

    /* ---- test 2: bit-10 blanking (play.dump, frame 1000, hardware list) */
    if (load_frame("play.dump", 1000) != 1)
        return 1;
    n_hw = 0; g_sink = SINK_HW;
    rc = dvg_render();
    if (rc != 0) {
        printf("dvg_test: FAIL test2 dvg_render returned %d (expected 0)\n", rc);
        return 1;
    }
    if (n_hw == 0) {
        printf("dvg_test: FAIL test2 hardware list is empty\n");
        return 1;
    }
    for (i = 0; i < n_hw; i++) {
        if (hw_segs[i].x0 < 0.0f || hw_segs[i].x0 > 1023.0f ||
            hw_segs[i].y0 < 0.0f || hw_segs[i].y0 > 1023.0f ||
            hw_segs[i].x1 < 0.0f || hw_segs[i].x1 > 1023.0f ||
            hw_segs[i].y1 < 0.0f || hw_segs[i].y1 > 1023.0f) {
            printf("dvg_test: FAIL test2 segment %d out of 0..1023: (%.3f,%.3f)-(%.3f,%.3f)\n",
                   i, (double)hw_segs[i].x0, (double)hw_segs[i].y0,
                   (double)hw_segs[i].x1, (double)hw_segs[i].y1);
            return 1;
        }
    }
    printf("dvg_test: PASS test2 (bit-10 blanking, %d segments)\n", n_hw);

    /* ---- test 3: differential, dvg_ref_render() vs dvg_render() ------- */
    for (i = 0; i < (int)(sizeof dumps / sizeof dumps[0]); i++) {
        if (!diff_one_dump(dumps[i]))
            return 1;
    }

    /* dvg_ref.c's own instrumentation (this file's header): neither
     * condition is expected in this ROM's display lists, but reported
     * either way rather than assumed. */
    printf("dvg_test: scale>=10 vectors seen: %d%s\n", dvg_ref_scale_ge10_count,
           dvg_ref_scale_ge10_count
               ? "" : " (none - old walker and hw model agree on every vector's scale)");
    if (dvg_ref_scale_ge10_count)
        printf("dvg_test:   first at $%04X w1=$%04X w2=$%04X\n",
               dvg_ref_scale_ge10_addr, dvg_ref_scale_ge10_w1, dvg_ref_scale_ge10_w2);
    printf("dvg_test: LABS bit10/11 seen: %d%s\n", dvg_ref_labs_hibits_count,
           dvg_ref_labs_hibits_count
               ? "" : " (none - every LABS in these dumps fits the old walker's 10-bit mask)");
    if (dvg_ref_labs_hibits_count)
        printf("dvg_test:   first at $%04X w1=$%04X w2=$%04X\n",
               dvg_ref_labs_hibits_addr, dvg_ref_labs_hibits_w1, dvg_ref_labs_hibits_w2);

    printf("dvg_test: PASS\n");
    return 0;
}
