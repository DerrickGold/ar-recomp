package toolchain

import (
	"strings"
	"testing"
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
