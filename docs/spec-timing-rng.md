I have extracted the complete spec from both disassemblies. They match exactly; the kaspermeerts version has the clearest labels/comments, meithecatte confirms every value with named constants. Below is the dense spec with hex values and `file:line` citations.

Citation shorthand:
- **KM** = `/home/thlira/repos/gameboy-tetris-pc-port/reference/kaspermeerts/tetris.asm`
- **MC** = `/home/thlira/repos/gameboy-tetris-pc-port/reference/meithecatte/bank_000.asm`

---

# Game Boy Tetris — Exact Gameplay Spec

## 0. Relevant state variables (HRAM)
Addresses from `reference/kaspermeerts/hram.asm`.

| Symbol | Addr | Meaning |
|---|---|---|
| `hDropTimer` | `$FF99` | Gravity/soft-drop countdown (frames until next cell drop) |
| `hFramesPerDrop` | `$FF9A` | Reload value = current level's gravity (from table) |
| `$FF98` (`hLockdownStage`) | `$FF98` | Lock state machine: 0 idle, 1 transfer-to-tilemap, 2 look-for-lines, 3 blink |
| `$FF9B` | `$FF9B` | Collision flag (set by DetectCollision) |
| `$FF9C` (`hBlinkCounter`) | `$FF9C` | Line-clear blink phase 0..6 |
| `hLines` | `$FF9E`/`$FF9F` | BCD line count, low byte `$FF9E`, high byte `$FF9F` |
| `$FFA0` | `$FFA0` | scratch (saved Y/X before a move) |
| `hTimer1` | `$FFA6` | Frame timer (line-clear/ARE delays) |
| `hTimer2` | `$FFA7` | Soft-drop rate countdown |
| `hLevel` | `$FFA9` | Current level (binary 0..20) |
| `hKeyRepeatTimer` | `$FFAA` | DAS timer for L/R |
| `hNextPreviewPiece` | `$FFAE` | Hidden "next-next" piece (RNG lookahead) |
| `hWipeCounter` | `$FFE3` | Post-clear board-redraw / falling animation step |
| `hSoftDropCounter` | `$FFE5` | Cells soft-dropped this piece (for scoring) |
| `hHeartMode` (`hStartAtLevel10`) | `$FFF4` | Heart mode: +10 to level for gravity |
| `hTempPreviewPiece` | `$FFFB` | RNG scratch / also spawn-lock counter (top-out) |
| active piece OAM | `$C200`(id) `$C201`(Y) `$C202`(X) `$C203`(rot) | |
| preview piece | `$C210`+3 (id at `$C213`) | |

Spawn position constants: `INITIAL_TETROMINO_Y = $18`, `INITIAL_TETROMINO_X = $3F` (`reference/meithecatte/constants.asm:201-202`).

---

## 1. Piece RNG

Source of randomness: **`rDIV`** (DIV timer register, increments at 16384 Hz). No LFSR. Two identical routines exist: `PickRandomPiece` (KM:1565, used to pre-warm the sequence) and the in-game `NextPiece.randomChoice` (KM:5116). meithecatte equivalent MC:2579-2622.

### 1a. Raw value → candidate piece ID
KM:5116-5132 / MC:2579-2596:
```
h = 3                     ; attempt counter (max 3 tries = up to 2 rerolls)
.tryAgain:
  b = [rDIV]              ; raw random byte
.wrap:
  a = 0
.loop:
  b = b - 1
  if b == 0 -> .checkRandomizer
  a += 4                  ; done as 4× INC A (KM:5126-5129), NOT ADD A,4
  if a == 7*4 (=$1C=28) -> .wrap   ; wrap at 28
  goto .loop
```
Net effect: **`a = (DIV mod 7) * 4`** — a value in `{0,4,8,12,16,20,24}`. Low 2 bits are the rotation state (0). `SPRITE_ID_ROTATION_MASK = $03`, first non-tetromino id `SPRITE_TYPE_A = $1C` (`meithecatte/constants.asm:110,140`). So there are 7 pieces × 4 rotations = 28 ids; the 7 spawnable base ids are the multiples of 4 from 0 to 24. Shape/tile for each id comes from the sprite table at ROM `$2C20` (`reference/kaspermeerts/sprites.asm:1-3`), entry = `$2C20 + id*4`.

### 1b. Anti-repeat reroll (accept/reject)
KM:5144-5157 / MC:2598-2615. Registers at this point: `c` = the piece just moved to the top of the field (current preview, `& ~$03`); `d` = candidate for the new hidden next-next; `e`/`a` = current `hNextPreviewPiece` (becomes the visible preview).
```
.checkRandomizer:
  d = a                        ; candidate next-next
  e = [hNextPreviewPiece]      ; will become the shown preview
  h = h - 1
  if h == 0 -> accept          ; 3rd try always accepted
  a = (candidate | hNextPreviewPiece | c) & ~$03
  if a == c -> .tryAgain       ; reject, reroll
accept:
  hNextPreviewPiece = d
  preview($C213) = e
```
- **Rerolls: up to 2** (h counts 3→2→1; the 3rd candidate is force-accepted).
- Reject test: `OR` of {candidate, current next-preview, active piece id}, masked with `~$03`, compared for equality against the active piece id `c`. If equal, reject. (Both disassemblies note the `& ~$03` mask is effectively a no-op since all three are already multiples of 4; and the comment flags this differs from the "official" Tetris random-generator description.)
- State compared against: the **active piece** (`c`) and the **current next-preview** (`hNextPreviewPiece`) — a 2-deep history, OR-combined.

At startup the sequence is primed by calling the picker 3× then 256× more (KM:995-1001) and NextPiece 3× (KM:4203-4205).

Determinism: in demo/multiplayer (`hDemoNumber`≠0 or `hIsMultiplayer`≠0) RNG is bypassed and pieces come from `wPieceList` indexed by `hNumPiecesPlayed`, wrapping after 256 (KM:5090-5114).

---

## 2. Gravity — frames per drop

Lookup: `LookupGravity` (KM:4240-4260) / MC `Call_000_1b43` (MC:1531-1555). Index `e = hLevel`; in **heart mode** (`hHeartMode`≠0) `e = min(hLevel + 10, 20)`. Table capped at level 20.

**Table `FramesPerDropTable` @ ROM `$1B61`** (KM:4262-4283, label at KM:4262; MC table pointer `$1b61` at MC:1549). Values (frames per one-cell drop):

| Level | Frames | Level | Frames |
|---|---|---|---|
| 0 | **52** | 11 | 8 |
| 1 | 48 | 12 | 7 |
| 2 | 44 | 13 | 6 |
| 3 | 40 | 14 | 5 |
| 4 | 36 | 15 | 5 |
| 5 | 32 | 16 | 4 |
| 6 | 27 | 17 | 4 |
| 7 | 21 | 18 | 3 |
| 8 | 16 | 19 | 3 |
| 9 | 10 | 20 | **2** |
| 10 | 9 | (>20) | capped at level 20 = 2 |

Full byte sequence: `52,48,44,40,36,32,27,21,16,10,9,8,7,6,5,5,4,4,3,3,2`.

- **A-type & B-type share this one table**, indexed by `hLevel`. There is no separate B-type gravity table; B-type "height" (0-5) only adds garbage rows, not gravity (see §7). B-type piece init transiently sets `hDropTimer = $34` (=52) for the first drop (KM:4212-4213).
- Reload: when `hDropTimer` reaches 0 it is reloaded from `hFramesPerDrop` (= table value) (KM:5218-5219).

---

## 3. Soft drop (hold Down)

`DownHeld` (KM:5167-5191) / MC `Gameplay_HoldingDown` (MC:2624-2654).

- Repeat rate: **`hTimer2 = 3` frames between soft-drop steps** (`FASTDROP_RATE EQU 3`, `meithecatte/constants.asm:199`; KM:5187-5188).
- Each soft-drop step increments `hSoftDropCounter` (KM:5189-5190) then falls through to the normal `DropPiece.drop` (moves piece down 8px = one cell).
- Requires Down held **without** Left/Right (`hJoyHeld & (DOWN|LEFT|RIGHT) == DOWN`, KM:5195-5198).
- A per-piece guard flag `$C0C7` (`wDidntUseFastDropOnThisPiece`) forces the player to release and re-press Down before soft-dropping a freshly spawned piece (KM:5168-5176, MC:2626-2636).
- Scoring: on lock, soft-drop cells add points (`hSoftDropCounter - 1` cells; KM:5237-5257), then counter cleared. (Type-A/B use different score paths, KM:5241-5299.)

**Gravity counter reset on move:** NO.
- `hDropTimer` (gravity) is only reloaded to `hFramesPerDrop` when it decrements to 0 (KM:5202-5219, MC:2665-2684). Horizontal shifts (§4) and rotations do not touch it.
- Soft drop does **not** reload the natural gravity timer either: the soft-drop path jumps straight to `.drop`/`.apply_gravity` and only resets its own `hTimer2`/`hFastDropDelayCounter` (KM:5191, MC:2650-2654). Natural gravity keeps counting independently.

---

## 4. DAS (delayed auto-shift) — Left/Right

`RotateAndShiftPiece` → `.shiftPiece` (KM:5965-6028) / MC constants `AUTOFIRE_DELAY EQU 23`, `AUTOFIRE_RATE EQU 9` (`meithecatte/constants.asm:197-198`).

| Event | `hKeyRepeatTimer` set to | Behavior |
|---|---|---|
| Fresh press (`hJoyPressed` bit set) | **23** | Move 1 cell immediately, load 23 (KM:5974, 6008) |
| Held, timer counts down to 0 | **9** | Move 1 cell, reload 9 (KM:5982-5984, 6016-6018) |
| Move blocked by collision | **1** | Cancel move, retry next frame (KM:6001-6003, 6028) |

- **Initial delay: 23 frames. Repeat: 9 frames.** Symmetric for both directions.
- Right checked before Left; only one direction acts per frame.
- SFX `wNewSquareSFXID = $04` on successful shift (KM:5989-5990).

---

## 5. Lock and entry delay (ARE)

Lock is a state machine on `$FF98` (`hLockdownStage`), advanced once per gameplay frame by the fixed call chain in `GameState_00` (KM:4414-4419):
`RotateAndShiftPiece → DropPiece → CheckForCompletedRows → LockPieceIntoBackground → MoveBlocksDownAfterLineClear`.

### Lock trigger — the frame a piece can't drop
`DropPiece.drop` (KM:5220-5236): moves piece down 8px, `DetectCollision`; if it collides it **reverses the move and immediately sets `$FF98 = 1`** and `$C0C7 = 1`. **No lock delay** — lock begins the same frame the drop fails (MC:2686-2703 identical). There is no "lock timer" / no reset-on-move lock extension.

### Stage progression
| `$FF98` | Set at | Action that frame |
|---|---|---|
| 1 = TRANSFER_TO_TILEMAP | KM:5235 (`DropPiece`) | `LockPieceIntoBackground` (KM:6068-6107): writes the 4 piece cells into the BG tilemap during HBlank, hides active OAM (`$C200=$80`), sets `$FF98 = 2` |
| 2 = LOOK_FOR_FULL_LINES | KM:6104 | `CheckForCompletedRows` (KM:5301-5401): scans rows for full lines, records them, sets `$FF98 = 3` and `hTimer1 = 2` (KM:5340-5342) |
| 3 = BLINK | KM:5340 | `AnimateLineClear` (VBlank, KM:5412) waits `hTimer1==0`, then either advances next piece (no lines) or runs blink (lines) |

### Entry delay (ARE)
- **No lines cleared:** after `$FF98=3` with `hTimer1=2`, `AnimateLineClear` sees an empty `wLineClearsList` and jumps to `.nextPiece` → `NextPiece`, then `$FF98=0` (KM:5424-5426, 5494-5496). **ARE ≈ 2 frames** (the `hTimer1=2` from stage-2→3) plus the 1-frame transfer/scan steps.
- **Lines cleared:** entry delay = full blink + settle sequence (see §6): 2-frame pre-delay + 7 blink phases × 10f + 13-frame post-delay + ~18-frame board redraw before the next piece spawns.
- Top-out check at lock: if the piece locks still at spawn (`$C201==$18 && $C202==$3F`), a counter at `$FFFB` increments; **the 2nd such spawn-position lock triggers game over** (KM:5261-5281).

---

## 6. Line clear

### Detection — `CheckForCompletedRows` (KM:5301-5337)
- Runs only when `$FF98 == 2`.
- Plays lock SFX (`wNewNoiseSFXID = $02`, KM:5305).
- Scans from BG buffer `$C842` (note: starts at the **3rd** row — documented bug) for `b = 16` rows (only 16 of 18 checked — bug), row stride `$0020`, width `c = 10` (`PLAYFIELD_WIDTH`).
- A row is full if none of its 10 cells equals `" "` (space = empty, KM:5316-5318).
- Full-row **tilemap addresses** are appended to `wLineClearsList` (2 bytes each), count in `$FFA0` (KM:5321-5330).
- Sets `$FF98 = 3`, `hTimer1 = 2` (KM:5339-5342). Then updates line/score/garbage counts by count (single/double/triple/tetris → `wSinglesCount`… `wTetrisCount`; SFX `$06`, Tetris `$07`; KM:5343-5401).

### Blink animation — `AnimateLineClear` (KM:5412-5496) / MC `VBlank_HandleLineClearBlink` (MC:2914-2978)
Runs in VBlank only when `$FF98==3` and `hTimer1==0`. Uses phase counter `$FF9C` (`hBlinkCounter`), **7 phases (0..6), 10 frames each** (`hTimer1=10` between phases, KM:5454-5455):

| `$FF9C` (bit 0) | What each full row shows |
|---|---|
| even (0,2,4) | Overwrite the 10 cells with solid grey tile **`$8C`** (KM:5435) |
| odd (1,3,5) | Restore the original cells (save/restore via scratch high-byte, KM:5470-5492) |
| == 6 (last) | Overwrite with empty space **`" "`** (KM:5434-5437) — cells vanish |

Net visual: rows flash grey↔normal, ending emptied. After phase 7 (`$FF9C` reaches 7): reset `$FF9C=0`, set **`hTimer1 = 13`**, `hWipeCounter = 1` (start row-shift), `$FF98 = 0` (KM:5458-5468).

### Rows above shift down — `MoveBlocksDownAfterLineClear` (KM:5498-5550)
- Runs when `hTimer1==0` and `hWipeCounter==1` (i.e. after the 13-frame settle).
- For each cleared-line address in `wLineClearsList`: copy each row above down by one row (stride `-$20`), walking up until high byte reaches `$C7` (top of field) (KM:5507-5538).
- Top row `$C802` cleared to spaces (KM:5540-5546); clear the list; `hWipeCounter = 2` (KM:5547-5549).
- Then the `PlayingFieldWipe02..19` chain (KM:5563-5751, one row redrawn per VBlank frame, `hWipeCounter` 2→19, ~18 frames) redraws the board; at wipe 16 it calls level-advance `Call_244B` (KM:5716-5717); at wipe 19 (`PlayingFieldWipe19`, KM:5744) `hWipeCounter` resets to 0 and `NextPiece` is triggered (KM:5798-5800), re-enabling `$C0C7`.

Gravity/soft-drop are suppressed while `hWipeCounter != 0` (KM:5215-5217, 5184-5186).

---

## 7. Level advancement (A-type)

`Call_244B` (KM:5813-5876), invoked during the post-clear board redraw (KM:5717). Helper `Call_249D` (KM:5878-5894) converts a binary value in `[hl]` to BCD in `b`.

Algorithm:
- Only runs for Type-A (`hGameType == $37`) during normal gameplay (`hGameState==0`) (KM:5815-5820).
- If `hLevel == $14` (20) → return (**level capped at 20**, KM:5834-5835).
- `b = BCD(hLevel)`.
- Build `floor(totalLines / 10)` in BCD from `hLines`: take low nibble of high byte `$FF9F` (hundreds) into the high position and high nibble of low byte `$FF9E` (tens) — i.e. the "tens-and-up" digits of the BCD line count (KM:5837-5848). If hundreds' high nibble ≠ 0 it early-returns (KM:5839-5840).
- Compare that against `b`: if `floor(lines/10) <= hLevel` → return; else **`hLevel++`** (KM:5849-5852), reprint level digits, play SFX `$08`, and re-run `LookupGravity` to apply new speed (KM:5872-5876).

**Rule in effect:** level = `max(startLevel, floor(totalLines / 10))`, incremented one step at a time. Because `floor(lines/10)` starts at 0 (< any nonzero start level), the level stays at the chosen start until `floor(lines/10)` exceeds it. Consequence:
- Start level 0 → advance every 10 lines (10, 20, 30, …).
- Start level N → **first advance to N+1 at line `(N+1)*10`**, then every 10 lines thereafter.
- Hard cap at level 20.

Starting level: chosen at menu into `hTypeALevel` (0-9 selectable, KM:3371-3377) then copied to `hLevel` when play begins (KM:4147-4153). Heart mode adds 10 to the gravity index only (§2), not to `hLevel` itself.

B-type note: `hTypeBLevel` (0-9) selects gravity/start; `hTypeBStartHeight` (0-5) adds garbage — **2 garbage rows per height unit** (KM:4229-4232, "Two rows of garbage per height"). B-type does not use the `Call_244B` line-based level advance (guarded to Type-A only).