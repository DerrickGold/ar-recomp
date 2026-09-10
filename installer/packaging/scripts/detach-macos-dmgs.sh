#!/bin/sh
#
# Detach disk images whose source is either the exact path or a child of the
# directory passed on the command line. This is safe to call when nothing is
# attached and handles partially attached images that do not yet have a mount
# point by falling back to their base /dev/disk node.

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <image-or-cache-directory>" >&2
    exit 2
fi

if [ "$(uname -s)" != "Darwin" ]; then
    exit 0
fi

scope=${1%/}
case "$scope" in
    ""|"/")
        echo "error: refusing to detach a broad disk-image scope" >&2
        exit 2
        ;;
esac
hdiutil_command=${HDIUTIL:-hdiutil}

detach_targets=$(
    "$hdiutil_command" info | awk -v scope="$scope" '
        function emit_target() {
            if (!matching) {
                return
            }
            if (mount_point != "") {
                target = mount_point
            } else if (device != "") {
                target = device
            } else {
                return
            }
            print target "\t" helper_pid
        }
        /^=+$/ {
            emit_target()
            matching = 0
            device = ""
            mount_point = ""
            helper_pid = ""
            next
        }
        index($0, "image-path      : ") == 1 {
            image = substr($0, length("image-path      : ") + 1)
            matching = (image == scope || index(image, scope "/") == 1)
            next
        }
        matching && index($0, "process ID      : ") == 1 {
            helper_pid = substr($0, length("process ID      : ") + 1)
            next
        }
        matching && /^\// {
            fields = split($0, entry, "\t")
            if (device == "") {
                device = entry[1]
            }
            if (fields >= 3 && entry[fields] != "") {
                mount_point = entry[fields]
            }
        }
        END {
            emit_target()
        }
    '
)

if [ -z "$detach_targets" ]; then
    exit 0
fi

detach_with_timeout()
{
    detach_target=$1
    "$hdiutil_command" detach "$detach_target" -force &
    detach_pid=$!
    seconds_left=15

    while kill -0 "$detach_pid" >/dev/null 2>&1; do
        if [ "$seconds_left" -eq 0 ]; then
            kill -TERM "$detach_pid" >/dev/null 2>&1 || true
            wait "$detach_pid" >/dev/null 2>&1 || true
            return 124
        fi
        sleep 1
        seconds_left=$((seconds_left - 1))
    done

    wait "$detach_pid"
}

old_ifs=$IFS
IFS='
'
tab=$(printf '\t')
for detach_record in $detach_targets; do
    detach_target=${detach_record%%"$tab"*}
    helper_pid=${detach_record#*"$tab"}
    echo "Detaching disk image at $detach_target"
    # These are private build-cache images and may be left in a half-attached
    # state where Disk Arbitration never supplied a mount point. Bound the
    # normal forced detach, then terminate only the diskimages-helper recorded
    # by hdiutil for this exact cache image if Disk Arbitration is wedged.
    if detach_with_timeout "$detach_target"; then
        continue
    fi

    case "$helper_pid" in
        ""|"0"|"1"|*[!0-9]*)
            echo "error: timed out detaching $detach_target and found no helper process" >&2
            exit 1
            ;;
    esac

    echo "Disk Arbitration is unresponsive; terminating stale helper PID $helper_pid"
    kill -TERM "$helper_pid" >/dev/null 2>&1 || true
    sleep 2
    if kill -0 "$helper_pid" >/dev/null 2>&1; then
        kill -KILL "$helper_pid"
    fi
    seconds_left=5
    while kill -0 "$helper_pid" >/dev/null 2>&1; do
        if [ "$seconds_left" -eq 0 ]; then
            echo "error: stale helper PID $helper_pid did not exit" >&2
            exit 1
        fi
        sleep 1
        seconds_left=$((seconds_left - 1))
    done
done
IFS=$old_ifs
