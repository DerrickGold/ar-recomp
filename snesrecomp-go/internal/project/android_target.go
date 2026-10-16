package project

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// Android targets ("aarch64-linux-android.29") differ from every other
// hermetic target in three ways. Zig carries no bionic, so its libc comes from
// an NDK-layout sysroot handed to Zig through a ZIG_LIBC paths file. The game is
// a shared library (libmain.so) that SDLActivity loads, not an executable. And
// Android 15+ devices may use 16 KB pages, which refuse 4 KB-aligned segments.

const (
	androidDefaultAPILevel = "29"
	androidLibraryName     = "libmain.so"
)

// IsAndroidTarget reports whether a Zig triple names an Android ABI.
func IsAndroidTarget(target string) bool {
	fields := strings.Split(target, "-")
	return len(fields) >= 3 && strings.HasPrefix(fields[2], "android")
}

// androidAPILevel is the ".NN" suffix Zig accepts on the ABI, else the default.
func androidAPILevel(target string) string {
	fields := strings.Split(target, "-")
	if _, version, found := strings.Cut(fields[len(fields)-1], "."); found && version != "" {
		return version
	}
	return androidDefaultAPILevel
}

// androidTripleDir is the NDK sysroot's per-architecture directory name.
func androidTripleDir(target string) string {
	arch := strings.Split(target, "-")[0]
	return arch + "-linux-android"
}

// androidToolEnv writes the ZIG_LIBC paths file for the target's sysroot and
// returns the environment every compile and link must carry, plus the CRT /
// stub-library directory the link searches.
func androidToolEnv(options HermeticOptions, outputDir string) ([]string, string, error) {
	sysroot := options.AndroidSysroot
	if sysroot == "" {
		sysroot = os.Getenv("SNESBUILD_ANDROID_SYSROOT")
	}
	if sysroot == "" {
		return nil, "", fmt.Errorf("target %s needs an Android sysroot: pass --android-sysroot "+
			"(an NDK's toolchains/llvm/prebuilt/<host>/sysroot) or set SNESBUILD_ANDROID_SYSROOT",
			options.Target)
	}
	triple := androidTripleDir(options.Target)
	includeDir := filepath.Join(sysroot, "usr", "include")
	crtDir := filepath.Join(sysroot, "usr", "lib", triple, androidAPILevel(options.Target))
	for _, required := range []string{
		filepath.Join(includeDir, "stdio.h"),
		filepath.Join(includeDir, triple),
		filepath.Join(crtDir, "crtbegin_so.o"),
	} {
		if _, err := os.Stat(required); err != nil {
			return nil, "", fmt.Errorf("Android sysroot %s is incomplete for %s: %w", sysroot, options.Target, err)
		}
	}
	libcFile := filepath.Join(outputDir, "android-libc.txt")
	contents := fmt.Sprintf("include_dir=%s\nsys_include_dir=%s\ncrt_dir=%s\n"+
		"msvc_lib_dir=\nkernel32_lib_dir=\ngcc_dir=\n",
		includeDir, filepath.Join(includeDir, triple), crtDir)
	if err := os.WriteFile(libcFile, []byte(contents), 0o644); err != nil {
		return nil, "", err
	}
	return []string{"ZIG_LIBC=" + libcFile}, crtDir, nil
}

// androidLinkArgs turns the executable link into SDLActivity's libmain.so.
func androidLinkArgs(crtDir string) []string {
	return []string{
		"-shared",
		"-Wl,-soname," + androidLibraryName,
		"-Wl,-z,max-page-size=16384",
		"-L" + crtDir,
		"-llog", "-landroid",
	}
}
