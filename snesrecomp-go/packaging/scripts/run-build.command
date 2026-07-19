#!/bin/bash
# One-click build-and-play for macOS. Double-click this file in Finder.
# Put your game ROM (.sfc) in this same folder first. See README.txt.

ROOT="$(cd "$(dirname "$0")" && pwd)"
UTILS="$ROOT/utils"

fail() {
    echo
    echo "ERROR: $1"
    echo
    read -r -p "Press Return to close this window."
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

echo "Using ROM: $ROM"
echo "Building — the first run takes a few minutes..."
echo

"$UTILS/tools/snesbuild" all --hermetic --root "$UTILS" --rom "$ROOT/$ROM" --allow-stubs \
    || fail "The build did not complete. The messages above say why; share them when asking for help."

BUILT="$(find "$UTILS/build/hermetic" -maxdepth 1 -type f -perm +111 ! -name '*.dylib' | head -1)"
[ -n "$BUILT" ] || fail "Build finished but no game program was found."
BIN_NAME="$(basename "$BUILT")"

# Put the finished game (and its media library) in this folder.
cp -f "$BUILT" "$ROOT/$BIN_NAME"
for lib in "$UTILS"/build/hermetic/*.dylib; do
    [ -f "$lib" ] && cp -f "$lib" "$ROOT/"
done

# Create a one-click "play again" script next to the game.
PLAY="$ROOT/run-game.command"
cat > "$PLAY" <<EOF
#!/bin/bash
# Runs the already-built game. Created by run-build.command.
ROOT="\$(cd "\$(dirname "\$0")" && pwd)"
cd "\$ROOT/utils" || exit 1
ROM=""
for c in "\$ROOT"/*.sfc "\$ROOT"/*.smc; do [ -f "\$c" ] && ROM="\$c" && break; done
exec "\$ROOT/$BIN_NAME" "\$ROM" --config config.ini
EOF
chmod +x "$PLAY"

echo
echo "Build complete - this was a one time process. Now you can open `run-game.bat` to start the game"
exit 0