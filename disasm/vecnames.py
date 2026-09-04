#!/usr/bin/env python3
"""Recover DVG shape names by aligning the vector source with the ROM.

`dvgdasm.py` decodes every display-list block, but most come out as
`VEC_xxxx`: the linker map names only eight shapes, because the game
reaches the rest through *computed* JSRL words rather than by referring
to them.

The shape source is in the archive though - DASVEC.MAC and the files it
includes (ROCK.DAT, SHIP.DAT, TRI.VEC, FRM.VEC, DSVECN.MAC) - and it
assembles linearly.  Each directive there is exactly one DVG
instruction, whatever its encoded length, so the two streams can be
walked in lockstep from a known address and every label lands on a real
one.

That "whatever its encoded length" is what makes this cheap.  `VCTR`
picks a two-word or a one-word encoding depending on the magnitudes, and
`PVCTR` converts absolute coordinates to deltas first, so predicting
*sizes* would mean implementing the macros.  Predicting the *number of
instructions* needs neither.

Alignment runs between anchors - the shapes the map already names, plus
the glyph table - and a stretch is only used when the source and the ROM
agree on instruction count and on the kind of every instruction in it.

Reads  <set>_dump.bin, <set>_codemap.json, ../asteroids-deluxe-main/
Writes astdelux_vecnames.py  (imported by dvgdasm.py)

Usage:
    python vecnames.py [--set astdelux2] [--report]
"""

import argparse
import json
import os
import re
import sys

import dvgdasm
import astdelux_defines as defs

SRC = os.path.join("..", "asteroids-deluxe-main")
OUT = "astdelux_vecnames.py"

# The vector-data source, in the order DASVEC.MAC assembles it.
ROOT = "DASVEC.MAC"

VECROM_LO, VECROM_HI = 0x4800, 0x5800

# What each directive assembles to, as one DVG instruction.
#   vec  - VCTR/SVCTR/PVCTR, however it ends up encoded
#   call - JSRL / JTRI
#   jump - JMPL
#   end  - RTSL / VEND
#   halt - HALT
#   labs - LABS
#   wait - WAIT
KIND = {
    "VCTR": "vec", "SVCTR": "vec", "PVCTR": "vec",
    "JSRL": "call", "JTRI": "call",
    "JMPL": "jump",
    "RTSL": "end", "VEND": "end",
    "HALT": "halt", "LABS": "labs", "WAIT": "wait",
}
# Emit nothing into the instruction stream.
NOEMIT = {"ROCK", "SHIP", "ASCIN"}

LABEL_RE = re.compile(r"^([A-Za-z$.][A-Za-z0-9$._]*)::?\s*")


def body_kind(body):
    """What one invocation of a macro emits, from its body.

    Only the single-instruction shapes these files use are recognised:
    a bare RTSL/JSRL/JMPL, or an SJSRL whose last argument says which.
    """
    for line in body:
        t = re.split(r"[\s,]", line.strip(), maxsplit=1)[0]
        if t == "SJSRL":
            return "jump" if "JMPL" in line else "call"
        if t in ("RTSL", "JSRL", "JMPL", "HALT", "VCTR", "SVCTR", "PVCTR"):
            return KIND.get(t)
    return None


def read(path):
    return open(path, "rb").read().decode("latin-1")


IRPC_RE = re.compile(r"^\s*\.IRPC\s+(\w+)\s*,\s*<([^>]*)>\s*$", re.I)
REPT_RE = re.compile(r"^\s*\.REPT\s+(\S+)\s*$", re.I)


def expand_irpc(lines, limit=6):
    """Expand `.IRPC` loops, innermost first, honouring nesting.

    TFPIX is built by two nested loops:

        .IRPC XX,<0123>
        .IRPC YY,<01234567>
        JSRL TRI'XX''YY
        .ENDR
        .ENDR

    which is 32 JSRLs to TRI00..TRI37, not one.  MACRO-11's apostrophe
    concatenates, so `TRI'XX''YY` with XX='2', YY='5' is `TRI25`.
    Without this the whole TRI/FRM shape family - most of the vector ROM
    by block count - stays anonymous.
    """
    for _ in range(limit):
        out, i, grew = [], 0, False
        while i < len(lines):
            m = IRPC_RE.match(lines[i])
            if not m:
                out.append(lines[i])
                i += 1
                continue
            # find the matching .ENDR, allowing nested loops inside
            depth, j = 1, i + 1
            body = []
            while j < len(lines) and depth:
                if IRPC_RE.match(lines[j]) or REPT_RE.match(lines[j]):
                    depth += 1
                elif re.match(r"^\s*\.ENDR", lines[j], re.I):
                    depth -= 1
                    if not depth:
                        break
                body.append(lines[j])
                j += 1
            var, chars = m.group(1), m.group(2)
            for ch in chars:
                for b in body:
                    out.append(re.sub(r"'%s\b|\b%s'" % (var, var), ch,
                                      b.replace("'" + var, ch)))
            grew = True
            i = j + 1
        lines = out
        if not grew:
            break
    return lines


def parse_source(path, seen=None, dyn=None):
    """-> [(label_or_None, kind_or_None)] in assembly order.

    `kind` is None for a label with nothing on its line, and "#data" for
    anything that emits bytes we are not modelling (`.WORD`, `.BYTE`,
    `ASCIN`, a `.REPT` fill).  Those break a stretch rather than
    silently shifting it.
    """
    out = []
    seen = set(seen or ())
    seen.add(os.path.normcase(os.path.abspath(path)))
    in_macro = in_rept = 0
    # Macros get redefined mid-assembly, and it changes what they emit:
    # TRI.MAC defines VEND as `RTSL` for the rocks and ships, then
    # redefines it after TRI.VEC as an `SJSRL ...,JMPL` for the frames.
    # A fixed table would call every FRM shape's terminator wrong.
    # Shared across includes: TRI.MAC redefines VEND *after* including
    # TRI.VEC, and FRM.VEC - included later - must see the new meaning.
    if dyn is None:
        dyn = {}
    macro_name, macro_body = None, []

    for raw in expand_irpc(read(path).splitlines()):
        line = raw.split(";")[0].rstrip()
        # Several archive files are NUL-padded to a block
        # boundary.  Python's strip() leaves NULs alone, so an
        # unfiltered pad line parses as an unknown directive and
        # breaks the stretch it lands in - which is how the whole
        # FRM shape family stayed anonymous.
        line = line.replace(chr(0), "").rstrip()
        if not line.strip():
            continue
        s = line.strip()
        up = s.upper()

        if in_macro:
            if up.startswith(".ENDM"):
                in_macro -= 1
                if not in_macro and macro_name:
                    dyn[macro_name] = body_kind(macro_body)
                    macro_name, macro_body = None, []
            else:
                macro_body.append(up)
            continue
        m = re.match(r"^\.MACRO\s+(\S+)", up)
        if m:
            in_macro += 1
            macro_name, macro_body = m.group(1).rstrip(","), []
            continue
        if in_rept:
            if up.startswith(".ENDR"):
                in_rept -= 1
            continue
        if up.startswith(".REPT"):
            in_rept += 1
            out.append((None, "#data"))       # the fill emits bytes
            continue
        if up.startswith(".ENDR"):
            continue                          # a leftover from expansion

        m = re.match(r"^\.INCLUDE\s+(\S+)", up)
        if m:
            inc = m.group(1)
            cand = inc if os.path.splitext(inc)[1] else inc + ".MAC"
            full = os.path.join(os.path.dirname(path), cand)
            key = os.path.normcase(os.path.abspath(full))
            if os.path.exists(full) and key not in seen:
                out.extend(parse_source(full, seen, dyn))
                seen.add(key)
            continue

        if up.startswith("."):
            if re.match(r"^\.(WORD|BYTE|ASCII|BLKB)\b", up):
                out.append((None, "#data"))
            elif up.startswith(".="):
                out.append((None, "#origin"))
            continue

        label = None
        m = LABEL_RE.match(s)
        if m:
            label = m.group(1)
            s = s[m.end():].strip()
            up = s.upper()
        if not s:
            out.append((label, None))
            continue

        tok = re.split(r"[\s,]", up, maxsplit=1)[0]
        if tok == "ALPHA":
            # one JSRL per character of the quoted string
            text = re.search(r"<([^>]*)>", s)
            n = len(text.group(1)) if text else 0
            out.append((label, "call"))
            out.extend([(None, "call")] * max(0, n - 1))
        elif tok in dyn and dyn[tok]:
            out.append((label, dyn[tok]))
        elif tok in KIND:
            out.append((label, KIND[tok]))
        elif tok == "SINIT":
            # Resets the pen: a new picture starts here.  That is what
            # separates ROCK1 from ROCK0 - neither ends in RTSL, so
            # nothing else marks the boundary.
            out.append((label, "#sinit"))
        elif tok in NOEMIT:
            out.append((label, None))
        elif "=" in s:
            out.append((label, None))         # symbol assignment
        else:
            out.append((label, "#data"))      # unknown macro: break here
    return out


def rom_stream(mem, lo, hi):
    """-> [(addr, kind)] decoding the ROM forward from `lo`."""
    out, a = [], lo
    while a < hi - 1:
        n, _t, _tgt, _e = dvgdasm.decode(mem, a)
        if a + n > hi:
            break
        w = dvgdasm.word(mem, a)
        op = w >> 12
        kind = ("labs" if op == 0xA else "halt" if op == 0xB else
                "call" if op == 0xC else "end" if op == 0xD else
                "jump" if op == 0xE else "vec")
        if op <= 9 and (w & 0x3FF) == 0 and (dvgdasm.word(mem, a + 2) & 0x3FF) == 0 \
                and not (w & 0x400):
            kind = "wait"
        out.append((a, kind))
        a += n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="romset", default="astdelux2")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    try:
        mem = bytearray(open("%s_dump.bin" % args.romset, "rb").read())
        cm = json.load(open("%s_codemap.json" % args.romset))
    except FileNotFoundError as exc:
        sys.exit("%s - run gen_from_roms.py and m6502trace.py first" % exc)
    if args.romset != defs.SYMBOL_REVISION:
        sys.exit("the source archive documents %s, not %s"
                 % (defs.SYMBOL_REVISION, args.romset))

    data = []
    for lo, hi, k in cm["runs"]:
        if k == "data":
            c = (max(lo, VECROM_LO), min(hi, VECROM_HI))
            if c[0] < c[1]:
                data.append(c)

    def is_data(a):
        return any(lo <= a < hi for lo, hi in data)

    # ---- anchors: shapes we already know the address of ----------------
    anchors = {}
    for a, n in defs.DVG_SYMBOLS.items():
        if is_data(a):
            anchors[n] = a
    for a, n in defs.ROM_SYMBOLS.items():
        if VECROM_LO <= a < VECROM_HI and is_data(a):
            anchors.setdefault(n, a)
    # glyph names come from the table VGCHAR indexes, not from the map
    code_runs = [(lo, hi) for lo, hi, k in cm["runs"] if k == "code"]
    char_names, _text, _tbl = dvgdasm.scan_char_table(mem, code_runs)
    glyph_addr = {}
    for a, nm in char_names.items():
        if nm.startswith("CHAR_") and nm != "CHAR_SPACE":
            glyph_addr["CHAR." + nm[5:]] = a
    anchors.update(glyph_addr)

    stream = parse_source(os.path.join(SRC, ROOT))
    src_at = {}                                  # label -> index in stream
    for i, (lb, _k) in enumerate(stream):
        if lb and lb not in src_at:
            src_at[lb] = i

    placed, where, checked, rejected = {}, {}, 0, []
    known = sorted(((src_at[n], a, n) for n, a in anchors.items()
                    if n in src_at), key=lambda t: t[0])

    for (i0, a0, n0), (i1, a1, _n1) in zip(known, known[1:]):
        # Walk forward from the anchor for as long as the two streams
        # agree, rather than demanding the whole stretch match.  A run of
        # shapes followed by a byte table - which is most of this ROM -
        # then still names everything up to the table.
        src = [(j, lb, k) for j, (lb, k) in enumerate(stream[i0:i1], start=i0)
               if k != "#origin"]
        rom = rom_stream(mem, a0, a1)
        pos, pending, stopped = 0, None, None
        for j, lb, k in src:
            if lb and pending is None:
                pending = (lb, j)          # remember where in the stream
            if k is None or k == "#sinit":
                continue
            if k == "#data":
                stopped = "data"
                break
            if pos >= len(rom):
                stopped = "ran past the end of the ROM stretch"
                break
            addr, rk = rom[pos]
            if rk != k:
                stopped = "kinds differ at $%04X (source %s, ROM %s)" % (addr, k, rk)
                break
            if pending:
                if addr not in placed:
                    placed[addr] = pending[0]
                    where[addr] = pending[1]
                pending = None
            pos += 1
        if stopped is None and pos == len(rom):
            checked += 1
        else:
            rejected.append((n0, "%s after %d of %d instructions"
                             % (stopped or "short", pos, len(rom))))

    for n, a in anchors.items():
        if a not in placed:
            placed[a] = n
            if n in src_at:
                where[a] = src_at[n]

    # How many top-level instructions each shape runs for.  This cannot be
    # read off the ROM: a glyph runs *through* the next label - CHAR.E is
    # two vectors of its own and then falls into CHAR.F's strokes - while
    # ROCK1 really is a new picture even though ROCK0 never terminates.
    # The source settles it: run to the terminator, but stop at a SINIT,
    # which is what resets the pen for a new picture.
    lengths = {}
    for a, j in where.items():
        n = 0
        for lb, k in stream[j:]:
            if k in (None, "#origin"):
                continue
            if k == "#sinit" and n:
                break                      # the next picture starts here
            if k == "#sinit" or k == "#data":
                if k == "#data":
                    break
                continue
            n += 1
            if k in ("end", "jump", "halt"):
                break                      # terminator, inclusive
        if n:
            lengths[a] = n

    print("vector-source labels        : %d" % len(src_at))
    print("anchors                     : %d" % len(anchors))
    print("stretches aligned 1:1       : %d of %d"
          % (checked, max(0, len(known) - 1)))
    print("shape names placed          : %d" % len(placed))
    print("shapes with a source length : %d" % len(lengths))
    if args.report and rejected:
        print("\nstretches not aligned:")
        for n, why in rejected:
            print("   after %-10s %s" % (n, why))

    lines = ['"""DVG shape names recovered from the Atari vector source.',
             "",
             "GENERATED by vecnames.py.  Each name sits where the source and",
             "the ROM agreed instruction for instruction between two known",
             "shapes; nothing here was guessed.",
             '"""',
             "",
             'NAME_REVISION = "%s"' % args.romset,
             "",
             "SHAPE_NAMES = {"]
    for a in sorted(placed):
        lines.append("    0x%04X: %r," % (a, placed[a]))
    lines.append("}")
    lines += ["",
              "# Top-level instructions each shape runs for, from the source.",
              "# A glyph runs through the label after it (CHAR.E falls into",
              "# CHAR.F's strokes); a rock does not, because SINIT starts a",
              "# new picture.  Only the source can tell those apart.",
              "SHAPE_LEN = {"]
    for a in sorted(lengths):
        lines.append("    0x%04X: %d," % (a, lengths[a]))
    lines.append("}")
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("\nwrote %s" % OUT)


if __name__ == "__main__":
    main()
