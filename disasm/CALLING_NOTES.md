# Asteroids Deluxe (rev 2) — calling conventions

Entry and exit contracts for every routine the C port calls across a
module boundary, read from `astdelux2_main.asm` (line numbers as
`L####`) and cross-checked against Atari's own sources (the `.MAC`
files of the source archive, not included here). This is the specification the C
signatures in `../c_src/astdelux.h` were fixed from.

## 0. Conventions the whole program assumes

**Object slot model.** One flat index `i` (0..32) selects a game object.
Every attribute lives in a parallel array on the banked page 2/3, stride
`$21` (33) between arrays:

| array | base | meaning |
|---|---|---|
| `OBJ` | `$0200` | picture/size code, 0 = free, bit7 set = exploding timer |
| `XINC` | `$0221` | X velocity (signed byte) |
| `YINC` | `$0242` | Y velocity |
| `OBJXH` | `$0263` | X position high |
| `OBJYH` | `$0284` | Y position high |
| `OBJXL` | `$02A5` | X position low |
| `OBJYL` | `$02C6` | Y position low |

Slots: **0–24 rocks**, **25 ship**, **26 saucer**, **27–28 saucer
torpedoes**, **29–32 ship torpedoes**. The `SHPPIX/SHPXI/SHPXL/...`
symbols are just `OBJ+$19`, `XINC+$19`, … so `lda SHPPIX,x` with X=0..7
means `OBJ[25+x]`.

**The `+$21` idiom.** Adding `$21` to an index converts any X-array
reference into the matching Y-array reference (`XINC`→`YINC`,
`OBJXH`→`OBJYH`, `OBJXL`→`OBJYL`, `SHPXI`→`SHPYI`, `SHPXL`→`SHPYL`).
`CPUTD`, `ATTACK`, `EFIRE`, `BOUNCE`/`BNC` all reuse the same code for
both axes this way. In C, index the raw page (`AD_P->raw[AD_XINC + i]`)
so the idiom carries over literally.

**Special-rock ("diamond") side arrays**, indexed 0..6, not by slot:
`SRTIME $02F8` (control byte, bit7=waiting, bit0=size), `SRANG $02F1`
(orientation). An `OBJ` byte with bit 6 set is a special rock; bits 5..2
(`(OBJ & $3C) >> 2`) are the *link* to its `SRTIME`/`SRANG` entry.

**ZP scratch aliases used throughout:** `VGLIST=$03`(+`$04`),
`XCOMP=$05`..`$08` (XL,XH,YL,YH), `TEMP1=$09`(+`$0A`), `TEMP2=$0C`(+`$0D`),
`TEMP3=$0E`(+`$0F`), `R0..R9=$10..$19`, `XT=$1A`, `TEMP4=$1B`(+`$1C`,`$1D`),
`ANGLE=$79`(+`$7A` = "firing direction"), `CHIST=$71`(+`$72`).

**Frame loop order** (`START2_10` L1321…): wait HALT → `ROTAST` → wait
`SYNC` → swap VG buffers via `RADDR` → `EAUPD` → `CHKST` → `UPDATE` →
`GETINT` → `SCORES` → `SHIELD`/`FIRE`/`MOVE`/`SETTIP`/`ENEMY` → `MOTION`
→ `COLIDE` → `PARAMS` → `SOUNDS` → `VGSABS`/`VGHALT` → `PKYTST`.

---

## 1. Object bookkeeping

### SEARCH ($6D6C) DSTRD0 — L3873
- entry: nothing (loads X=`$18`=24).
- exit: `X` = free rock slot, `A`=0, **N flag** = search result — N clear (X≥0) if a free entry exists, N set (X=`$FF`) if not. Y preserved.
- callers: `$4814` SPLTTP (free slot for split-off tip) · `$4858` SPLTTP_4 · `$6FB9` SPLIT_20 (first fragment; `bmi SPLIT_90` on failure).
- notes: falls straight into SEARC1. Source header (`DSTRD0.MAC:2612`): "EXIT (CC)=POSITIVE IF ENTRY EXISTS, (X)=FREE ENTRY INDEX IF POSSIBLE". Only scans slots 24→0, i.e. rocks only.

### SEARC1 ($6D6E) DSTRD0 — L3877
- entry: **X = slot to start scanning downward from** (caller-supplied).
- exit: same as SEARCH (X = free slot or `$FF`, N flag is the answer, A=0).
- callers: `$4884` SPLTTP_4 (continue below the slot just taken) · `$4B1D` SETTIP_5 · `$6D74` (SEARCH's own loop) · `$6FD3` SPLIT_20 (second fragment).
- notes: the "continue from X" entry exists purely so a caller that just consumed a slot doesn't re-find it.

### CPYPOS ($6210) DSTRD0 — L1672
- entry: **X = destination (new) rock slot, Y = source (old) rock slot**.
- exit: A = the new `OBJ` byte; X, Y preserved; `TEMP1` clobbered.
- callers: `$481E`, `$485B`, `$4887` SPLTTP · `$4B24` SETTIP_5 · `$6FC1`, `$6FDB` SPLIT_20.
- notes: copies OBJ (size bits 0-2 kept, **picture bits 3-4 re-randomised from `RANDOM`**), position (4 bytes) and velocity (2 bytes). It *destroys* the destination's picture code — SPLTTP at `$481D/$4821` deliberately pushes/pops the old `OBJ` around the call ("CPYPOS BLITZES 'OBJ'"). Reads hardware `RANDOM $2C0A`, so it is not a pure function.

---

## 2. Vector-list copy engine

### CPYVEC ($6246) DSTRD0 — L1701
- entry: `TEMP1` = X sign-flip mask (0 or 4), `TEMP1+1`(`$0A`) = Y sign-flip mask (0 or 4), **X = count of vectors to move, minus 1**, `R0/R1` = source vector-list address, `R2` = sign bit: positive → no X/Y swap, negative → swap (branch to CPYVYX), `VGLIST/VGLIST+1` = destination.
- exit: `VGLIST` advanced past what was written; `R0/R1` and `R2` unmodified; **`Y` = index of the last byte written** (this is the value `AYTOR0` needs); A, X, `R3`, `R4` clobbered.
- callers: `$4BCB` ROTAST · `$66BD` SHLDPX_1 · `$6E0A` SHPEXP_20 · `$6E8F` SHPPIC_1 · `$6EBE` AYTOR0 (`jmp`, tail-call).
- notes: handles both 4-byte long `VCTR` and 2-byte short vectors (`opcode >= $F0`). Ends in `jmp VGADD`, i.e. **`VGADD` is its return path** — Y-on-exit is consumed by VGADD before the RTS, but AYTOR0's contract still describes it. `TEMP1` doubles as a *brightness* injector: SHLDPX (`$66C3`) and SHPPIC (`$6E9E`) stuff an intensity nibble into `TEMP1` so the EOR turns an invisible source vector into a bright one.

### CPYVYX ($6279) DSTRD0 — L1755
- entry/exit: identical to CPYVEC; reached only when `R2` is negative.
- callers: `$624A` (CPYVEC dispatch) · `$62AC` (own loop).
- notes: emits each source vector with **X and Y components exchanged** by XOR-juggling the packed DVG words (`$6287`–`$62C4`); uses `R3`, `R4` and the 6502 stack as scratch. The final `bcs CPYVYX_5` at `$62C4` relies on carry still being set from the `cmp #$F0` at entry — a subtle "always" branch.

### AYTOR0 ($6EB2) DSTRD0 — L4087
- entry: **Y = the "last byte index" returned by the previous CPYVEC**, `R0/R1` = current source pointer, plus all normal CPYVEC inputs (`TEMP1`, `TEMP1+1`, `R2`, X = vector count-1).
- exit: `R0/R1` advanced by `Y+1`; then tail-jumps to CPYVEC, so exit state = CPYVEC's.
- callers: `$66C5`, `$66CC` SHLDPX_1 · `$6E6B`, `$6E7C`, `$6E89` SHPPIC · `$6EA2` SHPPIC_3 · plus **fall-through from THRTST** at `$6EB0`.
- notes: this is the "advance source cursor, copy next chunk" primitive. In C this is a stateful iterator over a ROM vector list.

---

## 3. Destruction / scoring

### DSTRCT ($62C6) DSTRD0 — L1814
- entry: **X = (ship/saucer/torpedo slot) − 25**, i.e. 0=ship, 1=saucer, 2–3=saucer torpedoes, 4–7=ship torpedoes. **Y = rock/ship/saucer slot** (0–24 rock, 25 ship, 26 saucer). Source header `DSTRD0.MAC:814`.
- exit: no meaningful registers; may not return normally (see notes).
- callers: `$61F0` COLIDE_38 only (X pushed/popped around the call).
- notes: **first thing it does is `jsr BOUNCE`, and BOUNCE can pop DSTRCT's return address off the stack so control lands back in COLIDE instead** — a non-local exit the C port must model (e.g. a `BOUNCE_ABSORBED` return code). Carry set from BOUNCE → jump to `SSBTLT_80` (award points, blow up saucer). At `$62D3` it rewrites X=0/Y=27 to convert "saucer hit ship" into "ship hit saucer". Sets `OBJ[slot] = $A0` (explosion timer) and zeroes that object's velocity.

### BLOUP ($62FC) DSTRD0 — L1860
- entry: **Y = slot of the object to blow up**.
- exit: A = the explosion timer value; `LEXPSND` set from the object's size (small/medium/large → different pitch+`$3F` length); `OBJ[Y]=$A0`, `XINC[Y]=YINC[Y]=0`.
- callers: `$631B` BLOUP_75 · `$6335` SSBTLT_80 (attract mode short-circuit) · `$6344` SSBTLT_85 (`jmp`).
- notes: `OBJ[Y]=$A0` is both "exploding" flag (bit 7) and a countdown that `MOV_12` advances.

### BLOUP_10 ($631D) — L1884
- entry: X = attacker index (saved through A). exit: `HITS[PLAYR]` decremented, A=`$81`, X restored. Falls into **SSBTLT**.
- callers: `$62D8` DSTRCT_60, `$6318` BLOUP_75.

### SSBTLT ($6325) DSTRD0 — L1892
- entry: **A = the value to store into `SDELAY`** (`$81` from BLOUP_10 = ship-death respawn delay; `$10` from CHKST1_62; `$01` from INIT).
- exit: `SDELAY = A`, `SBTLT = 5`; A=5; X,Y preserved.
- callers: `$4D3E` CHKST1_62 · `$7C17` INIT · fall-through from BLOUP_10.

### SSBTLT_80 ($632D) — L1899
- Not a callable routine — the "award saucer points" tail. Resets `EDELAY` from `SEDLAY`; in attract mode jumps to BLOUP with no score; otherwise gives `$00` (=1000 pts, small saucer) or `$20` (=200, large) to POINTS then `jmp BLOUP`.

### POINTS ($6C60) DSTRD0 — L3678
- entry: **A + carry = points/100**, in the packed form the routine shifts apart (`ROR A / ROR R0` ×4). Source header (`DSTRD0.MAC:2447`): "SCORE(PLAYR)=SCORE(PLAYR)+A*1000+CARRY*10000".
- exit: 3-byte BCD score at `SCORE+PLAYR3` updated; `R0`, `R1` clobbered; **X saved/restored via `R2`**; A clobbered; decimal mode entered (`sed`) and cleared (`cld`) internally.
- callers: `$48BB` GPTS_1 (`jmp`, so GPTS's caller gets POINTS's return) · `$6341` SSBTLT_85 · `$6FB1` SPLIT_15.
- notes: also implements the extra-life award — compares the running score against the per-player plateau at `$69,x`/`$6A,x`, bumps the plateau by `BONSCR+1/+2` (`$FE/$FF`), increments `HITS[PLAYR]` (capped at 10) and sets `SND3` to request the extra-life sound.

### GPTS ($48B2) TRIROT — L151
- entry: **A + carry = points to award; `TEMP3` = the index of whoever caused the hit** (0 = ship, 1 = saucer, 2–3 = saucer torpedoes, 4–7 = ship torpedoes).
- exit: if `TEMP3`==0 or `TEMP3`>=4 → `clc` and `jmp POINTS` (so exits as POINTS does); otherwise returns immediately via the shared RTS at `$48B1` with nothing changed.
- callers: `$4811` SPLTTP (5 = 500 pts, waiting tip) · `$4848` SPLTTP_1 (`$20` = 200) · `$4855` SPLTTP_4 (`$10` = 1000).
- notes: the guard means the saucer never scores for the player.

### SPLIT ($6F7E) DSTRD0 — L4247
- entry: **Y = index of the rock being split, X = index of the ship/saucer/torpedo that hit it** (absolute slot; stored in `TEMP3`).
- exit: X restored from `TEMP3`; `NROCKS` incremented by up to 2; `RTIMER=$50`; `TEMP3+1`(`$0F`) clobbered; A,Y clobbered.
- callers: `$62F9` DSTRCT_65.
- notes: if `OBJ[Y]` bit 6 is set (special rock) it `jmp SPLTTP` — a **tail call, so SPLTTP returns to DSTRCT_65's caller**. Otherwise it drops the size field by one (`(OBJ&7)>>1`), awards `HITSCR[size]` via POINTS, then twice: SEARCH/SEARC1 + CPYPOS + NEWVEL, and de-correlates the fragment positions by `OBJXL ^= (XINC&$1F)<<1` (`$6FC7`) and `OBJYL ^= (YINC&$1F)<<1`.

---

## 4. Sound

Sound numbers are `index*8+7`: `$07` THUMP1, `$0F` THUMP2, `$17` little saucer, `$1F` saucer fire, `$27` ship fire, `$2F` extra life, `$37` big saucer, `$3F` diamond, `$47` ship back to life, `$4F` high-score table, `$57` shields, `$5F` "death star"/hi-score-reached (`DASOUN.MAC:73` macro `SND.'LABEL==.-PNTRS+7`).

### SNDOFF ($7752) DASOUN — L5027
- entry: **Y = sound number** (`sound*8+7`). Sets V=1 (via `bit` on the `$60` RTS byte at `$7786`) and C=0, then enters the shared loop at `SNDOO_4`.
- exit: the 8 channels of that sound are silenced (`POINT[x]=0`, `MCOUNT[x]=$80`); X preserved (push/pop), A and Y clobbered.
- callers: `$6357` ENEMY_1 (`jmp`, saucer exploding) · `$6830` RSAUCR · `$775E` SNDOO (attract-mode redirect).

### SNDPON ($7758) — L5033
- entry: Y = sound number. `sec` (priority mode) then falls into SNDOO.
- exit: sound started **only on channels whose `POINT[x]` is 0** (won't interrupt a playing sound).
- callers: `$6690` SHIELD · `$6F66` SOUNDS_30 (thump).

### SNDON ($775B) — L5038
- entry: Y = sound number. `clc` (force mode) then falls into SNDOO — **unconditionally overwrites the channel**.
- callers: `$49AD` DIASND · `$63E1` LSF_40 (`jmp`) · `$6865` MOVE_92 (`jmp`) · `$6F22` SOUNDS · `$6F2F` SOUNDS_24 · `$7038` UPDATE_29.

### SNDOO ($775C) — L5042
- entry: Y = sound number, C = priority flag, V = off-flag.
- exit: A, Y clobbered; **X preserved** (pushed at `$7761`, pulled at `$7784`).
- callers: `$6584` FIRE3_51 (`jmp`) · `$7759` SNDPON.
- notes: `lda NPLAYR; beq SNDOFF` — **in attract mode every "start sound" call becomes a "stop sound" call**. The write order matters and is commented at `$7774`: zero `POINT[x]` first, then `MCOUNT[x]=$80`, then `POINT[x]=offset`, because NMIs run concurrently. Loops X=7→0 and Y downward through the 8-byte `PNTRS` row.

### CSOUND ($7787) DASOUN — L5094
- entry: none. Must be called once per NMI (4×/frame).
- exit: A, X, Y clobbered. Writes `POKEY+x` for each of the 8 channels.
- callers: `$78A6` NMI_3 only.
- notes: per-channel state machine over `POINT/CURRENT/FRAMES/COUNT/MCOUNT/MPNTR` (all 8-byte ZP arrays at `$A2/$AA/$B2/$BA/$C2/$CA`). Walks 4-byte records in the `DASOUN` data block; a zero frame-count byte ends a macro group, `MCOUNT` counts repeats. In the NMI path `$77D1 rts` returns to NMI_3, which then falls into MOOLAH at `$78A9`.

---

## 5. Message / text output

### VGMSG ($718F) DSTMSG — L4654
- entry: **Y = message number (0..13)**. `VGLIST` = current DVG write pointer.
- exit: `VGSIZE` forced to `$10`; `VGLIST` advanced; A, X, Y, `TEMP1`, `TEMP1+1`, `TEMP2` clobbered (source header `DSTMSG.MAC:168`).
- callers (12): `$4BFB`, `$4C08` CHKST · `$4CFB` BM_1 (`jmp`) · `$4D24` CHKST1 · `$6104` CHKST2 · `$659E`, `$65BB`, `$65C0`, `$65C5`, `$65CA` GETINT · `$6CD6` SCORES_90 · `$6CE5` SCORES_30.
- notes: reads the language from `OPTN4 $2803` bits 0-1, then falls into VGME.

### VGME ($7198) — L4663
- entry: **A = language index 0..3, Y = message number**. (English-only entry: callers pass A=0.)
- exit: same as VGMSG.
- callers: `$6B2B` PARAMS_21 (BONUS message, forced English).
- notes: `TEMP1/TEMP1+1` ← `VGMSGT[lang]` + `L<lang>[msg]`; screen position ← `VGMSGS[msg*2]`. Falls into CM.

### CM ($71B5) — L4684
- entry: **A = X position/4, X = Y position/4, `TEMP1/TEMP1+1` = packed-text pointer**.
- exit: beam positioned (`VGSABS` + `VGWAIT $70`) then falls into ASTMSG.
- callers: `$7178` SETROL_1.

### ASTMSG ($71BD) — L4690
- entry: **`TEMP1/TEMP1+1` = pointer to packed text**, `VGLIST` = DVG write pointer, `CPMTST` = test-mode flag.
- exit: `VGLIST` advanced; A, X, Y, `TEMP2` clobbered.
- callers: `$718D` CPYRS (`bne`, "always") · `$782B` VGRCPT_2 · fall-through from CM.
- notes (critical for C): **the loop terminates by popping its own return address.** `VGMSG2` at `$71F0` does `pla / pla` to discard the `jsr VGMSG1/VGMSG2` return, then `bne VGMSG0` → `dey / jmp VGADD`. Encoding is 3 characters per 2 bytes (5 bits each, used as `charcode*2` masked `$3E`); **bit 0 of the second byte of each pair is the "more text follows" flag** (`lsr TEMP2 / bcc` at `$71DE`). When `CPMTST` is negative, characters are XOR-accumulated into `CRMERR` via `CPR.UP` instead of being written — the copy-protection check.

### SETROL ($714E) DSTMSG — L4610
- entry: none. exit: `FRAME`=0, all four rock DVG subroutines rotated, the copyright display list built at `$4702`.
- callers: `$7BFA` INIT.
- notes: calls `ROTAST` 4 times (clobbering `FRAME` as the loop counter — the comment at `$7BFA` warns "CLEARS FRAME"), then writes `VGLIST=$4702` directly and unpacks `ASTM` at `$7603`. Ends `jmp VGRTSL`.

### CPYRS ($717E) DSTMSG — L4639
- entry: `VGLIST` = write pointer. exit: `TEMP1/TEMP1+1` = `$7603` (ASTM), `VGLIST` advanced; falls (via `bne ASTMSG`) into ASTMSG.
- callers: `$6CD1` SCORES_90.

### DIGITS ($7C98) DSTTST/CASTST — L6226
- entry: **C = 1 for leading-zero suppression; A = zero-page address of the LSB; Y = number of ZP bytes (1..127), each holding 2 BCD digits.**
- exit: `VGLIST` advanced by 2 glyph words per digit; A, X, Y clobbered; **`TEMP4`, `$1C`, `$1D` destroyed** (source header `DSTRD0.MAC:3220`).
- callers (7): `$4C30` CHKST_18 (credit count) · `$6ADA` PARAMS_10 (P1 score) · `$6B12` PARAMS_22 (high score) · `$6B36` PARAMS_21 (bonus) · `$6B73` PARAMS_30 (P2 score) · `$6D1C` SCORES_20 (rank) · `$6D2F` SCORES_20 (table score) · plus fall-through from BONDSP.
- notes: iterates MSB→LSB; the last nibble is forced visible by `clc` at `$7CB4`.

### VGHEXZ ($79F7) DSTCUT — L5646
- entry: **A = byte whose low nibble is the digit; C = 1 while still suppressing leading zeros.**
- exit: **C = updated suppression state** — stays set if the digit was 0 and suppression was on (glyph 0 = blank is emitted), cleared once a non-zero digit prints. A, X, Y clobbered; `VGLIST` advanced.
- callers: `$7CAD`, `$7CB9` DIGITS.

### VGHEX ($79FD) DSTCUT — L5656
- entry: **A = value, low 4 bits are the digit.** exit: `VGLIST` advanced, **C = 0** (header `DSTCUT.MAC:188`), A/X/Y clobbered.
- callers: `$610B` CHKST2 (`jmp`) · `$65B1` GETINT_20 · `$79F7` VGHEXZ · `$7CC5` VGBLNK.
- notes: glyph index is `digit+1` (glyph 0 is the blank).

### VGCHAR ($7A0A) DSTCUT — L5675
- entry: **Y = character index × 2** (header `DSTCUT.MAC:204`).
- exit: `VGLIST` advanced by 2; A, X, Y clobbered.
- callers (7): `$66F5` INITAL_1 (`jmp`) · `$6B19` PARAMS_22 · `$6D27`, `$6D34` SCORES_20 · `$7A05` VGHEX1 · `$7E95`, `$7E9D` STEST6_32.
- notes: **clamps** — `cpy #$4A / bcc; ldy #$00`, so any index ≥ 37 becomes the blank. Reads `VGMSGA[y]`/`VGMSGA+1[y]` then `jmp VGADD2`.

### VGWAIT ($7AFC) DSTCUT — L5901
- entry: **A = timer (0 = no delay, `$90` = max, increments of `$10`)**. exit: `VGLIST` advanced by 4; X forced to 0 (no intensity) then falls into VGDOT.
- callers (10): `$65DA`, `$6AB8`, `$6B0A`, `$6B53`, `$6C54`, `$6C5D`, `$6CCE`, `$6D0B`, `$71BA`, `$7C7F`.

### VGAWT ($7AF7) DSTCUT — L5892
- entry: **A = X position/4, X = Y position/4**, `VGSIZE` = scale. exit: emits a LABS then a wait-7; `VGLIST` advanced; A,X,Y used.
- callers: `$7E8D`, `$7EB0`, `$7EC8`, `$7F17`, `$7F53` (all self-test).
- notes: `jsr VGSABS` then `lda #$70` and **falls into VGWAIT**.

### VGDOT ($7AFE) DSTCUT — L5909
- entry: **A = wait timer, X = intensity** (`0`=off, `$F0`=max).
- exit: `VGLIST` advanced by 4; A, Y clobbered.
- callers: `$6C34` PICTUR_70 (torpedo dot) · `$6D22` SCORES_20 (period after the rank number).

### VGBLNK ($7CC5) DSTTST — L6271
- entry: A = digit value. exit: digit glyph + a space glyph appended; A,X,Y clobbered.
- callers: `$7EBC`, `$7EE4`, `$7EEB`, `$7EF4`, `$7EFB` (all STEST6, option-switch display).
- notes: `jsr VGHEX` then loads the `VGSPAC` JSRL (`$0A,$CB`) and **falls into VGADD2**.

### VGLABS ($7A4A) DSTCUT — L5747
- entry: **X = zero-page address of a 4-byte block laid out (X LSB, X MSB, Y LSB, Y MSB)**; `VGSIZE` = scale factor (`$00`..`$F0`) which is OR'd into the X-MSB byte.
- exit: 4 bytes appended; `VGLIST` advanced (falls into VGADD with Y=3); A, Y clobbered.
- callers: `$6C45` POSBEM (X=`$05`=`XCOMP`) · plus fall-through from `VGSABS` (which sets X=5).
- notes: the indexing is by absolute ZP address — `lda VGBRIT,x` with X=5 reads `$07` (Y LSB), etc.

---

## 6. Maths

### SIN ($7137) DSTRD0 — L4582
- entry: **A = angle, 0..$FF = 0..360°; N flag must match A's sign** (header `DSTRD0.MAC:3281`).
- exit: **A = sine, −127..+127**; uses A, X. Y preserved.
- callers: `$4990` ATTACK_19 · `$651B` FIRE3_30 · `$6905` CACCEL_2 · fall-through from COS.

### COS ($7134) — L4574
- entry: A = angle. exit: A = cosine. Implemented as `clc / adc #$40` then **falls into SIN**.
- callers: `$497D` ATTACK_19 · `$64F8` FIRE3.

### SIN1 ($7141) — L4593
- entry: A = angle 0..$7F. exit: A = table value 0..$7F.
- callers: `$7137`, `$713B` (both from SIN). Reads `SINCOS $4B51`.

### ATAN ($70D4) DSTRD0 — L4487
- entry: **X = X component (signed), Y = Y component (signed)**.
- exit: **A = angle, 0..$FF where `$40` = 90°**; uses A, X, Y, `TEMP2`, `TEMP2+1` (header L4489-4492).
- callers: `$4A57` SCALER_6 (`jmp` — so ATAN returns to SCALER's caller).
- notes: quadrant folding is done by *recursive* `jsr` into its own sub-entries.

### ATAN1 ($70E0) — L4499: entry A/Y = |Y| component, X = X component. Callers `$70D5` (branch), `$70DA` (jsr).
### ATAN2 ($70F2) — L4518: entry A = |X| (→`TEMP2+1`), Y = |Y|. Callers `$70E2`, `$70E7`. Falls into COMP at `$70EA` via `eor #$80`.
### ATAN3 ($7107) — L4532: entry A = dividend, `TEMP2+1` = divisor. `jsr DIVIDE` then `ATANA[quotient]`. Callers `$70F7`, `$70FE`.

### COMP ($70EC) DSTRD0 — L4508
- entry: **A = value to negate**. exit: **A = −A**, C cleared→set by the `adc`. Only A is touched.
- callers (10): `$4A2E`, `$4A37` SCALER · `$6724` MOV_12 · `$68EF` CACCEL · `$6DC4` SHPEXP_20 · `$70D7`, `$70DD` ATAN · `$70E4` ATAN1 · `$7104` ATAN2 (`jmp`) · `$713E` SIN (`jmp`) · fall-through from ATAN1.

### DIVIDE ($7121) DSTRD0 — L4554
- entry: **A = dividend (unsigned), `TEMP2+1` (`$0D`) = divisor (unsigned)**.
- exit: **A = 5-bit quotient (0..$10)**; `TEMP2` used as the shift register; Y clobbered (header `DSTRD0.MAC:3254`).
- callers: `$7107` ATAN3.

### MULT ($49B3) TRIROT — L345
- entry: **A = multiplicand (signed), `R1` = multiplier (`$80` = 1.000)**.
- exit: **A = product (signed)**; **Y, `R1`, `R2`, `R3` destroyed**; X preserved (header L347-349 / `TRIROT.MAC:364`).
- callers: `$4980`, `$4993` ATTACK_19 · `$6908` CACCEL_4.
- notes: shift-and-add over `R1`, sign handled by negating the operand and re-negating the product.

### RANGE ($49DF) TRIROT — L397
- entry: **`TEMP2`:A = 16-bit difference (low:high)**.
- exit: **`TEMP2`:A = the difference, wrapped to the shorter way around the torus**; `R5` destroyed.
- callers: `$4A01`, `$4A07` RTST (branch-in) · `$6431`, `$6445` EFIRE_81.
- notes: if `|high|` ≥ `$10` the difference is negated (i.e. go the other way round the wrap-around playfield).

### RTST ($49F5) TRIROT — L418
- entry: `TEMP2`:A = 16-bit difference; **X = the object index being tested** (used to decide X-axis vs Y-axis: `cpx #$21`); `BINSCR` = score/10000; `R9` = number of active attackers.
- exit: same contract as RANGE (`TEMP2`:A ranged), `R5` destroyed.
- callers: `$48FC`, `$4912` ATTACK_4.
- notes: this is the *difficulty* hook — below 40 000 points or with fewer than 3 enemies it just falls through to plain RANGE; above that it deliberately chooses the **longer** way round so the diamond attacks from the "wrong" side.

### CPUTD ($4A1A) TRIROT — L445
- entry: **Y = the reference object** (0 = ship X, 1 = saucer X, `$21` = ship Y, `$22` = saucer Y — the `+$21` alias), **X = the object under test** (add `$21` for the Y axis).
- exit: **A = high byte of the 16-bit difference (sign-extended), `TEMP2` = low byte**; X, Y preserved (header `TRIROT.MAC:444`).
- callers: `$48F9`, `$490F` ATTACK_4 · `$642E`, `$6442` EFIRE_81.

### SCALER ($4A2A) TRIROT — L455
- entry: **`R2`:`R3` = 16-bit X difference (low:high), `TEMP2`:A = 16-bit Y difference (low:high)**.
- exit: **A = angle between the objects**; X, Y, `R2`, `R3`, `R4`, `R5`, `TEMP2`, `TEMP2+1` destroyed (header `TRIROT.MAC:465`).
- callers: `$4915` ATTACK_4 · `$6448` EFIRE_81.
- notes: normalises both differences to the same binary exponent, then `jmp ATAN` — a tail call, so **SCALER's RTS is ATAN's**.

---

## 7. Object spawning / velocity

### NEWVEL ($6A57) DSTRD0 — L3292
- entry: **X = destination slot for the new velocity, Y = slot whose velocity is the seed** (header `DSTRD0.MAC:2133`).
- exit: `XINC[X]`, `YINC[X]` set; A clobbered; X, Y preserved.
- callers: `$6A0D` RNDPOS · `$6FC4`, `$6FDE` SPLIT_20.
- notes: adds a `RANDOM`-derived −16..+15 to each of the seed's components, then clamps via NEWVE1.

### NEWVE1 ($6A7E) DSTRD0 — L3318
- entry: **A = candidate velocity byte (signed)**; `RVELP $E1` / `RVELM $E2` = minimum positive/negative speeds (set by PKYTST).
- exit: **A = velocity clamped to ±`$3F`-ish and pushed away from zero** (`≥ RVELP` if positive, `≤ RVELM` if negative, never below `$E1`, never above `$1F`); flags reflect A.
- callers: `$6A64`, `$6A77` NEWVEL · `$7587`, `$7593` BOUNCE_6 (halving a rock's speed).

### NEWSHP ($6A37) DSTRD0 — L3272
- entry: none. exit: ship placed at `SHPXH=$10 / SHPXL=$60`, `SHPYH=$0C / SHPYL=$60`, zero velocity, `SHLDS=$FF` (full shields). A clobbered; X, Y preserved.
- callers: `$4C9A` CHKST_12 (2-player start) · `$4CB4` CHKST_20 (game start) · `$6753` MOV_18 (respawn after explosion finishes).

### RNDPOS ($6A03) DSTRD0 — L3238
- entry: **X = slot to populate, Y = slot to use as the velocity seed** (callers pass `$1A` = saucer).
- exit: `OBJ[X]` = random picture + size 4 (large), position placed on the X or Y screen edge, velocity from NEWVEL. A clobbered; X, Y preserved.
- callers: `$4ACE` SETTIP_9 · `$69E4` NEWAST_10 (loop over the whole new wave).

### RNDXYI ($4B3D) TRIROT — L674
- entry: **X = slot**. exit: `XINC[X]` and `YINC[X]` each set to `+6` or `−6` at random; A, Y clobbered.
- callers: `$4AD1` SETTIP_9.
- notes: `jsr RNDXYI_1` immediately followed by fall-through into `RNDXYI_1` — "do the following twice"; the first call's result becomes `XINC`, the fall-through's becomes `YINC`.

---

## 8. Display of scores / lives / initials

### INITAL ($66E1) DSTRD0 — L2603
- entry: **Y = index into the `INITL $44` table** (header L2606).
- exit: one glyph (or the underline shape) appended; `VGLIST` advanced; A, X, Y clobbered.
- callers: `$65E9`, `$65EF`, `$65F6` GETINT_25 · `$6D66` IN3TIM_1.
- notes: if the initial is blank *and* an update is in progress, it emits `JSRL UNDERL` ($56B0) instead of a character — that's the flashing cursor.

### LIVES ($7C72) CASTST — L6187
- entry: **A = X position for the first ship, Y = number of lives** (header L6189).
- exit: forces `X = $D5` (Y position 852/4) then falls into LIVESS.
- callers: `$6AE6` PARAMS_20 (player 1, A=`$28`) · `$6B7F` PARAMS_40 (player 2, A=`$CF`).

### LIVESS ($7C74) — L6193
- entry: **A = X/4, X = Y/4, Y = number of lives.**
- exit: **Y−1 ship glyphs drawn** (`dec TEMP1 / beq done / bpl draw`), `VGSIZE` left at `$E0`; A, X, Y, `TEMP1` clobbered.
- callers: `$6D49` SCORES_20 (top-3 high-score rows) · `$7F08` STEST6 (self-test) · fall-through from LIVES.
- notes: the "−1" is deliberate — the ship currently in play is on the playfield, and `GTSP` pre-adjusts Y when it isn't.

### BONDSP ($7C93) CASTST — L6219
- entry: none. exit: sets A=`$FD` (`BONSCR`), Y=3, C=1 and **falls into DIGITS**, so exit = DIGITS's.
- callers: `$7F1A` STEST6_27.

### GTSP ($6B90) DSTRD0 — L3498
- entry: **A = 0 for player 1, 1 for player 2; Y = that player's `HITS` count**; `PLAYR` = which player is up.
- exit: **Y = HITS−1 if that player's ship is currently visible, else Y unchanged** (header `DSTRD0.MAC:2295`); A, X clobbered.
- callers: `$6AE1` PARAMS_20 · `$6B7A` PARAMS_40.
- notes: for the not-up player it inspects `$0319` (= `SHPPIX` in the *other* bank) — a cross-bank read that a C port must map to the other page (`g.page[g.bank ^ 1].raw[0x19]`).

### TWO ($6BA5) DSTRD0 — L3529
- entry: none; reads `SBTL $8F` (ship-back-to-life intensity).
- exit: returns immediately if `SBTL >= $C0`; otherwise draws a "twinkle" ship off-screen with `ANGLE` temporarily `$40` and `SBTL` temporarily `$C0−SBTL`, restoring both. A clobbered.
- callers: `$6AF4` PARAMS_22 · `$6B8D` PARAMS_40 (`jmp`).

### IN3TIM ($6D5E) DSTRD0 — L3857
- entry: **`R3` = starting index into `INITL`**.
- exit: three initials drawn; `R3` advanced by 3; A, X, Y clobbered.
- callers: `$6B1C` PARAMS_22 · `$6D37` SCORES_20.
- notes: implemented as `jsr IN3TIM_1 / jsr IN3TIM_1 / fall into IN3TIM_1` — three iterations, unrolled through the stack.

---

## 9. Ship drawing

### SHPPIC ($6E3C) DSTRD0 — L4016
- entry: `VGLIST` = DVG write pointer; `ANGLE $79`; `SBTL $8F` (intensity/twinkle phase); `THRUST` switch.
- exit: `VGLIST` advanced; A, X, Y, `R0/R1`, `R2`, `R3`, `R4`, `R5`, `TEMP1`, `TEMP1+1` clobbered (header `DSTRD0.MAC:2738`).
- callers: `$6BBA` TWO_1 · `$6C27` PICTUR_50 (`jmp`).
- notes: two phases. If `SBTL < $C0` it first draws the materialisation sparkle: `SBTL += 6`, random inversion masks, `R0/R1 = $4D87` so the first `AYTOR0` lands on **`EXP16 $4D88`**, and 10 vector pairs are copied with random on/off intensity. Then `CPTSPX` + `CPYVEC` for the hull, then **falls into THRTST** for the flame.

### SHPEXP ($6D77) DSTRD0 — L3892
- entry: **`SHPPIX` = the explosion countdown** (`$A0`..`$FF`), `XCOMP..XCOMP+3` = ship position.
- exit: 8 fragments drawn; A, X, Y, `R0/R1`, `R2`, `R5`, `TEMP1`, `TEMP1+1`, `XCOMP..+3` clobbered.
- callers: `$6C02` PICTUR_33 (`jmp`).
- notes: on the first frame (`SHPPIX < $A2`) it seeds eight fragment records — `SXPXL/SXPYL/SXPXH/SXPYH $0100/$0108/$0110/$0118`, `EXPTMB $0120` (spin rate), `EXPDX $E5`, `EXPDY $ED`, `EXPANG $F5`. **These live in page 1, above the stack** — the C port keeps them in `g.pg1`. Each frame it advances position (`SHPEXP_100`, called twice with X and X+8 for the two axes), positions the beam with `POSBEM`, and copies one vector from `TFPIX+$40` ($506A).

### THRTST ($6EA5) DSTRD0 — L4079
- entry: reached only by falling out of `SHPPIC_3`; needs `R0/R1`, Y, `TEMP1` in the CPYVEC state SHPPIC left them in.
- exit: if `THRUST` is off or `FRAME & 4 == 0` (15 Hz flicker), branches to `TTST` (RTS). Otherwise sets X=1 (2 vectors) and **falls into AYTOR0**.
- callers: none — fall-through only.

### TTST ($6EE1) — L4123: a bare RTS shared by THRTST's two exits (`$6EA8`, `$6EAE`).

### CPTSPX ($6EC1) DSTRD0 — L4098
- entry: `ANGLE`.
- exit: `R0/R1` = address of the ship hull vector list inside `SHIPSV $53C6`; `TEMP1`/`TEMP1+1` = sign masks and `R2` = swap flag (all set by CPXROT); **X = 0** (1 vector); A, Y clobbered.
- callers: `$6E8C` SHPPIC_1.
- notes: subtracts `$40` first because "PICTURES ARE STORED 90-135 SO BACK UP".

### CPXROT ($6EE2) DSTRD0 — L4131
- entry: **A = orientation angle 0..$FF**.
- exit: **Y = index of one of 9 words (even, 0..$10)**; `R2` = positive if no X/Y swap, negative if swap; `TEMP1` = X sign-change mask (0 or 4); `TEMP1+1` = Y sign-change mask (0 or 4); uses A, Y, `R0`, `R2` (header `DSTRD0.MAC:2838`).
- callers: `$4BA4` ROTAST · `$6EC6` CPTSPX.
- notes: **Y can legitimately reach `$10`.** `SHIPS $53BC` has 9 entries so `CPTSPX` (which halves Y) is fine, but **`RSOURC $5002` only holds 8 words**, so `ROTAST` with Y=`$10` reads `$5012/$5013` — the first word of `SHLDVC`, giving address `$5180`. This is a real off-by-one in the ROM; the port reproduces it for free by reading the table through `ad_rom()`. Control table `TABLE` at `$6F0A` (8 bytes `00 A3 22 81 03 A0 21 82`): bit7 = negate picture select, bit5 = swap X/Y, bit1 = X sign mask, bit0 = Y sign mask.

---

## 10. Ship motion / collision response

### NEARBY ($6935) DSTRD0 — L3068
- entry: none; scans slots 24→0.
- exit: **Z flag is the answer — Z set (X=0) if the area around the ship is free of rocks**; Z clear and `SDELAY` incremented if a rock is within `$05` screen units in both axes (header L3070).
- callers: `$6857` MOVE_1 (`bne RTS.3` if something is close).
- notes: also contains the anti-stalemate timer: every 4 frames it increments `SBTLT`, and when that wraps it deletes one rock at a time (smallest first, skipping specials) and decrements `NROCKS`.

### MOVE2 ($6921) DSTRD0 — L3044
- entry: **A = high byte, X = low byte of a 16-bit velocity**.
- exit: **A/X clamped to the range `$C001`..`$3FFF`**; flags from the last operation.
- callers: `$6899`, `$68B3` MOVE1 · `$75DF` BNC_26.

### CACCEL ($68ED) DSTRD0 — L2995
- entry: **A = the current velocity component; `R2` = `$40` to use cosine (X axis) or `$00` to use sine (Y axis)**; `ANGLE`; `SHDON` (shields halve thrust).
- exit: **A = low byte, `R1` = high byte of the 16-bit acceleration delta; C = 0** so the caller can `adc XINCL` directly; X, Y, `R2`, `R3` clobbered (MULT's clobber list).
- callers: `$688E`, `$68A8` MOVE1.
- notes: acceleration falls off with speed via `ACCEL[|v|>>3]`.

### BOUNCE ($752C) BOUNCE — L4831
- entry: **X = attacker index−25** (0=ship, 1=saucer, 2-3 saucer torps, 4-7 ship torps), **Y = target slot** — same as DSTRCT.
- exit, three distinct ways:
  1. **C = 0, normal RTS** — no shields, or the pair is not a shield interaction. Caller DSTRCT proceeds to destroy.
  2. **C = 1, normal RTS** — award points and blow up the saucer (`BOUNCE_7` at `$7568`, ship rammed the saucer with ≥`$80` shields). DSTRCT jumps to `SSBTLT_80`.
  3. **stack-unwinding exit at `BOUNCE_14` `$754A`**: `pla / pla` discards BOUNCE's own return address, so the subsequent `rts` at `$755A` returns **to COLIDE (`$61F3`), skipping the rest of DSTRCT entirely**. This is the "shield absorbed it" path — used for torpedoes (`$7548`) and for rock bounces (`$75AA`, and the repeat-collision shortcut at `$7574`).
- callers: `$62C6` DSTRCT.
- notes: `BOUNCE_4`..`BOUNCE_1` (`$754C`-`$755A`) is used both as fall-through and as an internal subroutine (`jsr BOUNCE_4` at `$7568`). Shield cost per event is passed in X as a signed byte: `$E1` (−31) torpedo, `$F8` (−8) rock, `$B0` (−80) full rock bounce. When shields hit zero it clears `SHDON` so the *next* collision is lethal. Sets `CHIST` bit 7 and stores the rock slot in `CHIST+1` (`$72`) so repeated contact with the same rock drains shields without bouncing again.

### BNC ($75AC) BOUNCE — L4928
- entry: **X = 0 for the X axis or `$21` for the Y axis; Y = rock slot (X axis) or rock slot + `$21` (Y axis); `R0` = the size/special code** saved by BOUNCE_6 (bit 7 = special rock, non-zero = small/medium).
- exit: `SHPXI`/`SHPYI` (i.e. `SHPXI,x`) updated; `R1` used to save X; A, X, Y clobbered.
- callers: `$759B`, `$75A5` BOUNCE_9.
- notes: three cases — if the ship overtook the rock, ship velocity = −rock velocity; otherwise ship = 2× rock (or 1.5× for a special rock); always clamped by `MOVE2` and pushed at least `±6` away from zero. `BNC_30` at `$75F6` is a 16-bit position compare that answers "is the ship ahead of or behind the rock".

---

## 11. Enemy AI (special rock / saucer)

### SPLTTP ($4800) TRIROT — L44
- entry: **Y = index of the special-rock object being hit; `TEMP3` = index of whoever hit it**; X is clobbered immediately.
- exit: X restored from `TEMP3`; `NROCKS`, `SPROCK` updated; A, Y, `XT`, `R0` clobbered.
- callers: `$6F8D` SPLIT_1 (`jmp`, so SPLTTP returns to DSTRCT_65's caller).
- notes: three cases driven by `SRTIME[link]`. If the cluster is still *waiting* (bit 7): award 500, clone the object into a free slot preserving the old picture code, zero the original so it makes a big explosion, then walk the `SRTIME` link chain at `SPLTTP_3` (`$482C`) releasing every member. If it's a small diamond: deactivate, 200 points, `DIASND`. If it's a big diamond (`SPLTTP_4` `$4851`): 1000 points, and split into two new special rocks at `ANGLE ± $20`, each with a fresh `SRTIME`/`SRANG` control entry.

### FREESR ($48A7) TRIROT — L136
- entry: none. exit: **X = index of a free `SRTIME` slot (0..6), or `$FF`** via the shared RTS at `$48B1`; A clobbered.
- callers: `$487D` SPLTTP_4.

### ATTACK ($48BE) TRIROT — L167
- entry: **X = the attacking object's slot** (a rock slot 0..24 carrying the "special" bit); `R9` = running count of attackers this frame; `BINSCR` = score/10000 (difficulty).
- exit: `XINC[X]`, `YINC[X]` set to the new heading; `SRANG[link]` and `SRTIME[link]` updated; `R9` incremented; A, X, Y, `R0`, `R1`, `R2`, `R3`, `XT`, `TEMP2` clobbered.
- callers: `$679F` MOV_14.
- notes: uses `R1` as the *target selector*: `$19` = the ship, `$1A` = the saucer/"snow flake" (incremented at `ATTACK_2` `$48F1` when the timer runs out or the ship is not visible). Distance is computed twice through `CPUTD`+`RTST` (once per axis via the `+$21` trick) and turned into a heading with `SCALER`. Turn rate comes from `AVEL[R9]` (capped at index 6). Speed is `$1C + min(BINSCR*2, $0F)`, applied through `COS`/`SIN`+`MULT`.

### DIASND ($49A4) TRIROT — L326
- entry: none (reads `SPROCK`). exit: `SPROCK` decremented; if it reaches 0, sound `$3F` is started; **Y is preserved** (pushed/popped), A undefined (header `TRIROT.MAC:346`).
- callers: `$484B` SPLTTP_1.

### TRIPIX ($4A6D) TRIROT — L517
- entry: **A = the object's `OBJ` byte** (link field in bits 5..2 — note it shifts *three* times then masks `$0F`).
- exit: a JSRL word appended to the display list via `jmp VGADD2`; A, X, Y, `R1` clobbered.
- callers: `$6C12` PICTUR_35 (`jmp`).
- notes: picks one of 32 orientations from `SRANG[link]>>2`, and one of two size banks from `SRTIME[link]` bit 0, indexing `TFPIX $502A`.

### SETTIP ($4A96) TRIROT — L550
- entry: none. exit: up to three linked special-rock objects created; `NROCKS`, `SPROCK`, `HSSND`, `TRILE[PLAYR]` updated; A, X, Y, `R0`, `R1` clobbered.
- callers: `$60AD` (mainline, once per frame).
- notes: launch gating — needs `NROCKS <= DIFCTY/2`, `SROCKS >= 2` (wave 3+), `SPROCK == 0`, and either `TRILE[PLAYR] == 0` (first launch this wave) or at least 3 non-exploding objects on screen. `SETTIP_6` (`$4AAF`) scans slots 0..24 recording the *highest* free slot in `R0` while discounting exploding objects from the Y count. Then `RNDPOS` + `RNDXYI`, and `SETTIP_5` (`$4AD8`) builds three cluster members from the `ITXL/ITXH/ITYL/ITYH/ITIME/IANG` triples, each linked to control byte Y via `OBJ = (Y<<2) | $42`.

### ROTAST ($4B92) TRIROT — L705
- entry: **`FRAME` selects which of the 4 rock subroutines to rotate** (`FRAME & 3`); `ASTERS $86`..`$89` hold the 4 rotation accumulators.
- exit: one rock DVG subroutine in **vector RAM** rewritten; `R0`, `R1`, `R2`, `R3`, `R4`, `VGLIST`, `TEMP1`, A, X, Y clobbered (header `TRIROT.MAC:728`).
- callers: `$6019` START2 (once per frame, immediately after the HALT wait) · `$7152` SETROL_1 (4× at init).
- notes: **self-modifying display list.** It sets `VGLIST` from `ROCKSA[x]` (unpacking a JSRL word into a real address `$47CA`, `$4794`, …) and CPYVECs 13 vectors from `RSOURC[y]` into vector RAM. Source header: "IT MUST, HOWEVER, ONLY BE CALLED WHEN THE VECTOR GENERATOR IS HALTED SINCE IT MODIFIES THE ASTEROID SUBROUTINES." The mirror-image variety comes from `TEMP1 ^= (x<<2)&4` at `$4BB5`-`$4BB9`.

### RSAUCR ($6826) DSTRD0 — L2854
- entry: none. exit: `EDELAY` reloaded from `SEDLAY`, saucer sound `$17` stopped, then **falls into SRSAUC**. Y preserved (pushed/popped around SNDOFF), A clobbered.
- callers: `$4D49` CHKST1_62 · `$67BF` MOV_15 (saucer walked off the X edge).

### SRSAUC ($6835) DSTRD0 — L2865
- entry: none. exit: `SAUPIX`, `SAUXI`, `SAUYI`, `SAUXH`, `SAUXL` all zeroed (saucer parked off screen); A=0; X, Y preserved.
- callers: `$635D` ENEMY_15 · fall-through from RSAUCR.

---

## 12. Per-frame subsystems

### MOTION ($66F8) DSTRD0 — L2626
- entry: none. exit: `BINSCR` = the player's score in units of 10 000, capped at 15; then **falls into MOV**.
- callers: `$60B3` START2_15.

### MOV ($6713) DSTRD0 — L2651
- entry: none (fall-through only). Loops **X from `$20` (32) down to 0** over every object slot.
- exit: all active objects moved and drawn; `R9` = number of attacking specials counted by ATTACK; A, X, Y, `XCOMP..+3`, `TEMP4`, `$1C`, `$1D` clobbered.
- callers: none — fall-through from MOTION_2.
- notes: for exploding objects (`OBJ` bit 7) it advances the explosion counter and, on completion, decrements `NROCKS` (setting `RDELAY=$7F` when the last rock dies), respawns the ship (`NEWSHP`), or restarts the saucer delay. For live objects it adds `XINC`/`YINC` into the 16-bit position with sign extension, wraps X to 0..$1FFF and Y to 0..$17FF, calls `ATTACK` for special rocks, expires timed-out specials (`MOV_35` `$67C5`), and finally `PICTUR` with a scale from `SIZOPT[OBJ&3]`.

### COLIDE ($610E) DSTRD0 — L1474
- entry: none. exit: collisions resolved; `CHIST` bit 7 = "ship hit something this frame"; `CHIST+1` = `$FF` if not; A, X, Y, `TEMP1`, `TEMP1+1`, `TEMP2` clobbered.
- callers: `$60B6` START2_15.
- notes: double loop. Outer X = 7→0 over `SHPPIX,x` (slots 25..32); inner Y = `$1A`/`$19`→0. `$6133`-`$6139` prunes the pairs: objects 0..3 (ship + saucer + saucer torps) skip the saucer, and the ship skips itself. Fast reject is a high-byte subtract normalised to −6..−1 (`$6147`-`$615E`). The final test is a diamond, not a box: it compares `abs(dx)+abs(dy)` scaled against 1.5× the size sum "CHOP OFF CORNERS" (`$61DC`-`$61EC`). `COLIDE_1` (`$6122`) stashes `VGLIST` into the stack "hole" at `$0101,x` for the copyright check.

### SSZ ($61FA) — L1643: entry A = size accumulator; exit A += `$1C` (+`$08` if `SHDON` is negative). Callers `$61AE` COLIDE_35, `$61D1` COLIDE_44.
### SCRSZ ($6203) — L1654: entry A = size accumulator; exit A += `$1C`, plus `$12` more if `SAUPIX` bit 0 is clear (large saucer). Callers `$61B3` COLIDE_42, `$61CC` COLIDE_43.

### PICTUR ($6BC4) DSTRD0 — L3557
- entry: **Y = the DVG scale factor; X = object slot; `XCOMP..XCOMP+3` = the object's 16-bit X and Y position** (header L3559).
- exit: **X restored from `TEMP3`**; `VGSIZE` = Y; `VGLIST` advanced; A, Y, `XCOMP..+3` clobbered.
- callers: `$681C` MOV_30.
- notes: converts the 13-bit position to DVG coordinates by shifting X right 3 and Y right 2 with a `+128` bias, calls `POSBEM`, then `jsr PICTUR_31` and re-loads X — the comment at `$6BEA` says "THE FOLLOWING CODE ALWAYS RETURNS HERE", i.e. every path in `PICTUR_31` (rock JSRL, `TRIPIX`, `SHPPIC`, `SHPEXP`, saucer, torpedo dot) rejoins here. Torpedoes also age here: `dec OBJ,x` every 4th frame at `$6C3F`.

### POSBEM ($6C43) DSTRD0 — L3650
- entry: **`XCOMP..XCOMP+3` = beam target (XL,XH,YL,YH); `VGSIZE` = scale**.
- exit: a LABS plus one or more WAITs appended; A, X, Y clobbered.
- callers: `$6BE2` PICTUR · `$6DF3` SHPEXP_20.
- notes: the wait time is `$70 − VGSIZE`; because `VGSIZE` can exceed `$70` the subtraction underflows and the loop at `POSBEM_28` (`$6C4D`) emits repeated `$90` waits until the residue fits — a wrap-around loop that must be translated literally, not as `max(0, …)`.

### SOUNDS ($6F12) DSTRD0 — L4167
- entry: none. exit: `EXPSND $3600` and `SPTEN $3C03` written; `LEXPSND`, `THUMP1`, `THUMP2`, `LTHUMP`, `SND3`, `HSSND`, `R0` updated; A, X, Y clobbered.
- callers: `$60BC` (mainline).
- notes: `R0` bit 7 is the thrust flag, sampled by `asl THRUST / ror R0` (a read-modify-write on a read-only input port — a common Atari idiom). The extra-life (`SND3`) and high-score (`HSSND`) requests are only honoured while channel 7 (`POINT+7` = `$A9`) is idle. Thump tempo comes from `THUMP3`, which `CHKST1` ratchets down once a second.

### SHIELD ($6675) DSTRD0 — L2526
- entry: none. exit: `SHDON $73` bit 7 = shields active; `SHLDS $02EF` decremented every 4th frame while held; sound `$57` started with priority; A, Y clobbered.
- callers: `$60A4` (mainline).
- notes: `lsr SHDON` clears the flag first, then `asl HYPSW / ror SHDON` samples the button into bit 7. Blocked in attract mode, while exploding, while invisible, or at zero shield power.

### SHLDPX ($669D) DSTRD0 — L2554
- entry: `SHDON`, `SHLDS`; `VGLIST`. exit: the shield octagon appended; A, X, Y, `R0/R1`, `R2`, `TEMP1`, `TEMP1+1` clobbered.
- callers: `$6C24` PICTUR_50.
- notes: intensity = `SHLDS & $F0` with a floor of `$60`. Three CPYVEC passes from `SHLDVC $5012`: an invisible move to the corner, then 8 lit lines (intensity smuggled in through `TEMP1`), then the return move at zero intensity via a tail `jmp AYTOR0`.

### SRESET ($66CF) DSTRD0 — L2584
- entry: none. exit: `DIFCTY = 6`; **both page 2 and page 3 fully zeroed** (`sta OBJ,x` and `sta $0300,x` for x=0..255); A=0, X=0.
- callers: `$69C2` (NEWAST, when `SROCKS` reaches `$3F` in attract mode).

### MOVE ($6847) DSTRD0 — L2882
- entry: none. exit: `ANGLE` updated from the rotate switches, ship velocity updated on alternate frames; A, X, Y clobbered.
- callers: `$60AA` (mainline).
- notes: when the ship is dead it counts `SDELAY` down and calls `NEARBY`; on a clear area it sets `SHPPIX=1` (quarter-size picture), `SBTL=1` and starts sound `$47`. Rotation is ±3 per frame. Thrust is evaluated only on even frames (`lsr FRAME / bcs RTS.3`), then **falls into MOVE1**.

### MOVE1 ($6882) DSTRD0 — L2928
- entry: none (fall-through). Reads `THRUST $2405`.
- exit: `SHPXI`/`SHPYI` (velocity high) and `XINCL`/`YINCL` (velocity low) updated; A, X, `R1`, `R2`, and CACCEL/MULT's clobbers.
- callers: none — fall-through from MOVE_20.
- notes: thrust path uses `CACCEL` twice (`R2`=`$40` then `$00`); the no-thrust path (`MOVE1_80` `$68BC`) applies a linear drag of `(0−v)*4` to the 16-bit velocity.

### ENEMY ($6347) DSTRD0 — L1922
- entry: none. exit: saucer launched or its fire routine invoked; A, X, Y clobbered.
- callers: `$60B0` (mainline).
- notes: runs only on every 4th frame. If the saucer is alive it `jmp EFIRE`; if exploding it `jmp SNDOFF` with Y=`$17`; if dead it counts `EDELAY` down and, subject to `RTIMER`/`NROCKS`/`DIFCTY`, falls into `LSF`.

### LSF ($6399) DSTRD0 — L1981
- entry: none (entered by fall-through or the `bcc LSF` at `$6394`).
- exit: saucer placed on the left or right edge with a random Y (0..767), `SAUXI = ±$10`, `SAUPIX` = 1 (small) or 2 (large) chosen by `DIFCTY − NROCKS >= 4`; **tail-jumps to `SNDON`** with the sound from `SPSND−1[SAUPIX]`.
- callers: `$6394` ENEMY_5.
- notes: the random Y is built by rotating three random bits through `SAUYL`, so `SAUYL` is a scratch register here.

### EFIRE ($63E6) DSTRD0 — L2036
- entry: none. exit: `SAUYI` re-randomised every 128 frames from `EFIRE_99`; a torpedo launched via `jmp FIRE1`; `ANGLE+1 $7A` = the firing angle; A, X, Y, `R2`, `R3`, `R8`, `TEMP2`, `$0F` clobbered.
- callers: `$635A` ENEMY_16 (`jmp`).
- notes: target choice — in attract mode always a rock; otherwise `$AA/256` (small saucer) or `$40/256` (large) of the time the ship, else `LFCR`. **If the ROM checksum accumulator `$8C` is non-zero the saucer shoots wild** (`$6414`-`$6416`) — a tamper trap. Aim error comes from `EFIRE_97/98`, with the tighter row selected when the score exceeds 60 000.

### FIRE ($64BE) DSTRD0 — L2218
- entry: none. exit: on a fresh button edge, sets `R8=0`, `$0F=3`, X=`$19`, `$7A=ANGLE`, Y=7 and falls into FIRE1.
- callers: `$60A7` (mainline).
- notes: debounce is `asl FIRESW / ror LASTSW` then `bit LASTSW` — bit 7 = now, bit 6 = last frame. Blocked in attract mode and while shields are up.

### FIRE1 ($64E4) DSTRD0 — L2239
- entry: **Y = highest torpedo slot to try; `$0F` (`TEMP3+1`) = the stop index; X = the object whose velocity is inherited; `R8` = 0 (ship) or 1 (saucer); `$7A` = firing angle.**
- exit: falls into FIRE3 on the first free slot, or returns via `FIRE2 $64EE`.
- callers: `$6477` EFIRE_91 (`jmp`, Y=3, `$0F`=1 → slots 3,2 = saucer torpedoes 27,28) · `$64EC` (own loop; from FIRE Y=7, `$0F`=3 → slots 7..4 = ship torpedoes 29..32).

### FIRE2 ($64EE) — L2247: a shared RTS. Callers `$64C0`, `$64C4`, `$64CD`, `$64CF`, `$64D4` (all FIRE) and the FIRE1 loop exit.

### FIRE3 ($64EF) DSTRD0 — L2251
- entry: **Y = the torpedo slot, X = the object whose velocity to inherit, `R8` = 0/1 for ship/saucer position, `$7A` = angle.**
- exit: torpedo `SHPPIX[Y]=$12` (12-frame life), velocity and start position set; tail-jumps to `SNDOO` with Y=`$27` (player) or `$1F` (saucer).
- callers: `$64E7` FIRE1.
- notes: **quirk worth preserving** — for the saucer, X is the *target's* slot, so the torpedo inherits the target's velocity (a crude lead). For the player X is `$19` = the ship. Velocity is `cos/sin(angle)/2 + object velocity`, clamped to `$91`..`$6F`. The muzzle offset is 1.5× the direction vector added to `SHPXL,R8`/`SHPYL,R8` (i.e. `SHPXL` or `SAUXL`). `$0A` and `$0D` hold the half-components across the two axes.

---

## 13. Attract mode / high scores / state machine

### NEWAST ($698E) DSTRD0 — L3148
- entry: none. exit: a new rock wave activated (`NROCKS` from `RWAVE[min(SROCKS,3)]`), all `SRTIME` entries cleared, `SPROCK`=0, `TRILE[PLAYR]`=0, `EDELAY=$7F`, `THUMP3=$30`, unused slots cleared; A, X, Y, `TEMP1` clobbered.
- callers: `$4C97` CHKST_12 (2-player start) · `$6011` START1.
- notes: refuses while `RDELAY` is running, while the ship is invisible/exploding, or while a saucer is alive. `DIFCTY` ratchets up to `$0A`. In attract mode, once `SROCKS` hits `$3F` it calls `SRESET`.

### PARAMS ($6A9A) DSTRD0 — L3352
- entry: none. exit: the whole HUD appended to the display list; A, X, Y, `VGSIZE`, `R3` and all callee clobbers.
- callers: `$60B9` START2_20.
- notes: draws player-1 score (flashing while player 1 waits to enter), lives via `GTSP`+`LIVES`, high score + initials via `DIGITS`+`IN3TIM`, the BONUS message via `VGME` (English only), and player-2 score/lives. Emits a `JSRL $C381` copyright shape at `$6AA2` only when a game is in progress.

### SCORES ($6CB9) DSTRD0 — L3752
- entry: none. exit: **C = 1 if the high-score table was displayed** (so the mainline at `$609E` skips the attract-mode gameplay), C = 0 otherwise; A, X, Y, `VGSIZE`, `R0`, `R1`, `R2`, `R3` clobbered.
- callers: `$609B` (mainline).
- notes: only in attract mode, and only while `$77 & $04` is clear (a ~2-second alternation driven by the frame counter's high byte). Iterates the 10 entries at `HSCORE $23` (3 bytes each) until it finds an all-zero one; draws rank (BCD in `R0`), score, three initials, and for ranks 1-3 also `LIVESS` ship icons.

### UPDATE ($6FF0) DSTRD0 — L4323
- entry: none; acts only when `NPLAYR` is negative (`$FF`, the one frame after a game ends).
- exit: high-score table shifted down and the new score inserted; `UPDFLG`/`$43` = table indices of the two players (or `$FF`); `EAHSX` = the lowest index touched; `EABC`/`EAX` prepared for the EAROM write; `NPLAYR`=0; `UPDINT`=0; `$77`=`$F0` (60-second initial-entry timeout).
- callers: `$6093` (mainline).
- notes: compares three-byte BCD scores; the "divide by 3" at `UPDATE_33` (`$7070`) is a repeated-subtraction loop that converts a byte offset into an entry number, then `[3−n]*7` gives the EAROM byte count. `UPDATE_97` (`$704A`) runs the copyright verification (`VGRCPT`) and, if `CRMERR` is set, awards two free credits one time in three — a deliberate tell-tale for bootlegs.

### GETINT ($6587) DSTRD0 — L2353
- entry: none. exit: **A negative (`$FF`) = not entering initials (caller falls through to `SCORES`); A = 0 = entry in progress (caller at `$6099` branches to `START2_20`, skipping gameplay)**; `INITL`, `UPDINT`, `UPDFLG`, `PLAYR`, `$77` updated.
- callers: `$6096` (mainline).
- notes: rotate-left/right change the letter (with wrap through blank↔A↔Z), hyperspace/shield accepts. `SBANK` is called to swap the control bank to the player being prompted, and again on exit to restore player 1. Timeout at `$77 == 0` aborts entry.

### CHKST ($4BD1) DSTRD0(parked) — L745
- entry: none. exit: **C = 1 if a new game is starting** (caller at `$6091` branches to `START2_50` which restarts the wave). All ZP game state re-initialised on that path.
- callers: `$608E` (mainline).
- notes: in free play it force-feeds 2 credits. `$4C0B`-`$4C26` converts the credit count to BCD by shift-and-add in decimal mode. `CHKST_CRAPOUT` at `$4C02` is a deliberate infinite loop (`bcs` to itself) if `CRDT >= $40` — a corrupt-RAM trap. Starting a 2-player game switches `BNKSEL`, runs `INIT`/`NEWAST`/`NEWSHP` for player 2, then falls into the 1-player path for player 1.

### CHKST1 ($4CFE) — L965
- entry: none (game in progress, not in "player ready" mode).
- exit: C = 0 always; may swap the active player (`PLAYR`, `PLAYR2`, `PLAYR3`, `BNKSEL`), display GAME OVER, or set `NPLAYR = $FF` to trigger the high-score update.
- callers: `$4BD9` CHKST (`jmp`).
- notes: also ratchets `THUMP3` down by 1 per second to a floor of 8.

### BM ($4CEF) — L952
- entry: **Y = message number**. exit: displays the message unless `TWOCM`/`R3` indicate a blink-off phase (`FRAME & $20`); returns via the shared `RTS.4 $4CE5`.
- callers: `$4C6D` CHKST_21, `$4C79` CHKST_12.

### CHKST2 ($6102) DSTRD0 — L1461
- entry: none (reads `PLAYR`). exit: "PLAYER n" drawn; tail-jumps to `VGHEX`.
- callers: `$4BDE` CHKST_60 · `$4D2D` CHKST1.

### INIT ($7BF5) CASTST — L6099
- entry: none. exit: waits for the VG to halt, calls `SETROL` (which clobbers `FRAME`), writes a HALT at `$4003`, clears both players' 3-byte scores, clears the whole current bank page, seeds `SEDLAY/EDELAY=$98`, `RDELAY=$7F`, `DIFCTY=6`, `THUMP3=$30`, `UPDFLG/$43=$FF`, then **falls into SINIT**.
- callers: `$4C94` CHKST_12 · `$4CB1` CHKST_20 · `$6000` START · `$7BF8` (its own HALT spin).

### SINIT ($7C37) CASTST — L6138
- entry: none. exit: `PKYTST` run (sets `RVELP`/`RVELM`); bonus-life plateau seeded from `BONUS[OPTN1 & 3]` into `$69/$6A`, `$6C/$6D` and `BONSCR+1/+2` (`$FE/$FF`); `NHITS` computed from `OPTN3` (+1 if no bonus, +1 if 2-coin play); tail-jumps to `INISOU`.
- callers: `$7EFE` STEST6_28 (self-test) · fall-through from INIT.

### SBANK ($6CB1) DSTRD0 — L3740
- entry: **`PLAYR` = 0 or 1**. exit: `BNKSEL $3C04` written with `$00`/`$80`; **C = positive on return** (header `DSTRD0.MAC:2502`); A clobbered.
- callers: `$65B6` GETINT_25 · `$6625` GETINT_51 (`jmp`).

---

## 14. NMI, coins, EAROM, self-test

### NMI ($785C) DSTNMI — L5276
- entry: hardware NMI vector `$FFFA`. Fires every 4 ms.
- exit: `rti` at `COINC_1 $79E7` after `COINC_16` pulls X, Y, A.
- callers: hardware only.
- notes: **`bit $01FF / bpl` gate at the top** — if the byte above the stack is negative, the NMI returns immediately (power-up self-test suppression). `STEST6` clears it at `$7E66` to enable interrupts. Checks stack overflow/underflow by requiring `$01FF | $01D0 == 0` and *hangs deliberately* at `NMI_2 $786E` if not. Increments `INTCT`, and every 4th interrupt increments `SYNC` — if `SYNC` reaches 3 the mainline has fallen behind and it hangs at `NMI_HANG` with the watchdog left to reset the board. Reads the coin DIP `OPTN5` into `CMODE` while in attract mode. Then `jsr CSOUND` and **falls into MOOLAH**.

### MOOLAH ($78A9) DSTNMI — L5341
- entry: fall-through from `NMI_3`; no register inputs. Loops X = 2→0 over the three coin mechs.
- exit: `CRDT`, `CNCT`, `BCCNT`, `BC`, `CNSTT`, `PSTSL`, `CCTIM`, `LMTIM` updated; control continues into `$BONUS` → `$CNVRT` → `$EXT` → `COINC` → `rti`.
- callers: none (fall-through).
- notes: this is stock DCIN65.MAC. Debounce is a packed counter in `CNSTT,x`: bits 0-4 count coin-on samples down, bits 5-7 count coin-off samples up. Slam (`LAM $2006`) loads `LMTIM=$F0` and voids pending coins. Mech multipliers come from `CMODE` bits; the bonus adder uses `MODULO[mode]`.

### COINC ($79A3) DSTNMI — L5574
- entry: `CCTIM $97..$99` = the three counter timers, `LOUT1 $85` = lamp code, `LMTIM` = slam timer.
- exit: `CCLFT/CCMID/CCRIT` and `SLMP1/SLMP2` written; POKEY tone started if slammed; then pulls X, Y, A and `rti`.
- callers: `$797A`, `$7991`, `$799E` (all inside `$EXT`).
- notes: contains the second tamper trap — if `CKERR` is non-zero, the stack "hole" is armed, and a game is in progress, it **ORs `$08` into the saved status byte on the stack (`$0104,x`), so the interrupted code resumes in decimal mode** and the game corrupts itself.

### EAUPD ($7B11) EAROM — L5928
- entry: none; state lives in `EAFLG $D4` (0 = idle, `$80`+ = erase, `$40` = write, `$20` = read), `EAX $D2` (EAROM address), `EABC $D3` (byte counter), `EAHSX $D5` (high-score entry index), `EACS $D6` (checksum), `EABAD $D7`.
- exit: one EAROM byte processed; A, X, Y clobbered.
- callers: `$608B` (mainline, once per frame) · `$7F5D` STEST6_11 (self-test).
- notes: throttled to one byte per 16 interrupts (`INTCT & $0C`). Walks the `EABDS $7BD2` table (`00 01 02 21 22 23 FF`) which alternates between the 3 score bytes and the 3 initial bytes for each entry; the `FF` terminator triggers the checksum byte. On a read checksum mismatch it blanks that entry's `HSCORE`/`INITL` in RAM and sets `EABAD`. The self-test caller fakes `INTCT += 4` at `$7F60` because NMIs are off in self-test.

### STEAROM ($7BD9) EAROM — L6073
- entry: `EAHSX` = lowest entry index to write; `EAFLG` must be 0.
- exit: `EAFLG` bit 7 set (starts the erase→write sequence); if `EABAD` is set it widens the operation to the whole 21-byte ROM (`EABC=$15`, `EAX=$14`) and clears `EAHSX`/`EABAD`; Y clobbered.
- callers: `$4CEC` RTS.4_42 (`jmp`) · `$6622` GETINT_54.

### VGRCPT ($77F5) DSTMSG — L5197
- entry: `CPMTST $90` must be negative for the check to run (`UPDATE_97` sets it with `sec / ror CPMTST` at `$704E`).
- exit: `CRMERR $91` accumulates any mismatch; `VGLIST` saved and restored across the call; A, X, Y clobbered.
- callers: `$7050` UPDATE_97.
- notes: pretends to rebuild the copyright display list at `$4702` but instead XORs the expected bytes (`CPR.DT $783D` plus the unpacked `ASTMT $7845` text) against what is actually in vector RAM. A bootleg that edited the copyright leaves `CRMERR` non-zero, which later hands out free credits.

### CPR.UP ($7835) DSTMSG — L5242
- entry: **A = expected byte, Y = offset from `VGLIST`.**
- exit: `CRMERR |= (A ^ vgram[VGLIST+Y])`; Y incremented; A clobbered.
- callers: `$720E`, `$7214` VGMSG2_12 · `$780D`, `$7811` VGRCPT_1 · `$7819` VGRCPT_2.

### INISOU ($784F) DASOUN (linked among DSTNMI) — L5264; `ST1` in the linker map
- entry: none. exit: all 8 `POINT` entries zeroed (all sound channels silenced) and `AUDCTL $2C08` written with 0; A=0, X=`$FF`.
- callers: `$6591` GETINT (attract mode) · `$6FFC` UPDATE · `$7C6B` SINIT (`jmp`).

### PKYTST ($7FC1) DSTTST — L6835
- entry: none. Uses `FRAME & 3` as an index into the 4-entry history at `$DC..$DF`.
- exit: **`RVELM $E2` and `RVELP $E1` set to the minimum rock velocities** — normally `$F6`/`$0A`, or `$FB`/`$05` ("MAKE THE GAME EASY") when `OPTN5` bit 0 is set; `PERR $DB` = the POKEY random value / error code; A, X, Y clobbered.
- callers: `$60C8` (mainline, every frame) · `$7C37` SINIT.
- notes: samples POKEY's `RANDOM $2C0A` once per frame and compares against the last three samples; four identical readings means POKEY is dead. Note it doubles as the rock-speed initialiser, so it is not optional.

---

## 15. ROM data tables read by 6502 code

| name | address | length | read by |
|---|---|---|---|
| `AVEL` | `$499C` | 7 | `ATTACK_17 $495B` — angular velocity vs. attacker count |
| `ITXL` | `$4B2B` | 3 | `SETTIP_5 $4AF2` — cluster X offset low |
| `ITXH` | `$4B2E` | 3 | `SETTIP_5 $4AFC` |
| `ITYL` | `$4B31` | 3 | `SETTIP_5 $4B05` |
| `ITYH` | `$4B34` | 3 | `SETTIP_5 $4B0F` |
| `ITIME` | `$4B37` | 3 | `SETTIP_5 $4AE6` — initial `SRTIME` (`83 85 81`) |
| `IANG` | `$4B3A` | 3 | `SETTIP_5 $4AEC` — initial `SRANG` (`F0 98 40`) |
| `SINCOS` | `$4B51` | 65 | `SIN1_10 $714A` — sin(0°..90°) × 127 |
| `HITSCR` | `$4D7D` | 3 | `SPLIT_15 $6FAD` — `10 05 02` = 1000/500/200 pts for small/medium/large rock (`DSTRD0.MAC:3023`) |
| `EXPPIC` | `$4D80` | 8 | `PICTUR_31 $6BFA/$6BFD` — 4 JSRL words, rock-explosion sizes |
| `EXP16` | `$4D88` | 224 | `SHPPIC` twinkle, via `R0/R1 = $4D87` + `AYTOR0`/`CPYVEC` |
| `SAUCER` | `$4E68` | 410 | DVG only (JSRL `$734`), never read by the 6502 |
| `RSOURC` | `$5002` | 16 | `ROTAST $4BA7/$4BAC` — 8 pointers to the rock source vector lists. **Only 8 entries; CPXROT can return index 16** (see CPXROT notes) |
| `SHLDVC` | `$5012` | 24 | `SHLDPX_1 $66B1` — source vectors for the shield octagon (`R0/R1 = $5012`) |
| `TFPIX` | `$502A` | 856 | first `$40` bytes = 32 diamond JSRL words, read by `TRIPIX $4A8D/$4A90`; `TFPIX+$40` = `$506A`, read by `SHPEXP_20 $6DFA/$6E00` for explosion-fragment vector lists; the remainder is DVG vector data |
| `CPMGL` | `$5382` | 8 | DVG only |
| `ASTMSG` (shape) | `$538A` | 50 | DVG only (JSRL `$9C5`) |
| `SHIPS` | `$53BC` | 9 | `CPTSPX $6ECE` — byte offsets of the 9 ship orientations within `SHIPSV` |
| `SHIPSV` | `$53C6` | 386 | source vectors for the ship hull, via `CPTSPX` → `R0/R1` → `CPYVEC` |
| `SHIP17` | `$5548` | 204 | DVG only; JSRL constant `$A4,$CA` emitted by `LIVESS_10 $7C85` |
| `VGSPAC` | `$5614` | 44 | DVG only; JSRL constant `$0A,$CB` emitted by `VGBLNK $7CC8` |
| `Z.5` | `$5640` | 112 | DVG only; JSRL constant `$20,$CB` emitted by `CHKST_18 $4C40` |
| `UNDERL` | `$56B0` | 72 | DVG only; JSRL emitted by `INITAL $66EE` |
| `VGMSGA` | `$56F8` | 76 | `VGCHAR $7A10/$7A13` (indexed by charcode×2) and `VGMSG2_10 $71FB/$7205` (indexed from `VGMSGA−2 = $56F6`) — 38 glyph JSRL words |
| `TEST1` | `$5744` | 136 | DVG only; JSRL `$57,$44` from `STEST6_35 $7EA5` |
| `BNKERR` | `$57CC` | 28 | DVG only; JSRL `$57,$CC` from `STEST6_90 $7E70` |
| `PKYERR` | `$57E8` | 10 | DVG only; JSRL `$F4,$CB` from `STEST6_27 $7F21` |
| `ERASE` | `$57F2` | 14 | DVG only; JSRL `$57,$F2` from `STEST6_11 $7F56` |
| `SPSND` | `$63E4` | 2 | `LSF_40 $63DD` (as `$63E3,y`, y=1 or 2) — `$17` small / `$37` big saucer sound |
| aim mask | `$647A` (`EFIRE_97`) | 2 | `EFIRE_90 $6461` — `8F 87` |
| aim sign-ext | `$647C` (`EFIRE_98`) | 2 | `EFIRE_90 $6466` — `70 78` |
| saucer Y-vels | `$647E` (`EFIRE_99`) | 4 | `EFIRE $63F1` — `F0 00 00 10` |
| `SIZOPT` | `$6822` | 4 | `MOV_25 $6818` — DVG scale per rock size (`00 E0 F0 E0`) |
| `ACCEL` | `$6918` | 8 | `CACCEL_1 $68F6` — thrust magnitude vs. current speed |
| `RWAVE` | `$69FF` | 4 | `NEWAST_21 $69CE` — rocks per wave (`6 7 8 9`) |
| `TABLE` (CPXROT control) | `$6F0A` | 8 | `CPXROT $6EEA` — `00 A3 22 81 03 A0 21 82` |
| `TSS` | `$6F7C` | 2 | `SOUNDS_30 $6F63` — thump sound numbers `07 0F` |
| `ATANA` | `$710F` | 17 | `ATAN3 $710B` — arctangent lookup, 0..$20 |
| `VGMSGS` | `$7219` | 28 | `VGME_10 $71AF/$71B2` — 14 screen (X/4, Y/4) positions, one per message |
| `VGMSGT` | `$7235` | 8 | `VGME $719A/$719F` — 4 language table base pointers (`$723D`, `$72F3`, `$73C2`, `$747D`) |
| `L0`..`L3` | `$723D` | 751 total | indirect via `TEMP1` in `ASTMSG` — each language block starts with a 14-byte per-message offset table, then packed 5-bit text |
| `ASTM` | `$7603` | 10 | `CPYRS $7185`/`SETROL_1 $716C` set `TEMP1` to it; unpacked by `ASTMSG` — packed "1#0 ATARI INC" |
| `DASOUN`/`PNTRS` | `$760D` | 325 | `$760D`–`$766C` = 12 sounds × 8 channel offsets, read by `SNDOO_5 $776B`; `$766D`–`$7751` = 4-byte sequence records, read by `CSOUND` at `$779C`, `$77A8`, `$77AD`, `$77B2`, `$77D2` |
| `RADDR`/`BUFA` | `$77DE` | 4 | `START2_10 $6029` and `$6035` — the two DVG buffer JMPL words (double buffering) |
| `ROCKSA` | `$77E2` | 8 | `ROTAST $4BBB/$4BC1` (as a *destination* address, unpacked into `VGLIST`) and `PICTUR_37 $6C1B/$6C1E` (as a JSRL to emit) — 4 rock-subroutine JSRL words |
| `MODULO` | `$77EA` | 8 | `$BONUS $7945` — coins per bonus unit, indexed by `CMODE` bits 5-7 |
| `FILL` | `$77F2` | 3 | never read — DSFILL.MAC decoy `adc #15 / rts` |
| `CPR.DT` | `$783D` | 8 | `VGRCPT_2 $7816` — expected copyright display-list bytes |
| `ASTMT` | `$7845` | 10 | `VGRCPT_1 $7823/$7827` sets `TEMP1` to it for the checked unpack |
| `EABDS` | `$7BD2` | 7 | `EAUPD_5 $7B42` and `EAUPD_10 $7B8D` — `00 01 02 21 22 23 FF`, offsets from `HSCORE`, terminator `$FF` |
| `BONUS` | `$7C6E` | 4 | `SINIT $7C40` — bonus-life plateau (`00 20 50 FF`, `FF` = none) |
| `ROMX` | `$7FEE` | 6 | `STEST6_32 $7E92` — glyph indices for the ROM designators in self-test |
| `ROMY` | `$7FF4` | 6 | `STEST6_32 $7E9A` |
| checksum bytes | `$49A3`, `$53C5`, `$6101`, `$6920`, `$7120`, `$7D81` | 1 each | never read as data; summed by `STEST3` and by the running background checksum in `START2_12` |
| `VCTRS` | `$7FFA`–`$7FFF` | 6 | hardware: NMI `$785C`, RESET `$7CD7`, IRQ/BRK `$7CD7` |

---

## 16. Things that will bite a C translation

1. **Stack-unwinding exits.** `BOUNCE_14` (`$754A`) and `VGMSG2` (`$71F0`) both `pla/pla` to return *past* their caller. These are not `return` statements — model them as explicit status codes or loop structure.
2. **Fall-through chains.** `MOTION→MOV`, `MOVE→MOVE1`, `RSAUCR→SRSAUC`, `SHPPIC→THRTST→AYTOR0`, `BLOUP_10→SSBTLT`, `LIVES→LIVESS`, `BONDSP→DIGITS`, `VGSABS→VGLABS→VGADD`, `VGAWT→VGWAIT→VGDOT`, `VGBLNK→VGADD2`, `INIT→SINIT`, `COS→SIN`, `SNDOFF/SNDPON/SNDON→SNDOO`, `NMI_3→MOOLAH→$BONUS→$CNVRT→$EXT→COINC`, `CPYRS→ASTMSG`, `CM→ASTMSG`, `VGMSG→VGME→CM`.
3. **Tail calls that inherit the caller's return address**: `GPTS→POINTS`, `SCALER→ATAN`, `SPLIT→SPLTTP`, `CPYVEC→VGADD`, `AYTOR0→CPYVEC`, `TRIPIX→VGADD2`, `SINIT→INISOU`, `LSF→SNDON`, `FIRE3→SNDOO`, `ENEMY→EFIRE`/`SNDOFF`, `CHKST→CHKST1`.
4. **Self-modifying display list.** `ROTAST` rewrites the rock subroutines inside vector RAM `$4000-$47FF` and may only run while the DVG is halted.
5. **Read-modify-write on read-only input ports** (`asl FIRESW`, `asl HYPSW`, `asl THRUST`, `rol $COINA,x`) — these read the port and discard the write; the point is getting bit 7 into carry.
6. **Bank switching.** `BNKSEL $3C04` bit 7 swaps pages 2 and 3. `GTSP` deliberately reads `$0319` to see the *other* player's ship, and `SRESET`/`STEST3_25` touch both pages explicitly.
7. **Flag-only return values.** `CHKST` (C), `SCORES` (C), `BOUNCE` (C), `NEARBY` (Z), `SEARCH`/`SEARC1` (N), `GETINT` (N via A), `VGHEXZ` (C).
8. **Decimal mode.** `POINTS`, `CHKST_18`, `SCORES_20` all use `sed`/`cld` around BCD arithmetic. The `COINC` tamper trap works by leaving decimal mode *on* in the restored processor status.
9. **Page-1 data above the stack.** `SXPXL/SXPYL/SXPXH/SXPYH/EXPTMB` at `$0100`-`$0127`, and the copyright "hole" at `$0101,HOLE`, share the page with the 6502 stack (which is moved to `$01FC` at `$6083`).
10. **`RANDOM $2C0A` is hardware.** `CPYPOS`, `RNDPOS`, `NEWVEL`, `RNDXYI`, `SHPEXP`, `SHPPIC`, `EFIRE`, `LSF`, `PKYTST` all depend on it; it is not reproducible from a software PRNG without matching POKEY.
11. **`CPXROT` index 16 vs. `RSOURC`'s 8 entries** — a genuine ROM off-by-one, reproduced by reading the table through `ad_rom()`.
12. **`FIRE3` gives saucer torpedoes the *target's* velocity**, not the saucer's — deliberate-looking lead, easy to "fix" by accident.
