#!/usr/bin/env python3
"""Recover routine names by matching the Atari source against the listing.

The linker map only exports `.GLOBL` symbols - 92 of them - so most
routines come out of the trace as `SUB_xxxx`.  But the archive holds the
actual MACRO-65 source that built this ROM, and every routine in it has a
name.  The problem is only that the source has no addresses.

Rather than write a MAC65 assembler, this matches on *shape*: for each
label in the source, take the sequence of mnemonics that follows it, and
look for the one address in the listing whose instructions run in the
same order.  Operands are ignored, so none of the hard parts - macro
argument expansion, expression evaluation, zero-page vs absolute
selection, forward references - have to be solved.  A run of eight or
more opcodes is essentially unique in 12K of ROM.

A name is accepted only when the match is unique in both directions: the
source signature matches exactly one address, and no other source label
matches that same address.  Ambiguous ones are reported, not guessed.

Placement then proceeds in three widening passes, each feeding the next:

  1. unique whole-ROM signature match
  2. interpolation - an unplaced label between two placed ones can only
     lie between their addresses, and in that window even a three-opcode
     signature is usually unique
  3. harvest from proved alignment - once a stretch of source has been
     matched to a stretch of listing one instruction to one instruction,
     every label inside it sits at a known address, however short or
     generic its own signature

Two invariants guard the result, both hard failures:

  - the 92 addresses in DSTRD0.MAP are re-derived and must not be
    contradicted
  - within one contiguous stretch of one module the matched addresses
    must strictly increase, because an assembler emits labels in order

The second is also used as a *constraint*, not just a test: where a few
placements contradict it the longest consistent run is kept and the rest
dropped, since a generic signature (RTS.0 is a bare `rts`) can match in
more than one place.

Reads  <set>_dump.bin, <set>_codemap.json, ../asteroids-deluxe-main/
Writes astdelux_names.py - names, instruction comments, routine header
       blocks and .SBTTL section titles, all imported by m6502trace.py

Usage:
    python nameroutines.py [--set astdelux2] [--min-ops 8] [--report]
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict

import m6502
import astdelux_config as config
import astdelux_defines as defs

SRC = os.path.join("..", "asteroids-deluxe-main")
OUT = "astdelux_names.py"

# Modules that contribute 6502 code to the linked program, in link order
# (SHUFFL is just .CSECT ordering; the vector/diagnostic files are not
# part of this build).
# The files DASTRO.DOC lists as linked into DSTRD0, minus the ones that
# contribute no 6502 code (DASVEC is vector data, SHUFFL is .CSECT
# ordering, CKSUM is checksum directives).  DCIN65 and TRI are NOT here:
# they are .INCLUDEd, so they are spliced into their includer instead.
MODULES = ["TRIROT.MAC", "DSTMSG.MAC", "BOUNCE.MAC", "DASOUN.MAC",
           "DSFILL.MAC", "DSTNMI.MAC", "DSTCUT.MAC", "EAROM.MAC",
           "DSTTST.MAC", "DSTRD0.MAC"]

# Build-time conditionals.  This is the shipped Deluxe Asteroids ROM, so
# the abandoned $DTHST ("Death Star") variant is off, and DEVSYS is the
# development-system harness.
COND = {"$DAST": 1, "$DTHST": 0, "DEVSYS": 0, "$DTHS": 0}

MNEMONICS = {m for m, _mode, _u in m6502.OPCODES}

# Macros that synthesize an instruction out of raw bytes, so their bodies
# cannot simply be re-parsed: LXL/LAH/LAL/LXH are `.BYTE 0A9 / .WORD ADD /
# .=.-1`, where the .= trims the operand back to a single byte.  Each
# assembles to one immediate load.
#
# Every other parameterless macro is expanded from its real body instead
# (see expand_macros) - counting their instructions by hand was wrong for
# GDM (4, not 3) and GBAM (6, not 3), and would have stayed wrong for
# GCM, whose `AND` sits behind a `.IF NE,MULTS`.
CODE_MACROS = {
    "LXL": ["ldx"], "LXH": ["ldx"], "LAH": ["lda"], "LAL": ["lda"],
}

# Parameterless macros this parser interprets itself rather than expands.
MACRO_KEEP = {"ENTSEC", "XITSEC"}


def expand_macros(lines, limit=8):
    """Inline parameterless `.MACRO` bodies before parsing.

    The bodies are ordinary source - conditionals and all - so expanding
    them textually lets the rest of the parser evaluate them in the
    caller's context, which is what GCM needs.
    """
    bodies = {}
    out, i = [], 0
    while i < len(lines):
        m = re.match(r"^\s*\.MACRO\s+(\S+)\s*(.*)$", lines[i], re.I)
        if m and not m.group(2).strip():
            name, body, j = m.group(1).upper(), [], i + 1
            while j < len(lines) and not re.match(r"^\s*\.ENDM", lines[j], re.I):
                body.append(lines[j])
                j += 1
            if name not in MACRO_KEEP:
                bodies[name] = body
            out.extend(lines[i:j + 1])          # keep the definition in place
            i = j + 1
            continue
        out.append(lines[i])
        i += 1

    # An invocation can carry a label and a comment: DCIN65.MAC has
    # `$BONUS: GBAM  ;GET BONUS ADDER MODE`.  Keep the label on a line of
    # its own so it still marks the right address, and move the comment
    # onto the first instruction the body emits.
    inv = re.compile(r"^(\s*)((?:[A-Za-z$.][A-Za-z0-9$.]*|\d+\$)::?\s*)?"
                     r"([A-Za-z][A-Za-z0-9$.]*)\s*$")
    for _ in range(limit):
        grew, nxt = False, []
        for line in out:
            code, sep, comment = line.partition(";")
            m = inv.match(code.rstrip())
            name = m.group(3).upper() if m else None
            if name in bodies:
                if m.group(2):
                    nxt.append(m.group(1) + m.group(2).rstrip())
                body = list(bodies[name])
                if sep and body:
                    for k, b in enumerate(body):
                        if ";" not in b and b.strip():
                            body[k] = b + "\t;" + comment
                            break
                nxt.extend(body)
                grew = True
            else:
                nxt.append(line)
        out = nxt
        if not grew:
            break
    return out

LABEL_RE = re.compile(r"^([A-Za-z$.][A-Za-z0-9$.]*|\d+\$)::?\s*")
LOCAL_RE = re.compile(r"^(\d+)\$$")
ASSIGN_RE = re.compile(r"^([A-Za-z$.][A-Za-z0-9$.]*)\s*==?\s*([^;]+)$")
UNRESOLVED = []                    # conditions with no recorded answer

# Conditions on `.`, the assembler's location counter.  These cannot be
# evaluated without assembling, so each is answered here with its reason.
# There are only six in the whole archive; anything new is reported
# rather than guessed.
LOCATION_CONDITIONS = {
    # DSTCUT.MAC's guard idiom:  `.IIF NDF,VGHEXZ,VGHEXZ=.` immediately
    # followed by `.IF EQ,VGHEXZ-.`.  The .IIF defines the symbol as the
    # current location, so the difference is zero by construction and the
    # routine is assembled.
    ("EQ", "VGHEXZ-."): True,
    ("EQ", "VGVCTR-."): True,

    # PG0123.MAC's page-zero overflow assert.  Its body is `.ERROR`,
    # which emits no bytes, so the answer cannot affect the listing.
    ("GT", ".-^H100"): False,

    # DCIN65.MAC branch-range guard: when the loop end is more than 126
    # bytes from $DETCT a relative branch cannot reach, so the source
    # emits `BMI 120$ / JMP $DETCT` instead of a single `BPL $DETCT`.
    # The ROM settles it - $7935 reads `bmi $BONUS` followed by
    # `jmp $DETCT`, which is the long form.
    ("GT", ".-$DETCT-126."): True,

    # Inside a macro body, so never assembled where they appear.
    ("EQ", "...N-1"): False,
    ("EQ", "...N-2"): False,
}


def split_items(operand):
    """Split a directive's operand list on top-level commas.

    `<>` groups and quoted strings can contain commas of their own, so a
    plain split would over-count the items and get the size wrong.
    """
    items, depth, cur, quote = [], 0, "", None
    for ch in operand:
        if quote:
            cur += ch
            if ch == quote:
                quote = None
            continue
        if ch in "'\"":
            quote, cur = ch, cur + ch
        elif ch == "<":
            depth += 1; cur += ch
        elif ch == ">":
            depth -= 1; cur += ch
        elif ch == "," and depth == 0:
            items.append(cur); cur = ""
        else:
            cur += ch
    if cur.strip():
        items.append(cur)
    return [i for i in items if i.strip()]


def data_values(tok, line, syms):
    """The bytes a data directive emits, with None for the unknown ones.

    Label arithmetic (`.BYTE 10$-L0`) and forward references cannot be
    evaluated here, so those positions become wildcards.  Literals - which
    most of these tables are - come out exactly, and that is what lets a
    table be located in the ROM by its contents.
    """
    operand = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
    operand = operand.split(";")[0]
    items = split_items(operand)
    out = []
    if tok == ".BYTE":
        for it in items:
            v = evaluate(it, syms)
            out.append(None if v is None else v & 0xFF)
    elif tok == ".WORD":
        for it in items:
            v = evaluate(it, syms)
            out.extend((None, None) if v is None else (v & 0xFF, (v >> 8) & 0xFF))
    elif tok == ".BLKB":
        n = evaluate(operand, syms) if operand.strip() else 1
        out = [None] * (n or 0)
    return out


def data_size(tok, line, syms):
    """Bytes a data directive emits, or None when it cannot be counted."""
    operand = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ""
    operand = operand.split(";")[0]
    if tok == ".BYTE":
        return len(split_items(operand)) or None
    if tok == ".WORD":
        n = len(split_items(operand))
        return 2 * n if n else None
    if tok == ".BLKB":
        return evaluate(operand, syms) if operand.strip() else 1
    return None                       # .ASCII/.RAD50 and friends: not counted


def evaluate(expr, syms):
    """Evaluate a MACRO-11 expression, or None if a symbol is unknown.

    Radix is 16 (DSTDEC sets it), a trailing '.' means decimal, '<>' group,
    '!' is logical OR and '&' is AND.  Only what these sources actually
    use is supported - `OFFSET*<MECHS-1>` and `COIN67!COIN01` are the
    shapes that matter.
    """
    e = expr.strip().replace("<", "(").replace(">", ")").replace("!", "|")

    def tok(m):
        t = m.group(0)
        if t.startswith("^H"):
            return str(int(t[2:], 16))
        if t.endswith("."):
            return str(int(t[:-1], 10))
        if re.fullmatch(r"[0-9][0-9A-Fa-f]*", t):
            return str(int(t, 16))
        if t in syms:
            return str(syms[t])
        raise KeyError(t)

    try:
        py = re.sub(r"\^H[0-9A-Fa-f]+|[0-9][0-9A-Fa-f]*\.?|[A-Za-z$.][A-Za-z0-9$.]*",
                    tok, e)
        return int(eval(py, {"__builtins__": {}}, {}))
    except Exception:
        return None


def condition(op, expr, syms):
    """Truth of a MACRO-11 conditional, or None when it cannot be decided."""
    op = op.upper()
    if op in ("DF", "NDF"):
        known = expr.strip() in syms
        return known if op == "DF" else not known
    val = evaluate(expr, syms)
    if val is None:
        key = (op, expr.strip())
        if key in LOCATION_CONDITIONS:
            return LOCATION_CONDITIONS[key]
        UNRESOLVED.append(key)
        return None
    return {"NE": val != 0, "EQ": val == 0, "GT": val > 0, "LT": val < 0,
            "GE": val >= 0, "LE": val <= 0}[op]


# ---------------------------------------------------------------------------
# source parsing
# ---------------------------------------------------------------------------

def read(path):
    return open(path, "rb").read().decode("latin-1")


def parse_module(path, _seen=None, syms=None):
    """-> [Stmt] in source order, with .INCLUDE files spliced in place.

    `.INCLUDE` is not a reference, it is textual insertion: DSTNMI.MAC's
    `.INCLUDE DCIN65` puts the whole coin routine inside the NMI handler,
    which is why parsing DCIN65 as a module of its own left a 138
    instruction hole in the middle of NMI.  DSTDEC and PG0123 are pulled
    in the same way and contribute no instructions, but PG0123's `.=100`
    / `.=200` do move the location counter - correctly, since the
    including file re-origins itself afterwards.

    Each record carries what is needed both for shape matching (label,
    mnemonic) and for carrying the source's own documentation across
    (the trailing comment, any standalone comment block that precedes a
    label, and the last .SBTTL heading).  Data directives break the
    instruction chain, so they are recorded as a barrier.
    """
    out = []
    stack = []                     # nested .IF state
    in_macro = 0
    in_rept0 = 0
    block = []                     # standalone ; lines awaiting a label
    banner = None                  # last .SBTTL heading
    seen = set(_seen or ())
    seen.add(os.path.normcase(os.path.abspath(path)))
    # Assembly-time symbols.  An included file inherits what its includer
    # has already defined - DSTNMI.MAC sets EMCTRS/OFFSET/CNTINT before
    # `.INCLUDE DCIN65`, and DCIN65 fills the rest in with `.IIF NDF`.
    if syms is None:
        syms = dict(COND)

    def emit(label=None, mnem=None, comment="", size=None, values=None):
        nonlocal block, banner
        rec = {"label": label, "mnem": mnem, "comment": comment, "size": size,
               "values": values,
               "block": block if label else [], "banner": banner if label else None}
        if label:
            block, banner = [], None
        out.append(rec)

    for raw in expand_macros(read(path).splitlines()):
        full = raw.rstrip()
        comment = ""
        if ";" in full:
            comment = full.split(";", 1)[1].strip()
        line = full.split(";")[0].rstrip()
        # Several archive files are NUL-padded to a block
        # boundary.  Python's strip() leaves NULs alone, so an
        # unfiltered pad line parses as an unknown directive and
        # breaks the stretch it lands in - which is how the whole
        # FRM shape family stayed anonymous.
        line = line.replace(chr(0), "").rstrip()

        if not line.strip():
            # a standalone comment line: keep it for the next label, but
            # drop the ruled-off banners the source uses as separators
            if comment and not set(comment) <= set("*-=_ "):
                block.append(comment)
            elif not comment:
                block = []
            continue
        s = line.strip()
        up = s.upper()

        if up.startswith(".SBTTL"):
            banner = s[6:].strip().strip("*").strip()
            continue

        m = re.match(r"^\.INCLUDE\s+(\S+)", up)
        if m and all(active for _c, active in stack) and not in_macro                 and not in_rept0:
            inc = m.group(1)
            cand = inc if os.path.splitext(inc)[1] else inc + ".MAC"
            full_path = os.path.join(os.path.dirname(path), cand)
            key = os.path.normcase(os.path.abspath(full_path))
            if os.path.exists(full_path) and key not in seen:
                out.extend(parse_module(full_path, seen, syms))
                seen.add(key)
            continue

        # .REPT 0 ... .ENDR is how this source embeds prose blocks
        if in_rept0:
            if up.startswith(".ENDR"):
                in_rept0 -= 1
            continue
        if re.match(r"^\.REPT\s+0\s*$", up):
            in_rept0 += 1
            continue

        # macro definitions are not code where they are written
        if in_macro:
            if up.startswith(".ENDM"):
                in_macro -= 1
            continue
        if up.startswith(".MACRO"):
            in_macro += 1
            continue

        # Conditionals.  MACRO-11 has a full set inside one .IF block:
        # .IFT assembles when the condition held, .IFF when it did not,
        # and .IFTF in both cases.  Each *sets* the state - it does not
        # toggle it - so the original condition has to be remembered for
        # the whole block.  Getting this wrong is not subtle in its
        # effect but is silent: two .IFF sections in one block used to
        # flip assembly back off and swallow ~90 lines of TRIROT.MAC,
        # RELTIP and RNDXYI among them.
        m = re.match(r"^\.IF\s+(NE|EQ|GT|LT|GE|LE|DF|NDF)\s*,\s*(.+)$", s,
                     re.I)
        if m:
            truth = condition(m.group(1), m.group(2), syms)
            if truth is None:
                truth = False      # undecidable: assume not assembled
            stack.append([truth, truth])       # [condition, active now]
            continue
        if up.startswith(".IFTF"):             # before .IFT / .IFF: prefix
            if stack:
                stack[-1][1] = True
            continue
        if up.startswith(".IFF"):
            if stack:
                stack[-1][1] = not stack[-1][0]
            continue
        if up.startswith(".IFT"):
            if stack:
                stack[-1][1] = stack[-1][0]
            continue
        if up.startswith(".ENDC"):
            if stack:
                stack.pop()
            continue
        if not all(active for _cond, active in stack):
            continue
        m = re.match(r"^\.IIF\s+(NE|EQ|GT|LT|GE|LE|DF|NDF)\s*,\s*(.+)$", s,
                     re.I)
        if m:
            # `.IIF NDF,MECHS,MECHS=3` - condition, then the statement to
            # assemble if it holds.  Splitting on the first comma after the
            # condition expression is enough for these sources.
            rest = m.group(2)
            if "," in rest:
                cexpr, tail = rest.split(",", 1)
                if condition(m.group(1), cexpr, syms):
                    a = ASSIGN_RE.match(tail.strip())
                    if a:
                        v = evaluate(a.group(2), syms)
                        if v is not None:
                            syms[a.group(1)] = v
                    else:
                        t = re.split(r"[\s,]", tail.strip().upper(),
                                     maxsplit=1)[0]
                        if t.lower() in MNEMONICS:
                            emit(mnem=t.lower(), comment=comment)
            continue

        # split off the label
        label = None
        m = LABEL_RE.match(s)
        if m and not s.startswith("."):
            label = m.group(1)
            s = s[m.end():].strip()
            up = s.upper()

        if label is not None and not s:
            emit(label=label, comment=comment)
            continue
        pending_label = label

        tok = re.split(r"[\s,]", up, maxsplit=1)[0]

        if tok.startswith("."):
            if pending_label:
                emit(label=pending_label)
            # data directives emit bytes; anything else is bookkeeping.
            if tok in (".BYTE", ".WORD", ".ASCII", ".ASCIZ", ".BLKB", ".RAD50"):
                vals = data_values(tok, s, syms)
                # CKSUM.MAC links last and patches each checksum byte in
                # place, so what the source writes there (`.BYTE 0`) is
                # never what the ROM holds.  Treat those as unknown.
                if pending_label and pending_label.upper().startswith("CKSUM"):
                    vals = [None] * len(vals)
                emit(mnem="#data", size=data_size(tok, s, syms),
                     comment=comment, values=vals)
            elif tok in (".CSECT", ".ASECT") or up.startswith(".="):
                # A section or origin change moves the location counter, so
                # what follows is a new contiguous stretch.  These files do
                # switch mid-file: DSTMSG.MAC carries the DSTMSG, ASTMTS and
                # ASTM sections, and DSTRD0.MAC also holds CASTST.
                emit(mnem="#origin")
            elif tok == ".END":
                emit(mnem="#data")
            continue

        a = ASSIGN_RE.match(s)
        if a and not pending_label:
            v = evaluate(a.group(2), syms)
            if v is not None:
                syms[a.group(1)] = v
            continue
        if ("=" in s.split()[0]) if s.split() else False:
            if pending_label:
                emit(label=pending_label)
            continue                          # symbol assignment

        mn = tok.lower()
        if mn in MNEMONICS:
            emit(label=pending_label, mnem=mn, comment=comment)
        elif tok in CODE_MACROS:
            for i, x in enumerate(CODE_MACROS[tok]):
                emit(label=pending_label if i == 0 else None, mnem=x,
                     comment=comment if i == 0 else "")
        elif tok == "SND":
            args = s.split(None, 1)[1] if len(s.split(None, 1)) > 1 else ""
            parts = [p.strip() for p in args.split(",")]
            emit(label=pending_label, mnem="ldy", comment=comment)
            emit(mnem="jmp" if len(parts) > 2 and parts[2] else "jsr")
        elif tok in ("ENTSEC", "XITSEC"):
            # DSTRD0.MAC uses these to park the location counter in the
            # space left over after TRIROT and come back: ENTSEC saves `.`
            # and jumps to ...NPC, XITSEC restores it.  So the module
            # feeds two interleaved streams, each contiguous on its own.
            if pending_label:
                emit(label=pending_label)
            emit(mnem="#" + tok.lower())
        elif tok in ("JSRL", "JMPL", "RTSL", "HALT", "VCTR", "SVCTR", "LABS",
                     "WAIT", "ALPHA", "ASCIN", "STB", "REP", "SEND", "MEND",
                     "CT", "ITXY", "SN", "ASC", "LET", "NUM", "DTB"):
            emit(label=pending_label, mnem="#data")
        else:
            emit(label=pending_label, mnem="#data")   # unknown macro
    return out


def signatures(stmts, module):
    """-> [(name, [mnemonics...], is_local)] in source order.

    MACRO-11 local labels (`10$`, `2$`) restart inside every routine, so
    the bare number means nothing on its own; each is qualified with the
    routine it sits in - `10$` inside START becomes START_10.  They are
    marked local because their signatures are far too short and generic
    to search for across the whole ROM; they get placed by their
    neighbours instead (see interpolate).

    Source order matters: it is what brackets a short routine between two
    placed ones, and what the ordering check tests against.
    """
    out = []
    scope = None
    depth = 0                       # 0 = the module's own stream, 1 = the
    seg = 0                         # parked ...NPC stream
    for i, rec in enumerate(stmts):
        label, _mn = rec["label"], rec["mnem"]
        if _mn == "#entsec":
            depth += 1
        elif _mn == "#xitsec":
            depth = max(0, depth - 1)
        elif _mn == "#origin":
            seg += 1
        # Every record, not just the labelled ones: the comment alignment
        # slices this list between two labels, and that slice must not be
        # allowed to run through a stretch the location counter had
        # jumped away to.
        rec["unit"] = (depth, seg)
        if label is None:
            continue
        local = bool(LOCAL_RE.match(label))
        if local:
            if scope is None:
                continue                      # nothing to qualify it with
            name = "%s_%s" % (scope, LOCAL_RE.match(label).group(1))
        else:
            scope = label
            name = label
        ops = []
        for nxt in stmts[i:]:
            mn = nxt["mnem"]
            if mn is None:
                continue                      # another label at the same spot
            if mn.startswith("#"):
                break
            ops.append(mn)
            if len(ops) >= 24:
                break
        rec["qname"] = name
        if ops:
            out.append((name, ops, local, (depth, seg), i))
    return out


# ---------------------------------------------------------------------------
# listing side
# ---------------------------------------------------------------------------

def listing_signatures(mem, code_runs, depth=24):
    """-> {address: [mnemonics...]} for every instruction boundary."""
    starts = {}
    for lo, hi in code_runs:
        a = lo
        boundaries = []
        while a < hi:
            ins = m6502.decode(mem, a)
            if a + ins["size"] > hi:
                break
            boundaries.append((a, ins["mnem"]))
            a += ins["size"]
        for i, (addr, _mn) in enumerate(boundaries):
            starts[addr] = [mn for _a, mn in boundaries[i:i + depth]]
    return starts


# ---------------------------------------------------------------------------

def matches(ops, listing, n, lo=None, hi=None):
    """Addresses whose first `n` opcodes are `ops[:n]`, optionally windowed."""
    probe = ops[:n]
    out = set()
    for addr, seq in listing.items():
        if lo is not None and not (lo < addr < hi):
            continue
        if len(seq) >= n and seq[:n] == probe:
            out.add(addr)
    return out


def resolve(modules, listing, min_ops):
    """Match source labels to addresses; keep only mutually unique hits."""
    cand = defaultdict(set)                   # label -> {addresses}
    for _module, labels in modules:
        for label, ops, local, _ix in labels:
            if local or len(ops) < min_ops:
                continue
            n = max(min_ops, min(len(ops), 12))
            cand[label] |= matches(ops, listing, n)

    by_addr = defaultdict(set)
    for label, addrs in cand.items():
        for a in addrs:
            by_addr[a].add(label)

    resolved, ambiguous = {}, {}
    for label, addrs in cand.items():
        if len(addrs) != 1 or len(by_addr[next(iter(addrs))]) != 1:
            ambiguous[label] = addrs
        else:
            resolved[next(iter(addrs))] = label
    return resolved, ambiguous


def interpolate(modules, listing, resolved, floor=3):
    """Place short routines using the addresses of their neighbours.

    A routine only three opcodes long has a signature that repeats all
    over 12K of ROM, so the global pass cannot place it.  But the
    assembler emits a module in source order, so an unplaced label
    between two placed ones can only lie between their addresses.  Inside
    that window a short signature is usually unique.

    Runs to a fixed point, because each placement narrows the windows
    around it.
    """
    taken = dict(resolved)                    # addr -> label
    placed = {lb for lb in resolved.values()}
    added = 0
    changed = True
    while changed:
        changed = False
        for _module, labels in modules:
            known = [i for i, (lb, _o, _l, _x) in enumerate(labels) if lb in placed]
            addr_of = {lb: a for a, lb in taken.items()}
            for idx, (label, ops, _local, _x) in enumerate(labels):
                if label in placed:
                    continue
                before = [i for i in known if i < idx]
                after = [i for i in known if i > idx]
                if not before or not after:
                    continue
                lo = addr_of[labels[before[-1]][0]]
                hi = addr_of[labels[after[0]][0]]
                if not lo < hi:
                    continue
                n = max(floor, min(len(ops), 12))
                if len(ops) < floor:
                    continue
                hits = matches(ops, listing, n, lo, hi)
                hits = {a for a in hits if a not in taken}
                if len(hits) == 1:
                    a = next(iter(hits))
                    taken[a] = label
                    placed.add(label)
                    added += 1
                    changed = True
    return taken, added


def place_data_by_content(units, resolved, mem, data_runs, min_known=4):
    """Locate data tables in the ROM by what they contain.

    Anchoring a table to the routine after it only works when source order
    and address order agree locally, and the linker interleaves sections -
    ATANA is followed in the source by a `.CSECT` switch, not by the code
    that follows it in memory.

    So match on contents instead.  A run of data directives assembles to a
    known byte pattern, with wildcards where a value depends on an address
    (`.BYTE 10$-L0`).  Searching the listing's data regions for that
    pattern places the whole run at once, and every label inside it with
    it.  A run is used only when it has at least `min_known` concrete
    bytes and matches in exactly one place.
    """
    placed, ambiguous = [], 0
    for _name, unit, stmts, key in units:
        i = 0
        while i < len(stmts):
            r = stmts[i]
            if r.get("unit") != key or r["mnem"] != "#data" \
                    or r.get("values") is None:
                i += 1
                continue
            # gather one maximal run of data directives
            run, labels, off = [], [], 0
            j, pending = i, None
            # `VGMSGS: .BYTE 068,0B6` puts the label in a record of its own,
            # just before the run starts - so seed from what precedes it,
            # or the first (and usually most interesting) name is lost.
            k = i - 1
            while k >= 0 and stmts[k]["mnem"] is None \
                    and stmts[k].get("unit") == key:
                if stmts[k].get("qname"):
                    pending = stmts[k]["qname"]
                    break
                k -= 1
            while j < len(stmts):
                q = stmts[j]
                if q.get("unit") != key:
                    break
                if q["mnem"] is None:               # label on its own line
                    pending = pending or q.get("qname")
                    j += 1
                    continue
                if q["mnem"] != "#data" or q.get("values") is None:
                    break
                name = q.get("qname") or pending
                pending = None
                if name:
                    labels.append((name, off))
                run.extend(q["values"])
                off += len(q["values"])
                j += 1
            i = max(j, i + 1)
            known = [k for k, v in enumerate(run) if v is not None]
            if len(known) < min_known or not labels:
                continue

            hits = []
            for lo, hi in data_runs:
                for base in range(lo, hi - len(run) + 1):
                    if all(mem[base + k] == run[k] for k in known):
                        hits.append(base)
                        if len(hits) > 1:
                            break
                if len(hits) > 1:
                    break
            if len(hits) != 1:
                ambiguous += 1
                continue
            base = hits[0]
            for name, o in labels:
                a = base + o
                if a not in resolved and name not in set(resolved.values()):
                    resolved[a] = name
                    placed.append((a, name))
    return placed, ambiguous


def place_data_labels(units, resolved, is_data):
    """Name the data tables, by measuring backwards from the code after them.

    A data table has no opcodes, so none of the signature machinery can
    find it.  But `.BYTE` / `.WORD` / `.BLKB` sizes *are* computable, and
    a table almost always ends where the next routine begins.  So from a
    placed code label at `addrB`, walk back through the run of data
    directives that precede it in the source, adding up bytes: a
    directive that leaves the running total at T starts at addrB - T.

    The run is only accepted when every byte of [addrB-total, addrB) is
    data in the listing too.  That is a real check, not a formality - if
    the sizes were wrong, or the source and the ROM disagreed about what
    sits before that routine, the span would collide with code and be
    rejected.
    """
    placed = []
    for _name, unit, stmts, key in units:
        by_ix = {ix: lb for lb, _o, _l, ix in unit}
        addr_of = {lb: a for a, lb in resolved.items()}
        for ix, lb in sorted(by_ix.items()):
            addrB = addr_of.get(lb)
            if addrB is None or stmts[ix]["mnem"] in (None, "#data"):
                continue                       # anchor must be real code
            total, found, j = 0, [], ix - 1
            while j >= 0:
                r = stmts[j]
                if r.get("unit") != key:
                    break
                mn = r["mnem"]
                if mn is None:                 # a label on its own line
                    if r.get("qname"):
                        found.append((r["qname"], total))
                    j -= 1
                    continue
                if mn != "#data" or r.get("size") is None:
                    break
                total += r["size"]
                if r.get("qname"):
                    found.append((r["qname"], total))
                j -= 1
            if not total or not found:
                continue
            start = addrB - total
            if start < 0 or not all(is_data(a) for a in range(start, addrB)):
                continue
            for name, off in found:
                a = addrB - off
                if a not in resolved and name not in set(resolved.values()):
                    resolved[a] = name
                    placed.append((a, name))
    return placed


def prune_out_of_order(modules, resolved):
    """Drop placements that cannot be right because they break source order.

    Within one stretch of one module the assembler emits labels in order,
    so the matched addresses must increase.  When a few placements
    contradict that, the honest reading is that those few are wrong - not
    that the invariant is.  So keep the longest strictly increasing
    subsequence of each unit and discard the rest.

    A generic signature is what usually causes this: `RTS.0` is a bare
    `rts`, and there is more than one place in 12K of ROM where an rts is
    followed by the same handful of opcodes.

    Returns the labels dropped.
    """
    addr_of = {lb: a for a, lb in resolved.items()}
    dropped = []
    for _module, labels in modules:
        seq = [(lb, addr_of[lb]) for lb, _o, _l, _x in labels if lb in addr_of]
        if len(seq) < 2:
            continue
        # longest strictly increasing subsequence by address, O(n^2) - the
        # units are small enough that clarity beats cleverness here
        best = [1] * len(seq)
        prev = [-1] * len(seq)
        for i in range(len(seq)):
            for j in range(i):
                if seq[j][1] < seq[i][1] and best[j] + 1 > best[i]:
                    best[i], prev[i] = best[j] + 1, j
        end = max(range(len(seq)), key=lambda i: best[i])
        keep, i = set(), end
        while i != -1:
            keep.add(i)
            i = prev[i]
        for i, (lb, a) in enumerate(seq):
            if i not in keep:
                dropped.append((lb, a))
                del resolved[a]
    return dropped


def order_violations(modules, resolved):
    """Labels whose matched addresses run backwards within their module.

    An assembler emits a module's labels in source order, so within one
    module the matched addresses must increase.  This is an independent
    check on the matching - a signature that latched onto the wrong
    routine almost always lands out of sequence.

    The check is strict: within one stream the addresses must increase,
    with no tolerance.  That is only sound because ENTSEC/XITSEC streams
    are separated first (see signatures); without that, the module's two
    address ranges would look like violations of each other.
    """
    addr_of = {lb: a for a, lb in resolved.items()}
    bad = []
    for module, labels in modules:
        seq = [(lb, addr_of[lb]) for lb, _o, _l, _x in labels if lb in addr_of]
        for (l1, a1), (l2, a2) in zip(seq, seq[1:]):
            if a2 <= a1:
                bad.append((module, l1, a1, l2, a2))
    return bad


def _unused_order_violations(sigs, resolved):
    """Labels whose matched addresses run backwards within their module.

    An assembler emits a module's labels in source order, so within one
    module the matched addresses must increase monotonically.  This is an
    independent check on the matching: a signature that latched onto the
    wrong routine will almost always land out of sequence.  (A module can
    be split across sections, so a drop is only counted when it is not
    also a jump to a different part of the ROM.)
    """
    by_module = defaultdict(list)
    addr_of = {lb: a for a, lb in resolved.items()}
    for label, variants in sigs.items():
        if label not in addr_of:
            continue
        for module, _ops in variants:
            by_module[module].append((label, addr_of[label]))
            break

    bad = []
    for module, items in by_module.items():
        seq = [(lb, a) for lb, a in items]
        # keep source order: sigs was built by walking each file in order,
        # and dicts preserve insertion order
        for (l1, a1), (l2, a2) in zip(seq, seq[1:]):
            if a2 < a1 and (a1 - a2) < 0x400:
                bad.append((module, l1, a1, l2, a2))
    return bad


def align_comments(units, resolved, instrs):
    """Carry the source's own comments onto the instructions they describe.

    Two placed labels bracket a stretch of code.  Inside it, the source's
    instructions and the listing's instructions must correspond one to
    one - same count, same opcodes, in order.  When they do, the
    correspondence is not a guess and each comment can be attached to the
    exact byte it was written about.  When they do not, the stretch is
    skipped entirely rather than sliding comments onto the wrong lines.

    Returns (instruction comments, label blocks, banners, stats).
    """
    addr_of = {lb: a for a, lb in resolved.items()}
    order = sorted(instrs)                      # addresses, ascending
    pos = {a: i for i, a in enumerate(order)}

    inline, blocks, banners, found = {}, {}, {}, {}
    ok, misaligned = 0, []

    for _name, unit, stmts, key in units:
        module = _name.split("[")[0].replace(".MAC", "")
        placed = [(lb, addr_of[lb], ix) for lb, _o, _l, ix in unit
                  if lb in addr_of]
        placed.sort(key=lambda t: t[2])         # source order

        for n, (label, addr, ix) in enumerate(placed):
            rec = stmts[ix]
            if rec.get("block"):
                blocks[addr] = rec["block"]
            if rec.get("banner"):
                banners[addr] = rec["banner"]

            # bound this stretch by the next placed label, in both worlds
            if n + 1 < len(placed):
                nxt_ix, nxt_addr = placed[n + 1][2], placed[n + 1][1]
            else:
                nxt_ix, nxt_addr = len(stmts), None

            # A label written on a line of its own produces a record with
            # no mnemonic.  Those are not instructions, so they cannot be
            # part of the 1:1 correspondence - but the name belongs to the
            # instruction that follows, so carry it forward rather than
            # dropping it with the record.
            src, carried = [], None
            for r in stmts[ix:nxt_ix]:
                if r.get("unit") != key:
                    continue
                mn = r["mnem"]
                if mn is None:
                    carried = carried or r.get("qname")
                    continue
                if mn.startswith("#"):
                    carried = None
                    continue
                if carried and not r.get("qname"):
                    r = dict(r, qname=carried)
                carried = None
                src.append(r)
            if not src:
                continue

            i = pos.get(addr)
            if i is None:
                continue
            dst = []
            for a in order[i:]:
                if nxt_addr is not None and a >= nxt_addr:
                    break
                dst.append(a)
                if nxt_addr is None and len(dst) >= len(src):
                    break

            if len(dst) != len(src) or \
                    any(instrs[a] != r["mnem"] for a, r in zip(dst, src)):
                misaligned.append((label, addr, len(src), len(dst)))
                continue
            for a, r in zip(dst, src):
                if r["comment"]:
                    inline[a] = r["comment"]
                # Inside a stretch that lined up one to one, position is
                # proved - so a label here sits at a known address even if
                # its signature was too short or too generic to search for.
                if r.get("qname") and a != addr:
                    found.setdefault(a, (r["qname"], module))
            ok += 1

    return inline, blocks, banners, found, (ok, misaligned)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="romset", default="astdelux2")
    ap.add_argument("--min-ops", type=int, default=8,
                    help="shortest opcode run accepted as a signature")
    ap.add_argument("--report", action="store_true",
                    help="list what was matched and what stayed ambiguous")
    args = ap.parse_args()

    dump = "%s_dump.bin" % args.romset
    cmap = "%s_codemap.json" % args.romset
    try:
        mem = bytearray(open(dump, "rb").read())
        cm = json.load(open(cmap))
    except FileNotFoundError as exc:
        sys.exit("%s - run gen_from_roms.py and m6502trace.py first" % exc)
    if args.romset != defs.SYMBOL_REVISION:
        sys.exit("the source archive documents %s, not %s - names from it "
                 "would be wrong here" % (defs.SYMBOL_REVISION, args.romset))

    code_runs = [(lo, hi) for lo, hi, k in cm["runs"] if k == "code"]
    listing = listing_signatures(mem, code_runs)

    modules, units = [], []
    for mod in MODULES:
        path = os.path.join(SRC, mod)
        if not os.path.exists(path):
            continue
        stmts = parse_module(path)
        labels = signatures(stmts, mod)
        # One unit per stream.  Each is contiguous and increasing on its
        # own, so neither interpolation nor the ordering check ever spans
        # the jump between the two address ranges.
        for key in sorted({k for _n, _o, _l, k, _i in labels}):
            unit = [(n, o, l, ix) for n, o, l, s, ix in labels if s == key]
            # A label name that repeats inside one stretch cannot be told
            # apart from its twin, so neither is usable.
            seen = [n for n, _o, _l, _i in unit]
            unit = [(n, o, l, i) for n, o, l, i in unit if seen.count(n) == 1]
            if unit:
                stream, seg = key
                name = "%s%s[%d]" % (mod, "(parked)" if stream else "", seg)
                modules.append((name, unit))
                units.append((name, unit, stmts, key))
    n_labels = sum(len(v) for _m, v in modules)
    n_local = sum(1 for _m, v in modules for _lb, _o, loc, _x in v if loc)

    resolved, ambiguous = resolve(modules, listing, args.min_ops)
    n_global = len(resolved)
    resolved, n_interp = interpolate(modules, listing, resolved)
    dropped = prune_out_of_order(modules, resolved)

    # ---- validate against the linker map ------------------------------
    agree = wrong = novel = 0
    wrong_list = []
    for addr, name in sorted(resolved.items()):
        known = defs.ROM_SYMBOLS.get(addr)
        if known is None:
            novel += 1
        elif known == name:
            agree += 1
        else:
            wrong += 1
            wrong_list.append((addr, known, name))

    print("source labels with code after them    : %d  (%d local)"
          % (n_labels, n_local))
    print("placed by unique whole-ROM signature   : %d" % n_global)
    print("placed by neighbour interpolation      : %d" % n_interp)
    print("matched to a unique address           : %d" % len(resolved))
    print("  agree with the linker map           : %d" % agree)
    print("  contradict the linker map           : %d" % wrong)
    print("  new names the map did not have      : %d" % novel)
    if UNRESOLVED:
        seen_u = sorted(set(UNRESOLVED))
        print("NOTE: %d condition(s) on the location counter have no recorded"
              % len(seen_u))
        print("      answer and were assumed not assembled; add them to")
        print("      LOCATION_CONDITIONS with a reason:")
        for op, expr in seen_u:
            print("        .IF %-4s %s" % (op, expr))
    print("dropped for breaking source order      : %d" % len(dropped))
    print("left ambiguous                        : %d" % len(ambiguous))

    bad_order = order_violations(modules, resolved)
    print("source-order violations               : %d" % len(bad_order))
    if bad_order:
        print("\nFAILED - these labels matched addresses that run backwards")
        print("relative to their source order, so at least one is wrong:")
        for mod, l1, a1, l2, a2 in bad_order[:20]:
            print("   %-12s %-8s $%04X  then  %-8s $%04X" % (mod, l1, a1, l2, a2))
        return 1

    # A contradiction is not a warning: it means the matcher is unsound.
    # The map and the source describe the same build, so they cannot
    # disagree about an address unless the method is broken.
    real_conflicts = [(a, k, n) for a, k, n in wrong_list
                      if not aliased(a, k, n)]
    if real_conflicts:
        print("\nFAILED - the matcher disagrees with the linker map at %d "
              "address(es):" % len(real_conflicts))
        for a, k, n in real_conflicts[:20]:
            print("   $%04X  map says %-8s matcher says %s" % (a, k, n))
        return 1
    if wrong_list:
        print("\n%d address(es) where map and source name the same spot "
              "differently (both correct - a section name vs the routine's "
              "own label):" % len(wrong_list))
        for a, k, n in wrong_list:
            print("   $%04X  map %-8s  source %s" % (a, k, n))

    if args.report:
        print("\nnew names, by address:")
        for a in sorted(resolved):
            if a not in defs.ROM_SYMBOLS:
                print("   $%04X  %s" % (a, resolved[a]))
        if ambiguous:
            print("\nambiguous (not used):")
            for lb, addrs in sorted(ambiguous.items())[:40]:
                where = ("no match" if not addrs else
                         ", ".join("$%04X" % x for x in sorted(addrs)[:4]))
                print("   %-8s %s" % (lb, where))

    instrs = {a: seq[0] for a, seq in listing.items()}

    # Alignment and naming feed each other: every label placed adds an
    # anchor, which brackets more stretches, which aligns more code, which
    # exposes more labels at proved positions.  Run it to a fixed point.
    harvested = 0
    owners = {}
    for _round in range(12):
        inline, blocks, banners, found, (ok, misaligned) = align_comments(
            units, resolved, instrs)
        before = len(resolved)
        # A name may legitimately appear in two modules - MACRO-11 labels
        # are file-local unless declared `::`, and both TRIROT.MAC and
        # DSTRD0.MAC define an `RTS.0`.  Qualify the second one by its
        # module rather than letting the two block each other; the rest
        # of the pipeline keys labels by name, so they must stay unique.
        used = {n: a for a, n in resolved.items()}
        fresh = {}
        for a, (n, m) in sorted(found.items()):
            if a in resolved:
                continue
            name = n
            if used.get(n, a) != a:
                name = "%s_%s" % (m, n)
                if used.get(name, a) != a:
                    continue
            fresh[a] = name
            used[name] = a
        owners.update({a: m for a, (_n, m) in found.items()})
        resolved.update(fresh)
        prune_out_of_order(modules, resolved)
        # A label the prune threw out is not gone for good: its neighbours
        # may since have been placed, and the bracket they form is far
        # tighter than the whole-ROM search that mis-placed it.  Give it
        # another go before deciding it has no home.
        resolved, _again = interpolate(modules, listing, resolved)
        prune_out_of_order(modules, resolved)
        harvested += len(resolved) - before
        if len(resolved) == before and not fresh:
            break
    data_addrs = set()
    for lo, hi, k in cm["runs"]:
        if k == "data":
            data_addrs.update(range(lo, hi))
    data_runs = [(lo, hi) for lo, hi, k in cm["runs"] if k == "data"]
    data_named = place_data_labels(units, resolved, lambda a: a in data_addrs)
    by_content, amb = place_data_by_content(units, resolved, mem, data_runs)
    data_named += by_content

    inline, blocks, banners, found, (ok, misaligned) = align_comments(
        units, resolved, instrs)

    if harvested:
        print("placed by proved alignment             : %d" % harvested)
        prune_out_of_order(modules, resolved)
        bad = order_violations(modules, resolved)
        if bad:
            print("\nFAILED - alignment-harvested names break source order:")
            for mod, l1, a1, l2, a2 in bad[:20]:
                print("   %-12s %-8s $%04X  then  %-8s $%04X"
                      % (mod, l1, a1, l2, a2))
            return 1
    print("\ncomments carried across from the source:")
    print("  stretches aligned 1:1                  : %d" % ok)
    print("  stretches skipped (no exact alignment) : %d" % len(misaligned))
    print("  instruction comments                   : %d" % len(inline))
    print("  routine comment blocks                 : %d" % len(blocks))
    print("  section headings (.SBTTL)              : %d" % len(banners))
    print("data tables named                        : %d  (%d by content "
          "match, %d ambiguous and skipped)"
          % (len(data_named), len(by_content), amb))

    if args.report and misaligned:
        print("\n  not aligned, so their comments were left out")
        print("  (source vs listing instruction counts):")
        for lb, a, ns, nd in misaligned:
            print("     %-14s $%04X  source %d, listing %d" % (lb, a, ns, nd))

    # Names placed by hand, each with its evidence (astdelux_config.py
    # says why the automatic placement cannot reach them).  Merged after
    # the automatic pass so they can never displace a recovered name,
    # and held to the same in-module order rule, since a hand-placed
    # label that breaks source order is a wrong address, not a special
    # case.
    hand = config.for_set(config.HAND_NAMES, args.romset)
    for a, (name, module, why) in sorted(hand.items()):
        if a in resolved and resolved[a] != name:
            print("FAILED - hand name %s at $%04X, but the source placed %s "
                  "there" % (name, a, resolved[a]))
            return 1
        if name in resolved.values() and resolved.get(a) != name:
            print("FAILED - hand name %s at $%04X is already placed at $%04X"
                  % (name, a, [x for x, n in resolved.items() if n == name][0]))
            return 1
        resolved[a] = name
        owners[a] = module
        blocks[a] = ["placed by hand: " + why]
    if hand:
        bad = order_violations(modules, resolved)
        if bad:
            print("\nFAILED - hand-placed names break source order:")
            for mod, l1, a1, l2, a2 in bad[:20]:
                print("   %-12s %-8s $%04X  then  %-8s $%04X"
                      % (mod, l1, a1, l2, a2))
            return 1
        print("placed by hand, with evidence          : %d" % len(hand))

    dupes = defaultdict(list)
    for a, n in resolved.items():
        dupes[n].append(a)
    renamed = 0
    for n, addrs in dupes.items():
        if len(addrs) < 2:
            continue
        for a in addrs:
            mod = owners.get(a)
            if mod:
                resolved[a] = "%s_%s" % (mod, n)
                renamed += 1
    if renamed:
        print("names qualified by module (same name in two files): %d"
              % renamed)

    # Which source module each name came from, so the notes can group by
    # subsystem without re-deriving it.
    for _nm, unit, stmts, key in units:
        module = _nm.split("[")[0].replace(".MAC", "")
        for lb, _o, _l, _ix in unit:
            for a, n in resolved.items():
                if n == lb:
                    owners.setdefault(a, module)

    write_names(resolved, args.romset, inline, blocks, banners, owners)
    print("\nwrote %s (%d names, %d comments)"
          % (OUT, len(resolved), len(inline)))
    return 0


def aliased(addr, map_name, src_name):
    """True when the map and the source legitimately differ at one address.

    Three ways that happens, all benign:

      - LINKM lists a section at the address of its first routine, so the
        map may carry the section name where the source has the routine
        label (CASTST section at $7BF5, whose first routine is INIT).
      - The map sometimes records both names at one address; either is
        right.
      - LINKM truncates symbols to six characters, so a map name can be a
        prefix of the real one (STEARO for STEAROM).

    Anything else is a genuine disagreement about the same build, which
    would mean the matcher is unsound.
    """
    if map_name in getattr(defs, "SECTIONS", ()):
        return True
    if src_name in getattr(defs, "ROM_ALIASES", {}).get(addr, ()):
        return True
    if len(map_name) == 6 and src_name.startswith(map_name):
        return True
    return False


def write_names(resolved, romset, inline=None, blocks=None,
                banners=None, owners=None):
    lines = [
        '"""Routine names and comments recovered from the Atari source.',
        "",
        "GENERATED by nameroutines.py, which matched each source label's",
        "opcode sequence against the listing; only mutually unique matches",
        "are here.  Regenerate with",
        "    python nameroutines.py --report",
        "",
        "These name the same build the linker map does (%s); see" % romset,
        "mkdefines.py for why that matters.",
        '"""',
        "",
        'NAME_REVISION = "%s"' % romset,
        "",
        "ROUTINE_NAMES = {",
    ]
    for a in sorted(resolved):
        lines.append("    0x%04X: %r," % (a, resolved[a]))
    lines.append("}")

    lines += [
        "",
        "# The comment the source wrote against each instruction.  Attached",
        "# only where a stretch of source and a stretch of listing lined up",
        "# one instruction to one instruction, so each of these sits on the",
        "# exact byte it was written about.",
        "INSTR_COMMENTS = {",
    ]
    for a in sorted(inline or {}):
        lines.append("    0x%04X: %r," % (a, inline[a]))
    lines.append("}")

    lines += ["",
              "# Standalone comment blocks written above a routine.",
              "LABEL_BLOCKS = {"]
    for a in sorted(blocks or {}):
        lines.append("    0x%04X: %r," % (a, blocks[a]))
    lines.append("}")

    lines += ["",
              "# Which source module each name came from.",
              "MODULE_OF = {"]
    for a in sorted(owners or {}):
        if a in resolved:
            lines.append("    0x%04X: %r," % (a, owners[a]))
    lines.append("}")
    lines += ["",
              "# .SBTTL headings - the source's own section titles.",
              "BANNERS = {"]
    for a in sorted(banners or {}):
        lines.append("    0x%04X: %r," % (a, banners[a]))
    lines.append("}")

    open(OUT, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    sys.exit(main())
