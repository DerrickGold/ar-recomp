package builder

import (
	"context"
	"net/http/httptest"
	"regexp"
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

// New scripts must be served as well as embedded. Exercising every page script
// catches a missing route even when its isolated JavaScript tests pass.
func TestPageScriptsHaveFrontendRoutes(t *testing.T) {
	app := newApplication(context.Background(), Options{ProjectRoot: t.TempDir()}, "tok")
	for _, match := range regexp.MustCompile(`<script[^>]+src="(builder/[^"]+)"`).FindAllStringSubmatch(pageHTML, -1) {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest("GET", "/tok/"+match[1], nil))
		if w.Code != 200 || !strings.HasPrefix(w.Header().Get("Content-Type"), "text/javascript") {
			t.Errorf("page script %s unavailable: %d, %s", match[1], w.Code, w.Header().Get("Content-Type"))
		}
	}
}
