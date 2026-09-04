#!/usr/bin/env python3
"""Tracing 6502 disassembler for Asteroids Deluxe.

Walks control flow from the hardware vectors instead of sweeping the ROM
linearly, so code and data separate themselves: whatever the walk never
reaches is emitted as bytes rather than as invented instructions.

Two ROM regions hold 6502 code, and the first one is the interesting one:

    $4800-$57FF  the "vector board" ROM.  The Atari header calls this the
                 VECTOR ROM, but it is not all display-list data - the
                 TRIROT module is `.ASECT` / `.=4800`, so 6502 code and
                 DVG picture data are interleaved in it.  The trace is
                 what tells them apart.  The DVG side is decoded by
                 dvgdasm.py into <set>_vecrom.asm.
    $6000-$7FFF  the program ROM.

Reads  <set>_dump.bin      (build it with gen_from_roms.py)
Writes <set>_main.asm      and <set>_codemap.json, and prints a coverage
                           report naming what still needs work.

Usage:
    python m6502trace.py [--set astdelux2] [--quiet]
"""

import argparse
import json
import sys
from collections import defaultdict

import m6502
import astdelux_config as cfg
import astdelux_defines as defs

# Filenames carry the romset, so listings for different revisions coexist
# instead of overwriting each other.
def paths(romset):
    return ("%s_dump.bin" % romset, "%s_main.asm" % romset,
            "%s_codemap.json" % romset)

ROM_REGIONS = [
    (0x4800, 0x5800, "vector board ROM   036800 @ $4800, 036799 @ $5000"),
    (0x6000, 0x8000, "program ROM        036430/431/432/433"),
]

UNKNOWN, CODE, CONT, DATA = 0, 1, 2, 3


def _wrap(text, width):
    out, line = [], ""
    for word in text.split():
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = (line + " " + word).strip()
    if line:
        out.append(line)
    return out

VECTORS = [(0xFFFA, "NMI"), (0xFFFC, "RESET"), (0xFFFE, "IRQ/BRK")]


class Tracer:
    def __init__(self, mem, conf):
        self.mem = mem
        self.conf = conf
        self.flags = bytearray(0x10000)
        self.labels = {}                       # addr -> 'sub' | 'loc'
        self.xrefs = defaultdict(set)          # addr -> {referring addrs}
        self.data_xrefs = defaultdict(set)     # addr -> {addrs that name it}
        self.stops = []                        # (addr, reason)
        self.indirect = set()                  # unresolved jmp ($xxxx) sites

    def in_rom(self, a):
        return any(lo <= a < hi for lo, hi, _ in ROM_REGIONS)

    def declared_data(self, a):
        return any(lo <= a < hi for lo, hi, _ in self.conf["data"])

    def label(self, addr, kind):
        # 'sub' (a JSR target) outranks 'loc' (a branch/jump target)
        if kind == "sub" or addr not in self.labels:
            self.labels[addr] = kind

    # ------------------------------------------------------------------
    def trace(self, entries):
        work = list(entries)
        while work:
            self._walk(work.pop(), work)

    def _walk(self, addr, work):
        while True:
            if not self.in_rom(addr):
                self.stops.append((addr, "flow left ROM"))
                return
            if self.declared_data(addr):
                self.stops.append((addr, "flow entered a declared data region"))
                return
            if self.flags[addr] == CODE:
                return                          # already walked from here
            if self.flags[addr] == CONT:
                self.stops.append((addr, "flow landed mid-instruction"))
                return

            ins = m6502.decode(self.mem, addr)
            if ins["undoc"]:
                # Undocumented opcodes are essentially never deliberate in
                # this ROM, so hitting one means the walk has wandered into
                # data.  Stop and report instead of shredding the listing.
                self.stops.append((addr, "undocumented opcode $%02X (%s)"
                                   % (self.mem[addr], ins["mnem"])))
                return

            self.flags[addr] = CODE
            for i in range(1, ins["size"]):
                self.flags[(addr + i) & 0xFFFF] = CONT

            mnem, target = ins["mnem"], ins["target"]

            # record data references so named tables get xref lists too
            if target is None and ins["mode"] in ("abs", "abx", "aby"):
                self.data_xrefs[ins["operand"]].add(addr)

            if mnem in m6502.BRANCHES:
                self.label(target, "loc")
                self.xrefs[target].add(addr)
                work.append(target)
                addr += ins["size"]
                continue

            if mnem == "jsr":
                self.label(target, "sub")
                self.xrefs[target].add(addr)
                work.append(target)
                skip = self.conf["inline"].get(target, 0)
                addr += ins["size"] + skip
                continue

            if mnem == "jmp":
                if ins["mode"] == "ind":
                    self.indirect.add(addr)
                    for t in self.conf["jumps"].get(addr, ()):
                        self.label(t, "sub")
                        self.xrefs[t].add(addr)
                        work.append(t)
                    return
                self.label(target, "loc")
                self.xrefs[target].add(addr)
                work.append(target)
                return

            if mnem in m6502.TERMINAL or mnem == "brk":
                return

            addr += ins["size"]

    def finish(self):
        for lo, hi, _ in ROM_REGIONS:
            for a in range(lo, hi):
                if self.flags[a] == UNKNOWN:
                    self.flags[a] = DATA


# ----------------------------------------------------------------------
# symbols
# ----------------------------------------------------------------------

def load_source_notes(romset):
    """What nameroutines.py recovered from the Atari source, if it has run.

    Returns (names, instruction comments, label blocks, section banners).
    Empty when the module is absent or describes a different revision.
    """
    empty = ({}, {}, {}, {})
    try:
        import astdelux_names as src
    except ImportError:
        return empty
    if getattr(src, "NAME_REVISION", None) != romset:
        return empty
    return (src.ROUTINE_NAMES,
            getattr(src, "INSTR_COMMENTS", {}),
            getattr(src, "LABEL_BLOCKS", {}),
            getattr(src, "BANNERS", {}))


def load_routine_names(romset):
    return load_source_notes(romset)[0]


def build_symbols(romset):
    """Address -> name, plus {address: map name} where the two differ.

    Hardware names are board wiring and apply to every revision.  RAM and
    ROM names come from the rev 2 source archive and are only correct for
    that build, so they are withheld otherwise; see mkdefines.py.

    Where the linker map and the source name the same address, the source
    name wins: the map carries a section name for a module's first
    routine, and truncates every symbol to six characters, so `SETROL`
    and `STEAROM` are better than `DSTMSG` and `STEARO`.  The map's name
    is kept as an alias so nothing is lost.
    """
    syms = {a: n for a, (n, _c) in defs.HARDWARE.items()}
    aliases = {}
    applied_rev = romset == defs.SYMBOL_REVISION
    if applied_rev:
        for a, (n, _sz, _c) in defs.RAM.items():
            syms.setdefault(a, n.lstrip("$"))
        for a, n in defs.ROM_SYMBOLS.items():
            syms[a] = n
        for a, n in load_routine_names(romset).items():
            if a in syms and syms[a] != n:
                aliases[a] = syms[a]
            syms[a] = n
    return syms, applied_rev, aliases


def sound_rows(mem, snd, syms):
    """The sound tables: SOUND into `syms`, and a gutter text per decoded
    row (PNTRS entries, macro counts, steps, terminators).  See sndtab.py."""
    if not snd:
        return {}
    import sndtab
    labels, rows = sndtab.decode(mem, snd["pntrs"], snd["sound"], snd["end"],
                                 snd["sounds"])
    for a, n in labels.items():
        syms.setdefault(a, n)
    return rows


def shape_labels(romset, tracer, mem, syms):
    """Every DVG shape name the vector listing knows (the vector source's
    names, and the character glyphs named from the VGCHAR table) goes into
    `syms` too, so the data runs of the vector-board ROM break at each
    shape here as they do in astdelux2_vecrom.asm."""
    import dvgdasm
    names = {}
    try:
        import astdelux_vecnames as vn
        if vn.NAME_REVISION == romset:
            names.update(vn.SHAPE_NAMES)
    except ImportError:
        pass
    code_runs = []
    a = 0
    while a < 0x10000:
        if tracer.flags[a] == CODE:
            b = a
            while b < 0x10000 and tracer.flags[b] in (CODE, CONT):
                b += 1
            code_runs.append((a, b))
            a = b
        else:
            a += 1
    char_names, _text, _table = dvgdasm.scan_char_table(mem, code_runs)
    names.update(char_names)
    for a, n in names.items():
        if dvgdasm.VECROM_LO <= a < dvgdasm.VECROM_HI and tracer.flags[a] != CODE:
            syms.setdefault(a, n)


def message_labels(mem, tables, syms):
    """Name every packed message (L0_10, L0_11, ...) into `syms`, so the
    listing breaks at each one, and return what the emitter prints:
    {block: offset count} and {message: decoded text}.  See msgtext.py."""
    if not tables:
        return {}, {}
    import msgtext
    offsets, messages = msgtext.decode_tables(mem, tables)
    for addr, (name, _text) in messages.items():
        syms.setdefault(addr, name)
    return offsets, {addr: text for addr, (_n, text) in messages.items()}


# ----------------------------------------------------------------------
# emit
# ----------------------------------------------------------------------

def label_name(addr, tracer, syms):
    if addr in syms:
        return syms[addr]
    if addr in tracer.labels:
        return ("SUB_%04X" if tracer.labels[addr] == "sub" else "L%04X") % addr
    return None


def emit(tracer, mem, syms, romset, applied_rev, entries, aliases=None,
         notes=None):
    aliases = aliases or {}
    notes = notes or {"comments": {}, "blocks": {}, "banners": {}}
    data_notes = {lo: (hi, why) for lo, hi, why in tracer.conf["data"]}
    out = []
    w = out.append
    w("; Asteroids Deluxe (%s) - 6502 program, traced disassembly" % romset)
    w("; generated by m6502trace.py from %s_dump.bin - do not hand-edit" % romset)
    w(";")
    w("; Control flow was walked from the hardware vectors.  Bytes the walk")
    w("; never reached are emitted as data, not as invented instructions.")
    w(";")
    if notes["comments"]:
        w("; Comments to the right of an instruction, the blocks above a")
        w("; routine, and the ruled section titles are the ORIGINAL comments")
        w("; from Atari's source, carried across by nameroutines.py.  They")
        w("; were only attached where a stretch of source lined up with a")
        w("; stretch of this listing one instruction to one instruction, so")
        w("; each sits on the exact byte it was written about.")
        w(";")
    if notes.get("hand"):
        w("; Lines marked [note] are this project's own descriptions, added under")
        w("; labels where the source's comments are thin or absent (ROUTINE_NOTES")
        w("; in astdelux_config.py).  Everything else is Atari's.")
        w(";")
    w("; Reading the names: NAME is a routine or data label from the source.")
    w("; NAME_20 is that routine's local label 20$ - a branch target inside")
    w("; it, kept under its owner's name.  RTS.n and NAME_RTS.n are the")
    w("; source's shared `RTS` labels.  Lxxxx is an address the source does")
    w("; not name.  Packed message text is decoded into the gutter as")
    w("; \"TEXT\" (msgtext.py).")
    w(";")
    w("; The $4800-$57FF ROM holds BOTH 6502 code and DVG display-list data;")
    w("; the two are interleaved because TRIROT.MAC is `.ASECT` / `.=4800`.")
    w("; Data blocks below that the DVG actually draws are decoded as vector")
    w("; opcodes in astdelux_vecrom.asm - they are marked [DVG] here.")
    w(";")
    if applied_rev:
        w("; Symbol names are the originals, from the Atari source archive")
        w("; (DSTDEC/EAROM/PG0123 + the DSTRD0 linker map), which documents")
        w("; exactly this revision.")
    else:
        w("; NOTE: the source archive documents rev 2 (astdelux2), not %s."
          % romset)
        w("; Its RAM and ROM names would be wrong here (page zero moved by 7")
        w("; bytes between the revisions), so only hardware names - which are")
        w("; board wiring and revision-independent - have been applied.")
        w("; Run with --set astdelux2 to get the fully named listing.")
    w(";")
    for addr, why in entries:
        for i, line in enumerate(_wrap(why, 60)):
            w("; %s%s" % ("entry  $%04X  " % addr if i == 0 else " " * 14, line))
    w(";")

    for lo, hi, desc in ROM_REGIONS:
        w("")
        w(";" + "=" * 70)
        w("; $%04X-$%04X   %s" % (lo, hi - 1, desc))
        w(";" + "=" * 70)
        a = lo
        while a < hi:
            if tracer.flags[a] == CODE:
                a = emit_code(w, tracer, mem, a, syms, aliases, notes)
            else:
                a = emit_data(w, tracer, mem, a, hi, syms, data_notes,
                              notes)
    return "\n".join(out) + "\n"


def emit_header(w, tracer, addr, syms, kind):
    name = label_name(addr, tracer, syms)
    if not name:
        return
    w("")
    w("%s:" % name)
    refs = sorted(tracer.xrefs.get(addr, set()) | tracer.data_xrefs.get(addr, set()))
    if refs:
        shown = ", ".join("$%04X" % r for r in refs[:8])
        more = "" if len(refs) <= 8 else "  (+%d more)" % (len(refs) - 8)
        w("; xrefs: %s%s" % (shown, more))


def emit_hand_note(w, a, notes):
    """Our own description of a label (ROUTINE_NOTES), a string or a list
    of lines, marked [note] so it cannot pass for a recovered comment."""
    note = notes.get("hand", {}).get(a)
    if not note:
        return
    lines = [note] if isinstance(note, str) else list(note)
    w("; [note] %s" % lines[0])
    for line in lines[1:]:
        w(";        %s" % line)


def emit_code(w, tracer, mem, a, syms, aliases=None, notes=None):
    notes = notes or {"comments": {}, "blocks": {}, "banners": {}}
    if a in tracer.labels or a in syms:
        # The source's own section title, where it starts a new one.
        if a in notes["banners"]:
            w("")
            w(";" + "-" * 70)
            w("; %s" % notes["banners"][a])
            w(";" + "-" * 70)
        emit_header(w, tracer, a, syms, "code")
        if aliases and a in aliases:
            w("; also %s in the linker map" % aliases[a])
        # A comment block the programmer wrote above the routine.
        for line in notes["blocks"].get(a, ()):
            w("; %s" % line)
        emit_hand_note(w, a, notes)
    ins = m6502.decode(mem, a)
    raw = " ".join("%02X" % mem[a + i] for i in range(ins["size"]))
    sym = None
    if ins["mode"] in ("abs", "abx", "aby", "ind", "rel", "zp", "zpx", "zpy"):
        sym = label_name(ins["operand"], tracer, syms)
    body = m6502.text(ins, sym)
    note = notes["comments"].get(a)
    if note:
        w("%04X:  %-11s %-28s ; %s" % (a, raw, body, note))
    else:
        w("%04X:  %-11s %s" % (a, raw, body))
    return a + ins["size"]


def emit_data(w, tracer, mem, a, hi, syms, data_notes=None, notes=None):
    """Emit one run of non-code bytes, broken at any named address."""
    data_notes = data_notes or {}
    notes = notes or {"comments": {}, "blocks": {}, "banners": {}}
    msg_offsets = notes.get("msg_offsets", {})
    msg_text = notes.get("msg_text", {})
    start = a
    a += 1
    while a < hi and tracer.flags[a] != CODE and a not in syms \
            and a not in tracer.labels and a not in defs.DVG_SYMBOLS:
        a += 1
    end = a

    dvg = defs.DVG_SYMBOLS.get(start)
    emit_header(w, tracer, start, syms, "data")
    if dvg and not label_name(start, tracer, syms):
        w("")
        w("%s:" % dvg)
    # A named block in the picture half of the vector-board ROM is a DVG
    # shape: say so, with the JSRL word that reaches it, and point at the
    # vector listing where it is decoded.
    import dvgdasm
    is_shape = dvg or (start >= dvgdasm.DVSTRT and start < dvgdasm.VECROM_HI
                       and start in syms)
    tag = ("  [DVG]  JSRL $%03X - decoded in the vector listing"
           % ((start - 0x4000) >> 1)) if is_shape else ""
    w("; ---- data $%04X-$%04X (%d bytes)%s ----"
      % (start, end - 1, end - start, tag))
    if start in data_notes:
        why = data_notes[start][1]
        for i, line in enumerate(_wrap(why, 66)):
            w("; %s%s" % ("      " if i else "why:  ", line))
    # A comment block above a data label - the source's own, or the
    # evidence for a hand-placed name.
    for line in notes["blocks"].get(start, ()):
        w("; %s" % line)
    emit_hand_note(w, start, notes)
    if start in msg_offsets:
        w("; %d message offsets, each relative to %s (msgtext.py)"
          % (msg_offsets[start], label_name(start, tracer, syms) or "here"))
    # Decoded tables (the sound envelopes): explicit rows, each with its
    # own gutter text, instead of the 16-byte grid.
    data_rows = notes.get("data_rows", {})
    rows = [r for r in sorted(data_rows) if start <= r < end]
    if rows and rows[0] == start:
        for i, r in enumerate(rows):
            nxt = rows[i + 1] if i + 1 < len(rows) else end
            for base in range(r, nxt, 16):
                chunk = mem[base:min(base + 16, nxt)]
                hexs = " ".join("%02X" % b for b in chunk)
                w("%04X:  %-47s ; %s" % (base, hexs,
                                        data_rows[r] if base == r else "(cont.)"))
        return end
    for base in range(start, end, 16):
        chunk = mem[base:min(base + 16, end)]
        hexs = " ".join("%02X" % b for b in chunk)
        if start in msg_text:
            # Packed text: the decoded string instead of the ASCII gutter.
            asc = '"%s"' % msg_text[start] if base == start else "(cont.)"
        else:
            asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        w("%04X:  %-47s ; %s" % (base, hexs, asc))
    return end


# ----------------------------------------------------------------------
# exploration: does an unreached linker-map symbol look like code?
# ----------------------------------------------------------------------

def looks_like_code(mem, addr, limit=64):
    """Decode forward from `addr` and judge what we find.

    Returns (verdict, detail).  A run that decodes cleanly out of legal
    opcodes and reaches a natural terminator is evidence of a real entry
    point; an undocumented opcode in the first few instructions is
    evidence of data.  This is a *hint* for curation, not a decision -
    the config records the human call.
    """
    a, n = addr, 0
    while n < limit:
        ins = m6502.decode(mem, a)
        if ins["undoc"]:
            return ("data", "undocumented $%02X after %d instruction(s)"
                    % (mem[a], n))
        n += 1
        if ins["mnem"] in ("rts", "rti"):
            return ("code", "%d instructions, ends %s" % (n, ins["mnem"]))
        if ins["mnem"] == "jmp":
            return ("code", "%d instructions, ends jmp" % n)
        a = (a + ins["size"]) & 0xFFFF
    return ("code?", "%d instructions, no terminator within %d" % (n, limit))


def explore(mem, entries, conf):
    base = Tracer(mem, conf)
    base.trace([a for a, _ in entries])
    base.finish()
    print("unreached linker-map symbols, with a decode verdict:")
    print("(add the 'code' ones to CODE_ENTRY_POINTS in astdelux_config.py)")
    for a, name in sorted(defs.ROM_SYMBOLS.items()):
        if base.flags[a] in (CODE, CONT):
            continue
        verdict, detail = looks_like_code(mem, a)
        print("  $%04X  %-8s %-6s %s" % (a, name, verdict, detail))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="romset", default="astdelux2",
                    help="which romset to disassemble (default: astdelux2, "
                         "rev 2 - the revision the source archive documents, "
                         "and the only one that gets the original names)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--explore", action="store_true",
                    help="report whether each unreached linker-map symbol "
                         "looks like code, to help curate CODE_ENTRY_POINTS")
    args = ap.parse_args()

    DUMP, OUT, CODEMAP = paths(args.romset)
    try:
        mem = bytearray(open(DUMP, "rb").read())
    except FileNotFoundError:
        sys.exit("%s not found - run:  "
                 "python gen_from_roms.py <romdir> --set %s"
                 % (DUMP, args.romset))
    if len(mem) != 0x10000:
        sys.exit("%s is %d bytes, expected 65536" % (DUMP, len(mem)))

    # Resolve the per-revision configuration once, up front.  Config keyed
    # by anything else would silently mix revisions.
    conf = {
        "data": cfg.for_set(cfg.DATA_REGIONS, args.romset),
        "jumps": cfg.for_set(cfg.JUMP_TABLES, args.romset),
        "inline": cfg.for_set(cfg.INLINE_DATA_CALLS, args.romset),
        "extra": cfg.for_set(cfg.EXTRA_ENTRY_POINTS, args.romset),
        "code_entries": cfg.for_set(cfg.CODE_ENTRY_POINTS, args.romset),
    }

    entries = []
    for va, name in VECTORS:
        entries.append((mem[va] | (mem[va + 1] << 8), "%s vector ($%04X)" % (name, va)))
    for a, why in sorted(conf["extra"].items()):
        entries.append((a, why))
    for a, why in sorted(conf["code_entries"].items()):
        entries.append((a, why))

    if args.explore:
        explore(mem, entries, conf)
        return

    tracer = Tracer(mem, conf)
    tracer.trace([a for a, _ in entries])
    for a, _ in entries:
        tracer.label(a, "sub")
    tracer.finish()

    syms, applied_rev, aliases = build_symbols(args.romset)
    _n, comments, blocks, banners = load_source_notes(args.romset)
    # The packed message text: a label and the decoded string per message.
    msg_offsets, msg_text = message_labels(
        mem, cfg.for_set(cfg.MESSAGE_TABLES, args.romset), syms)
    data_rows = sound_rows(mem, cfg.for_set(cfg.SOUND_TABLES, args.romset), syms)
    shape_labels(args.romset, tracer, mem, syms)
    notes = {"comments": comments, "blocks": blocks, "banners": banners,
             "hand": cfg.for_set(cfg.ROUTINE_NOTES, args.romset),
             "msg_offsets": msg_offsets, "msg_text": msg_text,
             "data_rows": data_rows}
    open(OUT, "w").write(emit(tracer, mem, syms, args.romset, applied_rev,
                              entries, aliases, notes))

    # A machine-readable code/data map, so dvgdasm.py knows which bytes in
    # $4800-$57FF are 6502 code and must not be decoded as DVG opcodes.
    runs = []
    for lo, hi, _ in ROM_REGIONS:
        a = lo
        while a < hi:
            kind = "code" if tracer.flags[a] in (CODE, CONT) else "data"
            b = a
            while b < hi and (tracer.flags[b] in (CODE, CONT)) == (kind == "code"):
                b += 1
            runs.append([a, b, kind])
            a = b
    json.dump({"romset": args.romset, "runs": runs,
               "labels": {"%04X" % k: v for k, v in tracer.labels.items()}},
              open(CODEMAP, "w"), indent=1)

    # ---- report ----
    print("wrote %s and %s" % (OUT, CODEMAP))
    print("  symbols: %s"
          % ("original names applied (archive documents %s)" % args.romset
             if applied_rev else
             "hardware only - archive documents %s, not %s"
             % (defs.SYMBOL_REVISION, args.romset)))
    tot_code = tot = 0
    for lo, hi, desc in ROM_REGIONS:
        code = sum(1 for a in range(lo, hi) if tracer.flags[a] in (CODE, CONT))
        tot_code += code
        tot += hi - lo
        print("  $%04X-$%04X  %5d/%5d bytes code (%5.1f%%)   %s"
              % (lo, hi - 1, code, hi - lo, 100.0 * code / (hi - lo), desc))
    print("  total       %5d/%5d bytes code (%5.1f%%)"
          % (tot_code, tot, 100.0 * tot_code / tot))
    if _n:
        print("  from the Atari source: %d names, %d instruction comments, "
              "%d section titles" % (len(_n), len(comments), len(banners)))
    print("  labels: %d total, %d of them JSR targets"
          % (len(tracer.labels),
             sum(1 for k in tracer.labels.values() if k == "sub")))

    if tracer.indirect:
        print("  TODO - unresolved jmp ($xxxx) at: %s"
              % ", ".join("$%04X" % a for a in sorted(tracer.indirect)))
    odd = sorted({(a, r) for a, r in tracer.stops if "declared data" not in r})
    if odd and not args.quiet:
        print("  TODO - trace stopped unexpectedly at %d site(s):" % len(odd))
        for a, r in odd[:30]:
            print("      $%04X  %s" % (a, r))
        if len(odd) > 30:
            print("      ... and %d more" % (len(odd) - 30))


if __name__ == "__main__":
    main()
