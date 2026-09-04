# The object engine

How the game represents things on screen, moves them, and decides what
hit what.

## Everything is a parallel array

There is no object struct. Each attribute lives in its own array indexed
by object number, all declared in `PG0123.MAC` and reproduced in
[`astdelux_defines.asm`](astdelux_defines.asm).

The index space is fixed, and the same layout repeats in every array:

| index | count | what |
|---|---|---|
| 0 … 24 | 25 | rocks (`NOBJ`) |
| 25 | 1 | the ship |
| 26 | 1 | the saucer |
| 27-28 | 2 | saucer torpedoes |
| 29-32 | 4 | ship torpedoes |

so 33 slots. `OBJ` (`$0200`) is the base of the first array, and the ship
and saucer get their own names at their own offsets — `SHPPIX` is just
`OBJ+25`, `SAUPIX` is `OBJ+26`. The arrays:

| array | addr | holds |
|---|---|---|
| `OBJ` | `$0200` | picture select; 0 = inactive |
| `XINC` / `YINC` | `$0221` / `$0242` | velocity, format `S9999.BBB` |
| `OBJXH` / `OBJYH` | `$0263` / `$0284` | position, high order |
| `OBJXL` / `OBJYL` | `$02A5` / `$02C6` | position, low order |

`OBJ` is packed: bits 0-2 are size (1 small, 2 medium, 4 large), bits 3-6
the picture number, bit 7 set means exploding. A **special rock** — the
Deluxe addition — is flagged by bits 2 and 3 both set.

Positions are 16-bit split across two arrays rather than stored
adjacently, which is why `MOTION` reads `OBJXL,x` and `OBJXH,x` a page
apart. Velocities are fixed point with three fractional bits.

## Two players, one address

Pages 2 and 3 are a bank-switched pair. `BNKSEL` (`$3C04`) bit 7 selects
which is visible at `$0200`, so the code never indexes by player — the
same addresses always address whoever is up. `SBANK` (`$6CB1`) does the
switching.

That is also why the AAE driver has to swap the two pages in software
when the bank bit changes: the addresses do not move, the memory does.

## The frame

The main line calls, in this order:

```
SHIELD   FIRE   MOVE   SETTIP   ENEMY      (game only)
MOTION   COLIDE                            (always, attract included)
```

`MOTION` and `COLIDE` running in attract mode is what makes the attract
screen a real simulation with no ship in it.

| routine | addr | what |
|---|---|---|
| `SHIELD` | `$6675` | the shields button; drains `SHLDS`, one unit per four frames |
| `FIRE` | `$64BE` | fire a torpedo into a free slot |
| `MOVE` | `$6847` | rotate and thrust the ship from the controls |
| `SETTIP` | `$4A96` | the special rock's tip |
| `ENEMY` | `$6347` | launch the saucer and its torpedoes |
| `MOTION` | `$66F8` | advance every active object by its velocity |
| `COLIDE` | `$610E` | collision |

## Collision

`COLIDE` (`$610E`) starts by clearing `CHIST`, the collision history
register, then walks the eight objects from index 7 downward — the ship,
saucer and torpedoes — skipping inactive (`OBJ == 0`) and already
exploding (bit 7) ones. `CHIST` bit 7 records whether the ship was
involved, and when it was not, the code invalidates the flag with the `X`
register (which is `-1` after the loop).

The consequences are `DSTRCT` (`$62C6`, destruction during collision) and
`SPLIT` (`$6F7E`), which breaks a rock into fragments — the fragments get
positions from `CPYPOS` (`$6210`) and fresh velocities from `NEWVEL`
(`$6A57`), which derives a new random velocity from the old one.

## Rocks

`NEWAST` (`$698E`) starts a wave. `SEARCH` (`$6D6C`) finds a free slot,
`RNDPOS` (`$6A03`) picks a position, `NEWVE1` (`$6A7E`) a velocity.
`NEARBY` (`$6935`) checks whether rocks are close — used to avoid
materialising the ship on top of one.

`BOUNCE` (`$752C`) is its own module, and its own Deluxe addition: rocks
rebound rather than only wrapping.

The special rock's control logic is in `TRIROT.MAC`, over in the vector
board ROM: `SPLTTP` (`$4800`), `ATTACK` (`$48BE`), `RELTIP`, `SETTIP`
(`$4A96`), and the launch path that ends in `RNDXYI` (`$4B3D`) for a
random velocity. Its state sits at the top of page 2: `SROCKS` (`$02F0`,
how many special rocks), `SRANG` (`$02F1`, orientation) and `SRTIME`
(`$02F8`, control) — seven entries each, `NSPCLS`.

## The maths

Also in `TRIROT.MAC`, because the game needed the space:

| routine | addr | what |
|---|---|---|
| `MULT` | `$49B3` | signed multiply; exits with the product in `A` |
| `DIVIDE` | `$7121` | 4-bit divide (this one is in the program ROM) |
| `SINCOS` | `$4B51` | 65-byte sine/cosine table |
| `SIN` / `COS` | `$7137` / `$7134` | table lookup with quadrant folding |
| `ATAN` | `$70D4` | arctangent, via `ATANA` (`$710F`) |
| `ROTAST` | `$4B92` | rotate the rocks — called once per frame |
| `SCALER` | `$4A2A` | scaling |
| `CPXROT` | `$6EE2` | object orientation index |

`ATAN3` (`$7107`) shows the shape of it: `JSR DIVIDE / TAX / LDA ATANA,X`
— divide, then a 17-entry table lookup.

## What this note does not cover

The collision *geometry* — how a hit is actually decided — and the
saucer's aiming logic have not been read closely enough to describe
here. The listing has them fully named and commented; `COLIDE`,
`DSTRCT`, `EFIRE` (`$63E6`) and `LFCR` (`$6482`, "look for close rock")
are the places to start.

## Reading further

- [`MAINLINE_NOTES.md`](MAINLINE_NOTES.md) — where these calls sit in the frame
- [`VECTOR_NOTES.md`](VECTOR_NOTES.md) — how `PICTUR` and `SHPPIC` get drawn
- [`FUNCTIONS.md`](FUNCTIONS.md) — every routine, by module and section
