#!/usr/bin/env python3
"""NMOS 6502 instruction table and decoder shared by the tools here.

Undocumented opcodes are present but flagged: hitting one during a trace
is nearly always the signal that flow has wandered into data, so the
tracer treats `undoc` as a reason to stop and report rather than to keep
walking.
"""

# addressing mode -> (operand length, format string)
MODES = {
    "imp": (0, "%s"),
    "acc": (0, "%s a"),
    "imm": (1, "%s #$%02X"),
    "zp":  (1, "%s $%02X"),
    "zpx": (1, "%s $%02X,x"),
    "zpy": (1, "%s $%02X,y"),
    "izx": (1, "%s ($%02X,x)"),
    "izy": (1, "%s ($%02X),y"),
    "rel": (1, "%s $%04X"),
    "abs": (2, "%s $%04X"),
    "abx": (2, "%s $%04X,x"),
    "aby": (2, "%s $%04X,y"),
    "ind": (2, "%s ($%04X)"),
}

# 256 entries: (mnemonic, mode, undocumented)
_T = """
brk imp;ora izx;kil imp*;slo izx*;nop zp*;ora zp;asl zp;slo zp*
php imp;ora imm;asl acc;anc imm*;nop abs*;ora abs;asl abs;slo abs*
bpl rel;ora izy;kil imp*;slo izy*;nop zpx*;ora zpx;asl zpx;slo zpx*
clc imp;ora aby;nop imp*;slo aby*;nop abx*;ora abx;asl abx;slo abx*
jsr abs;and izx;kil imp*;rla izx*;bit zp;and zp;rol zp;rla zp*
plp imp;and imm;rol acc;anc imm*;bit abs;and abs;rol abs;rla abs*
bmi rel;and izy;kil imp*;rla izy*;nop zpx*;and zpx;rol zpx;rla zpx*
sec imp;and aby;nop imp*;rla aby*;nop abx*;and abx;rol abx;rla abx*
rti imp;eor izx;kil imp*;sre izx*;nop zp*;eor zp;lsr zp;sre zp*
pha imp;eor imm;lsr acc;alr imm*;jmp abs;eor abs;lsr abs;sre abs*
bvc rel;eor izy;kil imp*;sre izy*;nop zpx*;eor zpx;lsr zpx;sre zpx*
cli imp;eor aby;nop imp*;sre aby*;nop abx*;eor abx;lsr abx;sre abx*
rts imp;adc izx;kil imp*;rra izx*;nop zp*;adc zp;ror zp;rra zp*
pla imp;adc imm;ror acc;arr imm*;jmp ind;adc abs;ror abs;rra abs*
bvs rel;adc izy;kil imp*;rra izy*;nop zpx*;adc zpx;ror zpx;rra zpx*
sei imp;adc aby;nop imp*;rra aby*;nop abx*;adc abx;ror abx;rra abx*
nop imm*;sta izx;nop imm*;sax izx*;sty zp;sta zp;stx zp;sax zp*
dey imp;nop imm*;txa imp;xaa imm*;sty abs;sta abs;stx abs;sax abs*
bcc rel;sta izy;kil imp*;ahx izy*;sty zpx;sta zpx;stx zpy;sax zpy*
tya imp;sta aby;txs imp;tas aby*;shy abx*;sta abx;shx aby*;ahx aby*
ldy imm;lda izx;ldx imm;lax izx*;ldy zp;lda zp;ldx zp;lax zp*
tay imp;lda imm;tax imp;lax imm*;ldy abs;lda abs;ldx abs;lax abs*
bcs rel;lda izy;kil imp*;lax izy*;ldy zpx;lda zpx;ldx zpy;lax zpy*
clv imp;lda aby;tsx imp;las aby*;ldy abx;lda abx;ldx aby;lax aby*
cpy imm;cmp izx;nop imm*;dcp izx*;cpy zp;cmp zp;dec zp;dcp zp*
iny imp;cmp imm;dex imp;axs imm*;cpy abs;cmp abs;dec abs;dcp abs*
bne rel;cmp izy;kil imp*;dcp izy*;nop zpx*;cmp zpx;dec zpx;dcp zpx*
cld imp;cmp aby;nop imp*;dcp aby*;nop abx*;cmp abx;dec abx;dcp abx*
cpx imm;sbc izx;nop imm*;isc izx*;cpx zp;sbc zp;inc zp;isc zp*
inx imp;sbc imm;nop imp;sbc imm*;cpx abs;sbc abs;inc abs;isc abs*
beq rel;sbc izy;kil imp*;isc izy*;nop zpx*;sbc zpx;inc zpx;isc zpx*
sed imp;sbc aby;nop imp*;isc aby*;nop abx*;sbc abx;inc abx;isc abx*
"""

OPCODES = []
for _entry in (e.strip() for _line in _T.strip().splitlines()
               for e in _line.split(";")):
    _undoc = _entry.endswith("*")
    _mnem, _mode = _entry.rstrip("*").split()
    OPCODES.append((_mnem, _mode, _undoc))
assert len(OPCODES) == 256, len(OPCODES)

# control-flow classification
BRANCHES = {"bpl", "bmi", "bvc", "bvs", "bcc", "bcs", "bne", "beq"}
TERMINAL = {"rts", "rti", "kil"}          # flow stops dead here


def length(opcode):
    return 1 + MODES[OPCODES[opcode][1]][0]


def decode(mem, addr):
    """Decode one instruction.

    Returns a dict:
        addr, size, mnem, mode, undoc, operand (int or None),
        target (absolute address the instruction names, or None)
    """
    op = mem[addr]
    mnem, mode, undoc = OPCODES[op]
    nbytes, _fmt = MODES[mode]
    operand = None
    target = None
    if nbytes == 1:
        operand = mem[(addr + 1) & 0xFFFF]
        if mode == "rel":
            rel = operand - 256 if operand & 0x80 else operand
            target = (addr + 2 + rel) & 0xFFFF
            operand = target
    elif nbytes == 2:
        operand = mem[(addr + 1) & 0xFFFF] | (mem[(addr + 2) & 0xFFFF] << 8)
        if mnem in ("jmp", "jsr"):
            target = operand
    return {
        "addr": addr, "size": 1 + nbytes, "mnem": mnem, "mode": mode,
        "undoc": undoc, "operand": operand, "target": target,
    }


def text(ins, symbol=None):
    """Render an instruction; `symbol` replaces the operand if given."""
    mnem, mode = ins["mnem"], ins["mode"]
    fmt = MODES[mode][1]
    if MODES[mode][0] == 0:
        return fmt % mnem
    if symbol is not None:
        # substitute the name for the numeric field of the same shape
        numeric = "$%04X" if mode in ("rel", "abs", "abx", "aby", "ind") else "$%02X"
        return (fmt % (mnem, ins["operand"])).replace(numeric % ins["operand"],
                                                      symbol, 1)
    return fmt % (mnem, ins["operand"])
