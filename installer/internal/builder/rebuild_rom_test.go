package builder

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func rebuildRequest(t *testing.T, reuse bool, filename, contents string) *http.Request {
	t.Helper()
	var body bytes.Buffer
	w := multipart.NewWriter(&body)
	if reuse {
		if err := w.WriteField("reuseROM", "true"); err != nil {
			t.Fatal(err)
		}
	}
	// This untrusted path must never control the server's filesystem access.
	if err := w.WriteField("romPath", "/arbitrary/user/file.sfc"); err != nil {
		t.Fatal(err)
	}
	if filename != "" {
		part, err := w.CreateFormFile("rom", filename)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := io.WriteString(part, contents); err != nil {
			t.Fatal(err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}
	r := httptest.NewRequest(http.MethodPost, "/tok/build", &body)
	r.Header.Set("Content-Type", w.FormDataContentType())
	return r
}

func TestRebuildReusesInstalledROMOnReopenAndRelocation(t *testing.T) {
	for _, name := range []string{"user-rom.sfc", "game.sfc", "utils/../game.sfc"} {
		t.Run(name, func(t *testing.T) {
			old := filepath.Join(t.TempDir(), "old game")
			if err := os.MkdirAll(filepath.Join(old, "utils"), 0700); err != nil {
				t.Fatal(err)
			}
			if err := os.WriteFile(filepath.Join(old, name), []byte("previous ROM"), 0600); err != nil {
				t.Fatal(err)
			}
			moved := filepath.Join(filepath.Dir(old), "moved 日本語 game")
			if err := os.Rename(old, moved); err != nil {
				t.Fatal(err)
			}
			root := moved
			if strings.HasPrefix(name, "utils/") {
				root = filepath.Join(moved, "utils")
			}
			built := make(chan string, 1)
			app := newApplication(context.Background(), Options{ProjectRoot: root, Build: func(_ context.Context, rom string, _ io.Writer) (Result, error) {
				built <- rom
				return Result{}, nil
			}}, "tok")
			w := httptest.NewRecorder()
			app.ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/tok/status", nil))
			var status status
			if err := json.Unmarshal(w.Body.Bytes(), &status); err != nil {
				t.Fatal(err)
			}
			if status.RebuildROM != filepath.Join(moved, name) {
				t.Fatalf("ROM: %q", status.RebuildROM)
			}
			w = httptest.NewRecorder()
			app.ServeHTTP(w, rebuildRequest(t, true, "", ""))
			if w.Code != http.StatusAccepted {
				t.Fatalf("%d: %s", w.Code, w.Body.String())
			}
			select {
			case rom := <-built:
				if rom != status.RebuildROM {
					t.Fatal(rom)
				}
			case <-time.After(2 * time.Second):
				t.Fatal("rebuild did not start")
			}
		})
	}
}

func TestRebuildMissingROMIsActionableAndDoesNotChangePlayerData(t *testing.T) {
	root := t.TempDir()
	for _, name := range []string{"user-rom.sfc", "game.exe", "settings.ini", "save.srm"} {
		if err := os.WriteFile(filepath.Join(root, name), []byte(name), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if _, err := reusableBuildROM(root); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(filepath.Join(root, "user-rom.sfc")); err != nil {
		t.Fatal(err)
	}
	app := newApplication(context.Background(), Options{ProjectRoot: root, Build: func(context.Context, string, io.Writer) (Result, error) {
		t.Error("build started without a saved ROM")
		return Result{}, nil
	}}, "tok")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, rebuildRequest(t, true, "", ""))
	if w.Code != 400 || !strings.Contains(w.Body.String(), "builder.build.saved_rom_missing") {
		t.Fatal(w.Code, w.Body.String())
	}
	for _, name := range []string{"game.exe", "settings.ini", "save.srm"} {
		if data, err := os.ReadFile(filepath.Join(root, name)); err != nil || string(data) != name {
			t.Fatal("player file changed", name, err)
		}
	}
}

func TestRebuildExplicitUploadOverridesSavedROMAndBusyRequestIsRefused(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "user-rom.sfc"), []byte("old"), 0600); err != nil {
		t.Fatal(err)
	}
	started, release := make(chan string, 1), make(chan struct{})
	defer close(release)
	app := newApplication(context.Background(), Options{ProjectRoot: root, Build: func(_ context.Context, rom string, _ io.Writer) (Result, error) {
		data, _ := os.ReadFile(rom)
		started <- string(data)
		<-release
		return Result{}, nil
	}}, "tok")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, rebuildRequest(t, true, "replacement.SMC", "replacement"))
	if w.Code != 202 {
		t.Fatal(w.Code, w.Body.String())
	}
	select {
	case data := <-started:
		if data != "replacement" {
			t.Fatal(data)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("build did not start")
	}
	w = httptest.NewRecorder()
	app.ServeHTTP(w, rebuildRequest(t, true, "different.sfc", "must not replace"))
	if w.Code != 409 {
		t.Fatal(w.Code, w.Body.String())
	}
	if data, _ := os.ReadFile(filepath.Join(root, "user-rom.sfc")); string(data) != "replacement" {
		t.Fatal("busy request replaced ROM")
	}
}

func TestRebuildROMProbeRejectsUnsafeCopies(t *testing.T) {
	for _, kind := range []string{"empty", "oversize", "directory", "symlink", "unrelated parent"} {
		t.Run(kind, func(t *testing.T) {
			parent := t.TempDir()
			root := filepath.Join(parent, "game")
			if err := os.Mkdir(root, 0700); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(root, "user-rom.sfc")
			switch kind {
			case "directory":
				if err := os.Mkdir(path, 0700); err != nil {
					t.Fatal(err)
				}
			case "symlink":
				outside := filepath.Join(parent, "input.sfc")
				if err := os.WriteFile(outside, []byte("ROM"), 0600); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(outside, path); err != nil {
					t.Skip(err)
				}
			case "unrelated parent":
				if err := os.WriteFile(filepath.Join(parent, "user-rom.sfc"), []byte("other game"), 0600); err != nil {
					t.Fatal(err)
				}
			default:
				f, err := os.Create(path)
				if err != nil {
					t.Fatal(err)
				}
				if kind == "oversize" {
					err = f.Truncate(maxROMBytes + 1)
				}
				f.Close()
				if err != nil {
					t.Fatal(err)
				}
			}
			if rom, err := reusableBuildROM(root); err == nil || rom != "" {
				t.Fatal("accepted", kind, rom)
			}
		})
	}
}
