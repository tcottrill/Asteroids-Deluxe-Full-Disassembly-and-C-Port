# Sound

Two unrelated mechanisms: a table-driven sequencer that pokes the POKEY,
and a handful of discrete circuits the CPU pokes directly.

## The hardware

| address | what |
|---|---|
| `$2C00` | POKEY (16 registers); `AUDF1` is the base, `AUDCTL` is `$2C08` |
| `$2C0A` | `RANDOM` — the POKEY's noise register, used as the game's RNG |
| `$3600` | `EXPSND` — explosion; bits 6-7 pitch, bits 2-5 volume |
| `$3C03` | `SPTEN` — ship thrust enable |
| `$3E00` | `NRESET` — noise reset |

The explosion and thrust are discrete analogue circuits, not POKEY
voices — the CPU just writes a level. Everything else goes through the
sequencer.

## The sequencer

Eight channels. Each has a byte in six parallel page-zero arrays, indexed
by channel number 0-7:

| array | addr | meaning |
|---|---|---|
| `POINT` | `$A2` | offset into the sound tables; **0 means the channel is free** |
| `CURRENT` | `$AA` | the value currently being output |
| `FRAMES` | `$B2` | interrupts left before the next change |
| `COUNT` | `$BA` | changes left in this step |
| `MCOUNT` | `$C2` | background noise / start flag |
| `MPNTR` | `$CA` | |

`CSOUND` (`$7787`) runs from the NMI and steps all eight every interrupt:
decrement `FRAMES`; when it hits zero, add the step's change value to
`CURRENT` and decrement `COUNT`; when *that* hits zero, advance `Y` by
four to the next step record.

A step is four bytes, written with the `STB` macro:

```
STB FCNT, START, CHNG, TOT     ; .BYTE FCNT,START,CHNG,TOT
```

read as *"every `FCNT` interrupts, add `CHNG`, `TOT` times, starting from
`START`"*. The source's own examples make it plain:

```
STB 1,4,1,10      ; start at 4; every interrupt add 1, 10 times
STB 1,14,-1,10    ; start at 14; every interrupt add -1, 10 times
```

A sound is a pair of these envelopes, one for frequency and one for
volume:

```
STB 2,0C,1,18     ; saucer fire frequency
STB 10,0A4,-1,3   ; saucer fire volume
```

## The sound set

`PNTRS` is the pointer table, eight bytes per sound. The `OFFSET` macro
defines each entry's index as `.-PNTRS+7`, which is why the identifiers
run 7, 15, 23 … — and those values are what the code loads into `Y`
before calling `SNDON`:

| `Y` | symbol | sound |
|---|---|---|
| `$07` | `SND.T1` | thump 1 |
| `$0F` | `SND.T2` | thump 2 |
| `$17` | `SND.LS` | little saucer |
| `$1F` | `SND.SF` | saucer fire |
| `$27` | `SND.PF` | ship fire |
| `$2F` | `SND.EL` | extra life |
| `$37` | `SND.BS` | big saucer |
| `$3F` | `SND.DI` | diamond zapped |
| `$47` | `SND.BL` | ship back to life |
| `$4F` | `SND.HT` | on the high-score table |
| `$57` | `SND.S` | shields |
| `$5F` | `SND.DS` | "death star appearance" |

## Starting and stopping

`SNDON` (`$775B`) takes the sound in `Y` and hunts for a free channel —
`POINT,x == 0`. `SNDOFF` (`$7752`) silences.

Both begin by checking `NPLAYR`: **in attract mode the game is silent**,
and `SNDON` falls straight through to `SNDOFF` rather than starting
anything. The NMI makes one exception, at `NMI_10` — sound keeps running
with no game in progress while initials are being entered.

`SOUNDS` (`$6F12`) is the main-line half: it decides *what* wants to play
this frame and calls `SNDON`. It will not start the extra-life or
special-rock sounds unless channels 6 and 7 are both free (`$A9`,
i.e. `POINT+7`), deferring to the next frame if not.

## An oddity worth knowing

The flag `HSSND` (`$E3`) is named for the high-score table, and
`SOUNDS` reads it under the comment *"any hi score noise req?"* — but the
sound it plays is `$5F`, the `DS` slot, labelled *"death star
appearance"*. And the routine that **sets** `HSSND` is in `TRIROT.MAC`,
in the special-rock launch path, commented *"signal to make noise when
channel avail"*.

So in the shipped game this is the Deluxe special ("diamond") rock
announcing itself, reusing a sound slot and a flag left over from
`$DTHST` — the abandoned Death Star variant that shares this code base.
The names and one of the comments are stale; the call sites are not.

## Reading further

- [`BOOT_AND_NMI_NOTES.md`](BOOT_AND_NMI_NOTES.md) — where `CSOUND` is called from
- [`FUNCTIONS.md`](FUNCTIONS.md) — the `DASOUN` section
- [`astdelux_defines.asm`](astdelux_defines.asm) — the POKEY and discrete addresses
