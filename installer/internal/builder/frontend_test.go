package builder

import (
	"context"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestFrontendIsEmbeddedAndSessionScoped(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	for path, want := range map[string]int{
		"/tok/builder/theme.css": 200, "/tok/builder/app.js": 200,
		"/tok/builder/file-input.js":  200,
		"/tok/builder/scene.js":       200,
		"/tok/builder/encounters.mjs": 200,
		"/builder/app.js":             404, "/tok/builder/missing.js": 404,
		"/tok/builder/../gui.go": 404,
	} {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", path, nil))
		if w.Code != want {
			t.Fatalf("%s: %d want %d", path, w.Code, want)
		}
		if want == 200 && (w.Body.Len() == 0 || !strings.HasPrefix(w.Header().Get("Content-Type"), "text/")) {
			t.Fatal("missing frontend", path)
		}
	}
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/", nil))
	if strings.Contains(w.Body.String(), "<script>") || !strings.Contains(w.Header().Get("Content-Security-Policy"), "script-src 'self';") {
		t.Fatal("frontend regressed to inline scripts")
	}
}
