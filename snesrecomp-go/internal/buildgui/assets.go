package buildgui

import (
	"bytes"
	_ "embed"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

// The retail cover art, embedded so the builder needs no network and no
// sidecar file: `go:embed` compiles it into the snesbuild executable that the
// release bundle already ships (packaging/CMakeLists.txt installs
// utils/tools/snesbuild), so this adds one file to the repo and nothing to the
// bundle layout.
//
// WebP at ~56 KB is about a third the size of a visually equivalent JPEG, and
// every browser that can run this page (the system browser on macOS, Windows,
// or a Steam Deck) has supported it for years. It is served from its own
// endpoint rather than inlined as a data: URL so it is fetched once and cached
// per session instead of inflating every poll-driven page load by 33%.
//
//go:embed assets/boxart.webp
var boxArtWebP []byte

// The scanned instruction manual, served for in-page reading while the build
// runs (a full build takes minutes). Rendered by the browser's own PDF viewer
// in an <iframe>: every target platform's default browser has one, so this
// costs no JavaScript library and gains page navigation, zoom, text search and
// printing for free.
//
//go:embed assets/manual.pdf
var manualPDF []byte

// The project's current high-resolution title treatment. It stays inside the
// builder until a player enables it on the Assets tab, so a fresh install still
// presents the authentic ROM title. Saving the toggle materializes these bytes
// under game-assets/hd/builder/ and points both title hooks at that copy.
//
//go:embed assets/title-logo.png
var titleLogoPNG []byte

// The asset-replacement manifest TEMPLATE: every known hook, active but inert
// until its file exists, plus the reference documentation for the format.
//
// It lives here rather than at game-assets/manifest.ini because those are two
// different things that were one file. game-assets/manifest.ini is a LIVE,
// user-edited file -- the builder writes to it, players hand-edit it, and a
// developer's own experiments accumulate in it. Vending that same file as the
// release default shipped whatever happened to be in the working copy. This
// copy is the one the release vends and the one a fresh install starts from;
// the live file is materialized from it and thereafter belongs to the user,
// exactly like every other file under game-assets.
//
//go:embed assets/manifest.ini
var assetManifestTemplate []byte

// materializeBundledManual makes the builder's copy available to the game at
// the runtime path it already reads. The live game-assets directory survives a
// "keep just the game" cleanup, so this is a one-time handoff rather than a
// build input that has to be retained with the toolchain.
//
// An existing file always wins. Today that lets a developer supply a different
// album by hand; later it is the seam where the builder's converted user manual
// will land without this fallback overwriting it on the next launch.
func materializeBundledManual(root string) error {
	return materializeBundledFile(
		filepath.Join(root, "game-assets", "manual.pdf"), manualPDF, "manual")
}

// materializeAssetManifest seeds the live manifest from the template on a
// checkout or install that has none. Same never-overwrite rule as the manual,
// and for a stronger reason: this file accumulates a player's own entries and
// the builder's saves, so re-seeding an existing one would discard their work.
func materializeAssetManifest(root string) error {
	return materializeBundledFile(
		filepath.Join(root, "game-assets", "manifest.ini"),
		assetManifestTemplate, "asset manifest")
}

func materializeBundledFile(destination string, content []byte, label string) error {
	if info, err := os.Stat(destination); err == nil {
		if !info.Mode().IsRegular() {
			return fmt.Errorf("%s exists but is not a regular file", destination)
		}
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("inspect %s: %w", destination, err)
	}

	directory := filepath.Dir(destination)
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return fmt.Errorf("create %s directory: %w", label, err)
	}
	temporary, err := os.CreateTemp(directory, ".bundled-*")
	if err != nil {
		return fmt.Errorf("create temporary %s: %w", label, err)
	}
	temporaryPath := temporary.Name()
	defer func() {
		_ = temporary.Close()
		// After a successful rename this path no longer exists; otherwise this
		// removes a partial temporary file from every error path.
		_ = os.Remove(temporaryPath)
	}()
	if err := temporary.Chmod(0o644); err != nil {
		return fmt.Errorf("set %s permissions: %w", label, err)
	}
	if _, err := temporary.Write(content); err != nil {
		return fmt.Errorf("write bundled %s: %w", label, err)
	}
	if err := temporary.Sync(); err != nil {
		return fmt.Errorf("flush bundled %s: %w", label, err)
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("close bundled %s: %w", label, err)
	}
	if err := os.Rename(temporaryPath, destination); err != nil {
		return fmt.Errorf("install bundled %s: %w", label, err)
	}
	return nil
}

// assetModTime is a fixed timestamp for the embedded assets. Embedded files
// have no meaningful mtime, and http.ServeContent needs one to answer
// conditional requests; a constant is correct because the bytes only change
// when the binary does.
var assetModTime = time.Date(2026, time.July, 26, 0, 0, 0, 0, time.UTC)

// serveEmbeddedAsset writes one of the embedded static assets. They are
// immutable for the life of the process, so they are cacheable and support
// conditional/range requests via http.ServeContent -- the latter matters for
// the multi-megabyte PDF, which browsers fetch in ranges as the reader pages
// through it. Reachable only under the session token prefix, like every other
// endpoint.
func serveEmbeddedAsset(response http.ResponseWriter, request *http.Request,
	name, contentType string, content []byte) {
	response.Header().Set("Content-Type", contentType)
	response.Header().Set("Cache-Control", "private, max-age=3600")
	response.Header().Set("X-Content-Type-Options", "nosniff")
	http.ServeContent(response, request, name, assetModTime,
		bytes.NewReader(content))
}

func serveBoxArt(response http.ResponseWriter, request *http.Request) {
	serveEmbeddedAsset(response, request, "boxart.webp", "image/webp", boxArtWebP)
}

func serveTitleLogo(response http.ResponseWriter, request *http.Request) {
	serveEmbeddedAsset(response, request, "title-logo.png", "image/png", titleLogoPNG)
}

// serveManual writes the manual PDF. Content-Disposition is deliberately
// `inline`: the point is to read it in the page, not to download it.
func serveManual(response http.ResponseWriter, request *http.Request) {
	response.Header().Set("Content-Disposition", `inline; filename="ActRaiser-manual.pdf"`)
	serveEmbeddedAsset(response, request, "manual.pdf", "application/pdf", manualPDF)
}
