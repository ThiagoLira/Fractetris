I have all the data I need. Here is the complete extracted spec.

---

# Game Boy Tetris — Exact Spec (from kaspermeerts + meithecatte disassemblies)

Naming note: the two disassemblies disagree only on the **S/Z labels**. Tile `$82` is called *S* by kaspermeerts but *Z* by meithecatte; tile `$86` is the reverse. **meithecatte's names match modern convention** and are used below. Always treat the **tile ID** as canonical, not the letter.

---

## 1. Piece representation, IDs, tiles, and all 7×4 rotation offsets

### 1a. Representation model
Pieces are **not** stored as compact offset tables. Each of the 28 (piece × rotation) states is a **sprite object list**: a 4×4 grid of tile IDs, row-major (`$FE` = empty cell, `$FF` = end-of-list), prefixed by a pointer to a shared 4×4 dimension descriptor. The same lists drive both rendering (into OAM) *and* collision (the OAM buffer is re-read).

- Dimension descriptor (4×4, pixel offsets, row-major Y-outer/X-inner):
  `SpriteDim4x4` = kaspermeerts `Matrix_31A9` @ ROM **$31A9** (tetris.asm:6902-6906); meithecatte sprites.asm:558-574. Values: Y∈{0,8,16,24} × X∈{0,8,16,24}.
- Object lists: kaspermeerts `SpriteTiles_2D58`.. @ ROM **$2D58+** (sprites.asm:315-533); meithecatte `SpriteL0Objects`.. (sprites.asm:304-496).
- Descriptor table (per-state: ptr + anchor offsets): kaspermeerts `Sprite_2C20`.. @ ROM **$2C20** (sprites.asm:1-133); meithecatte `SpriteDescriptorPointers` @ ROM **$2C00** + `sprite_descriptor` block @ **$2C68** (sprites.asm:227-297).
- Master ID→descriptor pointer table: kaspermeerts `SpriteList` @ ROM **$6858 label** (tetris.asm:6858-6866).

### 1b. Piece IDs (SPRITE_OFFSET_ID). ID = base | rotation; `rotation = ID & 3`, `base = ID & ~3`
meithecatte constants.asm:112-139.

| ID range | Piece | Block tile(s) |
|----------|-------|---------------|
| `$00`–`$03` | L | `$84` |
| `$04`–`$07` | J | `$81` |
| `$08`–`$0B` | I | horizontal: `$8A $8B $8B $8F`; vertical: `$80 $88 $88 $89` |
| `$0C`–`$0F` | O | `$83` |
| `$10`–`$13` | Z (kaspermeerts "S") | `$82` |
| `$14`–`$17` | S (kaspermeerts "Z") | `$86` |
| `$18`–`$1B` | T | `$85` |

Tetromino block graphics occupy VRAM tiles **`$80`–`$8F`** (loaded from `gfx/configandgameplay.2bpp`, kaspermeerts tetris.asm:6935). `$80` = generic solid single block ("technically part of I", tetris.asm:1545); `$87` = unused by any tetromino (tetris.asm:4587). Anything **≠ `$2F` (space)** is treated as solid (collision & fill use the space char `$2F`, charmap.asm:42).

### 1c. T-piece rotation cycle is SHIFTED
For every piece **except T**, `rotation` directly indexes state 0,1,2,3 = the sprite suffix 0..3. **T is offset by 2**: rotation state `0`→"pointing down", `1`→right, `2`→up, `3`→left. Confirmed in both: meithecatte `SpriteDescriptorPointers` line `dw SpriteT2, SpriteT3, SpriteT0, SpriteT1` (sprites.asm:235, comment "shift the cycle to start pointing down"); kaspermeerts `SpriteList` T entries `$2C80,$2C84,$2C88,$2C8C` → tiles `2F0B(down),2F1C(right),2EEC(up),2EFA(left)`.

### 1d. Full cell-offset tables (grid coords `(row,col)`, row 0 = top, col 0 = left, of the 4×4 grid)
Derived directly from the object lists. Empty (`$FE`) cells omitted; all pieces have exactly 4 filled cells.

**L** (tile `$84`), sprites.asm(meithecatte):304-330 / (kasp)SpriteTiles_2D58,2D68,2D7A,2D89:
| rot | cells |
|---|---|
| 0 | (2,0)(2,1)(2,2)(3,0) |
| 1 | (1,1)(2,1)(3,1)(3,2) |
| 2 | (1,2)(2,0)(2,1)(2,2) |
| 3 | (1,0)(1,1)(2,1)(3,1) |

**J** (tile `$81`), sprites.asm:332-357 / SpriteTiles_2D9A,2DAC,2DBD,2DCB:
| rot | cells |
|---|---|
| 0 | (2,0)(2,1)(2,2)(3,2) |
| 1 | (1,1)(1,2)(2,1)(3,1) |
| 2 | (1,0)(2,0)(2,1)(2,2) |
| 3 | (1,1)(2,1)(3,0)(3,1) |

**I** (horiz tiles `$8A$8B$8B$8F`; vert tiles `$80$88$88$89`), sprites.asm:359-385 / SpriteTiles_2DDC,2DEB,2DFC,2E0B:
| rot | cells |
|---|---|
| 0 | (2,0)(2,1)(2,2)(2,3) — horizontal |
| 1 | (0,1)(1,1)(2,1)(3,1) — vertical |
| 2 | (2,0)(2,1)(2,2)(2,3) — horizontal (dup of 0) |
| 3 | (0,1)(1,1)(2,1)(3,1) — vertical (dup of 1) |

**O** (tile `$83`), sprites.asm:387-413 / SpriteTiles_2E1C,2E2E,2E40,2E52 — all 4 identical:
| rot | cells |
|---|---|
| 0–3 | (2,1)(2,2)(3,1)(3,2) |

**Z** (tile `$82`; kaspermeerts "S"), sprites.asm:415-441 / SpriteTiles_2E64,2E76,2E86,2E98:
| rot | cells |
|---|---|
| 0 | (2,0)(2,1)(3,1)(3,2) |
| 1 | (1,1)(2,0)(2,1)(3,0) |
| 2 | (2,0)(2,1)(3,1)(3,2) — dup of 0 |
| 3 | (1,1)(2,0)(2,1)(3,0) — dup of 1 |

**S** (tile `$86`; kaspermeerts "Z"), sprites.asm:443-469 / SpriteTiles_2EA8,2EB9,2ECA,2EDB:
| rot | cells |
|---|---|
| 0 | (2,1)(2,2)(3,0)(3,1) |
| 1 | (1,0)(2,0)(2,1)(3,1) |
| 2 | (2,1)(2,2)(3,0)(3,1) — dup of 0 |
| 3 | (1,0)(2,0)(2,1)(3,1) — dup of 1 |

**T** (tile `$85`), sprites.asm:471-496 / SpriteTiles_2EEC(up),2EFA(left),2F0B(down),2F1C(right). Listed here by **rotation-state index** (remember the shift, §1c):
| rot state | shape | cells |
|---|---|---|
| 0 | down  | (2,0)(2,1)(2,2)(3,1) |
| 1 | right | (1,1)(2,1)(2,2)(3,1) |
| 2 | up    | (1,1)(2,0)(2,1)(2,2) |
| 3 | left  | (1,1)(2,0)(2,1)(3,1) |

---

## 2. Spawn & preview positions

Constants (meithecatte constants.asm:201-202): `INITIAL_TETROMINO_Y = $18` (24 px), `INITIAL_TETROMINO_X = $3F` (63 px).

- **Spawn (active) sprite anchor**: sprite-list entry `wSpriteList[0]` set to `Y=$18(24), X=$3F(63)`, `SPRITE_BELOW_BG`. Written by `SpawnNewTetromino` (meithecatte bank_000.asm:2538-2547) and `NextPiece` (kaspermeerts tetris.asm:5078-5087). kaspermeerts `ActivePieceSprite` template @ tetris.asm:6280: `db $00,$18,$3F,$00,$80,$00,$00,$FF`.
- **Spawn rotation** = state **0** for all pieces. Randomly generated IDs are always multiples of 4 (rotation bits cleared), see NextPiece random loop (tetris.asm:5116-5132) and `SpawnNewTetromino .division_loop` incrementing by 4 (bank_000.asm:2586-2596). (For T, state 0 = "pointing down", per §1c.)
- Resulting board placement of the spawn: grid columns 0–3 map to tilemap columns **4–7** = playfield internal columns **2–5** (center-ish); the piece's occupied cells land in the top 1–2 rows. (See §4 formula.)
- **Next-piece preview anchor**: `Y=$80(128), X=$8F(143)`, `SPRITE_BELOW_BG`. meithecatte `NextTetrominoSpriteList` @ bank_000.asm:3826: `db SPRITE_VISIBLE,128,143,0,SPRITE_BELOW_BG,...`; kaspermeerts `PreviewPieceSprite` @ tetris.asm:6283: `db $00,$80,$8F,$00,$80,...`. This lands in the "NEXT" box on the right side (≈ tilemap row 13–14, col 15–16). Preview lives at `wSpriteList[1]` = `$C210` (kaspermeerts).

Spawn/preview handoff: on each new piece the preview ID becomes the active ID, and a fresh (partly deterministic) next piece is generated (`NextPiece` tetris.asm:5078-5164; `SpawnNewTetromino` bank_000.asm:2538-2622).

---

## 3. Playfield

- **Dimensions**: `PLAYFIELD_WIDTH = 10` (constants.asm:195). Height = `SCREEN_HEIGHT = 18` rows (hardware.asm:735). So **10 × 18 cells**.
- **Location in the 32-wide BG tilemap**: playfield occupies **columns 2–11, rows 0–17**. Fill routine `FillPlayfieldWithTile` starts at `coord wTileMap, 2, 0`, writes `PLAYFIELD_WIDTH` cells across × `SCREEN_HEIGHT` rows (bank_000.asm:2494-2510). Left wall at col 1 (`$7B`), col 0 = `$2A`, right wall at col 12 (`$7B`) — see `TypeAGameplayTilemap` (kaspermeerts tetris.asm:6939+): `$2A,$7B,` then ten `$2F`, then `$7B`. Walls are ordinary non-`$2F` tiles, so collision naturally blocks horizontal movement — there is no separate bounds check.
- **Two copies of the board**:
  - VRAM BG map `vBGMapA` = **$9800** (hardware.asm:41), 32-byte row stride (`BG_MAP_WIDTH = 32`, hardware.asm:739).
  - WRAM shadow `wTileMap` = **$C800** (meithecatte wram.asm:59-62), same 32-stride layout. `HIGH(wTileMap - vBGMapA) = $30`, i.e. shadow = VRAM addr + `$3000`.
- **Empty-cell value** = `" "` = **`$2F`** (charmap.asm:42). Any tile `≠ $2F` is solid.
- **Collision check** (`CheckCollision` meithecatte bank_000.asm:3543-3582; kaspermeerts `DetectCollision` tetris.asm:6030-6065):
  1. Iterate the 4 objects of the current piece in the OAM buffer (`wOAMBuffer_CurrentPiece` = `$C010` kasp / meithecatte, 4 bytes each: Y, X, tile, flags).
  2. For each object, convert its pixel (Y,X) → tilemap addr via `SpriteCoordToTilemapAddr` (utils.asm:52-79): `addr = $9800 + ((Y-16)>>3)*32 + ((X-8)>>3)`.
  3. Add `$30` to the high byte → read the **WRAM shadow** at `$C800+`.
  4. If that byte `≠ " " ($2F)` → collision (return 1); else continue. (An object X of 0 is treated as out-of-bounds/terminator, kasp:6037, meithe:3551.)
- **Locking a piece into the background** (`HandleLockdownTransferToTilemap` meithecatte bank_000.asm:3584-3632; kaspermeerts `LockPieceIntoBackground` tetris.asm:6068-6107): for each of the 4 objects, compute the `$9800` tilemap address, **wait for H-blank** (`rSTAT & 3 == 0`), write the object's **tile ID** to VRAM `$9800+`, then add `$30` and write the same tile to the WRAM shadow `$C800+`. So locked cells contain the actual block tile IDs (`$80`–`$8F`, including I's connected-tile graphics). Afterwards the active sprite is hidden (`SPRITE_HIDDEN`) and lockdown advances to `LOOK_FOR_FULL_LINES`.

---

## 4. Falling-piece display (sprites) & cell→pixel conversion

- The falling piece is drawn as **one "sprite" = up to 4 hardware objects (8×8)**, one per filled grid cell, into a dedicated OAM slot. `RenderActivePieceSprite` places it at OAM offset `$10` (object slot 4), preview at `$20` (slot 8) (kaspermeerts tetris.asm:6231-6252). meithecatte: `UpdateCurrentTetromino`→`wOAMBuffer_CurrentPiece` ($C010), `UpdateNextTetromino`→`wOAMBuffer_NextPiece` ($C020) (bank_000.asm:3772-3792).
- **Anchor offset** baked into every tetromino descriptor: `(offY, offX) = (-$11, -$10) = (-17, -16)` (kaspermeerts sprites.asm:1-133; meithecatte `sprite_descriptor …,-17,-16` sprites.asm:248-281).
- **Cell → object pixel** (grid cell `(r,c)`, r,c ∈ 0..3), per `UpdateSprites` (meithecatte sprites.asm:131-185):
  - `objY = spriteY + (-17) + 8*r`
  - `objX = spriteX + (-16) + 8*c`
  - (The asm uses `add`/`adc`, so a carry from the anchor add nudges the effective offset; the intended/observed result is a clean grid.)
- **Object pixel → tilemap cell** (same as collision):
  - `tileRow = (objY - 16) >> 3`
  - `tileCol = (objX - 8) >> 3`; playfield-internal `col = tileCol - 2`.
- Combined at spawn (`spriteY=24, spriteX=63`): `tileCol = ((63-24)+8*c)>>3` → grid cols 0..3 → tilemap cols **4,5,6,7** (playfield cols 2..5). Filled cells (grid rows 2–3) land near the top rows.
- Objects use `SPRITE_BELOW_BG` (`$80` in offset 4) so the piece renders behind the frame. Off/empty objects are pushed to Y=`$FF` to hide them.

Coordinate constants: GB OAM Y is offset +16 px, X offset +8 px vs. the visible screen (hence the `-16`/`-8` in the tilemap conversion).

---

## 5. Rotation, kicks, blocked-rotation behavior

Handler: meithecatte `HandleGameplayMovement` bank_000.asm:3406-3466; kaspermeerts `RotateAndShiftPiece` tetris.asm:5910-6004.

- **Button → direction** (rotation is just ±1 on the 2-bit rotation field of the ID, wrapping within the piece's 4 states):
  - **A button** → `.rotate_left`/CW path: if `rot != 0` `dec ID`, else wrap `rot |= 3` (meithe:3423-3435 `.wrap_right_rotation`; kasp:5922-5936 `.rotateCW`). Net effect: **rotation index decreases** (0→3→2→1→0).
  - **B button** → `.rotate_left` path in meithe naming: if `rot != 3` `inc ID`, else wrap `rot &= ~3` (meithe:3437-3449; kasp:5938-5949 `.rotateCCW`). Net effect: **rotation index increases** (0→1→2→3→0).
  - (Both files' internal labels are inconsistent about "CW/CCW"; the mechanical fact is: **A = index−1, B = index+1**, wrapping within the 4-state cycle. Because O/I/Z/S have duplicate even/odd states, A and B look identical for those pieces.)
- **Wall kicks: NONE.** Rotation only rewrites the rotation bits; X/Y are never adjusted to make a rotation fit. Confirmed by the blocked-rotation handling below.
- **Blocked rotation** (collision after rotating): the rotation is **reverted** to the saved original ID and the piece does not move; the rotate SFX is cancelled. meithecatte bank_000.asm:3455-3465 (`CheckCollision`; on hit, restore `hBuffer` ID, re-render, cancel `PULSESFX_ROTATE`); kaspermeerts tetris.asm:5955-5963 (`DetectCollision`; on hit restore `[$A0]` into `$C203`, cancel `wNewSquareSFXID`). So a rotation into a wall/stack is a **no-op** (no kick, no shift).
- Horizontal move (same routine): DAS/auto-fire is `AUTOFIRE_DELAY = 23` frames initial, `AUTOFIRE_RATE = 9` frames repeat (constants.asm:197-198; kaspermeerts uses literal 23/9 at tetris.asm:5974,5982). Blocked move → revert X, retry next frame. Soft/gravity drop `FASTDROP_RATE = 3` frames (constants.asm:199); on drop collision the Y is reverted and lockdown starts.

Top-out: if a piece locks at the spawn position (`Y=$18, X=$3F`) and `hFailedTetrominoPlacements` was already 1 → game over (`STATE_GAME_OVER`), i.e. two consecutive locks at spawn. meithecatte bank_000.asm:2736-2753; kaspermeerts tetris.asm:5261-5281 (`$FFFB` counter, tops out "when it hits 2").