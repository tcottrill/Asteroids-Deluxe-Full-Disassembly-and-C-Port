#!/usr/bin/env python3
"""Build the Asteroids Deluxe symbol table from the original Atari sources.

The source archive (`asteroids-deluxe-main/`) is not a decompilation aid we
have to guess at - it is Dave Sheppard / Ed Logg's actual MACRO-65 source
plus the linker map, so the names in it are the real ones.  This script
turns three of those files into machine-readable tables:

    DSTDEC.MAC   hardware equates (I/O addresses)   -> HARDWARE
    EAROM.MAC    EAROM port equates                 -> HARDWARE
    PG0123.MAC   RAM layout, pages 0-3, as a run of -> RAM
                 sequential .BLKB declarations
    DSTRD0.MAP   LINKM load map: ROM entry points   -> ROM_SYMBOLS
                 and DVG shape entry points         -> DVG_SYMBOLS

WHICH REVISION THESE DESCRIBE
-----------------------------
The archive is the **rev 2** build (MAME `astdelux2`).  That is not a
guess: DSTRD0.MAP puts NMI at $785C and PWRON at $7CD7, and the vectors
in 036433-02 read exactly $785C / $7CD7.  The rev 3 ROMs (MAME
`astdelux`, what `astdelux.asm` was dumped from) have vectors $7851 /
$7CE0 and a page zero shifted by 7 bytes, so **these names must not be
applied to a rev 3 image** - m6502trace.py enforces that by checking
SYMBOL_REVISION against the romset it was told to disassemble.

Outputs:
    astdelux_defines.py    tables the other tools import
    astdelux_defines.asm   the same thing as a readable memory map

Usage:
    python mkdefines.py [path-to-source-archive]
"""

import os
import re
import sys

SRC_DEFAULT = os.path.join("..", "asteroids-deluxe-main")

# Build-time conditionals.  The shipped game is Deluxe Asteroids, so the
# $DTHST ("Death Star", an abandoned variant sharing this code base)
# branches are off.
COND = {"$DAST": 1, "$DTHST": 0}
CONSTS = {"NOBJ": 25, "NSPCLS": 7}      # DSTDEC.MAC, decimal

ROM_LO, ROM_HI = 0x4800, 0x8000
VECROM_LO, VECROM_HI = 0x4800, 0x5800


# ---------------------------------------------------------------------------
# number parsing.  DSTDEC.MAC sets .RADIX 16, so a bare literal is hex and a
# trailing '.' means decimal ("3*10." is thirty).
# ---------------------------------------------------------------------------

def value(expr, syms):
    expr = expr.strip()
    if not expr:
        return 1

    def tok(m):
        t = m.group(0)
        if t.endswith("."):
            return str(int(t[:-1], 10))
        if re.fullmatch(r"[0-9A-F]+", t):
            return str(int(t, 16))
        if t in syms:
            return str(syms[t])
        raise KeyError(t)

    try:
        py = re.sub(r"\^H[0-9A-F]+|[0-9][0-9A-F]*\.?|[A-Z\$][A-Z0-9\$.]*",
                    lambda m: (str(int(m.group(0)[2:], 16))
                               if m.group(0).startswith("^H") else tok(m)),
                    expr)
        return int(eval(py, {"__builtins__": {}}, {}))
    except Exception:
        return None


def read(path):
    return open(path, "rb").read().decode("latin-1")


# ---------------------------------------------------------------------------
# hardware equates
# ---------------------------------------------------------------------------

EQU_RE = re.compile(r"^([A-Z\$][A-Z0-9\$.]*)\s*=\s*([^;\r\n]+?)\s*(?:;(.*))?$")


def parse_equates(src):
    """NAME = expr ; comment  ->  {name: (value, comment)}"""
    syms, out = {}, {}
    for fn in ("DSTDEC.MAC", "EAROM.MAC"):
        for line in read(os.path.join(src, fn)).splitlines():
            m = EQU_RE.match(line.rstrip())
            if not m:
                continue
            name, expr, comment = m.group(1), m.group(2), (m.group(3) or "").strip()
            v = value(expr, syms)
            if v is None:
                continue
            syms[name] = v
            out[name] = (v, comment)
    return out


# ---------------------------------------------------------------------------
# RAM layout from PG0123.MAC
# ---------------------------------------------------------------------------

BLKB_RE = re.compile(r"^(?:([A-Z\$][A-Z0-9\$.]*):)?\s*\.BLKB\s*([^;\r\n]*?)\s*(?:;(.*))?$")
ORG_RE = re.compile(r"^\s*\.=\s*([0-9A-F]+)\s*(?:;.*)?$")
IF_RE = re.compile(r"^\s*\.IF\s+(NE|EQ)\s*,\s*(\S+)\s*$")
IRPC_RE = re.compile(r"^\s*\.IRPC\s+(\w+)\s*,\s*<([^>]*)>\s*$")
REPT_RE = re.compile(r"^\s*\.REPT\s+([0-9A-F]+)\s*$")


def expand_repeats(lines):
    """Expand .IRPC/.REPT ... .ENDR blocks before the layout walk.

    PG0123.MAC declares the scratch registers as

        .IRPC X,<0123456789>
        R'X:    .BLKB
        .ENDR

    which is ten cells, not one.  MACRO-11's apostrophe is the
    concatenation operator, so R'X with X='3' is the label R3.  Missing
    this shifts every later page-zero cell by 10.
    """
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m_irpc, m_rept = IRPC_RE.match(line), REPT_RE.match(line)
        if not (m_irpc or m_rept):
            out.append(line)
            i += 1
            continue
        body, j = [], i + 1
        while j < len(lines) and lines[j].strip() != ".ENDR":
            body.append(lines[j])
            j += 1
        if j >= len(lines):
            sys.exit("PG0123.MAC: .IRPC/.REPT near line %d has no .ENDR" % i)
        if m_irpc:
            var, chars = m_irpc.group(1), m_irpc.group(2)
            for ch in chars:
                for b in body:
                    out.append(b.replace("'" + var, ch).replace(var + "'", ch))
        else:
            out.extend(body * int(m_rept.group(1), 16))
        i = j + 1
    return out


def parse_ram(src):
    """Walk PG0123.MAC's sequential .BLKB declarations, honouring .IF/.IFF."""
    syms = dict(CONSTS)
    out = {}
    dot = 0
    page_ends = {}                  # org address -> allocation pointer at .=
    stack = []                      # active-conditional stack
    for line in expand_repeats(read(os.path.join(src, "PG0123.MAC")).splitlines()):
        line = line.rstrip()
        s = line.strip()

        m = IF_RE.match(line)
        if m:
            cond = COND.get(m.group(2), 0)
            stack.append(bool(cond) if m.group(1) == "NE" else not cond)
            continue
        if s == ".IFF":
            if stack:
                stack[-1] = not stack[-1]
            continue
        if s == ".ENDC":
            if stack:
                stack.pop()
            continue
        if not all(stack):
            continue

        m = ORG_RE.match(line)
        if m:
            org = int(m.group(1), 16)
            page_ends[org] = dot
            dot = org
            continue

        m = BLKB_RE.match(line)
        if not m:
            continue
        name, expr, comment = m.group(1), m.group(2), (m.group(3) or "").strip()
        size = value(expr, syms) if expr else 1
        if size is None:
            sys.exit("PG0123.MAC: cannot evaluate .BLKB %r" % expr)
        if name:
            out[dot] = (name, size, comment)
        dot += size

    # PG0123.MAC asserts page zero fits ( .IIF GT,.-^H100,.ERROR ), so if the
    # .BLKB arithmetic is right the allocation pointer lands on exactly $0100
    # when the source switches to page 1.  That is an independent check on
    # the whole walk, so report it rather than assuming.
    return out, dot, page_ends.get(0x100)


# ---------------------------------------------------------------------------
# linker map
# ---------------------------------------------------------------------------

PAIR_RE = re.compile(r"([A-Z\$][A-Z0-9\$.]{0,5})\s+([0-9A-F]{4})\b")
MAP_SKIP = ("RELOCATION", "TRANSFER", "HIGH LIMIT", "LOAD MAP", "SECTION ADDR")


# A line that starts with a name in column 0 is a section header:
# "DSTMSG<tab>714E<tab>03DE<tab>SETROL<tab>714E ...".  Its first pair is
# the section, not a routine, which is why the map and the source
# sometimes name one address differently - both correctly.
SECTION_RE = re.compile(r"^([A-Z\$][A-Z0-9\$.]{0,5})\s+([0-9A-F]{4})\s+([0-9A-F]{4})\b")


def parse_map(src):
    """ROM entry points, plus DVG shapes named in JSRL-word form ($Cxxx).

    Also returns every name seen at each address (the map lists several
    at some) and the set of section names, so downstream tools can tell a
    real disagreement from a section-vs-routine naming difference.
    """
    rom, dvg, other = {}, {}, {}
    aliases, sections = {}, set()
    for line in read(os.path.join(src, "DSTRD0.MAP")).splitlines():
        if any(k in line for k in MAP_SKIP):
            continue
        m = SECTION_RE.match(line)
        if m:
            sections.add(m.group(1))
        for name, hexv in PAIR_RE.findall(line):
            a = int(hexv, 16)
            if ROM_LO <= a < ROM_HI:
                rom.setdefault(a, name)
                aliases.setdefault(a, [])
                if name not in aliases[a]:
                    aliases[a].append(name)
            elif 0xC000 <= a < 0xD000:
                # a DVG JSRL word: byte address = VECRAM + 2*word
                rom_addr = 0x4000 + ((a & 0xFFF) << 1)
                dvg.setdefault(rom_addr, name)
            else:
                other.setdefault(name, a)
    return rom, dvg, other, aliases, sections


# ---------------------------------------------------------------------------
# emit
# ---------------------------------------------------------------------------

def emit_py(hw, ram, rom, dvg, path, aliases=None, sections=None):
    L = []
    a = L.append
    a('"""Asteroids Deluxe symbol tables - GENERATED by mkdefines.py.')
    a("")
    a("Do not hand-edit; put hand-written knowledge in astdelux_config.py.")
    a("")
    a("Every name here comes from the original Atari source archive, which is")
    a("the rev 2 build (MAME `astdelux2`): DSTRD0.MAP's NMI=$785C / PWRON=$7CD7")
    a("match 036433-02's vectors exactly.  Rev 3 shifts page zero by 7 bytes,")
    a("so these are only valid against the revision named below.")
    a('"""')
    a("")
    a('SYMBOL_REVISION = "astdelux2"')
    a("")
    a("# I/O and hardware registers (DSTDEC.MAC, EAROM.MAC).")
    a("# Revision-independent: this is board wiring, not software layout.")
    a("HARDWARE = {")
    for name, (v, c) in sorted(hw.items(), key=lambda kv: (kv[1][0], kv[0])):
        if 0x2000 <= v < 0x4001:
            a("    0x%04X: (%-9s %s)," % (v, '"%s",' % name, repr(c)))
    a("}")
    a("")
    a("# Bit masks and build constants that are NOT addresses.")
    a("CONSTANTS = {")
    for name, (v, c) in sorted(hw.items()):
        if not 0x2000 <= v < 0x4001:
            a("    %-10s (0x%02X, %s)," % ('"%s":' % name, v, repr(c)))
    a("}")
    a("")
    a("# RAM cells, pages 0-3 (PG0123.MAC).  addr: (name, size, comment)")
    a("# Pages 2 and 3 are the bank-switched per-player copies (BNKSEL).")
    a("RAM = {")
    for addr in sorted(ram):
        name, size, c = ram[addr]
        a("    0x%04X: (%-9s %2d, %s)," % (addr, '"%s",' % name, size, repr(c)))
    a("}")
    a("")
    a("# ROM entry points named by the linker map.")
    a("ROM_SYMBOLS = {")
    for addr in sorted(rom):
        a("    0x%04X: %s," % (addr, repr(rom[addr])))
    a("}")
    a("")
    a("# Every name the map gives each ROM address - it lists more than one")
    a("# at some, e.g. a section name and the first routine inside it.")
    a("ROM_ALIASES = {")
    for addr in sorted(aliases or {}):
        if len(aliases[addr]) > 1 or addr in rom:
            a("    0x%04X: %r," % (addr, aliases[addr]))
    a("}")
    a("")
    a("# Section names.  LINKM lists a section at the address of its first")
    a("# routine, so these are the addresses where map and source can name")
    a("# the same byte differently without either being wrong.")
    a("SECTIONS = %r" % (sorted(sections or ()),))
    a("")
    a("# Note: LINKM truncates symbols to six characters, so a map name can")
    a("# be a prefix of the real one (STEARO for STEAROM).")
    a("")
    a("# DVG shape entry points.  The map records these as JSRL words ($Cxxx);")
    a("# the byte address is VECRAM + 2*word, which is how they are keyed here.")
    a("DVG_SYMBOLS = {")
    for addr in sorted(dvg):
        a("    0x%04X: %s," % (addr, repr(dvg[addr])))
    a("}")
    a("")
    open(path, "w").write("\n".join(L) + "\n")


def emit_asm(hw, ram, rom, dvg, path):
    L = []
    a = L.append
    a("; Asteroids Deluxe - memory map and symbol glossary")
    a("; GENERATED by mkdefines.py from the original Atari source archive.")
    a(";")
    a("; Names are the real ones: they come from DSTDEC.MAC, EAROM.MAC,")
    a("; PG0123.MAC and the LINKM load map DSTRD0.MAP, all written by the")
    a("; game's own authors.  The archive is the rev 2 build (MAME")
    a("; `astdelux2`) - DSTRD0.MAP's NMI $785C / PWRON $7CD7 are exactly the")
    a("; vectors in 036433-02.  Rev 3 moves page zero down by 7 bytes, so")
    a("; RAM addresses below apply to rev 2 only; the hardware block applies")
    a("; to every revision.")
    a(";")
    a("; The original assembler allowed '$' inside identifiers.  '$' is the")
    a("; hex prefix in this listing, so names like $INTCT are written INTCT")
    a("; here; the original spelling is noted in the comment.")
    a(";")
    a("; Address space (from the DSTRD0.MAC header):")
    a(";     0000-00FF  page 0   scratch / globals")
    a(";     0100-01FF  page 1   6502 stack (shares the page with SXP* cells)")
    a(";     0200-02FF  page 2   player 1 object state   } swapped by BNKSEL")
    a(";     0300-03FF  page 3   player 2 object state   } ($3C04 bit 7)")
    a(";     2000-3FFF  I/O")
    a(";     4000-47FF  vector RAM (2K) - the display list the DVG reads")
    a(";     4800-57FF  vector board ROM (4K) - 6502 code AND DVG data")
    a(";     6000-7FFF  program ROM (8K)")
    a(";     FFFA-FFFF  vectors, mirrored from 7FFA-7FFF")

    def block(title, rows, note=None, glossary=False):
        """One ruled section of equates.  With `glossary` the rows are
        written as comments: ROM entry points and DVG shapes are defined
        as labels in the plain-form ROM listings (mkplain.py), which
        .include this file, so equating them here as well would define
        every one of them twice."""
        a("")
        a(";" + "-" * 68)
        a("; " + title)
        if note:
            for n in note:
                a("; " + n)
        a(";" + "-" * 68)
        for name, addr, width, comment in rows:
            # '$' is the hex prefix and '.' is not an identifier character
            # to an assembler; the original spelling goes in the comment.
            clean = name.lstrip("$").replace(".", "_")
            txt = "%-10s = $%0*X" % (clean, width, addr)
            if clean != name:
                comment = ("orig. %s.  " % name) + comment if comment else \
                          "orig. %s" % name
            if glossary:
                txt = "; " + txt
            a("%-26s%s" % (txt, ("; " + comment) if comment else ""))

    block("HARDWARE / I/O  (all revisions)",
          [(n, v, 4, c) for n, (v, c) in
           sorted(hw.items(), key=lambda kv: kv[1][0]) if 0x2000 <= v < 0x4001])

    block("CONSTANTS  (bit masks, not addresses)",
          [(n, v, 2, c) for n, (v, c) in sorted(hw.items())
           if not 0x2000 <= v < 0x4001])

    block("PAGE 0  (rev 2 addresses)",
          [(nm, ad, 2, c) for ad, (nm, sz, c) in sorted(ram.items())
           if ad < 0x100])

    block("PAGE 1  (rev 2 addresses)",
          [(nm, ad, 4, c) for ad, (nm, sz, c) in sorted(ram.items())
           if 0x100 <= ad < 0x200],
          ["The 6502 stack lives in this page; these cells sit above it."])

    block("PAGES 2/3  per-player object state  (rev 2 addresses)",
          [(nm, ad, 4, c) for ad, (nm, sz, c) in sorted(ram.items())
           if ad >= 0x200],
          ["BNKSEL ($3C04) bit 7 swaps page 2 and page 3, so the same",
           "addresses address whichever player is up."])

    rom_note = ["Glossary only - commented out.  These are LABELS in the plain-form",
                "listings (astdelux2_vector_rom.asm / astdelux2_program_rom.asm),",
                "which .include this file; equating them here too would define",
                "each of them twice."]
    block("VECTOR BOARD ROM entry points  $4800-$57FF  (rev 2)",
          [(nm, ad, 4, "") for ad, nm in sorted(rom.items())
           if VECROM_LO <= ad < VECROM_HI], rom_note, glossary=True)

    block("PROGRAM ROM entry points  $6000-$7FFF  (rev 2)",
          [(nm, ad, 4, "") for ad, nm in sorted(rom.items()) if ad >= 0x6000],
          rom_note, glossary=True)

    block("DVG SHAPES  (named in the map as JSRL words; byte = 4000 + 2*word)",
          [(nm, ad, 4, "JSRL $%03X" % ((ad - 0x4000) >> 1))
           for ad, nm in sorted(dvg.items())], rom_note, glossary=True)
    a("")
    open(path, "w").write("\n".join(L) + "\n")


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else SRC_DEFAULT
    if not os.path.isdir(src):
        sys.exit("source archive not found: %s" % src)

    hw = parse_equates(src)
    ram, ram_end, page0_end = parse_ram(src)
    rom, dvg, other, aliases, sections = parse_map(src)

    emit_py(hw, ram, rom, dvg, "astdelux_defines.py", aliases, sections)
    emit_asm(hw, ram, rom, dvg, "astdelux_defines.asm")

    print("wrote astdelux_defines.py / astdelux_defines.asm")
    print("  hardware equates : %d" % len(hw))
    print("  RAM cells        : %d  (page 0 allocation ends $%04X, last page-3 cell $%04X)"
          % (len(ram), page0_end or 0, max(ram)))
    print("  ROM symbols      : %d  (%d in the vector board ROM)"
          % (len(rom), sum(1 for a in rom if a < VECROM_HI)))
    print("  DVG shapes       : %d" % len(dvg))

    # Cross-check the RAM walk against addresses the linker map states
    # independently.  If these disagree the .BLKB arithmetic is wrong.
    checks = {"VGSIZE": 0x01, "VGLIST": 0x03, "TEMP1": 0x09, "R0": 0x10,
              "TEMP4": 0x1B, "NPLAYR": 0x22, "UPDFLG": 0x42, "CHIST": 0x71,
              "SYNC": 0x75, "FRAME": 0x76, "LOUT1": 0x85, "ASTERS": 0x86,
              "CPMTST": 0x90, "HOLE": 0x92, "CKERR": 0xE0, "OBJ": 0x200,
              "XINC": 0x221, "SHPXI": 0x23A, "YINC": 0x242, "OBJXH": 0x263,
              "SHPXH": 0x27C, "OBJXL": 0x2A5, "SHPXL": 0x2BE, "SHLDS": 0x2EF}
    byname = {nm: ad for ad, (nm, sz, c) in ram.items()}
    bad = [(n, a, byname.get(n)) for n, a in checks.items() if byname.get(n) != a]
    if bad:
        print("  MAP CROSS-CHECK FAILED:")
        for n, want, got in bad:
            print("    %-8s map says $%04X, .BLKB walk says %s"
                  % (n, want, "$%04X" % got if got else "(absent)"))
        sys.exit(1)
    print("  cross-check      : %d/%d map addresses reproduced by the .BLKB walk"
          % (len(checks), len(checks)))
    if page0_end != 0x100:
        print("  WARNING: page 0 does not end exactly at $0100")


if __name__ == "__main__":
    main()
