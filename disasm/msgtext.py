#!/usr/bin/env python3
"""The packed message text of DSTMSG.MAC, decoded the way ASTMSG reads it.

DSTMSG.MAC's ASCIN macro packs three characters into every two bytes:

    b0 = c1<<3 | c2>>2          b1 = (c2&3)<<6 | c3<<1 | end

where each c is a five-bit code and `end` marks the last pair.  ASTMSG
($71BD) pulls the codes back out with a rotate chain (see message.c in
the C port for the bit-by-bit account) and VGMSG2 ($71EC) turns a code
into a glyph: 0 ends the message early, 1..4 are blank, '0', '1', '2'
("IF BLANK, 0, 1 OR 2"), and 5 upward are A..Z ("10. FOR A, 12. FOR
B, ..." - the code doubled, plus 14, indexes the VGMSGA JSRL table).
Digits 3-9 cannot be encoded, which is why the source's messages only
ever use 0, 1 and 2.

Each language block (L0..L3 in the source, pointed to by VGMSGT) opens
with a table of one-byte offsets, one per message, relative to the
block's own label; the first offset is also the table's length, since
message 10$ starts right after it.  Message i is the source's local
label (10+i)$ under that block, so it is named here the way the
pipeline names every local label: `L0_10`, `L0_11`, ...

MESSAGE_TABLES in astdelux_config.py says where the blocks are, per
romset; the listings (m6502trace.py, mkplain.py) call `decode_tables`
to label each message and print its text.
"""

from dvgdasm import CHARSET


def char_of(code):
    """VGMSG2's mapping.  None for the 0 terminator."""
    if code == 0:
        return None
    if code <= 4:
        return CHARSET[code - 1]
    i = code + 6
    return CHARSET[i] if i < len(CHARSET) else "?"


def unpack(mem, addr):
    """Decode one message at `addr`.  Returns (text, bytes_consumed)."""
    out = []
    a = addr
    while True:
        b0, b1 = mem[a], mem[a + 1]
        a += 2
        codes = (b0 >> 3, ((b0 & 7) << 2) | (b1 >> 6), (b1 >> 1) & 0x1F)
        for c in codes:
            ch = char_of(c)
            if ch is None:
                return "".join(out), a - addr
            out.append(ch)
        if b1 & 1:
            return "".join(out), a - addr


def decode_tables(mem, tables):
    """tables: {block address: block name}.

    Returns (offsets, messages):
        offsets[block]  -> number of entries in its offset table
        messages[addr]  -> (label, text) for every message start
    """
    offsets, messages = {}, {}
    for base, name in tables.items():
        n = mem[base]
        offsets[base] = n
        for i in range(n):
            start = base + mem[base + i]
            text, _n = unpack(mem, start)
            messages[start] = ("%s_%d" % (name, 10 + i), text)
    return offsets, messages
