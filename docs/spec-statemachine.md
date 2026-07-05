I have gathered all the necessary details from both disassemblies. Here is the extracted spec.

---

# Game Boy Tetris — Exact Spec for C Port

Sources abbreviated: **K** = `/home/thlira/repos/gameboy-tetris-pc-port/reference/kaspermeerts/`, **M** = `/home/thlira/repos/gameboy-tetris-pc-port/reference/meithecatte/`. All coords are **(column,row)** on the 32-wide VRAM BG map (`BG_MAP_WIDTH=32`, M/hardware.asm:739); visible window = 20×18 (M/hardware.asm:734-735). `vBGMapA=$9800`, `vBGMapB=$9C00` (M/hardware.asm:41-42). Address→coord: `col = (addr-base)&31`, `row = (addr-base)>>5`.

## 1. Master game-state variable + jump table

- Variable: **`hGameState` = `$FFE1`** (M/hram.asm:84; K `hGameState`). Dispatched every frame by `HandleGameState` (M/mainloop.asm:169-171) via `rst $28` jumptable macro (M/macros.asm:1-3). K dispatch at K/tetris.asm:388,420-477.
- Full state table (index = state id). Semantics from K/tetris.asm:423-477; handler labels from M/mainloop.asm:172-228. **★ = matters for single-player.**

| id | K semantics (K:line) | M label (M/mainloop.asm) |
|----|----------------------|--------------------------|
| $00 ★ | Normal gameplay (423) | HandleGameplay |
| $01 ★ | Init game over (424) | HandleGameOver |
| $02 | Buran liftoff (425) | HandleState2 |
| $03 | Buran rising (426) | HandleState3 |
| $04 ★ | Game over screen (427) | HandleState4 |
| $05 ★ | Type-B victory jingle (428) | HandleState5 |
| $06 ★ | Init title screen (429) | LoadTitlescreen |
| $07 ★ | Title screen (430) | HandleTitlescreen |
| $08 ★ | Init game-type/music select (431) | LoadModeSelect |
| $09 | points to a bare RET (432) | GenericEmptyRoutine2 |
| $0A ★ | Init game / load playfield (433) | LoadPlayfield |
| $0B ★ | Init Type-B scoreboard (434) | HandleState11 |
| $0C | ? (435) | HandleState12 |
| $0D ★ | Game-over curtain (436) | HandleState13 |
| $0E ★ | Select game type (437) | HandleModeSelect |
| $0F ★ | Select music type (438) | HandleMusicSelect |
| $10 ★ | Init Type-A difficulty select (439) | LoadTypeAMenu |
| $11 ★ | Type-A level select (440) | HandleTypeAMenu |
| $12 ★ | Init Type-B difficulty select (441) | LoadTypeBMenu |
| $13 ★ | Type-B level select (442) | HandleTypeBLevelSelect |
| $14 ★ | Type-B start-height select (443) | HandleTypeBHighSelect |
| $15 ★ | Enter high-score name (444) | HandleHighscoreEnterName |
| $16 | Init 2P difficulty select (445) | HandleState22 |
| $17 | Select 2P start height (446) | HandleState23 |
| $18 | Init 2P game (447) | HandleState24 |
| $19 | Init 2P game 2x (448) | HandleState25 |
| $1A | 2P game (449) | HandleState26 |
| $1B | 2P end jingle (450) | HandleState27 |
| $1C | Prepare garbage (451) | HandleState28 |
| $1D | Init 2P victory (452) | HandleState29 |
| $1E | Init 2P defeat (453) | HandleState30 |
| $1F | Init 2P game 3x (454) | HandleState31 |
| $20 | 2P victory screen (455) | HandleState32 |
| $21 | 2P defeat screen (456) | HandleState33 |
| $22 ★ | Init Type-B bonus ending (457) | HandleState34 |
| $23 ★ | Dancers (458) | HandleState35 |
| $24 ★ | Init copyright screen (459) | LoadCopyrightScreen |
| $25 ★ | Copyright screen (460) | HandleCopyrightScreen |
| $26 ★ | Init Buran ending (461) | HandleState38 |
| $27 ★ | Prepare Buran launch (462) | HandleState39 |
| $28 ★ | Buran ignition (463) | HandleState40 |
| $29 ★ | Buran ignition again (464) | HandleState41 |
| $2A | Init 2P music select (465) | LoadMultiplayerMusicSelect |
| $2B | 2P select music (466) | HandleState43 |
| $2C ★ | Print congratulations (467) | HandleState44 |
| $2D ★ | Congratulations (468) | HandleState45 |
| $2E ★ | Init rocket launch (469) | HandleState46 |
| $2F ★ | Rocket (470) | HandleState47 |
| $30 ★ | Rocket ignition (471) | HandleState48 |
| $31 ★ | Rocket liftoff (472) | HandleState49 |
| $32 ★ | Rocket main-engine fire (473) | HandleState50 |
| $33 ★ | End of bonus scene (474) | HandleState51 |
| $34 ★ | Game-over screen → bonus ending (475) | HandleState52 |
| $35 ★ | Copyright screen, skippable (476) | (INTERNATIONAL: MoreCopyrightScreenDelay) |
| $36 | end / `$27EA` (477) | GenericEmptyRoutine |

- Symbolic constants for the SP-relevant ones in M/constants.asm:20-48 (`STATE_GAMEPLAY=0`, `STATE_GAME_OVER=1`, `STATE_LOAD_TITLESCREEN=6`, `STATE_TITLESCREEN=7`, `STATE_LOAD_MODE_SELECT=8`, `STATE_LOAD_PLAYFIELD=10`, `STATE_MODE_SELECT=14`, `STATE_MUSIC_SELECT=15`, `STATE_LOAD_TYPE_A_MENU=16`, `STATE_TYPE_A_MENU=17`, `STATE_LOAD_TYPE_B_MENU=18`, `STATE_TYPE_B_LEVEL_SELECT=19`, `STATE_TYPE_B_HIGH_SELECT=20`, `STATE_HIGHSCORE_ENTER_NAME=21`, `STATE_LOAD_COPYRIGHT=36`, `STATE_COPYRIGHT=37`).
- **Minimal single-player path:** $24→$25(→$35)→$06→$07→$08→($0E,$0F)→($10/$11 A, or $12/$13/$14 B)→$0A→$00→($01,$0D,$04) game over, or $05/$34/$22… endings.
- Boot sets `STATE_LOAD_COPYRIGHT` (M/mainloop.asm:119-120).

## 2. Title screen (state $06 load / $07 handle)

Load = `LoadTitlescreen` (M/titlescreen.asm:55-123; K/tetris.asm:524-580).
- Static `TitlescreenTilemap` loaded to map A (M:96-97). No per-frame background/falling-block animation — the only moving object is the 1P/2P selector sprite. (Dead code draws black vertical strips cols 1 & 12, M:82-85.)
- **Cursor sprite** = `wOAMBuffer` (`$C000`): Y=`128`, X=`16`, tile=`$58`, attr 0 (M:100-105; K:559-564). X positions: **1P = `$10`(16), 2P = `$60`(96)** (M:275-279).
- Timers set: `hDelayCounter($FFA6)=125` and `hDemoCountdown($FFC6)`: **=4** if returning from a demo (`hDemoNumber≠0`), else **=19** (M:113-123; K:571-580: `$7D`=125, `$04`/`$13`=4/19).
- Song `SONG_TITLESCREEN`=3 (M:107-108).

Handle = `HandleTitlescreen` (M/titlescreen.asm:176-294):
- **Buttons** (read from `hKeysPressed`): **SELECT** toggles 1P/2P (`hMultiplayer($FFC5) ^= 1`, M:210-211,270-280); **D-RIGHT**→2P (M:213,282-286); **D-LEFT**→1P (M:216,288-294); **START** starts (M:219). 
- START with 1P → `STATE_LOAD_MODE_SELECT`($08) (M:223-224,261-269). START with 2P/serial → multiplayer path.
- **Level-10 secret:** if **D-DOWN held** (`hKeysHeld`) when START pressed, set `hStartAtLevel10($FFF4)` (M:262-266).
- **Demo timeout:** each frame `hDelayCounter` counts down (main-loop counter, §5). When it reaches 0, `hDemoCountdown` is decremented; on reaching 0 → `StartDemo`; otherwise `hDelayCounter` reloads to 125 (M:177-186). So demo auto-starts after **`hDemoCountdown` × 125 frames** ≈ 19×125 = **2375 frames (~39.6 s)** on a fresh title, or 4×125 = 500 frames if just returned from a demo.
- `StartDemo` (M:125-169): demos alternate; picks Type-A (level 9) or Type-B (level 9, high 2) by `hDemoNumber` (`DEMO_TYPE_A=2`, `DEMO_TYPE_B=1`, M/constants.asm:172-175), loads `ModeSelectTilemap`, sets state `$0A`.

## 3. Gameplay screen layout (state $0A load, `LoadPlayfield` M/bank_000.asm:1395-1528)

**Digit tiles:** characters `"0".."9"` map to **tile ids `$00..$09`** (M/charmap.asm:1-10). `" "`=`$2F` blank (M:42). Text drawn straight to BG uses these; sprite digits are separate (`SPRITE_DIGIT_0=$20`, M/constants.asm:144-153).

**Playfield origin & size:** 10 columns wide (`PLAYFIELD_WIDTH=10`, M/constants.asm:195) starting at **column 2**, visible rows **0–17** (top→bottom). Row shifts write `coord vBGMapA, 2, row` for rows 19..0 (M/bank_000.asm:3095-3170, `ShiftRow19` at :3168 uses row 0; `ShiftRow` copies 10 tiles :3386-3399). i.e. field occupies **cols 2–11, rows 0–17**.

**Spawn / active piece sprite:** metasprite anchor Y=`$18`(24), X=`$3F`(63) (`INITIAL_TETROMINO_Y/X`, M/constants.asm:201-202; `CurrentTetrominoSpriteList` M/bank_000.asm:3823). Sprite→tile: Y→row 1, X→col 6 (utils.asm:52-79). OAM at `wOAMBuffer_CurrentPiece` (`$C010`).

**Next-piece preview:** drawn as an **OAM metasprite**, not on the BG. `NextTetrominoSpriteList` = Y=`128`, X=`143` (M/bank_000.asm:3826) → tile ≈ **(col 16, row 14)**. OAM buffer `wOAMBuffer_NextPiece` (`$C020`), rendered by `UpdateNextTetromino` (M/bank_000.asm:3783-3792). The box outline itself is baked into the gameplay tilemap.

**Type-A panel (`GAME_TYPE_A=$37`):**
| Field | Coord (leftmost tile) | Width | Cite |
|-------|----------------------|-------|------|
| SCORE | **(13,3)** | 6 digits (`wScore` 3 bytes BCD, leading-zero→space) | RenderScore→DisplayBCD_ThreeBytes, M/bank_000.asm:3158-3160,3243-3255; utils.asm:133-184; vblank.asm:53,57 renders to both maps |
| LINES | **(14,10)** | 4 digits (`hLineCount` `$FF9E`, 2 bytes) | M/bank_000.asm:3182-3192 |
| LEVEL | **(16,7)–(17,7)** | 2 digits (tens `$98F0`, ones `$98F1`) | M/bank_000.asm:1421-1443 (`$FFE6=$F1`), 3330-3347 |

**Type-B panel (`GAME_TYPE_B=$77`):**
| Field | Coord | Width | Cite |
|-------|-------|-------|------|
| LINES (counts down from 25) | **(16,10)** | 2 digits (`hLineCount` 1 byte) | M/bank_000.asm:3188-3190 |
| LEVEL | **(16,2)** | (`$FFE6=$50`→`$9850`) | M/bank_000.asm:1419,1438-1443 |
| HIGH (start height) | **(16,5)** | 1 tile | M/bank_000.asm:1500-1504 |
- Type-B has **no score** (RenderScore/level-update early-return unless `GAME_TYPE_A`, M/bank_000.asm:3249-3251,3264-3266).

Both maps A and B get the same gameplay tilemap at load (M/bank_000.asm:1416-1432; A ptr `$3ED7`, B ptr `$403F`). Level start value from `hTypeALevel($FFC2)`/`hTypeBLevel($FFC3)` (M:1417-1428). Type-B gravity primed to 52 (M:1496-1497).

## 4. Pause (single-player)

- Toggled by **START** during gameplay (`GameState_00`/`HandleGameplay`→`HandleStartSelect`, K/tetris.asm:4405-4471; M `Call_000_1c68` M/bank_000.asm:1796-1858). Flag **`hPaused` = `$FFAB`** (K:4455-4457; M `[$ff00+$ab]`). When paused, `HandleGameplay` returns immediately (K:4408-4410; M:1759-1761).
- **On pause** (K:4460-4474; M:1820-1836): `set 3,[rLCDC]` → BG switches to map **$9C00** (mirror); write `1` to `$DF7F` (pause tone); copy the 4 LINES digits from `$994E`(map A, col 14 row 10) → `$9D4E`(map B) during HBlank; hide both tetromino sprites (`SPRITE_HIDDEN=$80` into `wSpriteList` sprite 0 & 1) (M:1838-1847).
- **What's shown/hidden:** map B has the empty playfield with the **"PAUSE" graphic** pre-baked into it — `PauseMessageTilemap` copied to `$9C63` (map B **col 3, row 3**), **8 tiles wide × 10 rows** (`Copy8TilesWide`, M/bank_000.asm:1433-1436, 2409-2428; K:4156-4161 same, K src `PauseMessageTilemap`). Since locked blocks and the active piece live only on map A / OAM, switching to B hides all playfield contents and the pieces, revealing PAUSE; score/lines still visible.
- **On unpause** (K:4459; M:1849-1858): `res 3,[rLCDC]` back to map A; write `2` to `$DF7F` (resume); re-show sprites (respecting the SELECT preview-hide flag).
- **SELECT during gameplay** (separate from pause): toggles next-piece preview visibility — `wHidePreviewPiece` (K `handleSelect` :4423-4438, hides sprite `$C210`); M toggles `$c0de` and hides `wSpriteList sprite 1` (M/bank_000.asm:1776-1794).
- Holding **A+B+SELECT+START** anywhere = soft reset (M/mainloop.asm:135-138; also M/bank_000.asm:1797-1800; K:4440-4444).

## 5. Frame loop, input, counters

**Main loop** `MainLoop` (M/mainloop.asm:130-167):
1. `ReadJoypad` (M/utils.asm:3-46).
2. `HandleGameState` (state dispatch).
3. `JumpUpdateAudio`.
4. A+B+SELECT+START held → SoftReset (M:135-138).
5. Decrement `hDelayCounter($FFA6)` and `hFastDropDelayCounter($FFA7)` if nonzero (M:140-151) — these are the per-frame countdown timers (used by demo timeout, cursor blink, etc.).
6. Multiplayer: re-enable serial IE (M:153-158).
7. **Spin-wait** on `hVBlankOccured($FF85)` until set, then clear it, loop (M:160-167). This is the frame-sync gate.

**VBlank ISR** `VBlankInterrupt` (M/vblank.asm:1-74):
- Serial send handling (M:6-20).
- `VBlank_HandleLineClearBlink`, then `ShiftRow19`…`ShiftRow2` (playfield row transfers to VRAM one row/frame), `VBlank_TypeBScoringScreen`, **OAM DMA** (`hOAMDMA` `$FFB6`), `VBlank_HighscoreTilemap` (M:22-43).
- Deferred score render: if `wScoreDirty` set and `hLockdownStage==LOCKDOWN_STAGE_BLINK(3)`, `RenderScore` to `(13,3)` on both map A and map B, set `hScoreDirty`, clear `wScoreDirty` (M:45-60).
- **Free-running frame counter:** `inc [$FFE2]` (M:63-64).
- Zero `rSCX`/`rSCY` (M:65-67); set `hVBlankOccured=1` (M:68-69); `reti`.
- Only IE = VBLANK (+SERIAL in MP) enabled (M/mainloop.asm:113-114,157).

**Input** `ReadJoypad` (M/utils.asm:3-46): select D-pad (`JOYP_DPAD`), read `rJOYP` twice (settle), `cpl & $0F`, `swap`→high nibble = B; select buttons (`JOYP_BUTTONS`), read 6×, `cpl & $0F`→low nibble; OR into C = current held byte. Bit layout (M/constants.asm:2-9): `A=%00000001, B=%00000010, SELECT=%00000100, START=%00001000, RIGHT=%00010000, LEFT=%00100000, UP=%01000000, DOWN=%10000000`.
- **Held** = `hKeysHeld($FF80)` = C.
- **Pressed (edge)** = `hKeysPressed($FF81)` = `(hKeysHeld XOR C) AND C` = newly-pressed this frame (M:38-43).
- Deselect pad at end (`JOYP_DESELECT`).

Autofire (DAS) constants: `AUTOFIRE_DELAY=23`, `AUTOFIRE_RATE=9`, `FASTDROP_RATE=3` (M/constants.asm:197-199); counters `hAutoFireCountdown($FFAA)`, `hFastDropDelayCounter($FFA7)`. Piece RNG source = `rDIV` (M/bank_000.asm:2582).

## 6. Copyright screen (states $24/$25, +$35/$53 skippable)

`LoadCopyrightScreen` (state $24) (M/titlescreen.asm:1-28; K `GameState_24` :479-500):
- Loads titlescreen tileset + `CopyrightTilemap`, clears OAM, seeds `wRandomness` from `DemoRandomness` (M:8-17).
- Sets `hDelayCounter`: **125 frames** (non-INTERNATIONAL) / **250 frames** (INTERNATIONAL) (M:20-25). K baserom uses **250** (`4*60+10`, K:496-497). Sets state = `STATE_COPYRIGHT`($25).

`HandleCopyrightScreen` (state $25) (M/titlescreen.asm:30-53; K `GameState_25` :502-510):
- Waits for `hDelayCounter==0` (no keypress skip in JP/non-INTERNATIONAL) (M:31-33).
- **Non-INTERNATIONAL:** → `STATE_LOAD_TITLESCREEN`($06) directly (M:51-52).
- **INTERNATIONAL / K:** reload timer (250 / `4*60+10`) and go to a **second, skippable** copyright state ($53 `MoreCopyrightScreenDelay` M:36-52 / K `GameState_35` :512-522): `hKeysPressed != 0` (any key) skips immediately, else waits for `hDelayCounter==0`, then → title ($06).

**Net:** copyright shows ~125 frames (JP) or two ~250-frame screens (Intl), the second skippable by any button press; then title.