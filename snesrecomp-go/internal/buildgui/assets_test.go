package buildgui

import (
	"bytes"
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// All assets must actually be embedded. A `go:embed` of a missing file is a
// compile error, but an empty or truncated file is not -- and either would only
// show up as a broken image or an unreadable manual in a browser.
func TestEmbeddedAssetsArePresentAndWellFormed(t *testing.T) {
	if len(boxArtWebP) < 4096 {
		t.Errorf("box art is %d bytes; expected a real image", len(boxArtWebP))
	}
	// RIFF....WEBP
	if !bytes.HasPrefix(boxArtWebP, []byte("RIFF")) ||
		!bytes.Equal(boxArtWebP[8:12], []byte("WEBP")) {
		t.Errorf("box art is not a WebP file (prefix %q)", boxArtWebP[:min(12, len(boxArtWebP))])
	}
	if len(manualPDF) < 100_000 {
		t.Errorf("manual is %d bytes; expected the full scanned booklet", len(manualPDF))
	}
	if !bytes.HasPrefix(manualPDF, []byte("%PDF-")) {
		t.Errorf("manual is not a PDF (prefix %q)", manualPDF[:min(8, len(manualPDF))])
	}
	// A truncated PDF is the likely corruption; the trailer proves the tail.
	if !bytes.Contains(manualPDF[max(0, len(manualPDF)-2048):], []byte("%%EOF")) {
		t.Error("manual PDF has no EOF trailer; it may be truncated")
	}
	if len(titleLogoPNG) < 100_000 {
		t.Errorf("HD title art is %d bytes; expected the full image", len(titleLogoPNG))
	}
	if !bytes.HasPrefix(titleLogoPNG, []byte("\x89PNG\r\n\x1a\n")) {
		t.Errorf("HD title art is not a PNG (prefix %q)",
			titleLogoPNG[:min(8, len(titleLogoPNG))])
	}
}

func TestBundledManualIsMaterializedForTheGame(t *testing.T) {
	root := t.TempDir()
	if err := materializeBundledManual(root); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(root, "game-assets", "manual.pdf")
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, manualPDF) {
		t.Fatalf("materialized manual differs: got %d bytes, want %d",
			len(got), len(manualPDF))
	}
}

func TestBundledManualDoesNotOverwriteASuppliedManual(t *testing.T) {
	root := t.TempDir()
	directory := filepath.Join(root, "game-assets")
	if err := os.MkdirAll(directory, 0o755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "manual.pdf")
	const supplied = "future user-supplied manual"
	if err := os.WriteFile(path, []byte(supplied), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := materializeBundledManual(root); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != supplied {
		t.Fatalf("supplied manual was overwritten with %d bytes", len(got))
	}
}

func TestAssetEndpointsServeCorrectTypes(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	cases := []struct {
		endpoint, contentType string
		minBytes              int
	}{
		{"boxart.webp", "image/webp", 4096},
		{"title-logo.png", "image/png", 100_000},
		{"manual.pdf", "application/pdf", 100_000},
	}
	for _, testCase := range cases {
		t.Run(testCase.endpoint, func(t *testing.T) {
			response := httptest.NewRecorder()
			app.ServeHTTP(response,
				httptest.NewRequest(http.MethodGet, "/tok/"+testCase.endpoint, nil))
			if response.Code != http.StatusOK {
				t.Fatalf("status = %d", response.Code)
			}
			if got := response.Header().Get("Content-Type"); got != testCase.contentType {
				t.Errorf("Content-Type = %q, want %q", got, testCase.contentType)
			}
			if response.Body.Len() < testCase.minBytes {
				t.Errorf("served %d bytes, want at least %d",
					response.Body.Len(), testCase.minBytes)
			}
			if response.Header().Get("X-Content-Type-Options") != "nosniff" {
				t.Error("missing nosniff on an asset response")
			}
		})
	}
}

// The manual must render in the page rather than download, and must be a
// tokenless-URL 404 like every other endpoint.
func TestManualServedInlineAndTokenGated(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/tok/manual.pdf", nil))
	disposition := response.Header().Get("Content-Disposition")
	if !strings.HasPrefix(disposition, "inline") {
		t.Errorf("Content-Disposition = %q, want inline so it reads in-page", disposition)
	}
	// Range support matters: browsers fetch large PDFs incrementally.
	if response.Header().Get("Accept-Ranges") != "bytes" {
		t.Errorf("Accept-Ranges = %q, want bytes",
			response.Header().Get("Accept-Ranges"))
	}

	unguarded := httptest.NewRecorder()
	app.ServeHTTP(unguarded, httptest.NewRequest(http.MethodGet, "/manual.pdf", nil))
	if unguarded.Code != http.StatusNotFound {
		t.Errorf("manual without the session token = %d, want 404", unguarded.Code)
	}
}

// The page must reference both assets, and the CSP must admit the manual's
// iframe without loosening object-src.
func TestPageReferencesAssetsAndPermitsTheManualFrame(t *testing.T) {
	app := newApplication(context.Background(), Options{
		Title: "Builder", ProjectRoot: t.TempDir(),
	}, "tok")
	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/tok/", nil))
	body := response.Body.String()
	for _, reference := range []string{
		`src="boxart.webp"`, `src="title-logo.png"`, `href="manual.pdf"`,
	} {
		if !strings.Contains(body, reference) {
			t.Errorf("page does not reference %s", reference)
		}
	}
	if !strings.Contains(body, `id="manual-frame"`) {
		t.Error("page has no manual iframe")
	}
	policy := response.Header().Get("Content-Security-Policy")
	if !strings.Contains(policy, "frame-src 'self'") {
		t.Errorf("CSP must admit the manual iframe: %s", policy)
	}
	if !strings.Contains(policy, "object-src 'none'") {
		t.Errorf("CSP loosened object-src: %s", policy)
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// The manifest the release vends is the TEMPLATE, not whatever the working copy
// happens to hold. It must carry every hook, since a fresh install's entire
// "drop a file with the matching name" workflow depends on the records being
// there and inert.
func TestAssetManifestTemplateCarriesEveryHook(t *testing.T) {
	template := string(assetManifestTemplate)
	for _, track := range assetTracks {
		section := "[music:" + track.ID + "]"
		if !strings.Contains(template, section) {
			t.Errorf("template has no %s", section)
		}
	}
	// Exactly the song-table slots: a split or a personal experiment leaking
	// into the template is the thing this file exists to prevent.
	if got := strings.Count(template, "\n[music:"); got != len(assetTracks) {
		t.Errorf("template has %d music records, want the %d song-table slots",
			got, len(assetTracks))
	}
	for _, hook := range []string{"[replace:title-logo]", "[replace:title-swirl]"} {
		if !strings.Contains(template, hook) {
			t.Errorf("template has no %s", hook)
		}
	}
	// Every record must name a file, or the loader drops it outright.
	if strings.Count(template, "\nfile = ") < len(assetTracks) {
		t.Error("a music record in the template names no file")
	}
}

// A fresh checkout or install gets the template; one that already has a
// manifest keeps every entry in it, because that file holds the player's own
// work and the builder's saves.
func TestAssetManifestIsSeededButNeverOverwritten(t *testing.T) {
	root := t.TempDir()
	if err := materializeAssetManifest(root); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(root, "game-assets", "manifest.ini")
	seeded, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(seeded, assetManifestTemplate) {
		t.Fatalf("seeded %d bytes, want the %d-byte template",
			len(seeded), len(assetManifestTemplate))
	}
	// The seeded file must be usable by the builder straight away.
	configuration, err := loadAssetConfiguration(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(configuration.Tracks) != len(assetTracks) {
		t.Errorf("seeded manifest yields %d slots", len(configuration.Tracks))
	}

	const mine = "[music:song-00]\nsrc = 18:947F\nfile = audio/mine.ogg\n"
	if err := os.WriteFile(path, []byte(mine), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := materializeAssetManifest(root); err != nil {
		t.Fatal(err)
	}
	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(after) != mine {
		t.Errorf("an existing manifest was overwritten:\n%s", after)
	}
}
