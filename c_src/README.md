# Asteroids Deluxe — C conversion

A faithful C translation of the game logic in
[`../disasm/astdelux2_main.asm`](../disasm/astdelux2_main.asm).

**Every routine is translated, and it plays in a window with sound and
persistent high scores.** The main line, the object engine, the
enemies, scoring and the high-score table, the sound sequencer, the NMI
and coin logic, the EAROM, the display-list builders and the cabinet
self-test are all in, one C function per named routine, with the ROM
address on each. A real POKEY (`pokey.c`) stands behind RANDOM, the
coin option switches and the game's audio, and its output is streamed
to the window's audio device every NMI tick. The two discrete circuits,
explosion and thrust, play as samples recorded from a real cabinet.
Behind `EACTL`/`EADAL`/`EAIN` sits a real ER2055 model (`er2055.c`,
translated from MAME's `er2055.cpp`); the window host persists it in
`astdelux.nv` beside the exe, so high scores survive between runs.

## Building and running

The game, in a window (Win32 + OpenGL 3.3, the same backend as the
Omega Race port):

```bash
build_win_gl.bat
astdelux_win.exe
```

Needs Visual Studio 2022 (the scripts call its developer command
prompt); nothing else.

Keys: Left/Right rotate, Up or Alt thrust, Ctrl fire, Space (or Shift,
or Down) shield, 5 and 6 coin, 1 and 2 start, F2 the cabinet self-test
switch (a toggle, like the real one: press to enter, press again to
leave), ALT+ENTER fullscreen, Esc quit — MAME's defaults for this game.

`astdelux_win.ini` sits beside the exe. The host writes it on the first
run with every key at its default, so there is nothing to set up; the
keys are:

| section | keys |
|---|---|
| `[main]` | `frame_hz` — 62.5 is the board; 60 runs the whole machine 4% slow so it sits still on a 60 Hz monitor (see the root README). `vsync` — 0 or 1 |
| `[game]` | `revision` — 2 (the ROMs the port is built from) or 3 (rev 3's behavioural differences switched on where they occur) |
| `[dips]` | both option-switch banks by the manual's names: `language`, `lives`, `min_plays`, `difficulty`, `bonus_life`, `coinage`, `right_coin`, `center_coin`, `bonus_coins`. Defaults are the factory settings. An unknown word falls back to the default and the accepted spelling is written back |
| `[vector]` | the beam renderer: `linewidth` (pixels at the default 1024-wide window, scaling with the picture) and `line_smoothing` (anti-alias feather in physical pixels); `corner_strength` and `endcap_strength` (join and cap disc radii as a fraction of the half-width); `gain`; `fire_point_size`; `phosphor` (0/1) and `phosphor_ms` (the afterglow's decay time) |
| `[joystick]` | `deadzone` |

The sample set, `samples\astdelux.zip`, must sit beside the exe; the
checked-in copy already does, since the build leaves `astdelux_win.exe`
in `c_src`. A missing `astdelux.nv` just leaves the high-score table
blank, same as a fresh chip on the real board.

The tests live in `tests/`. The headless harness:

```bash
cd tests
build_test.bat
astdelux_test.exe 300 --start --dump game.bin
python ..\..\disasm\vramview.py game.bin --rom ..\..\disasm\astdelux2_dump.bin -o game.html
```

`astdelux_test.exe` runs the same game against quiet hardware
(`tests/host_stub.c`): the translated NMI four times per frame, a coin
and START on `--start`, and `--fire --thrust --rotl --shield` held from
frame 8. It prints the game state and object table at the end, and
`--dump` writes vector RAM for `vramview.py` to draw. `--test` runs the
cabinet self-test instead of the game; `--rev3` selects rev 3's
behaviour; `--nv <file>` loads and saves an EAROM image.

Each module has a probe with expectations derived from the listing (or,
for the chip models, from the chip's documented behaviour), never from
running the translation:

```bash
cd tests
build_mod.bat objects
test_objects.exe --probe
```

## What is here

| file | |
|---|---|
| `astdelux_state.h` | **generated** — the 6502 memory model, with a `_Static_assert` per field |
| `astdelux_rom.c/.h` | **generated** — both ROM regions and the DVG PROM as data; `ad_rom(addr)` reads a table at its ROM address |
| `astdelux.h` | state, the object-array accessors, the `ad_hw_*` hardware seam |
| `mainline.c` | `START` and the frame loop |
| `vgutil.c` | the `DSTCUT.MAC` display-list builders |
| `frame.c`, `objects.c`, `draw.c`, `player.c`, `enemy.c`, `score.c`, `sound.c`, `nmi.c`, `earom.c`, `message.c`, `mathrom.c`, `stest.c` | the game, one file per subsystem; `CONVENTIONS.md` has the map |
| `pokey.c/.h` | the POKEY sound and RNG chip — hardware the ROM talks to, not a ROM routine; shared unchanged with the Space Duel port |
| `er2055.c/.h` | the ER2055 EAROM behind `EACTL`/`EADAL`/`EAIN`, translated from MAME's `er2055.cpp` |
| `app_win.c` | the Windows host's game loop: NMI pacing, POKEY cycle feed, audio push, input, EAROM persistence |
| `platform/` | `ad_platform.h`, the host interface; `windows/` — the OpenGL beam renderer, XAudio2 mixer, raw input, joystick, ini and logging; `teensy/` — the standalone Teensy 4.1 backend for the Teensy Vector Emulation PCB (DAC7811 X/Y, Z ladder, PT8211 audio), see its README |
| `build_win_gl.bat` | the window build |
| `tests/host_stub.c` | the headless host |
| `tests/probe_*.c` | per-module checks, built with `tests/build_mod.bat <module>` |
| `tests/stubs.c` | empty; the staging hook `build_mod.bat` keeps |
| `tests/build_test.bat`, `tests/build_mod.bat` | the headless build, and one module against the probe harness |
| `CONVENTIONS.md` | **read this before adding code** |

## The memory model is generated, not typed

`disasm/mkstate.py` builds `astdelux_state.h` from the RAM layout in
`PG0123.MAC` — the same table the disassembly uses, cross-checked
against the linker map. Regenerate with:

```bash
cd ../disasm && python mkstate.py
```

Every field carries an assertion pinning its offset to the address the
ROM uses:

```c
_Static_assert(offsetof(ad_player_t, f.OBJ) == 0x00, "OBJ must sit at $0200");
_Static_assert(offsetof(ad_zp_t, f.SYNC)    == 0x75, "SYNC must sit at $0075");
```

If the layout ever drifts, the port stops compiling rather than going
quietly wrong — and it means the ROM's pointer arithmetic can be
translated literally.

## Why the display list is modelled

The port builds real DVG words in `g.vram` instead of calling a draw
API. Two reasons, both in `CONVENTIONS.md` rule 5: the shapes are copied
*and modified* on the way in (`CPYVEC` rotates the ship by flipping
vector sign bits, and supplies the intensity the ship frames are stored
without), and it makes the generated vector RAM directly comparable with
a hardware dump — the strongest test available.

## Revision

Rev 2 (`astdelux2`), exact, bugs included (`CONVENTIONS.md` rule 13).
Rev 3 differs by 231 instructions across 40 named regions — `START2`,
`NEWAST`, `COINC`, `FIRE`, `MOTION`, `UPDATE`, `SETTIP`, `STEST6`,
`COLIDE` — plus 117 instructions of dead code it removed. Each region
is switched on behind `ad_hw_rom_rev()` at the point where the listings
diverge; the saucer's aim in `EFIRE` is done, the rest follow the same
pattern. See [`../disasm/README.md#revisions`](../disasm/README.md#revisions).
