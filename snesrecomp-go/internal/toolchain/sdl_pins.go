package toolchain

import "fmt"

// PinnedSDL3Version is the fixed developer cross-build SDK. Installer archives
// use ResolveSDLSDK's stable-3.x selection instead. These reviewed checksums
// also support exact legacy assets that predate GitHub's SHA-256 metadata.
const PinnedSDL3Version = "3.4.12"

// PinnedSDL3TtfVersion is the SDL_ttf release bundled beside SDL3. SDL_ttf
// versions independently of SDL3, so this deliberately does not track
// PinnedSDL3Version. Enhanced-text builds require its headers and library;
// staging only SDL3 cannot compile/link the game's font backend.
const PinnedSDL3TtfVersion = "3.2.2"

const (
	PinnedSteamDeckSDL3HeaderVersion  = "3.4.14"
	PinnedSteamDeckSDL3RuntimeVersion = PinnedSteamDeckSDL3HeaderVersion + "+ds-1+steamrt3.1+bsrt3.1"
	// Valve packages the same upstream SDL_ttf release the official
	// redistributables carry, so headers and runtime agree on one version.
	PinnedSteamDeckSDL3TtfHeaderVersion  = PinnedSDL3TtfVersion
	PinnedSteamDeckSDL3TtfRuntimeVersion = "3.2.2+ds-1~steamrt3.1+bsrt3.1"
	steamRuntimeSniperBaseURL            = "https://repo.steampowered.com/steamrt-sniper"
)

// sdlPin is one bundled SDL3 redistributable: the download file name (under the
// SDL release URL), its SHA256, and the archive kind that tells the packaging
// script how to unpack it ("dmg" on macOS, "mingw" for the GNU Windows
// tarball, and "vc" for the Windows ARM64 development ZIP).
type sdlPin struct {
	Archive string
	SHA256  string
	Kind    string
}

// pinnedSDL3 maps GOOS/GOARCH to the official SDL3 redistributable, keyed the
// same way as pinnedZig. Only platforms with an official prebuilt binary are
// present: the macOS universal .dmg (one file serves both arches), the Windows
// x86_64 MinGW tarball, and the Windows ARM64 VC development archive. Zig/lld
// accepts the latter's COFF import library when targeting aarch64-windows-gnu.
// Installer development/runtime package pairs are selected by ResolveSDLSDK.
// Unsupported targets have no pin here and SDL3Pin returns an error for them.
var pinnedSDL3 = map[string]sdlPin{
	"darwin/arm64":  {"SDL3-" + PinnedSDL3Version + ".dmg", "c77d36d9393bb5481e38d222b75a1a63ab16274457b3d18c63fef90aaf5fc93b", "dmg"},
	"darwin/amd64":  {"SDL3-" + PinnedSDL3Version + ".dmg", "c77d36d9393bb5481e38d222b75a1a63ab16274457b3d18c63fef90aaf5fc93b", "dmg"},
	"windows/amd64": {"SDL3-devel-" + PinnedSDL3Version + "-mingw.tar.gz", "ea8071241b934e1feec0337f7d78807a5004de9a500dba1942aaf615a988d7a2", "mingw"},
	"windows/arm64": {"SDL3-devel-" + PinnedSDL3Version + "-VC.zip", "8793a153c7eba93b1eb8022fd2356383ec446b2584e43724a72ef68d682813ab", "vc"},
}

// pinnedSDL3Ttf mirrors pinnedSDL3 exactly: SDL_ttf publishes the same three
// redistributable kinds under the same naming scheme, so the packaging script
// unpacks them with the same three branches.
var pinnedSDL3Ttf = map[string]sdlPin{
	"darwin/arm64":  {"SDL3_ttf-" + PinnedSDL3TtfVersion + ".dmg", "2c7035eba9e63df137dbe95d2672f4c52e68d76bf6dd7c47a325f29bb6ba3cf7", "dmg"},
	"darwin/amd64":  {"SDL3_ttf-" + PinnedSDL3TtfVersion + ".dmg", "2c7035eba9e63df137dbe95d2672f4c52e68d76bf6dd7c47a325f29bb6ba3cf7", "dmg"},
	"windows/amd64": {"SDL3_ttf-devel-" + PinnedSDL3TtfVersion + "-mingw.tar.gz", "bb57f26787d6a2e108158562feb061fcdf6f68a110f9c8cf9af42ff343d4e41c", "mingw"},
	"windows/arm64": {"SDL3_ttf-devel-" + PinnedSDL3TtfVersion + "-VC.zip", "67805c5babfc49ca0c56882dc9b8cabbcdd1e6f9edde10ddac91ddb38f3afb8c", "vc"},
}

// SDL3TtfPin is SDL3Pin's counterpart for SDL_ttf, with the same contract: an
// error on a platform with no official redistributable. Linux installers use
// ResolveSDLSDK for publisher development/runtime package pairs instead.
func SDL3TtfPin(goos, goarch string) (url, sha, archive, kind string, err error) {
	entry, ok := pinnedSDL3Ttf[goos+"/"+goarch]
	if !ok {
		return "", "", "", "", fmt.Errorf("no official SDL_ttf redistributable for %s/%s", goos, goarch)
	}
	base := "https://github.com/libsdl-org/SDL_ttf/releases/download/release-" + PinnedSDL3TtfVersion
	return base + "/" + entry.Archive, entry.SHA256, entry.Archive, entry.Kind, nil
}

// SDL3Pin returns the bundled SDL3 redistributable pin for a target platform:
// its download URL, SHA256, archive file name, and archive kind. It returns an
// error for platforms with no official redistributable; Linux installers use
// ResolveSDLSDK instead.
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
// Valve's x86_64 Steam Runtime shared library used to link and run it.
// Keeping both inputs on the same SDL release avoids a compile-time/runtime
// API mismatch while retaining Valve's SteamOS-specific build configuration.
func SteamDeckSDL3Pins() (
	headersURL, headersSHA, headersArchive string,
	runtimeURL, runtimeSHA, runtimeArchive string,
) {
	headersArchive = "SDL3-" + PinnedSteamDeckSDL3HeaderVersion + ".tar.gz"
	headersURL = "https://github.com/libsdl-org/SDL/releases/download/release-" +
		PinnedSteamDeckSDL3HeaderVersion + "/" + headersArchive
	headersSHA = "30d4aa2b3037718142b32dffd4e72f917ebb6cc5227150e7bb9c45efb2153aeb"

	runtimeArchive = "libsdl3-0_" + PinnedSteamDeckSDL3RuntimeVersion + "_amd64.deb"
	runtimeURL = steamRuntimeSniperBaseURL +
		"/pool/main/libs/libsdl3/" + runtimeArchive
	runtimeSHA = "87449bfb70b8191c778ba87959649983a2613b64c80f585702e61048b11880d5"
	return
}

// SteamDeckSDL3TtfPins is SteamDeckSDL3Pins' counterpart for SDL_ttf: the
// public source tarball supplies headers to compile against, and Valve's own
// Steam Runtime package supplies the shared library to link and run against.
// Valve builds SDL_ttf against the runtime's FreeType rather than statically,
// which is why the Deck uses their package instead of the official tarball.
func SteamDeckSDL3TtfPins() (
	headersURL, headersSHA, headersArchive string,
	runtimeURL, runtimeSHA, runtimeArchive string,
) {
	headersArchive = "SDL3_ttf-" + PinnedSteamDeckSDL3TtfHeaderVersion + ".tar.gz"
	headersURL = "https://github.com/libsdl-org/SDL_ttf/releases/download/release-" +
		PinnedSteamDeckSDL3TtfHeaderVersion + "/" + headersArchive
	headersSHA = "63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d"

	runtimeArchive = "libsdl3-ttf0_" + PinnedSteamDeckSDL3TtfRuntimeVersion + "_amd64.deb"
	runtimeURL = steamRuntimeSniperBaseURL +
		"/pool/main/libs/libsdl3-ttf/" + runtimeArchive
	runtimeSHA = "32a0e683d254d3d4706cb80aa10bbf64ff29dbb347ce34fc41b42bd083fff4d9"
	return
}
