#!/usr/bin/env python3
"""Draw every DVG shape as SVG, for review.

A decoded display list is still hard to check by eye - `SVEC u=8 dx=24
dy=-16 z=13` says little about whether the saucer looks like a saucer.
This runs each shape the way the hardware would (following JSRL into
sub-shapes, honouring intensity) and writes one page with all of them.

It is a review tool, so it shows what is actually there rather than a
tidied version: a shape that decodes to nothing renders as an empty box,
and blanked vectors between marks are drawn faintly so the pen path is
visible.

Two things are deliberately *not* shown as stored, because showing them
that way described the storage format rather than the picture:

  - the blanked moves a shape uses to get into position and back out
    again.  They start from wherever the caller left the beam, so drawing
    them from our arbitrary (0,0) invents lines across the middle.
  - the all-blanked shapes.  The ship frames and the shield hold `z=0`
    throughout and the 6502 ORs intensity in as it copies them
    (`CPYVEC`); they are drawn lit, because that is how they are seen.

Reads  <set>_dump.bin, <set>_vecrom.asm
Writes <set>_shapes.html

Usage:
    python mkpreview.py [--set astdelux2]
"""

import argparse
import re
import sys
from collections import OrderedDict

import dvgdasm

VECRAM = 0x4000
MAX_STEPS = 4000


def decode_values(mem, a):
    """-> (size, kind, dx, dy, z, target) for one DVG instruction.

    dx/dy come back in the units the programmer wrote, matching what
    dvgdasm prints: the long VCTR form stores magnitudes scaled up by
    2**(9-opcode), so they are shifted back here too.
    """
    w = dvgdasm.word(mem, a)
    op = w >> 12
    if op <= 9:
        w2 = dvgdasm.word(mem, a + 2)
        dy, dx, z = w & 0x3FF, w2 & 0x3FF, w2 >> 12
        if w & 0x400:
            dy = -dy
        if w2 & 0x400:
            dx = -dx
        shift = 9 - op
        if shift >= 0 and not ((abs(dx) | abs(dy)) & ((1 << shift) - 1)):
            dx >>= shift
            dy >>= shift
        return (4, "vec", dx, dy, z, None)
    if op == 0xA:
        w2 = dvgdasm.word(mem, a + 2)
        return (4, "labs", w2 & 0xFFF, w & 0xFFF, 0, None)
    if op == 0xB:
        return (2, "halt", 0, 0, 0, None)
    if op == 0xC:
        return (2, "call", 0, 0, 0, dvgdasm.target_of(w))
    if op == 0xD:
        return (2, "end", 0, 0, 0, None)
    if op == 0xE:
        return (2, "jump", 0, 0, 0, dvgdasm.target_of(w))
    unit = {(0, 0): 2, (1, 0): 4, (0, 1): 8, (1, 1): 16}[
        ((w >> 11) & 1, (w >> 3) & 1)]
    dx = (w & 3) * unit
    dy = ((w >> 8) & 3) * unit
    if w & 0x004:
        dx = -dx
    if w & 0x400:
        dy = -dy
    return (2, "vec", dx, dy, (w >> 4) & 0xF, None)


def run(mem, addr, lo, hi, stop=None, limit=None):
    """Execute a display list; -> (segments, steps, truncated).

    A segment is (x0, y0, x1, y1, z).  JSRL recurses, with the beam
    position carrying in and out exactly as the hardware does.

    `stop` bounds the top-level walk at the next block.  Not every shape
    ends in RTSL: the rocks and the ship frames are plain vector lists
    that the 6502 *copies* into the display list, a fixed count at a time
    (the `ROCK` and `SHIP` macros record it as RVCC/RSZB and SHPVCT), so
    there is no terminator to stop at.  Without the bound ROCK0 runs on
    into everything after it and comes out 3000 units wide.
    """
    segs = []
    state = {"x": 0.0, "y": 0.0, "steps": 0, "cut": False, "jumped": False,
             "top": 0}

    def go(a, depth):
        while True:
            if not (lo <= a < hi) or state["steps"] > MAX_STEPS or depth > 6:
                state["cut"] = True
                return
            # `stop` guards against a shape with no terminator running on
            # into the next one.  It must not apply once a JMPL has been
            # taken: that is real control flow to somewhere else in the
            # ROM.  `CHAR.C` is left stroke, top stroke, a blanked move
            # back, then `JMPL UNDERL` for the bottom - bounding the jump
            # threw the bottom stroke away and left an incomplete C.
            if depth == 0 and not state["jumped"]:
                # `limit` is how many instructions the source says this
                # shape runs for.  It is the only thing that can tell a
                # glyph from a rock: CHAR.E is two vectors and then falls
                # through CHAR.F's label into its strokes, while ROCK1 is
                # a new picture even though ROCK0 never terminates.
                # Counting stops once a JMPL is taken - after that we are
                # in someone else's code and run to its terminator.
                if limit is not None and state["top"] >= limit:
                    return
                if limit is None and stop is not None and a >= stop:
                    return
                state["top"] += 1
            state["steps"] += 1
            n, kind, dx, dy, z, tgt = decode_values(mem, a)
            if kind == "vec":
                x0, y0 = state["x"], state["y"]
                state["x"] += dx
                state["y"] += dy
                segs.append((x0, y0, state["x"], state["y"], z))
            elif kind == "labs":
                state["x"], state["y"] = float(dx), float(dy)
            elif kind == "call":
                go(tgt, depth + 1)
            elif kind == "jump":
                state["jumped"] = True
                a = tgt
                continue
            elif kind in ("end", "halt"):
                return
            a += n

    go(addr, 0)

    # A shape is a subroutine, and it brackets its picture with blanked
    # moves: one in from wherever the caller left the beam, and often one
    # back out again.  `SHLDVC` is the clearest case - `VCTR 56,24,0` in
    # and `VCTR -56,-24,0` out, exact negations - and `SHIP17` ends
    # `PVCTR 112,0,0` before its RTSL.  Neither draws anything, but our
    # start point is (0,0), which is nowhere in particular, so drawing
    # them invents lines across the middle of the picture.
    if segs and segs[0][4] == 0:
        segs = segs[1:]

    # Trailing moves need more care.  Where a shape has lit vectors, a
    # blanked one at the end can only be a move - there is nothing after
    # it to draw.  Where the shape is stored entirely blanked, because the
    # 6502 ORs intensity in as it copies (the ship frames), that reasoning
    # does not hold and a trailing vector may be real geometry.  There,
    # only drop one that demonstrably returns the beam to where it came
    # in - which is what makes SHLDVC's pair recognisable.
    has_lit = any(s[4] > 0 for s in segs)
    while segs and segs[-1][4] == 0:
        ends_home = abs(segs[-1][2]) < 1e-6 and abs(segs[-1][3]) < 1e-6
        if has_lit or ends_home:
            segs = segs[:-1]
            if not has_lit:
                break                       # only the one, for a blanked shape
        else:
            break
    return segs, state["steps"], state["cut"]


def parse_blocks(path):
    """-> OrderedDict name -> {addr, word, refs} from the vector listing."""
    blocks, cur = OrderedDict(), None
    head = re.compile(r"^([A-Za-z_$][\w.$]*): \(JSRL \$([0-9A-F]{3})\)")
    for line in open(path, encoding="utf-8"):
        m = head.match(line)
        if m:
            cur = {"name": m.group(1), "word": int(m.group(2), 16),
                   "addr": None, "refs": []}
            blocks[m.group(1)] = cur
            continue
        if cur is None:
            continue
        if cur["addr"] is None:
            m = re.match(r"^([0-9A-F]{4})/", line)
            if m:
                cur["addr"] = int(m.group(1), 16)
        m = re.match(r"^;   refs: (.+)$", line)
        if m and "none found" not in m.group(1):
            cur["refs"].append(m.group(1).strip())
    return OrderedDict((k, v) for k, v in blocks.items() if v["addr"] is not None)


FAMILIES = [
    ("Explosions", lambda n: n.startswith("EXP")),
    ("Rocks", lambda n: n.startswith("ROCK")),
    ("Special rock (TRI)", lambda n: n.startswith("TRI")),
    ("Special rock frames (FRM)", lambda n: n.startswith("FRM")),
    ("Ship", lambda n: n.startswith("SHIP")),
    ("Saucer & shield", lambda n: n in ("SAUCER", "SHLDVC", "RSOURC")),
    ("Glyphs", lambda n: n.startswith("CHAR") or n in ("UNDERL", "Z.5", "JJ98")),
    ("Messages", lambda n: n in ("ASTMSG", "CPMGL", "CIRCL", "ERASE", "ERM",
                                 "VGMSGA", "TFPIX")),
    ("Self-test", lambda n: n in ("TEST1", "BNKERR", "PKYERR")),
]


def svg_for(segs, size=118, blanked=False):
    if not segs:
        return '<div class="empty">no vectors</div>'
    xs = [p for s in segs for p in (s[0], s[2])]
    ys = [p for s in segs for p in (s[1], s[3])]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    w, h = max(x1 - x0, 1e-6), max(y1 - y0, 1e-6)
    span = max(w, h) * 1.14 or 1
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    vb = "%.2f %.2f %.2f %.2f" % (cx - span / 2, cy - span / 2, span, span)
    out = ['<svg viewBox="%s" width="%d" height="%d" '
           'preserveAspectRatio="xMidYMid meet">' % (vb, size, size),
           '<g transform="scale(1,-1) translate(0,%.2f)"' % (-2 * cy),
           ' vector-effect="non-scaling-stroke">']
    r = span / 42.0                      # dot radius, in shape units
    for x0_, y0_, x1_, y1_, z in segs:
        if blanked:
            # Stored blanked, with the 6502 supplying intensity as it
            # copies.  We know it is drawn, so draw it - rendering the
            # ship and the shield as ghosts said more about the storage
            # format than about the shape.
            cls, op, z = "lit", 0.9, 1
        elif z == 0:
            cls, op = "blank", 0.16
        else:
            cls, op = "lit", 0.35 + 0.65 * (z / 15.0)
        if z and x0_ == x1_ and y0_ == y1_:
            # A lit vector of zero length is a DOT, and the game leans on
            # them: an explosion is a scatter of `VCTR 0,0,.BRITE` points
            # between blanked moves, and shots are single dots.  Drawn as
            # a line it would be invisible.
            out.append('<circle class="dot" cx="%.2f" cy="%.2f" r="%.2f" '
                       'opacity="%.2f"/>' % (x0_, y0_, r, op))
        else:
            out.append('<line class="%s" x1="%.2f" y1="%.2f" x2="%.2f" '
                       'y2="%.2f" opacity="%.2f"/>'
                       % (cls, x0_, y0_, x1_, y1_, op))
    out.append("</g></svg>")
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="romset", default="astdelux2")
    args = ap.parse_args()

    dump = "%s_dump.bin" % args.romset
    listing = "%s_vecrom.asm" % args.romset
    out = "%s_shapes.html" % args.romset
    try:
        mem = bytearray(open(dump, "rb").read())
        blocks = parse_blocks(listing)
    except FileNotFoundError as exc:
        sys.exit("%s - run gen_from_roms.py and dvgdasm.py first" % exc)

    lo, hi = 0x4800, 0x5800
    starts = sorted(b["addr"] for b in blocks.values())
    try:
        import astdelux_vecnames as vn
        lengths = vn.SHAPE_LEN
    except (ImportError, AttributeError):
        lengths = {}
    cards, drawn, empty = [], 0, 0
    for name, b in blocks.items():
        nxt = next((a for a in starts if a > b["addr"]), hi)
        segs, steps, cut = run(mem, b["addr"], lo, hi, stop=nxt,
                               limit=lengths.get(b["addr"]))
        lit = sum(1 for s in segs if s[4] > 0)
        dots = sum(1 for s in segs if s[4] > 0 and s[0] == s[2] and s[1] == s[3])
        # A shape with vectors but none lit is not empty - the ship frames
        # are stored blanked and the 6502 ORs the intensity in as it copies
        # them (CPYVEC, "copy and modify vectors").  Say so rather than
        # showing a blank card.
        blanked = bool(segs) and not lit
        if segs:
            drawn += 1
        else:
            empty += 1
        b.update(segs=segs, steps=steps, cut=cut, lit=lit, dots=dots,
                 blanked=blanked)
        cards.append(b)

    grouped, seen = [], set()
    for title, pred in FAMILIES:
        members = [b for b in cards if pred(b["name"]) and b["name"] not in seen]
        seen.update(b["name"] for b in members)
        if members:
            grouped.append((title, members))
    rest = [b for b in cards if b["name"] not in seen]
    if rest:
        grouped.append(("Everything else", rest))

    H = []
    H.append("<title>Asteroids Deluxe — vector shapes</title>")
    H.append("""<style>
:root{--bg:#faf9f7;--fg:#1a1a1a;--dim:#6b6b6b;--line:#e0ddd8;
      --card:#ffffff;--ink:#123;--beam:#0a7d3f;--blank:#b04a2f;}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){
      --bg:#141414;--fg:#ececec;--dim:#9a9a9a;--line:#2c2c2c;
      --card:#1c1c1c;--beam:#48e08a;--blank:#e08a6a;}}
:root[data-theme=dark]{--bg:#141414;--fg:#ececec;--dim:#9a9a9a;
      --line:#2c2c2c;--card:#1c1c1c;--beam:#48e08a;--blank:#e08a6a;}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--fg);margin:0;padding:28px 22px 60px;
     font:14px/1.55 ui-sans-serif,system-ui,-apple-system,Segoe UI,sans-serif}
h1{font-size:21px;margin:0 0 4px;letter-spacing:-.01em}
h2{font-size:14px;margin:34px 0 12px;padding-bottom:6px;
   border-bottom:1px solid var(--line);color:var(--fg);
   text-transform:uppercase;letter-spacing:.08em;font-weight:600}
.sub{color:var(--dim);margin:0 0 18px;max-width:70ch}
.bar{display:flex;gap:14px;align-items:center;flex-wrap:wrap;margin:16px 0 4px}
input[type=search]{background:var(--card);color:var(--fg);border:1px solid var(--line);
   border-radius:7px;padding:7px 11px;font:inherit;min-width:230px}
.stat{color:var(--dim);font-size:13px}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(150px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:9px;
      padding:10px 10px 8px;text-align:center}
.card svg{display:block;margin:0 auto}
.nm{font:600 12px ui-monospace,SFMono-Regular,Menlo,monospace;margin-top:7px;
    word-break:break-all}
.meta{color:var(--dim);font:11px ui-monospace,SFMono-Regular,Menlo,monospace;
      margin-top:2px}
.empty{height:118px;display:flex;align-items:center;justify-content:center;
       color:var(--dim);font-size:11px;font-style:italic}
line{vector-effect:non-scaling-stroke}
line.lit{stroke:var(--beam);stroke-width:1.6}
circle.dot{fill:var(--beam);stroke:none}
line.blank{stroke:var(--blank);stroke-width:1;stroke-dasharray:2 3}
.legend{color:var(--dim);font-size:12.5px;margin:10px 0 0}
.k{display:inline-block;width:11px;height:0;border-top:2px solid var(--beam);
   vertical-align:middle;margin:0 5px}
.k2{border-top:2px dashed var(--blank)}
.hidden{display:none}
</style>""")

    H.append("<h1>Asteroids Deluxe — vector shapes</h1>")
    H.append('<p class="sub">Every display-list block in <code>$4D80–$57FF</code>, '
             'executed the way the DVG would: JSRL is followed into sub-shapes, '
             'and intensity sets opacity. Names come from the Atari vector '
             'source. Generated by <code>mkpreview.py</code>.</p>')
    H.append('<p class="legend"><span class="k"></span> lit vector &nbsp;&nbsp;'
             '<span class="k k2"></span> blanked (<code>z=0</code>) — the beam '
             'moving dark <em>between</em> marks. A shape brackets its picture '
             'with blanked moves in and out of position; those are not drawn, '
             'because they start from wherever the caller left the beam rather '
             'than from anywhere in the shape. Shapes tagged '
             '<code>blanked</code> are stored with <code>z=0</code> throughout '
             'and the 6502 ORs the intensity in as it copies them '
             '(<code>CPYVEC</code>) — they are drawn lit here, because that is '
             'how they reach the screen.</p>')
    H.append('<div class="bar"><input type="search" id="q" '
             'placeholder="filter by name…" autocomplete="off">'
             '<span class="stat">%d shapes · %d with vectors · %d empty · '
             '%d stored blanked</span>'
             '</div>' % (len(cards), drawn, empty,
                         sum(1 for b in cards if b["blanked"])))

    for title, members in grouped:
        H.append('<h2>%s <span style="opacity:.55;font-weight:400">(%d)</span></h2>'
                 % (title, len(members)))
        H.append('<div class="grid">')
        for b in members:
            note = []
            if b["blanked"]:
                note.append("blanked")
            if b["dots"]:
                note.append("%d dot%s" % (b["dots"], "" if b["dots"] == 1 else "s"))
            if b["cut"]:
                note.append("truncated")
            if b["refs"]:
                note.append("%d ref%s" % (len(b["refs"]),
                                          "" if len(b["refs"]) == 1 else "s"))
            H.append('<div class="card" data-name="%s">%s'
                     '<div class="nm">%s</div>'
                     '<div class="meta">$%04X · JSRL $%03X</div>'
                     '<div class="meta">%d vec%s</div></div>'
                     % (b["name"].lower(),
                        svg_for(b["segs"], blanked=b["blanked"]), b["name"],
                        b["addr"], b["word"], len(b["segs"]),
                        (" · " + ", ".join(note)) if note else ""))
        H.append("</div>")

    H.append("""<script>
const q=document.getElementById('q');
q.addEventListener('input',()=>{
  const v=q.value.trim().toLowerCase();
  document.querySelectorAll('.card').forEach(c=>{
    c.classList.toggle('hidden', v && !c.dataset.name.includes(v));
  });
  document.querySelectorAll('h2').forEach(h=>{
    let g=h.nextElementSibling, any=false;
    if(g) g.querySelectorAll('.card').forEach(c=>{if(!c.classList.contains('hidden'))any=true;});
    h.classList.toggle('hidden',!any); if(g) g.classList.toggle('hidden',!any);
  });
});
</script>""")

    open(out, "w", encoding="utf-8").write("\n".join(H) + "\n")
    print("wrote %s" % out)
    print("  shapes: %d  (%d with vectors, %d empty, %d stored blanked)"
          % (len(cards), drawn, empty,
             sum(1 for b in cards if b["blanked"])))
    truncated = [b["name"] for b in cards if b["cut"]]
    if truncated:
        print("  truncated (recursion or step limit): %s"
              % ", ".join(truncated[:8]))


if __name__ == "__main__":
    main()
