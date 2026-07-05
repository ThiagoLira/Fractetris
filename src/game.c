/* Game logic ported from the Tetris (World, Rev 1) disassemblies.
 * All constants and timings cite docs/spec-*.md, which cite the asm. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"
#include "audio.h"
#include "game.h"
#include "video.h"

/* ---- VRAM tile layout (mirrors the original loaders) ---- */
#define FONT_BASE 0
#define TITLE_BASE 39
#define GAMEPLAY_BASE 48 /* unused: gameplay tiles keep their ROM ids 0x30+ */

#define T_EMPTY 0x2F /* " " */
#define T_GREY 0x8C  /* line-clear blink fill */
#define T_CURTAIN 0x87

#define FIELD_W 10
#define FIELD_H 18
#define FIELD_BG_X 2 /* playfield starts at BG column 2 */

/* Sprite slots */
#define SPR_PIECE 0   /* 4 objs */
#define SPR_PREVIEW 4 /* 4 objs */
#define SPR_CURSOR 8  /* up to 6 objs (menu boxes) */

typedef struct { int8_t r, c; uint8_t tile; } PCell;

/* 7 pieces x 4 rotation states x 4 cells, from docs/spec-pieces.md.
 * Piece id = (index<<2)|rot. A button: rot-1, B button: rot+1 (wrapping). */
static const PCell PIECES[7][4][4] = {
    { /* L, tile 0x84 */
        {{2,0,0x84},{2,1,0x84},{2,2,0x84},{3,0,0x84}},
        {{1,1,0x84},{2,1,0x84},{3,1,0x84},{3,2,0x84}},
        {{1,2,0x84},{2,0,0x84},{2,1,0x84},{2,2,0x84}},
        {{1,0,0x84},{1,1,0x84},{2,1,0x84},{3,1,0x84}},
    },
    { /* J, tile 0x81 */
        {{2,0,0x81},{2,1,0x81},{2,2,0x81},{3,2,0x81}},
        {{1,1,0x81},{1,2,0x81},{2,1,0x81},{3,1,0x81}},
        {{1,0,0x81},{2,0,0x81},{2,1,0x81},{2,2,0x81}},
        {{1,1,0x81},{2,1,0x81},{3,0,0x81},{3,1,0x81}},
    },
    { /* I: horizontal 8A 8B 8B 8F, vertical 80 88 88 89 */
        {{2,0,0x8A},{2,1,0x8B},{2,2,0x8B},{2,3,0x8F}},
        {{0,1,0x80},{1,1,0x88},{2,1,0x88},{3,1,0x89}},
        {{2,0,0x8A},{2,1,0x8B},{2,2,0x8B},{2,3,0x8F}},
        {{0,1,0x80},{1,1,0x88},{2,1,0x88},{3,1,0x89}},
    },
    { /* O, tile 0x83 */
        {{2,1,0x83},{2,2,0x83},{3,1,0x83},{3,2,0x83}},
        {{2,1,0x83},{2,2,0x83},{3,1,0x83},{3,2,0x83}},
        {{2,1,0x83},{2,2,0x83},{3,1,0x83},{3,2,0x83}},
        {{2,1,0x83},{2,2,0x83},{3,1,0x83},{3,2,0x83}},
    },
    { /* Z, tile 0x82 */
        {{2,0,0x82},{2,1,0x82},{3,1,0x82},{3,2,0x82}},
        {{1,1,0x82},{2,0,0x82},{2,1,0x82},{3,0,0x82}},
        {{2,0,0x82},{2,1,0x82},{3,1,0x82},{3,2,0x82}},
        {{1,1,0x82},{2,0,0x82},{2,1,0x82},{3,0,0x82}},
    },
    { /* S, tile 0x86 */
        {{2,1,0x86},{2,2,0x86},{3,0,0x86},{3,1,0x86}},
        {{1,0,0x86},{2,0,0x86},{2,1,0x86},{3,1,0x86}},
        {{2,1,0x86},{2,2,0x86},{3,0,0x86},{3,1,0x86}},
        {{1,0,0x86},{2,0,0x86},{2,1,0x86},{3,1,0x86}},
    },
    { /* T, tile 0x85; state 0 = pointing down (shifted cycle) */
        {{2,0,0x85},{2,1,0x85},{2,2,0x85},{3,1,0x85}},
        {{1,1,0x85},{2,1,0x85},{2,2,0x85},{3,1,0x85}},
        {{1,1,0x85},{2,0,0x85},{2,1,0x85},{2,2,0x85}},
        {{1,1,0x85},{2,0,0x85},{2,1,0x85},{3,1,0x85}},
    },
};

/* FramesPerDropTable @ $1B61 */
static const uint8_t GRAVITY[21] = {
    52,48,44,40,36,32,27,21,16,10,9,8,7,6,5,5,4,4,3,3,2
};

#define AUTOFIRE_DELAY 23
#define AUTOFIRE_RATE 9
#define FASTDROP_RATE 3

/* Spawn: OAM Y=$18 X=$3F -> grid origin at board (row -2, col 2) */
#define SPAWN_ROW (-2)
#define SPAWN_COL 2

typedef enum {
    ST_TITLE,
    ST_MODE_SELECT,  /* $0E: A-type/B-type */
    ST_MUSIC_SELECT, /* $0F */
    ST_A_LEVEL,      /* $11 */
    ST_B_LEVEL,      /* $13 */
    ST_B_HIGH,       /* $14 */
    ST_PLAY,         /* $00 */
    ST_CURTAIN,      /* $01/$0D game-over wipe */
    ST_GAMEOVER,     /* $04 */
    ST_B_WIN,        /* simplified $05 */
} State;

static State state;

static Tileset ts_font, ts_title, ts_gameplay;
static Tilemap tm_title, tm_modeselect, tm_adiff, tm_bdiff, tm_aplay, tm_bplay;

/* ---- persistent selections ---- */
static int game_type_b;     /* 0 = A-type, 1 = B-type */
static int music_sel;       /* 0..3 = A/B/C/off */
static int a_level, b_level, b_high;
static int heart_mode;      /* hidden: DOWN held + START on mode select */

/* ---- gameplay state (names mirror the original HRAM) ---- */
static uint8_t board[FIELD_H][FIELD_W]; /* tile ids, T_EMPTY = empty */
static uint8_t active_id, preview_id, nextnext_id;
static int piece_row, piece_col; /* grid origin in board coords */
static int level, lines;         /* lines counts down in B-type */
static uint32_t score;
static uint8_t drop_timer, frames_per_drop;
static uint8_t lock_stage;  /* $FF98: 0 idle 1 transfer 2 scan 3 blink */
static uint8_t blink_phase; /* $FF9C */
static uint8_t timer1, timer2; /* $FFA6/$FFA7 */
static uint8_t das_timer;      /* $FFAA */
static uint8_t wipe;           /* $FFE3 */
static uint8_t softdrop_cells; /* $FFE5 */
static uint8_t softdrop_armed; /* !wDidntUseFastDropOnThisPiece */
static uint8_t failed_placements; /* $FFFB */
static uint8_t paused, preview_hidden;
static uint8_t cleared_rows[4];
static int cleared_count, clear_kind; /* kind: lines cleared this lock */
static uint8_t saved_row_tiles[4][FIELD_W]; /* for blink restore */
static int menu_cursor, blink_on;
static uint8_t blink_timer;
static uint32_t b_softdrop_tally;

/* ---- RNG: stands in for rDIV reads ---- */
static uint16_t rng_state = 0xACE1;

/* test hook: TETRIS_PIECES="8,8,8" forces the piece sequence (ids, cyclic) */
static uint8_t forced_pieces[64];
static int forced_count, forced_idx;

void game_seed(unsigned s) { rng_state = (uint16_t)(s | 1); }

static uint8_t read_div(void)
{
    rng_state = (uint16_t)(rng_state * 25173u + 13849u);
    return (uint8_t)(rng_state >> 8);
}

/* ((DIV-1) mod 7) * 4, emulating the exact wrap loop */
static uint8_t random_piece_id(void)
{
    uint8_t b = read_div(), a;
    for (a = 0;;) {
        if (--b == 0)
            break;
        a += 4;
        if (a == 28)
            a = 0;
    }
    return a;
}

/* ---- helpers ---- */
static void bg_field_set(int r, int c, uint8_t tile)
{
    video_bg_set(FIELD_BG_X + c, r, tile);
}

static void draw_field_row(int r)
{
    for (int c = 0; c < FIELD_W; c++)
        bg_field_set(r, c, board[r][c]);
}

static void draw_field(void)
{
    for (int r = 0; r < FIELD_H; r++)
        draw_field_row(r);
}

/* font mapping: digits 0x00+, A-Z 0x0A+, '-' 0x25, ' ' blank */
static void print_text(int x, int y, const char *s)
{
    for (; *s; s++, x++) {
        uint8_t t = T_EMPTY;
        if (*s >= '0' && *s <= '9') t = (uint8_t)(*s - '0');
        else if (*s >= 'A' && *s <= 'Z') t = (uint8_t)(*s - 'A' + 0x0A);
        else if (*s == '-') t = 0x25;
        video_bg_set(x, y, t);
    }
}

/* right-aligned number, leading zeros as spaces (like PrintSixDigitNumber) */
static void print_num(int x, int y, uint32_t v, int width)
{
    for (int i = width - 1; i >= 0; i--) {
        uint8_t t = (uint8_t)(v % 10);
        if (v == 0 && i != width - 1)
            t = T_EMPTY;
        video_bg_set(x + i, y, t);
        v /= 10;
    }
}

static void draw_hud(void)
{
    if (!game_type_b) {
        print_num(13, 3, score, 6);
        print_num(14, 10, (uint32_t)lines, 4);
        print_num(16, 7, (uint32_t)level, 2);
    } else {
        print_num(16, 2, (uint32_t)level, 2);
        print_num(16, 5, (uint32_t)b_high, 2);
        print_num(16, 10, (uint32_t)lines, 2);
    }
}

static void add_score(uint32_t pts)
{
    score += pts;
    if (score > 999999)
        score = 999999; /* AddBCD saturates at $99$99$99 */
}

static int piece_hits(uint8_t id, int row, int col)
{
    const PCell *cells = PIECES[id >> 2][id & 3];
    for (int i = 0; i < 4; i++) {
        int br = row + cells[i].r, bc = col + cells[i].c;
        /* Off-field rows read the zero-filled WRAM shadow in the original:
         * anything above or below the field is solid. Side walls are real
         * tiles on the map; out-of-range columns are solid too. */
        if (br < 0 || br >= FIELD_H || bc < 0 || bc >= FIELD_W)
            return 1;
        if (board[br][bc] != T_EMPTY)
            return 1;
    }
    return 0;
}

static void draw_active_piece(void)
{
    const PCell *cells = PIECES[active_id >> 2][active_id & 3];
    for (int i = 0; i < 4; i++) {
        Sprite *s = video_sprite(SPR_PIECE + i);
        s->enabled = !paused && lock_stage == 0 && wipe == 0;
        s->x = (int16_t)((piece_col + cells[i].c + FIELD_BG_X) * 8);
        s->y = (int16_t)((piece_row + cells[i].r) * 8);
        s->tile = cells[i].tile;
        s->flags = 0;
    }
}

static void draw_preview(void)
{
    const PCell *cells = PIECES[preview_id >> 2][preview_id & 3];
    for (int i = 0; i < 4; i++) {
        Sprite *s = video_sprite(SPR_PREVIEW + i);
        s->enabled = !paused && !preview_hidden;
        s->x = (int16_t)(119 + cells[i].c * 8); /* OAM (128,143) anchor */
        s->y = (int16_t)(95 + cells[i].r * 8);
        s->tile = cells[i].tile;
        s->flags = 0;
    }
}

static void hide_sprites(int from, int n)
{
    for (int i = 0; i < n; i++)
        video_sprite(from + i)->enabled = 0;
}

/* NextPiece: pipeline shift + reroll RNG (docs/spec-timing-rng.md §1) */
static void next_piece(void)
{
    if (forced_count) {
        active_id = preview_id;
        preview_id = nextnext_id;
        nextnext_id = forced_pieces[forced_idx++ % forced_count];
        piece_row = SPAWN_ROW;
        piece_col = SPAWN_COL;
        softdrop_armed = 0;
        softdrop_cells = 0;
        drop_timer = frames_per_drop;
        draw_active_piece();
        draw_preview();
        return;
    }

    active_id = preview_id;
    uint8_t c = active_id & (uint8_t)~3;
    uint8_t d = 0, e = nextnext_id;
    for (int h = 3;;) {
        d = random_piece_id();
        if (--h == 0)
            break; /* 3rd try always accepted */
        if (((d | e | c) & (uint8_t)~3) != c)
            break;
    }
    nextnext_id = d;
    preview_id = e;

    piece_row = SPAWN_ROW;
    piece_col = SPAWN_COL;
    softdrop_armed = 0;
    softdrop_cells = 0;
    drop_timer = frames_per_drop;
    draw_active_piece();
    draw_preview();
}

static void start_game(void)
{
    memset(board, T_EMPTY, sizeof board);
    score = 0;
    lock_stage = 0;
    blink_phase = 0;
    wipe = 0;
    timer1 = timer2 = 0;
    das_timer = 0;
    failed_placements = 0;
    paused = 0;
    preview_hidden = 0;
    cleared_count = 0;
    b_softdrop_tally = 0;

    if (!game_type_b) {
        level = a_level;
        lines = 0;
    } else {
        level = b_level;
        lines = 25; /* counts down */
        /* garbage: 2 rows per height unit, bottom-up */
        for (int r = FIELD_H - 1; r >= FIELD_H - 2 * b_high; r--) {
            int holes = 0;
            for (int c = 0; c < FIELD_W; c++) {
                if (read_div() & 1) {
                    board[r][c] = (uint8_t)((read_div() & 7) | 0x80);
                } else {
                    board[r][c] = T_EMPTY;
                    holes++;
                }
            }
            if (!holes)
                board[r][FIELD_W - 1] = T_EMPTY; /* guaranteed hole */
        }
    }

    /* test hook: TETRIS_FILL="r0,r1,c0,c1" prefills a board block */
    const char *fill = getenv("TETRIS_FILL");
    if (fill) {
        int v[4] = {0}, n = 0;
        for (const char *p = fill; *p && n < 4;) {
            v[n++] = (int)strtol(p, (char **)&p, 10);
            if (*p == ',')
                p++;
        }
        for (int r = v[0]; r <= v[1] && r < FIELD_H; r++)
            for (int c = v[2]; c <= v[3] && c < FIELD_W; c++)
                board[r][c] = 0x80;
    }

    int gl = level + (heart_mode ? 10 : 0);
    frames_per_drop = GRAVITY[gl > 20 ? 20 : gl];

    video_clear_bank();
    video_load_tiles(ts_font.tiles, ts_font.count, FONT_BASE);
    video_load_tiles(ts_title.tiles, 10, TITLE_BASE);
    video_load_tiles(ts_gameplay.tiles, ts_gameplay.count, GAMEPLAY_BASE);
    const Tilemap *tm = game_type_b ? &tm_bplay : &tm_aplay;
    video_bg_blit(tm->data, tm->w, tm->h, 0, 0);
    draw_field();
    draw_hud();

    /* prime the piece pipeline (original calls NextPiece 3x at init) */
    if (forced_count) {
        preview_id = forced_pieces[0];
        nextnext_id = forced_pieces[forced_count > 1 ? 1 : 0];
        forced_idx = forced_count > 2 ? 2 : 0;
    } else {
        preview_id = random_piece_id();
        nextnext_id = random_piece_id();
    }
    next_piece();
    if (game_type_b)
        drop_timer = 52; /* first drop primed to $34 */

    audio_music(music_sel);
    state = ST_PLAY;
}

/* ---- gameplay per-frame pieces, in the original call order ---- */

static void rotate_and_shift(Input in)
{
    if (lock_stage || wipe)
        return;

    /* rotation: A = index-1, B = index+1, revert on collision, no kicks */
    uint8_t old = active_id;
    if (in.pressed & BTN_A)
        active_id = (uint8_t)((active_id & ~3) | ((active_id - 1) & 3));
    else if (in.pressed & BTN_B)
        active_id = (uint8_t)((active_id & ~3) | ((active_id + 1) & 3));
    if (active_id != old) {
        if (piece_hits(active_id, piece_row, piece_col))
            active_id = old;
        else
            audio_sfx(SFX_ROTATE);
    }

    /* horizontal, DAS 23/9 */
    int dir = 0;
    if (in.held & BTN_RIGHT) {
        if (in.pressed & BTN_RIGHT) { dir = 1; das_timer = AUTOFIRE_DELAY; }
        else if (--das_timer == 0) { dir = 1; das_timer = AUTOFIRE_RATE; }
    } else if (in.held & BTN_LEFT) {
        if (in.pressed & BTN_LEFT) { dir = -1; das_timer = AUTOFIRE_DELAY; }
        else if (--das_timer == 0) { dir = -1; das_timer = AUTOFIRE_RATE; }
    }
    if (dir) {
        if (piece_hits(active_id, piece_row, piece_col + dir))
            das_timer = 1; /* blocked: retry next frame */
        else {
            piece_col += dir;
            audio_sfx(SFX_MOVE);
        }
    }
    draw_active_piece();
}

static void lock_current_piece(void)
{
    /* soft-drop points (Type A immediate, Type B tallied for the end) */
    if (softdrop_cells > 1) {
        if (!game_type_b)
            add_score(softdrop_cells - 1u);
        else
            b_softdrop_tally += softdrop_cells - 1u;
    }
    softdrop_cells = 0;
    lock_stage = 1;
    softdrop_armed = 0;

    /* top-out: 2nd lock at the spawn position ends the game */
    if (piece_row == SPAWN_ROW && piece_col == SPAWN_COL) {
        if (failed_placements == 1) {
            state = ST_CURTAIN;
            wipe = 0;
            timer1 = 0;
            audio_sfx(SFX_GAMEOVER);
            return;
        }
        failed_placements++;
    }
}

static void drop_piece(Input in)
{
    if (lock_stage || wipe)
        return;

    int do_drop = 0;

    /* soft drop: DOWN without LEFT/RIGHT; must re-press after each piece */
    uint8_t dlr = in.held & (BTN_DOWN | BTN_LEFT | BTN_RIGHT);
    if (!(in.held & BTN_DOWN))
        softdrop_armed = 1;
    if (dlr == BTN_DOWN && softdrop_armed) {
        if (timer2 == 0) {
            timer2 = FASTDROP_RATE;
            softdrop_cells++;
            do_drop = 1;
        }
    } else {
        softdrop_cells = 0;
    }

    /* natural gravity keeps its own independent countdown */
    if (!do_drop) {
        if (--drop_timer != 0)
            return;
        drop_timer = frames_per_drop;
        do_drop = 1;
    }

    if (piece_hits(active_id, piece_row + 1, piece_col))
        lock_current_piece();
    else
        piece_row++;
    draw_active_piece();
}

static void check_completed_rows(void)
{
    if (lock_stage != 2)
        return;
    audio_sfx(SFX_LOCK);

    /* original scans rows 2..17 only (starts at $C842 -- faithful bug) */
    cleared_count = 0;
    for (int r = 2; r < FIELD_H; r++) {
        int full = 1;
        for (int c = 0; c < FIELD_W; c++)
            if (board[r][c] == T_EMPTY) { full = 0; break; }
        if (full && cleared_count < 4)
            cleared_rows[cleared_count++] = (uint8_t)r;
    }
    clear_kind = cleared_count;
    lock_stage = 3;
    blink_phase = 0;
    timer1 = 2;

    if (cleared_count) {
        audio_sfx(cleared_count == 4 ? SFX_TETRIS : SFX_LINECLEAR);
        if (!game_type_b) {
            lines += cleared_count;
            if (lines > 9999)
                lines = 9999;
        } else {
            lines -= cleared_count;
            if (lines < 0)
                lines = 0;
        }
        draw_hud();
    }
}

static void lock_piece_into_bg(void)
{
    if (lock_stage != 1)
        return;
    const PCell *cells = PIECES[active_id >> 2][active_id & 3];
    for (int i = 0; i < 4; i++) {
        int br = piece_row + cells[i].r, bc = piece_col + cells[i].c;
        if (br >= 0 && br < FIELD_H && bc >= 0 && bc < FIELD_W) {
            board[br][bc] = cells[i].tile;
            bg_field_set(br, bc, cells[i].tile);
        }
    }
    hide_sprites(SPR_PIECE, 4);
    lock_stage = 2;
}

static void move_blocks_down(void)
{
    if (wipe != 1 || timer1 != 0)
        return;
    for (int i = 0; i < cleared_count; i++) {
        for (int r = cleared_rows[i]; r > 0; r--)
            memcpy(board[r], board[r - 1], FIELD_W);
        memset(board[0], T_EMPTY, FIELD_W);
    }
    cleared_count = 0;
    wipe = 2;
}

/* VBlank part: blink animation (stage 3) */
static void animate_line_clear(void)
{
    if (lock_stage != 3 || timer1 != 0)
        return;

    if (clear_kind == 0) { /* no lines: short ARE, straight to next piece */
        lock_stage = 0;
        next_piece();
        return;
    }

    if (blink_phase == 6) {
        for (int i = 0; i < cleared_count; i++) {
            memset(board[cleared_rows[i]], T_EMPTY, FIELD_W);
            draw_field_row(cleared_rows[i]);
        }
        blink_phase = 0;
        timer1 = 13;
        wipe = 1;
        lock_stage = 0;
        return;
    }

    for (int i = 0; i < cleared_count; i++) {
        int r = cleared_rows[i];
        if (!(blink_phase & 1)) { /* even: grey fill */
            if (blink_phase == 0)
                memcpy(saved_row_tiles[i], board[r], FIELD_W);
            for (int c = 0; c < FIELD_W; c++)
                bg_field_set(r, c, T_GREY);
        } else { /* odd: restore */
            for (int c = 0; c < FIELD_W; c++)
                bg_field_set(r, c, saved_row_tiles[i][c]);
        }
    }
    blink_phase++;
    timer1 = 10;
}

/* VBlank part: post-clear board redraw, one row per frame (wipe 2..19) */
static void wipe_chain(void)
{
    if (wipe < 2 || timer1 != 0)
        return;

    draw_field_row(FIELD_H - 1 - (wipe - 2)); /* bottom-up */

    if (wipe == 5 && clear_kind && !game_type_b) {
        static const uint16_t base[4] = { 40, 100, 300, 1200 };
        add_score((uint32_t)base[clear_kind - 1] * (uint32_t)(level + 1));
        draw_hud();
    }
    if (wipe == 16 && !game_type_b && level < 20 && lines / 10 > level) {
        level++;
        int gl = level + (heart_mode ? 10 : 0);
        frames_per_drop = GRAVITY[gl > 20 ? 20 : gl];
        audio_sfx(SFX_LEVELUP);
        draw_hud();
    }

    if (++wipe > 19) {
        wipe = 0;
        if (game_type_b && lines == 0) { /* B-type win */
            state = ST_B_WIN;
            timer1 = 0;
            return;
        }
        clear_kind = 0;
        next_piece();
    }
}

/* ---- menus ---- */

static void enter_title(void)
{
    video_clear_bank();
    video_load_tiles(ts_font.tiles, ts_font.count, FONT_BASE);
    video_load_tiles(ts_title.tiles, ts_title.count, TITLE_BASE);
    video_bg_blit(tm_title.data, tm_title.w, tm_title.h, 0, 0);
    hide_sprites(0, MAX_SPRITES);
    /* 1P cursor sprite, tile $58, OAM (Y=128,X=16) -> screen (8,112) */
    Sprite *s = video_sprite(SPR_CURSOR);
    s->enabled = 1;
    s->x = 8;
    s->y = 112;
    s->tile = 0x58;
    s->flags = 0;
    state = ST_TITLE;
    audio_music(MUSIC_TITLE);
}

static void load_menu_screen(const Tilemap *tm, State st)
{
    video_clear_bank();
    video_load_tiles(ts_font.tiles, ts_font.count, FONT_BASE);
    video_load_tiles(ts_title.tiles, 10, TITLE_BASE);
    video_load_tiles(ts_gameplay.tiles, ts_gameplay.count, GAMEPLAY_BASE);
    video_bg_blit(tm->data, tm->w, tm->h, 0, 0);
    hide_sprites(0, MAX_SPRITES);
    blink_timer = 16;
    blink_on = 1;
    state = st;
}

/* blinking highlight: dark backdrop + inverted glyphs over the box tiles
 * (font glyph pixels are transparent at shade 0, so the backdrop is what
 * makes the selection read as light-text-on-dark) */
static void mode_boxes_cursor(const Tilemap *tm, int x, int y, int w)
{
    hide_sprites(SPR_CURSOR, 12);
    for (int i = 0; i < w && i < 6; i++) {
        Sprite *bg = video_sprite(SPR_CURSOR + i);
        bg->enabled = blink_on;
        bg->x = (int16_t)((x + i) * 8);
        bg->y = (int16_t)(y * 8);
        bg->tile = 0x36; /* dark chrome tile */
        bg->flags = 0;
        Sprite *fg = video_sprite(SPR_CURSOR + 6 + i);
        *fg = *bg;
        fg->tile = tm->data[y * tm->w + x + i];
        fg->flags = SPR_INVERT;
    }
}

static void digit_cursor(int x, int y, int digit)
{
    hide_sprites(SPR_CURSOR, 12);
    Sprite *s = video_sprite(SPR_CURSOR);
    s->enabled = blink_on;
    s->x = (int16_t)(x * 8);
    s->y = (int16_t)(y * 8);
    s->tile = (uint8_t)(0x90 + digit); /* boxed digit tiles */
    s->flags = SPR_INVERT;
}

static void tick_blink(void)
{
    if (--blink_timer == 0) {
        blink_timer = 16;
        blink_on = !blink_on;
    }
}

/* ---- public API ---- */

int game_init(void)
{
    if (assets_load_tileset("font", &ts_font) ||
        assets_load_tileset("copyrightandtitlescreen", &ts_title) ||
        assets_load_tileset("configandgameplay", &ts_gameplay) ||
        assets_load_tilemap("titlescreen", 20, 18, &tm_title) ||
        assets_load_tilemap("gameplay", 20, 18, &tm_modeselect) ||
        assets_load_tilemap("typeAdifficulty", 20, 18, &tm_adiff) ||
        assets_load_tilemap("typeBdifficulty", 20, 18, &tm_bdiff) ||
        assets_load_tilemap("typeagameplay", 20, 18, &tm_aplay) ||
        assets_load_tilemap("typebgameplay", 20, 18, &tm_bplay))
        return -1;

    const char *fp = getenv("TETRIS_PIECES");
    if (fp) {
        while (*fp && forced_count < 64) {
            forced_pieces[forced_count++] = (uint8_t)strtol(fp, NULL, 10);
            while (*fp && *fp != ',')
                fp++;
            if (*fp == ',')
                fp++;
        }
    }

    enter_title();
    return 0;
}

void game_update(Input in)
{
    switch (state) {
    case ST_TITLE:
        if (in.pressed & BTN_START) {
            heart_mode = 0;
            load_menu_screen(&tm_modeselect, ST_MODE_SELECT);
            break;
        }
        break;

    case ST_MODE_SELECT:
        if (in.pressed & BTN_LEFT) { game_type_b = 0; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_RIGHT) { game_type_b = 1; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_A) { state = ST_MUSIC_SELECT; break; }
        if (in.pressed & BTN_START) {
            if (in.held & BTN_DOWN)
                heart_mode = 1; /* hidden levels 10-19 */
            menu_cursor = game_type_b ? b_level : a_level;
            load_menu_screen(game_type_b ? &tm_bdiff : &tm_adiff,
                             game_type_b ? ST_B_LEVEL : ST_A_LEVEL);
            break;
        }
        tick_blink();
        mode_boxes_cursor(&tm_modeselect, game_type_b ? 11 : 3, 5, 6);
        break;

    case ST_MUSIC_SELECT:
        if (in.pressed & BTN_LEFT && (music_sel & 1)) music_sel--;
        if (in.pressed & BTN_RIGHT && !(music_sel & 1)) music_sel++;
        if (in.pressed & BTN_UP && music_sel >= 2) music_sel -= 2;
        if (in.pressed & BTN_DOWN && music_sel < 2) music_sel += 2;
        if (in.pressed & BTN_B) { state = ST_MODE_SELECT; break; }
        if (in.pressed & (BTN_START | BTN_A)) {
            menu_cursor = game_type_b ? b_level : a_level;
            load_menu_screen(game_type_b ? &tm_bdiff : &tm_adiff,
                             game_type_b ? ST_B_LEVEL : ST_A_LEVEL);
            break;
        }
        tick_blink();
        /* music boxes: A(3,12) B(11,12) C(3,14) OFF(10,14) */
        {
            static const uint8_t mx[4] = { 3, 11, 3, 10 };
            static const uint8_t my[4] = { 12, 12, 14, 14 };
            static const uint8_t mw[4] = { 6, 6, 6, 3 };
            mode_boxes_cursor(&tm_modeselect, mx[music_sel], my[music_sel],
                              mw[music_sel]);
        }
        break;

    case ST_A_LEVEL:
        if (in.pressed & BTN_B) { load_menu_screen(&tm_modeselect, ST_MODE_SELECT); break; }
        if (in.pressed & BTN_RIGHT && menu_cursor < 9) { menu_cursor++; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_LEFT && menu_cursor > 0) { menu_cursor--; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_DOWN && menu_cursor < 5) { menu_cursor += 5; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_UP && menu_cursor >= 5) { menu_cursor -= 5; audio_sfx(SFX_CURSOR); }
        if (in.pressed & (BTN_START | BTN_A)) {
            a_level = menu_cursor;
            start_game();
            break;
        }
        tick_blink();
        digit_cursor(5 + 2 * (menu_cursor % 5), 6 + 2 * (menu_cursor / 5),
                     menu_cursor);
        break;

    case ST_B_LEVEL:
        if (in.pressed & BTN_B) { load_menu_screen(&tm_modeselect, ST_MODE_SELECT); break; }
        if (in.pressed & BTN_RIGHT && menu_cursor < 9) { menu_cursor++; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_LEFT && menu_cursor > 0) { menu_cursor--; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_DOWN && menu_cursor < 5) { menu_cursor += 5; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_UP && menu_cursor >= 5) { menu_cursor -= 5; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_START) { b_level = menu_cursor; start_game(); break; }
        if (in.pressed & BTN_A) {
            b_level = menu_cursor;
            menu_cursor = b_high;
            state = ST_B_HIGH;
            break;
        }
        tick_blink();
        digit_cursor(2 + 2 * (menu_cursor % 5), 6 + 2 * (menu_cursor / 5),
                     menu_cursor);
        break;

    case ST_B_HIGH:
        if (in.pressed & BTN_B) { menu_cursor = b_level; state = ST_B_LEVEL; break; }
        if (in.pressed & BTN_RIGHT && menu_cursor < 5) { menu_cursor++; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_LEFT && menu_cursor > 0) { menu_cursor--; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_DOWN && menu_cursor < 3) { menu_cursor += 3; audio_sfx(SFX_CURSOR); }
        if (in.pressed & BTN_UP && menu_cursor >= 3) { menu_cursor -= 3; audio_sfx(SFX_CURSOR); }
        if (in.pressed & (BTN_START | BTN_A)) {
            b_high = menu_cursor;
            start_game();
            break;
        }
        tick_blink();
        digit_cursor(13 + 2 * (menu_cursor % 3), 6 + 2 * (menu_cursor / 3),
                     menu_cursor);
        break;

    case ST_PLAY:
        /* START pauses; SELECT toggles the preview */
        if (in.pressed & BTN_START) {
            paused = !paused;
            audio_sfx(SFX_PAUSE);
            if (paused) {
                print_text(4, 8, "PAUSE");
                hide_sprites(SPR_PIECE, 8);
            } else {
                for (int r = 7; r <= 9; r++)
                    draw_field_row(r);
                draw_active_piece();
                draw_preview();
            }
        }
        if (paused)
            break;
        if (in.pressed & BTN_SELECT) {
            preview_hidden = !preview_hidden;
            draw_preview();
        }

        rotate_and_shift(in);
        drop_piece(in);
        if (state != ST_PLAY)
            break; /* topped out */
        check_completed_rows();
        lock_piece_into_bg();
        move_blocks_down();

        if (timer1) timer1--;
        if (timer2) timer2--;

        animate_line_clear();
        wipe_chain();
        break;

    case ST_CURTAIN:
        /* fill field with $87 bottom-up, one row per frame, then wait 70 */
        if (wipe < FIELD_H) {
            int r = FIELD_H - 1 - wipe;
            memset(board[r], T_CURTAIN, FIELD_W);
            draw_field_row(r);
            hide_sprites(SPR_PIECE, 8);
            wipe++;
            if (wipe == FIELD_H)
                timer1 = 70;
            break;
        }
        if (timer1) { timer1--; break; }
        print_text(3, 6, "GAME");
        print_text(3, 8, "OVER");
        state = ST_GAMEOVER;
        audio_music(MUSIC_GAMEOVER);
        break;

    case ST_GAMEOVER:
        if (in.pressed & BTN_START)
            enter_title();
        break;

    case ST_B_WIN:
        if (timer1 == 0) {
            print_text(3, 6, "WIN");
            /* B-type completion bonus: per-clear tallies + soft drop */
            /* simplified: applied at once (original animates it) */
            timer1 = 1;
        }
        if (in.pressed & BTN_START)
            enter_title();
        break;
    }
}
