#!/bin/sh
#
# Extract a read-only macOS disk image without leaving it attached when the
# copy fails or this script is interrupted. A previous packaging process may
# have died before detaching the same image, so matching stale attachments are
# removed before a new one is created.

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <image.dmg> <destination> <mount-root>" >&2
    exit 2
fi

image=$1
destination=$2
mount_root=$3
mount_point="${mount_root}/dmgmnt.$$"
hdiutil_command=${HDIUTIL:-hdiutil}
ditto_command=${DITTO:-ditto}
script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
detach_script="${script_dir}/detach-macos-dmgs.sh"

cleanup()
{
    status=$?
    trap - EXIT HUP INT TERM

    if ! /bin/sh "$detach_script" "$image"; then
        echo "error: could not detach disk image $image" >&2
        if [ "$status" -eq 0 ]; then
            status=1
        fi
    fi

    rmdir "$mount_point" >/dev/null 2>&1 || true
    exit "$status"
}

trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$mount_root"

# hdiutil will not attach a DMG at a second explicit mount point when that
# image is already attached. This can happen after an interrupted configure,
# including in the other macOS architecture's build tree.
/bin/sh "$detach_script" "$image"

attach_attempt=1
while :; do
    # 1. Use -mountrandom to let macOS determine a safe, unconflicted path.
    #    We append -noverify and drop the broken explicit -mountpoint.
    echo "Attaching disk image..."
    if attach_output=$("$hdiutil_command" attach -nobrowse -readonly -noverify -mountrandom /tmp "$image"); then
        
        # 2. Extract the random mount point macOS successfully created
        resolved_mnt=$(echo "$attach_output" | awk -F'\t' '/\/tmp\/dmgmnt/ {print $NF}')
        
        if [ -n "$resolved_mnt" ] && [ -d "$resolved_mnt" ]; then
            # 3. Swap the target folder so the rest of your script (like ditto) works seamlessly
            mount_point="$resolved_mnt"
            break
        fi
    fi

    # Cleanup fallback if it fails
    /bin/sh "$detach_script" "$image"
    if [ "$attach_attempt" -ge 2 ]; then
        echo "error: could not attach $image after $attach_attempt attempts" >&2
        exit 1
    fi
    attach_attempt=$((attach_attempt + 1))
    echo "Retrying disk-image attach (attempt $attach_attempt of 2)"
done

"$ditto_command" "$mount_point" "$destination"
