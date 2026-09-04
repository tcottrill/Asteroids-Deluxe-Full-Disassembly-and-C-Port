#!/usr/bin/env python3
"""Generate FUNCTIONS.md - the routine index - from the listing.

Everything here is derived, not transcribed: names and the one-line
descriptions come from the Atari source via nameroutines.py, the caller
counts come from the trace's cross-references, and the grouping follows
the source's own module and `.SBTTL` structure.  So the index cannot
drift from the listing - regenerate and it is correct again.

The prose notes beside it (MAINLINE_NOTES.md and friends) are written by
hand; this file only builds the index they refer to.

Usage:
    python mknotes.py [--set astdelux2]
"""

import argparse
import re
import sys
from collections import defaultdict

import astdelux_defines as defs

try:
    import astdelux_names as srcnames
except ImportError:
    srcnames = None

OUT = "FUNCTIONS.md"

# Which subsystem each source module is, in reading order.
SUBSYSTEMS = [
    ("DSTRD0", "Game program",
     "The main line: startup, the frame loop, the object engine, "
     "collision, scoring and the attract mode."),
    ("DSTRD0(parked)", "Game program (parked section)",
     "Code DSTRD0.MAC parks in the space left over after TRIROT, using "
     "the ENTSEC/XITSEC macros. It is ordinary game code; only its "
     "address range is unusual."),
    ("TRIROT", "Rotation, scaling and the special rock",
     "Lives in the vector-board ROM at $4800. The 16-bit maths the rest "
     "of the game leans on - multiply, divide, sine/cosine, rotation - "
     "plus the special (Deluxe) rock's control logic."),
    ("DSTCUT", "Vector generator utilities",
     "The routines that build a DVG display list: emit a vector, an "
     "absolute position, a character, a subroutine call."),
    ("DSTMSG", "Messages",
     "The packed text format and its unpacker, the language tables, and "
     "the copyright message with its tamper check."),
    ("DASOUN", "Sound",
     "Sequenced sound: eight channels of table-driven envelopes poked "
     "into the POKEY, plus the discrete explosion and thrust circuits."),
    ("DSTNMI", "NMI and coin handling",
     "The 4 ms interrupt, and the coin/credit machinery included from "
     "DCIN65.MAC."),
    ("EAROM", "Non-volatile storage",
     "Reading and writing the EAROM that holds high scores and "
     "bookkeeping."),
    ("BOUNCE", "Rock bouncing",
     "The Deluxe-specific behaviour where rocks rebound."),
    ("SHUFFL", "Link order",
     "The linker's section ordering. Its only bytes in the image are "
     "the 6502 vectors at $7FFA."),
    ("DSTTST", "Self-test",
     "The operator self-test: RAM, ROM checksums, the vector generator, "
     "switches, the POKEY and the EAROM."),
]

LABEL_RE = re.compile(r"^([A-Za-z_$][A-Za-z0-9_.$]*):\s*$")
ADDR_RE = re.compile(r"^([0-9A-F]{4}):")
XREF_RE = re.compile(r"^; xrefs: (.*)$")
DATA_RE = re.compile(r"^; ---- data \$([0-9A-F]{4})-\$([0-9A-F]{4}) \((\d+) bytes\)")


def parse_listing(path):
    """-> [ {name, addr, size, callers, is_data, comment} ] in address order."""
    entries, lines = [], open(path, encoding="utf-8").read().splitlines()
    cur = None
    for i, l in enumerate(lines):
        m = LABEL_RE.match(l)
        if m:
            # the address is on the next line that starts with one
            addr = None
            for j in range(i + 1, min(i + 14, len(lines))):
                a = ADDR_RE.match(lines[j])
                d = DATA_RE.match(lines[j])
                if d:
                    addr = int(d.group(1), 16)
                    break
                if a:
                    addr = int(a.group(1), 16)
                    break
            if addr is None:
                continue
            cur = {"name": m.group(1), "addr": addr, "callers": 0,
                   "is_data": False, "comment": ""}
            entries.append(cur)
            continue
        if cur is None:
            continue
        m = XREF_RE.match(l)
        if m and not cur["callers"]:
            shown = len(re.findall(r"\$[0-9A-F]{4}", m.group(1)))
            more = re.search(r"\+(\d+) more", m.group(1))
            cur["callers"] = shown + (int(more.group(1)) if more else 0)
        if DATA_RE.match(l):
            cur["is_data"] = True

    entries.sort(key=lambda e: e["addr"])
    for a, b in zip(entries, entries[1:]):
        a["size"] = b["addr"] - a["addr"]
    if entries:
        entries[-1]["size"] = 0
    return entries


def format_banner(text, known):
    """Render a `.SBTTL` heading readably without losing the label.

    The source writes them in caps, usually as `LABEL-what it does`.
    Split the label off only when the first word really is one - "MAIN
    LINE LOOP" is a title, not the routine MAIN followed by "line loop".
    """
    m = (re.match(r"^([A-Z][A-Z0-9$.]{1,7})\s*[-—]\s*(.+)$", text)
         or re.match(r"^([A-Z][A-Z0-9$.]{1,7})\s+(.{4,})$", text))
    if m and m.group(1) in known and not m.group(2).startswith("-"):
        return "`%s` — %s" % (m.group(1), m.group(2).strip().lower())
    return text.capitalize() if text.isupper() else text


def describe(addr):
    """A one-line description, taken from the source's own words."""
    if not srcnames:
        return ""
    blocks = getattr(srcnames, "LABEL_BLOCKS", {}).get(addr)
    if blocks:
        return blocks[0].replace("\t", " ").strip()
    return (getattr(srcnames, "INSTR_COMMENTS", {}).get(addr, "")
            .replace("\t", " ").strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", dest="romset", default="astdelux2")
    args = ap.parse_args()

    listing = "%s_main.asm" % args.romset
    try:
        entries = parse_listing(listing)
    except FileNotFoundError:
        sys.exit("%s not found - run m6502trace.py first" % listing)

    module_of = getattr(srcnames, "MODULE_OF", {}) if srcnames else {}
    banners = getattr(srcnames, "BANNERS", {}) if srcnames else {}
    known_names = set(getattr(srcnames, "ROUTINE_NAMES", {}).values())
    known_names |= set(defs.ROM_SYMBOLS.values())

    # Linker-map symbols have no module recorded (they come from the map,
    # not the source walk).  A module occupies a contiguous stretch, so
    # attribute each straggler to the nearest attributed address before it.
    known = sorted(module_of)
    for e in entries:
        if e["addr"] in module_of:
            continue
        before = [a for a in known if a <= e["addr"]]
        if before:
            module_of[e["addr"]] = module_of[before[-1]]

    by_module = defaultdict(list)
    for e in entries:
        by_module[module_of.get(e["addr"], "(unattributed)")].append(e)

    L = ["# Asteroids Deluxe — routine index", "",
         "Every named location in `%s`, grouped by the source module it" % listing,
         "came from and, inside that, by the source's own `.SBTTL` sections.",
         "",
         "GENERATED by `mknotes.py` — do not hand-edit. Names and the",
         "one-line descriptions are the original Atari ones; caller counts",
         "come from the trace's cross-references. The prose notes beside",
         "this file explain how the pieces fit together.",
         ""]

    total = sum(len(v) for v in by_module.values())
    L += ["%d named locations across %d modules." % (total, len(by_module)), ""]

    ordered = [m for m, _t, _d in SUBSYSTEMS if m in by_module]
    ordered += [m for m in sorted(by_module) if m not in ordered]

    L.append("| module | subsystem | named |")
    L.append("|---|---|---|")
    titles = {m: (t, d) for m, t, d in SUBSYSTEMS}
    for m in ordered:
        t = titles.get(m, ("—", ""))[0]
        L.append("| `%s` | %s | %d |" % (m, t, len(by_module[m])))
    L.append("")

    for m in ordered:
        t, desc = titles.get(m, (m, ""))
        L += ["", "## %s — `%s`" % (t, m), ""]
        if desc:
            L += [desc, ""]
        section = None
        rows = []

        def flush():
            if not rows:
                return
            L.append("| addr | name | size | callers | from the source |")
            L.append("|---|---|---|---|---|")
            L.extend(rows)
            L.append("")
            del rows[:]

        for e in sorted(by_module[m], key=lambda x: x["addr"]):
            b = banners.get(e["addr"])
            if b and b != section:
                flush()
                section = b
                L += ["### %s" % format_banner(b, known_names), ""]
            note = describe(e["addr"]).replace("|", "\\|")
            rows.append("| `$%04X` | `%s` | %d | %s | %s |"
                        % (e["addr"], e["name"], e.get("size", 0),
                           e["callers"] or "—",
                           ("*data* — " + note) if e["is_data"] else note))
        flush()

    open(OUT, "w", encoding="utf-8").write("\n".join(L).rstrip() + "\n")
    print("wrote %s (%d entries, %d modules)" % (OUT, total, len(by_module)))


if __name__ == "__main__":
    main()
