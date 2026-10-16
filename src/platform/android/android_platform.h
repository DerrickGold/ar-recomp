#ifndef AR_ANDROID_PLATFORM_H
#define AR_ANDROID_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/* Android has no desktop launcher to choose the data root or pass the ROM.
 * The APK carries the player's ROM and seed data under assets/, listed in
 * android-assets.txt. This selects the app's external files directory
 * (visible over USB, no storage permission needed), copies the listed assets
 * there when the APK's stamp changes, publishes AR_USER_DATA_DIR and returns
 * the absolute ROM path. Must run before SDL_Init and any file IO. */
bool AndroidPlatform_PrepareStorage(char *rom_path, size_t rom_path_size);

/* SDLActivity calls SDL_main on a Java thread with ART's default ~1 MB stack.
 * Desktop main threads get 8 MB, and the game's deepest paths (the world-map
 * terrain prior alone reserves ~146 KB) are sized against that. Runs `entry`
 * on a native thread with a desktop-sized stack and returns its result. SDL
 * treats the thread that initializes video as its main thread off Apple
 * platforms, and attaches new threads to the JVM on first use. */
int AndroidPlatform_RunWithDesktopStack(int (*entry)(int, char **), int argc,
                                        char **argv);

/* Mirrors stdout/stderr into logcat (tag "ActRaiser") and `log_path`. Replaces
 * the desktop `tee` child process, which an app cannot spawn. */
void AndroidPlatform_TeeConsole(const char *log_path);

#endif /* AR_ANDROID_PLATFORM_H */
