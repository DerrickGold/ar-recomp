package host

import (
	"bytes"
	"encoding/json"
	"io"
	"mime"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type archiveTestTransport func(*http.Request) (*http.Response, error)

func (transport archiveTestTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	return transport(r)
}

func TestNativeArchiveSaveUsesChosenDestination(t *testing.T) {
	const name = "music café.o2r"
	prepared := "mods/download/" + strings.Repeat("a", 36)
	destination := filepath.Join(t.TempDir(), "renamed.o2r")
	bridge := &Bridge{}
	bridge.Connect(&url.URL{Scheme: "http", Host: "127.0.0.1:9", Path: "/builder-session/"})
	pickerCalls, reads := 0, 0
	pick := destination
	bridge.SetSavePicker(func(gotName, title string) (string, error) {
		pickerCalls++
		if gotName != name || title != "Save archive" {
			t.Fatalf("picker got %q and %q", gotName, title)
		}
		return pick, nil
	})
	contents := []byte("prepared archive")
	size := int64(len(contents))
	expected := "/builder-session/" + prepared
	bridge.proxy.Transport = archiveTestTransport(func(r *http.Request) (*http.Response, error) {
		reads++
		if r.URL.EscapedPath() != expected || r.Method != http.MethodGet {
			t.Fatalf("unexpected download request: %s %s", r.Method, r.URL.EscapedPath())
		}
		headers := http.Header{"Content-Type": {"application/zip"}}
		headers.Set("Content-Disposition", mime.FormatMediaType("attachment", map[string]string{"filename": name}))
		return &http.Response{
			StatusCode: http.StatusOK, Header: headers,
			Body: io.NopCloser(bytes.NewReader(contents)), ContentLength: size,
		}, nil
	})
	save := func(route string) *httptest.ResponseRecorder {
		t.Helper()
		body, err := json.Marshal(map[string]string{"url": route, "name": name, "title": "Save archive"})
		if err != nil {
			t.Fatal(err)
		}
		r := httptest.NewRequest(http.MethodPost, "/__shell/save-archive", bytes.NewReader(body))
		r.Header.Set("Content-Type", "application/json")
		w := httptest.NewRecorder()
		bridge.ServeHTTP(w, r)
		return w
	}
	for _, bad := range []string{"http://127.0.0.1:9/music", "../mods/archive/music.o2r", prepared + "?different=1"} {
		if w := save(bad); w.Code != http.StatusBadRequest || reads != 0 || pickerCalls != 0 {
			t.Fatalf("accepted unexpected route: %d %s", w.Code, w.Body)
		}
	}
	if w := save(prepared); w.Code != http.StatusOK || !strings.Contains(w.Body.String(), `"saved":true`) {
		t.Fatalf("save failed: %d %s", w.Code, w.Body)
	}
	if data, err := os.ReadFile(destination); err != nil || !bytes.Equal(data, contents) {
		t.Fatalf("saved archive = %q: %v", data, err)
	}

	pick = ""
	if w := save(prepared); w.Code != http.StatusOK || reads != 1 || !strings.Contains(w.Body.String(), `"saved":false`) {
		t.Fatalf("cancelled save should not read the archive: %d %s", w.Code, w.Body)
	}

	pick = destination
	installed := "mods/archive/" + url.PathEscape(name)
	expected = "/builder-session/" + installed
	contents, size = []byte("incomplete"), 100
	if w := save(installed); w.Code != http.StatusInternalServerError || reads != 2 {
		t.Fatalf("partial transfer result: %d %s", w.Code, w.Body)
	}
	if data, err := os.ReadFile(destination); err != nil || string(data) != "prepared archive" {
		t.Fatalf("incomplete download replaced the destination: %q (%v)", data, err)
	}
}
