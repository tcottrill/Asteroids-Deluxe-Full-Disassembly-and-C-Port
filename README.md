# Asteroids Deluxe — disassembly and C port

An **AI-assisted disassembly of Asteroids Deluxe** (Atari, 1981) and a
complete, playable **1:1 C port** of it — no emulation, no 6502 core at
runtime; the original program itself, translated one routine at a time.
The reverse engineering and the port were done together with an AI
assistant (Anthropic's Claude), with one rule enforced throughout:
**no invented data**. Every constant, table and branch traces to a ROM
address, every name traces to Atari's own source, and every behavioural
question was settled by running the ROM, not by guessing.

What sets this one apart from a disassembly worked out from scratch:
Atari's source archive for this exact build survives, so **every
subroutine and variable in the listings carries the name and comment its
author gave it**, placed on the ROM by matching instruction sequences.
Nothing was renamed to taste.

## What's in the repository

| where | what |
|-------|------|
| [`disasm/`](disasm/README.md) | the disassembly: a plain three-file assembler source (program ROM, vector ROM, and the symbol/memory-map file they include) that assembles back to the ROM byte for byte; the traced, byte-annotated listings that were the working form; the DVG vector-ROM listing with every shape named; the tools that generate all of it; and the subsystem notes |
| [`c_src/`](c_src/README.md) | the C port: the whole 6502 program as C11, a real DVG display list built in modelled vector RAM, a real POKEY and a real ER2055 EAROM behind the hardware seam, a Windows host (OpenGL beam renderer, XAudio2 sound, keyboard/joystick, option switches, persistent high scores, cabinet self-test) and a headless host for the regression probes. Builds with VS2022, no external SDK |
| `c_src/samples/astdelux.zip` | the explosion and thrust circuits as samples recorded from a real cabinet (the port plays samples for the two discrete circuits and synthesises everything else from the POKEY's registers) |

Each of the two main directories has its own README with the detail.

Not included, but referred to: **Atari's own source archive** for this
game — the `.MAC` assembler sources, the `DSTRD0.MAP` linker map and
`DASTRO.DOC`, published at
[historicalsource/asteroids-deluxe](https://github.com/historicalsource/asteroids-deluxe).
Every name and comment in the listings was recovered from it, and the
recovered tables are checked in, so nothing here needs it to build or
regenerate. The three tools that read it directly (`mkdefines.py`,
`nameroutines.py`, `vecnames.py`) expect a copy at
`asteroids-deluxe-main/` beside `disasm/` and are skipped when it is
absent.

## No ROMs included — bring your own

Atari's ROM images are not distributed here. The C port **builds and
runs without them** (the ROM-derived data it needs is checked in as
generated C files), but the disassembly tools and regenerating those
files read real ROM images.

Given a MAME `astdelux2` ROM set (rev 2; see below for why), one script
rebuilds everything ROM-derived — the working dump, every listing, the C
memory model and the C ROM data:

```bash
cd disasm
python gen_from_roms.py <path-to-your-rom-directory> --listings
```

ROMs are matched by part number and SHA-1, not by file name, so a MAME
zip or a loose directory both work. Regenerating reproduces the
checked-in files byte for byte.

## Quick start

```bat
cd c_src
build_win_gl.bat
astdelux_win.exe
```

Keys: Left/Right rotate, Up or Alt thrust, Ctrl fire, Space (or Shift,
or Down) shield, 5 coin, 1 start, F2 cabinet self-test, ALT+ENTER
fullscreen, Esc quit — MAME's defaults for this game. Both option-switch
banks are set by name in `astdelux_win.ini` (`[dips]`), the way the
cabinet manual lists them. See [`c_src/README.md`](c_src/README.md) for
the rest, including the headless host and the per-module probes.

## The frame rate is 62.5 Hz, and what that means on a 60 Hz monitor

The board has no vertical blank. Its only clock is a 4 ms NMI, and the
main loop runs one frame every fourth interrupt — **62.5 frames per
second**, which is what the port paces to by default (`[main]
frame_hz=62.5`). On a 60 Hz monitor that rate beats against the
display: two and a half frames a second have nowhere to go, so the
picture hitches on a regular cycle, and vsync can only trade the hitch
for tearing.

The fix the port offers is the one a slower crystal would give the real
board: `[main] frame_hz=60` stretches the NMI period so **the whole
machine runs 4% slow** — game logic, rock speeds, POKEY pitch and all —
and every frame then lands on its own refresh. Pair it with `vsync=1`
for a picture that sits still. Nothing else changes; the audio stream
stays real-time because only the block size follows the period. If you
want the board's exact speed, keep 62.5 and accept the beat, or use a
display that can refresh at 62.5 Hz.

## Rev 2, exactly, and rev 3 as a switch

The port is **rev 2** (MAME `astdelux2`), not MAME's primary rev 3 set,
because rev 2 is the build Atari's source archive documents — proved,
not assumed: the linker map puts NMI at `$785C` and PWRON at `$7CD7`,
and the vectors in ROM `036433-02` read exactly that. Porting rev 2
means every routine arrives with its author's name and comment. Rev 2 is
reproduced bugs included; the one that matters is the saucer's aim,
where a typo in Atari's source (`CMP 6` for `CMP I,6`) makes the saucer
"get mad" far earlier than the 60,000 points the comment intends. Every
rev 1 and rev 2 board has it.

Rev 3's behavioural changes are layered on behind `[game] revision=3`,
at the exact points where the listings diverge, starting with the
saucer's aim. The rest of the 40 delta regions follow the same pattern.

## The POKEY is a real chip

`c_src/pokey.c` is a POKEY, not a sound driver: the real chip's
maximal-length polynomial counters, a RANDOM register that is
cycle-exact given a cycle feed from the host, SKCTL reset semantics,
and a pot scanner that answers ALLPOT the way the silicon does — which
this board relies on, because the coin option switches sit on the pot
pins. It has no platform includes and is written to be shared between
ports; the Space Duel port uses the same file unchanged.

## Provenance and method

Trace the ROM from its hardware vectors with a control-flow-following
disassembler, so that what is code and what is data is derived rather
than assumed (the vector-board ROM holds both, interleaved). Recover the
names by matching the source archive's instruction sequences to the
listing, and only keep a name where the match is unique. Translate one
routine at a time under the rules in
[`c_src/CONVENTIONS.md`](c_src/CONVENTIONS.md), against a generated
memory model whose every field is pinned to its 6502 address by a
static assertion. Verify each module with a probe whose expectations
are derived from the listing, not from running the translation.

## License

The disassembly, the tools and the C port are released under the
**GNU General Public License, version 2 or later**, the same terms as
MAME (see `LICENSE`). Two files are translations of MAME sources that
carry MAME's BSD-3-Clause terms, and keep that attribution in their
headers: `c_src/pokey.c` (the POKEY's polynomial counters and reset
model, from `pokey.cpp`) and `c_src/er2055.c` (from `er2055.cpp`). The
names and comments recovered from Atari's source archive remain Atari's;
the archive itself is not distributed here. Asteroids Deluxe is a
trademark of its owner, and no ROM images are included.
