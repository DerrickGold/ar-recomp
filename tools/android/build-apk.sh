#!/usr/bin/env bash
# Android P0 spike: cross-compile the game to libmain.so and package a debug
# APK with SDL's official Android build. Plan and gates:
# development/specs/ar-recomp-android-target.md.
#
#   tools/android/build-apk.sh --rom ar.sfc [--install] [--skip-native]
#
# Needs: a regenerated tree (`snesbuild regen`), a built snesbuild, a JDK 17+,
# and an Android SDK with the NDK below. The APK holds the player's ROM; it is
# for their own device only. Everything is staged under build/android/.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ROM=""
INSTALL=0
SKIP_NATIVE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --rom) ROM=$2; shift 2 ;;
    --install) INSTALL=1; shift ;;
    --skip-native) SKIP_NATIVE=1; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -n "$ROM" && -f "$ROM" ]] || { echo "pass --rom <US ROM>" >&2; exit 2; }
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")

TARGET=aarch64-linux-android.29
ABI=arm64-v8a
NDK_VERSION=28.2.13676358
SDL_VERSION=3.4.12
SDL_TTF_VERSION=3.2.2
SDL_ANDROID_SHA=21e60b78542c05afb81df015d0f8f8cb30e57fcafebaf2dbc38cb62ba92d6198
SDL_TTF_ANDROID_SHA=48e534daf88eec5c7f0c95fa6ca0e8a5f38f5fc7276824573131cebc403ee14c
SDL_SOURCE_SHA=f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7

ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=$ANDROID_HOME/ndk/$NDK_VERSION
case "$(uname -s)" in
  Darwin) NDK_HOST=darwin-x86_64 ;;
  Linux) NDK_HOST=linux-x86_64 ;;
  *) echo "unsupported host" >&2; exit 2 ;;
esac
SYSROOT=$NDK/toolchains/llvm/prebuilt/$NDK_HOST/sysroot
[[ -d "$SYSROOT" ]] || { echo "NDK $NDK_VERSION not found under $ANDROID_HOME" >&2; exit 2; }
if [[ -z "${JAVA_HOME:-}" && -d /opt/homebrew/opt/openjdk@17 ]]; then
  export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
fi
export ANDROID_HOME

WORK=$ROOT/build/android
CACHE=$WORK/cache
SDK=$WORK/sdl-sdk
PROJECT=$WORK/project
mkdir -p "$CACHE"

fetch() {  # url sha256 -> cached path
  local url=$1 sha=$2 out=$CACHE/$(basename "$1")
  if [[ ! -f "$out" ]] || ! echo "$sha  $out" | shasum -a 256 -c - >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$out.part"
    echo "$sha  $out.part" | shasum -a 256 -c - >/dev/null
    mv "$out.part" "$out"
  fi
  echo "$out"
}

SDL_ZIP=$(fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-devel-$SDL_VERSION-android.zip" $SDL_ANDROID_SHA)
TTF_ZIP=$(fetch "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$SDL_TTF_VERSION/SDL3_ttf-devel-$SDL_TTF_VERSION-android.zip" $SDL_TTF_ANDROID_SHA)
SDL_SRC=$(fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" $SDL_SOURCE_SHA)

# One SDK folder with both libraries' headers and arm64 .so files, the layout
# `snesbuild --sdl-include/--sdl-lib` expects. Restaged only when the pins
# change: fresh header mtimes would make every object look stale.
SDK_STAMP="$SDL_ANDROID_SHA $SDL_TTF_ANDROID_SHA"
if [[ "$(cat "$SDK/.stamp" 2>/dev/null)" != "$SDK_STAMP" ]]; then
rm -rf "$SDK" "$WORK/aar"
mkdir -p "$SDK/include" "$SDK/lib" "$WORK/aar/sdl" "$WORK/aar/ttf"
unzip -qo "$SDL_ZIP" "SDL3-$SDL_VERSION.aar" -d "$WORK/aar"
unzip -qo "$TTF_ZIP" "SDL3_ttf-$SDL_TTF_VERSION.aar" -d "$WORK/aar"
unzip -qo "$WORK/aar/SDL3-$SDL_VERSION.aar" -d "$WORK/aar/sdl"
unzip -qo "$WORK/aar/SDL3_ttf-$SDL_TTF_VERSION.aar" -d "$WORK/aar/ttf"
cp -R "$WORK/aar/sdl/prefab/modules/SDL3-Headers/include/SDL3" "$SDK/include/"
cp -R "$WORK/aar/ttf/prefab/modules/SDL3_ttf-shared/include/SDL3_ttf" "$SDK/include/"
cp "$WORK/aar/sdl/prefab/modules/SDL3-shared/libs/android.$ABI/libSDL3.so" "$SDK/lib/"
cp "$WORK/aar/ttf/prefab/modules/SDL3_ttf-shared/libs/android.$ABI/libSDL3_ttf.so" "$SDK/lib/"
echo "$SDK_STAMP" > "$SDK/.stamp"
fi

LIBMAIN=$ROOT/build/hermetic/$TARGET/libmain.so
if [[ $SKIP_NATIVE -eq 0 ]]; then
  SNESBUILD=${SNESBUILD:-$ROOT/snesrecomp-go/build/snesbuild}
  "$SNESBUILD" build --hermetic --root "$ROOT" --target "$TARGET" \
    --android-sysroot "$SYSROOT" --sdl-include "$SDK/include" --sdl-lib "$SDK/lib"
fi
[[ -f "$LIBMAIN" ]] || { echo "missing $LIBMAIN" >&2; exit 1; }

# Gradle project: our sources + SDL's Java classes + its Gradle wrapper.
rm -rf "$PROJECT"
cp -R "$ROOT/tools/android/project" "$PROJECT"
tar -xzf "$SDL_SRC" -C "$WORK" "SDL3-$SDL_VERSION/android-project/gradlew" \
  "SDL3-$SDL_VERSION/android-project/gradle"
cp -R "$WORK/SDL3-$SDL_VERSION/android-project/gradlew" \
  "$WORK/SDL3-$SDL_VERSION/android-project/gradle" "$PROJECT/"
mkdir -p "$PROJECT/app/libs" "$PROJECT/app/src/main/jniLibs/$ABI" \
  "$PROJECT/app/src/main/assets" "$PROJECT/app/src/main/res/mipmap-xxxhdpi"
cp "$WORK/aar/sdl/classes.jar" "$PROJECT/app/libs/sdl3-classes.jar"
cp "$LIBMAIN" "$SDK/lib/libSDL3.so" "$SDK/lib/libSDL3_ttf.so" \
  "$PROJECT/app/src/main/jniLibs/$ABI/"
cp "$ROOT/installer/internal/appicons/assets/game/256.png" \
  "$PROJECT/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png"

# Assets the game reads from its data root; android_platform.c copies each
# listed file out of the APK whenever the stamp changes.
ASSETS=$PROJECT/app/src/main/assets
cp "$ROM" "$ASSETS/user-rom.sfc"
cp "$ROOT/installer/packaging/templates/config.ini" "$ASSETS/config.ini"
cp "$ROOT/diorama-layers.ini" "$ROOT/action-effects.ini" "$ASSETS/"
(cd "$ROOT" && find game-assets -type f ! -name '.*' -print0 | \
  xargs -0 -I{} sh -c 'mkdir -p "$1/$(dirname "$2")" && cp "$2" "$1/$2"' _ "$ASSETS" {})
(
  cd "$ASSETS"
  find . -type f ! -name android-assets.txt | sed 's|^\./||' | LC_ALL=C sort > "$WORK/asset-files.txt"
  STAMP=$(xargs shasum -a 256 < "$WORK/asset-files.txt" | shasum -a 256 | cut -c1-16)
  { echo "stamp $STAMP"; cat "$WORK/asset-files.txt"; } > android-assets.txt
)

(cd "$PROJECT" && ./gradlew --quiet assembleDebug)
APK=$PROJECT/app/build/outputs/apk/debug/app-debug.apk
cp "$APK" "$WORK/ActRaiserRecomp-debug.apk"
echo "APK: $WORK/ActRaiserRecomp-debug.apk ($(du -h "$APK" | cut -f1))"

if [[ $INSTALL -eq 1 ]]; then
  "$ANDROID_HOME/platform-tools/adb" install -r "$WORK/ActRaiserRecomp-debug.apk"
  "$ANDROID_HOME/platform-tools/adb" shell am start -n \
    io.github.derrickgold.arrecomp/.ActRaiserActivity
fi
