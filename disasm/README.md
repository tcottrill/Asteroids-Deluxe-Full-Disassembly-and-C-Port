# Asteroids Deluxe — disassembly

A traced, annotated disassembly of **Asteroids Deluxe** (Atari, 1981),
covering the 6502 program and the DVG vector ROM, plus the tooling that
generates it. Modeled on the
[Omega Race decompilation](https://github.com/tcottrill/Omega-Race-Full-Disassembly-and-C-Port).

The target is **rev 2** (MAME `astdelux2`) — the revision Atari's own
source archive documents, so the listings carry the original names rather
than invented ones. See [Revisions](#revisions).

Everything here is generated. The listings are outputs, not sources —
edit the tools and the config, then regenerate.

The disassembly comes in two forms. The **plain form** is three
assembler source files in the layout of Nick Mikstas's Asteroids
disassembly — `astdelux2_program_rom.asm`, `astdelux2_vector_rom.asm`,
and `astdelux_defines.asm`, which both include — with an address label
on every line and no byte columns, and it assembles back to the ROM
byte for byte (`python mkplain.py --check`, with ca65). The **traced
form** (`astdelux2_main.asm`, `astdelux2_vecrom.asm`) is the working
listing: bytes, cross-references, the evidence for each data decision.
Both carry the same names and the same comments.

**Every subroutine and variable name is Atari's own**, taken from the
released rev 2 source archive and placed on the ROM by matching
instruction sequences; nothing was named to taste. Comments to the
right of instructions and the blocks above routines are Atari's too.
Our own additions are marked: `[note]` lines under routines, local
labels and tables where the source's comments are thin or absent
(`ROUTINE_NOTES` in `astdelux_config.py`), and the packed message text
decoded beside its bytes. A name like `ASTMSG_20` is the source's local
label `20$` inside `ASTMSG`; both listing headers explain the naming.

## The hardware, briefly

- **CPU:** 6502 at 1.512 MHz. One periodic **NMI** (≈4 ms per the source
  header; MAME's driver runs 4 per frame).
- **Display:** Atari Digital Vector Generator driven by a state PROM
  (034602-01). The CPU builds a display list in vector RAM at `$4000` and
  kicks the DVG by writing `GOADD` (`$3000`); `HALT` (`$2002`) bit 7
  reports busy.
- **Sound:** POKEY at `$2C00` plus discrete explosion/thrust circuits
  (`EXPSND $3600`, `SPTEN $3C03`).
- **Persistence:** EAROM (`EAIN $2C40`, `EADAL $3200`, `EACTL $3A00`) for
  high scores and bookkeeping.
- **Two-player state:** RAM pages 2 and 3 are a bank-switched pair,
  swapped by `BNKSEL` (`$3C04`) bit 7, so the same addresses always
  address whichever player is up.

## Memory map

Straight from the `DSTRD0.MAC` header:

| range | contents |
|---|---|
| `0000-00FF` | page 0 — scratch and globals |
| `0100-01FF` | page 1 — 6502 stack (shares the page with the `SXP*` cells) |
| `0200-02FF` | page 2 — player 1 object state ⎫ swapped by `BNKSEL` |
| `0300-03FF` | page 3 — player 2 object state ⎭ |
| `2000-3FFF` | I/O |
| `4000-47FF` | vector RAM (2K) — the display list the DVG reads |
| `4800-57FF` | vector board ROM (4K) — **6502 code *and* DVG data** |
| `6000-7FFF` | program ROM (8K) |
| `FFFA-FFFF` | vectors, mirrored from `7FFA-7FFF` |

### The one thing worth knowing before you read anything else

`$4800-$57FF` is **not** all vector data, despite Atari calling it the
VECTOR ROM. `TRIROT.MAC` is `.ASECT` / `.=4800`, so the vector board's
ROM carries 6502 code — the rock/saucer rotation and scaling math —
interleaved with the DVG picture data. Roughly a third of it is code.

Nothing here assumes a fixed boundary between the two. The 6502 trace
establishes what is code; whatever it does not reach is handed to the DVG
disassembler. The split between the two listings is *derived*.

## Listings (generated — do not hand-edit)

| file | contents |
|---|---|
| `astdelux2_program_rom.asm` | **plain form** — the program ROM `$6000-$7FFF` as assembler source: `.include`s the defines, `.org $6000`, forward declarations for the vector-ROM labels it calls, an address label on every line |
| `astdelux2_vector_rom.asm` | **plain form** — the vector-board ROM `$4800-$57FF`: its 6502 code as instructions, its DVG display lists as `.word` lines with the decoded vector opcode in the comment |
| `astdelux2_main.asm` | **traced form** — the 6502 program, both ROM regions: bytes, cross-references, every reached routine labelled, unreached bytes emitted as data with an ASCII gutter, `[note]` lines where the source's comments are thin, message text and sound envelopes decoded beside their bytes, and every DVG shape in the vector-board ROM labelled with its JSRL word and a pointer to the vector listing |
| `astdelux2_vecrom.asm` | **traced form** — the DVG display lists in `$4D80-$57FF`, decoded as vector opcodes, one named block per shape |
| `astdelux2_shapes.html` | every shape drawn — the visual index for reviewing the vector ROM |
| `astdelux_defines.asm` | the memory map as a standalone glossary: every hardware register, RAM cell and ROM entry point with its original Atari name |
| `astdelux2_codemap.json` | machine-readable code/data map — how `dvgdasm.py` knows which bytes are 6502 code |
| `FUNCTIONS.md` | index of every named location, by module and by the source's own `.SBTTL` sections |

Listing filenames carry the romset, so other revisions can be generated
alongside without overwriting anything.

## Tools

| file | role |
|---|---|
| `gen_from_roms.py` | assembles `<set>_dump.bin`, a 64K image of the CPU address space, from a ROM directory or MAME romset zips. Matches ROMs by part number and SHA-1, not filename. `--listings` runs the whole chain. |
| `mkdefines.py` | turns the original Atari source archive into symbol tables (`astdelux_defines.py` / `.asm`) |
| `m6502.py` | 6502 instruction table and decoder, undocumented opcodes flagged |
| `m6502trace.py` | the tracing disassembler. `--explore` reports whether an unreached linker-map symbol looks like code. |
| `dvgdasm.py` | DVG display-list disassembler |
| `mkplain.py` | writes the plain-form source files from the trace, the names and the DVG decoder; `--check` assembles them with ca65 and compares against the ROM dump |
| `msgtext.py` | decodes `DSTMSG.MAC`'s packed message text (three characters per two bytes) the way `ASTMSG` reads it, so both listings label every message and print its string; verified against the source's own `ASCIN` lines, all 53 |
| `sndtab.py` | decodes `DASOUN.MAC`'s sound tables the way `CSOUND` plays them — the `PNTRS` entry per sound (which POKEY register plays which envelope) and every envelope step as "every N NMIs add C, T times, from S" — so both listings print the sequencer's data row by row; every step checked against the source's `STB` lines, and the decode is proved to cover the block with no byte left over |
| `nameroutines.py` | recovers routine names *and the original comments* by matching the Atari source's opcode sequences against the listing |
| `crosscheck.py` | validates the ROM image and the 6502 decoder against an independently produced listing |
| `vecnames.py` | recovers DVG shape names by aligning the vector source with the ROM |
| `mkpreview.py` | draws every shape as SVG into `<set>_shapes.html` |
| `mknotes.py` | generates `FUNCTIONS.md` from the listing |
| `mkstate.py` | generates the C port's memory model, `c_src/astdelux_state.h`, from the RAM layout |
| `mkrom.py` | exports the ROM images as C data, `c_src/astdelux_rom.c`, so the port reads tables at their ROM addresses |
| `vramview.py` | renders a vector-RAM dump from the C port's host (`astdelux_test.exe N --dump f.bin`) as the DVG would draw it, with `--trace` in the vector listing's columns |
| `astdelux_config.py` | hand-maintained findings: data regions, extra entry points, jump tables, hand-placed names, the message tables — each with the evidence that settled it — and `ROUTINE_NOTES`, our own descriptions of routines, local labels and tables where the source's comments are thin, printed as `[note]` lines |

## Regenerating

ROMs are not in the repository. Supply a directory of MAME romset zips
(or unpacked ROMs) and run:

```bash
python gen_from_roms.py ../roms --listings
```

Atari's source archive is not in the repository either. The three steps
that read it (`mkdefines.py`, `nameroutines.py`, `vecnames.py`) are
skipped when `../asteroids-deluxe-main/` is absent and the chain uses
the committed tables they produced (`astdelux_defines.py`/`.asm`,
`astdelux_names.py`, `astdelux_vecnames.py`); put a copy of the archive
there to run them again. `mkplain.py --check` needs ca65 and ld65 from
[cc65](https://cc65.github.io/) on PATH, or `--cc65 <bindir>`.

## Notes (the findings)

| file | contents |
|---|---|
| [`FUNCTIONS.md`](FUNCTIONS.md) | every named location, by subsystem — generated, so it cannot drift from the listing |
| [`MAINLINE_NOTES.md`](MAINLINE_NOTES.md) | the frame loop, double-buffered display lists, frame sync, the rolling checksum |
| [`BOOT_AND_NMI_NOTES.md`](BOOT_AND_NMI_NOTES.md) | reset and the RAM test, the NMI body, the included coin module, the EAROM |
| [`GAMEPLAY_NOTES.md`](GAMEPLAY_NOTES.md) | the parallel-array object model, the bank-switched player pages, collision, the maths library |
| [`VECTOR_NOTES.md`](VECTOR_NOTES.md) | DVG encoding, how a display list is built, glyphs and text |
| [`SOUND_NOTES.md`](SOUND_NOTES.md) | the eight-channel sequencer, the `STB` envelope format, the sound set |
| [`CALLING_NOTES.md`](CALLING_NOTES.md) | entry/exit contract of every routine the C port calls across a module: registers, zero-page temporaries, flag results, callers, and the constructs that bite a translation |

## Method

The listing is not a linear dump. `m6502trace.py` walks control flow from
the RESET/NMI/IRQ vectors, so unreached bytes render as data instead of
as invented instructions. When the walk stops somewhere unexpected it
says so, and the fix goes into `astdelux_config.py` with the reason. That
loop is how coverage closes; it currently sits at **67.2%** of the 12K of
ROM being reachable code, with no unresolved indirect jumps and no
unexplained stops.

Naming is closed. **660 names: 646 recovered from the source** by
signature matching, on top of the 92 the linker map gave, **plus 14
placed by hand** with their evidence in `astdelux_config.py`
(`HAND_NAMES`). No `SUB_xxxx` and no `Lxxxx` remain. Of the 14, ten are
real source labels the automatic placement could not reach (a bare
`RTS.0` that matches everywhere; `INISOU`, linked away from the rest of
its module; `$CNVRT` and its locals; the `TABLE` a macro builds;
`ROMX`/`ROMY`), and four are spots the source never labelled — `BNE .`
self-loops and the vector table — which are *named*, not recovered, and
say so in the listing (`placed by hand: ...`).

The data tables are named too: **every data byte in both ROMs**,
including the 787-byte message block (`VGMSGS`, `VGMSGT`, language
tables `L0`–`L3`).

The listing also carries **2,153 of the original Atari comments**, on the
instructions they were written against, plus 73 of the source's own
`.SBTTL` section titles and 26 routine header blocks — the ones that
document calling conventions (`ENTRY (X)=NEW ROCK INDEX`, `EXIT
(A)=PRODUCT (SIGNED)`). Every one of the 618 code stretches now aligns
one instruction to one instruction.

### Coverage is closed

The 33% of the ROM that is not code is not a backlog of undisassembled
routines — it is data, and that has been checked two independent ways:

- **Against the source.** The trace decodes **4,083** instructions; the
  Atari source contains **4,085**. The two unaccounted for are
  `DSFILL.MAC`'s `ADC #15 / RTS` at `$77F2`, the decoy filler that exists
  so the ROM has no obviously empty space and which nothing ever
  executes. It is declared as data deliberately. Every other instruction
  in the source is in the listing.
- **Without the source.** Decoding each of the 22 data runs forward as
  6502 finds an illegal opcode or a `brk` within a few instructions in
  every one. None decodes as a plausible routine.

Control flow has nowhere left to hide either: the ROM contains **no
`jmp ($xxxx)`** at all and no `pha/pha/rts` dispatch, so there is no
computed transfer the walk could have failed to follow.

Claims here were checked rather than assumed:

- **The symbol names are the originals, not invented.** They come from
  Atari's source archive ([historicalsource/asteroids-deluxe](https://github.com/historicalsource/asteroids-deluxe),
  not included here; the naming tools read a copy at
  `../asteroids-deluxe-main/` when one is present): hardware
  equates from `DSTDEC.MAC` and `EAROM.MAC`, the RAM layout from
  `PG0123.MAC`, ROM entry points from the `DSTRD0.MAP` linker map.
- **The RAM layout is verified, not trusted.** `PG0123.MAC` declares
  pages 0–3 as a run of sequential `.BLKB` directives; `mkdefines.py`
  walks them (expanding the `.IRPC` block that declares `R0`–`R9`, which
  is worth ten bytes) and checks the result against the 24 addresses the
  linker map states independently. All 24 agree, and the page-zero
  allocation lands on exactly `$0100`, which is what the source's own
  `.IIF GT,.-^H100,.ERROR` asserts.
- **The DVG decoder was derived from the macros, then round-tripped.**
  Encodings come from `DSTVEC.MAC` rather than a datasheet. Decoding
  `SAUCER` reproduces `DASVEC.MAC`'s twelve `VCTR` lines exactly — every
  `dx`, `dy` and intensity, including `VCTR -80,0,.BRITR+1`, which is
  stored as `dx=-640` at `s=6` and shifts back by 8. `ERASE` reads back
  as the string `ERASIN`, matching its `ALPHA <ERASIN>` source line.
- **The trace was validated against the linker map.** Of the 65 map
  symbols that are code, the walk independently found 64 as call or
  branch targets, having been told only the three hardware vectors.
- **Routine names come from the source, and are checked two ways.** The
  linker map only exports 92 `.GLOBL` symbols, so most routines came out
  as `SUB_xxxx`. `nameroutines.py` closes that by matching each source
  label's *opcode sequence* against the listing - operands are ignored,
  so no MAC65 assembler is needed. Placement widens in three passes that
  feed each other: unique whole-ROM match, then interpolation between
  placed neighbours, then harvesting labels out of stretches that have
  already been proved to align instruction-for-instruction. Two
  independent checks then have to pass, both as hard failures:
  the 92 map addresses are re-derived and must not be contradicted, and
  within every contiguous stretch of every module the matched addresses
  must strictly increase, since an assembler emits labels in order. That
  ordering check is what forced modelling `ENTSEC`/`XITSEC` (DSTRD0.MAC
  parks the location counter in the space left after TRIROT and returns)
  and the mid-file `.CSECT` switches - without those the check failed,
  which is exactly what it is for. It is also used as a *constraint* and
  not only a test: where a handful of placements contradict it the
  longest consistent run is kept and the rest dropped, because a generic
  signature can genuinely match twice (`RTS.0` is a bare `rts`).
- **A silent parser bug was caught by that check, not by inspection.**
  MACRO-11 has `.IFT` / `.IFF` / `.IFTF` inside one `.IF` block, and each
  *sets* the assembly state rather than toggling it. Treating `.IFF` as a
  flip meant two `.IFF` sections in one block turned assembly back off,
  silently swallowing ~90 lines of `TRIROT.MAC` - `RELTIP` and `RNDXYI`
  among them. Nothing looked wrong until the ordering invariant failed.
- **Every DVG shape is named, by aligning the vector source with the
  ROM.** The linker map names eight; the rest come from `DASVEC.MAC` and
  its includes. Each source directive is exactly one DVG instruction
  whatever its encoded length, so the two streams walk in lockstep from a
  known address without needing the macros implemented. A stretch counts
  only when source and ROM agree on the kind of every instruction in it.
  `TRI00` checks out against its source line for line, including
  `PVCTR`'s absolute-to-delta conversion.
- **Data tables are located by their contents, not by position.**
  Tables have no opcodes, so the signature machinery cannot see them, and
  anchoring them to the routine that follows only works when source order
  and address order agree — the linker interleaves sections, so `ATANA`
  is followed in the source by a `.CSECT` switch rather than by the code
  that follows it in memory. Instead `.BYTE`/`.WORD`/`.BLKB` are sized
  and *evaluated*: a run assembles to a known byte pattern, with
  wildcards where a value depends on an address (`.BYTE 10$-L0`), and
  that pattern is searched for in the listing's data regions. A run is
  used only when it has at least four concrete bytes and matches in
  exactly one place. The checksum bytes are wildcards by necessity —
  `CKSUM.MAC` links last and patches them, so what the source writes
  there is never what the ROM holds.
- **The comments sit on the bytes they were written about.** Two placed
  labels bracket a stretch of code; a comment is carried across only when
  the source's instructions and the listing's instructions inside that
  bracket correspond one to one - same count, same opcodes, in order.
  That now holds for all 618 stretches. Any that did not would be
  skipped whole rather than sliding comments onto the wrong lines, and
  `nameroutines.py --report` lists them.
- **Getting the last few names meant parsing MACRO-11 properly.** Four
  more silent errors surfaced, each caught by an invariant rather than by
  reading the output: `.INCLUDE` is textual insertion, so parsing
  `DCIN65.MAC` as its own module left a 138-instruction hole inside the
  NMI handler; `.IF` conditions are *expressions* (`.IF EQ,MECHS-1`), and
  defaulting the undecidable ones to true took the wrong branch and lost
  `MOOLAH`'s leading `LDX`; macro instruction counts done by hand were
  wrong for `GDM` (4, not 3) and `GBAM` (6, not 3), so bodies are now
  expanded for real, conditionals and all; and a macro invoked on a
  *labelled* line (`$BONUS: GBAM`) was not being expanded at all.
- **Where the location counter cannot be evaluated, the answer is
  recorded with its evidence.** Six conditions in the archive test `.`,
  which needs a real assembler. Each is answered in
  `LOCATION_CONDITIONS` with a reason. One was initially wrong: a
  branch-range guard in `DCIN65.MAC` was set to the short form on an
  instruction count that the `GCM`/`GHM` miscount had itself distorted.
  The ROM settles it - `$7935` reads `bmi $BONUS` / `jmp $DETCT`, the
  long form.
- **The image and the decoder were validated against an outside
  disassembly.** An independently produced linear listing of the same
  ROM set (not included; `crosscheck.py` takes its path) was compared
  against ours: all **12,288
  ROM bytes match** the image `gen_from_roms.py` builds, and the decoder
  agrees with it on **6,340 of 6,340 instructions**, with zero
  instruction-length disagreements.

## Revisions

**The source archive documents rev 2** (MAME `astdelux2`), and this is
not a guess: `DSTRD0.MAP` puts NMI at `$785C` and PWRON at `$7CD7`, and
the vectors in `036433-02` read exactly that.

That is why rev 2 is the default. Page zero moved by 7 bytes between rev
2 and rev 3 and the code shifted with it, so the archive's names cannot
be applied to rev 3 — `m6502trace.py` checks `SYMBOL_REVISION` and
refuses, leaving that listing with hardware names only:

| | rev 2 (`astdelux2`, default) | rev 3 (`astdelux`) |
|---|---|---|
| named symbols | ~200, the originals | hardware only |
| DVG shapes named | 53 | 35 (character glyphs only) |
| code coverage | 67.2% | 64.9% |

Rev 3 is MAME's primary set, so it is still worth generating if you need
to match that specific binary:

```bash
python gen_from_roms.py ../roms --set astdelux --listings
```

The shapes from `$4D80` up are byte-identical between the two, so vector
art transfers directly; program-ROM findings need realigning.

## Still open

- Realign the rev 2 symbol names onto rev 3 by structural matching, so
  that listing can get real names too.
- `GAMEPLAY_NOTES.md` stops short of the collision *geometry* and the
  saucer's aiming logic — both are named and commented in the listing but
  have not been read closely enough to write down.
- Shape provenance is still thin: only 7 shapes have a traced 6502
  reference, because most are reached through computed JSRL words rather
  than literal `LXL`/`LAH` pairs. The names are recovered, but *who draws
  what* is largely not.
- Extract the shapes as C data and a visual index, as the Omega Race
  project does with `omega_shapes.c` and `shapes_preview.html`.
