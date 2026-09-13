package host

import (
	"context"
	"crypto/sha256"
	_ "embed"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"github.com/DerrickGold/ar-recomp/installer/internal/buildworkspace"
	"github.com/DerrickGold/ar-recomp/installer/internal/workshopui"
)

const outputPreferenceName = "game-output.json"

//go:embed output.html
var outputHTML string

//go:embed output.js
var outputJS string

type outputPreference struct {
	Schema   int    `json:"schema"`
	Path     string `json:"path"`
	Relative bool   `json:"relativeToBuilder"`
	Create   bool   `json:"createOnNextLaunch,omitempty"`
}

type OutputReview struct {
	Directory string   `json:"directory"`
	Revision  string   `json:"revision"`
	Existing  bool     `json:"existing"`
	Game      bool     `json:"game"`
	Entries   []string `json:"entries"`
}

// OutputSelection owns only the destination preference. Reviewing/choosing a
// path never seeds or clears that directory. The backend is started only after
// the first decision; later changes are explicitly deferred to the next launch.
type OutputSelection struct {
	mu                                sync.Mutex
	artifact, workspace               string
	protected                         []string
	candidate, current, next, warning string
	selected                          chan string
	chooser                           func() (string, error)
	choosing                          bool
	explicit                          bool
	pending                           *OutputReview
}

func NewOutputSelection(artifact, workspace string, protected ...string) *OutputSelection {
	return &OutputSelection{artifact: artifact, workspace: workspace, protected: protected, selected: make(chan string, 1)}
}

func (s *OutputSelection) SetChooser(choose func() (string, error)) { s.chooser = choose }

// Initial honors an explicit CLI destination for scripted builds. Interactive
// launches only resume a remembered, recognized game folder; missing/invalid
// preferences return to the chooser instead of silently creating output.
func (s *OutputSelection) Initial(ctx context.Context, override string) (string, error) {
	s.mu.Lock()
	if override != "" {
		s.explicit = true
		path, err := filepath.Abs(override)
		var review OutputReview
		if err == nil {
			review, err = s.review(path)
		}
		if err != nil {
			s.mu.Unlock()
			return "", err
		}
		s.current, s.candidate = review.Directory, review.Directory
		s.mu.Unlock()
		return review.Directory, nil
	}
	s.candidate, _ = DefaultOutputDirectory(s.artifact, "")
	if saved, create, err := s.readPreference(); err == nil {
		s.candidate = saved
		if review, err := s.review(saved); err == nil && (review.Game || create && !review.Existing) {
			s.current = review.Directory
			s.mu.Unlock()
			return review.Directory, nil
		}
		s.warning = "The saved game folder is missing or no longer recognized. Review a destination before continuing."
	} else if !errors.Is(err, os.ErrNotExist) {
		s.warning = "The saved game-folder preference could not be read. Choose a destination again. " + err.Error()
	}
	if s.candidate == "" {
		s.warning = "Choose a writable game folder. The Builder's current location cannot be used as the default."
	}
	s.mu.Unlock()
	return s.Wait(ctx)
}

func (s *OutputSelection) Wait(ctx context.Context) (string, error) {
	select {
	case path := <-s.selected:
		return path, nil
	case <-ctx.Done():
		return "", ctx.Err()
	}
}

func (s *OutputSelection) Retry(err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.current = ""
	s.warning = "Could not open the game folder. Choose another destination or correct the problem and try again. " + err.Error()
	s.pending = nil
}

// Clear the one-time permission to create a newly selected empty destination.
// A previously initialized folder that is later deleted must prompt again.
func (s *OutputSelection) Started() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.explicit {
		return nil
	}
	return s.savePreference(s.current)
}

func (s *OutputSelection) NeedsChoice() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.current == ""
}

func (s *OutputSelection) review(path string) (OutputReview, error) {
	var result OutputReview
	if !filepath.IsAbs(path) {
		return result, errors.New("Enter the full path to your game folder, or use Choose folder….")
	}
	physical, err := buildworkspace.Physical(filepath.Clean(path))
	if err != nil {
		return result, err
	}
	if physical == filepath.Dir(physical) {
		return result, errors.New("choose a game subfolder, not a filesystem root")
	}
	if home, err := os.UserHomeDir(); err == nil {
		resolved, _ := buildworkspace.Physical(home)
		if physical == resolved {
			return result, errors.New("choose a game subfolder, not your home folder")
		}
	}
	for _, protected := range append([]string{s.workspace, s.artifact}, s.protected...) {
		if protected != "" {
			if err := buildworkspace.Separate(physical, protected); err != nil {
				return result, fmt.Errorf("game folder must be separate from the Builder, its inputs and its workspace: %w", err)
			}
		}
	}
	for parent := physical; parent != filepath.Dir(parent); parent = filepath.Dir(parent) {
		if strings.HasSuffix(strings.ToLower(parent), ".app") || strings.HasSuffix(strings.ToLower(parent), ".appdir") {
			return result, errors.New("select the folder containing the game application, not a folder inside an application")
		}
	}
	result.Directory = physical
	hash := sha256.New()
	fmt.Fprintln(hash, physical)
	dir, err := os.Open(physical)
	if errors.Is(err, os.ErrNotExist) {
		result.Revision = fmt.Sprintf("%x", hash.Sum(nil))
		return result, nil
	}
	if err != nil {
		return result, err
	}
	defer dir.Close()
	info, err := dir.Stat()
	if err != nil {
		return result, err
	}
	if !info.IsDir() {
		return result, errors.New("game output must be a directory")
	}
	entries, err := dir.ReadDir(513)
	if err != nil && err != io.EOF {
		return result, err
	}
	if len(entries) > 512 {
		return result, errors.New("this folder contains too many items; choose a dedicated game subfolder")
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name() < entries[j].Name() })
	// Directory metadata is part of the optimistic review receipt. A path that
	// becomes non-empty after review must be reviewed/confirmed again.
	fmt.Fprintf(hash, "%v:%v\n", info.ModTime(), info.Mode())
	result.Existing = len(entries) > 0
	for _, entry := range entries {
		info, err := entry.Info()
		if err != nil {
			return result, err
		}
		fmt.Fprintf(hash, "%s:%v:%d:%v\n", entry.Name(), info.Mode(), info.Size(), info.ModTime())
		if len(result.Entries) < 8 {
			result.Entries = append(result.Entries, entry.Name())
		}
		if entry.Name() == ".actraiser-seed.json" && info.Mode().IsRegular() {
			result.Game = true
		}
	}
	// Recognize the current root-data and desktop artifact layouts, even without
	// a seed receipt. Never follow a symlink to classify someone else's data.
	for _, relative := range []string{"game-assets/manifest.ini", "ActRaiserRecomp.app/Contents/Resources/actraiser-app.json", "ActRaiserRecomp.AppDir/usr/share/ActRaiserRecomp/actraiser-app.json"} {
		p := physical
		for _, part := range strings.Split(relative, "/") {
			p = filepath.Join(p, part)
			info, err := os.Lstat(p)
			if err != nil || info.Mode()&os.ModeSymlink != 0 {
				break
			}
			if p == filepath.Join(physical, filepath.FromSlash(relative)) && info.Mode().IsRegular() {
				result.Game = true
			}
		}
	}
	result.Revision = fmt.Sprintf("%x", hash.Sum(nil))
	if !result.Game {
		if info, err := os.Stat(filepath.Join(physical, "utils", "game-assets", "manifest.ini")); err == nil && info.Mode().IsRegular() {
			return OutputReview{}, errors.New("this legacy installation stores its data in utils; choose a new game subfolder, then use Import previous installation to bring its saves, settings and assets into the new layout")
		}
		for _, entry := range entries {
			name := strings.ToLower(entry.Name())
			if name == "actraiserrecomp" || strings.HasPrefix(name, "actraiserrecomp.") || strings.HasPrefix(name, "sdl3") || strings.HasPrefix(name, "libsdl3") || name == "user-rom.sfc" {
				return OutputReview{}, errors.New("unrecognized folder contains files with game-output names; choose a dedicated game folder to avoid replacing unrelated files")
			}
		}
		for _, name := range []string{"actraiser-builder", "actraiser-builder.exe"} {
			if _, err := os.Lstat(filepath.Join(physical, "tools", name)); err == nil {
				return OutputReview{}, errors.New("unrecognized folder already contains a runtime helper; choose a dedicated game folder")
			}
		}
	}
	return result, nil
}

func (s *OutputSelection) readPreference() (string, bool, error) {
	path := filepath.Join(s.workspace, outputPreferenceName)
	info, err := os.Lstat(path)
	if err != nil {
		return "", false, err
	}
	if !info.Mode().IsRegular() || info.Size() > 16384 {
		return "", false, errors.New("invalid game-folder preference")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", false, err
	}
	var preference outputPreference
	if err := json.Unmarshal(data, &preference); err != nil {
		return "", false, err
	}
	if preference.Schema != 1 {
		return "", false, errors.New("unsupported game-folder preference")
	}
	if preference.Relative {
		if !filepath.IsLocal(preference.Path) {
			return "", false, errors.New("invalid relative game-folder preference")
		}
		return filepath.Join(filepath.Dir(s.artifact), preference.Path), preference.Create, nil
	}
	if !filepath.IsAbs(preference.Path) {
		return "", false, errors.New("invalid absolute game-folder preference")
	}
	return preference.Path, preference.Create, nil
}

func (s *OutputSelection) savePreference(path string, create ...bool) error {
	pref := outputPreference{Schema: 1, Path: path}
	if len(create) > 0 {
		pref.Create = create[0]
	}
	base, err := buildworkspace.Physical(filepath.Dir(s.artifact))
	if err != nil {
		return err
	}
	if rel, err := filepath.Rel(base, path); err == nil && filepath.IsLocal(rel) {
		pref.Path, pref.Relative = rel, true
	}
	data, err := json.MarshalIndent(pref, "", "  ")
	if err != nil {
		return err
	}
	final := filepath.Join(s.workspace, outputPreferenceName)
	if info, err := os.Lstat(final); err == nil && !info.Mode().IsRegular() {
		return errors.New("game-folder preference must be a regular file")
	} else if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	temporary, err := os.CreateTemp(s.workspace, ".game-output-")
	if err != nil {
		return err
	}
	defer os.Remove(temporary.Name())
	if _, err = temporary.Write(data); err == nil {
		err = temporary.Sync()
	}
	closeErr := temporary.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	return os.Rename(temporary.Name(), final)
}

func outputJSON(w http.ResponseWriter, code int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(value)
}

func (s *OutputSelection) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	if r.Method == "GET" && (r.URL.Path == "/__shell/output/feedback.js" || r.URL.Path == "/__shell/output/feedback.css") {
		name := strings.TrimPrefix(r.URL.Path, "/__shell/output/")
		data, _ := workshopui.Assets.ReadFile(name)
		w.Header().Set("Content-Type", "text/javascript; charset=utf-8")
		if strings.HasSuffix(name, ".css") {
			w.Header().Set("Content-Type", "text/css; charset=utf-8")
		}
		w.Write(data)
		return
	}
	if r.Method == "GET" && r.URL.Path == "/__shell/output/" {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		io.WriteString(w, outputHTML)
		return
	}
	if r.Method == "GET" && r.URL.Path == "/__shell/output/app.js" {
		w.Header().Set("Content-Type", "text/javascript")
		io.WriteString(w, outputJS)
		return
	}
	if r.Method == "GET" && r.URL.Path == "/__shell/output/state" {
		s.mu.Lock()
		defer s.mu.Unlock()
		outputJSON(w, 200, map[string]any{"candidate": s.candidate, "current": s.current, "next": s.next, "warning": s.warning})
		return
	}
	if r.Method != "POST" {
		http.NotFound(w, r)
		return
	}
	// The renderer is token-scoped loopback, but reject cross-origin form posts
	// as well. No native custom-scheme route is allowed to reach these writes.
	if origin := r.Header.Get("Origin"); origin != "" && origin != "http://"+r.Host {
		http.Error(w, "invalid origin", 403)
		return
	}
	fail := func(code int, err error) { outputJSON(w, code, map[string]string{"error": err.Error()}) }
	s.mu.Lock()
	defer s.mu.Unlock()
	if r.URL.Path == "/__shell/output/choose" {
		if s.choosing {
			fail(409, errors.New("a folder chooser is already open"))
			return
		}
		if s.chooser == nil {
			fail(400, errors.New("folder chooser unavailable; enter an absolute path"))
			return
		}
		s.choosing = true
		s.mu.Unlock()
		path, err := s.chooser()
		s.mu.Lock()
		s.choosing = false
		if err != nil {
			fail(400, err)
			return
		}
		outputJSON(w, 200, map[string]string{"directory": path})
		return
	}
	var q struct {
		Directory string `json:"directory"`
		Revision  string `json:"revision"`
		Confirm   bool   `json:"confirm"`
	}
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 16384))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&q); err != nil {
		fail(400, err)
		return
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		fail(400, errors.New("unexpected request data"))
		return
	}
	switch r.URL.Path {
	case "/__shell/output/review":
		s.pending = nil
		review, err := s.review(q.Directory)
		if err != nil {
			fail(400, err)
			return
		}
		s.pending = &review
		outputJSON(w, 200, review)
	case "/__shell/output/apply":
		if s.pending == nil || q.Revision != s.pending.Revision {
			fail(409, errors.New("review the destination again before continuing"))
			return
		}
		review, err := s.review(s.pending.Directory)
		if err != nil {
			fail(400, err)
			return
		}
		if review.Revision != s.pending.Revision {
			s.pending = nil
			fail(409, errors.New("the destination changed; review it again"))
			return
		}
		if review.Existing && !q.Confirm {
			fail(400, errors.New("confirm updating this non-empty game folder"))
			return
		}
		if err := s.savePreference(review.Directory, !review.Existing); err != nil {
			fail(400, err)
			return
		}
		restart := s.current != ""
		s.candidate, s.next, s.pending = review.Directory, review.Directory, nil
		if !restart {
			s.current = review.Directory
			s.selected <- review.Directory
		}
		outputJSON(w, 200, map[string]any{"restartRequired": restart, "directory": review.Directory})
	default:
		http.NotFound(w, r)
	}
}
