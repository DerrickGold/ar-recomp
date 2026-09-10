package localizationkit

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"slices"
	"strings"
	"time"
	"unicode/utf8"
)

const MaxAuthorArchiveBytes = MaxAuthorPackBytes + (1 << 20)
const maxArchiveFiles = 256

type archiveHeader struct {
	Format  string `json:"format"`
	Version int    `json:"version"`
	Kind    string `json:"kind"`
}

func strictAuthorJSON(data []byte, out any) error {
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(out); err != nil {
		return err
	}
	var trailing any
	if d.Decode(&trailing) != io.EOF {
		return fmt.Errorf("unexpected trailing JSON")
	}
	return nil
}

// WriteArchive writes a portable ZIP container, never uploads or installs it.
// Publication projects must already have passed Publication's explicit review.
func (p *AuthorProject) WriteArchive(output io.Writer, kind string) error {
	if kind != "backup" && kind != "publication" {
		return fmt.Errorf("archive kind must be backup or publication")
	}
	if kind == "publication" && !p.publicationReady {
		return fmt.Errorf("prepare publication before exporting")
	}
	files := p.projectFiles(kind == "backup")
	header, _ := json.Marshal(archiveHeader{"actraiser-language-archive", 1, kind})
	files["package.json"] = header
	if err := validateProjectFiles(files); err != nil {
		return err
	}
	paths := make([]string, 0, len(files))
	for path := range files {
		paths = append(paths, path)
	}
	slices.Sort(paths)
	w := zip.NewWriter(output)
	for _, path := range paths {
		h := zip.FileHeader{Name: path, Method: zip.Deflate}
		h.SetMode(0644)
		h.SetModTime(time.Date(1980, 1, 1, 0, 0, 0, 0, time.UTC))
		member, err := w.CreateHeader(&h)
		if err != nil {
			return err
		}
		if _, err = member.Write(files[path]); err != nil {
			return err
		}
	}
	return w.Close()
}

// ReadAuthorArchive validates all members before exposing any project. No ZIP
// entry is extracted to disk, and executable/unreferenced payloads are rejected.
func ReadAuthorArchive(input io.ReaderAt, size int64) (*AuthorProject, error) {
	if size <= 0 || size > MaxAuthorArchiveBytes {
		return nil, fmt.Errorf("archive exceeds size limit")
	}
	z, err := zip.NewReader(input, size)
	if err != nil {
		return nil, fmt.Errorf("invalid language ZIP: %w", err)
	}
	if len(z.File) == 0 || len(z.File) > maxArchiveFiles {
		return nil, fmt.Errorf("invalid archive member count")
	}
	files := map[string][]byte{}
	remaining := uint64(MaxAuthorPackBytes)
	for _, member := range z.File {
		if !PortablePackPath(member.Name) || !member.Mode().IsRegular() || member.Mode()&0111 != 0 || member.Flags&1 != 0 {
			return nil, fmt.Errorf("unsafe archive member %q", member.Name)
		}
		if _, ok := files[member.Name]; ok {
			return nil, fmt.Errorf("duplicate archive member %s", member.Name)
		}
		if member.UncompressedSize64 > remaining {
			return nil, fmt.Errorf("archive expansion exceeds size limit")
		}
		limit := uint64(MaxAuthorScriptBytes)
		switch {
		case member.Name == "pack.ini":
			limit = MaxPackManifestBytes
		case member.Name == "package.json":
			limit = 4096
		case member.Name == "author-project.json":
			limit = 2 << 20
		case strings.HasPrefix(member.Name, "notices/"):
			limit = 1 << 20
		case strings.HasSuffix(strings.ToLower(member.Name), ".ttf") || strings.HasSuffix(strings.ToLower(member.Name), ".otf"):
			limit = MaxPackFontBytes
		}
		if member.UncompressedSize64 > limit {
			return nil, fmt.Errorf("archive member exceeds size limit: %s", member.Name)
		}
		reader, err := member.Open()
		if err != nil {
			return nil, err
		}
		data, err := io.ReadAll(io.LimitReader(reader, int64(member.UncompressedSize64)+1))
		closeErr := reader.Close()
		if err != nil {
			return nil, err
		}
		if closeErr != nil {
			return nil, closeErr
		}
		if uint64(len(data)) != member.UncompressedSize64 {
			return nil, fmt.Errorf("archive member size mismatch")
		}
		remaining -= uint64(len(data))
		files[member.Name] = data
	}
	if err := validateProjectFiles(files); err != nil {
		return nil, err
	}
	var header archiveHeader
	if err := strictAuthorJSON(files["package.json"], &header); err != nil {
		return nil, fmt.Errorf("invalid package.json: %w", err)
	}
	if header.Format != "actraiser-language-archive" || header.Version != 1 || (header.Kind != "backup" && header.Kind != "publication") {
		return nil, fmt.Errorf("unsupported language archive schema")
	}
	pack, err := LoadAuthorPack(projectMapFS(files))
	if err != nil {
		return nil, err
	}
	project := &AuthorProject{pack: pack, info: projectInfo{Version: 1, Origin: "publication"}, notices: map[string][]byte{}}
	allowed := map[string]bool{"package.json": true}
	for path := range pack.Files() {
		allowed[path] = true
	}
	if header.Kind == "backup" {
		project.info, err = decodeProjectInfo(files["author-project.json"], pack)
		if err != nil {
			return nil, err
		}
		allowed["author-project.json"] = true
	} else {
		if _, ok := files["translation-progress.tsv"]; ok {
			return nil, fmt.Errorf("publication must not contain private progress")
		}
		if pack.manifest.metadata.Target != "us-runtime" {
			return nil, fmt.Errorf("publication requires US runtime contracts")
		}
		var progress strings.Builder
		for _, script := range pack.workspace.scripts {
			for _, message := range script.messages {
				fmt.Fprintf(&progress, "%s\tdone\n", message.ID)
			}
		}
		w, err := newAuthorWorkspace("us", pack.manifest.metadata.Coverage, pack.workspace.scripts, progress.String())
		if err != nil {
			return nil, err
		}
		project.pack, err = pack.withWorkspace(w)
		if err != nil {
			return nil, err
		}
	}
	for path, data := range files {
		if strings.HasPrefix(path, "notices/") {
			name := strings.TrimPrefix(path, "notices/")
			next, err := project.WithNotice(name, string(data))
			if err != nil {
				return nil, err
			}
			project = next
			allowed[path] = true
		}
		if !allowed[path] {
			return nil, fmt.Errorf("unexpected archive payload %s", path)
		}
	}
	for path, data := range pack.fonts {
		if !(strings.HasSuffix(strings.ToLower(path), ".ttf") || strings.HasSuffix(strings.ToLower(path), ".otf")) || len(data) < 4 || !(bytes.Equal(data[:4], []byte{0, 1, 0, 0}) || string(data[:4]) == "OTTO") {
			return nil, fmt.Errorf("unsupported font payload %s (use TTF/OTF)", path)
		}
	}
	if header.Kind == "publication" && len(pack.fonts) > 0 && len(project.notices) == 0 {
		return nil, fmt.Errorf("font license notices are missing")
	}
	// Archive metadata is not an export authorization. Re-review after import.
	if header.Kind == "publication" {
		project.info.Origin = "imported"
	}
	return project, nil
}

func validateProjectFiles(files map[string][]byte) error {
	if len(files) == 0 || len(files) > maxArchiveFiles {
		return fmt.Errorf("too many project files")
	}
	paths := make([]string, 0, len(files))
	total := 0
	for path, data := range files {
		if !PortablePackPath(path) {
			return fmt.Errorf("unsafe project path %q", path)
		}
		for _, other := range paths {
			if strings.EqualFold(path, other) || strings.HasPrefix(strings.ToLower(path), strings.ToLower(other)+"/") || strings.HasPrefix(strings.ToLower(other), strings.ToLower(path)+"/") {
				return fmt.Errorf("conflicting project paths")
			}
		}
		paths = append(paths, path)
		if len(data) > MaxAuthorPackBytes-total {
			return fmt.Errorf("project exceeds aggregate size limit")
		}
		total += len(data)
	}
	return nil
}

func decodeProjectInfo(data []byte, pack *AuthorPack) (projectInfo, error) {
	var info projectInfo
	if len(data) > 2<<20 {
		return info, fmt.Errorf("author metadata exceeds size limit")
	}
	if err := strictAuthorJSON(data, &info); err != nil {
		return projectInfo{}, fmt.Errorf("invalid author metadata: %w", err)
	}
	if info.Version != 1 || !slices.Contains([]string{"native-source", "translation", "publication", "imported"}, info.Origin) {
		return projectInfo{}, fmt.Errorf("unsupported author project")
	}
	if len(info.Notes) > 1<<20 || !utf8.ValidString(info.Notes) || strings.ContainsRune(info.Notes, 0) {
		return projectInfo{}, fmt.Errorf("invalid author notes")
	}
	for id, digest := range info.Baseline {
		if _, ok := pack.workspace.references[id]; !ok || len(digest) != 64 || strings.Trim(digest, "0123456789abcdef") != "" {
			return projectInfo{}, fmt.Errorf("invalid source fingerprint")
		}
	}
	return info, nil
}

// A small immutable VFS for detached files; neither the GUI nor archive reader
// needs to materialize untrusted paths to validate a pack.
func projectMapFS(files map[string][]byte) fs.FS {
	return projectFS(files)
}

type projectFS map[string][]byte

func (f projectFS) Open(name string) (fs.File, error) {
	data, ok := f[name]
	if !ok {
		return nil, &fs.PathError{Op: "open", Path: name, Err: fs.ErrNotExist}
	}
	return &projectFile{Reader: *bytes.NewReader(data), name: name, size: int64(len(data))}, nil
}

type projectFile struct {
	bytes.Reader
	name string
	size int64
}

func (f *projectFile) Close() error               { return nil }
func (f *projectFile) Stat() (fs.FileInfo, error) { return projectFileInfo{f.name, f.size}, nil }

type projectFileInfo struct {
	name string
	size int64
}

func (i projectFileInfo) Name() string       { return i.name }
func (i projectFileInfo) Size() int64        { return i.size }
func (i projectFileInfo) Mode() fs.FileMode  { return 0644 }
func (i projectFileInfo) ModTime() time.Time { return time.Time{} }
func (i projectFileInfo) IsDir() bool        { return false }
func (i projectFileInfo) Sys() any           { return nil }

// OpenAuthorProjectDirectory imports an explicitly selected author directory.
// Unknown files are not silently backed up: users must name notices explicitly.
func OpenAuthorProjectDirectory(path string) (*AuthorProject, error) {
	pack, err := OpenAuthorPack(path)
	if err != nil {
		return nil, err
	}
	p := &AuthorProject{pack: pack, info: projectInfo{Version: 1, Origin: "imported"}, notices: map[string][]byte{}}
	root, err := os.OpenRoot(path)
	if err != nil {
		return nil, err
	}
	defer root.Close()
	noticeDir := "notices"
	// Our versioned installer keeps notices with the immutable scripts/fonts.
	// Read that one known version directory, never enumerate historical versions.
	if prefix, _, ok := strings.Cut(pack.manifest.sources[0], "/"); ok && strings.HasPrefix(prefix, "v-") && len(prefix) > 2 && strings.Trim(prefix[2:], "0123456789") == "" {
		shared := true
		for _, source := range pack.manifest.sources {
			shared = shared && strings.HasPrefix(source, prefix+"/")
		}
		if shared {
			noticeDir = prefix + "/notices"
		}
	}
	info, err := root.Lstat(noticeDir)
	if err == nil {
		if !info.IsDir() {
			return nil, fmt.Errorf("notices must be a regular directory")
		}
		entries, err := fs.ReadDir(root.FS(), noticeDir)
		if err != nil {
			return nil, err
		}
		if len(entries) > 32 {
			return nil, fmt.Errorf("too many notices")
		}
		for _, entry := range entries {
			data, err := readPackFile(rootedPackFS{root}, noticeDir+"/"+entry.Name(), 1<<20)
			if err != nil {
				return nil, err
			}
			p, err = p.WithNotice(entry.Name(), string(data))
			if err != nil {
				return nil, err
			}
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	// Explicitly handle the defined author metadata; unrelated files stay put.
	if data, err := readPackFile(rootedPackFS{root}, "author-project.json", 2<<20); err == nil {
		p.info, err = decodeProjectInfo(data, pack)
		if err != nil {
			return nil, err
		}
	} else if !errors.Is(err, fs.ErrNotExist) {
		return nil, err
	}
	return p, nil
}
