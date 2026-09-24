package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"mime/multipart"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/DerrickGold/ar-recomp/installer/internal/regionalmedia"
)

func mediaUpload(t *testing.T, app *application, data []byte, replace bool) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	part, err := w.CreateFormFile("donor", "../../not-a-path.sfc")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = part.Write(data); err != nil {
		t.Fatal(err)
	}
	if replace {
		if err = w.WriteField("replace", "true"); err != nil {
			t.Fatal(err)
		}
	}
	if err = w.Close(); err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest("POST", "/tok/regional-media", &body)
	r.Header.Set("Content-Type", w.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, r)
	return response
}
func TestRegionalMediaHTTPBoundaries(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	for path, want := range map[string]int{"/tok/regional-media": 200, "/regional-media": 404, "/other/regional-media": 404} {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", path, nil))
		if w.Code != want {
			t.Fatal(path, w.Code)
		}
	}
	for _, data := range [][]byte{[]byte("no ROM"), make([]byte, regionalmedia.MaximumBytes+1)} {
		if w := mediaUpload(t, app, data, false); w.Code != 400 {
			t.Fatal(w.Code, w.Body.String())
		}
	}
	entries, _ := os.ReadDir(root)
	if len(entries) != 0 {
		t.Fatal("rejected input wrote files", entries)
	}
	app.dataGate.Lock()
	w := mediaUpload(t, app, []byte("no ROM"), false)
	app.dataGate.Unlock()
	if w.Code != 409 {
		t.Fatal("installation-import exclusion lost", w.Code)
	}
}
func TestRegionalMediaGUIUsesProductionExtractor(t *testing.T) {
	romRoot := os.Getenv("AR_MEDIA_ROM_DIR")
	if romRoot == "" {
		t.Skip("optional GUI ROM integration")
	}
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	rom, err := os.ReadFile(filepath.Join(romRoot, "ar-jp.sfc"))
	if err != nil {
		t.Fatal(err)
	}
	if w := mediaUpload(t, app, rom, false); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	path := filepath.Join(root, "game-assets", "regions", "jp.armedia")
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	extraction, err := regionalmedia.Extract(rom)
	if err != nil {
		t.Fatal(err)
	}
	want, err := regionalmedia.Pack(extraction)
	if err != nil || !bytes.Equal(got, want) {
		t.Fatal("GUI/CLI extraction drift", err)
	}
	if err = os.WriteFile(path, []byte("older format"), 0600); err != nil {
		t.Fatal(err)
	}
	w := mediaUpload(t, app, got, false)
	if w.Code != 409 {
		t.Fatal(w.Code, w.Body.String())
	}
	var conflict map[string]string
	if err = json.Unmarshal(w.Body.Bytes(), &conflict); err != nil || conflict["code"] != "replace_required" || conflict["release"] != "jp" {
		t.Fatal(conflict, err)
	}
	if w = mediaUpload(t, app, got, true); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	if _, err = os.Stat(filepath.Join(root, "user-rom.sfc")); !os.IsNotExist(err) {
		t.Fatal("donor replaced the US build ROM")
	}
}
