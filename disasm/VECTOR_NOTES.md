# The vector generator

Asteroids Deluxe draws with an Atari DVG: a small processor that reads a
display list out of shared RAM and steers the beam. The CPU's job each
frame is to *write a program* for it.

## The address space, and a trap in it

| range | what |
|---|---|
| `$4000-$47FF` | vector RAM — the display list the CPU builds |
| `$4800-$57FF` | vector board ROM |
| `$3000` | `GOADD` — writing here starts the DVG |
| `$2002` | `HALT` — bit 7 set means still drawing |
| `$3800` | `VGRESET` — stop it |

The trap: **`$4800-$57FF` is not all vector data**, despite Atari calling
it the VECTOR ROM. `TRIROT.MAC` is `.ASECT` / `.=4800`, so the vector
board's ROM also carries 6502 code — the rotation and scaling maths — and
`DSTRD0.MAC` parks more code there through its `ENTSEC`/`XITSEC` macros.
About a third of that ROM is instructions.

Nothing in this project assumes a boundary. The 6502 trace establishes
what is code, and whatever it does not reach is handed to `dvgdasm.py`;
the split between the two listings is derived. See
[`README.md`](README.md#the-one-thing-worth-knowing-before-you-read-anything-else).

## Instruction encoding

Taken from the macros in `DSTVEC.MAC`, so the decoder inverts exactly
what the assembler emitted:

| op | form | notes |
|---|---|---|
| `VCTR` | two words | `op<<12 \| dysign<<10 \| \|dy\|`, then `z<<12 \| dxsign<<10 \| \|dx\|` |
| `WAIT` | two words | a `VCTR` with both magnitudes zero: `.WORD T*1000, Z*1000` |
| `SVEC` | one word | short vector; bits 11 and 3 select a unit of 2, 4, 8 or 16 |
| `LABS` | two words | `A000+(y&FFF)`, then `size<<12 \| (x&FFF)` |
| `HALT` | `B000` | |
| `JSRL` | `C000 + addr/2` | |
| `RTSL` | `D000` | |
| `JMPL` | `E000 + addr/2` | |

`JSRL`/`JMPL` mask the address with `1FFF` before halving, which is why a
shape's word address is `(byte - $4000) / 2`. The linker map records
shape entry points in that form — `SAUCER` appears there as `$C734`,
meaning byte `$4E68`.

`VCTR` is the one that needs care. The macro doubles the magnitudes and
counts the opcode down from 9 until they fill the field, so a long vector
carries a *low* opcode and a large stored magnitude. The listing prints
the values already shifted back — what the programmer wrote — and marks
the factor as `[xN]`. `SAUCER`'s `VCTR -80,0,.BRITR+1` is stored as
`dx=-640` at `s=6`, and `640 >> 3 == 80`.

## Building a list

`VGLIST` (`$03/$04`) is the build pointer. Two primitives do all the
appending:

```
VGADD2  ($7CCC)   store A,X as a word at (VGLIST), then fall into VGADD
VGADD   ($7A67)   advance VGLIST by 1+Y
```

Everything else in `DSTCUT` is a wrapper that computes a word and calls
one of those:

| routine | emits |
|---|---|
| `VGVCTR` | a vector, converting 16-bit two's complement to 11-bit sign-magnitude and normalising |
| `VGSABS` / `VGLABS` | a short / long absolute beam position |
| `VGJSRL` / `VGJMPL` | a call / jump to a shape |
| `VGCHAR` | one character, via the `VGMSGA` glyph table |
| `VGHEX` / `VGHEXZ` | a hex digit, with and without zero suppression |
| `VGDOT`, `VGWAIT`, `VGHALT`, `VGRTSL` | a dot, a wait, the terminator, a return |

`VGJSRL` and `VGJMPL` are the same routine twice: each does
`lsr / and #$0F / ora #$C0` (or `#$E0`) and falls into a shared tail at
`$7A1E`. The `lsr` plus the `ror` in the tail is the divide-by-two that
turns a byte address into a DVG word address.

`VGJMPL` and `VGVCTR` are never called in the shipped ROM — nothing
`jsr`s either address. They are in the linker map because the diagnostic
build (`GONOGO`) links against them, and the trace only finds them
because the map says to look.

## How the CPU hands a list over

Two lists alternate on `FRAME` bit 0; the first two bytes of vector RAM
hold a `JMPL` into whichever was finished last. See
[`MAINLINE_NOTES.md`](MAINLINE_NOTES.md#double-buffered-display-list) for
the sequence.

## Text

Text is not a string in a display list — it is a run of `JSRL`s, one per
character, which is what `DSTVEC.MAC`'s `ALPHA` macro expands to. So the
messages can be read straight back out of the decoded vector ROM:
`ERASE` at `$57F2` decodes to `JSRL CHAR_E, CHAR_R, CHAR_A, CHAR_S,
CHAR_I, CHAR_N`, matching its source line `ALPHA <ERASIN>`.

The glyph table is `VGMSGA` (`$56F8`), 37 entries of `JSRL` words in the
order space, `0`–`9`, `A`–`Z` — `VGCHAR` rejects an index of `$4A` or
more, which is exactly 37 words. Two glyphs are shared: `DSVECN.MAC` has
`CHAR.0 = CHAR.O` and `CHAR.5 = CHAR.S`, so the digits `0` and `5` draw
the letters.

## Where the shapes are, and what they are

`DASVEC.MAC` fixes the boundary itself: `DVSTRT = ^H4D80 ;START ADDRESS
OF DASVEC STUFF`. Below that the ROM is TRIROT's — 1,314 bytes of 6502
code plus the 6502's own tables (`AVEL`, `ITXL`/`ITYL`/`ITIME`/`IANG`,
`SINCOS`). Those are data as far as the trace is concerned, so they used
to reach `dvgdasm.py` and get decoded into shapes that do not exist. It
now stops at `DVSTRT` and lists them as not-decoded instead.

All 134 blocks from `$4D80` up are named, via `vecnames.py`:

| family | count | |
|---|---|---|
| `EXPPIC`, `EXP10`–`EXP16` | 5 | explosions — scatters of dots |
| `ROCK0`–`ROCK7` | 8 | the rock shapes |
| `TRI00`–`TRI37` | 32 | the special rock, 32 rotations |
| `FRM00`–`FRM17` | 16 | its frame wrappers |
| `SHIPSV`, `SHIP01`–`SHIP10`, `SHIP17` | 10 | the ship |
| `SAUCER`, `SHLDVC` | 2 | saucer and shield |
| `CHAR_*`, `UNDERL`, `Z.5` | 49 | glyphs |
| messages and self-test patterns | 12 | `ASTMSG`, `TEST1`, `BNKERR`, … |

[`astdelux2_shapes.html`](astdelux2_shapes.html) draws all of them.

**The glyphs share strokes by falling through each other.** `CHAR.E` is
only two vectors of its own — the bottom stroke and a blanked move — and
then runs straight through `CHAR.F`'s label into F's left, top and middle
strokes and the shared tail at `CHARF1`. Several others end in a `JMPL`
to a fragment instead: `C` jumps to `UNDERL` for its bottom stroke, `H`
and `M` to `CHAR72`, `N` to `CHARU1`. So a glyph's extent cannot be read
off the ROM — a label is not a boundary. Only the source says where one
ends, which is why `vecnames.py` exports an instruction count per shape.

Contrast the rocks: `ROCK0` has no terminator either, but `ROCK1` really
is a new picture. What separates them is `SINIT`, the macro that resets
the pen — present at the head of every rock, ship and frame, absent from
a shared glyph tail.

Two more things the preview makes obvious that the listing does not:

**Explosions are dots.** `EXP16` alternates a blanked move with
`VCTR 0,0,.BRITE` — a lit vector of zero length, which the DVG draws as a
point. Each explosion is a scatter of ten; `EXPPIC` calls all four sizes
for forty.

**The ship frames are stored blanked.** Every vector in `SHIPSV` and
`SHIP01`–`SHIP10` has `z=0`. The 6502 ORs the intensity in as it copies
them into the display list — which is what `CPYVEC` means by *"copy and
modify vectors"*, alongside the sign and swap masks it applies to rotate
them. `SHIP17` is the exception and is stored lit.

## Reading further

- [`astdelux2_vecrom.asm`](astdelux2_vecrom.asm) — the decoded display lists
- [`astdelux_defines.asm`](astdelux_defines.asm) — the hardware addresses
- [`FUNCTIONS.md`](FUNCTIONS.md) — the `DSTCUT` section lists every utility
