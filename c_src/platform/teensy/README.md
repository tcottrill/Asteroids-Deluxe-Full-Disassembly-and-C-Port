# Teensy 4.1 backend

The C port running standalone on the Teensy 4.1 on the Teensy Vector Emulation
PCB: X/Y out through two DAC7811s, intensity through the 5-bit
resistor ladder, sound through the PT8211, the cabinet's switches and
start lamps through the 44-pin edge connector. No PC, no SD card.

It implements [`../ad_platform.h`](../ad_platform.h), the same contract the
Windows backend implements; the game core is not modified. Design notes
and the reasoning behind every constant: the spec this was built from is
summarised in the comments of `astdelux_teensy/teensy_config.h`.

## Files

| file | |
|---|---|
| `astdelux_teensy/` | the Arduino sketch |
| `astdelux_teensy/teensy_config.h` | **every pin and tunable**; read this first |
| `astdelux_teensy/vec_beam.c/.h` | pure C: coordinate map, DAC7811 word, Z code, step planner |
| `astdelux_teensy/vec_out.cpp/.h` | the DAC7811 pair and Z ladder driver |
| `astdelux_teensy/audio_mix.c/.h` | pure C: audio ring, rate adapter, cabinet-sample mixer |
| `astdelux_teensy/audio_out.cpp/.h` | Audio Library glue to the PT8211 |
| `astdelux_teensy/plat_teensy.cpp/.h` | input, time, EEPROM, lamps, status, the test pattern |
| `astdelux_teensy/src/` | **generated** by `stage.py`, not in git: the game core and the samples as data |
| `stage.py` | copies the core into the sketch; converts `samples/astdelux.zip` to C arrays |
| `build_teensy.bat` | `stage.py` then `arduino-cli compile`; `build_teensy.bat upload` flashes |
| `tests/probe_teensy.c`, `tests/build_probe.bat` | PC checks of the two pure modules |

## Wiring

Pins are the schematic's net labels on the Teensy symbol
(the PCB's KiCad schematic). Switches close to ground and use the
Teensy's pull-ups.

| Teensy | net | |
|---|---|---|
| 0, 1 | SW_ROT_L, SW_ROT_R | rotate |
| 8 | SW_THRUST | |
| 12 | SW_FIRE | SPI0's default MISO; the driver moves MISO to the unused pin 39 |
| 14 | SW_HYPER | shields |
| 15, 16 | SW_START1, SW_START2 | |
| 17, 18 | SW_COIN_L, SW_COIN_C | `coin1`, `coin2` |
| 19, 22, 24 | SW_COIN_R, SW_SLAM, SW_DIAG | read, shown in the status line, not wired to the game |
| 23 | SW_SELFTEST | the cabinet's self-test switch, read as a level |
| 2, 3, 4, 5, 6 | Z_B0..Z_B4 | ladder bits LSB first, through the inverting 74HC04 |
| 9, 10 | CS_Y, CS_X | DAC7811 SYNC |
| 11, 13 | MOSI, SCK | both DACs |
| 7, 20, 21 | I2S_DIN, I2S_LRCLK, I2S_BCLK | PT8211 (the Audio Library's default pins) |
| 25, 26 | T_LED1, T_LED2 | start lamps, HIGH = lit |

## Building

Install [arduino-cli](https://arduino.github.io/arduino-cli/) and the PJRC
Teensy package:

```bash
arduino-cli config init --additional-urls https://www.pjrc.com/teensy/package_teensy_index.json
arduino-cli core update-index
arduino-cli core install teensy:avr
```

Then, from this folder:

```bash
build_teensy.bat
build_teensy.bat upload
```

The hex lands in `astdelux_teensy\build\teensy.avr.teensy41\`. Board
options are Teensy 4.1, 816 MHz (overclock), Fastest, USB type Serial.
Arduino IDE 2 works too: add the same package URL in Boards Manager, run
`python stage.py` once, open `astdelux_teensy\astdelux_teensy.ino`, pick
those options and upload. Re-run `stage.py` whenever the core changes.

The PC probe of the pure modules:

```bash
tests\build_probe.bat
```

## Bring-up

The order from the board's design notes, with what to set here:

1. **Power and Teensy alone.** Flash with `VEC_TEST_PATTERN 1`. The USB
   serial console shows `astdelux_teensy backend up`.
2. **X/Y on a scope in XY mode.** The pattern is the DVG's full 1024
   square, the game's visible window (a pair of horizontal lines), a
   crosshair through the centre, sixteen intensity bars and a dot. Set
   the SIZE/CENTER pots so the outer square is the swing a stock board
   gives and the crosshair sits at 0 V. If the picture is mirrored, set
   `VEC_FLIP_X` / `VEC_FLIP_Y`. If nothing moves, try `VEC_SPI_MODE`
   `SPI_MODE2`.
3. **Z staircase.** The sixteen bars step through the ladder codes in
   `VEC_Z_TABLE`, dim on the left. Scope Z_OUT; every bar should be a
   distinct level and the blanked moves between them at the off level.
4. **Beam speed.** Turn `VEC_DRAW_STEP`, `VEC_MOVE_STEP`, the two dwells,
   `VEC_SETTLE_US` and `VEC_DOT_US` until corners are sharp and moves
   leave no retrace. Smaller steps and longer dwells are brighter and
   slower; the status line's `draw N us` must stay well under 16000 for
   the game to run at speed.
5. **The game.** `VEC_TEST_PATTERN 0`, reflash. Hold SELF TEST at power-up
   for the cabinet self-test. The status line once a second shows fps,
   the draw time, audio ring fill and any over/underruns; a steady fill
   with zero counts is right.
6. **Sound.** `AUDIO_POKEY_GAIN` and `AUDIO_SAMPLE_GAIN` balance the POKEY
   against the explosion and thrust samples; the sum clamps rather than
   wrapping.

High scores live in the Teensy's EEPROM behind a magic word; a fresh
Teensy starts with an empty table, same as a fresh ER2055.

## How it works

`dvg.c` walks the display list the game built and hands every lit segment
to `plat_video_line`, which maps it to DAC codes (DVG 0..1023 shifted onto
the 12-bit DAC, so 512 is mid-scale, exactly the stock board's geometry)
into a per-frame buffer. `plat_video_present` draws the buffer synchronously:
blanked moves in `VEC_MOVE_STEP` counts, lit segments in `VEC_DRAW_STEP`
counts, Z set through the ladder before each lit run and off before each
move and at the end of the frame. While it draws, the game's 4 ms NMI loop
waits; `ad_app_step` then runs the missed ticks (capped at one frame) and
the audio ring absorbs the burst. The beam is blanked and centred at boot
and whenever nothing has been drawn for `VEC_PARK_MS`.

The POKEY renders at 44100 Hz in the core; the Audio Library's I2S clock is
44117.6 Hz. `audio_mix` bridges them with a ring buffer that duplicates or
skips one frame per 128-frame block when its fill drifts out of band, which
is inaudible and never blocks.

RAM1 is tight: the game core's POKEY builds two 128 KB polynomial tables at
runtime, which is most of RAM1's variable space before the backend adds
anything. The backend's own 40 KB video segment buffer and 8 KB audio ring
are placed in RAM2 with `DMAMEM` instead, leaving RAM1 to the POKEY tables,
the rest of the core's variables and the stack.
