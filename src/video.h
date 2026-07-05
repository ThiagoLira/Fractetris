#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>

/* GB-like tile video: a bank of 8x8 tiles ("VRAM"), a 32x32 background map
 * with pixel scroll, and a sprite list. Renders into a 160x144 RGBA buffer. */

#define GB_W 160
#define GB_H 144
#define BG_MAP_W 32
#define BG_MAP_H 32
#define MAX_BANK_TILES 512
#define MAX_SPRITES 40

#define SPR_FLIP_X 0x01
#define SPR_FLIP_Y 0x02
/* Render with shades inverted — stands in for the original's OBP1 palette
 * (used by the blinking menu cursors). */
#define SPR_INVERT 0x04

typedef struct {
    uint32_t px[64]; /* RGBA, row-major; alpha 0 = transparent (sprites) */
} Tile;

typedef struct {
    int16_t x, y;    /* top-left, on-screen pixels */
    uint16_t tile;
    uint8_t flags;
    uint8_t enabled;
} Sprite;

void video_clear_bank(void);
/* Copy tiles into the bank at base_index — the analogue of loading VRAM. */
void video_load_tiles(const Tile *tiles, int count, int base_index);
void video_bg_fill(uint8_t tile_index);
/* Blit a w*h tile-index map to the BG map at (tx, ty). */
void video_bg_blit(const uint8_t *map, int w, int h, int tx, int ty);
void video_bg_set(int tx, int ty, uint8_t tile_index);
void video_set_scroll(int x, int y);
Sprite *video_sprite(int i);
void video_render(uint32_t *fb /* GB_W*GB_H */);

#endif
