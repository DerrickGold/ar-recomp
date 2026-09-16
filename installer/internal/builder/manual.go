package builder

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path/filepath"
)

// The instruction manual is the player's own file. The builder ships no booklet:
// a scan of the retail manual is the whole copyrighted work, so this is the one
// asset whose bytes have to come from whoever is running the build.
//
// It lands at game-assets/manual.pdf, which is the path the in-game reader
// already opens (src/manual/manual_reader.c), so installing one is a plain file
// write with no manifest record to keep in step.
const manualRelativePath = "manual.pdf"

const (
	// A 40-page colour scan at a readable DPI runs 5-15 MB; the cap is generous
	// enough for a 600-DPI archival scan and small enough that a misdirected
	// upload cannot exhaust the disk.
	maxManualBytes = 128 << 20
	// LooksLikeAlbum's thresholds, mirrored from manual_pages.c so the builder
	// and the reader agree on what an album is.
	manualAlbumContentMinimumPercent = 80
	// How far a page may drift from the book's geometry and still be a page.
	// A flatbed pass over a stapled booklet varies a pixel or two per sheet, so
	// exact equality rejected genuine scans; a figure or logo is off by tens of
	// percent. See kManualAlbumGeometryTolerancePercent.
	manualAlbumGeometryTolerancePercent = 5
	// kManualMaxPages from manual_pages.h. The carve stops here, so a file with
	// more images is judged on the first 512 like the reader would judge it.
	manualMaxPages = 512
)

func manualPath(root string) string {
	return filepath.Join(root, "game-assets", manualRelativePath)
}

// manualPage mirrors ManualPageEntry: one carved page image.
type manualPage struct {
	length        int
	width, height uint16
}

// carveManualAlbum is a Go port of ManualPages_CarveAlbum. It exists so the
// builder can answer "will the in-game reader accept this?" at upload time
// rather than letting the player discover the answer in the game's menu.
//
// It must stay behaviourally identical to the C: same baseline-only rule, same
// extent walk, same page cap. Where the two disagree the player sees a manual
// the builder accepted and the game refuses, which is the exact confusion this
// whole check exists to prevent.
func carveManualAlbum(data []byte) []manualPage {
	var pages []manualPage
	for at := 0; at+1 < len(data) && len(pages) < manualMaxPages; {
		if data[at] != 0xFF || data[at+1] != 0xD8 {
			at++
			continue
		}
		end, ok := jpegExtent(data, at)
		if !ok {
			at += 2 // a false SOI in binary noise
			continue
		}
		width, height, ok := jpegGeometry(data[at:end])
		if !ok {
			at += 2
			continue
		}
		pages = append(pages, manualPage{length: end - at, width: width, height: height})
		at = end // never rescan inside a recorded page
	}
	return pages
}

// jpegGeometry ports JpegGeometry: SOF0/SOF1 only. Progressive JPEG is refused
// here for the same reason the C refuses it -- stb_image decodes baseline only,
// so accepting one would trade a clear rejection for a page that fails to draw.
func jpegGeometry(data []byte) (width, height uint16, ok bool) {
	if len(data) < 4 || data[0] != 0xFF || data[1] != 0xD8 {
		return 0, 0, false
	}
	for at := 2; at+3 < len(data); {
		if data[at] != 0xFF {
			at++ // resync over fill bytes
			continue
		}
		marker := data[at+1]
		if marker == 0xFF {
			at++
			continue
		}
		// Standalone markers carry no length payload.
		if marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) {
			at += 2
			continue
		}
		segment := int(binary.BigEndian.Uint16(data[at+2:]))
		if segment < 2 || at+2+segment > len(data) {
			return 0, 0, false
		}
		if marker == 0xC0 || marker == 0xC1 { // SOF0/SOF1: baseline
			if segment < 7 || at+8 >= len(data) {
				return 0, 0, false
			}
			height = binary.BigEndian.Uint16(data[at+5:])
			width = binary.BigEndian.Uint16(data[at+7:])
			return width, height, width > 0 && height > 0
		}
		if marker == 0xDA { // scan data reached with no SOF
			return 0, 0, false
		}
		at += 2 + segment
	}
	return 0, 0, false
}

// jpegExtent ports JpegExtent: the EOI that closes the image at `start`.
// Scanning for the first 0xFFD9 would cut the page short at an EXIF thumbnail's
// own EOI, so marker segments are skipped wholesale and only the entropy-coded
// scan is walked byte by byte.
func jpegExtent(data []byte, start int) (end int, ok bool) {
	for at := start + 2; at+1 < len(data); {
		if data[at] != 0xFF {
			at++
			continue
		}
		marker := data[at+1]
		if marker == 0xFF {
			at++
			continue
		}
		if marker == 0xD9 {
			return at + 2, true
		}
		if marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7) {
			at += 2
			continue
		}
		if marker == 0xDA {
			if at+3 >= len(data) {
				return 0, false
			}
			segment := int(binary.BigEndian.Uint16(data[at+2:]))
			if segment < 2 || at+2+segment > len(data) {
				return 0, false
			}
			at += 2 + segment
			resumeSegments := false
			for at+1 < len(data) {
				if data[at] != 0xFF {
					at++
					continue
				}
				next := data[at+1]
				if next == 0xD9 {
					return at + 2, true
				}
				// In entropy-coded data an 0xFF is either stuffed (0xFF00) or a
				// restart marker; both are payload, not structure.
				if next == 0x00 || (next >= 0xD0 && next <= 0xD7) {
					at += 2
					continue
				}
				if next == 0xFF {
					at++ // fill byte
					continue
				}
				// Any other marker legitimately ends this scan (a multi-scan
				// image), so resume the segment walk.
				resumeSegments = true
				break
			}
			if !resumeSegments {
				return 0, false // ran off the end with no EOI
			}
			continue
		}
		if at+3 >= len(data) {
			return 0, false
		}
		segment := int(binary.BigEndian.Uint16(data[at+2:]))
		if segment < 2 || at+2+segment > len(data) {
			return 0, false
		}
		at += 2 + segment
	}
	return 0, false
}

// manualAlbumVerdict reports whether the in-game reader will accept these bytes,
// and in the reader's own terms when it will not.
type manualAlbumVerdict struct {
	Pages  int
	Width  int
	Height int
	OK     bool
	// Reason is empty when OK. It is phrased for the person who chose the file,
	// not for a log: every path says what to do about it.
	Reason string
}

// judgeManualAlbum ports ManualPages_LooksLikeAlbum's three tests and explains
// which one failed. The reader collapses all of them into "needs converting";
// the builder is the place where a specific answer is actually actionable.
func judgeManualAlbum(data []byte) manualAlbumVerdict {
	pages := carveManualAlbum(data)
	verdict := manualAlbumVerdict{Pages: len(pages)}
	verdict.Width, verdict.Height = nominalManualGeometry(pages)

	if len(pages) < 2 {
		verdict.Reason = "holds no baseline-JPEG page scans. Export it as a " +
			"scan album — one baseline JPEG per page, all the same size — " +
			"or simply read it here in the workshop."
		return verdict
	}

	// The images must BE the document, not decorate it.
	imageBytes := 0
	for _, page := range pages {
		imageBytes += page.length
	}
	if imageBytes*100 < len(data)*manualAlbumContentMinimumPercent {
		verdict.Reason = fmt.Sprintf(
			"is only %d%% page images, below the %d%% required. That usually "+
				"means a text PDF with a logo on each page rather than a scan.",
			imageBytes*100/max(len(data), 1), manualAlbumContentMinimumPercent)
		return verdict
	}

	// One geometry throughout, to a tolerance, measured against the MODAL size
	// rather than page 0's -- the cover is the sheet likeliest to be the odd one.
	for index, page := range pages {
		if withinManualTolerance(int(page.width), verdict.Width) &&
			withinManualTolerance(int(page.height), verdict.Height) {
			continue
		}
		verdict.Reason = fmt.Sprintf(
			"has pages at different sizes: page %d is %d×%d where the book "+
				"is %d×%d, further apart than the %d%% a scan normally "+
				"drifts. Re-export that page at the same size as the rest.",
			index+1, page.width, page.height, verdict.Width, verdict.Height,
			manualAlbumGeometryTolerancePercent)
		return verdict
	}

	verdict.OK = true
	return verdict
}

// nominalManualGeometry ports ManualPages_NominalGeometry: the size shared by
// the most pages, ties going to the earliest. It is the size the reader lays
// the whole book out to, so it is also what the tolerance is measured against.
//
// Quadratic, like the C, over a list capped at manualMaxPages and computed once
// per file -- cheaper than the sort it would take to do better, and the tie rule
// stays obvious.
func nominalManualGeometry(pages []manualPage) (width, height int) {
	best, bestCount := -1, 0
	for i, page := range pages {
		// A zero dimension is not a geometry. The carver cannot record one, but
		// it must never win the vote and take the whole book with it.
		if page.width == 0 || page.height == 0 {
			continue
		}
		count := 0
		for _, other := range pages {
			if other.width == page.width && other.height == page.height {
				count++
			}
		}
		if count > bestCount {
			best, bestCount = i, count
		}
	}
	if best < 0 {
		return 0, 0
	}
	return int(pages[best].width), int(pages[best].height)
}

// withinManualTolerance ports WithinTolerance: |value-reference| <= reference*N%.
func withinManualTolerance(value, reference int) bool {
	difference := value - reference
	if difference < 0 {
		difference = -difference
	}
	return difference*100 <= reference*manualAlbumGeometryTolerancePercent
}

// manualStatus is what the Help tab polls to decide between the reader and the
// empty state.
type manualStatus struct {
	Present bool  `json:"present"`
	Bytes   int64 `json:"bytes"`
	Pages   int   `json:"pages"`
	Width   int   `json:"width,omitempty"`
	Height  int   `json:"height,omitempty"`
	// InGame reports whether the game's own reader will open this file. A manual
	// can be perfectly readable in the browser and still fail this.
	InGame  bool   `json:"inGame"`
	Warning string `json:"warning,omitempty"`
}

func (app *application) manualStatus() manualStatus {
	path := manualPath(app.options.ProjectRoot)
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		return manualStatus{}
	}
	status := manualStatus{Present: true, Bytes: info.Size()}
	// The album probe needs the bytes. Reading 8-100 MB on a tab open is one
	// read of a file the browser is about to fetch in full anyway, and it only
	// happens when the Help tab is opened.
	content, err := os.ReadFile(path)
	if err != nil {
		return status
	}
	verdict := judgeManualAlbum(content)
	status.Pages, status.Width, status.Height = verdict.Pages, verdict.Width, verdict.Height
	status.InGame = verdict.OK
	if !verdict.OK {
		status.Warning = "This manual opens in the workshop, but the in-game " +
			"reader will not show it: the file " + verdict.Reason
	}
	return status
}

func (app *application) writeManualStatus(response http.ResponseWriter) {
	writeJSON(response, http.StatusOK, app.manualStatus())
}

// serveManual streams the player's own file from disk. Unlike the other static
// assets this one is NOT embedded, so it is read through http.ServeFile, which
// keeps the range support the PDF viewer relies on to page through a large scan.
func (app *application) serveManual(response http.ResponseWriter, request *http.Request) {
	path := manualPath(app.options.ProjectRoot)
	info, err := os.Stat(path)
	if err != nil || !info.Mode().IsRegular() {
		http.NotFound(response, request)
		return
	}
	response.Header().Set("Content-Type", "application/pdf")
	response.Header().Set("X-Content-Type-Options", "nosniff")
	// Read it in the page, do not download it.
	response.Header().Set("Content-Disposition", `inline; filename="manual.pdf"`)
	// A player can replace this file mid-session, so it must not be cached the
	// way the immutable embedded assets are.
	response.Header().Set("Cache-Control", "private, no-cache")
	http.ServeFile(response, request, path)
}

// readManualUpload applies the same bounded read and format sniff as the audio
// slots. It checks that the file IS a PDF; whether it is a PDF the in-game
// reader can use is a separate question, reported as a warning rather than a
// rejection so a browser-readable manual still installs.
func readManualUpload(header *multipart.FileHeader) ([]byte, error) {
	input, err := header.Open()
	if err != nil {
		return nil, err
	}
	defer input.Close()
	content, err := io.ReadAll(io.LimitReader(input, maxManualBytes+1))
	if err != nil {
		return nil, fmt.Errorf("copy the manual: %w", err)
	}
	if len(content) == 0 {
		return nil, errors.New("the selected manual is empty")
	}
	if int64(len(content)) > maxManualBytes {
		return nil, fmt.Errorf("the manual exceeds the %d MiB safety limit",
			maxManualBytes>>20)
	}
	if !bytes.HasPrefix(content, []byte("%PDF-")) {
		return nil, errors.New("the manual must be a PDF file")
	}
	// A truncated upload is the likely corruption; the trailer proves the tail.
	if !bytes.Contains(content[max(0, len(content)-2048):], []byte("%%EOF")) {
		return nil, errors.New("the manual has no PDF end marker; it may be truncated")
	}
	return content, nil
}

func (app *application) saveManual(response http.ResponseWriter, request *http.Request) {
	app.assetMu.Lock()
	defer app.assetMu.Unlock()

	request.Body = http.MaxBytesReader(response, request.Body, maxManualBytes+(1<<20))
	if err := request.ParseMultipartForm(8 << 20); err != nil {
		writeJSONError(response, http.StatusBadRequest,
			"could not read the selected manual")
		return
	}
	defer func() { _ = request.MultipartForm.RemoveAll() }()

	files := request.MultipartForm.File["manual"]
	if len(files) == 0 {
		writeJSONError(response, http.StatusBadRequest, "choose a PDF to install")
		return
	}
	content, err := readManualUpload(files[0])
	if err != nil {
		writeJSONError(response, http.StatusBadRequest, err.Error())
		return
	}
	staged, err := stageBytes(app.options.ProjectRoot, manualRelativePath, content)
	if err != nil {
		writeJSONError(response, http.StatusInternalServerError, err.Error())
		return
	}
	if err := replaceStagedFile(staged); err != nil {
		writeJSONError(response, http.StatusInternalServerError,
			fmt.Sprintf("install the manual: %v", err))
		return
	}
	writeJSON(response, http.StatusOK, app.manualStatus())
}

// removeManual deletes the installed manual. The game and the Help tab both
// treat absence as a normal state, so this needs no confirmation beyond the
// one the page already asks for.
func (app *application) removeManual(response http.ResponseWriter) {
	app.assetMu.Lock()
	defer app.assetMu.Unlock()

	if err := os.Remove(manualPath(app.options.ProjectRoot)); err != nil &&
		!errors.Is(err, os.ErrNotExist) {
		writeJSONError(response, http.StatusInternalServerError,
			fmt.Sprintf("remove the manual: %v", err))
		return
	}
	writeJSON(response, http.StatusOK, app.manualStatus())
}
