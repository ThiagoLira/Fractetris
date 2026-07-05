# Fractetris

A falling-block puzzle game where **every pixel of the screen is also playing
the game**. You play a Game Boy-style puzzle game at 160×144 — and each of
those 23,040 pixels is a live micro-game with its own board, simulated
entirely on the GPU in fragment shaders, tinted by the color of its pixel in
the main game. Zoom in and the image you were just playing dissolves into
thousands of tiny self-playing boards.

**[Play it in the browser](https://thiagolira.blot.im/_projects/fractetris/index.html)**

## How it works

- The main game is written in C on a tiny tile engine (SDL2), with mechanics
  reimplemented from scratch to match the classic handheld feel:
  frame-based gravity tables, 23/9-frame delayed auto-shift, the quirky
  piece randomizer, authentic line-clear and game-over cadences. The
  mechanics research lives in `docs/`.
- The micro-games run in GLSL: game state lives in ping-ponged textures
  (one board texel per cell, four register texels per game) and two
  fragment-shader passes advance all 23,040 games each frame — piece
  placement AI, gravity, line clears, scoring, top-outs and restarts.
- One codebase, three artifacts: `tetris` (the classic-style game),
  `fractal` (the per-pixel meta-game), and a single-file amalgamation.

## Building

Requires CMake, SDL2, and a C compiler. Web builds use Emscripten (WebGL2).

```sh
cmake -B build && cmake --build build
./build/fractal          # the fractal meta-game
./build/tetris           # just the classic-style game

# web
emcmake cmake -B build-web && cmake --build build-web
python3 -m http.server -d build-web 8000
```

## Controls

Arrows/WASD move, Z/X rotate, Enter starts/pauses, Tab hides the preview.
In the fractal build: mouse wheel or `=`/`-` zooms, drag pans, `Home` fits
the whole wall of games on screen. Touch controls appear on mobile.
Joysticks work via the plain SDL joystick API.

## Assets

All art is original: an 8×8 pixel font and UI chrome authored for this
project, plus Rio-themed artwork (title scenery, toucan/mask/coffee block
tiles, palm-frond walls, and the Copacabana-wave game-over curtain)
generated with image models and hand-assembled into tilesets. Custom
tilesets/tilemaps can be dropped into `assets/custom/` to reskin the game —
full-color PNG is supported.

There is no sound yet; an original soundtrack is welcome (PRs open).

## License

MIT. Assets included in `assets/` are released under the same terms.
