# Fractetris

A falling-block puzzle game where **every pixel of the screen is also playing
the game**. You play a Game Boy-style puzzle game at 160×144 — and each of
those 23,040 pixels is a live micro-game with its own board, simulated
entirely on the GPU in fragment shaders, tinted by the color of its pixel in
the main game. Zoom in and the image you were just playing dissolves into
thousands of tiny self-playing boards.

Under the hood this is a **full recreation of the original Game Boy game**,
recompiled by hand twice: the original assembly (Tetris World, Rev 1
disassembly) was reverse-engineered routine by routine into portable C, and
that C game loop was then ported again, line for line, into GLSL — so the
23,040 micro-games and the big game you play are all running the same
original game logic.

**[Play it in the browser](https://thiagolira.blot.im/_projects/fractetris/index.html)**

## How it works

- **The C codebase is a recreation of the original GB assembly, not an
  homage.** Game logic was ported routine-for-routine from the Tetris
  (World, Rev 1) disassemblies: the `FramesPerDropTable` at `$1B61`, the
  23/9-frame delayed auto-shift, the DIV-register piece randomizer with its
  2-reroll anti-repeat quirk, spawn-lock top-outs, line-clear blink and
  progressive-wipe cadences, and even the faithful bug where the full-line
  scan only covers rows 2–17. State variables mirror the original HRAM
  addresses, and every constant in `src/game.c` cites the reverse-engineering
  notes in `docs/spec-*.md`, which cite the assembly.
- **The GLSL codebase is a second recompilation of the same game.** The
  entire game loop runs as a pure per-pixel step function (Doom-in-a-shader
  style): game state lives in ping-ponged textures (one board texel per
  cell, five register texels per game) and three fragment-shader passes
  advance all 23,040 games each frame — the same call order, stage timings,
  RNG chain, input path (a virtual player's button bytes go through the real
  DAS code) and scoring as the C port, which is to say, as the original ROM.
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
project, plus a Rio-themed take on the classic 1989 title screen (TETRIS™
over Corcovado instead of the Kremlin), beveled block tiles, palm-frond
walls, and the Copacabana-wave game-over curtain — generated with image
models and hand-assembled into tilesets. Custom tilesets/tilemaps can be
dropped into `assets/custom/` to reskin the game — full-color PNG is
supported, and `assets/packs/` ships a night palette switched live with `T`.

There is no sound yet; an original soundtrack is welcome (PRs open).

## License

MIT. Assets included in `assets/` are released under the same terms.
