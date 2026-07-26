#!/bin/sh
#
# Extract a macOS disk image archive cleanly using 7-Zip by preserving
# the full structural bundle (like .xcframework) that CMake expects.

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <image.dmg> <destination> <mount-root>" >&2
    exit 2
fi

image=$1
destination=$2
mount_root=$3

# Define a clean temporary workspace
stage_one="${mount_root}/stage1_$$"

rm -rf "$stage_one"
mkdir -p "$stage_one"
mkdir -p "$(dirname "$destination")"

echo "Bypassing hdiutil: Extracting DMG via 7-Zip..."

# Extract the DMG content directly into stage_one
7zz x "$image" -o"$stage_one" -y >/dev/null

# Look for the root of the framework payload. We search for .xcframework first,
# then fall back to .framework if CMake passed a direct destination.
found_payload=$(find "$stage_one" -type d \( -name "*.xcframework" -o -name "*.framework" \) -print -quit)

if [ -n "$found_payload" ] && [ -d "$found_payload" ]; then
    echo "Found payload at: $(basename "$found_payload")! Aligning bundle layout for CMake..."
    
    # If the destination does not already end in the framework name, append it
    # to ensure we don't flatten the .xcframework or .framework bundle structure.
    target_dir="$destination"
    payload_name=$(basename "$found_payload")

    if [ "$(basename "$target_dir")" != "$payload_name" ]; then
        target_dir="$target_dir/$payload_name"
    fi

    echo "Copying bundle to: $target_dir"
    rm -rf "$target_dir"
    mkdir -p "$(dirname "$target_dir")"
    ditto "$found_payload" "$target_dir"

else
    # Fallback: If 7-Zip extracted everything flat into a subfolder (like 'SDL3'), copy its entire contents
    found_dir=$(find "$stage_one" -mindepth 1 -maxdepth 2 -type d ! -name ".*" -print -quit)
    if [ -n "$found_dir" ]; then
        echo "Copying extracted folder contents directly..."
        rm -rf "$destination"
        ditto "$found_dir" "$destination"
    else
        echo "error: No framework files were found in the extracted payload!" >&2
        ls -R "$stage_one"
        rm -rf "$stage_one"
        exit 1
    fi
fi

# Clean up temporary staging workspace
rm -rf "$stage_one"
echo "Extraction completed successfully!"
