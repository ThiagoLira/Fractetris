#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO /* we feed it buffers so paths work the same on wasm */
#include "../vendor/stb_image.h"

#include "assets.h"

static char asset_root[512] = "assets";

/* 0xRRGGBB -> internal layout (R in low byte, matches ABGR8888 textures) */
static uint32_t rgb(uint32_t c)
{
    return (c >> 16 & 0xFF) | (c & 0xFF00) | (c & 0xFF) << 16;
}

/* Classic DMG green, shades 0..3 (set in assets_init_palette) */
static uint32_t palette[4];
static int palette_ready;

static void palette_default(void)
{
    static const uint32_t dmg[4] = { 0xE0F8D0, 0x88C070, 0x346856, 0x081820 };
    for (int i = 0; i < 4; i++)
        palette[i] = rgb(dmg[i]);
    palette_ready = 1;
}

void assets_set_root(const char *root)
{
    snprintf(asset_root, sizeof asset_root, "%s", root);
}

void assets_set_palette(const uint32_t pal[4])
{
    for (int i = 0; i < 4; i++)
        palette[i] = rgb(pal[i]);
    palette_ready = 1;
}

uint32_t assets_palette_color(int shade)
{
    if (!palette_ready)
        palette_default();
    return palette[shade & 3];
}

static uint8_t *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = n;
    return buf;
}

/* custom/<subpath> wins over <subpath> */
static uint8_t *read_asset(const char *subpath, long *size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/custom/%s", asset_root, subpath);
    uint8_t *buf = read_file(path, size);
    if (buf)
        return buf;
    snprintf(path, sizeof path, "%s/%s", asset_root, subpath);
    return read_file(path, size);
}

int assets_load_tileset(const char *name, Tileset *out)
{
    char subpath[256];
    snprintf(subpath, sizeof subpath, "tiles/%s.png", name);

    if (!palette_ready)
        palette_default();

    long size;
    uint8_t *buf = read_asset(subpath, &size);
    if (!buf) {
        fprintf(stderr, "assets: missing %s\n", subpath);
        return -1;
    }

    int w, h, channels;
    uint8_t *img = stbi_load_from_memory(buf, (int)size, &w, &h, &channels, 4);
    free(buf);
    if (!img || (w & 7) || (h & 7)) {
        fprintf(stderr, "assets: bad tileset %s\n", subpath);
        stbi_image_free(img);
        return -1;
    }

    /* Greyscale source = extracted original: map shades through the palette.
     * Anything with color or alpha = custom art, taken as-is. */
    int greyscale = channels <= 2;

    int tpr = w / 8, rows = h / 8;
    out->count = tpr * rows;
    out->tiles = calloc((size_t)out->count, sizeof(Tile));

    for (int i = 0; i < out->count; i++) {
        int ox = (i % tpr) * 8, oy = (i / tpr) * 8;
        for (int py = 0; py < 8; py++) {
            for (int px = 0; px < 8; px++) {
                const uint8_t *p = &img[((oy + py) * w + ox + px) * 4];
                uint32_t c;
                if (greyscale) {
                    int shade = (255 - p[0]) / 64; /* 255,170,85,0 -> 0..3 */
                    /* shade 0 transparent for sprite use; BG render ORs
                     * alpha back in so this costs nothing there.
                     * Shade is stashed in the low alpha bits so sprites can
                     * palette-invert (SPR_INVERT). */
                    c = palette[shade] | (shade ? 0xFC000000u | (uint32_t)shade << 24 : 0);
                } else {
                    c = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
                }
                out->tiles[i].px[py * 8 + px] = c;
            }
        }
    }
    stbi_image_free(img);
    return 0;
}

int assets_load_tilemap(const char *name, int w, int h, Tilemap *out)
{
    char subpath[256];
    snprintf(subpath, sizeof subpath, "tilemaps/%s.bin", name);

    long size;
    uint8_t *buf = read_asset(subpath, &size);
    if (!buf || size < (long)w * h) {
        fprintf(stderr, "assets: missing/short %s\n", subpath);
        free(buf);
        return -1;
    }
    out->data = buf;
    out->w = w;
    out->h = h;
    return 0;
}

int assets_load_blob(const char *subpath, Blob *out)
{
    out->data = read_asset(subpath, &out->size);
    if (!out->data) {
        fprintf(stderr, "assets: missing %s\n", subpath);
        return -1;
    }
    return 0;
}

void tileset_free(Tileset *t) { free(t->tiles); t->tiles = NULL; t->count = 0; }
void tilemap_free(Tilemap *m) { free(m->data); m->data = NULL; }
void blob_free(Blob *b) { free(b->data); b->data = NULL; b->size = 0; }
