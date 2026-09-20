package localization

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
)

var ErrProjectConflict = errors.New("project changed or already exists; reopen or explicitly choose replacement")

// AuthorStore owns only .arproject archives below an explicitly selected root.
// A save atomically replaces one file; it never rewrites an imported directory.
type AuthorStore struct {
	root string
	// Revision of the archive this store last read or wrote, with the file
	// identity it had at the time. A save can then check for a conflict
	// without reparsing a large pack, and any change on disk -- size, time,
	// mode, or a different file -- misses and falls back to a full read.
	mu    sync.Mutex
	known map[string]storedRevision
}

type storedRevision struct {
	info     os.FileInfo
	revision string
}

// The identity must belong to the file that supplied this revision, not a
// fresh lookup of path: another store may have atomically replaced it while
// Open was parsing the archive. A stale identity safely misses on the next save.
func (s *AuthorStore) rememberRevision(path string, info os.FileInfo, revision string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.known == nil {
		s.known = map[string]storedRevision{}
	}
	s.known[path] = storedRevision{info, revision}
}

func (s *AuthorStore) rememberedRevision(path string, info os.FileInfo) (string, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	known, ok := s.known[path]
	if !ok || !os.SameFile(info, known.info) || info.Size() != known.info.Size() ||
		!info.ModTime().Equal(known.info.ModTime()) || info.Mode() != known.info.Mode() {
		return "", false
	}
	return known.revision, true
}

func NewAuthorStore(directory string) (*AuthorStore, error) {
	root, err := filepath.Abs(directory)
	if err != nil {
		return nil, err
	}
	if err := makeAuthorDirectory(root); err != nil {
		return nil, err
	}
	return &AuthorStore{root: root}, nil
}
func (s *AuthorStore) Root() string { return s.root }
func validProjectID(id string) bool {
	return len(id) <= 96 && authorIdentifier(id) && PortablePackPath(id) && !strings.Contains(id, "/")
}
func (s *AuthorStore) path(id string) (string, error) {
	if !validProjectID(id) {
		return "", fmt.Errorf("invalid project ID")
	}
	return filepath.Join(s.root, id+".arproject"), nil
}
func (s *AuthorStore) Open(id string) (*AuthorProject, error) {
	path, err := s.path(id)
	if err != nil {
		return nil, err
	}
	file, err := openRegularAuthorFile(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, err
	}
	p, err := ReadAuthorArchive(file, info.Size())
	if err != nil {
		return nil, err
	}
	if p.pack.manifest.metadata.ID != id {
		return nil, fmt.Errorf("project filename/identity mismatch")
	}
	s.rememberRevision(path, info, p.ProjectRevision())
	return p, nil
}
func (s *AuthorStore) Save(p *AuthorProject, expected string) error {
	id := p.pack.manifest.metadata.ID
	path, err := s.path(id)
	if err != nil {
		return err
	}
	unlock, err := authorLock(s.root)
	if err != nil {
		return err
	}
	defer unlock()
	entries, err := os.ReadDir(s.root)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if strings.EqualFold(entry.Name(), id+".arproject") && entry.Name() != id+".arproject" {
			return ErrProjectConflict
		}
	}
	info, err := os.Lstat(path)
	if err == nil {
		revision, known := s.rememberedRevision(path, info)
		if !known {
			current, readErr := s.Open(id)
			if readErr != nil {
				return readErr
			}
			revision = current.ProjectRevision()
		}
		if expected == "" || revision != expected {
			return ErrProjectConflict
		}
	} else if !os.IsNotExist(err) {
		return err
	} else if expected != "" {
		return ErrProjectConflict
	}
	if err := atomicAuthorFile(path, func(w io.Writer) error { return p.WriteArchive(w, "backup") }); err != nil {
		return err
	}
	// Unlike Open, Save still holds the cross-process author lock here, so no
	// cooperating writer can replace the file between publication and this stat.
	if savedInfo, statErr := os.Lstat(path); statErr == nil {
		s.rememberRevision(path, savedInfo, p.ProjectRevision())
	}
	return nil
}

type AuthorProjectSummary struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Locale  string `json:"locale"`
	Autonym string `json:"autonym"`
	Error   string `json:"error,omitempty"`
	Origin  string `json:"origin"`
}

func (s *AuthorStore) List() ([]AuthorProjectSummary, error) {
	entries, err := os.ReadDir(s.root)
	if err != nil {
		return nil, err
	}
	if len(entries) > 1024 {
		return nil, fmt.Errorf("too many files in author store")
	}
	result := []AuthorProjectSummary{}
	for _, entry := range entries {
		if !strings.HasSuffix(entry.Name(), ".arproject") {
			continue
		}
		id := strings.TrimSuffix(entry.Name(), ".arproject")
		row := AuthorProjectSummary{ID: id, Name: id}
		if !validProjectID(id) || entry.Type()&os.ModeSymlink != 0 {
			row.Error = "unsafe project file"
			result = append(result, row)
			continue
		}
		func() {
			f, err := openRegularAuthorFile(filepath.Join(s.root, entry.Name()))
			if err != nil {
				row.Error = err.Error()
				return
			}
			defer f.Close()
			info, err := f.Stat()
			if err != nil || info.Size() > MaxAuthorArchiveBytes {
				row.Error = "invalid project size"
				return
			}
			z, err := zip.NewReader(f, info.Size())
			if err != nil || len(z.File) > maxArchiveFiles {
				row.Error = "invalid project archive"
				return
			}
			var manifest *PackManifest
			for _, file := range z.File {
				if file.Name == "author-project.json" {
					if file.UncompressedSize64 > 2<<20 {
						row.Error = "invalid author metadata size"
						return
					}
					r, err := file.Open()
					if err != nil {
						row.Error = err.Error()
						return
					}
					data, err := io.ReadAll(io.LimitReader(r, (2<<20)+1))
					r.Close()
					var info projectInfo
					if err != nil || len(data) > 2<<20 || json.Unmarshal(data, &info) != nil {
						row.Error = "invalid author metadata"
						return
					}
					row.Origin = info.Origin
					continue
				}
				if file.Name != "pack.ini" {
					continue
				}
				if manifest != nil || file.UncompressedSize64 > MaxPackManifestBytes {
					row.Error = "invalid project manifest"
					return
				}
				r, err := file.Open()
				if err != nil {
					row.Error = err.Error()
					return
				}
				data, err := io.ReadAll(io.LimitReader(r, MaxPackManifestBytes+1))
				r.Close()
				if err != nil {
					row.Error = err.Error()
					return
				}
				manifest, err = ParsePackManifest(string(data), "pack.ini")
				if err != nil {
					row.Error = err.Error()
					return
				}
			}
			if manifest == nil || manifest.metadata.ID != id {
				row.Error = "missing or mismatched manifest"
				return
			}
			m := manifest.metadata
			row.Name, row.Locale, row.Autonym = m.Name, m.Locale, m.Autonym
		}()
		result = append(result, row)
	}
	slices.SortFunc(result, func(a, b AuthorProjectSummary) int {
		for _, pair := range [][2]string{{a.Name, b.Name}, {a.ID, b.ID}} {
			if c := strings.Compare(strings.ToLower(pair[0]), strings.ToLower(pair[1])); c != 0 {
				return c
			}
		}
		return strings.Compare(a.ID, b.ID)
	})
	return result, nil
}

func openRegularAuthorFile(path string) (*os.File, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("not a regular author file: %s", path)
	}
	return os.Open(path)
}

// Reject static symlinks in managed subdirectories. The selected existing
// parent itself may be an OS alias (e.g. /var); resolve that root explicitly.
func makeAuthorDirectory(path string) error {
	info, err := os.Lstat(path)
	if err == nil {
		if !info.IsDir() {
			return fmt.Errorf("author path is not a directory: %s", path)
		}
		return nil
	}
	if !os.IsNotExist(err) {
		return err
	}
	parent := filepath.Dir(path)
	if parent == path {
		return err
	}
	if err := makeAuthorDirectory(parent); err != nil {
		return err
	}
	return os.Mkdir(path, 0755)
}
func authorLock(root string) (func(), error) {
	if err := makeAuthorDirectory(root); err != nil {
		return nil, err
	}
	path := filepath.Join(root, ".author-write.lock")
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return nil, fmt.Errorf("another author save/install is active (or a stale .author-write.lock needs review): %w", err)
	}
	f.Close()
	return func() { _ = os.Remove(path) }, nil
}
func atomicAuthorFile(path string, write func(io.Writer) error) (err error) {
	parent := filepath.Dir(path)
	info, err := os.Stat(parent)
	if err != nil {
		return err
	}
	file, err := os.CreateTemp(parent, ".author-stage-")
	if err != nil {
		return err
	}
	temporary := file.Name()
	defer os.Remove(temporary)
	if err = write(file); err == nil {
		err = file.Sync()
	}
	closeErr := file.Close()
	if err != nil {
		return err
	}
	if closeErr != nil {
		return closeErr
	}
	current, err := os.Stat(parent)
	if err != nil || !os.SameFile(info, current) {
		return fmt.Errorf("author directory moved during save")
	}
	if existing, err := os.Lstat(path); err == nil && !existing.Mode().IsRegular() {
		return fmt.Errorf("destination is not a regular file")
	} else if err != nil && !os.IsNotExist(err) {
		return err
	}
	return os.Rename(temporary, path)
}

// InstallAuthorProject uses immutable version directories and an atomic manifest
// replacement as its commit point. Readers see either complete version. Prior
// versions remain available for recovery; install never changes game selection.
func InstallAuthorProject(directory string, project *AuthorProject, replace bool) (string, error) {
	return installAuthorProject(directory, project, replace, 0)
}

// A pack opened from an installed layout carries the previous install's
// version directory on every path it named. This installer adds its own, so
// that one has to come off first: otherwise each update buries the source a
// level deeper and the manifest path grows without bound. Only a prefix this
// installer could have written is removed -- a single leading "v-" segment
// shared by every source and font the manifest names, each of which must be
// present in the pack under that prefix. Anything else is left alone, since it
// is the author's own directory rather than a storage detail.
// The directory an install stages a version into: "v-" and digits, nothing
// else, matching what os.MkdirTemp produces below. Shared with the archiver,
// which reads notices out of one -- two spellings of "is this ours" would let
// a pack be stripped here and not recognised there.
func installedVersionSegment(path string) (string, bool) {
	prefix, _, found := strings.Cut(path, "/")
	if !found || !strings.HasPrefix(prefix, "v-") || len(prefix) <= 2 ||
		strings.Trim(prefix[2:], "0123456789") != "" {
		return "", false
	}
	return prefix, true
}

func stripInstalledVersion(files map[string][]byte, sources []string, fonts *PackFonts) string {
	if len(sources) == 0 {
		return ""
	}
	prefix := ""
	named := append([]string{}, sources...)
	for _, path := range fonts.References() {
		if !strings.HasPrefix(path, "builtin:") {
			named = append(named, path)
		}
	}
	for _, path := range named {
		segment, ours := installedVersionSegment(path)
		if !ours {
			return ""
		}
		if prefix == "" {
			prefix = segment + "/"
		} else if prefix != segment+"/" {
			return ""
		}
		if _, present := files[path]; !present {
			return ""
		}
	}
	for i, source := range sources {
		sources[i] = strings.TrimPrefix(source, prefix)
	}
	fonts.mapPaths(func(path string) string { return strings.TrimPrefix(path, prefix) })
	for _, path := range named {
		data := files[path]
		delete(files, path)
		files[strings.TrimPrefix(path, prefix)] = data
	}
	return prefix
}

func installAuthorProject(directory string, project *AuthorProject, replace bool, expectedRevision uint64) (string, error) {
	if project == nil || (!project.publicationReady && !project.installationReady && !project.nativeReady) {
		return "", fmt.Errorf("prepare installation or publication before installing")
	}
	pack := project.pack
	if pack == nil || pack.manifest.metadata.Target != "us-runtime" {
		return "", fmt.Errorf("only validated US runtime packs can be installed")
	}
	root, err := filepath.Abs(directory)
	if err != nil {
		return "", err
	}
	if err = makeAuthorDirectory(root); err != nil {
		return "", err
	}
	unlock, err := authorLock(root)
	if err != nil {
		return "", err
	}
	defer unlock()
	return writeInstalledProject(root, project, replace, expectedRevision)
}

// The caller holds the directory's author lock through validation and manifest
// publication, including any checks against a previously inspected install.
func writeInstalledProject(root string, project *AuthorProject, replace bool, expectedRevision uint64) (string, error) {
	pack := project.pack
	manifestPath := filepath.Join(root, "pack.ini")
	if name, _, err := installedManifest(root); err == nil {
		// Updating a disabled pack preserves the user's availability choice.
		manifestPath = filepath.Join(root, name)
		old, readErr := openInstalledAuthorPack(root, name)
		if readErr != nil {
			return "", readErr
		}
		if !replace || old.manifest.metadata.ID != pack.manifest.metadata.ID {
			return "", ErrProjectConflict
		}
		if expectedRevision != 0 && old.RuntimeRevision() != expectedRevision {
			return "", ErrProjectConflict
		}
	} else if !os.IsNotExist(err) {
		return "", err
	}
	version, err := os.MkdirTemp(root, "v-")
	if err != nil {
		return "", err
	}
	committed := false
	defer func() {
		if !committed {
			_ = os.RemoveAll(version)
		}
	}()
	files := pack.Files()
	for path, data := range project.notices {
		files[path] = data
	}
	delete(files, "translation-progress.tsv")
	fonts := pack.manifest.Fonts()
	sources := pack.manifest.Sources()
	if stripInstalledVersion(files, sources, &fonts) != "" {
		// The copy staged inside the version directory has to name its members
		// the way they now sit beside it, or opening it below fails.
		relative, err := NewPackManifestVersion(pack.manifest.Metadata(), fonts, sources, pack.manifest.Version())
		if err != nil {
			return "", err
		}
		files["pack.ini"] = []byte(relative.Text())
	}
	for path, data := range files {
		if !PortablePackPath(path) {
			return "", fmt.Errorf("unsafe pack member")
		}
		destination := filepath.Join(version, filepath.FromSlash(path))
		if err := makeAuthorDirectory(filepath.Dir(destination)); err != nil {
			return "", err
		}
		if err := atomicAuthorFile(destination, func(w io.Writer) error { _, err := w.Write(data); return err }); err != nil {
			return "", err
		}
	}
	if _, err := OpenAuthorPack(version); err != nil {
		return "", err
	}
	prefix := filepath.Base(version) + "/"
	fonts.mapPaths(func(path string) string { return prefix + path })
	for i := range sources {
		sources[i] = prefix + sources[i]
	}
	manifest, err := NewPackManifestVersion(pack.manifest.Metadata(), fonts, sources, pack.manifest.Version())
	if err != nil {
		return "", err
	}
	// Validate against the staged filesystem before publishing the manifest.
	validation := map[string][]byte{"pack.ini": []byte(manifest.Text())}
	for path, data := range files {
		validation[prefix+path] = data
	}
	if _, err := LoadAuthorPack(projectMapFS(validation)); err != nil {
		return "", err
	}
	if err := atomicAuthorFile(manifestPath, func(w io.Writer) error { _, err := io.Copy(w, bytes.NewBufferString(manifest.Text())); return err }); err != nil {
		return "", err
	}
	committed = true
	return manifestPath, nil
}
