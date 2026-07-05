I have everything needed. Here is the complete extracted spec.

---

# Game Boy Tetris — Exact Spec (from disassemblies)

Primary source: `reference/kaspermeerts/tetris.asm` (well-annotated). Cross-checks: `reference/meithecatte/bank_000.asm`, `menus.asm`, `highscores.asm`, `constants.asm`.
Game-type IDs: **`$37` = A-type**, **`$77` = B-type** (kaspermeerts `tetris.asm:584,600`).

## 1. A-type Scoring

### Line-clear score
No ROM base-value table — values are **inline immediates**. `AddLineClearScore` (kaspermeerts `tetris.asm:4992-5037`) runs only for Type A (`hGameType==$37`) after the wipe (`hWipeCounter==$05`):

| Clear | Lines | Base value | Source line |
|-------|-------|-----------|-------------|
| Single | 1 | `$0040` = 40 | `tetris.asm:5005` |
| Double | 2 | `$0100` = 100 | `tetris.asm:5010` |
| Triple | 3 | `$0300` = 300 | `tetris.asm:5015` |
| Tetris | 4 | `$1200` = 1200 | `tetris.asm:5019` |

**Formula:** `score += base × (hLevel + 1)`, implemented as a loop adding `base` `(level+1)` times via `AddBCD` (`tetris.asm:5025-5036`, `inc b` = level+1). Only one clear-type is scored per lock (whichever count byte in `wLineClearStats` is nonzero; the byte is zeroed after — `tetris.asm:5024`). `hLevel` = current level (can exceed the selected start level).

### Soft-drop scoring
`hSoftDropCounter` counts cells dropped **while DOWN is held continuously** (reset to 0 if DOWN released or piece just locked — `tetris.asm:5199-5201, 5168`). 3 frames between soft-drop steps (`tetris.asm:5187`). On lock (`tetris.asm:5237-5299`):

- **Type A** (`.addSoftDropPointsToScore`, `tetris.asm:5283-5299`): points = `(hSoftDropCounter − 1)`, converted binary→BCD (loop `dec c; inc a; daa`), then `AddBCD` into `wScore`. So **points = cells fallen while holding down − 1** (the last/lock cell isn't counted). No explicit cap; bounded by field height (~17 max/piece).
- **Type B** (`tetris.asm:5244-5257`): accumulates `(counter − 1)` into 16-bit `wSoftDropPoints` for the end-of-game tally (see §2).

Cross-check meithecatte `bank_000.asm:2704-2778`: `hFastDropDistance`; Type A `points = distance − 1` (BCD); Type B `wTypeBScoring_Drop += distance − 1`.

### Score storage & cap
- `wScore` = **3 bytes, packed BCD = 6 decimal digits** (`wram.asm:5`; `SCORE_SIZE EQU 3` meithecatte `constants.asm:190`).
- `AddBCD` (`tetris.asm:174-194`) saturates: on carry out of the 6th digit it writes `$99 $99 $99` → **max score = 999999** (`tetris.asm:190-193`).
- Displayed during Type A play only, most-significant pair first, leading zeros as spaces (`PrintSixDigitNumber`/`PrintScore` `tetris.asm:6617-6659`; `Call_243B` `tetris.asm:5814-5823`).

## 2. B-type Mode

### Player selections
- **Level** `hTypeBLevel` 0-9 (`TYPE_B_LEVEL_COUNT 10`).
- **"High" / start height** `hTypeBStartHeight` 0-5 (`TYPE_B_HIGH_COUNT 6`, meithecatte `constants.asm:188-189`).

### Garbage generation (`InitGarbage`, `tetris.asm:4324-4403`)
- **2 rows of garbage per height** unit: called with `A = hTypeBStartHeight`, `DE = −2×$20` (raises 2 rows per count), `HL = $9A02` (2nd row from bottom). So visible garbage = `2 × height` rows (0,2,4,6,8,10) filling from the bottom up (`tetris.asm:4214-4232`).
- Per cell: 50/50 random block vs empty, driven by `rDIV` (`tetris.asm:4333-4343`). Block tile = `rDIV & $07 | $80` (tiles `$80-$87`); empty = `" "` (`$2F`) (`tetris.asm:4348-4350`).
- **Guaranteed hole:** if a row ended up with no empty cell, the rightmost cell (low nibble `$0B`) is forced empty (~1/512 chance) (`tetris.asm:4355-4366`).

### Win condition
- `hLines` initialized to **`$25` BCD = 25 lines** for Type B (`tetris.asm:4186-4190`, also `4657,4768`).
- On each clear, Type B **counts down** `hLines −= linesCleared`, clamped at 0 (`tetris.asm:5366-5376, 5407-5409`). Reaching 0 → win (`tetris.asm:5775-5796`).

### Completion (bonus) scoring
No score tracked during Type B play; it is computed on the win/scoreboard screen. Two contributions summed into `wScore`:

1. **Line-clear tally** — `UpdateScoreboard`/`Call_25D9` (`tetris.asm:4880-4907, 6109-6163`) walks `wScoreboardState` 0→3 (singles, doubles, triples, tetris) using the **same base values 40/100/300/1200** (`tetris.asm:4887-4904`), adding `base × (hTypeBLevel+1)` per counted clear (`tetris.asm:6151-6160`), animated.
2. **Soft-drop tally** — `wScoreboardState==4` → `tallySoftDropPoints` (`tetris.asm:4844-4878`) drains `wSoftDropPoints` (1 pt/frame) into `wScore` via `AddBCD`.

Final `wScore` (BCD, cap 999999) is what's checked for Type B top scores. The scoreboard tilemap also prints each clear-type's per-clear worth at that level (`PrintLineClearScores`, `tetris.asm:4662-4706`).

### Win screen / celebration tiers
Only when **level 9 is completed** does the dance/rocket sequence play; otherwise straight to scoreboard (`tetris.asm:5788-5795`: level 9 → state `$22`, else `$05`).

| Condition | Celebration | Source |
|-----------|-------------|--------|
| Type B, level < 9 completed | Plain scoreboard (`GameState_05`) | `tetris.asm:5791` |
| Type B, **level 9**, height 0-4 | **Dancers**: `height+1` dancers dance, then scoreboard | `GameState_22/23` `tetris.asm:4718-4829`; dancer count `4749-4763` |
| Type B, **level 9, height 5** | **10 dancers**, then **Buran rocket launch** (`GameState_26`) | `tetris.asm:4750-4755, 4822-4825, 2694-2803` |

Dancer jingle scales with height: `wNewMusicID = height + $0A` (jingles `$0A`..`$0F`) (`tetris.asm:4764-4766`).

### A-type "congratulations" rocket tiers
Separate from Type B. On Type-A game over (`tetris.asm:4943-4970`), reads `wScore+2` (top BCD byte = hundred-thousands/ten-thousands):

| Score threshold | Top byte | Rocket sprite | Source |
|-----------------|----------|--------------|--------|
| ≥ 200000 | `≥ $20` | `$58` | `tetris.asm:4948-4950` |
| ≥ 150000 | `≥ $15` | `$59` | `tetris.asm:4952-4953` |
| ≥ 100000 | `≥ $10` | `$5A` | `tetris.asm:4955-4956` |
| < 100000 | — | none (stay on game-over) | `tetris.asm:4957-4958` |

Bonus ending → `GameState_34` (`tetris.asm:4963-4969`).

## 3. Level-Select Screens

### A-type level select (`GameState_10/11`, `tetris.asm:3317-3405`)
- Grid **levels 0-9**, 2 rows × 5 cols: top row 0-4, bottom row 5-9. Cursor coords `Data_1615` (`tetris.asm:3403-3405`).
- Cursor: LEFT/RIGHT ±1 clamped [0,9] (`tetris.asm:3363-3394`); DOWN adds 5 (only if level<5), UP subtracts 5 (`tetris.asm:3367-3400`). Selection in `hTypeALevel`.
- **Confirm:** START or A → start game (state `$0A`); **Cancel:** B → game/music-type screen (state `$08`) (`tetris.asm:3355-3361`).
- **No hidden 10-19 grid here.** The only levels 0-9 are selectable.

### Heart mode (hidden high levels 10-19)
Entered **not** on the level grid but on the **game-type/music selection screen** (`GameState_07`): pressing **START while holding DOWN** sets `hHeartMode` (`tetris.asm:698-706`). Effect: `LookupGravity` adds 10 to `hLevel` (capped at 20) when `hHeartMode` set (`tetris.asm:4240-4260`); level display shows a **"♥"** next to the number (`tetris.asm:4028-4031, 4169-4175`). Meithecatte equivalent flag `hStartAtLevel10` (highscores.asm:469,507). Gravity table indexes 0-20 (see §5).

### B-type level select (`GameState_12/13`, `tetris.asm:3408-3505`)
- **Level grid 0-9**, 2×5: top 0-4, bottom 5-9. Coords `Data_16D2` (`tetris.asm:3503-3505`). Same movement rules as A-type; clamps [0,9].
- **Confirm:** A → go to **height select** (`GameState_14`); START → start game directly (state `$0A`). **Cancel:** B → type/music screen (state `$08`) (`tetris.asm:3454-3462`).

### B-type start-height select (`GameState_14`, `tetris.asm:3514-3564`)
- Grid **heights 0-5**, 2 rows × 3 cols: top 0-2, bottom 3-5. Coords `Data_1741` (`tetris.asm:3566+`). RIGHT/LEFT ±1 clamp [0,5]; DOWN +3 (if <3), UP −3 (`tetris.asm:3527-3564`). Stored in `hTypeBStartHeight`.
- **Confirm:** START or A → start game (`$0A`). **Cancel:** B → back to level select (`$13`) (`tetris.asm:3519-3525`).

## 4. Game Over

### Top-out (block-out) condition
No height-based top-out. A piece spawns at Y=`$18`, X=`$3F` (`tetris.asm:5084`; meithecatte `INITIAL_TETROMINO_Y $18`). When a piece **locks while still at the spawn Y/X**, counter `$FFFB` (`hFailedTetrominoPlacements`) is checked: allowed once; the **2nd** such lock triggers game over (`tetris.asm:5261-5281`, `cp 1 → game over`; meithecatte `bank_000.asm:2736-2758`). Sets `hGameState=$01`, wave SFX `$02`.

### Game-over animation ("curtain")
`GameState_01` (`tetris.asm:4577-4593`):
1. Fills entire playfield buffer with tile **`$87`** (a non-tetromino solid block) via `FillPlayingFieldAndWipe` (`tetris.asm:4588, 5039-5059`), which sets `hWipeCounter=2`.
2. The VBlank `PlayingFieldWipe02..19` chain (`tetris.asm:214-232, 5563-5622`) copies one buffer row → VRAM **per frame**, advancing `hWipeCounter` each row (`WipePlayingFieldRow` `tetris.asm:5896-5908`). Rows fill **bottom→top** (`$9A22`, `$9A02`, `$99E2`, …) over the **18 playfield rows ≈ 18 frames**.
3. Sets `hTimer1 = 70` frames (≈1⅙ s) then `hGameState=$0D` (`tetris.asm:4589-4592`).
4. `GameState_0D` (`tetris.asm:4917-4970`): after timer, plays game-over jingle (`wNewMusicID=$04`), prints "GAME OVER" text (`Data_293E`/`Data_2976`, `tetris.asm:4935-4942`), then either A-type bonus ending (§2) or waits for START (state `$04`).

### High-score entry screens
Constants (meithecatte `constants.asm:191-193`):

| Field | Value |
|-------|-------|
| Entries per table slot | **`HIGHSCORE_ENTRY_COUNT = 3`** |
| Name length | **`HIGHSCORE_NAME_LENGTH = 6`** chars |
| Entry size | 6 (name) + 3 (BCD score) = 9 bytes |
| Type A tables | 10 levels × 3 entries (`wTypeATopScores`, `wram.asm:59-60`) |
| Type B tables | 10 levels × 6 highs × 3 entries (`wTypeBTopScores`, `wram.asm:56-57`) |

Name entry (kaspermeerts `tetris.asm:4020-4112`; meithecatte `highscores.asm:375-567`):
- Cursor over 6 letter slots. UP/DOWN cycle character: `A`..`Z` → `space` → wraps; extra top char is `×` normally, **`♥` if heart/`hStartAtLevel10` set** (`tetris.asm:4027-4031, 4059-4063`; meithecatte `highscores.asm:466-472, 504-510`). Auto-repeat every 9 frames (`tetris.asm:4024`; meithecatte `AUTOFIRE_DELAY 23`, `AUTOFIRE_RATE 9`).
- **A** = confirm letter, advance; after 6th letter → submit (`tetris.asm:4085-4087`; `highscores.asm:536-537`).
- **B** = backspace (won't go before first slot) (`tetris.asm:4102-4112`; `highscores.asm:555-567`).
- **START** = finish immediately (`highscores.asm:440-455`).
- New score is inserted by comparing `wScore` (3-byte BCD, MSB first) against the 3 entries, shifting lower ones down; ties give priority to the earlier score (meithecatte `RenderHighscores`, `highscores.asm:105-233`).

## 5. Lines Counter

| Aspect | A-type | B-type |
|--------|--------|--------|
| Storage | `hLines` 2-byte BCD (`hram.asm:59`) | `hLines` (low byte used) |
| Init | `$00` (`tetris.asm:4188-4190`) | **`$25` = 25** (`tetris.asm:4186`) |
| Update on clear | `hLines += cleared`, BCD (`tetris.asm:5351-5364`) | `hLines −= cleared`, clamp 0 (`tetris.asm:5366-5409`) |
| Cap / limit | **max 9999** — carry saturates to `$99 $99` (`tetris.asm:5360-5363`) | floor at 0 → triggers win |
| Display | 4 digits at `$994E`, `hLines+1`, `c=2` pairs (`tetris.asm:5761-5766`) | 2 digits at `$9950`, `hLines`, `c=1` pair, counts 25→00 (`tetris.asm:5767-5771`) |

### A-type level progression (`Call_244B` + `Call_249D`, `tetris.asm:5825-5894`)
- Runs only Type A, during play. Level cap **`$14` = 20** (`tetris.asm:5834`).
- Effective rule: **level = max(startLevel, ⌊totalLines/10⌋)** — computed by comparing BCD(level) against the tens-place of `hLines` combined with `$9F` hundreds counter (`tetris.asm:5836-5852`); increments level by 1 when the lines-tens exceeds current level. On level-up, updates the on-screen digit and calls `LookupGravity`. (`$9F` init 0 at `tetris.asm:532,1225,4132`.)

### Gravity table (`FramesPerDropTable`, `tetris.asm:4262-4283`) — frames per 1-row drop, indexed by level (heart mode = level+10, capped 20 via `LookupGravity` `tetris.asm:4240-4260`)

| Lvl | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 |
|-----|---|---|---|---|---|---|---|---|---|---|----|----|----|----|----|----|----|----|----|----|----|
| Frames | 52 | 48 | 44 | 40 | 36 | 32 | 27 | 21 | 16 | 10 | 9 | 8 | 7 | 6 | 5 | 5 | 4 | 4 | 3 | 3 | 2 |

---

### Key addresses / labels
- Score `wScore` = 3-byte BCD (`kaspermeerts/wram.asm:5`); line-clear counts `wSinglesCount/wDoublesCount/wTriplesCount/wTetrisCount` spaced 5 bytes (`wram.asm:15-32`); `wSoftDropPoints` (16-bit) + `wSoftDropPointsBCD` (`wram.asm:36-40`); `wScoreboardState` (`wram.asm:42`).
- HRAM: `hLines` (`hram.asm:59`), `hLevel` (`hram.asm:78`), `hGameType` (`hram.asm:110`), `hTypeALevel/hTypeBLevel/hTypeBStartHeight` (`hram.asm:116-122`), `hGameState` (`hram.asm:165`), `hSoftDropCounter` (`hram.asm:175`), `hHeartMode` (`hram.asm:216`), `hFramesPerDrop`/`hDropTimer` (`hram.asm:51-54`); top-out counter at `$FFFB` (`hram.asm:219`, `tetris.asm:5268`).
- No ROM "base-value table" exists — 40/100/300/1200 are inline immediates in `AddLineClearScore` (`tetris.asm:5005-5019`), `UpdateScoreboard` (`tetris.asm:4887-4904`), and `Call_25D9` (`tetris.asm:6109+`).