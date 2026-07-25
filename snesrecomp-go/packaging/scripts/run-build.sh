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

[ -x "$UTILS/tools/snesbuild" ] || fail "This package looks incomplete.
Re-extract the downloaded archive and run this again."

# Generic Linux bundles rely on the system package. The Steam Deck preset
# instead carries Valve's pinned x86_64 Steam Runtime SDL under tools/sdl3.
if [ ! -f "$UTILS/tools/sdl3/lib/libSDL3.so" ] \
        && ! pkg-config --exists sdl3 2>/dev/null \
        && ! ldconfig -p 2>/dev/null | grep -q 'libSDL3\.so'; then
    fail "SDL3 is not installed. Install it first, for example:
  Debian 13+/Ubuntu 24.04+:  sudo apt install libsdl3-dev
  (Ubuntu 22.04 / Debian 12 have no SDL3 package - use a PPA or build from source)
  Fedora:         sudo dnf install SDL3-devel
  Arch:           sudo pacman -S sdl3
then run this again."
fi

echo "Opening the local ActRaiser Recomp builder..."
echo "If the browser does not open, use the private URL shown below."
echo

"$UTILS/tools/snesbuild" gui \
    --root "$UTILS" \
    --output-dir "$ROOT" \
    --allow-stubs \
    || fail "The builder stopped unexpectedly. Share the messages above when asking for help."
