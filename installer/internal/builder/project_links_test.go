package builder

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
)

func TestProjectLinksOpenOnlyFixedPagesFromScopedPost(t *testing.T) {
	var opened []string
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		openURL: func(address string) error {
			opened = append(opened, address)
			return nil
		},
	}, "tok")
	for _, request := range []struct{ method, path string }{
		{http.MethodGet, "/tok/project-links/releases"},
		{http.MethodPost, "/project-links/releases"},
		{http.MethodPost, "/other/project-links/releases"},
		{http.MethodPost, "/tok/project-links/https://example.com"},
		{http.MethodPost, "/tok/project-links/latest"},
	} {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest(request.method, request.path, nil))
		if w.Code != http.StatusNotFound || len(opened) != 0 {
			t.Fatalf("%s %s: status %d, opened %v", request.method, request.path, w.Code, opened)
		}
	}
	for _, page := range []string{"releases", "repository"} {
		w := httptest.NewRecorder()
		app.ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/tok/project-links/"+page, nil))
		if w.Code != http.StatusOK {
			t.Fatal(w.Code, w.Body.String())
		}
	}
	want := []string{
		"https://github.com/DerrickGold/ar-recomp/releases",
		"https://github.com/DerrickGold/ar-recomp",
	}
	if !reflect.DeepEqual(opened, want) {
		t.Fatalf("opened %v, want %v", opened, want)
	}
}

func TestProjectLinkBrowserFailureIsReported(t *testing.T) {
	app := newApplication(context.Background(), Options{
		ProjectRoot: t.TempDir(),
		openURL:     func(string) error { return errors.New("browser unavailable") },
	}, "tok")
	w := httptest.NewRecorder()
	app.ServeHTTP(w, httptest.NewRequest(http.MethodPost, "/tok/project-links/releases", nil))
	if w.Code != http.StatusInternalServerError ||
		!strings.Contains(w.Body.String(), `"errorCode":"builder.links.open_failed"`) {
		t.Fatal(w.Code, w.Body.String())
	}
}
