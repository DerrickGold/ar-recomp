package localizationkit

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
)

var ErrProjectConflict = errors.New("project changed or already exists; reopen or explicitly choose replacement")

// AuthorStore owns only .arproject archives below an explicitly selected root.
// A save atomically replaces one file; it never rewrites an imported directory.
type AuthorStore struct{ root string }

func NewAuthorStore(directory string) (*AuthorStore, error) {
	root, err := filepath.Abs(directory)
	if err != nil {
		return nil, err
	}
	if err := makeAuthorDirectory(root); err != nil {
		return nil, err
	}
	return &AuthorStore{root}, nil
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
	_, err = os.Lstat(path)
	if err == nil {
		current, readErr := s.Open(id)
		if readErr != nil {
			return readErr
		}
		if expected == "" || current.ProjectRevision() != expected {
			return ErrProjectConflict
		}
	} else if !os.IsNotExist(err) {
		return err
	} else if expected != "" {
		return ErrProjectConflict
	}
	return atomicAuthorFile(path, func(w io.Writer) error { return p.WriteArchive(w, "backup") })
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
	fonts := pack.manifest.Fonts()
	if !strings.HasPrefix(fonts.Primary, "builtin:") {
		fonts.Primary = prefix + fonts.Primary
	}
	for i, path := range fonts.Fallback {
		if !strings.HasPrefix(path, "builtin:") {
			fonts.Fallback[i] = prefix + path
		}
	}
	sources := pack.manifest.Sources()
	for i := range sources {
		sources[i] = prefix + sources[i]
	}
	manifest, err := NewPackManifest(pack.manifest.Metadata(), fonts, sources)
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
