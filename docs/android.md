# Android preview builds

ActRaiser Recompiled can run on Android phones and tablets. This is a preview
for testers: the Builder and Workshop cannot build Android yet, so you build a
debug APK from a source checkout and install it over USB.

The APK contains your ROM and is for your own device. Do not share it.

## What you need

- A source checkout on **macOS or Linux**, and your **US ROM**.
- **Go 1.25** or newer and **GNU Make**. The build fetches its own pinned Zig
  compiler.
- **JDK 17**, and the **Android SDK** with these packages: `platform-tools`,
  `platforms;android-35`, `build-tools;35.0.0` and `ndk;28.2.13676358`.
  Installing them asks you to accept Google's Android SDK license.
- An **arm64 Android device** running **Android 10 or newer** with **Vulkan**
  support (most phones from the last several years).

On macOS:

```sh
brew install go openjdk@17
brew install --cask android-commandlinetools
```

```sh
export ANDROID_HOME="$HOME/Library/Android/sdk"
sdkmanager --sdk_root="$ANDROID_HOME" --licenses
sdkmanager --sdk_root="$ANDROID_HOME" "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;28.2.13676358"
```

On Linux, install a JDK 17 package and Google's command-line tools, set
`JAVA_HOME`, and set `ANDROID_HOME` to your SDK folder. The build finds the
SDK at `$ANDROID_HOME`, or at `~/Library/Android/sdk` when that is unset.

## Build the APK

From the repository root:

```sh
make android ROM=/absolute/path/to/your-usa-rom.sfc
```

This prepares the native US language source, generates the game code if
`src/gen/` is empty, cross-compiles the game for Android, and packages
`build/android/ActRaiserRecomp-debug.apk`. The first build downloads the pinned
Zig compiler, SDL's official Android release and the Gradle build tool, all
checksum-verified. Depending on your connection that can take 5–20 minutes,
plus a few minutes of compiling. Later builds recompile only what changed.

## Install on your device

1. On the device, turn on **Developer options**, then **USB debugging**.
2. Connect it over USB and accept the debugging prompt on its screen.
3. Install and launch:

   ```sh
   make android ROM=/absolute/path/to/your-usa-rom.sfc INSTALL=1
   ```

   If more than one device or emulator is connected, choose one first, for
   example `export ANDROID_SERIAL=<serial from adb devices>`.

USB installation is the recommended route. Copying the APK to the device and
opening it there also works, but newer Android releases restrict apps from
developers Google has not verified. Every preview APK is signed with your own
machine's debug key, so it counts as one of those apps.

Updating from the same computer keeps your data. Installing a build signed on
a different computer requires uninstalling first, which **deletes your saves**
(see below).

## Playing

- **Touch controls** appear over the game: a D-pad on the left, the SNES
  **A/B/X/Y** buttons on the right, **L/R** at the top corners, and
  **Select/Start** at the bottom. **MENU** opens the settings overlay. The
  pad appears as **Touch controls** under Controls → Devices. Rebind its
  buttons under **Controls → Gamepad**: select an action, then tap the touch
  button you want to use. Touch and physical controllers share these saved
  bindings; use **Auto** or **Gamepad** as the input device. **MENU** always
  opens settings or cancels an active rebind.
- **Gamepads** work as on desktop. The touch controls hide while one is
  connected.
- The game runs full screen in landscape.

## Your data

Saves, settings and replacement content live in:

```text
Android/data/io.github.derrickgold.arrecomp/files/game/
```

You can open this folder from a computer over USB. To replace music or
artwork, copy files into its `game-assets/` folder using the file names that
`game-assets/manifest.ini` lists. They take effect the next time the game
starts, with no rebuild. Installing a newer APK refreshes the files the APK
ships, including `manifest.ini`, but leaves files you added alone. Your
`config.ini` and `settings.ini` are never overwritten.

**Uninstalling the app deletes this folder.** Copy `saves/` somewhere safe
before uninstalling or switching computers.

## Known limitations

- The first time an action stage starts in each session, the game can pause
  for a few seconds while the GPU driver compiles its effect shaders.
- Android may close the game while it is in the background. Save in-game before
  switching away for long.
- Only arm64 devices are supported. x86_64 emulators and Chromebooks are not.

## Reporting problems

Include the device model and Android version. The game's console output goes
to the system log:

```sh
adb logcat -s ActRaiser SDL
```

Each launch also writes a log under `runs/` in the data folder above. For a
crash, `adb logcat -b crash` shows the native backtrace.
