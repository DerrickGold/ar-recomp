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

	"github.com/DerrickGold/ar-recomp/installer/internal/ar2master"
)

func uploadGameContent(t *testing.T, app *application, target string, data []byte) *httptest.ResponseRecorder {
	t.Helper()
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	part, err := w.CreateFormFile("rom", "../../user-rom.sfc")
	if err != nil {
		t.Fatal(err)
	}
	if _, err = part.Write(data); err != nil {
		t.Fatal(err)
	}
	if err = w.WriteField("content", "ar2-master"); err != nil {
		t.Fatal(err)
	}
	if err = w.Close(); err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest("POST", target, &body)
	r.Header.Set("Content-Type", w.FormDataContentType())
	response := httptest.NewRecorder()
	app.ServeHTTP(response, r)
	return response
}

func TestGameImportsHTTP(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	for path, code := range map[string]int{"/tok/game-imports": 200, "/game-imports": 404, "/other/game-imports": 404} {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", path, nil))
		if w.Code != code {
			t.Fatal(path, w.Code)
		}
		if code == 200 {
			var catalog struct {
				Imports []json.RawMessage `json:"imports"`
			}
			if err := json.Unmarshal(w.Body.Bytes(), &catalog); err != nil || len(catalog.Imports) != 0 {
				t.Fatal("fresh installation advertises prototype imports", w.Body.String(), err)
			}
		}
	}
	for _, data := range [][]byte{[]byte("wrong ROM"), make([]byte, ar2master.ROMBytes+513)} {
		if w := uploadGameContent(t, app, "/tok/game-imports", data); w.Code != 400 {
			t.Fatal(w.Code, w.Body.String())
		}
	}
	entries, _ := os.ReadDir(root)
	if len(entries) != 0 {
		t.Fatal("rejected import wrote files")
	}
	app.dataGate.Lock()
	w := uploadGameContent(t, app, "/tok/game-imports", []byte("ROM"))
	app.dataGate.Unlock()
	if w.Code != 409 {
		t.Fatal("installation import exclusion lost")
	}
}

func TestGameImportContentComesFromMultipart(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	rom := []byte("wrong ROM")
	want := uploadGameContent(t, app, "/tok/game-imports", rom)
	if want.Code != 400 {
		t.Fatal(want.Code, want.Body.String())
	}
	// Query parameters must not select a different importer from the form we
	// validated. Both requests should reach the selected ROM validator.
	got := uploadGameContent(t, app, "/tok/game-imports?content=unknown-content", rom)
	if got.Code != want.Code || got.Body.String() != want.Body.String() {
		t.Fatalf("query changed import selection: %d %s; want %d %s", got.Code, got.Body.String(), want.Code, want.Body.String())
	}
}

func TestCharacterLocalROMImport(t *testing.T) {
	path := os.Getenv("AR2_MASTER_ROM")
	if path == "" {
		t.Skip("optional local ActRaiser 2 ROM integration")
	}
	rom, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	base := filepath.Join(root, "user-rom.sfc")
	if err := os.WriteFile(base, []byte("original game"), 0600); err != nil {
		t.Fatal(err)
	}
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	if w := uploadGameContent(t, app, "/tok/game-imports?content=unknown-content", rom); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	want, err := ar2master.Extract(rom)
	if err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(root, "game-assets", "imports", ar2master.Filename))
	if err != nil || !bytes.Equal(got, want) {
		t.Fatal("GUI and CLI extraction differ", err)
	}
	if data, _ := os.ReadFile(base); string(data) != "original game" {
		t.Fatal("overwrote base ROM")
	}
	if ar2master.Status(root) != "installed" {
		t.Fatal("import unavailable")
	}
}
