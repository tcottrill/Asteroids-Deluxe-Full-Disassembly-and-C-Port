"""Hand-maintained trace configuration for m6502trace.py.

astdelux_defines.py is generated from the Atari source archive; this file
is where knowledge that has to be *worked out* lives.  It is meant to
grow: run m6502trace.py, read the TODO list it prints, resolve one entry,
record it here with the evidence, run again.  That loop is how coverage
closes.

Everything is keyed by romset, because the revisions are genuinely
different programs - page zero moved by 7 bytes and code shifted between
rev 2 and rev 3, so an address that is a checksum byte in one is
mid-instruction in the other.  Sharing these tables across revisions
would silently corrupt a listing.

Nothing here should be a guess.  Each entry cites what settled it:
a line of the original source, the linker map, or a decode that only has
one consistent reading.
"""


# ---------------------------------------------------------------------------
# Byte ranges that are data, as (lo, hi, why).  `hi` is exclusive.
#
# The tracer refuses to walk into these.  That matters because most of
# them sit right after a branch that is taken 100% of the time (the
# flags are known at that point) - the walk cannot know that, follows the
# fall-through, and would otherwise decode a chunk of a data table as
# instructions and invent labels pointing into it.
#
# Regions the trace already leaves alone do not need declaring.
# ---------------------------------------------------------------------------

DATA_REGIONS = {
    "astdelux2": [
        # The six checksum bytes.  CKSUM.MAC is explicit about these: it
        # links last and patches one byte at each address so the block
        # sums to a constant.  Five of the six are one byte stranded in
        # the middle of code, which is why the trace trips over them.
        (0x49A3, 0x49A4, "CKSUM0 - CKSUM.MAC: .CKSUM 49A3,0BA"),
        (0x53C5, 0x53C6, "CKSUM1 - CKSUM.MAC: .CKSUM 53C5,0B4"),
        (0x6101, 0x6102, "CKSUM2 - CKSUM.MAC: .CKSUM 6101,031"),
        (0x6920, 0x6921, "CKSUM3 - CKSUM.MAC: .CKSUM 6920,0A6"),
        (0x7120, 0x7121, "CKSUM4 - CKSUM.MAC: .CKSUM 7120,074"),
        (0x7D81, 0x7D82, "CKSUM5 - CKSUM.MAC: .CKSUM 7D81,02C"),
        # (The seventh, .CKSUM 6080,003, is not listed: $6080 is the
        #  immediate operand of `eor #$03` at $607F, so the linker patches
        #  it into an instruction and the trace covers it as code.)

        (0x4D7D, 0x4D80,
         "HITSCR, the three rock scores (DSTRD0.MAC:3023 `HITSCR: .BYTE "
         "10,5,2`), read by SPLIT_15 at $6FAD as `lda $4D7D,x`.  Sits "
         "right after CHKST1's last branch: $4D7B `bne` follows `lda #$03` "
         "/ `sta BNKSEL`, so Z is clear and the branch is always taken, "
         "and the walk would otherwise fall into the table"),

        (0x7219, 0x752C,
         "DSTMSG packed message text.  Verified byte-for-byte against the "
         "source: VGMSGS at $7219 is DSTMSG.MAC's `.BYTE 068,0B6 / 72,0B6 "
         "/ 00C,0AA ...`, then VGMSGT at $7235 holds the four language "
         "table pointers $723D/$72F3/$73C2/$747D = L0-L3.  Runs to the "
         "end of the DSTMSG section ($752B; BOUNC starts at $752C)"),

        (0x4B2B, 0x4B3D,
         "TRIROT.MAC's ITXL/ITXH/ITYL/ITYH/ITIME/IANG tables - six "
         "`.BYTE x,y,z` triples, 18 bytes, ending where RNDXYI starts. "
         "The trace walks in here off the fall-through of $4B29 `bpl "
         "SETTIP_5`, which the source itself annotates `(ALWAYS)`. The "
         "bytes confirm it: $4B2B reads 00 58 70, exactly ITXL's "
         "`.BYTE 0,58,70`"),

        (0x7BD2, 0x7BD9,
         "EAROM.MAC's EABDS table: `.BYTE 0,1,2,INITL-HSCORE,+1,+2,-1`. "
         "With INITL=$44 and HSCORE=$23 that is 00 01 02 21 22 23 FF, "
         "which is what these seven bytes read. Reached the same way - "
         "the fall-through of $7BD0 `beq`, commented `(ALWAYS)`"),

        (0x77DE, 0x77F5,
         "three small linker sections that are pure data: RADDR (BUFA "
         "$77DE, ROCKSA $77E2, 12 bytes), MODULO ($77EA, 8 bytes of bonus "
         "constants), and FILL ($77F2) - DSFILL.MAC's decoy `ADC #15 / "
         "RTS`, placed so the ROM has no obviously empty space"),
    ],

    # Rev 3 shares the vector-board data layout from $4D80 up (those bytes
    # are byte-identical to rev 2), but its program ROM is shifted, so
    # nothing below $4D80 or above $6000 carries over.
    "astdelux": [
        (0x4D75, 0x4D80,
         "filler up to EXPPIC, same construction as rev 2: $4D73 `bne` "
         "follows `lda #$03` / `sta BNKSEL` and is always taken"),

        (0x7799, 0x77AD,
         "RADDR (BUFA/ROCKSA address table, 12 bytes) followed by MODULO "
         "(8 bytes of bonus constants).  Located by byte-identical match "
         "with the rev-2 sections the linker map names at $77DE and "
         "$77EA - rev 3 shifts them down by $45.  Rev 3 drops the FILL "
         "decoy and the second copyright message that follow them in "
         "rev 2, padding with $FF instead"),

        (0x7D8A, 0x7D8B,
         "checksum byte 5 (rev 2's CKSUM5, .CKSUM 7D81,02C).  The "
         "surrounding bytes are identical in both revisions - rev 2 "
         "$7D7B and rev 3 $7D84 both read "
         "`E0 48 90 D4 B0 72 <ck> A0 00 F0 0E` - and only the checksum "
         "value itself differs ($2C vs $64), which is what a per-build "
         "checksum byte must do"),
    ],

    "astdelux1": [],
}


# ---------------------------------------------------------------------------
# Linker-map entry points that are real 6502 code but that nothing in the
# shipped ROM calls, so a vector walk can never reach them.  DSTRD0.MAC
# declares them .GLOBL and the diagnostic build (GONOGO) links against
# them, which is why they are in the map at all.
# ---------------------------------------------------------------------------

CODE_ENTRY_POINTS = {
    "astdelux2": {
        0x7A19: "VGJMPL - builds a DVG JMPL word ($E000). Reads as "
                "`lsr / and #$0F / ora #$E0` then falls into the shared "
                "tail at $7A1E, exactly mirroring VGJSRL at $7A2A "
                "(`... ora #$C0`), which IS called. No `jsr $7A19` exists "
                "anywhere in the image",
        0x7A72: "VGVCTR - the general vector emitter. Decodes cleanly for "
                "64+ instructions from `ldx #$00 / lda XCOMP+1 / cmp #$80`. "
                "No `jsr $7A72` exists in the image; the game emits vectors "
                "through VGADD/VGSABS instead",
    },
    "astdelux": {},
    "astdelux1": {},
}


# ---------------------------------------------------------------------------
# Extra entry points, as {address: why}.  RESET / NMI / IRQ are read from
# the image itself and do not belong here.
# ---------------------------------------------------------------------------

EXTRA_ENTRY_POINTS = {
    "astdelux2": {},
    "astdelux": {},
    "astdelux1": {},
}


# ---------------------------------------------------------------------------
# Targets of `jmp ($xxxx)`, as {address of the jmp: (targets, ...)}.
# The tracer reports every indirect jump it could not follow.
# ---------------------------------------------------------------------------

JUMP_TABLES = {
    "astdelux2": {},
    "astdelux": {},
    "astdelux1": {},
}


# ---------------------------------------------------------------------------
# Routines whose JSR is followed by inline data rather than by the next
# instruction, as {routine address: bytes to skip}.  Empty until such a
# routine is actually identified - a wrong guess here corrupts the
# listing from the call site onward.
# ---------------------------------------------------------------------------

INLINE_DATA_CALLS = {
    "astdelux2": {},
    "astdelux": {},
    "astdelux1": {},
}


# ---------------------------------------------------------------------------
# Names placed by hand, as {address: (name, module, evidence)}.
#
# nameroutines.py places a label only where the source's opcode sequence
# matches the listing at exactly one address, and then insists that the
# labels of one module land in increasing address order.  A handful of
# locations defeat that on purpose:
#
#   - the source branches to an unlabelled address (`BCS .`, `BNE .`),
#     so there is no label to recover - these are NAMED HERE, not
#     recovered, and the name says what the instruction does;
#   - a label's signature is a bare `RTS`, which matches everywhere;
#   - a routine is linked into another module's address range (INISOU
#     lives in DASOUN.MAC but sits among the DSTNMI code), so the
#     in-module order check rejects it;
#   - a data table is built by a macro, so its bytes are computed and the
#     by-content placement cannot see them.
#
# Each entry cites the source line.  Local labels keep the pipeline's
# convention (`33$` under `$CNVRT` is `$CNVRT_33`); a twin of a placed
# name is qualified by module, as the pipeline does.  These are merged
# after the automatic placement and go through the same order check.
# ---------------------------------------------------------------------------

HAND_NAMES = {
    "astdelux2": {
        0x48B1: ("TRIROT_RTS.0", "TRIROT",
                 "TRIROT.MAC:214 `RTS.0: RTS`, FREESR's exit. A bare RTS "
                 "matches at every RTS in the ROM, so the order prune drops "
                 "it. DSTRD0.MAC has its own RTS.0 (placed at $669C as "
                 "DSTRD0_RTS.0); qualified by module the same way"),
        0x4C02: ("CHKST_CRAPOUT", "DSTRD0",
                 "DSTRD0.MAC:311 `BCS .  ;YES. CRAP OUT`. The source branches "
                 "to itself without a label; NAMED here for its comment. "
                 "Spins until the watchdog resets the board"),
        0x4D7D: ("HITSCR", "DSTRD0",
                 "DSTRD0.MAC:3023 `HITSCR: .BYTE 10,5,2  ;SCORES FOR HITTING "
                 "SMALL, MEDIUM & LARGE ROCKS`, parked after CHKST1 by "
                 "ENTSEC/XITSEC and read by SPLIT_15 ($6FAD) as `lda "
                 "$4D7D,x`. Three bytes is too short for the by-content "
                 "placement to trust"),
        0x6F0A: ("TABLE", "DSTRD0",
                 "DSTRD0.MAC:2891 `TABLE: CT 0,0,0,0 ...`, the CPXROT "
                 "control table: bit 7 negate pix select, 5 swap X with Y, "
                 "1 X sign mask, 0 Y sign mask. Eight bytes, one per "
                 "orientation octant. Built by the CT macro, so the "
                 "by-content placement cannot see its bytes"),
        0x784F: ("INISOU", "DASOUN",
                 "DASOUN.MAC:423 `INISOU:: LDA I,0 / LDX #7 / 1$: STA X,POINT "
                 "/ DEX / BPL 1$ / STA AUDCTL`. Linked into the DSTNMI "
                 "region, away from the rest of DASOUN, so the in-module "
                 "order check rejects it. ST1 is the linker map's name for "
                 "the address and is kept as the alias"),
        0x7853: ("INISOU_1", "DASOUN",
                 "DASOUN.MAC:425 `1$: STA X,POINT`, the clear loop"),
        0x7880: ("NMI_HANG", "DSTNMI",
                 "DSTNMI.MAC:90 `BNE .  ;PROGRAM WHERE ARE YOU?`. The source "
                 "branches to itself without a label; NAMED here. Reached "
                 "when the main line misses two SYNC ticks, and spins until "
                 "the watchdog resets the board"),
        0x7954: ("$CNVRT", "DSTNMI",
                 "DCIN65.MAC:585 `$CNVRT: GCM`, coins to credits. "
                 "$EXTB (DCIN65.MAC:579), the bonus adder's exit, is the "
                 "same address"),
        0x796D: ("$CNVRT_33", "DSTNMI",
                 "DCIN65.MAC:602 `33$:  ;GENERATE CREDITS: 1 OR 2`"),
        0x7973: ("$CNVRT_1", "DSTNMI",
                 "DCIN65.MAC:606 `1$: INC $$CRDT`"),
        0x7975: ("$CNVRT_2", "DSTNMI",
                 "DCIN65.MAC:607 `2$: STA $CNCT  ;UPDAT COINCT`"),
        0x7FBF: ("STEST7_FILL", "DSTTST",
                 "DSTTST.MAC:460 `.REPT STEST7+81.-. / .BYTE -1 / .ENDR  "
                 ";FILLER SO ADDRESS DON'T CHANGE`. Unlabelled padding "
                 "that pins PKYTST to $7FC1; NAMED here"),
        0x72F3: ("L1", "DSTMSG",
                 "DSTMSG.MAC:286 `L1: .BYTE 10$-L1,...`, the German message "
                 "table; VGMSGT's second word ($7237) is $72F3. L0 was "
                 "placed by content; L1-L3 are offset tables whose bytes "
                 "depend on their own position, so by-content placement "
                 "cannot see them"),
        0x73C2: ("L2", "DSTMSG",
                 "DSTMSG.MAC:303 `L2: .BYTE 10$-L2,...`, the French message "
                 "table; VGMSGT's third word ($7239) is $73C2"),
        0x747D: ("L3", "DSTMSG",
                 "DSTMSG.MAC:325 `L3: .BYTE 10$-L3,...`, the Spanish message "
                 "table; VGMSGT's fourth word ($723B) is $747D"),
        0x7FEE: ("ROMX", "DSTTST",
                 "DSTTST.MAC:502 `ROMX: LET R,N,D,E,H,J`, the ROM position "
                 "letters the checksum display prints, encoded "
                 "(letter-'A'+11)*2: 38 30 1C 1E 24 28"),
        0x7FF4: ("ROMY", "DSTTST",
                 "DSTTST.MAC:503 `ROMY: NUM 2,2,1,1,1,1`, the matching "
                 "position digits, encoded (n+1)*2: 06 06 04 04 04 04"),
        0x7FFA: ("VCTRS", "SHUFFL",
                 "SHUFFL.MAC:23 `.VCTRS ^H7FFA,NMI,PWRON,PWRON`, the 6502 "
                 "vectors: NMI $785C, RESET $7CD7, IRQ/BRK $7CD7. The macro "
                 "emits no label; NAMED here after the macro"),
    },
    "astdelux": {},
    "astdelux1": {},
}


# ---------------------------------------------------------------------------
# The packed message text, as {block address: block name}: DSTMSG.MAC's
# four language blocks, each an offset table followed by ASCIN-packed
# strings (msgtext.py decodes them).  The addresses are the four words
# of VGMSGT ($7235), which VGME indexes by the language option switch.
# ---------------------------------------------------------------------------

MESSAGE_TABLES = {
    "astdelux2": {
        0x723D: "L0",       # English
        0x72F3: "L1",       # German
        0x73C2: "L2",       # French
        0x747D: "L3",       # Spanish
    },
    "astdelux": {},
    "astdelux1": {},
}


# ---------------------------------------------------------------------------
# The sound tables (sndtab.py decodes them): where PNTRS and SOUND sit,
# where the block ends, and the twelve sounds in PNTRS order with the
# source's own labels and comments (DASOUN.MAC:111-122).  The addresses
# are the linker map's DASOUN section ($760D) and the `lda $766D,y` /
# `$766E,y` / `$766F,y` / `$7670,y` reads in CSOUND, which are the
# source's `LDA Y,SOUND` .. `LDA Y,SOUND+3`.
# ---------------------------------------------------------------------------

SOUND_TABLES = {
    "astdelux2": {
        "pntrs": 0x760D, "sound": 0x766D, "end": 0x7752,
        "sounds": [
            ("T1", "thump 1"),
            ("T2", "thump 2"),
            ("LS", "little saucer"),
            ("SF", "saucer fire"),
            ("PF", "ship fire"),
            ("EL", "extra life"),
            ("BS", "big saucer"),
            ("DI", "diamond zapped"),
            ("BL", "ship back to life"),
            ("HT", "on the high score table"),
            ("S",  "shields"),
            ("DS", "death star appearance"),
        ],
    },
    "astdelux": {},
    "astdelux1": {},
}


# ---------------------------------------------------------------------------
# Our own descriptions of labels, as {address: text or [lines]}.  These
# are OURS, not Atari's: written from the C port's per-routine comments
# (each reviewed against the listing) and from reading the code.  The
# listings print them marked `[note]` so they can never be mistaken for
# a recovered comment.  Any label may have one - a routine, its local
# labels (NAME_20 is the source's 20$), or a data table - and a note may
# sit under a source block where that block is thin.  A single string is
# one line; a list is printed as consecutive lines.
# ---------------------------------------------------------------------------

ROUTINE_NOTES = {
    "astdelux2": {
        # ---- DSTMSG: the message processor and its text ----------------
        0x718F: ["Display message Y in the language the option switch selects",
                 "(OPTN4 bits 0-1: 0 English, 1 German, 2 French, 3 Spanish), in",
                 "medium characters.  Falls into VGME."],
        0x7198: ["Display message Y in language A.  VGMSG arrives here with the",
                 "switch's language; SETROL's copyright line enters here with A=0",
                 "(English only).  Fetch the language block's address from VGMSGT,",
                 "add that block's offset for message Y, and TEMP1 now points at",
                 "the packed text."],
        0x71AC: ["The message's screen position: VGMSGS holds an X,Y pair per",
                 "message number, in beam units over 4 as VGSABS takes them.",
                 "A = X, X = Y, then fall into CM."],
        0x71B5: ["Position the beam at (A,X), emit a WAIT so it settles, then",
                 "unpack the text at TEMP1 into the display list (falls into ASTMSG)."],
        0x71BD: ["Unpack ASCIN-packed text into the display list.  Every two bytes",
                 "hold three 5-bit character codes: b0 = c1<<3 | c2>>2, and",
                 "b1 = (c2 & 3)<<6 | c3<<1 | end.  Each code goes to VGMSG1/VGMSG2,",
                 "which append the glyph's JSRL word.  Stops at the end bit, or",
                 "early on a code of 0."],
        0x71C1: ["The loop, one byte pair per pass: code 1 is the top five bits of",
                 "the first byte; code 2 straddles the two bytes, assembled by the",
                 "rotate chain; code 3 is bits 5-1 of the second byte, whose bit 0",
                 "ends the message."],
        0x71E2: ["Message done: Y counts the bytes written, VGADD moves VGLIST",
                 "past them."],
        0x71E6: ["Step the text pointer TEMP1 (carry into the high byte), then",
                 "emit the character in A - A is already the code doubled."],
        0x71EC: ["Emit one character; A = code*2.  Zero ends the message: the two",
                 "PLAs drop VGMSG1's return address so the BNE lands in VGMSG0.",
                 "Codes 1-4 are blank, 0, 1, 2; any other code has 14 added so it",
                 "indexes A..Z in the VGMSGA JSRL table at $56F8.  In copyright-test",
                 "mode (CPMTST set) the bytes are checked by CPR.UP, not written."],
        0x71F4: "Not the end: map the code to its VGMSGA table index (see VGMSG2).",
        0x71FA: ["Fetch the glyph's JSRL word from VGMSGA ($56F6+X, X = 2*code or",
                 "2*code+14) and store both bytes at VGLIST,Y - unless CPMTST says",
                 "to test them instead."],
        0x720B: "Common exit: X back to 0 for the next (TEMP1,X) read.",
        0x720E: ["Copyright test mode: fold both bytes of the JSRL word into the",
                 "error flag through CPR.UP instead of storing them."],
        0x7219: ["Screen position of every message, an X,Y byte pair per message",
                 "number: 0 HIGH SCORES, 1 PLAYER, 2 YOUR SCORE IS ONE OF THE TEN",
                 "BEST, 3 PLEASE ENTER YOUR INITIALS, 4 PRESS ROTATE TO SELECT",
                 "LETTER, 5 PRESS SHIELD WHEN LETTER IS CORRECT, 6 PRESS START,",
                 "7 GAME OVER, 8 1 COIN 2 CREDITS, 9 1 COIN 1 CREDIT, 10 2 COINS 1",
                 "CREDIT, 11 CREDITS, 12 2 GAME MINIMUM, 13 BONUS AT.  Units are",
                 "beam position over 4, as VGSABS takes them."],
        0x7235: ["The four language blocks, one word each: L0 English, L1 German,",
                 "L2 French, L3 Spanish.  VGME indexes it by 2*language."],
        0x723D: ["English.  14 one-byte offsets, one per message number, relative",
                 "to L0, then the packed text (three characters per two bytes,",
                 "decoded in the gutter).  Message 13, BONUS AT, is English only."],
        0x72F3: ["German.  13 offsets relative to L1, then the packed text."],
        0x73C2: ["French.  13 offsets relative to L2, then the packed text."],
        0x747D: ["Spanish.  13 offsets relative to L3, then the packed text."],
        # ---- TRI: the special rock's pictures ---------------------------
        0x502A: ["The special rock's picture table: 48 JSRL words (TRI.MAC).  The",
                 "first 32 (TFPIX..TFPIX+63) are TRI00-TRI37, the single triangle",
                 "at 32 orientations 1/32 turn apart; TRIPIX indexes them by",
                 "SRANG>>2 with bit 0 cleared.  The next 16 (TFPIX+64..+95) are",
                 "FRM00-FRM17, the big piece - two triangles point to point - at 16",
                 "orientations; TRIPIX takes those when SRTIME bit 0 says big, and",
                 "SHPEXP lifts one vector from them for each fragment of the",
                 "exploding ship.  Every word is decoded in the vector listing."],
        0x508A: ["TRI00-TRI37: the special rock's triangle at 32 orientations, 18",
                 "bytes each (four VCTRs and an RTSL), reached through TFPIX.",
                 "Decoded in the vector listing."],
        0x52CA: ["FRM00-FRM17: the big special rock at 16 orientations - an SVEC",
                 "out, JSRL to one triangle, an SVEC back, and a JMPL to the",
                 "triangle facing the other way (TRI00 and TRI20 for FRM00).",
                 "Reached through TFPIX+64; also where SHPEXP takes the ship's",
                 "explosion fragments from."],
        # ---- DASOUN: the sound sequencer and its tables ----------------
        0x7752: ["Turn sound Y off.  `bit SNDOO_1` reads the RTS opcode ($60),",
                 "whose bit 6 sets V; SNDOO_5 then writes 0 in place of each",
                 "register's offset.  Joins SNDOO_4 with carry clear."],
        0x7758: "Start sound Y with priority: carry set takes a busy register over.",
        0x775B: "Start sound Y on free registers only (carry clear); falls into SNDOO.",
        0x775C: ["Common body of SNDON/SNDPON.  In attract mode (NPLAYR = 0) every",
                 "request turns into SNDOFF.  V clear means turn on."],
        0x7761: ["Walk the eight POKEY registers, X = 7 down to 0, with Y on the",
                 "sound's PNTRS entry, last byte first (Y arrives as offset + 7)."],
        0x7765: "Without priority (C clear) a register whose POINT is busy is left alone.",
        0x776B: ["This register's sequence offset from PNTRS; 0 means the sound does",
                 "not use it.  V set (SNDOFF) substitutes 0."],
        0x7774: ["Hand the register to CSOUND in the order the running NMI needs:",
                 "POINT = 0 stops whatever was playing, MCOUNT = $80 flags a start,",
                 "then POINT = the offset.  CSOUND takes it from there."],
        0x7780: "Next register.",
        0x7786: "Shared return; SNDOFF also BITs this $60 to set V.",
        0x7787: ["The sequencer, run once per NMI: for each of the eight POKEY",
                 "registers whose POINT is non-zero, advance its envelope one",
                 "tick, then write CURRENT to the register ($2C00+X)."],
        0x7789: ["Per register: idle if POINT is 0.  MCOUNT bit 7 means a sound was",
                 "just requested - go start it.  Otherwise count FRAMES down; at 0,",
                 "count COUNT down; at 0 the step is done, else CURRENT += CHNG."],
        0x77A2: "Step done: Y += 4 to the next STB group.",
        0x77A6: ["Load a step: POINT = Y, COUNT = TOT (SOUND+3,Y), CURRENT = START",
                 "(SOUND+1,Y)."],
        0x77B0: ["Store the value, reload FRAMES = FCNT (SOUND,Y).  FCNT 0 is the",
                 "group terminator: rewind Y to MPNTR and repeat while MCOUNT lasts,",
                 "else step past the 0 to the next macro count (CSOUND_11)."],
        0x77C4: "Macro count 0: the sound is over.  Y = 0 goes into POINT and CURRENT.",
        0x77C5: "Free the register (POINT = 0).",
        0x77C7: "Silence it (CURRENT = 0), then write it out.",
        0x77C9: "Write CURRENT to POKEY register X; next register; exit after 0.",
        0x77D2: "Start, or continue with the next macro: the count at POINT; 0 ends.",
        0x77D7: "MCOUNT = the count, MPNTR = the first step (for repeats), load it.",
        0x760D: ["PNTRS: 12 sounds x 8 bytes, one per POKEY register ($2C00+n; even",
                 "n is a channel's frequency AUDF, odd its volume/distortion AUDC).",
                 "Each byte is that register's envelope offset from SOUND; 0 leaves",
                 "the register alone.  The sound number the game loads into Y is",
                 "the entry's offset + 7 (SND.T1 = $07, SND.T2 = $0F, ...).  Decoded",
                 "row by row (sndtab.py)."],
        0x766D: ["The envelopes.  A sequence is a macro count (1-127; 127 repeats",
                 "for as long as the sound is allowed to run) then 4-byte steps",
                 "FCNT,START,CHNG,TOT - every FCNT NMIs add CHNG, TOT times, from",
                 "START - a 0 ending the repeated group, and 0,0 ending the",
                 "sequence (the source's REP / STB / SEND / MEND).  CSOUND reads",
                 "SOUND..SOUND+3,Y.  Decoded row by row (sndtab.py); every step",
                 "checked against DASOUN.MAC."],
        # ---- one-liners for routines whose source has no comment block --
        0x4800: "Split the tips off a special-rock cluster on collision; frees the linked objects",
        0x48A7: "Find a free special-rock control entry, 6 down to 0; $FF if none",
        0x48B2: "Award points for a hit, but not when the saucer made it",
        0x48BE: "Steer special rock X toward the ship",
        0x49A4: "Play the diamond sound when the last special rock of a cluster goes",
        0x49DF: "Wrap a 16-bit position difference the short way round the screen",
        0x49F5: "RANGE with the difficulty hook: is object X within range?",
        0x4A1A: "16-bit position difference, object Y minus object X",
        0x4A2A: "Normalise two differences, then take the angle between them",
        0x4A6D: "Draw the special rock's picture",
        0x4A96: "Set up a special-rock cluster",
        0x4B3D: "Add a random +/-6 to each axis",
        0x4B92: "Rewrite one rock subroutine in vector RAM (the rocks' rotation)",
        0x4CE5: "Shared return",
        0x4CEF: "Blink the two-coin-minimum message",
        0x4CFE: "Game in progress: handle game over and the player change",
        0x6000: "Cold start of the game program: INIT, then the frame loop",
        0x6011: "Start a new wave of asteroids, then fall into the frame loop",
        0x6014: "The frame loop: wait for the DVG, then run one frame of the game",
        0x6102: "Display the PLAYER n message",
        0x610E: "Collision detection for every object",
        0x61FA: "Add the ship's collision size to A",
        0x6203: "Add the saucer's collision size to A",
        0x6279: "CPYVEC's loop: copy one vector instruction, rotating it via the sign bits",
        0x62C6: "Destruction after a collision; decides whether to bounce instead",
        0x62FC: "Explosion size for object Y: small, medium or large",
        0x6325: "Set the saucer delay and the ship's back-to-life timer",
        0x6347: "The saucer: launch it, or let it shoot",
        0x6399: "Launch a saucer from one side at a random height",
        0x63E6: "Enemy fire control: the saucer's aim and torpedo",
        0x64BE: "The fire button, then into FIRE1",
        0x64E4: "Find a free torpedo slot for a ship or saucer launch",
        0x64EE: "Shared return",
        0x64EF: "Launch a torpedo into slot Y",
        0x6587: "Get the player's initials for a high score",
        0x6675: "The shield button: drain the shield while it is up",
        0x669C: "Shared return",
        0x669D: "Draw the shield around the ship",
        0x66CF: "Soft reset: clear both player pages",
        0x6713: "MOTION's loop: move every object by its velocity",
        0x6826: "Reset the saucer: reload its delay, silence its sound",
        0x6835: "Park the saucer off screen",
        0x6846: "Shared return",
        0x6847: "Rotate and thrust the ship, or bring it back to life",
        0x6882: "Thrust or drag, every other frame",
        0x68ED: "Thrust acceleration for one axis",
        0x6921: "Clamp a 16-bit velocity to the maximum",
        0x698D: "Shared return",
        0x698E: "Start up new asteroids for a wave",
        0x6A03: "Place a random large rock at a screen edge",
        0x6A37: "Put the ship at the centre, still, with full shields",
        0x6A57: "New random velocity from the old one",
        0x6A7E: "Range-limit a velocity",
        0x6A99: "Shared return",
        0x6A9A: "Display the score, lives and messages for this frame",
        0x6BA5: "Ramp the returning ship's intensity up (the back-to-life twinkle)",
        0x6C43: "Position the beam, then wait for it",
        0x6CB1: "Select the current player's RAM page",
        0x6D5E: "Display all three initials: INITAL for each",
        0x6D6C: "Find a free rock slot, from X down; $FF if none",
        0x6D6E: "SEARCH's loop entry",
        0x6D77: "The ship's explosion: move and draw its fragments",
        0x6E3C: "Draw the ship, and its flame when thrusting",
        0x6EA5: "Is the thrust button on?",
        0x6EB2: "Advance the source pointer by Y+1, then CPYVEC",
        0x6EC1: "Point R0/R1 at the ship hull for the current ANGLE",
        0x6EE1: "Shared return",
        0x6EE2: "Object orientation index from the angle",
        0x6F12: "Decide which sounds play this frame",
        0x6FF0: "Update the high-score table with a finished game's score",
        0x70E0: "ATAN, right half-plane",
        0x70F2: "ATAN, first quadrant",
        0x7107: "ATAN, first octant: table lookup on the quotient",
        0x7121: "5-bit unsigned quotient of A / TEMP2+1",
        0x7141: "SIN for 0..180 degrees via the quarter-wave table",
        0x714E: "Rotate the four rock shapes and build the copyright display list",
        0x717E: "The copyright line on the high-score screen",
        0x752C: "Does the shield take this collision? Bounce if so",
        0x75AC: "Bounce the ship off rock Y on one axis",
        0x7752: "Silence sound Y",
        0x7758: "Start sound Y, but only on idle channels",
        0x775B: "Start sound Y, taking its channels over",
        0x775C: "SNDON/SNDPON's shared body; C = priority",
        0x7787: "Step all eight sound channels; once per NMI",
        0x77F5: "The copyright check: compare the display list against the expected bytes",
        0x7835: "Fold one expected byte into the copyright error flag",
        0x785C: "The interrupt, every 4 ms: switches, coins, sound, POKEY",
        0x78AB: "Debounce one coin mech",
        0x7977: "Pulse the coin counters",
        0x79A3: "Drive the coin outputs and return from the interrupt",
        0x79E8: "Append RTSL to the display list",
        0x79EC: "Append HALT to the display list",
        0x79EE: "Store the word in A twice and advance (VGHALT/VGRTSL tail)",
        0x79F7: "Append a hex digit with leading-zero suppression",
        0x79FD: "Append one hex digit",
        0x7A02: "VGHEX/VGHEXZ tail: append glyph A",
        0x7A19: "Append a JMPL to the display list (unused by the shipped ROM)",
        0x7A1E: "Store the JSRL/JMPL word and advance",
        0x7A2A: "Append a JSRL to a shape",
        0x7A31: "Set an absolute beam position, then VGLABS",
        0x7A4A: "Append a LABS from the zero-page position block",
        0x7A67: "Advance VGLIST by Y+1",
        0x7A71: "Shared return",
        0x7A72: "The general vector emitter (unused by the shipped ROM)",
        0x7AF7: "Position the beam, then a WAIT",
        0x7AFC: "A WAIT with no intensity",
        0x7AFE: "A dot: a WAIT with intensity X",
        0x7B11: "One step of the EAROM state machine",
        0x7BD9: "Start an EAROM erase-then-write",
        0x7BF5: "Initialise for a new game",
        0x7C37: "The part of INIT the self-test shares: rock velocities, POKEY test",
        0x7C74: "Draw Y-1 ship glyphs at quarter size (the reserve ships)",
        0x7CC5: "A digit followed by a space",
        0x7CCC: "Store A,X as a word and advance two",
        0x7CD7: "Reset: RAM test, then into the self-test or the game",
        0x7D4C: "RAM test: pages 0 and 1 passed, test pages 2 and 3",
        0x7D82: "RAM test: error in the first 1K",
        0x7D86: "RAM test: error in vector RAM",
        0x7D94: "Bad RAM: beep out the failing page on the 3 kHz clock",
        0x7DE9: "Spin until the self-test switch is pushed",
        0x7DF3: "Checksum the ROMs and test the bank select",
        0x7E39: "Self-test display loop: one 16 ms pass",
        0x7E5F: "Self-test: read the switch, draw the option switches and checksum results",
        0x7F70: "Self-test: clear the POKEY, read the player switches, drive the sound test",
        0x7F94: "Read all the switches for the self-test display",
        0x7FC1: "POKEY test, and the rock-speed initialiser",
    },
    "astdelux": {},
    "astdelux1": {},
}


def for_set(table, romset):
    """Look up a per-revision table, defaulting to empty for unknown sets."""
    return table.get(romset, type(next(iter(table.values())))())
