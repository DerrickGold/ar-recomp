package host

import (
	"context"
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func outputFixture(t *testing.T) (*OutputSelection, string) {
	t.Helper()
	base, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	work := filepath.Join(base, "BuilderData")
	if err := os.Mkdir(work, 0700); err != nil {
		t.Fatal(err)
	}
	return NewOutputSelection(filepath.Join(base, "ActRaiserRecompBuilder.app"), work, filepath.Join(base, "payload")), filepath.Join(base, "ActRaiserRecomp")
}

func outputCall(t *testing.T, s *OutputSelection, endpoint string, body any) *httptest.ResponseRecorder {
	t.Helper()
	data, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	w := httptest.NewRecorder()
	s.ServeHTTP(w, httptest.NewRequest("POST", "http://127.0.0.1/__shell/output/"+endpoint, strings.NewReader(string(data))))
	return w
}

func reviewOutput(t *testing.T, s *OutputSelection, path string) OutputReview {
	t.Helper()
	w := outputCall(t, s, "review", map[string]string{"directory": path})
	if w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	var review OutputReview
	if err := json.Unmarshal(w.Body.Bytes(), &review); err != nil {
		t.Fatal(err)
	}
	return review
}

func TestOutputFirstLaunchWritesNothingUntilConfirmed(t *testing.T) {
	s, path := outputFixture(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := s.Initial(ctx, ""); err != context.Canceled {
		t.Fatal(err)
	}
	if s.candidate != path || !s.NeedsChoice() {
		t.Fatal("default chooser missing", s.candidate)
	}
	review := reviewOutput(t, s, path)
	if review.Existing || review.Game {
		t.Fatal(review)
	}
	for _, p := range []string{path, filepath.Join(s.workspace, outputPreferenceName)} {
		if _, err := os.Stat(p); !os.IsNotExist(err) {
			t.Fatal("review wrote files", p, err)
		}
	}
	w := outputCall(t, s, "apply", map[string]any{"revision": review.Revision})
	if w.Code != 200 {
		t.Fatal(w.Body.String())
	}
	if got, err := s.Wait(context.Background()); err != nil || got != path {
		t.Fatal(got, err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatal("selection seeded output", err)
	}
	data, err := os.ReadFile(filepath.Join(s.workspace, outputPreferenceName))
	if err != nil {
		t.Fatal(err)
	}
	var pref outputPreference
	json.Unmarshal(data, &pref)
	if !pref.Relative || pref.Path != "ActRaiserRecomp" {
		t.Fatal(pref)
	}
}

func TestNonemptyOutputRequiresFreshExplicitReview(t *testing.T) {
	s, path := outputFixture(t)
	os.Mkdir(path, 0755)
	file := filepath.Join(path, "my-document.txt")
	os.WriteFile(file, []byte("leave me"), 0600)
	review := reviewOutput(t, s, path)
	if !review.Existing || review.Game {
		t.Fatal(review)
	}
	if w := outputCall(t, s, "apply", map[string]any{"revision": review.Revision}); w.Code != 400 {
		t.Fatal(w.Code, w.Body.String())
	}
	os.WriteFile(filepath.Join(path, "new-document.txt"), nil, 0600)
	if w := outputCall(t, s, "apply", map[string]any{"revision": review.Revision, "confirm": true}); w.Code != 409 {
		t.Fatal(w.Code, w.Body.String())
	}
	review = reviewOutput(t, s, path)
	if w := outputCall(t, s, "apply", map[string]any{"revision": review.Revision, "confirm": true}); w.Code != 200 {
		t.Fatal(w.Code, w.Body.String())
	}
	if data, _ := os.ReadFile(file); string(data) != "leave me" {
		t.Fatal("unrelated file modified")
	}
}

func TestOutputRejectsUnsafeAndCollidingDestinations(t *testing.T) {
	s, path := outputFixture(t)
	file := filepath.Join(filepath.Dir(path), "file")
	os.WriteFile(file, nil, 0600)
	for _, p := range []string{"relative", filepath.Dir(path), s.workspace, filepath.Join(s.workspace, "game"), filepath.Join(s.artifact, "Contents", "game"), filepath.Join(filepath.Dir(path), "payload", "game"), file, filepath.Join(path, "Other.app", "game")} {
		if _, err := s.review(p); err == nil {
			t.Errorf("accepted unsafe path %s", p)
		}
	}
	os.Mkdir(path, 0755)
	os.WriteFile(filepath.Join(path, "ActRaiserRecomp.exe"), []byte("unrelated"), 0600)
	if _, err := s.review(path); err == nil {
		t.Fatal("accepted unrecognized game filename")
	}
	os.WriteFile(filepath.Join(path, ".actraiser-seed.json"), []byte("{}"), 0600)
	if review, err := s.review(path); err != nil || !review.Game {
		t.Fatal(review, err)
	}
	alias := filepath.Join(filepath.Dir(path), "alias")
	if err := os.Symlink(s.workspace, alias); err == nil {
		if _, err := s.review(filepath.Join(alias, "new")); err == nil {
			t.Fatal("accepted workspace ancestor alias")
		}
	}
}

func TestOutputWarmLaunchAndPortableRelocation(t *testing.T) {
	s, path := outputFixture(t)
	os.Mkdir(path, 0755)
	os.WriteFile(filepath.Join(path, ".actraiser-seed.json"), []byte("{}"), 0600)
	if err := s.savePreference(path); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if got, err := s.Initial(ctx, ""); err != nil || got != path {
		t.Fatal(got, err)
	}
	moved := filepath.Join(t.TempDir(), "moved")
	if err := os.Rename(filepath.Dir(s.artifact), moved); err != nil {
		t.Fatal(err)
	}
	next := NewOutputSelection(filepath.Join(moved, filepath.Base(s.artifact)), filepath.Join(moved, "BuilderData"))
	want, _ := filepath.EvalSymlinks(filepath.Join(moved, "ActRaiserRecomp"))
	if got, err := next.Initial(ctx, ""); err != nil || got != want {
		t.Fatal(got, want, err)
	}
}

func TestLegacyOutputRequiresImportIntoNewLayout(t *testing.T) {
	s, path := outputFixture(t)
	legacy := filepath.Join(path, "utils", "game-assets")
	if err := os.MkdirAll(legacy, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(legacy, "manifest.ini"), []byte("legacy assets"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := s.review(path); err == nil || !strings.Contains(err.Error(), "Import previous installation") {
		t.Fatal("legacy data was silently accepted in the wrong layout", err)
	}
	if _, err := s.review(filepath.Join(path, "ActRaiserRecomp")); err != nil {
		t.Fatal("could not choose a new child output folder", err)
	}
}

func TestOutputChangesApplyNextLaunchAndMissingOutputReprompts(t *testing.T) {
	s, path := outputFixture(t)
	if got, err := s.Initial(context.Background(), path); err != nil || got != path {
		t.Fatal(got, err)
	}
	external, _ := filepath.EvalSymlinks(t.TempDir())
	nextPath := filepath.Join(external, "Game")
	review := reviewOutput(t, s, nextPath)
	w := outputCall(t, s, "apply", map[string]any{"revision": review.Revision})
	if w.Code != 200 || !strings.Contains(w.Body.String(), `"restartRequired":true`) {
		t.Fatal(w.Code, w.Body.String())
	}
	if s.current != path || s.next != nextPath {
		t.Fatal("live destination changed")
	}
	if _, err := os.Stat(nextPath); !os.IsNotExist(err) {
		t.Fatal("next destination created early")
	}
	data, _ := os.ReadFile(filepath.Join(s.workspace, outputPreferenceName))
	var pref outputPreference
	json.Unmarshal(data, &pref)
	if pref.Relative || pref.Path != nextPath {
		t.Fatal(pref)
	}
	next := NewOutputSelection(s.artifact, s.workspace)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if got, err := next.Initial(ctx, ""); err != nil || got != nextPath {
		t.Fatal("new chosen folder prompted twice", got, err)
	}
	if err := next.Started(); err != nil {
		t.Fatal(err)
	}
	next = NewOutputSelection(s.artifact, s.workspace)
	if _, err := next.Initial(ctx, ""); err != context.Canceled {
		t.Fatal(err)
	}
	if !next.NeedsChoice() || next.warning == "" || next.candidate != nextPath {
		t.Fatal("missing output silently accepted")
	}
}

func TestOutputPreferenceCorruptionAndSymlinkAreNotFollowed(t *testing.T) {
	for _, data := range []string{`broken`, `{"schema":1,"path":"../escape","relativeToBuilder":true}`, `{"schema":2,"path":"/elsewhere"}`} {
		s, _ := outputFixture(t)
		os.WriteFile(filepath.Join(s.workspace, outputPreferenceName), []byte(data), 0600)
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		if _, err := s.Initial(ctx, ""); err != context.Canceled || s.warning == "" {
			t.Fatal(err, s.warning)
		}
	}
	s, path := outputFixture(t)
	other := filepath.Join(filepath.Dir(path), "other.json")
	os.WriteFile(other, []byte("do not replace"), 0600)
	if err := os.Symlink(other, filepath.Join(s.workspace, outputPreferenceName)); err != nil {
		t.Skip(err)
	}
	if err := s.savePreference(path); err == nil {
		t.Fatal("replaced linked preference")
	}
	if data, _ := os.ReadFile(other); string(data) != "do not replace" {
		t.Fatal("followed symlink")
	}
}

func TestOutputHTTPGuardsAndChooserCancellation(t *testing.T) {
	s, path := outputFixture(t)
	s.SetChooser(func() (string, error) { return "", nil })
	if w := outputCall(t, s, "choose", map[string]any{}); w.Code != 200 || !strings.Contains(w.Body.String(), `"directory":""`) {
		t.Fatal(w.Code, w.Body.String())
	}
	w := httptest.NewRecorder()
	r := httptest.NewRequest("POST", "http://127.0.0.1/__shell/output/review", strings.NewReader(`{"directory":"`+filepath.ToSlash(path)+`"}`))
	r.Header.Set("Origin", "https://untrusted.example")
	s.ServeHTTP(w, r)
	if w.Code != 403 {
		t.Fatal(w.Code)
	}
	if w := outputCall(t, s, "apply", map[string]any{"revision": "not-reviewed", "confirm": true}); w.Code != 409 {
		t.Fatal(w.Code)
	}
	if w := outputCall(t, s, "review", map[string]any{"directory": path, "unknown": true}); w.Code != 400 {
		t.Fatal(w.Code)
	}
	bridge := &Bridge{}
	bridge.SetOutputSelection(s)
	w = httptest.NewRecorder()
	bridge.Bootstrap(w, httptest.NewRequest("GET", "wails://wails/__shell/output/state", nil))
	if w.Code != 404 {
		t.Fatal("native scheme exposed selector", w.Code)
	}
	s.current = path
	s.Retry(os.ErrPermission)
	if !s.NeedsChoice() || s.warning == "" {
		t.Fatal("startup failure cannot recover")
	}
}
