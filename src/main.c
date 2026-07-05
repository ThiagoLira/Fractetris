#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "assets.h"
#include "game.h"
#include "input.h"
#include "video.h"

#define WINDOW_SCALE 4
/* Original DMG frame rate */
#define FRAME_HZ 59.7275

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screen;
static uint32_t framebuffer[GB_W * GB_H];
static int running = 1;

/* The simulation always steps at the DMG's 59.73Hz; rendering runs at
 * whatever rate the display gives us, with an accumulator in between. */
static double sim_accum;
static Uint64 last_counter;

static void frame(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT ||
            (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
            running = 0;
        input_handle_event(&ev);
    }

    Uint64 now = SDL_GetPerformanceCounter();
    if (last_counter)
        sim_accum += (double)(now - last_counter) /
                     (double)SDL_GetPerformanceFrequency();
    last_counter = now;

    const double step = 1.0 / FRAME_HZ;
    if (sim_accum > 5 * step) /* stall guard (window drag, tab switch) */
        sim_accum = 5 * step;
    while (sim_accum >= step) {
        game_update(input_poll());
        sim_accum -= step;
    }

    video_render(framebuffer);
    SDL_UpdateTexture(screen, NULL, framebuffer, GB_W * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, screen, NULL, NULL);
    SDL_RenderPresent(renderer);
}

#ifdef __EMSCRIPTEN__
static void em_frame(void)
{
    if (!running)
        emscripten_cancel_main_loop();
    frame();
}
#endif

#ifndef TETRIS_NO_MAIN
int main(int argc, char **argv)
{
    if (argc > 1)
        assets_set_root(argv[1]);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    input_init();

    window = SDL_CreateWindow("Fractetris (classic)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        GB_W * WINDOW_SCALE, GB_H * WINDOW_SCALE, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, GB_W, GB_H);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
    screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, GB_W, GB_H);

    game_seed((unsigned)SDL_GetPerformanceCounter());
    if (game_init())
        return 1;

    /* TETRIS_SCRIPT="60x,1xS,30x" — comma list of <frames>x<buttons>
     * (S=start E=select A B L R U D); runs headless, then TETRIS_SHOT. */
    const char *script = getenv("TETRIS_SCRIPT");
    if (script) {
        uint8_t prev = 0;
        const char *p = script;
        while (*p) {
            int n = 0;
            while (*p >= '0' && *p <= '9')
                n = n * 10 + *p++ - '0';
            uint8_t held = 0;
            if (*p == 'x')
                p++;
            for (; *p && *p != ','; p++) {
                switch (*p) {
                case 'A': held |= BTN_A; break;
                case 'B': held |= BTN_B; break;
                case 'S': held |= BTN_START; break;
                case 'E': held |= BTN_SELECT; break;
                case 'L': held |= BTN_LEFT; break;
                case 'R': held |= BTN_RIGHT; break;
                case 'U': held |= BTN_UP; break;
                case 'D': held |= BTN_DOWN; break;
                }
            }
            if (*p == ',')
                p++;
            for (int i = 0; i < n; i++) {
                Input in = { held, (uint8_t)(held & ~prev) };
                game_update(in);
                prev = held;
            }
        }
    }

    /* TETRIS_SHOT=file.ppm: render one frame, dump it, exit (for testing) */
    const char *shot = getenv("TETRIS_SHOT");
    if (shot) {
        video_render(framebuffer);
        FILE *f = fopen(shot, "wb");
        fprintf(f, "P6\n%d %d\n255\n", GB_W, GB_H);
        for (int i = 0; i < GB_W * GB_H; i++) {
            uint8_t rgb[3] = { framebuffer[i] & 0xFF,
                               framebuffer[i] >> 8 & 0xFF,
                               framebuffer[i] >> 16 & 0xFF };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        SDL_Quit();
        return 0;
    }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(em_frame, 0 /* let vsync drive it */, 1);
#else
    while (running) {
        Uint64 t0 = SDL_GetPerformanceCounter();
        frame();
        /* vsync normally paces us; this caps the no-vsync fallback */
        double elapsed = (double)(SDL_GetPerformanceCounter() - t0) /
                         (double)SDL_GetPerformanceFrequency();
        double target = 1.0 / FRAME_HZ;
        if (elapsed < target * 0.25)
            SDL_Delay(1);
    }
#endif

    SDL_Quit();
    return 0;
}
#endif /* TETRIS_NO_MAIN */
