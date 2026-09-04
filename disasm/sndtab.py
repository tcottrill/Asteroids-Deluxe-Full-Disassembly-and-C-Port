#!/usr/bin/env python3
"""DASOUN.MAC's sound tables, decoded the way CSOUND plays them.

Two tables, back to back in the program ROM:

PNTRS - eight bytes per sound, one per POKEY register 0-7 ($2C00+n:
    even registers are a channel's frequency AUDF1-4, odd ones its
    volume and distortion AUDC1-4).  Each byte is the offset, from the
    SOUND base, of the envelope sequence that register plays; 0 means
    the sound leaves that register alone.  The source's OFFSET macro
    builds an entry from labels named <sound><register> (T16 is thump 1
    on register 6), and defines the sound number the game passes to
    SNDON as the entry's offset plus 7 (SND.T1 = $07, SND.T2 = $0F ...).

SOUND - a mandatory 0 (offset 0 = "register free"), then the sequences.
    A sequence is a macro count (1-127; 127 in practice means "for as
    long as the sound is allowed to run") followed by 4-byte steps
    FCNT,START,CHNG,TOT - every FCNT NMIs add CHNG to the register,
    TOT times, starting from START - then a 0 that ends the repeated
    group.  If the byte after that 0 is another macro count the
    sequence goes on; if it is 0 the sequence ends.  The source writes
    these with its REP / STB / SEND / MEND macros.

`decode` returns a row per table entry, per macro count, per step and
per terminator, each with the text the listings print beside its bytes;
the coverage check in `decode` proves the sequences tile the SOUND
block exactly, with no byte unaccounted for.
"""


def _s8(v):
    return v - 256 if v >= 128 else v


def decode(mem, pntrs, sound, end, sounds):
    """sounds: [(label, description)] in PNTRS order (from the source).

    Returns (labels, rows):
        labels {addr: name}      - SOUND, for the listing's symbol table
        rows   {addr: text}      - a row starts at every key; the text is
                                   the gutter comment for that row
    Raises ValueError if the sequences do not tile [sound+1, end).
    """
    labels = {sound: "SOUND"}
    rows = {}
    seq_name = {}                       # offset -> first label that uses it
    for i, (lab, desc) in enumerate(sounds):
        row = pntrs + 8 * i
        parts = []
        for reg in range(8):
            off = mem[row + reg]
            if not off:
                continue
            name = seq_name.setdefault(off, "%s%d" % (lab, reg))
            parts.append("%s%d=%s+$%02X" % ("AUDF" if reg % 2 == 0 else "AUDC",
                                            reg // 2 + 1, name, off))
        rows[row] = "SND.%s ($%02X) %s: %s" % (lab, 8 * i + 7, desc,
                                              ", ".join(parts) or "no registers")

    rows[sound] = "SOUND: offset 0, a register with POINT 0 is free"
    covered = bytearray(end - sound)
    covered[0] = 1
    for off in sorted(seq_name):
        a = sound + off
        name = seq_name[off]
        first = True
        while True:
            n = mem[a]
            if n == 0:
                break                       # the MEND's second 0, below
            rows[a] = "%srepeat %d%s" % ("%s: " % name if first else "", n,
                                         " (for the whole sound)" if n == 127 else "")
            first = False
            covered[a - sound] = 1
            a += 1
            while mem[a] != 0:
                f, s, c, t = mem[a], mem[a + 1], mem[a + 2], mem[a + 3]
                rows[a] = "every %d NMI%s add %d, %d time%s, from $%02X" % (
                    f, "" if f == 1 else "s", _s8(c), t, "" if t == 1 else "s", s)
                for k in range(4):
                    covered[a - sound + k] = 1
                a += 4
            if mem[a + 1] == 0:
                rows[a] = "end of %s" % name
                covered[a - sound] = covered[a - sound + 1] = 1
                a += 2
                break
            rows[a] = "end of the repeated group"
            covered[a - sound] = 1
            a += 1
    gaps = [sound + i for i, c in enumerate(covered) if not c]
    if gaps:
        raise ValueError("sound block bytes not covered by any sequence: %s"
                         % ", ".join("$%04X" % g for g in gaps[:8]))
    return labels, rows
