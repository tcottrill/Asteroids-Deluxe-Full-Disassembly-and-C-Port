#!/usr/bin/env python3
"""DVG display-list disassembler for the Asteroids Deluxe vector ROM.

The $4800-$57FF ROM is shared between 6502 code and DVG picture data (see
m6502trace.py).  This tool decodes only the parts the 6502 trace proved
are *not* code, reading the code/data map that trace wrote.  So the split
between "game code" and "vector code" is derived, not assumed.

INSTRUCTION ENCODING
--------------------
Taken from the macro definitions in the original DSTVEC.MAC rather than
from a datasheet, so the decoder inverts exactly what the assembler
emitted:

    VCTR    two words.  word1 = op<<12 | dysign<<10 | |dy|
                        word2 = z<<12  | dxsign<<10 | |dx|
            `op` (1-9) is the DVG's scale/rate.  The macro doubles the
            magnitudes and counts down from op=9 until they fill the
            field, so a long vector carries a low op.  This listing
            prints dx/dy already shifted back by (9-op) - the values the
            programmer wrote - and marks the factor as [xN].
    WAIT    a VCTR with both magnitudes zero:  .WORD T*1000, Z*1000
    SVEC    one word, four short forms sharing a layout: bits 1-0 = |dx|
            in units, bits 9-8 = |dy| in units, bit 2 = dx sign, bit 10 =
            dy sign, bits 7-4 = z, and bits 11 and 3 select the unit
            (2, 4, 8 or 16).  Decoding back through the unit recovers the
            DX,DY the programmer actually wrote.
    LABS    .WORD A000+(y&FFF), size<<12 | (x&FFF)
    HALT    B000
    JSRL    .WORD <addr & 1FFF>/2 + C000
    RTSL    D000
    JMPL    .WORD <addr & 1FFF>/2 + E000

The `& 1FFF` in JSRL/JMPL is why a shape's word address is
(byte - $4000) / 2: the vector address space is $4000-$5FFF and the mask
drops the base.  DSTRD0.MAP records shape entry points in JSRL form,
which is how SAUCER shows up there as $C734 = byte $4E68.

Reads  <set>_dump.bin and <set>_codemap.json
Writes <set>_vecrom.asm

Usage:
    python dvgdasm.py [--set astdelux2]
"""

import argparse
import json
import sys
from collections import defaultdict

import m6502
import astdelux_defines as defs

VECROM_LO, VECROM_HI = 0x4800, 0x5800
VECRAM = 0x4000                     # DVG address space base

# DASVEC.MAC states where the picture data starts:
#     DVSTRT = ^H4D80    ;START ADDRESS OF DASVEC STUFF
# Below that the ROM is TRIROT's - 6502 code and the 6502's own tables
# (AVEL, ITXL/ITYL/ITIME/IANG, SINCOS).  Those are data runs, so they
# reach this tool, but they are not display lists: decoding them as
# vectors invents shapes that do not exist.  They are named and typed
# properly in <set>_main.asm.
DVSTRT = 0x4D80

# VGCHAR ($7A0A) indexes the VGMSGA table and rejects Y >= $4A, so the
# table holds $4A/2 = 37 entries.  DSVECN.MAC writes them out in order -
# `VGMSGA: JSRL CHAR. / JSRL CHAR.0 / JSRL CHAR.1 ...` - which is space,
# then the ten digits, then A-Z.  Exactly 37.
CHAR_TABLE_LIMIT = 0x4A
CHARSET = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"

# Some glyphs are shared: DSVECN.MAC has `CHAR.0 = CHAR.O` and
# `CHAR.5 = CHAR.S`, so two indices resolve to one address.  Prefer the
# letter when naming, and record the alias.
def char_label(indices):
    chars = [CHARSET[i] for i in sorted(indices)]
    letters = [c for c in chars if c.isalpha()]
    primary = letters[0] if letters else chars[0]
    name = "CHAR_SPACE" if primary == " " else "CHAR_%s" % primary
    alias = [c for c in chars if c != primary]
    return name, alias


def word(mem, a):
    return mem[a] | (mem[a + 1] << 8)


def target_of(w):
    """JSRL/JMPL operand -> byte address."""
    return VECRAM + ((w & 0xFFF) << 1)


def decode(mem, a):
    """Decode one DVG instruction.

    Returns (size, text, target_or_None, ends_block).
    """
    w1 = word(mem, a)
    op = w1 >> 12

    if op <= 9:
        w2 = word(mem, a + 2)
        dy, dx = w1 & 0x3FF, w2 & 0x3FF
        z = w2 >> 12
        if dx == 0 and dy == 0 and not (w1 & 0x400) and not (w2 & 0x400):
            # .WORD T*1000, Z*1000 - the WAIT macro
            return (4, "WAIT   t=%-4d               z=%d" % (op, z), None, False)
        if w1 & 0x400:
            dy = -dy
        if w2 & 0x400:
            dx = -dx
        # The VCTR macro doubles the magnitudes and counts the opcode down
        # from 9 until they fill the field, so the value the programmer
        # wrote is the stored one shifted back by (9 - op).  Verified
        # against DASVEC.MAC: SAUCER's `VCTR -80,0,.BRITR+1` is stored as
        # dx=-640 at s=6, and 640 >> 3 == 80.
        shift = 9 - op
        exact = shift >= 0 and not ((abs(dx) | abs(dy)) & ((1 << shift) - 1))
        if exact:
            return (4, "VCTR   s=%-2d  dx=%-5d dy=%-5d z=%-2d  [x%d]"
                    % (op, dx >> shift, dy >> shift, z, 1 << shift),
                    None, False)
        return (4, "VCTR   s=%-2d  dx=%-5d dy=%-5d z=%-2d  [raw]"
                % (op, dx, dy, z), None, False)

    if op == 0xA:
        w2 = word(mem, a + 2)
        return (4, "LABS   size=%-2d  x=%-4d y=%-4d"
                % (w2 >> 12, w2 & 0xFFF, w1 & 0xFFF), None, False)

    if op == 0xB:
        return (2, "HALT", None, True)

    if op == 0xC:
        t = target_of(w1)
        return (2, "JSRL   $%04X" % t, t, False)

    if op == 0xD:
        return (2, "RTSL", None, True)

    if op == 0xE:
        t = target_of(w1)
        return (2, "JMPL   $%04X" % t, t, True)

    # op == 0xF : SVEC.  bits 11 and 3 pick the unit; see the header.
    unit = {(0, 0): 2, (1, 0): 4, (0, 1): 8, (1, 1): 16}[
        ((w1 >> 11) & 1, (w1 >> 3) & 1)]
    dx = ((w1 >> 0) & 0x03) * unit
    dy = ((w1 >> 8) & 0x03) * unit
    if w1 & 0x004:
        dx = -dx
    if w1 & 0x400:
        dy = -dy
    z = (w1 >> 4) & 0x0F
    return (2, "SVEC   u=%-2d  dx=%-5d dy=%-5d z=%d" % (unit, dx, dy, z),
            None, False)


# ----------------------------------------------------------------------
# where the 6502 side points into this ROM
# ----------------------------------------------------------------------

def scan_6502_refs(mem, code_runs):
    """Find `LXL shape / LAH shape` pairs in traced 6502 code.

    DSTDEC.MAC defines LXL/LAH as macros that assemble to `LDX #<low>`
    and `LDA #<high>` of an address; the game then calls VGJSRL, which
    halves the 16-bit value into a DVG word.  The two macros are written
    on consecutive source lines, so in the ROM they are adjacent
    `A2 lo` / `A9 hi` bytes in either order.

    LAL/LXL load an already-formed JSRL word instead (DSTMSG.MAC's
    `CPYRS:: LAL CPMG`), so a $Cxxx/$Exxx pair is accepted too and
    converted through the same word->byte rule.
    """
    refs = defaultdict(set)
    for lo, hi in code_runs:
        for a in range(lo, hi - 3):
            b = mem[a:a + 4]
            if b[0] == 0xA2 and b[2] == 0xA9:          # ldx #lo ; lda #hi
                value = (b[3] << 8) | b[1]
            elif b[0] == 0xA9 and b[2] == 0xA2:        # lda #hi ; ldx #lo
                value = (b[1] << 8) | b[3]
            else:
                continue
            for addr in (value, target_of(value) if 0xC000 <= value < 0xF000
                         else None):
                if addr is not None and VECROM_LO <= addr < VECROM_HI:
                    refs[addr].add(a)
                    break
    return refs


def scan_char_table(mem, code_runs):
    """Decode the VGCHAR character-shape table into {addr: CHAR_nn}.

    VGCHAR reads `ldx tbl+1,y / lda tbl,y`, i.e. a table stepped two bytes
    at a time.  In the rev 2 map that table is VGMSGA ($56F8); it is
    located here from the instruction pair instead, so the tool does not
    depend on having a map.

    The entries are JSRL *words* ($Cxxx), not byte addresses - VGCHAR
    hands the pair straight to VGADD2, which stores it into the display
    list as-is.  Checked against the map: entry 0 reads $CB0A, and
    $4000 + 2*$B0A = $5614 = VGSPAC.
    """
    table = None
    for lo, hi in code_runs:
        for a in range(lo, hi - 6):
            # ldx abs,y (BE lo hi) then lda abs,y (B9 lo hi) one lower
            if mem[a] == 0xBE and mem[a + 3] == 0xB9:
                hi_tbl = mem[a + 1] | (mem[a + 2] << 8)
                lo_tbl = mem[a + 4] | (mem[a + 5] << 8)
                if hi_tbl == lo_tbl + 1 and VECROM_LO <= lo_tbl < VECROM_HI:
                    table = lo_tbl
    if table is None:
        return {}, {}, None

    by_addr = defaultdict(set)
    for i in range(0, CHAR_TABLE_LIMIT, 2):
        w = word(mem, table + i)
        addr = target_of(w) if 0xC000 <= w < 0xF000 else w
        if VECROM_LO <= addr < VECROM_HI:
            by_addr[addr].add(i // 2)

    names, text = {}, {}
    for addr, indices in by_addr.items():
        name, alias = char_label(indices)
        # Use the same letter-first choice the label uses, so a shared
        # glyph reads as text: CHAR.5 == CHAR.S, and DASVEC.MAC's
        # `ERASE:: ALPHA <ERASIN>` must read back as ERASIN, not ERA5IN.
        text[addr] = (name[len("CHAR_"):] if name != "CHAR_SPACE" else " ",
                      alias)
        names[addr] = name
    return names, text, table


def render_text(mem, a, hi, char_text):
    """If a block is a run of JSRLs to glyphs, read back the string.

    DSTVEC.MAC's ALPHA macro emits one `JSRL CHAR.x` per character, so a
    message in this ROM literally is a list of glyph calls.  Only report
    a string when the whole run is glyphs and there are at least two of
    them, so an ordinary shape that happens to start with one call is not
    mislabelled as text.
    """
    if not char_text:
        return None
    out, p = [], a
    while p < hi - 1:
        w = word(mem, p)
        if (w >> 12) != 0xC:
            break
        t = target_of(w)
        if t not in char_text:
            break
        out.append(char_text[t][0])
        p += 2
    return "".join(out) if len(out) >= 2 else None


# ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="romset", default="astdelux2")
    args = ap.parse_args()

    dump = "%s_dump.bin" % args.romset
    codemap = "%s_codemap.json" % args.romset
    out_path = "%s_vecrom.asm" % args.romset

    try:
        mem = bytearray(open(dump, "rb").read())
        cm = json.load(open(codemap))
    except FileNotFoundError as exc:
        sys.exit("%s - run gen_from_roms.py and m6502trace.py --set %s first"
                 % (exc, args.romset))

    # Data runs inside the vector ROM are the candidate DVG regions; the
    # 6502 trace already proved everything else there is code.
    data_runs, below = [], []
    for lo, hi, kind in cm["runs"]:
        if kind == "data":
            clipped = (max(lo, VECROM_LO), min(hi, VECROM_HI))
            if clipped[0] >= clipped[1]:
                continue
            if clipped[1] <= DVSTRT:
                below.append(clipped)       # TRIROT's tables, not art
            else:
                data_runs.append((max(clipped[0], DVSTRT), clipped[1]))
    all_code_runs = [(lo, hi) for lo, hi, k in cm["runs"] if k == "code"]

    def is_data(a):
        return any(lo <= a < hi for lo, hi in data_runs)

    # ---- names and references -------------------------------------
    use_map = args.romset == defs.SYMBOL_REVISION
    names = {}
    if use_map:
        names.update(defs.DVG_SYMBOLS)
        # Shape names recovered from the vector source by vecnames.py.
        try:
            import astdelux_vecnames as vn
            if vn.NAME_REVISION == args.romset:
                names.update(vn.SHAPE_NAMES)
        except ImportError:
            pass
        for a, n in defs.ROM_SYMBOLS.items():
            if VECROM_LO <= a < VECROM_HI and is_data(a):
                names.setdefault(a, n)

    refs6502 = scan_6502_refs(mem, all_code_runs)
    char_names, char_text, char_table = scan_char_table(mem, all_code_runs)
    # Character names win over map names here: the map calls $5614 VGSPAC,
    # but CHAR_SPACE says what it draws.  Keep the map name as an alias.
    map_alias = {a: names[a] for a in char_names if a in names}
    names.update(char_names)

    # DVG instructions are 16-bit words on the even byte addresses of the
    # $4000 vector space (that is what makes a shape's word address
    # (byte-$4000)/2).  A data run can begin on an odd byte - it starts
    # wherever the preceding 6502 code stopped - so decoding must be
    # aligned up to the word grid or every instruction in the run is read
    # one byte out of phase.
    def align(a):
        return a + (a & 1)

    # ---- pass 1: find block starts --------------------------------
    starts = set(names) | set(refs6502)
    starts = {a for a in starts if is_data(a) and not (a & 1)}
    # A known entry point defines the phase: filler bytes ahead of a shape
    # can put the decoder half a word out, and an instruction is never
    # allowed to straddle an address something else enters at.
    def resync(a, n, stops):
        nxt = min((s for s in stops if s > a), default=None)
        return nxt if nxt is not None and a + n > nxt else None

    internal = defaultdict(set)          # target -> {JSRL/JMPL sites}
    for _pass in range(2):               # targets found in pass 1 add phase
        for lo, hi in data_runs:
            a = align(lo)
            while a < hi - 1:
                n, _t, tgt, _e = decode(mem, a)
                if a + n > hi:          # would read past the run
                    break
                if tgt is not None and is_data(tgt) and not (tgt & 1):
                    internal[tgt].add(a)
                    starts.add(tgt)
                a = resync(a, n, starts) or (a + n)

    # ---- pass 2: emit ---------------------------------------------
    L = []
    w = L.append
    w("; Asteroids Deluxe (%s) - vector ROM, DVG display-list disassembly"
      % args.romset)
    w("; generated by dvgdasm.py - do not hand-edit")
    w(";")
    w("; Covers only the parts of $4800-$57FF that m6502trace.py proved are")
    w("; not 6502 code.  That ROM holds both: TRIROT.MAC is `.ASECT` /")
    w("; `.=4800`, so the vector board's ROM carries game code as well as")
    w("; pictures.  The 6502 side is in %s_main.asm." % args.romset)
    w(";")
    w("; Columns: byte address / DVG word address / raw words / instruction.")
    w("; The word address is what a JSRL operand holds: (byte - $4000) / 2.")
    w(";")
    if use_map:
        w("; Shape names come from DSTRD0.MAP, which records them as JSRL")
        w("; words - SAUCER appears there as $C734, i.e. byte $4E68.")
    else:
        w("; NOTE: the linker map documents %s, not %s, so map shape names"
          % (defs.SYMBOL_REVISION, args.romset))
        w("; are withheld here.  Blocks are named from the character table")
        w("; and by address only.")
    if char_table:
        w("; Character shapes were named from the VGCHAR table at $%04X."
          % char_table)
    w(";")
    w("; 'refs:' lines cite where a shape is entered from:")
    w(";   6502 $xxxx  - an LXL/LAH address pair in the program, fed to VGJSRL")
    w(";   DVG  $xxxx  - a JSRL/JMPL inside this ROM")
    w(";")

    total = decoded = 0
    for lo, hi in sorted(data_runs):
        total += hi - lo
        w("")
        w(";" + "-" * 68)
        w("; data run $%04X-$%04X  (%d bytes)" % (lo, hi - 1, hi - lo))
        w(";" + "-" * 68)
        if lo & 1:
            w("%04X/---:  %02X          ; odd leading byte - the run starts "
              "mid-word, so decoding begins at $%04X" % (lo, mem[lo], lo + 1))
        a = align(lo)
        new_block = True
        while a < hi - 1:
            n, text, tgt, ends = decode(mem, a)
            if a + n > hi:
                # A two-word instruction at the very end would read past the
                # ROM.  Show the bytes rather than decoding memory we do not
                # own; a real display list never runs off the end like this.
                w("%04X/%03X:  %-10s ; %d byte(s) short of a complete "
                  "instruction at the end of the run"
                  % (a, (a - VECRAM) >> 1,
                     " ".join("%02X" % b for b in mem[a:hi]), hi - a))
                a = hi
                break
            if a in starts or new_block:
                w("")
                w("%s: (JSRL $%03X)" % (names.get(a, "VEC_%04X" % a),
                                        (a - VECRAM) >> 1))
                if a in map_alias:
                    w(";   also %s in the linker map" % map_alias[a])
                if a in char_text and char_text[a][1]:
                    w(";   glyph shared with %s"
                      % ", ".join("'%s'" % c for c in char_text[a][1]))
                s = render_text(mem, a, hi, char_text)
                if s:
                    w(';   draws: "%s"' % s)
                for r in sorted(refs6502.get(a, ())):
                    w(";   refs: 6502 $%04X" % r)
                for r in sorted(internal.get(a, ())):
                    w(";   refs: DVG  $%04X" % r)
                if a not in refs6502 and a not in internal:
                    w(";   refs: none found - unreferenced art, reached via a "
                      "computed word, or entered by fall-through")
                new_block = False
            cut = resync(a, n, starts)
            if cut is not None:
                gap = " ".join("%02X" % b for b in mem[a:cut])
                w("%04X/%03X:  %-10s ; %d filler byte(s) before the next entry "
                  "point" % (a, (a - VECRAM) >> 1, gap, cut - a))
                a = cut
                new_block = True
                continue
            raw = " ".join("%04X" % word(mem, a + i) for i in range(0, n, 2))
            if tgt is not None:
                text += "   ; -> %s" % names.get(tgt, "VEC_%04X" % tgt)
            w("%04X/%03X:  %-10s %s" % (a, (a - VECRAM) >> 1, raw, text))
            decoded += n
            a += n
            if ends:
                new_block = True
        if a < hi:                        # trailing odd byte
            w("%04X/---:  %02X          ; odd trailing byte" % (a, mem[a]))

    if below:
        L += ["", ";" + "=" * 68,
              "; NOT DECODED - 6502 data below DVSTRT ($%04X)" % DVSTRT,
              ";" + "=" * 68, ";",
              "; Tables TRIROT indexes, not vector art.  Decoding them as a",
              "; display list would invent shapes the game never draws.",
              "; They are named and typed in %s_main.asm." % args.romset, ""]
        for lo, hi in below:
            L.append("; $%04X-$%04X  %5d bytes" % (lo, hi - 1, hi - lo))

    open(out_path, "w").write("\n".join(L) + "\n")

    # ---- report ----
    print("wrote %s" % out_path)
    print("  DVG data in $%04X-$%04X : %d bytes in %d run(s)"
          % (VECROM_LO, VECROM_HI - 1, total, len(data_runs)))
    print("  blocks                  : %d" % len(starts))
    try:
        import astdelux_vecnames as _vn
        n_src = len({a for a in starts if a in _vn.SHAPE_NAMES})
    except ImportError:
        n_src = 0
    print("  named from vector source: %d" % n_src)
    print("  named                   : %d  (%d from the linker map, %d chars)"
          % (len({a for a in starts if a in names}),
             len({a for a in starts if a in defs.DVG_SYMBOLS}) if use_map else 0,
             len(char_names)))
    print("  refs from 6502 code     : %d shape(s) cited by %d LXL/LAH pair(s)"
          % (len(refs6502), sum(len(v) for v in refs6502.values())))

    # sanity: JSRL/JMPL targets should land on a block start, and blocks
    # should terminate.  Report violations instead of hiding them.
    bad = sorted(t for t in internal if not is_data(t))
    if bad:
        print("  WARNING: %d JSRL/JMPL target(s) land outside the DVG data: %s"
              % (len(bad), ", ".join("$%04X" % t for t in bad[:8])))


if __name__ == "__main__":
    main()
