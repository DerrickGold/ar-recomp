#!/bin/sh
# One-click graphical build for macOS. Double-click this file in Finder.

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
UTILS="$ROOT/utils"

fail() {
    echo
    echo "ERROR: $1"
    echo
    printf "Press Return to close this window."
    read -r ignored
    exit 1
}

[ -x "$UTILS/tools/snesbuild" ] || fail "This package looks incomplete.
Re-extract the downloaded archive and run this again."

echo "Opening the local ActRaiser Recomp builder..."
echo "If the browser does not open, use the private URL shown below."
echo

"$UTILS/tools/snesbuild" gui \
    --root "$UTILS" \
    --output-dir "$ROOT" \
    --allow-stubs \
    || fail "The builder stopped unexpectedly. Share the messages above when asking for help."
