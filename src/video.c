#include <string.h>
#include "assets.h"
#include "video.h"

static Tile bank[MAX_BANK_TILES];
static uint8_t bg_map[BG_MAP_H][BG_MAP_W];
static Sprite sprites[MAX_SPRITES];
static int scroll_x, scroll_y;

void video_clear_bank(void)
{
    memset(bank, 0, sizeof bank);
    memset(bg_map, 0, sizeof bg_map);
    memset(sprites, 0, sizeof sprites);
    scroll_x = scroll_y = 0;
}

void video_load_tiles(const Tile *tiles, int count, int base_index)
{
    if (base_index < 0 || base_index >= MAX_BANK_TILES)
        return;
    if (count > MAX_BANK_TILES - base_index)
        count = MAX_BANK_TILES - base_index;
    memcpy(&bank[base_index], tiles, (size_t)count * sizeof(Tile));
}

void video_bg_fill(uint8_t tile_index)
{
    memset(bg_map, tile_index, sizeof bg_map);
}

void video_bg_blit(const uint8_t *map, int w, int h, int tx, int ty)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            bg_map[(ty + y) & (BG_MAP_H - 1)][(tx + x) & (BG_MAP_W - 1)] =
                map[y * w + x];
}

void video_bg_set(int tx, int ty, uint8_t tile_index)
{
    bg_map[ty & (BG_MAP_H - 1)][tx & (BG_MAP_W - 1)] = tile_index;
}

void video_set_scroll(int x, int y)
{
    scroll_x = x;
    scroll_y = y;
}

Sprite *video_sprite(int i)
{
    return &sprites[i];
}

void video_render(uint32_t *fb)
{
    for (int y = 0; y < GB_H; y++) {
        int by = (y + scroll_y) & (BG_MAP_H * 8 - 1);
        for (int x = 0; x < GB_W; x++) {
            int bx = (x + scroll_x) & (BG_MAP_W * 8 - 1);
            const Tile *t = &bank[bg_map[by >> 3][bx >> 3]];
            fb[y * GB_W + x] = t->px[(by & 7) * 8 + (bx & 7)] | 0xFF000000u;
        }
    }

    for (int i = 0; i < MAX_SPRITES; i++) {
        const Sprite *s = &sprites[i];
        if (!s->enabled || s->tile >= MAX_BANK_TILES)
            continue;
        const Tile *t = &bank[s->tile];
        for (int py = 0; py < 8; py++) {
            int y = s->y + py;
            if (y < 0 || y >= GB_H)
                continue;
            int sy = (s->flags & SPR_FLIP_Y) ? 7 - py : py;
            for (int px = 0; px < 8; px++) {
                int x = s->x + px;
                if (x < 0 || x >= GB_W)
                    continue;
                int sx = (s->flags & SPR_FLIP_X) ? 7 - px : px;
                uint32_t c = t->px[sy * 8 + sx];
                uint32_t a = c >> 24;
                if (!a)
                    continue;
                if (s->flags & SPR_INVERT) {
                    /* map luminance to a GB shade, then flip it through the
                     * palette; works for shade-encoded and full-color tiles */
                    uint32_t r = c & 0xFF, g = c >> 8 & 0xFF,
                             b = c >> 16 & 0xFF;
                    int lum = (int)((r * 77 + g * 150 + b * 29) >> 8);
                    /* bright pixel -> dark palette entry and vice versa */
                    c = assets_palette_color(lum >> 6);
                }
                fb[y * GB_W + x] = c | 0xFF000000u;
            }
        }
    }
}
