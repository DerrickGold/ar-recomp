#!/bin/sh
# Validate an existing generated game without repeating the full ROM build.
# Retains private generated data and logs in a new disk-backed test directory.
set -eu
test "$#" -eq 1 || { echo "usage: $0 /absolute/generated/ActRaiserRecomp.AppImage" >&2; exit 2; }
game=$(realpath "$1")
workspace=$(dirname "$game")
test -x "$game"
test "$(tr -d '\r\n' < "$game.portable")" = .
test_parent=$(realpath "${AR_BUILDER_TEST_ROOT:-$PWD}")
test_root=$(mktemp -d "$test_parent/game-player.XXXXXXXX")
echo "Retaining game-test results: $test_root"
mkdir "$test_root/tmp" "$test_root/Global Game"
export TMPDIR="$test_root/tmp" XDG_DATA_HOME="$test_root/global-data"
export APPIMAGE_EXTRACT_AND_RUN=1 AR_HEADLESS=1 AR_QUIT_FRAMES=60 AR_NO_RUN_DIR=1 AR_HEADLESS_VIDEO=0 SDL_AUDIODRIVER=dummy
unset AR_USER_DATA_DIR APPDIR APPIMAGE OWD
cd "$test_root"
timeout --kill-after=10s 120s "$game" --print-paths > "$test_root/portable-paths.log" 2>&1
grep -Fx "Data: $workspace" "$test_root/portable-paths.log"
timeout --kill-after=10s 120s "$game" > "$test_root/portable.log" 2>&1
cp "$game" "$test_root/Global Game/Renamed Game.AppImage"
game="$test_root/Global Game/Renamed Game.AppImage"
test ! -e "$game.portable"
timeout --kill-after=10s 120s "$game" --print-paths > "$test_root/global-paths.log" 2>&1
grep -Fx "Data: $XDG_DATA_HOME/ActRaiserRecomp/game" "$test_root/global-paths.log"
timeout --kill-after=10s 120s "$game" > "$test_root/global.log" 2>&1
echo 'PASS: generated game launches portably and globally'
