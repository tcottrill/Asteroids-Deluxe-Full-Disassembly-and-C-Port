#!/usr/bin/env python3
"""Check this project's ROM image and 6502 decoder against an outside one.

The outside listing (not distributed with this repository; pass its path,
default `../astdelux2.asm`) is an independently produced linear
disassembly of the same rev 2 ROM set, with raw bytes beside each
instruction.  That
makes it two independent checks we did not have to trust ourselves for:

  1. Its raw bytes vs. the image gen_from_roms.py assembles from the ROM
     files.  If those agree, our image really is that ROM.
  2. Its instruction lengths and mnemonics vs. m6502.py, at every address
     it lists.  A shared error here is very unlikely - two different
     opcode tables would have to be wrong the same way.

Only the ROM regions are compared.  Outside them the two disagree by
construction: our image has $00 in the I/O window and the RAM pages,
where a dump taken from a running machine sees live values.

Undocumented opcodes have more than one accepted spelling (`isb`/`isc`,
`shs`/`tas`, `ane`/`xaa`, ...), so those are reconciled by name before
comparing rather than counted as differences.

Usage:
    python crosscheck.py [reference.asm] [--set astdelux2]
"""

import argparse
import os
import re
import sys
from collections import Counter

import m6502

ROM_REGIONS = [(0x4800, 0x5800), (0x6000, 0x8000)]

# "6000: 20 F5 7B jsr $7bf5"
LINE_RE = re.compile(r"^([0-9A-Fa-f]{4}): ((?:[0-9A-Fa-f]{2} ?){1,3})\s+(\S+)")

# Accepted alternative spellings for the undocumented opcodes.  Maps the
# other project's name onto the one m6502.py uses.
ALIASES = {
    "isb": "isc", "ins": "isc", "sbx": "axs", "jam": "kil", "asr": "alr",
    "dcm": "dcp", "aac": "anc", "sha": "ahx", "shs": "tas", "ane": "xaa",
    "lxa": "lax", "sax": "sax", "top": "nop", "dop": "nop",
}


def in_rom(a):
    return any(lo <= a < hi for lo, hi in ROM_REGIONS)


def parse(path):
    """address -> (raw bytes, mnemonic) for every line that looks like one."""
    out = {}
    with open(path) as f:
        for line in f:
            m = LINE_RE.match(line)
            if m:
                out[int(m.group(1), 16)] = (
                    [int(b, 16) for b in m.group(2).split()],
                    m.group(3).lower())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference", nargs="?",
                    default=os.path.join("..", "astdelux2.asm"))
    ap.add_argument("--set", dest="romset", default="astdelux2")
    args = ap.parse_args()

    dump = "%s_dump.bin" % args.romset
    try:
        mem = bytearray(open(dump, "rb").read())
    except FileNotFoundError:
        sys.exit("%s not found - run gen_from_roms.py first" % dump)
    try:
        ref = parse(args.reference)
    except FileNotFoundError:
        sys.exit("reference listing not found: %s" % args.reference)

    if not ref:
        sys.exit("%s: no instruction lines recognised - is it a listing with "
                 "raw bytes?" % args.reference)

    print("reference : %s" % args.reference)
    print("            %d instructions, $%04X-$%04X"
          % (len(ref), min(ref), max(ref)))
    print("image     : %s" % dump)

    # ---- 1. raw bytes -------------------------------------------------
    compared = mismatched = 0
    for a, (raw, _mn) in ref.items():
        for i, b in enumerate(raw):
            if in_rom(a + i):
                compared += 1
                if mem[a + i] != b:
                    mismatched += 1
    print("\nraw bytes inside ROM : %d compared, %d mismatched"
          % (compared, mismatched))

    # ---- 2. decoder ---------------------------------------------------
    ok, length_bad, mnem_bad = 0, [], []
    for a, (raw, mn) in sorted(ref.items()):
        if not in_rom(a):
            continue
        ins = m6502.decode(mem, a)
        if ins["size"] != len(raw):
            length_bad.append((a, len(raw), ins["size"], mn, ins["mnem"]))
        elif ins["mnem"] != ALIASES.get(mn, mn):
            mnem_bad.append((a, mn, ins["mnem"]))
        else:
            ok += 1

    total = ok + len(length_bad) + len(mnem_bad)
    print("decoder inside ROM   : %d/%d agree (%.3f%%)"
          % (ok, total, 100.0 * ok / total if total else 0.0))

    if length_bad:
        print("  instruction-length disagreements: %d" % len(length_bad))
        for a, tl, ml, tm, mm in length_bad[:15]:
            print("    $%04X  reference %dB %-5s | ours %dB %-5s | %s"
                  % (a, tl, tm, ml, mm,
                     " ".join("%02X" % b for b in mem[a:a + 3])))
    if mnem_bad:
        print("  mnemonic differences: %d" % len(mnem_bad))
        for (t, m), n in Counter((t, m) for _a, t, m in mnem_bad).most_common(15):
            print("    reference=%-6s ours=%-6s  x%d   (add to ALIASES if these "
                  "name the same opcode)" % (t, m, n))

    bad = mismatched or length_bad or mnem_bad
    print("\n%s" % ("MISMATCHES FOUND - see above" if bad
                    else "clean: image and decoder both agree with the reference"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
