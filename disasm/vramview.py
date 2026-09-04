#!/usr/bin/env python3
"""Render a vector-RAM dump the way the DVG would draw it.

The C port (../c_src) builds a real display list in its 2 KB model of
vector RAM ($4000-$47FF) instead of calling a draw routine.  That is the
strongest test available - the words can be compared with a hardware
dump - but a hex diff says nothing about whether the frame *looks*
right.  This takes such a dump, runs it from word address 0 exactly as
the hardware does after GO, follows JMPL/JSRL/RTSL through RAM and the
vector ROM until HALT, and writes the result as one SVG frame in the
arcade's 1024x870 beam space.

What it reuses, and what it does not:

  - dvgdasm.decode gives the trace text, so `--trace` prints the same
    columns as astdelux2_vecrom.asm and a reviewer can diff the two.
  - mkpreview's conventions are kept - Y up, z as opacity, a zero-length
    lit vector drawn as a dot - but not its code.  mkpreview draws each
    shape at LABS scale 0 and fits the picture to its bounding box; a
    frame needs the LABS global scale honoured (the rocks are sized with
    it) and a fixed viewport, so the geometry is worked out here, in
    `delta`, following the rule the MAME DVG uses.
  - astdelux_vecnames names the shapes in `--list`, when it is present.

The list is guarded: an instruction cap, a stack cap, a check that no
address is executed twice in the same call frame (the DVG has no
conditionals, so that is always a loop), and a check that fetches stay
inside RAM+ROM.  A bad list is reported, not hung on.

Usage:
    python vramview.py vram.bin [-o out.html] [--rom astdelux2_dump.bin]
                                [--start 0] [--trace] [--list]
"""

import argparse
import sys

import dvgdasm

VECRAM, VECRAM_END = 0x4000, 0x4800
VECROM_LO, VECROM_HI = dvgdasm.VECROM_LO, dvgdasm.VECROM_HI
WIDTH, HEIGHT = 1040, 950   # the visible field is x 0..1040, y 70..950 (AAE_DRIVER_SCREEN)
MAX_STEPS = 20000
MAX_DEPTH = 8                       # the hardware stack is 4 deep


def delta(w1, w2, scale):
    """-> (dx, dy, z) in beam units for a VCTR or SVEC word pair.

    The DVG adds the LABS scale to the instruction's own scale and draws
    magnitude * 2**(sum - 9); a sum above 9 wraps to a shift of 10.  An
    SVEC is the same rule with the two-bit counts in bits 9-8 of a
    ten-bit field, and its unit bits (11 and 3) feeding the scale.
    """
    op = w1 >> 12
    if op == 0xF:
        dy, dx = (w1 >> 8) & 3, w1 & 3
        dy, dx = dy << 8, dx << 8
        if w1 & 0x400:
            dy = -dy
        if w1 & 0x004:
            dx = -dx
        s = 2 + ((w1 >> 2) & 2) + ((w1 >> 11) & 1)
        z = (w1 >> 4) & 0xF
    else:
        dy, dx, z = w1 & 0x3FF, w2 & 0x3FF, w2 >> 12
        if w1 & 0x400:
            dy = -dy
        if w2 & 0x400:
            dx = -dx
        s = op
    s = (s + scale) & 0xF
    shift = 10 if s > 9 else 9 - s
    return dx / float(1 << shift), dy / float(1 << shift), z


def run(mem, start):
    """Execute from byte address `start`; -> (segments, trace, calls, report).

    A segment is (x0, y0, x1, y1, z).  `trace` is one (addr, raw, text)
    per instruction executed; `calls` the JSRL targets in the order first
    seen, with counts.  `report` is None for a list that HALTed cleanly,
    otherwise says what went wrong.
    """
    segs, trace, calls = [], [], {}
    x = y = 0.0
    scale = 0
    a = start
    stack = []                     # (return address, visited-set of that frame)
    seen = set()
    steps = 0
    while True:
        if not (VECRAM <= a < VECROM_HI) or a & 1:
            return segs, trace, calls, "fetch from $%04X, outside RAM+ROM" % a
        if a in seen:
            return segs, trace, calls, ("loop: $%04X executed twice in the same "
                                        "call frame (no HALT reached)" % a)
        if steps >= MAX_STEPS:
            return segs, trace, calls, "runaway: %d instructions, no HALT" % steps
        seen.add(a)
        steps += 1
        n, text, tgt, _ends = dvgdasm.decode(mem, a)
        w1 = dvgdasm.word(mem, a)
        raw = " ".join("%04X" % dvgdasm.word(mem, a + i) for i in range(0, n, 2))
        trace.append((a, raw, text))
        op = w1 >> 12
        if op <= 9 or op == 0xF:
            dx, dy, z = delta(w1, dvgdasm.word(mem, a + 2) if n == 4 else 0, scale)
            segs.append((x, y, x + dx, y + dy, z))
            x, y = x + dx, y + dy
        elif op == 0xA:
            w2 = dvgdasm.word(mem, a + 2)
            x, y = float(w2 & 0x3FF), float(w1 & 0x3FF)
            scale = w2 >> 12
        elif op == 0xB:
            return segs, trace, calls, None
        elif op == 0xC:
            calls[tgt] = calls.get(tgt, 0) + 1
            if len(stack) >= MAX_DEPTH:
                return segs, trace, calls, "JSRL nested %d deep at $%04X" % (len(stack), a)
            stack.append((a + n, seen))
            seen = set()
            a = tgt
            continue
        elif op == 0xD:
            if not stack:
                return segs, trace, calls, "RTSL at $%04X with an empty stack" % a
            a, seen = stack.pop()
            continue
        elif op == 0xE:
            a = tgt
            continue
        a += n


def svg_for(segs):
    out = ['<svg viewBox="0 0 %d %d" width="%d" height="%d">'
           % (WIDTH, HEIGHT, WIDTH, HEIGHT),
           '<rect width="%d" height="%d" class="crt"/>' % (WIDTH, HEIGHT),
           '<g transform="translate(0,%d) scale(1,-1)">' % HEIGHT]
    for x0, y0, x1, y1, z in segs:
        if z == 0:
            continue                          # a blanked move: the beam is dark
        op = 0.35 + 0.65 * (z / 15.0)
        if x0 == x1 and y0 == y1:
            out.append('<circle cx="%.2f" cy="%.2f" r="2.2" opacity="%.2f"/>'
                       % (x0, y0, op))
        else:
            out.append('<line x1="%.2f" y1="%.2f" x2="%.2f" y2="%.2f" '
                       'opacity="%.2f"/>' % (x0, y0, x1, y1, op))
    out.append("</g></svg>")
    return "\n".join(out)


def shape_names():
    try:
        import astdelux_vecnames as vn
        return dict(vn.SHAPE_NAMES)
    except ImportError:
        return {}


def load_memory(vram_path, rom_path):
    vram = open(vram_path, "rb").read()
    if len(vram) != VECRAM_END - VECRAM:
        sys.exit("%s: expected %d bytes of vector RAM, got %d"
                 % (vram_path, VECRAM_END - VECRAM, len(vram)))
    rom = open(rom_path, "rb").read()
    if len(rom) == VECROM_HI - VECROM_LO:
        rom = bytes(VECROM_LO) + rom          # a bare vector ROM
    if len(rom) < VECROM_HI:
        sys.exit("%s: too short to hold $%04X-$%04X" % (rom_path, VECROM_LO, VECROM_HI - 1))
    mem = bytearray(VECROM_HI)
    mem[VECRAM:VECRAM_END] = vram
    mem[VECROM_LO:VECROM_HI] = rom[VECROM_LO:VECROM_HI]
    return mem


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("vram", help="2048-byte dump of $4000-$47FF")
    ap.add_argument("-o", "--out", help="HTML to write (default: <vram>.html)")
    ap.add_argument("--rom", default="astdelux2_dump.bin",
                    help="64 KB address-space image, or a bare 4 KB vector ROM")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=0,
                    help="DVG word address to start at (default 0)")
    ap.add_argument("--trace", action="store_true",
                    help="print every instruction executed, in listing format")
    ap.add_argument("--list", action="store_true",
                    help="print the JSRL targets hit, with shape names")
    args = ap.parse_args()

    mem = load_memory(args.vram, args.rom)
    names = shape_names()
    out = args.out or (args.vram.rsplit(".", 1)[0] + ".html")

    segs, trace, calls, report = run(mem, VECRAM + (args.start << 1))
    if args.trace:
        for a, raw, text in trace:
            if "$" in text and text.split()[0] in ("JSRL", "JMPL"):
                t = dvgdasm.target_of(dvgdasm.word(mem, a))
                text += "   ; -> %s" % names.get(t, "VEC_%04X" % t)
            print("%04X/%03X:  %-10s %s" % (a, (a - VECRAM) >> 1, raw, text))
    if args.list:
        for t, n in calls.items():
            print("  JSRL $%04X (word $%03X)  %-12s x%d"
                  % (t, (t - VECRAM) >> 1, names.get(t, "VEC_%04X" % t), n))

    lit = [s for s in segs if s[4] > 0]
    dots = sum(1 for s in lit if s[0] == s[2] and s[1] == s[3])
    status = report or "HALT reached"
    H = ["<title>Asteroids Deluxe — vector RAM frame</title>",
         "<style>",
         "body{background:#0b0d10;color:#c9cfd6;margin:0;padding:18px;"
         "font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}",
         "svg{display:block;max-width:100%;height:auto;border:1px solid #2a3038}",
         ".crt{fill:#000}",
         "line{stroke:#d8f0ff;stroke-width:1.5;stroke-linecap:round}",
         "circle{fill:#d8f0ff}",
         ".bad{color:#ff8a6a}",
         "</style>",
         "<p>%s · start word $%03X · %d instructions · %d lit vectors "
         "(%d dots) · %d JSRL targets · <span%s>%s</span></p>"
         % (args.vram, args.start, len(trace), len(lit), dots, len(calls),
            ' class="bad"' if report else "", status),
         svg_for(segs)]
    open(out, "w", encoding="utf-8").write("\n".join(H) + "\n")

    print("wrote %s" % out)
    print("  %d instructions, %d lit vectors (%d dots), %d JSRL targets - %s"
          % (len(trace), len(lit), dots, len(calls), status))
    if report:
        sys.exit(2)


if __name__ == "__main__":
    main()
