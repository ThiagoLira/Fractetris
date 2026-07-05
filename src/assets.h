#ifndef ASSETS_H
#define ASSETS_H

#include <stdint.h>
#include "video.h"

/* Assets live under a root directory (default "assets"). Every file is first
 * looked up in <root>/custom/<subpath>; if present it overrides the default.
 * Custom tilesets are ordinary PNGs (same dimensions as the originals, 16
 * tiles per row); full-color RGBA is allowed and alpha marks sprite
 * transparency. The extracted originals are greyscale GB shades, which get
 * mapped through the active palette on load. */

typedef struct {
    Tile *tiles;
    int count;
} Tileset;

typedef struct {
    uint8_t *data; /* tile indices, row-major */
    int w, h;
} Tilemap;

typedef struct {
    uint8_t *data;
    long size;
} Blob;

void assets_set_root(const char *root);
/* Directory (under the root) checked before the defaults; "custom" by
 * default. Point it at a pack (e.g. "packs/night") to switch skins. */
void assets_set_override(const char *dir);
const char *assets_root(void);
/* 4 colors for GB shades 0..3 (lightest..darkest), 0xRRGGBB. */
void assets_set_palette(const uint32_t pal[4]);
/* Active palette color in internal pixel layout (R low byte). */
uint32_t assets_palette_color(int shade);

int assets_load_tileset(const char *name, Tileset *out); /* tiles/<name>.png */
int assets_load_tilemap(const char *name, int w, int h, Tilemap *out);
int assets_load_blob(const char *subpath, Blob *out);

void tileset_free(Tileset *t);
void tilemap_free(Tilemap *m);
void blob_free(Blob *b);

#endif
