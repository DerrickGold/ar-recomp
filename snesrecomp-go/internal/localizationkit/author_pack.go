package localizationkit

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash"
	"hash/fnv"
	"io"
	"io/fs"
	"os"
	"strings"
	"sync"
)

// The editor/import boundary caps the aggregate data it retains as well as
// each individual file. This is not a per-frame or runtime text budget.
const MaxAuthorPackBytes = 256 << 20

// AuthorPack is a detached, immutable snapshot of declared pack files. Its
// workspace validates content, but font shaping/coverage and runtime layout
// readiness still require the game's font backend. Unreferenced files are not
// read or exported; Files is not a publication or whole-directory backup API.
type AuthorPack struct {
	manifest        *PackManifest
	workspace       *AuthorWorkspace
	fonts           map[string][]byte
	progressPresent bool
	revisionOnce    sync.Once
	revision        uint64
}

func (p *AuthorPack) Manifest() *PackManifest {
	copy := *p.manifest
	return &copy
}
func (p *AuthorPack) Workspace() *AuthorWorkspace {
	copy := *p.workspace
	return &copy
}

// MessageOperations resolves aliases for previews and independent edits using
// the same semantic representation consumed by publication. Results are copies.
func (p *AuthorPack) MessageOperations(id string) ([]AuthorOperation, error) {
	return resolvedAuthorOperations(p.workspace, id)
}

// Files produces owned bytes for the next save/export adapter. It preserves
// manifest/script/progress comments exactly and includes referenced local fonts
// only. A caller must not mistake this working snapshot for publication output:
// it may contain private notes or locally extracted retail material.
func (p *AuthorPack) Files() map[string][]byte {
	files := make(map[string][]byte, len(p.workspace.scripts)+len(p.fonts)+2)
	files["pack.ini"] = []byte(p.manifest.text)
	for _, script := range p.workspace.scripts {
		files[script.path] = []byte(script.text)
	}
	for path, data := range p.fonts {
		files[path] = append([]byte{}, data...)
	}
	if p.progressPresent || p.workspace.progress.text != "" {
		files["translation-progress.tsv"] = []byte(p.workspace.progress.text)
	}
	return files
}

func hashPackPart(h hash.Hash64, kind, name string, data []byte) {
	var length [8]byte
	for _, part := range [][]byte{[]byte(kind), []byte(name), data} {
		binary.BigEndian.PutUint64(length[:], uint64(len(part)))
		_, _ = h.Write(length[:])
		_, _ = h.Write(part)
	}
}

// RuntimeRevision matches the C loader byte-for-byte, including manifest order,
// source bytes/BOM/line endings, and ordered font references. Progress metadata
// is deliberately excluded. Hashing fonts occurs once per immutable snapshot,
// on demand, never while polling the message tree or rendering a frame.
func (p *AuthorPack) RuntimeRevision() uint64 {
	p.revisionOnce.Do(func() {
		h := fnv.New64a()
		hashPackPart(h, "manifest", "pack.ini", []byte(p.manifest.text))
		for _, script := range p.workspace.scripts {
			hashPackPart(h, "script", script.path, []byte(script.text))
		}
		for _, font := range append([]string{p.manifest.fonts.Primary}, p.manifest.fonts.Fallback...) {
			if strings.HasPrefix(font, "builtin:") {
				hashPackPart(h, "builtin-font", font, nil)
			} else {
				hashPackPart(h, "font", font, p.fonts[font])
			}
		}
		p.revision = h.Sum64()
	})
	return p.revision
}

func (p *AuthorPack) EditMessage(id, body string, status TranslationStatus) (*AuthorPack, error) {
	w, err := p.workspace.EditMessage(id, body, status)
	if err != nil {
		return nil, err
	}
	return p.withWorkspace(w)
}

func (p *AuthorPack) AddMessage(id, path, body string, status TranslationStatus) (*AuthorPack, error) {
	w, err := p.workspace.AddMessage(id, path, body, status)
	if err != nil {
		return nil, err
	}
	return p.withWorkspace(w)
}

func (p *AuthorPack) withWorkspace(w *AuthorWorkspace) (*AuthorPack, error) {
	next := &AuthorPack{manifest: p.manifest, workspace: w, fonts: p.fonts, progressPresent: p.progressPresent}
	if next.byteSize() > MaxAuthorPackBytes {
		return nil, fmt.Errorf("pack exceeds aggregate size limit")
	}
	return next, nil
}

func (p *AuthorPack) WithMetadata(metadata PackMetadata) (*AuthorPack, error) {
	m, err := p.manifest.WithMetadata(metadata)
	if err != nil {
		return nil, err
	}
	w, err := newAuthorWorkspace(metadata.SourceProfile, metadata.Coverage, p.workspace.scripts, p.workspace.progress.text)
	if err != nil {
		return nil, err
	}
	next := &AuthorPack{manifest: m, workspace: w, fonts: p.fonts, progressPresent: p.progressPresent}
	if next.byteSize() > MaxAuthorPackBytes {
		return nil, fmt.Errorf("pack exceeds aggregate size limit")
	}
	return next, nil
}

func (p *AuthorPack) byteSize() int {
	size := len(p.manifest.text) + len(p.workspace.progress.text)
	for _, s := range p.workspace.scripts {
		size += len(s.text)
	}
	for _, font := range p.fonts {
		size += len(font)
	}
	return size
}

// LoadAuthorPack is the shared VFS boundary for directory and future archive
// adapters. The filesystem must be confined to its pack root; this function
// never joins caller paths with the process CWD. Use OpenAuthorPack for disk.
func LoadAuthorPack(fsys fs.FS) (*AuthorPack, error) {
	return loadAuthorPack(fsys, MaxAuthorPackBytes)
}

func loadAuthorPack(fsys fs.FS, maximum int) (*AuthorPack, error) {
	if fsys == nil {
		return nil, fmt.Errorf("pack filesystem is required")
	}
	remaining := maximum
	read := func(path string, maximum int) ([]byte, error) {
		if maximum > remaining {
			maximum = remaining
		}
		data, err := readPackFile(fsys, path, maximum)
		if err != nil {
			return nil, err
		}
		remaining -= len(data)
		return data, nil
	}
	raw, err := read("pack.ini", MaxPackManifestBytes)
	if err != nil {
		return nil, err
	}
	m, err := ParsePackManifest(string(raw), "pack.ini")
	if err != nil {
		return nil, err
	}
	if err := validateAuthorPackPaths(m); err != nil {
		return nil, err
	}
	fontReferences := append([]string{m.fonts.Primary}, m.fonts.Fallback...)
	scripts := make([]*AuthorScript, 0, len(m.sources))
	for _, path := range m.sources {
		raw, err := read(path, MaxAuthorScriptBytes)
		if err != nil {
			return nil, err
		}
		script, err := ParseAuthorScript(string(raw), path)
		if err != nil {
			return nil, err
		}
		scripts = append(scripts, script)
	}
	progress, present := "", false
	if raw, err := read("translation-progress.tsv", MaxAuthorScriptBytes); err == nil {
		progress, present = string(raw), true
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	w, err := newAuthorWorkspace(m.metadata.SourceProfile, m.metadata.Coverage, scripts, progress)
	if err != nil {
		return nil, err
	}
	fonts := make(map[string][]byte)
	for _, path := range fontReferences {
		if strings.HasPrefix(path, "builtin:") {
			continue
		}
		if _, ok := fonts[path]; ok {
			continue
		}
		data, err := read(path, MaxPackFontBytes)
		if err != nil {
			return nil, err
		}
		if len(data) == 0 {
			return nil, fmt.Errorf("%s: font is empty", path)
		}
		fonts[path] = data
	}
	return &AuthorPack{manifest: m, workspace: w, fonts: fonts, progressPresent: present}, nil
}

func validateAuthorPackPaths(m *PackManifest) error {
	// A portable snapshot cannot contain conflicting file/directory roles or
	// case aliases. Keep these checks independent of the host filesystem.
	type fileRole struct{ path, kind string }
	roles := []fileRole{{"pack.ini", "manifest"}, {"translation-progress.tsv", "progress"},
		{"package.json", "archive metadata"}, {"author-project.json", "author metadata"}, {"notices", "notice directory"}}
	addRole := func(path, role string) error {
		for _, entry := range roles {
			existing, kind := entry.path, entry.kind
			if existing == path {
				if role == "font" && kind == "font" {
					return nil
				}
				return fmt.Errorf("%s is used as both %s and %s", path, kind, role)
			}
			if strings.EqualFold(existing, path) || strings.HasPrefix(strings.ToLower(path), strings.ToLower(existing)+"/") || strings.HasPrefix(strings.ToLower(existing), strings.ToLower(path)+"/") {
				return fmt.Errorf("conflicting pack paths %q and %q", existing, path)
			}
		}
		roles = append(roles, fileRole{path, role})
		return nil
	}
	for _, path := range m.sources {
		if err := addRole(path, "script"); err != nil {
			return err
		}
	}
	for _, path := range append([]string{m.fonts.Primary}, m.fonts.Fallback...) {
		if !strings.HasPrefix(path, "builtin:") {
			if err := addRole(path, "font"); err != nil {
				return err
			}
		}
	}
	return nil
}

func readPackFile(fsys fs.FS, path string, maximum int) (data []byte, err error) {
	if !PortablePackPath(path) {
		return nil, fmt.Errorf("unsafe pack path %q", path)
	}
	f, err := fsys.Open(path)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	defer func() {
		if closeErr := f.Close(); err == nil && closeErr != nil {
			data = nil
			err = fmt.Errorf("%s: %w", path, closeErr)
		}
	}()
	info, err := f.Stat()
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if !info.Mode().IsRegular() || info.Size() < 0 || info.Size() > int64(maximum) {
		return nil, fmt.Errorf("%s: not a regular file or exceeds size limit", path)
	}
	data, err = io.ReadAll(io.LimitReader(f, int64(maximum)+1))
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	if len(data) > maximum || int64(len(data)) != info.Size() {
		return nil, fmt.Errorf("%s: file changed size or exceeds size limit", path)
	}
	return data, nil
}

// OpenAuthorPack opens a detached snapshot of an explicitly selected directory.
// Go's rooted file handles enforce containment even if a directory is renamed
// during the read. Referenced symlinks/special files are rejected; no arbitrary
// recursive directory enumeration, subprocesses or writes are performed.
func OpenAuthorPack(directory string) (*AuthorPack, error) {
	root, err := os.OpenRoot(directory)
	if err != nil {
		return nil, fmt.Errorf("open pack directory: %w", err)
	}
	defer root.Close()
	return LoadAuthorPack(rootedPackFS{root})
}

type rootedPackFS struct{ root *os.Root }

func (r rootedPackFS) Open(name string) (fs.File, error) {
	if !PortablePackPath(name) {
		return nil, fmt.Errorf("unsafe pack path %q", name)
	}
	parts := strings.Split(name, "/")
	for i := range parts {
		info, err := r.root.Lstat(strings.Join(parts[:i+1], "/"))
		if err != nil {
			return nil, err
		}
		if i < len(parts)-1 {
			if !info.IsDir() {
				return nil, fmt.Errorf("pack path component is not a directory: %s", name)
			}
		} else if !info.Mode().IsRegular() {
			return nil, fmt.Errorf("pack member is not a regular file: %s", name)
		}
	}
	return r.root.Open(name)
}
