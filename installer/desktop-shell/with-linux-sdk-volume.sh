#!/bin/sh
# Linux has case-distinct headers. This is native macOS storage, not a VM.
# The lock prevents two releases from mounting/detaching the same cached image.
set -eu
test "$#" -ge 2 || { echo 'usage: with-linux-sdk-volume.sh cache command [args...]' >&2; exit 2; }
cache=$1
shift
mkdir -p "$cache"
cache=$(CDPATH='' cd -- "$cache" && pwd -P)
image="$cache/linux-sdk.sparseimage"
lock="$cache/linux-sdk.lock"
for path in "$image" "$lock"; do
    test ! -L "$path" || { echo "Refusing symlink: $path" >&2; exit 1; }
done
mkdir "$lock" 2>/dev/null || {
    echo "Linux SDK is in use, or a previous build was interrupted: $lock" >&2
    echo "Once builds on this host are idle, check 'hdiutil info' and detach $image if present before removing the empty lock directory." >&2
    exit 1
}
attached=0
attempted_attach=0
volume=
image_is_attached() {
    listing=$(hdiutil info) || return 2
    printf '%s\n' "$listing" | grep -Fxq "image-path      : $image"
}
keep_lock() {
    echo "Could not confirm detach of $image; keeping $lock. Check the disk image and mountpoint $volume before removing the lock." >&2
    exit 1
}
cleanup() {
    result=$?
    # Ctrl-C kills the parent CMake first, leaving stdout/stderr unread: never
    # let SIGPIPE or a repeated signal turn a good detach into a stale lock.
    trap - EXIT
    trap '' HUP INT TERM PIPE
    if test "$attempted_attach" = 1; then
        if test "$attached" = 1 || image_is_attached; then
            hdiutil detach "$volume" || keep_lock
        elif test "$?" -ne 1; then
            keep_lock
        fi
    fi
    if test -n "$volume"; then rmdir "$volume"; fi
    rmdir "$lock"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
if image_is_attached; then
    echo "Linux SDK image is already attached: $image" >&2
    exit 1
elif test "$?" -ne 1; then
    echo "Could not inspect disk images before attaching $image" >&2
    exit 1
fi
if test ! -e "$image"; then
    hdiutil create -size 12g -fs 'Case-sensitive APFS' -type SPARSE \
        -volname ActRaiserBuilderLinuxSDK "$image"
fi
# The cache may live on an external volume mounted with ownership disabled.
# Disk Arbitration requires an ownership-enabled filesystem at the mountpoint.
volume=$(mktemp -d /tmp/actraiser-linux-sdk.XXXXXXXX)
volume=$(CDPATH='' cd -- "$volume" && pwd -P)
attempted_attach=1
hdiutil attach "$image" -nobrowse -owners off -mountpoint "$volume"
attached=1
BUILDER_LINUX_SDK_VOLUME="$volume" "$@"
