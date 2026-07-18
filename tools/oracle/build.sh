#!/usr/bin/env bash
# Build the snesref differential oracle (macOS).
# Requires: SDL2 (brew), and a libretro SNES core at runtime (snes9x_libretro.dylib).
set -euo pipefail
cd "$(dirname "$0")"

SDL_CFLAGS=$(pkg-config --cflags sdl2)
SDL_LIBS=$(pkg-config --libs sdl2)
LIBRETRO_H_DIR="."

clang++ -std=c++17 -O2 -Wall \
  $SDL_CFLAGS -I"$LIBRETRO_H_DIR" \
  snesref.cpp -o snesref \
  $SDL_LIBS

echo "built: $(pwd)/snesref"
