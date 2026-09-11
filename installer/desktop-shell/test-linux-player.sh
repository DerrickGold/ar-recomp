#!/bin/sh
# Run a test-only Builder AppImage on a clean Linux player VM. No downloads.
# Optional AR_BUILDER_SMOKE_ROM enables a full private-ROM-to-game build.
# Results (including derived game data) are retained in the printed directory.
set -eu
test "$#" -eq 1 || { echo "usage: $0 /absolute/path/to/smoketest.AppImage" >&2; exit 2; }
source_image=$(realpath "$1")
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
test -f "$source_image"
for tool in go cmake cc gcc clang; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "Clean-player check failed: system $tool is installed" >&2
        exit 1
    fi
done
if ldconfig -p | grep -E 'lib(webkit2gtk|gtk-3|SDL3)[.-]' >/dev/null; then
    echo 'Clean-player check failed: system WebKit, GTK3 or SDL3 is installed' >&2
    exit 1
fi
for tool in xvfb-run dbus-run-session timeout sha256sum; do command -v "$tool" >/dev/null; done
# A full build and AppImage extraction can exceed RAM-backed /tmp capacity.
# Keep all bulky test data on the caller's disk, with an isolated temp directory.
test_parent=$(realpath "${AR_BUILDER_TEST_ROOT:-$PWD}")
test_root=$(mktemp -d "$test_parent/builder-player.XXXXXXXX")
echo "Retaining player-test results: $test_root"
mkdir "$test_root/Portable Builder" "$test_root/Unrelated CWD" "$test_root/tmp"
export TMPDIR="$test_root/tmp"
image="$test_root/Portable Builder/Renamed Builder.AppImage"
cp "$source_image" "$image"
chmod 755 "$image"
sha256sum "$image" > "$test_root/image.sha256"
printf 'Builder Data\n' > "$image.portable"
export XDG_DATA_HOME="$test_root/global-data" XDG_CACHE_HOME="$test_root/cache" XDG_CONFIG_HOME="$test_root/config"
unset AR_USER_DATA_DIR APPDIR APPIMAGE OWD
cd "$test_root/Unrelated CWD"
run_probe() {
    label=$1
    shift
    probe_timeout=180s
    if test -n "${AR_BUILDER_SMOKE_ROM:-}"; then probe_timeout=1800s; fi
    if ! timeout --kill-after=10s "$probe_timeout" dbus-run-session -- xvfb-run -a "$@" --jobs 1 > "$test_root/$label.log" 2>&1; then
        tail -n 80 "$test_root/$label.log"
        echo "FAIL: $label did not exit successfully" >&2
        exit 1
    fi
    if ! grep '^BUILDER_RENDERER_SMOKE PASS ' "$test_root/$label.log"; then
        tail -n 80 "$test_root/$label.log"
        echo "FAIL: $label did not report renderer PASS" >&2
        exit 1
    fi
}
export APPIMAGE_EXTRACT_AND_RUN=1
run_probe portable "$image"
workspace="$test_root/Portable Builder/Builder Data"
test -d "$workspace/utils/tools"
test ! -e "$XDG_DATA_HOME/ActRaiserRecomp/installer"
game_output="$test_root/Portable Builder/ActRaiserRecomp"
test -d "$game_output/game-assets"
if test -n "${AR_BUILDER_SMOKE_ROM:-}"; then
    grep 'PASS .*full game build' "$test_root/portable.log"
    AR_BUILDER_TEST_ROOT="$test_root" sh "$script_dir/test-linux-game.sh" "$game_output/ActRaiserRecomp.AppImage"
fi
unset AR_BUILDER_SMOKE_ROM
# Move all three portable pieces together and warm-start from an unrelated CWD.
mv "$test_root/Portable Builder" "$test_root/Relocated Builder"
image="$test_root/Relocated Builder/Renamed Builder.AppImage"
run_probe relocated "$image"
test ! -e "$XDG_DATA_HOME/ActRaiserRecomp/installer"
mv "$image.portable" "$image.portable.saved"
run_probe global "$image"
test -d "$XDG_DATA_HOME/ActRaiserRecomp/installer/workspace/utils/tools"
if test -c /dev/fuse && test -r /dev/fuse && test -w /dev/fuse &&
    { command -v fusermount3 >/dev/null 2>&1 || command -v fusermount >/dev/null 2>&1; }; then
    unset APPIMAGE_EXTRACT_AND_RUN
    run_probe fuse "$image"
    echo 'PASS: FUSE-mounted launch'
else
    echo 'SKIP: FUSE device or mount helper is unavailable; extract-and-run was tested'
fi
original_hash=$(cut -d ' ' -f 1 "$test_root/image.sha256")
test "$(sha256sum "$image" | cut -d ' ' -f 1)" = "$original_hash"
echo 'PASS: clean-player Builder, portable relocation, global workspace, unchanged AppImage'
