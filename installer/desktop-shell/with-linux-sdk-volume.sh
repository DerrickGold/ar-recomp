#!/bin/sh
# Linux has case-distinct headers. This is native macOS storage, not a VM.
# The lock prevents two releases from mounting/detaching the same cached image.
set -eu
test "$#" -ge 2 || { echo 'usage: with-linux-sdk-volume.sh cache command [args...]' >&2; exit 2; }
cache=$1
shift
mkdir -p "$cache"
cache=$(CDPATH= cd -- "$cache" && pwd -P)
image="$cache/linux-sdk.sparseimage"
volume="$cache/linux-sdk-volume"
lock="$cache/linux-sdk.lock"
for path in "$image" "$volume" "$lock"; do
    test ! -L "$path" || { echo "Refusing symlink: $path" >&2; exit 1; }
done
mkdir "$lock" 2>/dev/null || {
    echo "Linux SDK is in use, or a previous build was interrupted: $lock" >&2
    echo 'Check for an active build/mount before removing a stale lock.' >&2
    exit 1
}
attached=0
made_volume=0
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if test "$attached" = 1; then
        if ! hdiutil detach "$volume"; then
            echo "Could not detach $volume; keeping the lock. Close processes using it, detach it, then remove the empty mountpoint and lock directories." >&2
            exit 1
        fi
    fi
    if test "$made_volume" = 1; then rmdir "$volume"; fi
    rmdir "$lock"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
test ! -e "$volume" || { echo "Refusing existing mountpoint: $volume" >&2; exit 1; }
if test ! -e "$image"; then
    hdiutil create -size 12g -fs 'Case-sensitive APFS' -type SPARSE \
        -volname ActRaiserBuilderLinuxSDK "$image"
fi
mkdir "$volume"
made_volume=1
# Keep the lock if attach partially succeeds and detach cannot be confirmed.
attached=1
hdiutil attach "$image" -nobrowse -owners off -mountpoint "$volume"
BUILDER_LINUX_SDK_VOLUME="$volume" "$@"
