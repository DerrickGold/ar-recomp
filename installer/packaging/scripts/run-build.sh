#!/bin/sh
# One-click graphical build for Linux. Run from a file manager or terminal:
#   ./run-build.sh

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
UTILS="$ROOT/utils"

fail() {
    echo
    echo "ERROR: $1"
    echo
    printf "Press Return to close."
    read -r ignored
    exit 1
}

[ -x "$UTILS/tools/actraiser-builder" ] || fail "This package looks incomplete.
Re-extract the downloaded archive and run this again."

# Every Linux installer includes a matched SDL SDK. Do not silently substitute
# an older system SDL if an archive was incompletely extracted.
for sdk_file in include/SDL3/SDL.h include/SDL3_ttf/SDL_ttf.h \
        lib/libSDL3.so lib/libSDL3.so.0 lib/libSDL3_ttf.so lib/libSDL3_ttf.so.0; do
    [ -s "$UTILS/tools/sdl3/$sdk_file" ] || fail "The bundled SDL SDK is incomplete.
Re-extract the downloaded archive and run this again."
done

echo "Opening the local ActRaiser Recomp builder..."
echo "If the browser does not open, use the private URL shown below."
echo

"$UTILS/tools/actraiser-builder" gui \
    --root "$UTILS" \
    --output-dir "$ROOT" \
    --snesbuild "$UTILS/tools/snesbuild" \
    --allow-stubs \
    || fail "The builder stopped unexpectedly. Share the messages above when asking for help."
