package builder

import (
	"archive/zip"
	"bytes"
	"context"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestManualPackConflictAndLegacyFallback(t *testing.T) {
	root, external := t.TempDir(), t.TempDir()
	legacy := albumPDF(t, 3)
	legacyPath := installManual(t, root, legacy)
	second := albumPDF(t, 5)
	for _, item := range []struct {
		id   string
		book []byte
	}{{"first-manual", albumPDF(t, 4)}, {"second-manual", second}} {
		var archive bytes.Buffer
		if err := exportManualMod(&archive, modMetadata{Name: item.id}, item.id, bytes.NewReader(item.book)); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(external, item.id+".o2r"), archive.Bytes(), 0644); err != nil {
			t.Fatal(err)
		}
	}
	app := newApplication(context.Background(), Options{ProjectRoot: root, ModDirectories: []string{external}}, "tok")
	send := func(method, path string, body []byte) *httptest.ResponseRecorder {
		t.Helper()
		rec := httptest.NewRecorder()
		app.ServeHTTP(rec, httptest.NewRequest(method, "/tok/"+path, bytes.NewReader(body)))
		if rec.Code != http.StatusOK {
			t.Fatalf("%s %s = %d: %s", method, path, rec.Code, rec.Body)
		}
		return rec
	}
	checkSlot := func(rec *httptest.ResponseRecorder, winner string) {
		t.Helper()
		var catalog modCatalog
		if err := json.Unmarshal(rec.Body.Bytes(), &catalog); err != nil {
			t.Fatal(err)
		}
		if len(catalog.Slots) != 1 || catalog.Slots[0].ID != manualModTarget || catalog.Slots[0].Winner != winner {
			t.Fatalf("unexpected manual slot: %+v", catalog.Slots)
		}
	}
	assertManual := func(data []byte, pages int) {
		t.Helper()
		rec := send(http.MethodGet, "manual", nil)
		var status manualStatus
		if err := json.Unmarshal(rec.Body.Bytes(), &status); err != nil {
			t.Fatal(err)
		}
		if !status.Present || !status.LegacyPresent || !status.InGame || status.Pages != pages {
			t.Fatalf("manual status = %+v, want %d pages and the legacy PDF present", status, pages)
		}
		rec = send(http.MethodGet, "manual.pdf", nil)
		if !bytes.Equal(rec.Body.Bytes(), data) {
			t.Fatal("served the wrong manual")
		}
	}
	set := func(action, id string) *httptest.ResponseRecorder {
		t.Helper()
		body, err := json.Marshal(modChange{Action: action, Pack: id, Slot: manualModTarget})
		if err != nil {
			t.Fatal(err)
		}
		return send(http.MethodPost, "mods/activation", body)
	}

	checkSlot(send(http.MethodGet, "mods/catalog", nil), "")
	assertManual(legacy, 3)
	checkSlot(set("select", "second-manual"), "second-manual")
	assertManual(second, 5)
	active, _, _ := activeManualPack(root)
	if err := os.Remove(active); err != nil {
		t.Fatal(err)
	}
	assertManual(legacy, 3)
	checkSlot(send(http.MethodGet, "mods/catalog", nil), "second-manual")
	assertManual(second, 5)

	set("disable", "first-manual")
	var disabled modCatalog
	if err := json.Unmarshal(set("disable", "second-manual").Body.Bytes(), &disabled); err != nil {
		t.Fatal(err)
	}
	for _, pack := range disabled.Packs {
		if pack.ID != "second-manual" {
			continue
		}
		if pack.Enabled || pack.ManualPages != 5 || len(pack.Previews) != 1 {
			t.Fatalf("unexpected disabled pack preview: %+v", pack)
		}
		image := send(http.MethodGet, pack.Previews[0]+"?page=2", nil)
		if got := image.Header().Get("Content-Type"); got != "image/jpeg" || !bytes.Equal(image.Body.Bytes(), syntheticJPEG(1024, 1448, 4096)) {
			t.Fatalf("incorrect preview page type %s or JPEG bytes", got)
		}
	}
	assertManual(legacy, 3)
	if got, err := os.ReadFile(legacyPath); err != nil || !bytes.Equal(got, legacy) {
		t.Fatal("mod activation changed the imported legacy manual", err)
	}
	for _, id := range []string{"first-manual", "second-manual"} {
		if _, err := os.Stat(filepath.Join(external, id+".o2r")); err != nil {
			t.Fatal("disabling a manual removed the external archive", err)
		}
	}
}

func TestManualPackExportInstallAndValidation(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	defer app.replaceModDownload(nil)
	book := albumPDF(t, 6)
	var upload bytes.Buffer
	form := multipart.NewWriter(&upload)
	for name, value := range map[string]string{"id": "game-guide", "name": "Game guide", "author": "Fixture", "version": "1.0.0"} {
		if err := form.WriteField(name, value); err != nil {
			t.Fatal(err)
		}
	}
	part, err := form.CreateFormFile("manual", "game-guide.pdf")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := part.Write(book); err != nil {
		t.Fatal(err)
	}
	if err := form.Close(); err != nil {
		t.Fatal(err)
	}
	export := httptest.NewRequest(http.MethodPost, "/tok/mods/export-manual", &upload)
	export.Header.Set("Content-Type", form.FormDataContentType())
	rec := httptest.NewRecorder()
	app.ServeHTTP(rec, export)
	var response map[string]string
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil || rec.Code != http.StatusOK || !strings.HasPrefix(response["name"], "manual-") {
		t.Fatalf("export failed: %d %s (%v)", rec.Code, rec.Body, err)
	}
	rec = httptest.NewRecorder()
	app.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/tok/"+response["url"], nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("download failed: %d", rec.Code)
	}
	path := filepath.Join(t.TempDir(), response["name"])
	if err := os.WriteFile(path, rec.Body.Bytes(), 0644); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	if code := RunMods([]string{"install", "--output-dir", root, path}, &stdout, &stderr); code != 0 {
		t.Fatal(code, stdout.String(), stderr.String())
	}
	if got := app.manualStatus(); got.Pages != 6 || got.LegacyPresent {
		t.Fatalf("pack should be active without installing a loose PDF: %+v", got)
	}
	if _, err := os.Stat(manualPath(root)); !os.IsNotExist(err) {
		t.Fatal("manual install wrote to the legacy path", err)
	}

	var invalid bytes.Buffer
	archive := zip.NewWriter(&invalid)
	file, err := archive.Create("text.pdf")
	if err != nil {
		t.Fatal(err)
	}
	if _, err := file.Write([]byte("%PDF-1.4\nText-only PDF.\n%%EOF\n")); err != nil {
		t.Fatal(err)
	}
	if err := finishModArchive(archive, modMetadata{Name: "Invalid"}, modPayload{
		Format: 1, ID: "invalid-book", Type: manualModType, Game: modGameID, Manual: "text.pdf",
	}, nil); err != nil {
		t.Fatal(err)
	}
	rec = httptest.NewRecorder()
	app.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/tok/mods/install?type=actraiser-manual", &invalid))
	if rec.Code != http.StatusBadRequest || app.manualStatus().Pages != 6 {
		t.Fatalf("non-album pack was installed: %d %s", rec.Code, rec.Body)
	}
}

func TestManualPackPreflightDistinguishesInvalidAndReaderOnlyPDFs(t *testing.T) {
	root := t.TempDir()
	app := newApplication(context.Background(), Options{ProjectRoot: root}, "tok")
	for _, test := range []struct {
		name string
		data []byte
		code int
		ok   bool
	}{
		{"scan album", albumPDF(t, 3), http.StatusOK, true},
		{"text PDF", []byte("%PDF-1.4\nText only.\n%%EOF\n"), http.StatusOK, false},
		{"truncated PDF", []byte("%PDF-1.4\nNo trailer."), http.StatusBadRequest, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			response := httptest.NewRecorder()
			app.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/tok/mods/validate-manual", bytes.NewReader(test.data)))
			if response.Code != test.code {
				t.Fatalf("preflight returned %d: %s", response.Code, response.Body)
			}
			if test.code == http.StatusOK {
				var result struct {
					Valid bool
					Pages int
				}
				if err := json.Unmarshal(response.Body.Bytes(), &result); err != nil || result.Valid != test.ok ||
					test.ok && result.Pages != 3 {
					t.Fatalf("preflight result: %+v %v", result, err)
				}
			}
		})
	}
	if hasModSources(root, nil) || regularFileExists(manualPath(root)) {
		t.Fatal("preflight installed a pack or saved a loose manual")
	}
}
