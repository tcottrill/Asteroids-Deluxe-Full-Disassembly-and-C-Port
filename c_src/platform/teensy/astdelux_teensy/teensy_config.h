/* teensy_config.h - every hardware and tuning constant of the Teensy 4.1
 * backend for the Teensy Vector Emulation PCB (pin numbers are the
 * schematic's net labels on the Teensy symbol).  Plain C: this header is
 * also compiled into the PC
 * probe (tests/probe_teensy.c), so no Arduino names outside #ifdef ARDUINO.
 *
 * Beam and audio values marked TUNE are starting points for bring-up, not
 * measured constants.  The status line the backend prints once a second
 * (frame draw time, audio over/underruns) is what to watch while turning
 * them.
 */
#ifndef TEENSY_CONFIG_H
#define TEENSY_CONFIG_H

/* ---- switches: cabinet buttons close to ground, INPUT_PULLUP, LOW = pressed */
#define PIN_SW_ROT_L     0
#define PIN_SW_ROT_R     1
#define PIN_SW_THRUST    8
#define PIN_SW_FIRE      12   /* SPI0's default MISO - see PIN_SPI_MISO_ALT */
#define PIN_SW_HYPER     14   /* the cabinet's HYPER position: shields on Deluxe */
#define PIN_SW_START1    15
#define PIN_SW_START2    16
#define PIN_SW_COIN_L    17   /* -> plat_inputs.coin1, the left mech ($2400) */
#define PIN_SW_COIN_C    18   /* -> plat_inputs.coin2, the centre mech ($2401) */
#define PIN_SW_COIN_R    19   /* read, not mapped: the contract has two coin fields */
#define PIN_SW_SLAM      22   /* read, not mapped */
#define PIN_SW_SELFTEST  23   /* -> plat_inputs.test, the live switch level */
#define PIN_SW_DIAG      24   /* read, not mapped */

/* ---- lamps: 2N7000 low-side drivers, HIGH = lit */
#define PIN_LED_START1   25
#define PIN_LED_START2   26

/* ---- X/Y: two DAC7811 on SPI0 */
#define PIN_CS_X         10
#define PIN_CS_Y         9
#define PIN_SPI_MISO_ALT 39   /* unconnected on the board; frees pin 12 for FIRE */
#define VEC_SPI_HZ       30000000
#ifdef ARDUINO
#define VEC_SPI_MODE     SPI_MODE1   /* idle low, latched on the falling edge:
                                      * the DAC7811's default.  MODE2 also
                                      * satisfies the datasheet. TUNE on a scope */
#endif

/* ---- Z: the 5-bit binary-weighted ladder through a 74HC04 (inverting) */
#define PIN_Z_B0         2    /* LSB, 16K */
#define PIN_Z_B1         3
#define PIN_Z_B2         4
#define PIN_Z_B3         5
#define PIN_Z_B4         6    /* MSB, 1K */
#define VEC_Z_INVERT     1    /* the 74HC04: pin level = !bit */
/* DVG intensity nibble 0..15 -> ladder code 0..31; 0 stays off.  TUNE
 * against the monitor: the ladder is linear, the phosphor is not. */
#define VEC_Z_TABLE { 0, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31 }

/* ---- geometry: the stock DVG's.  Its counters and DACs are 10-bit, so its
 * beam space is 1024x1024 with the centre at 512; the DAC7811 is 12-bit, so
 * DVG code c becomes DAC code c << VEC_SHIFT (0..4092), no offset.  DVG 512
 * lands on DAC 2048, the DAC's mid-scale and 0 V after the bipolar stage -
 * where a stock board puts the centre.  The size/centre pots do the rest. */
#define VEC_SHIFT        2
#define VEC_DAC_MAX      4092 /* 1023 << VEC_SHIFT */
#define VEC_DAC_CENTER   2048
#define VEC_FLIP_X       0    /* 1 mirrors about the centre */
#define VEC_FLIP_Y       0

/* ---- beam stepping.  TUNE, all of them. */
#define VEC_DRAW_STEP      16   /* DAC counts per step with the beam on */
#define VEC_MOVE_STEP      64   /* DAC counts per step with the beam off */
#define VEC_DRAW_DWELL_US  0    /* extra wait per lit step (SPI time is the floor) */
#define VEC_MOVE_DWELL_US  0    /* extra wait per blanked step */
#define VEC_SETTLE_US      3    /* after arriving at a segment start, before Z on */
#define VEC_DOT_US         4    /* Z-on time for a zero-length segment */
#define VEC_PARK_MS        250  /* idle time before the beam is parked centred, Z off */
#define VEC_MAX_SEGS       4096 /* segment buffer entries per frame */
#define VEC_MAX_PTS        512  /* longest step plan: a 4092-count move at step 8 */

/* ---- audio ring and mixer */
#define AUDIO_RING_SIZE    4096 /* frames; power of two; ~93 ms at 44.1 kHz */
#define AUDIO_RING_START   1024 /* output stays silent until the ring first holds this */
#define AUDIO_RING_LOW     512  /* below: duplicate one frame per pull (consumer slows) */
#define AUDIO_RING_HIGH    3072 /* above: skip one frame per pull */
#define AUDIO_POKEY_GAIN   256  /* x/256 on the POKEY output (c012294.c already
                                 * scales a full channel to 32767/11 * 15) TUNE */
#define AUDIO_SAMPLE_GAIN  128  /* (u8 - 128) * this: +-16384 full scale  TUNE */
#define AUDIO_CHANNELS     2    /* app_win.c's AD_CHAN_THRUST 0, AD_CHAN_EXPLOSION 1 */

/* ---- build mode */
#define VEC_TEST_PATTERN   0    /* 1: draw the bring-up pattern instead of the game */

#endif
