# Reset, the NMI, and coins

Everything that happens outside the frame loop: how the board comes up,
what the interrupt does, and how money becomes credits.

## Reset — `PWRON` (`$7CD7`)

The reset vector points into the self-test module (`DSTTST`), not into
the game. `PWRON` sets the stack to `$FE`, clears decimal mode, and
strobes `$2C0F` (a POKEY register) — the comment says *"we have 5.33 ms
to check page 1"*, i.e. before the first NMI arrives.

Then the RAM test, walking a `$11` bit pattern through each cell and
reading it back (`PWRON_11` at `$7CE0`, *"2 2114's"*), followed by a
clear of page 0, page 1, the two player pages, and vector RAM at `$4000`
and `$4100`.

The vectors themselves come from `SHUFFL.MAC`, which places them with
`.VCTRS ^H7FFA,NMI,PWRON,PWRON` — so NMI, RESET and IRQ/BRK are `$785C`,
`$7CD7`, `$7CD7`. Those three values are what identify this ROM as
rev 2; see [`README.md`](README.md#revisions).

## The NMI — `$785C`

Roughly every 4 ms. It is short and does four things.

**1. Bail out during power-up self-test.**

```
785C:  bit $01FF / bpl NMI_100
7861:  rti
```

**2. Check the stack has not run away.** `$01FF` is the top of the stack
and `$01D0` a floor; if either is non-zero the handler spins forever at
`NMI_2` and lets the watchdog reset the board. The game deliberately
leaves a known byte at `$01FF` — see the `HOLE` business in
[`MAINLINE_NOTES.md`](MAINLINE_NOTES.md#the-checksum-trickle).

**3. Divide down to the frame tick.** `INTCT` counts every interrupt;
every fourth one bumps `SYNC`, giving the ~16 ms tick the main line waits
on. If `SYNC` reaches 3 the main line has missed two ticks and the NMI
hangs on purpose (`L7880`, *"PROGRAM WHERE ARE YOU?"*).

**4. Run the sound and coin machinery.** `CSOUND` steps all eight sound
channels, then `MOOLAH` reads the coin mechs.

Sound is skipped in attract mode unless initials are being entered —
`NMI_10` tests `NPLAYR`, and then `UPDFLG AND $43` to catch the
high-score entry case where sound must keep running with no game in
progress.

## Coins — `MOOLAH` (`$78A9`)

The coin code is not part of this game: `DSTNMI.MAC` does
`.INCLUDE DCIN65`, a shared Atari coin module, and it assembles inline
inside the NMI handler. That is worth knowing when reading the listing —
the routines between `$78A9` and `$79E7` are a different provenance from
everything around them, and the `$`-prefixed names (`$DETCT`, `$BONUS`,
`$CNVRT`, `$EXT`) are its convention.

The module is configured by symbols the includer defines before pulling
it in. For this game:

| symbol | value | meaning |
|---|---|---|
| `MECHS` | 3 | three coin mechanisms |
| `OFFSET` | 1 | coin inputs are one bit apart |
| `EMCTRS` | 3 | three electro-mechanical counters |
| `CNTINT` | 0 | the includer counts interrupts itself |
| `BONADD` | 1 | bonus-adder mode enabled |
| `COIN` | 1 | coin inputs are active high |

Those values decide which arms of the module's conditionals assemble, and
getting them wrong silently changes the code — `MECHS` in particular
selects between a single-mech read and the three-mech indexed loop that
`$DETCT` actually is. This is why `nameroutines.py` evaluates conditional
expressions properly rather than guessing.

`$DETCT` walks the mechs from right to left, debouncing each through a
per-mech state counter in `CNSTT` (`$9D`), then converts to credits at
`$CNVRT` using the coinage options, applies the bonus adder at `$BONUS`,
and pulses the counters.

## EAROM

High scores and bookkeeping live in a 4-bit EAROM, reached through three
latches: `EAIN` (`$2C40`, read), `EADAL` (`$3200`, address/data) and
`EACTL` (`$3A00`, control).

It is slow, so nothing blocks on it. `EAUPD` (`$7B11`) is a state machine
the main line calls once per frame, driven by `EAFLG` (`$D4`) — the main
line at `$6003` kicks a read by storing `$20` there, and `START` skips
that if a previous erase is still running. `EABDS` (`$7BD2`) is the table
of which bytes get written: `0,1,2` then the three initials offsets
(`INITL-HSCORE` = `$21`), terminated with `-1`.

## Reading further

- [`MAINLINE_NOTES.md`](MAINLINE_NOTES.md) — what the frame loop does with `SYNC`
- [`SOUND_NOTES.md`](SOUND_NOTES.md) — what `CSOUND` is stepping
- [`FUNCTIONS.md`](FUNCTIONS.md) — the routine index
