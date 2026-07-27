package buildgui

import (
	"bytes"
	_ "embed"
	"net/http"
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

// assetModTime is a fixed timestamp for the embedded assets. Embedded files
// have no meaningful mtime, and http.ServeContent needs one to answer
// conditional requests; a constant is correct because the bytes only change
// when the binary does.
var assetModTime = time.Date(2026, time.July, 26, 0, 0, 0, 0, time.UTC)

// serveEmbeddedAsset writes one of the embedded static assets. Both are
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

// serveManual writes the manual PDF. Content-Disposition is deliberately
// `inline`: the point is to read it in the page, not to download it.
func serveManual(response http.ResponseWriter, request *http.Request) {
	response.Header().Set("Content-Disposition", `inline; filename="ActRaiser-manual.pdf"`)
	serveEmbeddedAsset(response, request, "manual.pdf", "application/pdf", manualPDF)
}
