#!/usr/bin/env python3
"""Plain, assembler-style form of the Asteroids Deluxe disassembly.

m6502trace.py and dvgdasm.py write a *traced listing* - raw address on
the left, one entry per instruction/data-run, meant to be read.  This
tool writes a second form of the same information, modelled on the
released Asteroids (not Deluxe) source archive
(asteroids_defines.asm / asteroids_program_rom.asm / asteroids_vector_rom.asm):
plain ca65 source, one file per ROM, every line carrying its own address
label, that assembles straight back to the ROM image.  Nothing here is
re-derived independently - it is the same code/data split, the same
names, and the same comments as the traced listings, reformatted.

Inputs (read, never written):
    <set>_dump.bin        the 64K image (gen_from_roms.py)
    <set>_codemap.json     code/data runs + labels (m6502trace.py)
    astdelux_defines.asm   memory map + symbol glossary (mkdefines.py) -
                            both output files .include this; it is NOT
                            regenerated or edited here
    astdelux_defines.py, astdelux_config.py, astdelux_names.py,
    astdelux_vecnames.py, m6502.py, m6502trace.py, dvgdasm.py

Outputs:
    <set>_program_rom.asm   the program ROM, $6000-$7FFF
    <set>_vector_rom.asm    the vector-board ROM, $4800-$57FF (6502 code
                            AND DVG display-list data, interleaved - the
                            code/data map is what tells them apart)

Names are Atari's own (from the rev 2 source archive, applied only when
--set astdelux2), recovered onto the ROM by nameroutines.py/vecnames.py.
Comments right of an instruction, the blocks above a routine, and the
ruled section banners are the ORIGINAL Atari comments, carried across
the same way.  Lines marked '[note]' are this project's own one-line
descriptions (ROUTINE_NOTES in astdelux_config.py) for routines whose
source carries no comment block - everything else is Atari's.

ca65 legality: astdelux_names.py's recovered names can contain '$' or
'.' (`$DETCT`, `RTS.4`, `TRIROT_RTS.0`), which ca65 identifiers cannot.
Names are sanitised (leading '$' dropped - the defines file's own
convention; '.' becomes '_'), checked against a 6502 mnemonic or a bare
register letter (A/X/Y - '_' appended if so), and de-duplicated against
every other name this run emits, INCLUDING astdelux_defines.asm's own
equates, so nothing here can double-define one of them.  (The defines
file lists ROM entry points and DVG shapes as a commented-out glossary
for that reason: they are labels here.)  Address-based labels (`Lxxxx`)
never collide with any of that, since they always start with a letter
no name starts with, followed by four hex digits.

A data block in the DVG area that 6502 code reads directly (an operand
target that is neither a shape name nor a JSRL/JMPL target - RSOURC's
table of rock-shape addresses, for one) is written as `.byte` lines,
not decoded as vector opcodes it is not.

Round-trip check:  python mkplain.py --check
    Assembles both generated files with ca65 against the real
    astdelux_defines.asm, links each with a throwaway MEMORY config (one
    ro CODE segment at the ROM's base) via ld65, and compares the result
    byte-for-byte against the matching slice of <set>_dump.bin.  ca65 and
    ld65 are found on PATH, or under --cc65 <bindir>; they are part of the
    cc65 suite (https://cc65.github.io/).

Usage:
    python mkplain.py [--set astdelux2] [--check] [--cc65 <bindir>]
"""

import argparse
import bisect
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict

import m6502
import m6502trace as trace
import astdelux_defines as defs
import astdelux_config as cfg
import dvgdasm

def find_cc65():
    """The directory holding ca65 on PATH, or None."""
    exe = shutil.which("ca65")
    return os.path.dirname(exe) if exe else None

ROM_REGIONS = trace.ROM_REGIONS          # [(0x4800,0x5800,desc), (0x6000,0x8000,desc)]
VECROM_LO, VECROM_HI = dvgdasm.VECROM_LO, dvgdasm.VECROM_HI      # 0x4800, 0x5800
PROGROM_LO, PROGROM_HI = 0x6000, 0x8000
DVSTRT = dvgdasm.DVSTRT

MNEMONICS = {op[0] for op in m6502.OPCODES}
REGISTERS = {"a", "x", "y"}


def window_of(addr):
    if VECROM_LO <= addr < VECROM_HI:
        return "vecrom"
    if PROGROM_LO <= addr < PROGROM_HI:
        return "progrom"
    return None


# ----------------------------------------------------------------------
# name sanitising / de-duplication
# ----------------------------------------------------------------------

def sanitize(name):
    n = name
    if n.startswith("$"):
        n = n[1:]
    n = n.replace(".", "_")
    return n


class NameRegistry:
    """Sanitises and de-duplicates every symbol name this run emits.

    Seeded with astdelux_defines.asm's own equates (name -> address) so a
    routine/shape name can never double-define one of them: ca65 treats
    '=' and ':' as one namespace.  A name that collides with an equate,
    or with another name this run emits, gets a numeric suffix.
    """

    def __init__(self, defines_addrs):
        self.used = {n: ("predef", a) for n, a in defines_addrs.items()}
        self.renamed = []          # (addr, original, final) for the report
        self.by_addr = {}

    def resolve(self, addr, raw_name):
        if addr in self.by_addr:
            return self.by_addr[addr]
        name = sanitize(raw_name)
        if name.lower() in MNEMONICS or name.lower() in REGISTERS:
            name = name + "_"
        base = name
        n = 2
        while name in self.used:
            name = "%s_%d" % (base, n)
            n += 1
        if name != sanitize(raw_name):
            self.renamed.append((addr, raw_name, name))
        self.used[name] = ("addr", addr)
        self.by_addr[addr] = name
        return name


def defines_symbol_addrs(path):
    """name -> address, for every live `NAME = $hex` equate in the file
    (the commented-out glossary rows do not count)."""
    addrs = {}
    for line in open(path, encoding="utf-8"):
        raw = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        if not raw:
            continue
        m = re.search(r"=\s*\$([0-9A-Fa-f]+)\b", line)
        if m:
            addrs[raw.group(1)] = int(m.group(1), 16)
    return addrs


# ----------------------------------------------------------------------
# DVG block/name computation - ports dvgdasm.main()'s pass 1/2 logic,
# reusing its decode()/scan_6502_refs()/scan_char_table(), corrected so
# a data run straddling DVSTRT is split rather than partly dropped (a
# reporting tool can afford to lose a few bytes off a report; a
# round-trip generator cannot).
# ----------------------------------------------------------------------

def compute_dvg(mem, vecrom_runs, all_runs, romset):
    data_runs, below = [], []
    for lo, hi, kind in vecrom_runs:
        if kind != "data":
            continue
        if hi <= DVSTRT:
            below.append((lo, hi))
        elif lo >= DVSTRT:
            data_runs.append((lo, hi))
        else:
            below.append((lo, DVSTRT))
            data_runs.append((DVSTRT, hi))

    # The 6502 code that references these shapes (LXL/LAH pairs, and the
    # VGCHAR table scan) is scattered across BOTH ROM windows - VGCHAR
    # itself lives in the *program* ROM ($7A0A) - so this must search
    # every code run, not just the vector board's own.
    all_code_runs = [(lo, hi) for lo, hi, k in all_runs if k == "code"]

    def is_data(a):
        return any(lo <= a < hi for lo, hi in data_runs)

    use_map = romset == defs.SYMBOL_REVISION
    names = {}
    if use_map:
        names.update(defs.DVG_SYMBOLS)
        try:
            import astdelux_vecnames as vn
            if vn.NAME_REVISION == romset:
                names.update(vn.SHAPE_NAMES)
        except ImportError:
            pass
        for a, n in defs.ROM_SYMBOLS.items():
            if VECROM_LO <= a < VECROM_HI and is_data(a):
                names.setdefault(a, n)

    refs6502 = dvgdasm.scan_6502_refs(mem, all_code_runs)
    char_names, char_text, char_table = dvgdasm.scan_char_table(mem, all_code_runs)
    names.update(char_names)

    def align(a):
        return a + (a & 1)

    starts = set(names) | set(refs6502)
    starts = {a for a in starts if is_data(a) and not (a & 1)}

    def resync(a, n, stops):
        nxt = min((s for s in stops if s > a), default=None)
        return nxt if nxt is not None and a + n > nxt else None

    internal = defaultdict(set)
    for _pass in range(2):
        for lo, hi in data_runs:
            a = align(lo)
            while a < hi - 1:
                n, _t, tgt, _e = dvgdasm.decode(mem, a)
                if a + n > hi:
                    break
                if tgt is not None and is_data(tgt) and not (tgt & 1):
                    internal[tgt].add(a)
                    starts.add(tgt)
                a = resync(a, n, starts) or (a + n)

    return {
        "data_runs": data_runs, "below": below, "is_data": is_data,
        "names": names, "starts": starts, "align": align, "resync": resync,
        "char_table": char_table, "shape_refs": refs6502, "internal": internal,
    }


# ----------------------------------------------------------------------
# pass 1: walk every instruction once, collecting label needs
# ----------------------------------------------------------------------

def collect(mem, runs):
    """Decode every code run once.  Returns:
        instr_at[addr]  -> decoded m6502 dict, for every instruction start
        referenced      -> set of ROM addresses named by an operand/target
    """
    instr_at = {}
    referenced = set()
    for lo, hi, kind in runs:
        if kind != "code":
            continue
        a = lo
        while a < hi:
            ins = m6502.decode(mem, a)
            instr_at[a] = ins
            target = ins["target"]
            if target is not None:
                referenced.add(target)
            elif ins["mode"] in ("abs", "abx", "aby", "zp", "zpx", "zpy"):
                referenced.add(ins["operand"])
            a += ins["size"]
    return instr_at, referenced


# ----------------------------------------------------------------------
# line formatting
# ----------------------------------------------------------------------

LABEL_COL = 8
COMMENT_COL = 32


def pad_to(s, col, min_gap=1):
    if len(s) + min_gap <= col:
        return s + " " * (col - len(s))
    return s + " " * min_gap


def code_line(addr, raw_size, body, comment):
    label = "L%04X:" % addr
    line = pad_to(label, LABEL_COL) + body
    if comment:
        line = pad_to(line, COMMENT_COL) + ";" + comment
    return line


def banner(title, width=100):
    inner = "[ %s ]" % title
    pad = max(width - 1 - len(inner), 0)
    left = pad // 2
    right = pad - left
    return ";" + "-" * left + inner + "-" * right


SUFFIX = {
    "zp": "", "zpx": ",X", "zpy": ",Y",
    "abs": "", "abx": ",X", "aby": ",Y",
}


def format_instruction(ins, operand_text, override):
    mnem = ins["mnem"].upper()
    mode = ins["mode"]
    if mode == "imp":
        return mnem
    if mode == "acc":
        return "%s A" % mnem
    if mode == "imm":
        return "%s #$%02X" % (mnem, ins["operand"])
    p = "a:" if override else ""
    if mode == "ind":
        return "%s (%s%s)" % (mnem, p, operand_text)
    if mode == "izx":
        return "%s (%s%s,X)" % (mnem, p, operand_text)
    if mode == "izy":
        return "%s (%s%s),Y" % (mnem, p, operand_text)
    if mode == "rel":
        return "%s %s" % (mnem, operand_text)
    return "%s %s%s%s" % (mnem, p, operand_text, SUFFIX.get(mode, ""))


def numeric_operand(mode, value):
    width = 2 if mode in ("zp", "zpx", "zpy", "izx", "izy") else 4
    return "$%0*X" % (width, value)


# ----------------------------------------------------------------------
# main generator
# ----------------------------------------------------------------------

def build(romset):
    dump = "%s_dump.bin" % romset
    codemap = "%s_codemap.json" % romset
    try:
        mem = bytearray(open(dump, "rb").read())
        cm = json.load(open(codemap))
    except FileNotFoundError as exc:
        sys.exit("%s - run gen_from_roms.py and m6502trace.py --set %s first"
                 % (exc, romset))

    runs = cm["runs"]                                    # [[lo,hi,kind], ...]
    vecrom_runs = [r for r in runs if VECROM_LO <= r[0] and r[1] <= VECROM_HI]
    progrom_runs = [r for r in runs if PROGROM_LO <= r[0] and r[1] <= PROGROM_HI]

    syms, applied_rev, aliases = trace.build_symbols(romset)
    _n, comments, blocks, banners = trace.load_source_notes(romset)
    hand_notes = cfg.for_set(cfg.ROUTINE_NOTES, romset)
    # The packed message text: a label per message, and its decoded
    # string for the .byte rows (msgtext.py).
    msg_offsets, msg_text = trace.message_labels(
        mem, cfg.for_set(cfg.MESSAGE_TABLES, romset), syms)
    # The sound tables: SOUND label, and a gutter per decoded row.
    data_rows = trace.sound_rows(mem, cfg.for_set(cfg.SOUND_TABLES, romset), syms)

    dvg = compute_dvg(mem, vecrom_runs, runs, romset)

    name_for = dict(syms)
    name_for.update(dvg["names"])

    instr_at, referenced = collect(mem, runs)
    instr_starts = sorted(instr_at)

    # Some "DVG data" is really a byte/word table 6502 code walks (RSOURC's
    # table of rock-shape addresses, EXPPIC's JSRL words, the VGCHAR
    # table).  Every 6502 operand target in the DVG area starts a block,
    # so a byte read at an odd offset ($5003 into RSOURC) gets its own
    # line rather than landing mid-`.word`.  The words themselves stay
    # decoded as dvgdasm.py decodes them - the traced listing does the
    # same - except that a word which is the address of a named shape is
    # commented as that shape (see dvg_word_comment).
    dvg["starts"].update(a for a in referenced if dvg["is_data"](a))

    # Every code byte, so label_of() can tell "unnamed data/DVG address -
    # always its own line by construction below" from "mid-instruction
    # byte of a *code* run" - the one case that can legitimately need an
    # `Lxxxx+n` fallback (the ROM re-reads part of a jmp/branch operand's
    # own encoding as a data byte at $63DD: `lda $63E3,y` reads the high
    # byte of the very next instruction's `jmp SNDON` operand).
    code_byte = bytearray(0x10000)
    for lo, hi, kind in runs:
        if kind == "code":
            for a in range(lo, hi):
                code_byte[a] = 1

    # Addresses that must start their own line in the generic byte
    # grouping: anything referenced by an operand, or that carries a
    # name/block comment, landing inside a (non-DVG) data run.
    must_break = set(referenced)
    must_break.update(a for a in name_for if window_of(a) is not None)
    must_break.update(a for a in blocks if window_of(a) is not None)
    must_break.update(data_rows)

    # A data byte read a few bytes past a named table is written NAME+n,
    # as the source writes `SOUND+1` or `VGMSGT+1`, rather than being
    # given a line of its own: {addr: base}.
    small_off = {}
    for a in referenced:
        if window_of(a) is None or code_byte[a] or a in name_for:
            continue
        for b in range(a - 1, a - 5, -1):
            if b in name_for and window_of(b) is not None \
                    and not any(code_byte[k] for k in range(b, a + 1)):
                small_off[a] = b
                break
    must_break -= set(small_off)

    # ---- naming -------------------------------------------------------
    defines_path = "astdelux_defines.asm"
    reg = NameRegistry(defines_symbol_addrs(defines_path))
    final_name = {}
    for a in sorted(name_for):
        if window_of(a) is not None:
            final_name[a] = reg.resolve(a, name_for[a])

    midinstr_refs = []      # (addr, base, offset) - for the report

    def label_of(addr):
        """ca65 token for an operand address: a named routine/shape, an
        Lxxxx for an unnamed ROM-window address, a RAM/hardware name
        (astdelux_defines.asm - already unique, already clean), or None
        if nothing names it (caller falls back to a numeric literal).

        A data/DVG address that is ever used as an operand target is
        guaranteed (via must_break/DVG block-start tracking) to start
        its own line, so it always has an exact Lxxxx.  A *code*
        address can still be referenced mid-instruction; that gets
        `Lxxxx+n` against the instruction it falls inside.
        """
        if addr in final_name:
            return final_name[addr]
        if window_of(addr) is not None:
            if addr in small_off:
                base = small_off[addr]
                return "%s+%d" % (final_name[base], addr - base)
            if addr in instr_at or not code_byte[addr]:
                return "L%04X" % addr
            i = bisect.bisect_right(instr_starts, addr) - 1
            if i >= 0:
                base = instr_starts[i]
                size = instr_at[base]["size"]
                if base <= addr < base + size:
                    off = addr - base
                    midinstr_refs.append((addr, base, off))
                    return "L%04X+%d" % (base, off)
            return "L%04X" % addr
        if addr in syms:
            return sanitize(syms[addr])
        return None

    has_real_name = set(final_name)

    # Cross-file references: populated as a side effect of render_operand()
    # while the body is rendered below, so it reflects exactly what each
    # emitted operand actually resolved to.  DVG JSRL/JMPL never leave the
    # vector ROM's own 12-bit word space, so the vector side never adds to
    # this from its DVG instructions - only from genuine 6502 code.
    fwd_lxxxx = {"vecrom": set(), "progrom": set()}

    # ==================================================================
    # emission
    # ==================================================================

    def render_operand(ins, addr_of_instr):
        """Return (operand_text, override) for a non-implied instruction."""
        mode = ins["mode"]
        if mode in ("imp", "acc", "imm"):
            return None, False
        target = ins["target"] if ins["target"] is not None else ins["operand"]
        sym = None
        if mode in ("abs", "abx", "aby", "zp", "zpx", "zpy", "rel", "ind", "izx", "izy"):
            sym = label_of(target)
            if sym is not None and window_of(target) is not None:
                w = window_of(target)
                w_from = window_of(addr_of_instr)
                if w != w_from:
                    fwd_lxxxx[w_from].add(target)
        text = sym if sym is not None else numeric_operand(mode, target)
        override = mode in ("abs", "abx", "aby") and target < 0x100
        return text, override

    def instr_comment(a, ins):
        note = comments.get(a)
        if ins["mnem"] in ("jsr", "jmp") and ins["target"] is not None \
                and ins["target"] in has_real_name:
            prefix = "($%04X) " % ins["target"]
            return prefix + note if note else prefix.rstrip()
        return note

    headers_done = set()

    def emit_named_header(out, a):
        if a in headers_done:       # a DVG block start may be visited twice
            return
        headers_done.add(a)
        if a in banners:
            out.append("")
            out.append(banner(banners[a]))
        if a in final_name:
            out.append("")
            out.append("%s:" % final_name[a])
            if a in aliases:
                out.append("; also %s in the linker map" % aliases[a])
        for line in blocks.get(a, ()):
            out.append("; %s" % line)
        note = hand_notes.get(a)
        if note:
            lines = [note] if isinstance(note, str) else list(note)
            out.append("; [note] %s" % lines[0])
            out.extend(";        %s" % l for l in lines[1:])

    def emit_code_run(out, lo, hi):
        a = lo
        while a < hi:
            if a in name_for or a in blocks or a in banners or a in hand_notes:
                emit_named_header(out, a)
            ins = instr_at[a]
            operand_text, override = render_operand(ins, a)
            body = format_instruction(ins, operand_text, override) if operand_text is not None \
                else format_instruction(ins, None, False)
            comment = instr_comment(a, ins)
            out.append(code_line(a, ins["size"], body, comment))
            a += ins["size"]

    def emit_byte_group(out, lo, hi):
        a = lo
        while a < hi:
            start = a
            if start in name_for or start in blocks or start in banners or start in hand_notes:
                emit_named_header(out, start)
                if start in msg_offsets:
                    out.append("; %d message offsets, each relative to %s"
                               % (msg_offsets[start], final_name.get(start, "here")))
            a += 1
            n = 1
            while a < hi and n < 8 and a not in must_break:
                a += 1
                n += 1
            chunk = mem[start:a]
            vals = ", ".join("$%02X" % b for b in chunk)
            if start in msg_text:
                asc = '"%s"' % msg_text[start]         # packed text, decoded
            elif lo in msg_text:
                asc = "(cont.)"
            elif start in data_rows:
                asc = data_rows[start]                 # a decoded table row
            else:
                asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            label = "L%04X:" % start
            body = ".byte %s" % vals
            line = pad_to(label, LABEL_COL) + body
            line = pad_to(line, COMMENT_COL) + ";" + asc
            out.append(line)

    def dvg_word_comment(a, size, tgt):
        n, text, _tgt, _ends = dvgdasm.decode(mem, a)
        text = re.sub(r" +", " ", text).strip()
        if tgt is not None:
            name = label_of(tgt) or ("$%04X" % tgt)
            op = text.split()[0]
            return "%s %s ($%04X)" % (op, name, tgt)
        # A "vector" whose words are all addresses of named shapes is a
        # pointer table the 6502 reads (RSOURC -> ROCK0..ROCK7), not a
        # picture: say which shapes.
        words = [mem[a + i] | (mem[a + i + 1] << 8) for i in range(0, n, 2)]
        if "[raw]" in text and all(w in final_name and window_of(w) == "vecrom"
                                   for w in words):
            return "-> " + ", ".join(final_name[w] for w in words)
        return text

    def emit_dvg_run(out, lo, hi):
        align, resync, starts = dvg["align"], dvg["resync"], dvg["starts"]
        names = dvg["names"]
        a = align(lo)
        if lo & 1:
            emit_byte_group(out, lo, lo + 1)
        new_block = True
        while a < hi - 1:
            n, _text, tgt, ends = dvgdasm.decode(mem, a)
            if a + n > hi:
                emit_byte_group(out, a, hi)
                a = hi
                break
            if a in starts or new_block:
                if a in names:
                    emit_named_header(out, a)
                new_block = False
            cut = resync(a, n, starts)
            if cut is not None:
                emit_byte_group(out, a, cut)
                a = cut
                if a & 1:
                    # An odd-address entry point (a byte reference into
                    # what would otherwise be mid-word) can't start a
                    # `.word`; emit it alone and get back onto the word
                    # grid the rest of the run was decoded on.
                    emit_byte_group(out, a, a + 1)
                    a += 1
                new_block = True
                continue
            comment = dvg_word_comment(a, n, tgt)
            words = [mem[a + i] | (mem[a + i + 1] << 8) for i in range(0, n, 2)]
            body = ".word " + ", ".join("$%04X" % w for w in words)
            label = "L%04X:" % a
            line = pad_to(label, LABEL_COL) + body
            line = pad_to(line, COMMENT_COL) + ";" + comment
            out.append(line)
            a += n
            if ends:
                new_block = True
        if a < hi:
            emit_byte_group(out, a, hi)

    def emit_data_run(out, lo, hi):
        if window_of(lo) != "vecrom" or hi <= DVSTRT or lo >= VECROM_HI:
            emit_byte_group(out, lo, hi)
            return
        # Split at DVSTRT/DVG-run boundaries so a run spanning the line
        # between TRIROT's own tables and the picture data is handled
        # correctly by both halves.
        cursor = lo
        if cursor < DVSTRT:
            emit_byte_group(out, cursor, DVSTRT)
            cursor = DVSTRT
        while cursor < hi:
            in_dvg = any(l <= cursor < h for l, h in dvg["data_runs"])
            if in_dvg:
                end = min((h for l, h in dvg["data_runs"] if l <= cursor < h), default=hi)
                emit_dvg_run(out, cursor, end)
                cursor = end
            else:
                end = min((l for l, h in dvg["data_runs"] if l > cursor), default=hi)
                emit_byte_group(out, cursor, end)
                cursor = end

    def emit_region(out, region_runs, lo0, hi0):
        for lo, hi, kind in region_runs:
            if kind == "code":
                emit_code_run(out, lo, hi)
            else:
                emit_data_run(out, lo, hi)

    def render_body(region_runs, lo0, hi0):
        out = []
        emit_region(out, region_runs, lo0, hi0)
        return out

    vecrom_body = render_body(vecrom_runs, VECROM_LO, VECROM_HI)
    progrom_body = render_body(progrom_runs, PROGROM_LO, PROGROM_HI)

    return {
        "romset": romset, "applied_rev": applied_rev,
        "vecrom_body": vecrom_body, "progrom_body": progrom_body,
        "cross_needed": fwd_lxxxx,
        "final_name": final_name, "reg": reg, "n": _n, "comments": comments,
        "banners_count": len(banners), "midinstr_refs": midinstr_refs,
    }


# ----------------------------------------------------------------------
# whole-file assembly (header + forward decls + body)
# ----------------------------------------------------------------------

def header_lines(romset, applied_rev, kind):
    w = []
    title = "vector-board ROM ($4800-$57FF)" if kind == "vecrom" else "program ROM ($6000-$7FFF)"
    w.append("; Asteroids Deluxe (%s) - %s, plain assembler-style listing" % (romset, title))
    w.append("; GENERATED by mkplain.py from %s_dump.bin - do not hand-edit." % romset)
    w.append(";")
    w.append("; Subroutine and variable names are Atari's own, taken from the released")
    w.append("; rev 2 Asteroids Deluxe source archive")
    w.append("; (github.com/historicalsource/asteroids-deluxe) and placed on this ROM")
    w.append("; by matching instruction sequences (nameroutines.py) - nothing here is")
    w.append("; invented.  Comments to the right of an instruction, the blocks above a")
    w.append("; routine, and the ruled section banners are the ORIGINAL Atari comments,")
    w.append("; carried across the same way.  Lines marked '[note]' are this project's")
    w.append("; own descriptions, added where the source's comments are thin or absent")
    w.append("; (ROUTINE_NOTES in astdelux_config.py) - everything else is Atari's.")
    w.append(";")
    w.append("; Reading the names: NAME is a routine or data label from the source.")
    w.append("; NAME_20 is that routine's local label 20$ - a branch target inside it,")
    w.append("; kept under its owner's name.  RTS_n and NAME_RTS_n are the source's")
    w.append("; shared RTS labels (RTS.n there; '.' is not legal to the assembler).")
    w.append("; Lxxxx is an address the source does not name, and every line carries")
    w.append("; one so any byte can be referred to.  Packed message text is decoded")
    w.append("; in the comment as \"TEXT\" (msgtext.py).")
    w.append(";")
    w.append("; Memory map (see astdelux_defines.asm for the full glossary):")
    w.append(";     0000-00FF  page 0   scratch / globals")
    w.append(";     0100-01FF  page 1   6502 stack")
    w.append(";     0200-03FF  pages 2/3 player object state (bank-switched)")
    w.append(";     2000-3FFF  I/O")
    w.append(";     4000-47FF  vector RAM (2K) - the display list the DVG reads")
    w.append(";     4800-57FF  vector board ROM (4K) - 6502 code AND DVG picture")
    w.append(";                data, interleaved: TRIROT.MAC is `.ASECT` / `.=4800`,")
    w.append(";                so this ROM is not all display-list data.  Which bytes")
    w.append(";                are DVG opcodes (`.word`) and which are 6502 (decoded")
    w.append(";                as instructions) comes from the traced code/data map")
    w.append(";                (m6502trace.py), not a guess.")
    w.append(";     6000-7FFF  program ROM (8K)")
    w.append(";")
    if applied_rev:
        w.append("; Symbol names are the originals - the source archive documents exactly")
        w.append("; this revision.")
    else:
        w.append("; NOTE: the source archive documents rev 2 (astdelux2), not %s." % romset)
        w.append("; Only hardware names - board wiring, revision-independent - apply here;")
        w.append("; RAM/ROM names would be wrong (rev 3 moves page zero by 7 bytes).  Run")
        w.append("; with --set astdelux2 for the fully named listing.")
    w.append(";")
    w.append("; Round-trip check: this file assembles back to the ROM bytes with ca65.")
    w.append(";     python mkplain.py --set %s --check" % romset)
    w.append(";")
    return w


def forward_decl_block(title, names_map):
    out = ["", banner(title), ""]
    for addr in sorted(names_map):
        out.append("%-10s = $%04X" % (names_map[addr], addr))
    return out


def assemble_file(romset, kind, org, built):
    applied_rev = built["applied_rev"]
    out = header_lines(romset, applied_rev, kind)
    out.append(".include \"astdelux_defines.asm\"")
    out.append("")
    out.append(".org $%04X" % org)

    final_name = built["final_name"]
    needed = built["cross_needed"][kind]
    if needed:
        title = "Vector ROM Forward Declarations" if kind == "progrom" else "Program ROM Forward Declarations"
        names_map = {a: (final_name.get(a) or "L%04X" % a) for a in needed}
        out += forward_decl_block(title, names_map)

    out.append("")
    out.append(banner("Start Of %s" % ("Vector Board ROM" if kind == "vecrom" else "Program ROM")))
    out += built["vecrom_body"] if kind == "vecrom" else built["progrom_body"]
    return "\n".join(out) + "\n"


# ----------------------------------------------------------------------
# --check : round-trip through ca65 + ld65
# ----------------------------------------------------------------------

def run_check(romset, cc65_bin, targets):
    if cc65_bin is None:
        sys.exit("--check needs ca65/ld65: put the cc65 bin directory on PATH "
                 "or pass --cc65 <bindir> (https://cc65.github.io/)")
    exe = ".exe" if os.name == "nt" else ""
    ca65 = os.path.join(cc65_bin, "ca65" + exe)
    ld65 = os.path.join(cc65_bin, "ld65" + exe)
    if not os.path.isfile(ca65) or not os.path.isfile(ld65):
        sys.exit("ca65/ld65 not found under %s" % cc65_bin)

    dump = open("%s_dump.bin" % romset, "rb").read()
    all_ok = True
    for label, path, base, size in targets:
        with tempfile.TemporaryDirectory(prefix="mkplain_check_") as td:
            shutil.copy(path, os.path.join(td, os.path.basename(path)))
            shutil.copy("astdelux_defines.asm", os.path.join(td, "astdelux_defines.asm"))
            cfg_path = os.path.join(td, "link.cfg")
            open(cfg_path, "w").write(
                "MEMORY {\n"
                "    ROM: start = $%04X, size = $%04X, file = %%O, fill = yes;\n"
                "}\n"
                "SEGMENTS {\n"
                "    CODE: load = ROM, type = ro;\n"
                "}\n" % (base, size))
            obj = os.path.join(td, "out.o")
            r = subprocess.run([ca65, os.path.basename(path), "-o", "out.o"],
                               cwd=td, capture_output=True, text=True)
            if r.returncode:
                print("FAIL  %-14s ca65 error:" % label)
                print("\n".join("        " + l for l in r.stdout.splitlines() + r.stderr.splitlines()))
                all_ok = False
                continue
            binp = os.path.join(td, "out.bin")
            r = subprocess.run([ld65, "-C", "link.cfg", "out.o", "-o", "out.bin"],
                               cwd=td, capture_output=True, text=True)
            if r.returncode:
                print("FAIL  %-14s ld65 error:" % label)
                print("\n".join("        " + l for l in r.stdout.splitlines() + r.stderr.splitlines()))
                all_ok = False
                continue
            got = open(binp, "rb").read()
            want = dump[base:base + size]
            if got == want:
                print("PASS  %-14s %d bytes match %s_dump.bin[$%04X:$%04X]"
                      % (label, size, romset, base, base + size))
            else:
                n = min(len(got), len(want))
                diff = next((i for i in range(n) if got[i] != want[i]), n)
                print("FAIL  %-14s first difference at $%04X (got %s, want %s); "
                      "%d bytes assembled, %d expected"
                      % (label, base + diff,
                         "$%02X" % got[diff] if diff < len(got) else "<eof>",
                         "$%02X" % want[diff] if diff < len(want) else "<eof>",
                         len(got), len(want)))
                all_ok = False
    return all_ok


# ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="romset", default="astdelux2")
    ap.add_argument("--check", action="store_true",
                    help="round-trip both generated files through ca65+ld65 "
                         "and compare against <set>_dump.bin")
    ap.add_argument("--cc65", default=find_cc65(),
                    help="directory holding ca65/ld65 (default: wherever "
                         "ca65 is on PATH)")
    args = ap.parse_args()

    built = build(args.romset)

    vec_path = "%s_vector_rom.asm" % args.romset
    prog_path = "%s_program_rom.asm" % args.romset
    open(vec_path, "w").write(assemble_file(args.romset, "vecrom", VECROM_LO, built))
    open(prog_path, "w").write(assemble_file(args.romset, "progrom", PROGROM_LO, built))

    print("wrote %s and %s" % (vec_path, prog_path))
    reg = built["reg"]
    if reg.renamed:
        print("  %d name(s) sanitised/de-duplicated:" % len(reg.renamed))
        for addr, orig, final in reg.renamed:
            print("      $%04X  %-16s -> %s" % (addr, orig, final))
    if built["midinstr_refs"]:
        print("  %d operand(s) needed Lxxxx+n (reference into the middle of "
              "an instruction):" % len(built["midinstr_refs"]))
        for addr, base, off in built["midinstr_refs"]:
            print("      $%04X -> L%04X+%d" % (addr, base, off))

    if args.check:
        ok = run_check(args.romset, args.cc65,
                       [("vector_rom", vec_path, VECROM_LO, VECROM_HI - VECROM_LO),
                        ("program_rom", prog_path, PROGROM_LO, PROGROM_HI - PROGROM_LO)])
        if not ok:
            sys.exit(1)


if __name__ == "__main__":
    main()
