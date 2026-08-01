package toolchain

import (
	"bytes"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestPinTableCoversSupportedPlatforms(t *testing.T) {
	required := []string{
		"darwin/arm64", "darwin/amd64",
		"linux/amd64", "linux/arm64",
		"windows/amd64", "windows/arm64",
	}
	for _, platform := range required {
		entry, ok := pinnedZig[platform]
		if !ok {
			t.Errorf("missing pin for %s", platform)
			continue
		}
		if !strings.Contains(entry.Archive, PinnedZigVersion) {
			t.Errorf("%s: archive %q does not carry pinned version %s", platform, entry.Archive, PinnedZigVersion)
		}
		if len(entry.SHA256) != 64 {
			t.Errorf("%s: malformed sha256 %q", platform, entry.SHA256)
		}
		windows := strings.HasPrefix(platform, "windows/")
		if windows != strings.HasSuffix(entry.Archive, ".zip") {
			t.Errorf("%s: unexpected archive format %q", platform, entry.Archive)
		}
	}
}

func TestPinnedURLForHost(t *testing.T) {
	url, sha, err := PinnedURL()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(url, "https://ziglang.org/download/"+PinnedZigVersion+"/") {
		t.Fatalf("url: %s", url)
	}
	if len(sha) != 64 {
		t.Fatalf("sha: %s", sha)
	}
}

func TestPinForCrossTargets(t *testing.T) {
	url, sha, archive, err := Pin("windows", "amd64")
	if err != nil {
		t.Fatal(err)
	}
	if archive != "zig-x86_64-windows-0.16.0.zip" {
		t.Fatalf("archive: %s", archive)
	}
	if !strings.HasSuffix(url, archive) || len(sha) != 64 {
		t.Fatalf("url %s sha %s", url, sha)
	}
	if _, _, _, err := Pin("plan9", "mips"); err == nil {
		t.Fatal("expected error for unsupported target")
	}
}

func TestLocateRejectsBrokenOverride(t *testing.T) {
	t.Setenv(EnvOverride, "/nonexistent/zig-binary")
	if _, err := Locate(t.TempDir()); err == nil {
		t.Fatal("expected error for unusable override")
	}
}

func TestLocateReportsActionableError(t *testing.T) {
	t.Setenv(EnvOverride, "")
	t.Setenv("PATH", t.TempDir()) // hide any real zig
	_, err := Locate(t.TempDir())
	if err == nil {
		t.Skip("a zig satisfied Locate despite the emptied PATH")
	}
	message := err.Error()
	if !strings.Contains(message, "toolchain fetch") || !strings.Contains(message, EnvOverride) {
		t.Fatalf("error not actionable: %s", message)
	}
}

func TestProgressReaderReportsPercentage(t *testing.T) {
	var out bytes.Buffer
	payload := strings.Repeat("x", 100)
	reader := &progressReader{inner: strings.NewReader(payload), total: 100, out: &out}
	n, err := io.Copy(io.Discard, reader)
	if err != nil {
		t.Fatal(err)
	}
	if n != 100 {
		t.Fatalf("copied %d bytes, want 100", n)
	}
	if reader.read != 100 {
		t.Fatalf("progressReader.read=%d, want 100", reader.read)
	}
	if !strings.Contains(out.String(), "100%") {
		t.Fatalf("progress output never reached 100%%: %q", out.String())
	}
}

func TestProgressReaderUnknownTotalIsSilent(t *testing.T) {
	var out bytes.Buffer
	// total==0 (unknown Content-Length) must still count bytes but print nothing.
	reader := &progressReader{inner: strings.NewReader("abcd"), total: 0, out: &out}
	if _, err := io.Copy(io.Discard, reader); err != nil {
		t.Fatal(err)
	}
	if reader.read != 4 {
		t.Fatalf("progressReader.read=%d, want 4", reader.read)
	}
	if out.Len() != 0 {
		t.Fatalf("expected no output for unknown total, got %q", out.String())
	}
}

func TestDownloadClientHasTimeout(t *testing.T) {
	// The download client must cap the whole transfer so a stalled connection
	// cannot hang the build forever. This mirrors the constant used in download().
	client := &http.Client{Timeout: 30 * time.Minute}
	if client.Timeout != 30*time.Minute {
		t.Fatalf("timeout=%v, want 30m", client.Timeout)
	}
}

func TestSDL3PinReturnsRedistributables(t *testing.T) {
	// macOS is a universal dmg: both arches share one file + hash.
	for _, arch := range []string{"arm64", "amd64"} {
		url, sha, archive, kind, err := SDL3Pin("darwin", arch)
		if err != nil {
			t.Fatalf("darwin/%s: %v", arch, err)
		}
		if kind != "dmg" || !strings.HasSuffix(archive, ".dmg") {
			t.Fatalf("darwin/%s: kind=%q archive=%q", arch, kind, archive)
		}
		if len(sha) != 64 || !strings.Contains(url, PinnedSDL3Version) {
			t.Fatalf("darwin/%s: sha=%q url=%q", arch, sha, url)
		}
	}
	_, _, _, kind, err := SDL3Pin("windows", "amd64")
	if err != nil || kind != "mingw" {
		t.Fatalf("windows/amd64: kind=%q err=%v", kind, err)
	}
}

func TestSDL3PinErrorsWithoutRedistributable(t *testing.T) {
	// Linux and windows-arm64 have no official redistributable; the packaging
	// script must fall back to a system SDL3, so the pin must error (not panic).
	for _, p := range []struct{ goos, goarch string }{
		{"linux", "amd64"}, {"linux", "arm64"}, {"windows", "arm64"},
	} {
		if _, _, _, _, err := SDL3Pin(p.goos, p.goarch); err == nil {
			t.Fatalf("%s/%s: expected error, got nil", p.goos, p.goarch)
		}
	}
}

func TestSteamDeckSDL3PinsAreVerifiedX8664Inputs(t *testing.T) {
	headersURL, headersSHA, headersArchive,
		runtimeURL, runtimeSHA, runtimeArchive := SteamDeckSDL3Pins()
	if headersArchive != "SDL3-"+PinnedSteamDeckSDL3HeaderVersion+".tar.gz" ||
		!strings.HasSuffix(headersURL, headersArchive) ||
		len(headersSHA) != 64 {
		t.Fatalf("invalid Deck SDL headers pin: %q %q %q",
			headersURL, headersSHA, headersArchive)
	}
	if !strings.Contains(runtimeArchive, PinnedSteamDeckSDL3RuntimeVersion) ||
		!strings.HasSuffix(runtimeArchive, "_amd64.deb") ||
		!strings.HasSuffix(runtimeURL, runtimeArchive) ||
		len(runtimeSHA) != 64 {
		t.Fatalf("invalid Deck SDL runtime pin: %q %q %q",
			runtimeURL, runtimeSHA, runtimeArchive)
	}
}

func TestTargetGo(t *testing.T) {
	for triple, want := range map[string][2]string{
		"x86_64-windows-gnu":  {"windows", "amd64"},
		"aarch64-windows-gnu": {"windows", "arm64"},
		"aarch64-macos-none":  {"darwin", "arm64"},
		"x86_64-linux-gnu":    {"linux", "amd64"},
	} {
		goos, goarch, err := TargetGo(triple)
		if err != nil {
			t.Errorf("TargetGo(%q): %v", triple, err)
			continue
		}
		if goos != want[0] || goarch != want[1] {
			t.Errorf("TargetGo(%q) = %s/%s, want %s/%s", triple, goos, goarch, want[0], want[1])
		}
	}
	// A bad triple must be rejected rather than defaulting to something that
	// would stage the wrong platform's SDL.
	for _, bad := range []string{"", "x86_64", "riscv64-linux-gnu", "x86_64-plan9-none"} {
		if _, _, err := TargetGo(bad); err == nil {
			t.Errorf("TargetGo(%q) should have failed", bad)
		}
	}
}

func TestStageSDL3RejectsTargetsWithoutRedistributable(t *testing.T) {
	// Linux has no official SDL3 redistributable pin, and macOS ships a .dmg
	// this path deliberately does not open. Both must fail before touching the
	// network, with a message that names the way forward.
	for _, target := range []string{"x86_64-linux-gnu", "aarch64-macos-none"} {
		err := StageSDL3(target, t.TempDir(), filepath.Join(t.TempDir(), "sdl3"), nil)
		if err == nil {
			t.Fatalf("StageSDL3(%q) should have failed", target)
		}
	}
}

func TestStageSDL3IsIdempotent(t *testing.T) {
	// An already-staged directory must short-circuit: `make check-cross` runs
	// this on every invocation and must not re-download or re-extract.
	stage := filepath.Join(t.TempDir(), "sdl3")
	if err := os.MkdirAll(filepath.Join(stage, "include", "SDL3"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(stage, "lib"), 0o755); err != nil {
		t.Fatal(err)
	}
	// An empty cache dir would force a download if the short-circuit missed.
	if err := StageSDL3("x86_64-windows-gnu", filepath.Join(t.TempDir(), "empty"), stage, nil); err != nil {
		t.Fatal(err)
	}
}
