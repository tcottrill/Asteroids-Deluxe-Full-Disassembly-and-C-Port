# The main line

How a frame is put together, from `START` at `$6000` to the jump back.
Everything here is read off `astdelux2_main.asm`; addresses are links into
it.

## The loop

`START` runs once. It calls `INIT` (`$7BF5`, the `CASTST` section) to set
up player 1, then kicks off an EAROM read — unless `EAFLG` says the chip
is still busy erasing, in which case it leaves it alone and picks the
read up on a later pass.

Everything after that is two nested loops:

```
START1   JSR NEWAST            start a new wave of rocks
START2   BIT HALT / BMI        wait for the DVG to finish the last frame
         JSR ROTAST            rotate the rocks
10$      LSR SYNC / BCC        wait for frame sync
         ... build and kick the display list ...
         ... run the game ...
         JMP START2            next frame
         JMP START1            or: next wave
```

The wave loop closes at `$60F6`: `RDELAY` is OR'd with `NROCKS`, and only
when **both** are zero does control fall through to `START2_50` and back
to `START1` for a new wave. `RDELAY` is the delay before adding rocks, so
this waits for the screen to be genuinely clear *and* the pause to
expire.

## Frame sync

`SYNC` is a counter the NMI bumps; the main line spins on `LSR SYNC`
until the bit comes out. NMI runs every ~4 ms and increments `SYNC` on
every fourth interrupt (`INTCT AND #3`), so the sync tick is ~16 ms and
the game runs at roughly 60 Hz.

The interesting part is what happens when the main line is *late*. The
NMI checks `SYNC` right after incrementing it, and if it has reached 3 —
meaning two ticks went by unconsumed — it drops into `L7880`:

```
7880:  D0 FE       bne L7880          ; PROGRAM WHERE ARE YOU?
```

an infinite loop, deliberately, so the watchdog resets the board. The
one way out is `SYNC` having wrapped to zero, which falls through to
`NMI_127` and strobes `VGRESET` "to stop the VG in case it's hung".

So there is no frame skipping and no catch-up. The game either keeps up
or the machine restarts.

## Double-buffered display list

Two display lists live in vector RAM and alternate on `FRAME` bit 0:

```
6020:  lda FRAME / and #$01 / asl / tax     ; this frame's buffer
6026:  eor #$02 / tay                       ; the other one
6029:  lda RADDR,x  →  VECRAM ($4000)       ; hand the DVG a JMPL to it
6035:  lda RADDR,y  →  VGLIST               ; build into the other
6045:  sta GOADD                            ; start the DVG
6048:  sta WTDOG                            ; kick the dog
```

`RADDR` (`$77DE`) is a four-byte table of the two buffers' `JMPL` words.
The first two bytes of vector RAM are therefore never picture data — they
are a jump into whichever buffer was completed last. `VGLIST` (`$03/$04`)
is the build pointer for the *other* buffer, and everything the frame
draws appends there through `VGADD2` / `VGADD`.

Note the shift-and-mask at `$6038`: the stored word is a DVG `JMPL`
operand, so the byte address is recovered by `ASL`/`ROL`, masking off the
opcode bits and OR-ing in `$40` — the high byte of `VECRAM`.

## What runs each frame

After the kick, in order:

| call | what it does |
|---|---|
| `EAUPD` | one step of the EAROM state machine |
| `CHKST` | check the start buttons — carry set means begin a game |
| `UPDATE` | update the high-score table |
| `GETINT` | collect initials, if a new high score is being entered |
| `SCORES` | draw the high-score table |
| `SHIELD` `FIRE` `MOVE` `SETTIP` `ENEMY` | player and enemy control — **game only** |
| `MOTION` `COLIDE` | move every object, then test collisions |
| `PARAMS` | draw score, lives, and the rest of the status |
| `SOUNDS` | decide which sounds to start |
| `VGSABS` `VGHALT` | park the beam and terminate the list |
| `PKYTST` | POKEY test |

The player block is skipped when `GDELAY` is non-zero (a new player is
starting) and when the high-score table is on screen — the comment at
`$60A0` explains why: *"not enough time for asteroids and score
tables"*. `MOTION` and `COLIDE`, by contrast, run in attract mode too,
which is what makes the attract screen a real game with no ship.

The beam park is deliberate: `VGSABS` is called with `A = X = $7F`, the
screen centre, *"to position beam for minimum current draw"* — the beam
rests in the middle rather than wherever the last vector left it.

## The checksum trickle

`START2_12` (`$604B`) increments `FRAME`, and when the low byte wraps —
once every 256 frames, so about every four seconds — it does one slice of
a rolling checksum over `$5000-$57FF`, `$6800-$6FFF` and `$7000-$77FF`,
one byte from each per pass, walking `PROT` through the range.

When the walk completes (`CPX #$6F`) it folds in a seed with `EOR #$03` —
and `$6080`, the operand byte of that `EOR`, is one of the addresses
`CKSUM.MAC` patches at link time. The result is stored to `$8C`, "signal
error or not (s/b 0)". The same block then moves the stack (`LDX #$FC /
TXS`) and stores `$FC` into `HOLE`.

`HOLE` is checked back at `$60CB` — "are we to test? ... only works once
every 2.5 hours". This is anti-tamper machinery rather than gameplay: a
bad checksum leaves a non-zero value where the code expects zero, and the
consequences surface much later and far away from the check.

## Reading further

- [`BOOT_AND_NMI_NOTES.md`](BOOT_AND_NMI_NOTES.md) — reset, the NMI body, coins
- [`GAMEPLAY_NOTES.md`](GAMEPLAY_NOTES.md) — the object engine `MOTION` and `COLIDE` drive
- [`VECTOR_NOTES.md`](VECTOR_NOTES.md) — what `VGADD2` and friends are appending
- [`FUNCTIONS.md`](FUNCTIONS.md) — the index of every routine named here
