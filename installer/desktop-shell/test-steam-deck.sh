#!/bin/sh
# Run a TEST-ONLY (--smoke / BUILDER_SMOKE_TEST=ON) AppImage from Steam Deck
# Desktop Mode. No sudo, package installation, OS unlock or VM is needed.
# This deliberately keeps logs and synthetic/private game output for review.
set -eu
test "$#" -eq 1 || { echo "usage: sh $0 /absolute/ActRaiserRecompBuilder-smoke.AppImage" >&2; exit 2; }
test "$(uname -m)" = x86_64 || { echo 'This test targets an x86-64 Steam Deck.' >&2; exit 1; }
test -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" || { echo 'Run this in Steam Deck Desktop Mode, inside its graphical session.' >&2; exit 1; }
grep -Eq '^ID="?steamos"?$' /etc/os-release || { echo 'This acceptance test requires SteamOS.' >&2; exit 1; }
for tool in timeout realpath sha256sum getconf; do command -v "$tool" >/dev/null; done
glibc=$(getconf GNU_LIBC_VERSION)
version=${glibc#glibc }
major=${version%%.*}
minor=${version#*.}; minor=${minor%%.*}
if test "$major" -lt 2 || { test "$major" -eq 2 && test "$minor" -lt 36; }; then
    echo "This Builder requires glibc 2.36 or newer; found $glibc. Update SteamOS normally, not individual system libraries." >&2
    exit 1
fi
source_image=$(realpath "$1")
test -f "$source_image"
test_parent=$(realpath "${AR_BUILDER_TEST_ROOT:-$PWD}")
test_root=$(mktemp -d "$test_parent/builder-deck.XXXXXXXX")
echo "Retaining Deck acceptance results: $test_root"
mkdir "$test_root/Portable Builder" "$test_root/Unrelated CWD" "$test_root/tmp"
cp "$source_image" "$test_root/Portable Builder/Renamed Builder.AppImage"
image="$test_root/Portable Builder/Renamed Builder.AppImage"
chmod 755 "$image"
printf 'Builder Data\n' > "$image.portable"
sha256sum "$image" > "$test_root/image.sha256"
cp /etc/os-release "$test_root/os-release"
printf '%s\n' "$glibc" > "$test_root/glibc.txt"
export XDG_DATA_HOME="$test_root/global-data" XDG_CONFIG_HOME="$test_root/config" XDG_CACHE_HOME="$test_root/cache" TMPDIR="$test_root/tmp"
unset AR_USER_DATA_DIR APPDIR APPIMAGE OWD LD_LIBRARY_PATH LD_PRELOAD
cd "$test_root/Unrelated CWD"
run_probe() {
    label=$1
    probe_timeout=180s
    if test -n "${AR_BUILDER_SMOKE_ROM:-}"; then probe_timeout=1800s; fi
    if ! timeout --kill-after=10s "$probe_timeout" "$image" --jobs 2 > "$test_root/$label.log" 2>&1; then
        tail -n 80 "$test_root/$label.log"
        echo "FAIL: $label did not exit successfully" >&2; exit 1
    fi
    grep '^BUILDER_RENDERER_SMOKE PASS ' "$test_root/$label.log" || {
        tail -n 80 "$test_root/$label.log"
        echo 'FAIL: no explicit renderer PASS. Use a test-only smoke AppImage, not the normal release.' >&2; exit 1
    }
}
# Required double-click-equivalent mounted launch; failure is not hidden by
# forcing extraction. The same artifact's FUSE-free fallback is tested below.
unset APPIMAGE_EXTRACT_AND_RUN
run_probe portable-mounted
test -d "$test_root/Portable Builder/Builder Data/utils/tools"
test -f "$test_root/Portable Builder/ActRaiserRecomp/.actraiser-import.json"
test ! -e "$XDG_DATA_HOME/ActRaiserRecomp/installer"
unset AR_BUILDER_SMOKE_ROM
mv "$test_root/Portable Builder" "$test_root/Relocated Builder"
image="$test_root/Relocated Builder/Renamed Builder.AppImage"
run_probe relocated-mounted
mv "$image.portable" "$image.portable.saved"
run_probe global-mounted
test -d "$XDG_DATA_HOME/ActRaiserRecomp/installer/workspace/utils/tools"
export APPIMAGE_EXTRACT_AND_RUN=1
run_probe global-extracted
original_hash=$(cut -d ' ' -f 1 "$test_root/image.sha256")
test "$(sha256sum "$image" | cut -d ' ' -f 1)" = "$original_hash"
echo 'PASS: SteamOS Desktop Mode renderer, import prompt, media decoder, portable/global storage, relocation, mounted/extracted launch.'
echo 'Still verify visually: fonts/scaling, folder picker, audible audio, accelerated graphics, controller/touch input and the generated game.'
