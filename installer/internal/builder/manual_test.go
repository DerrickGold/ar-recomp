package builder

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// syntheticJPEG builds the smallest byte sequence the reader's carver accepts:
// SOI, a baseline SOF0 carrying the geometry, a scan segment, entropy bytes and
// EOI. Real page scans are not needed to test the predicate, and a fixture we
// can shape exactly is what lets the size and geometry rules be exercised.
func syntheticJPEG(width, height uint16, payload int) []byte {
	var out bytes.Buffer
	out.Write([]byte{0xFF, 0xD8}) // SOI

	// SOF0: length(2) precision(1) height(2) width(2) components(1) + 3 per component.
	sof := []byte{0xFF, 0xC0, 0x00, 0x11, 0x08}
	sof = binary.BigEndian.AppendUint16(sof, height)
	sof = binary.BigEndian.AppendUint16(sof, width)
	sof = append(sof, 0x03)
	sof = append(sof, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01)
	out.Write(sof)

	out.Write([]byte{0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00})
	// Entropy-coded bytes. 0x00 never looks like a marker, so the extent walk
	// runs to the EOI below rather than stopping early.
	out.Write(bytes.Repeat([]byte{0x42}, payload))
	out.Write([]byte{0xFF, 0xD9}) // EOI
	return out.Bytes()
}

// albumPDF wraps N identical page images in the thinnest possible PDF shell, so
// image bytes dominate the file exactly as they do in a real scan album.
func albumPDF(t *testing.T, pages int) []byte {
	t.Helper()
	var out bytes.Buffer
	out.WriteString("%PDF-1.4\n")
	for i := 0; i < pages; i++ {
		out.Write(syntheticJPEG(1024, 1448, 4096))
	}
	out.WriteString("\n%%EOF\n")
	return out.Bytes()
}

func installManual(t *testing.T, root string, content []byte) string {
	t.Helper()
	path := manualPath(root)
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, content, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// The carve must find every page and read its real geometry. This is the half
// of the port that decides what the in-game reader will see.
func TestCarveFindsEveryPageAndItsGeometry(t *testing.T) {
	pages := carveManualAlbum(albumPDF(t, 5))
	if len(pages) != 5 {
		t.Fatalf("carved %d pages, want 5", len(pages))
	}
	for i, page := range pages {
		if page.width != 1024 || page.height != 1448 {
			t.Errorf("page %d geometry = %dx%d, want 1024x1448", i, page.width, page.height)
		}
	}
}

// An EXIF/JFIF thumbnail carries its OWN end-of-image marker. Stopping at the
// first one would cut the page short and leave the rest of the real image to be
// rescanned as garbage, so the extent walk must skip APPn payloads wholesale.
func TestCarveIsNotFooledByAThumbnailsEndMarker(t *testing.T) {
	page := syntheticJPEG(800, 1000, 2048)
	thumbnail := []byte{0xFF, 0xD8, 0xFF, 0xD9} // a whole tiny JPEG
	app1 := append([]byte{0xFF, 0xE1}, 0x00, byte(len(thumbnail)+2))
	app1 = append(app1, thumbnail...)
	// Splice the APP1 in immediately after the SOI, where a real one lives.
	withThumbnail := append([]byte{}, page[:2]...)
	withThumbnail = append(withThumbnail, app1...)
	withThumbnail = append(withThumbnail, page[2:]...)

	pages := carveManualAlbum(withThumbnail)
	if len(pages) != 1 {
		t.Fatalf("carved %d pages, want 1 whole page", len(pages))
	}
	if pages[0].length != len(withThumbnail) {
		t.Errorf("page length = %d, want the whole %d-byte image",
			pages[0].length, len(withThumbnail))
	}
	if pages[0].width != 800 || pages[0].height != 1000 {
		t.Errorf("geometry = %dx%d, want the page's 800x1000 and not the thumbnail's",
			pages[0].width, pages[0].height)
	}
}

// Progressive JPEG (SOF2) must be refused for the reason the C refuses it:
// stb_image decodes baseline only, so accepting one would trade a clear
// rejection for a page that fails at display time.
func TestCarveRefusesProgressiveJPEG(t *testing.T) {
	page := syntheticJPEG(1024, 1448, 1024)
	progressive := bytes.Replace(page, []byte{0xFF, 0xC0}, []byte{0xFF, 0xC2}, 1)
	if pages := carveManualAlbum(progressive); len(pages) != 0 {
		t.Fatalf("carved %d pages from a progressive JPEG, want 0", len(pages))
	}
}

func TestAlbumVerdicts(t *testing.T) {
	textPDF := append([]byte("%PDF-1.4\n"), bytes.Repeat([]byte("text stream "), 4096)...)
	logoOnEveryPage := append([]byte{}, textPDF...)
	for i := 0; i < 8; i++ {
		logoOnEveryPage = append(logoOnEveryPage, syntheticJPEG(64, 64, 64)...)
	}

	mixed := []byte("%PDF-1.4\n")
	mixed = append(mixed, syntheticJPEG(1024, 1448, 4096)...)
	mixed = append(mixed, syntheticJPEG(1024, 2048, 4096)...)

	// A real flatbed pass: a majority size, a large minority one pixel wider,
	// and one crooked sheet. The shape of an actual 48-page scan.
	drift := []byte("%PDF-1.4\n")
	for i := 0; i < 8; i++ {
		drift = append(drift, syntheticJPEG(1009, 1767, 2048)...)
	}
	for i := 0; i < 4; i++ {
		drift = append(drift, syntheticJPEG(1010, 1767, 2048)...)
	}
	drift = append(drift, syntheticJPEG(1014, 1770, 2048)...)

	// The cover is the sheet likeliest to differ, and must not condemn the book.
	oddCover := append([]byte("%PDF-1.4\n"), syntheticJPEG(1040, 1800, 2048)...)
	for i := 0; i < 12; i++ {
		oddCover = append(oddCover, syntheticJPEG(1009, 1767, 2048)...)
	}

	// Drift is a few percent; an embedded figure is off by a multiple.
	figure := []byte("%PDF-1.4\n")
	for i := 0; i < 12; i++ {
		figure = append(figure, syntheticJPEG(1009, 1767, 2048)...)
	}
	figure = append(figure, syntheticJPEG(1120, 1767, 2048)...)

	cases := []struct {
		name    string
		content []byte
		ok      bool
		reason  string
	}{
		{"a scan album", albumPDF(t, 12), true, ""},
		{"a real scan's drift", drift, true, ""},
		{"an odd cover on a consistent body", oddCover, true, ""},
		{"an embedded figure", figure, false, "different sizes"},
		{"a single page", albumPDF(t, 1), false, "page scans"},
		{"a text PDF", textPDF, false, "page scans"},
		{"a logo on every page", logoOnEveryPage, false, "page images"},
		{"mixed page sizes", mixed, false, "different sizes"},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			verdict := judgeManualAlbum(testCase.content)
			if verdict.OK != testCase.ok {
				t.Fatalf("OK = %v, want %v (reason %q)", verdict.OK, testCase.ok, verdict.Reason)
			}
			if testCase.ok {
				return
			}
			if !strings.Contains(verdict.Reason, testCase.reason) {
				t.Errorf("reason = %q, want it to mention %q", verdict.Reason, testCase.reason)
			}
		})
	}
}

// Absence is a normal state, not an error: the game reports it and carries on,
// and the workshop must do the same rather than serving a broken viewer.
func TestManualStatusAndEndpointWhenNoneIsInstalled(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")

	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/tok/manual", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("status endpoint = %d, want 200", response.Code)
	}
	var status manualStatus
	if err := json.Unmarshal(response.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if status.Present {
		t.Error("reported a manual in an empty tree")
	}

	pdf := httptest.NewRecorder()
	app.ServeHTTP(pdf, httptest.NewRequest(http.MethodGet, "/tok/manual.pdf", nil))
	if pdf.Code != http.StatusNotFound {
		t.Errorf("manual.pdf with none installed = %d, want 404", pdf.Code)
	}
}

func TestManualStatusReportsInGameReadiness(t *testing.T) {
	for _, testCase := range []struct {
		name    string
		content []byte
		inGame  bool
	}{
		{"scan album", albumPDF(t, 6), true},
		{"text PDF", append([]byte("%PDF-1.4\n"), bytes.Repeat([]byte("text "), 8192)...), false},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			root := t.TempDir()
			installManual(t, root, testCase.content)
			app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
			status := app.manualStatus()
			if !status.Present {
				t.Fatal("installed manual not reported as present")
			}
			if status.InGame != testCase.inGame {
				t.Errorf("InGame = %v, want %v (warning %q)",
					status.InGame, testCase.inGame, status.Warning)
			}
			// A file the game cannot open must SAY so; silence here is the
			// confusion this whole check exists to prevent.
			if !testCase.inGame && status.Warning == "" {
				t.Error("no warning for a manual the in-game reader will refuse")
			}
			if testCase.inGame && status.Warning != "" {
				t.Errorf("unexpected warning for a valid album: %q", status.Warning)
			}
		})
	}
}

func postManual(t *testing.T, app *application, filename string, content []byte) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	form := multipart.NewWriter(&body)
	part, err := form.CreateFormFile("manual", filename)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := part.Write(content); err != nil {
		t.Fatal(err)
	}
	if err := form.Close(); err != nil {
		t.Fatal(err)
	}
	request := httptest.NewRequest(http.MethodPost, "/tok/manual", &body)
	request.Header.Set("Content-Type", form.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, request)
	return response
}

func TestInstallingAManualWritesItWhereTheGameReads(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	content := albumPDF(t, 8)

	response := postManual(t, app, "my-manual.pdf", content)
	if response.Code != http.StatusOK {
		t.Fatalf("install = %d: %s", response.Code, response.Body)
	}
	// The path is the contract with src/manual/manual_reader.c; a manual written
	// anywhere else installs successfully and never appears in the game.
	got, err := os.ReadFile(filepath.Join(root, "game-assets", "manual.pdf"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, content) {
		t.Fatalf("installed %d bytes, want the uploaded %d", len(got), len(content))
	}
	var status manualStatus
	if err := json.Unmarshal(response.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if !status.Present || !status.InGame || status.Pages != 8 {
		t.Errorf("status = %+v, want a present 8-page in-game-ready album", status)
	}
}

// A PDF the game cannot page through is still worth reading in the browser, so
// it installs WITH a warning rather than being refused.
func TestANonAlbumPDFInstallsWithAWarning(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	text := append([]byte("%PDF-1.4\n"), bytes.Repeat([]byte("text "), 8192)...)
	text = append(text, []byte("\n%%EOF\n")...)

	response := postManual(t, app, "text.pdf", text)
	if response.Code != http.StatusOK {
		t.Fatalf("install = %d: %s", response.Code, response.Body)
	}
	var status manualStatus
	if err := json.Unmarshal(response.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if !status.Present {
		t.Fatal("a readable PDF was not installed")
	}
	if status.InGame || status.Warning == "" {
		t.Errorf("status = %+v, want installed-but-warned", status)
	}
}

func TestUploadRejectsWhatIsNotAPDF(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	cases := []struct {
		name, want string
		content    []byte
	}{
		{"a PNG", "must be a PDF", []byte("\x89PNG\r\n\x1a\nnot a manual")},
		{"an empty file", "empty", nil},
		{"a truncated PDF", "truncated", append([]byte("%PDF-1.4\n"),
			bytes.Repeat([]byte("x"), 8192)...)},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			response := postManual(t, app, "manual.pdf", testCase.content)
			if response.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400", response.Code)
			}
			if !strings.Contains(response.Body.String(), testCase.want) {
				t.Errorf("error = %s, want it to mention %q", response.Body, testCase.want)
			}
			// A refused upload must leave no file behind.
			if _, err := os.Stat(manualPath(root)); !os.IsNotExist(err) {
				t.Errorf("a refused upload left a manual behind (err = %v)", err)
			}
		})
	}
}

func TestReinstallingReplacesAndRemovingClears(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")

	if code := postManual(t, app, "first.pdf", albumPDF(t, 3)).Code; code != http.StatusOK {
		t.Fatalf("first install = %d", code)
	}
	replacement := albumPDF(t, 9)
	if code := postManual(t, app, "second.pdf", replacement).Code; code != http.StatusOK {
		t.Fatalf("second install = %d", code)
	}
	got, err := os.ReadFile(manualPath(root))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got, replacement) {
		t.Error("re-installing did not replace the previous manual")
	}

	response := httptest.NewRecorder()
	app.ServeHTTP(response, httptest.NewRequest(http.MethodDelete, "/tok/manual", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("remove = %d: %s", response.Code, response.Body)
	}
	if _, err := os.Stat(manualPath(root)); !os.IsNotExist(err) {
		t.Errorf("manual survived removal (err = %v)", err)
	}
	// Removing again is not an error: the end state is what was asked for.
	repeat := httptest.NewRecorder()
	app.ServeHTTP(repeat, httptest.NewRequest(http.MethodDelete, "/tok/manual", nil))
	if repeat.Code != http.StatusOK {
		t.Errorf("removing an absent manual = %d, want 200", repeat.Code)
	}
}
