#!/bin/bash
# One-click build-and-play for Linux. Run from a file manager or a terminal:
#   ./run-build.sh
# Put your game ROM (.sfc) in this same folder first. See README.txt.

ROOT="$(cd "$(dirname "$0")" && pwd)"
UTILS="$ROOT/utils"

fail() {
    echo
    echo "ERROR: $1"
    echo
    read -r -p "Press Return to close."
    exit 1
}

[ -d "$UTILS" ] || fail "This package looks incomplete (the utils folder is missing).
Re-extract the downloaded archive and run this again."

cd "$ROOT" || fail "cannot enter $ROOT"

ROM=""
for candidate in *.sfc *.smc; do
    [ -f "$candidate" ] && ROM="$candidate" && break
done
[ -n "$ROM" ] || fail "No ROM found. Copy your game ROM (a .sfc file) into this folder:
  $ROOT
then run this again."

# Linux has no single portable SDL3 redistributable, so it is not bundled;
# rely on the system package. Give a clear hint if it is missing.
if ! pkg-config --exists sdl3 2>/dev/null \
        && [ ! -e /usr/lib/libSDL3.so.0 ] \
        && [ ! -e /usr/lib/x86_64-linux-gnu/libSDL3.so.0 ]; then
    fail "SDL3 is not installed. Install it first, for example:
  Debian/Ubuntu:  sudo apt install libsdl3-dev
  Fedora:         sudo dnf install SDL3-devel
  Arch:           sudo pacman -S sdl3
then run this again."
fi

echo "Using ROM: $ROM"
echo "Building — the first run takes a few minutes..."
echo

"$UTILS/tools/snesbuild" all --hermetic --root "$UTILS" --rom "$ROOT/$ROM" --allow-stubs \
    || fail "The build did not complete. The messages above say why; share them when asking for help."

BUILT="$(find "$UTILS/build/hermetic" -maxdepth 1 -type f -executable ! -name '*.so*' | head -1)"
[ -n "$BUILT" ] || fail "Build finished but no game program was found."
BIN_NAME="$(basename "$BUILT")"

# Put the finished game (and its media library, if bundled) in this folder.
cp -f "$BUILT" "$ROOT/$BIN_NAME"
for lib in "$UTILS"/build/hermetic/*.so*; do
    [ -f "$lib" ] && cp -f "$lib" "$ROOT/"
done

# Create a one-click "play again" script next to the game.
PLAY="$ROOT/run-game.sh"
cat > "$PLAY" <<EOF
#!/bin/bash
# Runs the already-built game. Created by run-build.sh.
ROOT="\$(cd "\$(dirname "\$0")" && pwd)"
cd "\$ROOT/utils" || exit 1
ROM=""
for c in "\$ROOT"/*.sfc "\$ROOT"/*.smc; do [ -f "\$c" ] && ROM="\$c" && break; done
exec "\$ROOT/$BIN_NAME" "\$ROM" --config config.ini
EOF
chmod +x "$PLAY"

echo
echo "Build complete - this was a one time process. Now you can run ./run-game.sh to start the game."
exit 0