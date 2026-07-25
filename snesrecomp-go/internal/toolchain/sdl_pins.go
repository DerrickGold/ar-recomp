package toolchain

import "fmt"

// PinnedSDL3Version is the single SDL3 release every platform bundle carries
// where an official redistributable exists. It is the source of truth for the
// version and checksums the packaging CMake used to hardcode inline.
const PinnedSDL3Version = "3.4.12"

const (
	PinnedSteamDeckSDL3HeaderVersion  = "3.4.10"
	PinnedSteamDeckSDL3RuntimeVersion = "3.4.10+ds-1+steamrt3.1+bsrt3.1"
	steamRuntimeSniperBaseURL         = "https://repo.steampowered.com/steamrt-sniper"
)

// sdlPin is one bundled SDL3 redistributable: the download file name (under the
// SDL release URL), its SHA256, and the archive kind that tells the packaging
// script how to unpack it ("dmg" on macOS, "mingw" for the Windows tarball).
type sdlPin struct {
	Archive string
	SHA256  string
	Kind    string
}

// pinnedSDL3 maps GOOS/GOARCH to the official SDL3 redistributable, keyed the
// same way as pinnedZig. Only platforms with an official prebuilt binary are
// present: the macOS universal .dmg (one file serves both arches) and the
// Windows x86_64 mingw tarball. Everything else falls back to a system SDL3, so
// it has no pin here and SDL3Pin returns an error for it.
var pinnedSDL3 = map[string]sdlPin{
	"darwin/arm64":  {"SDL3-" + PinnedSDL3Version + ".dmg", "c77d36d9393bb5481e38d222b75a1a63ab16274457b3d18c63fef90aaf5fc93b", "dmg"},
	"darwin/amd64":  {"SDL3-" + PinnedSDL3Version + ".dmg", "c77d36d9393bb5481e38d222b75a1a63ab16274457b3d18c63fef90aaf5fc93b", "dmg"},
	"windows/amd64": {"SDL3-devel-" + PinnedSDL3Version + "-mingw.tar.gz", "ea8071241b934e1feec0337f7d78807a5004de9a500dba1942aaf615a988d7a2", "mingw"},
}

// SDL3Pin returns the bundled SDL3 redistributable pin for a target platform:
// its download URL, SHA256, archive file name, and archive kind. It returns an
// error for platforms with no official redistributable (the caller falls back
// to a system SDL3). The URL base mirrors the packaging CMake's _sdl_base.
func SDL3Pin(goos, goarch string) (url, sha, archive, kind string, err error) {
	entry, ok := pinnedSDL3[goos+"/"+goarch]
	if !ok {
		return "", "", "", "", fmt.Errorf("no official SDL3 redistributable for %s/%s", goos, goarch)
	}
	base := "https://github.com/libsdl-org/SDL/releases/download/release-" + PinnedSDL3Version
	return base + "/" + entry.Archive, entry.SHA256, entry.Archive, entry.Kind, nil
}

// SteamDeckSDL3Pins returns the two independently verified inputs for the
// standalone Deck bundle: public SDL headers used to compile the game and
// Valve's x86_64 Steam Runtime shared library used to link and run it. Valve
// publishes. Keeping both inputs on SDL 3.4.10 avoids a compile-time/runtime
// API mismatch while retaining Valve's SteamOS-specific build configuration.
func SteamDeckSDL3Pins() (
	headersURL, headersSHA, headersArchive string,
	runtimeURL, runtimeSHA, runtimeArchive string,
) {
	headersArchive = "SDL3-" + PinnedSteamDeckSDL3HeaderVersion + ".tar.gz"
	headersURL = "https://github.com/libsdl-org/SDL/releases/download/release-" +
		PinnedSteamDeckSDL3HeaderVersion + "/" + headersArchive
	headersSHA = "12b34280415ec8418c864408b93d008a20a6530687ee613d60bfbd20411f2785"

	runtimeArchive = "libsdl3-0_" + PinnedSteamDeckSDL3RuntimeVersion + "_amd64.deb"
	runtimeURL = steamRuntimeSniperBaseURL +
		"/pool/main/libs/libsdl3/" + runtimeArchive
	runtimeSHA = "a4e4086b1fb5461ffc2e1df5cd6810531abf86a57f2aee558f5cb9addd6f48e6"
	return
}
